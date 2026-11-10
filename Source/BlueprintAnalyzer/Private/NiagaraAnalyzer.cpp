#include "NiagaraAnalyzer.h"

#include "NiagaraAnalyzerGraph.h"
#include "NiagaraAnalyzerProperties.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "UObject/UnrealType.h"

namespace
{
using FJsonItems = TArray<TSharedPtr<FJsonValue>>;

// The public handle supplies the effective emitter. The lightweight implementation and module
// classes live in Niagara's Internal directory, but their authored properties are reflected.
template <typename Visitor>
void VisitObjectArray(const UObject* Owner, FName FieldName, Visitor Visit)
{
    const FArrayProperty* Array = FindFProperty<FArrayProperty>(Owner->GetClass(), FieldName);
    const FObjectPropertyBase* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
    if (!Inner) return;
    FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Owner));
    for (int32 Index = 0; Index < Values.Num(); ++Index)
    {
        if (UObject* Object = Inner->GetObjectPropertyValue(Values.GetRawPtr(Index)); IsValid(Object))
        {
            Visit(Object, Index);
        }
    }
}

FJsonItems ReadUserParameters(const FNiagaraUserRedirectionParameterStore& Store)
{
    FJsonItems Result;
    TArray<FNiagaraVariable> Parameters;
    Store.GetParameters(Parameters); // Canonical, fully qualified names in store order.
    for (FNiagaraVariable& Parameter : Parameters)
    {
        const FNiagaraTypeDefinition& Type = Parameter.GetType();
        if (!Parameter.IsValid()) continue;
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("Name"), Parameter.GetName().ToString());
        Json->SetStringField(TEXT("Type"), Type.GetName());
        TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueNull>();
        if (Type.IsDataInterface())
        {
            if (UNiagaraDataInterface* DI = Store.GetDataInterface(Parameter))
            {
                Value = MakeShared<FJsonValueObject>(DescribeNiagaraObject(DI));
            }
        }
        else if (Type.IsUObject())
        {
            if (const UObject* Object = Store.GetUObject(Parameter))
            {
                Value = MakeShared<FJsonValueString>(Object->GetPathName());
            }
        }
        else if (const FVector* Position = Type == FNiagaraTypeDefinition::GetPositionDef()
            ? Store.GetPositionParameterValue(Parameter.GetName()) : nullptr)
        {
            Value = MakeShared<FJsonValueString>(Position->ToString());
        }
        else
        {
            Parameter.AllocateData();
            if (Parameter.GetData() && Store.CopyParameterData(Parameter, Parameter.GetData()))
            {
                if (Type.IsEnum() && Parameter.GetSizeInBytes() == sizeof(int32))
                {
                    int32 Index;
                    FMemory::Memcpy(&Index, Parameter.GetData(), sizeof(Index));
                    Value = MakeShared<FJsonValueString>(Type.GetEnum()->GetNameStringByValue(Index));
                }
                else if (UScriptStruct* Struct = Type.GetScriptStruct())
                {
                    Value = DescribeNiagaraStruct(Struct, Parameter.GetData());
                }
            }
        }
        Json->SetField(TEXT("Default"), Value);
        Result.Add(MakeShared<FJsonValueObject>(Json));
    }
    return Result;
}

void AppendScript(FJsonItems& Scripts, UNiagaraScript* Script, const FString& EmitterName,
    TSharedRef<FJsonObject> Definitions)
{
    if (!IsValid(Script)) return;
    auto Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("Usage"), StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(int64(Script->GetUsage())));
    Json->SetStringField(TEXT("UsageId"), Script->GetUsageId().ToString());
    Json->SetStringField(TEXT("Path"), Script->GetPathName());
    AddNiagaraScriptAnalysis(Json, Script, EmitterName, Definitions);
    FJsonItems Interfaces;
    for (const FNiagaraScriptResolvedDataInterfaceInfo& Info : Script->GetResolvedDataInterfaces())
    {
        auto Binding = MakeShared<FJsonObject>();
        Binding->SetStringField(TEXT("Name"), Info.Name.ToString());
        Binding->SetStringField(TEXT("SourceEmitter"), Info.ResolvedSourceEmitterName);
        Binding->SetStringField(TEXT("Variable"), Info.ResolvedVariable.GetName().ToString());
        if (IsValid(Info.ResolvedDataInterface))
        {
            Binding->SetStringField(TEXT("Class"), Info.ResolvedDataInterface->GetClass()->GetPathName());
            Binding->SetStringField(TEXT("Path"), Info.ResolvedDataInterface->GetPathName());
        }
        Interfaces.Add(MakeShared<FJsonValueObject>(Binding));
    }
    if (!Interfaces.IsEmpty()) Json->SetArrayField(TEXT("LastCompiledDataInterfaceBindings"), Interfaces);
    Scripts.Add(MakeShared<FJsonValueObject>(Json));
}

void DescribeStandardEmitter(TSharedRef<FJsonObject> Json, FVersionedNiagaraEmitterData& Data,
    const FString& EmitterName, TSharedRef<FJsonObject> Definitions)
{
    Json->SetStringField(TEXT("VersionGuid"), Data.Version.VersionGuid.ToString());
    Json->SetStringField(TEXT("Version"), FString::Printf(TEXT("%d.%d"), Data.Version.MajorVersion, Data.Version.MinorVersion));
    if (const FVersionedNiagaraEmitter Parent = Data.GetParent(); IsValid(Parent.Emitter))
    {
        Json->SetStringField(TEXT("ParentEmitter"), Parent.Emitter->GetPathName());
        Json->SetStringField(TEXT("ParentVersionGuid"), Parent.Version.ToString());
    }
    Json->SetStringField(TEXT("SimTarget"), StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(int64(Data.SimTarget)));
    Json->SetObjectField(TEXT("Settings"), DescribeNiagaraSettings(FVersionedNiagaraEmitterData::StaticStruct(), &Data));

    FJsonItems Events;
    for (const FNiagaraEventScriptProperties& Event : Data.GetEventHandlers())
    {
        auto Entry = DescribeNiagaraSettings(FNiagaraEventScriptProperties::StaticStruct(), &Event);
        if (Event.Script) Entry->SetStringField(TEXT("Script"), Event.Script->GetPathName());
        Entry->SetStringField(TEXT("SourceEmitterID"), Event.SourceEmitterID.IsValid()
            ? Event.SourceEmitterID.ToString() : TEXT("Self"));
        Events.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Json->SetArrayField(TEXT("EventHandlers"), Events);

    FJsonItems Stages;
    for (const UNiagaraSimulationStageBase* Stage : Data.GetSimulationStages())
    {
        if (!IsValid(Stage)) continue;
        auto Entry = DescribeNiagaraObject(Stage);
        Entry->SetStringField(TEXT("StageName"), Stage->SimulationStageName.ToString());
        Entry->SetBoolField(TEXT("Enabled"), Stage->bEnabled);
        if (Stage->Script) Entry->SetStringField(TEXT("Script"), Stage->Script->GetPathName());
        if (const auto* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
        {
            const FNiagaraParameterBindingWithValue& Iterations = Generic->NumIterations;
            if (Iterations.GetDefaultValueArray().Num() == sizeof(int32))
            {
                Entry->SetNumberField(TEXT("DefaultIterations"), Iterations.GetDefaultValue<int32>());
            }
            if (!Iterations.ResolvedParameter.GetName().IsNone())
            {
                Entry->SetStringField(TEXT("IterationsBinding"), Iterations.ResolvedParameter.GetName().ToString());
            }
        }
        Stages.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Json->SetArrayField(TEXT("SimulationStages"), Stages);

    FJsonItems Scripts;
    TArray<UNiagaraScript*> AllScripts;
    Data.GetScripts(AllScripts, false, false);
    for (UNiagaraScript* Script : AllScripts)
    {
        // A GPU compute script combines existing source stages. It has no authored module stack.
        if (Script && Script->GetUsage() != ENiagaraScriptUsage::ParticleGPUComputeScript)
        {
            AppendScript(Scripts, Script, EmitterName, Definitions);
        }
    }
    Json->SetArrayField(TEXT("Scripts"), Scripts);
}

void DescribeLightweightEmitter(TSharedRef<FJsonObject> Json, const UObject* Emitter)
{
    auto Settings = DescribeNiagaraSettings(Emitter->GetClass(), Emitter);
    const TArray<TSharedPtr<FJsonValue>>* SpawnInfos = nullptr;
    if (Settings->TryGetArrayField(TEXT("SpawnInfos"), SpawnInfos))
    {
        // Burst and rate fields share a serialized struct. Only one drives particle creation.
        for (const TSharedPtr<FJsonValue>& Spawn : *SpawnInfos)
        {
            const TSharedPtr<FJsonObject>& Info = Spawn->AsObject();
            const FString Type = Info->GetStringField(TEXT("Type"));
            if (Type == TEXT("Burst")) Info->RemoveField(TEXT("Rate"));
            if (Type == TEXT("Rate"))
            {
                Info->RemoveField(TEXT("Amount"));
                Info->RemoveField(TEXT("SpawnTime"));
            }
            if (!Info->GetBoolField(TEXT("bSpawnProbabilityEnabled"))) Info->RemoveField(TEXT("SpawnProbability"));
            if (!Info->GetBoolField(TEXT("bLoopCountLimitEnabled"))) Info->RemoveField(TEXT("LoopCountLimit"));
        }
        Json->SetArrayField(TEXT("SpawnInfos"), *SpawnInfos);
        Settings->RemoveField(TEXT("SpawnInfos"));
    }
    Settings->RemoveField(TEXT("Modules"));
    Settings->RemoveField(TEXT("RendererProperties"));
    Json->SetObjectField(TEXT("Settings"), Settings);

    FJsonItems Modules;
    VisitObjectArray(Emitter, TEXT("Modules"), [&](UObject* Module, int32 Index)
    {
        auto Entry = DescribeNiagaraObject(Module);
        Entry->SetNumberField(TEXT("Index"), Index);
        if (const auto* Enabled = FindFProperty<FBoolProperty>(Module->GetClass(), TEXT("bModuleEnabled")))
        {
            Entry->SetBoolField(TEXT("Enabled"), Enabled->GetPropertyValue_InContainer(Module) && !Module->HasAnyFlags(RF_ClassDefaultObject));
        }
        Modules.Add(MakeShared<FJsonValueObject>(Entry));
    });
    Json->SetArrayField(TEXT("Modules"), Modules);
}

TSharedRef<FJsonObject> DescribeEmitter(const FNiagaraEmitterHandle& Handle, TSharedRef<FJsonObject> Definitions)
{
    auto Json = MakeShared<FJsonObject>();
    const FString Name = Handle.GetName().ToString();
    Json->SetStringField(TEXT("Name"), Name);
    Json->SetStringField(TEXT("Id"), Handle.GetId().ToString());
    Json->SetBoolField(TEXT("Enabled"), Handle.GetIsEnabled());
    const bool bStandard = Handle.GetEmitterMode() == ENiagaraEmitterMode::Standard;
    Json->SetStringField(TEXT("Mode"), bStandard ? TEXT("Standard") : TEXT("Stateless"));
    UNiagaraEmitterBase* Emitter = Handle.GetEmitterBase();
    if (!IsValid(Emitter))
    {
        Json->SetStringField(TEXT("Status"), TEXT("Missing emitter instance"));
        return Json;
    }
    Json->SetStringField(TEXT("Path"), Emitter->GetPathName());
    TSet<int32> EnabledRenderers;
    Handle.ForEachEnabledRendererWithIndex([&](const UNiagaraRendererProperties*, int32 Index)
    {
        EnabledRenderers.Add(Index);
    });
    FJsonItems Renderers;
    auto AddRenderer = [&](const UNiagaraRendererProperties* Renderer, int32 Index)
    {
        if (!IsValid(Renderer)) return;
        auto Entry = DescribeNiagaraObject(Renderer);
        Entry->SetNumberField(TEXT("Index"), Index);
        Entry->SetBoolField(TEXT("Enabled"), Renderer->GetIsEnabled());
        // Platform sets, renderer bindings and per-instance culling still apply at runtime.
        Entry->SetBoolField(TEXT("EnabledForEmitterAndSimTarget"), Handle.GetIsEnabled() && EnabledRenderers.Contains(Index));
        TArray<UMaterialInterface*> Materials;
        Renderer->GetUsedMaterials(nullptr, Materials);
        FJsonItems MaterialPaths;
        for (const UMaterialInterface* Material : Materials)
        {
            if (IsValid(Material)) MaterialPaths.Add(MakeShared<FJsonValueString>(Material->GetPathName()));
        }
        Entry->SetArrayField(TEXT("MaterialsWithoutInstanceOverrides"), MaterialPaths);
        Renderers.Add(MakeShared<FJsonValueObject>(Entry));
    };
    if (bStandard)
    {
        if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
        {
            DescribeStandardEmitter(Json, *Data, Handle.GetUniqueInstanceName(), Definitions);
            for (int32 Index = 0; Index < Data->GetRenderers().Num(); ++Index)
            {
                AddRenderer(Data->GetRenderers()[Index], Index);
            }
        }
        else
        {
            Json->SetStringField(TEXT("Status"), TEXT("Missing emitter version data"));
        }
    }
    else
    {
        DescribeLightweightEmitter(Json, Emitter);
        bool bCanUseGPURenderers = true;
        VisitObjectArray(Emitter, TEXT("RendererProperties"), [&](UObject* Object, int32 Index)
        {
            if (const auto* Renderer = Cast<UNiagaraRendererProperties>(Object))
            {
                AddRenderer(Renderer, Index);
                if (Renderer->GetIsEnabled() && !Renderer->IsSimTargetSupported(ENiagaraSimTarget::GPUComputeSim))
                {
                    bCanUseGPURenderers = false;
                }
            }
        });
        // UE selects the stateless renderer target from the enabled renderer types.
        Json->SetStringField(TEXT("RendererSimTarget"), bCanUseGPURenderers ? TEXT("GPUComputeSim") : TEXT("CPUSim"));
    }
    Json->SetArrayField(TEXT("Renderers"), Renderers);
    return Json;
}
}

TSharedPtr<FJsonObject> FNiagaraAnalyzer::Analyze(UNiagaraSystem* System)
{
    if (!IsValid(System)) return nullptr;
    auto Json = DescribeNiagaraObject(System);
    Json->SetStringField(TEXT("AssetType"), TEXT("NiagaraSystem"));
    Json->SetArrayField(TEXT("UserParameters"), ReadUserParameters(System->GetExposedParameters()));
    auto Definitions = MakeShared<FJsonObject>();
    FJsonItems Scripts;
    AppendScript(Scripts, System->GetSystemSpawnScript(), FString(), Definitions);
    AppendScript(Scripts, System->GetSystemUpdateScript(), FString(), Definitions);
    Json->SetArrayField(TEXT("Scripts"), Scripts);

    FJsonItems Emitters;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Emitters.Add(MakeShared<FJsonValueObject>(DescribeEmitter(Handle, Definitions)));
    }
    Json->SetArrayField(TEXT("Emitters"), Emitters);
    Json->SetObjectField(TEXT("ScriptDefinitions"), Definitions);
    return Json;
}
