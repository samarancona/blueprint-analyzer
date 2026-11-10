#include "NiagaraAnalyzerGraph.h"

#include "NiagaraAnalyzerProperties.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace NiagaraAnalyzerGraph
{
using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

FString UsageName(ENiagaraScriptUsage Usage)
{
    return StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Usage));
}

TSharedRef<FJsonObject> VariableInfo(const FNiagaraVariableBase& Var)
{
    auto Info = MakeShared<FJsonObject>();
    Info->SetStringField(TEXT("Name"), Var.GetName().ToString());
    Info->SetStringField(TEXT("Type"), Var.GetType().GetName());
    return Info;
}

void SetLiteral(const TSharedRef<FJsonObject>& Info, const FNiagaraVariable& Var)
{
    if (!Var.GetType().IsValid() || Var.IsUObject() || !Var.IsDataAllocated()) return;
    FString Value;
    if (!GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(Var, Value))
    {
        const UScriptStruct* Struct = Var.GetType().GetScriptStruct();
        if (!Struct) return;
        Struct->ExportText(Value, Var.GetData(), nullptr, nullptr, PPF_None, nullptr);
    }
    Info->SetStringField(TEXT("Value"), Value);
}

FJsonArray RapidIterationValues(const FNiagaraParameterStore& Store)
{
    FJsonArray Values;
    for (const FNiagaraVariableWithOffset& Entry : Store.ReadParameterVariables())
    {
        FNiagaraVariable Var(Entry.GetType(), Entry.GetName());
        auto Info = VariableInfo(Var);
        if (Var.IsValid())
        {
            if (Var.IsDataInterface())
            {
                if (const auto* DI = Store.GetDataInterface(Entry.Offset))
                    Info->SetObjectField(TEXT("DataInterface"), DescribeNiagaraObject(DI));
            }
            else if (Var.IsUObject())
            {
                Info->SetStringField(TEXT("Object"), GetPathNameSafe(Store.GetUObject(Entry.Offset).Get()));
            }
            else
            {
                Var.AllocateData();
                if (Var.IsDataAllocated() && Store.CopyParameterData(Var, Var.GetData())) SetLiteral(Info, Var);
            }
        }
        Values.Add(MakeShared<FJsonValueObject>(Info));
    }
    return Values;
}

FJsonArray DeclaredInputs(const UNiagaraGraph& Graph)
{
    TArray<UNiagaraScriptVariable*> Inputs;
    for (const auto& Pair : Graph.GetAllMetaData())
    {
        UNiagaraScriptVariable* Var = Pair.Value;
        if (Var && (Var->GetIsStaticSwitch() || Var->Variable.GetName().ToString().StartsWith(TEXT("Module."))))
            Inputs.Add(Var);
    }
    Inputs.Sort([](const UNiagaraScriptVariable& A, const UNiagaraScriptVariable& B)
    {
        return A.Variable.GetName().LexicalLess(B.Variable.GetName());
    });
    FJsonArray Result;
    for (const UNiagaraScriptVariable* Input : Inputs)
    {
        FNiagaraVariable Var = Input->Variable;
        auto Info = VariableInfo(Var);
        if (Input->GetIsStaticSwitch())
        {
            Info->SetNumberField(TEXT("StaticSwitchDefault"), Input->GetStaticSwitchDefaultValue());
        }
        else
        {
            Info->SetStringField(TEXT("DefaultMode"), StaticEnum<ENiagaraDefaultMode>()->GetNameStringByValue(static_cast<int64>(Input->DefaultMode)));
            if (Input->DefaultMode == ENiagaraDefaultMode::Binding)
                Info->SetStringField(TEXT("Binding"), Input->DefaultBinding.Name.ToString());
            if (Input->DefaultMode == ENiagaraDefaultMode::Value && Var.IsValid() && Input->GetDefaultValueData() &&
                Var.GetSizeInBytes() == Input->GetDefaultValueVariant().GetNumBytes())
            {
                Var.SetData(Input->GetDefaultValueData());
                SetLiteral(Info, Var);
            }
        }
        Result.Add(MakeShared<FJsonValueObject>(Info));
    }
    return Result;
}

bool IsSourcePin(const UEdGraphPin* Pin)
{
    return Pin && !Pin->bOrphanedPin && !(Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc &&
        Pin->PinName == TEXT("Add"));
}

// A map-get's unnamed input pins contain defaults for different output variables.
// Their persistent mapping is serialized by a private editor class.
FString PinName(const UEdGraphPin& Pin)
{
    if (!Pin.PinName.IsNone()) return Pin.PinName.ToString();
    const UEdGraphNode* Owner = Pin.GetOwningNodeUnchecked();
    if (Owner && Pin.PersistentGuid.IsValid())
    {
        const FMapProperty* Defaults = FindFProperty<FMapProperty>(Owner->GetClass(), TEXT("PinOutputToPinDefaultPersistentId"));
        const auto* KeyType = Defaults ? CastField<FStructProperty>(Defaults->KeyProp) : nullptr;
        const auto* ValueType = Defaults ? CastField<FStructProperty>(Defaults->ValueProp) : nullptr;
        if (KeyType && ValueType && KeyType->Struct == TBaseStructure<FGuid>::Get() && ValueType->Struct == KeyType->Struct)
        {
            const auto* Map = Defaults->ContainerPtrToValuePtr<TMap<FGuid, FGuid>>(Owner);
            if (const FGuid* OutputGuid = Map->FindKey(Pin.PersistentGuid))
            {
                for (const UEdGraphPin* Output : Owner->Pins)
                    if (Output && Output->Direction == EGPD_Output && Output->PersistentGuid == *OutputGuid)
                        return TEXT("Default:") + Output->PinName.ToString();
            }
        }
    }
    return Pin.PinId.ToString();
}

UEdGraphPin* MapInput(const UEdGraphNode& Node)
{
    for (UEdGraphPin* Pin : Node.Pins)
        if (IsSourcePin(Pin) && Pin->Direction == EGPD_Input &&
            UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef()) return Pin;
    return nullptr;
}

const UEdGraphNode* PreviousStackNode(const UEdGraphNode& Node)
{
    const UEdGraphPin* Pin = MapInput(Node);
    if (!Pin || Pin->LinkedTo.Num() != 1 || !IsSourcePin(Pin->LinkedTo[0])) return nullptr;
    const UEdGraphNode* Upstream = Pin->LinkedTo[0]->GetOwningNodeUnchecked();
    return Upstream && Upstream->GetGraph() == Node.GetGraph() ? Upstream : nullptr;
}

TSharedRef<FJsonObject> DescribeInputPin(const UEdGraphPin& Pin)
{
    auto Info = MakeShared<FJsonObject>();
    Info->SetStringField(TEXT("Name"), PinName(Pin));
    Info->SetStringField(TEXT("Type"), UEdGraphSchema_Niagara::PinToTypeDefinition(&Pin).GetName());
    if (UEdGraphSchema_Niagara::IsStaticPin(&Pin)) Info->SetBoolField(TEXT("Static"), true);
    if (Pin.LinkedTo.IsEmpty() && !Pin.bDefaultValueIsIgnored)
    {
        const FString Value = Pin.GetDefaultAsString();
        if (!Value.IsEmpty()) Info->SetStringField(TEXT("Value"), Value);
    }
    return Info;
}

void AddOverrides(FJsonObject& Info, const UNiagaraNodeFunctionCall& Call)
{
    const UEdGraphNode* Set = PreviousStackNode(Call);
    if (!Set || Set->GetClass()->GetFName() != TEXT("NiagaraNodeParameterMapSet")) return;
    FJsonArray Overrides;
    const FString Prefix = Call.GetFunctionName() + TEXT(".");
    for (const UEdGraphPin* Pin : Set->Pins)
    {
        if (!IsSourcePin(Pin) || Pin->Direction != EGPD_Input || !Pin->PinName.ToString().StartsWith(Prefix)) continue;
        auto Override = DescribeInputPin(*Pin);
        for (const UEdGraphPin* Source : Pin->LinkedTo)
        {
            const UEdGraphNode* Node = Source ? Source->GetOwningNodeUnchecked() : nullptr;
            if (Node)
            {
                Override->SetStringField(TEXT("FromNode"), Node->GetName());
                Override->SetStringField(TEXT("FromPin"), PinName(*Source));
                break;
            }
        }
        Overrides.Add(MakeShared<FJsonValueObject>(Override));
    }
    if (!Overrides.IsEmpty()) Info.SetArrayField(TEXT("Overrides"), MoveTemp(Overrides));
}

void ReadGraph(FJsonObject& Info, const UNiagaraGraph& Graph, const UNiagaraNodeOutput& Output,
    const UPackage* OwningPackage, const TSharedRef<FJsonObject>& Definitions, bool bReadStack);

FString AddDefinition(UNiagaraScript& Script, FGuid SelectedVersion, const UPackage* OwningPackage,
    const TSharedRef<FJsonObject>& Definitions)
{
    const FVersionedNiagaraScriptData* Data = Script.GetScriptData(SelectedVersion);
    const FGuid Version = Data ? Data->Version.VersionGuid : SelectedVersion;
    const FString Key = Script.GetPathName() + TEXT("@") + Version.ToString();
    if (Definitions->HasField(Key)) return Key;
    auto Info = MakeShared<FJsonObject>();
    Definitions->SetObjectField(Key, Info); // Insert before following further calls, including recursive references.
    Info->SetStringField(TEXT("Path"), Script.GetPathName());
    Info->SetStringField(TEXT("Usage"), UsageName(Script.GetUsage()));
    if (!Data)
    {
        Info->SetStringField(TEXT("Coverage"), TEXT("VersionUnavailable"));
        return Key;
    }
    Info->SetStringField(TEXT("Version"), FString::Printf(TEXT("%d.%d"), Data->Version.MajorVersion, Data->Version.MinorVersion));
    if (!Data->Description.IsEmpty()) Info->SetStringField(TEXT("Description"), Data->Description.ToString());
    const auto* Source = Cast<UNiagaraScriptSource>(Script.GetSource(Version));
    const UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
    if (!Graph)
    {
        Info->SetStringField(TEXT("Coverage"), TEXT("SourceUnavailable"));
        return Key;
    }
    Info->SetArrayField(TEXT("DeclaredInputs"), DeclaredInputs(*Graph));
    const FString PackageName = Script.GetOutermost()->GetName();
    const bool bExternalEngineAsset = Script.GetOutermost() != OwningPackage &&
        (PackageName.StartsWith(TEXT("/Niagara/")) || PackageName.StartsWith(TEXT("/Engine/")));
    if (bExternalEngineAsset)
    {
        Info->SetStringField(TEXT("Coverage"), TEXT("EngineScriptInterface"));
        return Key;
    }
    const UNiagaraNodeOutput* Output = Graph->FindEquivalentOutputNode(Script.GetUsage(), Script.GetUsageId());
    Info->SetStringField(TEXT("Coverage"), Output ? TEXT("SourceGraph") : TEXT("OutputUnavailable"));
    if (Output) ReadGraph(*Info, *Graph, *Output, OwningPackage, Definitions, false);
    return Key;
}

void AddNodeDetails(FJsonObject& Info, const UNiagaraNode& Node, const UPackage* OwningPackage,
    const TSharedRef<FJsonObject>& Definitions)
{
    if (const auto* Op = Cast<UNiagaraNodeOp>(&Node))
    {
        Info.SetStringField(TEXT("Operation"), Op->OpName.ToString());
    }
    // A Convert node can connect individual vector or struct members internally.
    // Its graph wires alone cannot distinguish, for example, X -> Y from Z -> Y.
    if (Node.GetClass()->GetFName() == TEXT("NiagaraNodeConvert"))
    {
        const auto* Prop = FindFProperty<FArrayProperty>(Node.GetClass(), TEXT("Connections"));
        if (Prop)
        {
            FJsonArray Mappings;
            FScriptArrayHelper Array(Prop, Prop->ContainerPtrToValuePtr<void>(&Node));
            for (int32 Index = 0; Index < Array.Num(); ++Index)
            {
                FString Mapping;
                Prop->Inner->ExportTextItem_Direct(Mapping, Array.GetRawPtr(Index), nullptr, nullptr, PPF_None);
                Mappings.Add(MakeShared<FJsonValueString>(Mapping));
            }
            auto PinNames = MakeShared<FJsonObject>();
            for (const UEdGraphPin* Pin : Node.Pins)
                if (IsSourcePin(Pin)) PinNames->SetStringField(Pin->PinId.ToString(), PinName(*Pin));
            Info.SetObjectField(TEXT("ConvertPinNames"), PinNames);
            Info.SetArrayField(TEXT("MemberMappings"), MoveTemp(Mappings));
        }
    }
    if (const auto* Input = Cast<UNiagaraNodeInput>(&Node))
    {
        auto Variable = VariableInfo(Input->Input);
        SetLiteral(Variable, Input->Input);
        // Both UObject accessors are private to the NiagaraEditor DLL in 5.7.
        for (const FName PropertyName : {FName(TEXT("DataInterface")), FName(TEXT("ObjectAsset"))})
        {
            if (const auto* Prop = FindFProperty<FObjectPropertyBase>(Input->GetClass(), PropertyName))
            {
                if (UObject* Obj = Prop->GetObjectPropertyValue_InContainer(Input))
                {
                    if (PropertyName == TEXT("DataInterface")) Variable->SetObjectField(TEXT("DataInterface"), DescribeNiagaraObject(Obj));
                    else Variable->SetStringField(TEXT("Object"), Obj->GetPathName());
                }
            }
        }
        Info.SetObjectField(TEXT("Variable"), Variable);
    }
    const auto* Call = Cast<UNiagaraNodeFunctionCall>(&Node);
    if (!Call) return;
    Info.SetStringField(TEXT("Function"), Call->GetFunctionName());
    if (!Call->FunctionSpecifiers.IsEmpty())
    {
        auto Specifiers = MakeShared<FJsonObject>();
        for (const auto& Pair : Call->FunctionSpecifiers)
            Specifiers->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
        Info.SetObjectField(TEXT("Specifiers"), Specifiers);
    }
    AddOverrides(Info, *Call);
    if (const auto* Hlsl = Cast<UNiagaraNodeCustomHlsl>(Call))
    {
        // GetCustomHlsl() is not exported by the installed NiagaraEditor module.
        if (const auto* Prop = FindFProperty<FStrProperty>(Hlsl->GetClass(), TEXT("CustomHlsl")))
            Info.SetStringField(TEXT("HLSL"), Prop->GetPropertyValue_InContainer(Hlsl));
        for (const FName Name : {FName(TEXT("AbsoluteIncludeFilePaths")), FName(TEXT("VirtualIncludeFilePaths"))})
        {
            if (const auto* Prop = FindFProperty<FArrayProperty>(Hlsl->GetClass(), Name))
            {
                FScriptArrayHelper Array(Prop, Prop->ContainerPtrToValuePtr<void>(Hlsl));
                FJsonArray Paths;
                for (int32 Index = 0; Index < Array.Num(); ++Index)
                {
                    FString Path;
                    Prop->Inner->ExportTextItem_Direct(Path, Array.GetRawPtr(Index), nullptr, nullptr, PPF_None);
                    Paths.Add(MakeShared<FJsonValueString>(Path));
                }
                if (!Paths.IsEmpty()) Info.SetArrayField(Name.ToString(), MoveTemp(Paths));
            }
        }
    }
    else if (const auto* Assignment = Cast<UNiagaraNodeAssignment>(Call))
    {
        FJsonArray Targets;
        for (int32 Index = 0; Index < Assignment->NumTargets(); ++Index)
        {
            auto Target = VariableInfo(Assignment->GetAssignmentTarget(Index));
            if (Assignment->GetAssignmentDefaults().IsValidIndex(Index))
                Target->SetStringField(TEXT("Default"), Assignment->GetAssignmentDefaults()[Index]);
            Targets.Add(MakeShared<FJsonValueObject>(Target));
        }
        Info.SetArrayField(TEXT("Assignments"), MoveTemp(Targets));
    }
    else if (Call->FunctionScript)
    {
        Info.SetStringField(TEXT("Definition"), AddDefinition(*Call->FunctionScript, Call->SelectedScriptVersion, OwningPackage, Definitions));
    }
    else if (!Call->Signature.Name.IsNone())
    {
        Info.SetStringField(TEXT("NativeFunction"), Call->Signature.Name.ToString());
        Info.SetStringField(TEXT("Owner"), Call->Signature.OwnerName.ToString());
    }
}

void ReadGraph(FJsonObject& Info, const UNiagaraGraph& Graph, const UNiagaraNodeOutput& Output,
    const UPackage* OwningPackage, const TSharedRef<FJsonObject>& Definitions, bool bReadStack)
{
    Info.SetStringField(TEXT("OutputNode"), Output.GetName());
    if (bReadStack)
    {
        TArray<const UNiagaraNodeFunctionCall*> Calls;
        TSet<const UEdGraphNode*> Seen;
        const UEdGraphNode* Current = &Output;
        while (Current && !Seen.Contains(Current))
        {
            Seen.Add(Current);
            if (const auto* Call = Cast<UNiagaraNodeFunctionCall>(Current)) Calls.Add(Call);
            Current = PreviousStackNode(*Current);
        }
        FJsonArray Modules;
        for (int32 Index = Calls.Num() - 1; Index >= 0; --Index)
        {
            const UNiagaraNodeFunctionCall* Call = Calls[Index];
            auto Module = MakeShared<FJsonObject>();
            Module->SetStringField(TEXT("Node"), Call->GetName());
            Module->SetStringField(TEXT("Name"), Call->GetFunctionName());
            Module->SetBoolField(TEXT("Enabled"), Call->IsNodeEnabled());
            if (Call->FunctionScript) Module->SetStringField(TEXT("Script"), Call->FunctionScript->GetPathName());
            if (Call->SelectedScriptVersion.IsValid()) Module->SetStringField(TEXT("SelectedVersion"), Call->SelectedScriptVersion.ToString());
            Modules.Add(MakeShared<FJsonValueObject>(Module));
        }
        Info.SetArrayField(TEXT("Modules"), MoveTemp(Modules));
    }

    // Each output is a separate stage in a shared graph. Trace every authored
    // branch upstream, including disabled modules and unresolved switches.
    TArray<const UNiagaraNode*> Pending = {&Output};
    TSet<const UNiagaraNode*> Seen;
    FJsonArray Nodes, Edges;
    while (!Pending.IsEmpty())
    {
        const UNiagaraNode* Node = Pending.Pop(EAllowShrinking::No);
        if (Seen.Contains(Node)) continue;
        Seen.Add(Node);
        auto Record = MakeShared<FJsonObject>();
        Record->SetStringField(TEXT("Id"), Node->GetName());
        Record->SetStringField(TEXT("Class"), Node->GetClass()->GetName());
        Record->SetStringField(TEXT("Title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        if (!Node->IsNodeEnabled()) Record->SetBoolField(TEXT("Enabled"), false);
        AddNodeDetails(*Record, *Node, OwningPackage, Definitions);
        FJsonArray Inputs;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsSourcePin(Pin) || Pin->Direction != EGPD_Input) continue;
            if (Pin->LinkedTo.IsEmpty())
            {
                if (!Pin->bDefaultValueIsIgnored && !Pin->GetDefaultAsString().IsEmpty())
                    Inputs.Add(MakeShared<FJsonValueObject>(DescribeInputPin(*Pin)));
                continue;
            }
            for (const UEdGraphPin* From : Pin->LinkedTo)
            {
                const UNiagaraNode* Source = IsSourcePin(From) ? Cast<UNiagaraNode>(From->GetOwningNodeUnchecked()) : nullptr;
                if (!Source || Source->GetGraph() != &Graph) continue;
                auto Edge = MakeShared<FJsonObject>();
                Edge->SetStringField(TEXT("FromNode"), Source->GetName());
                Edge->SetStringField(TEXT("FromPin"), PinName(*From));
                Edge->SetStringField(TEXT("ToNode"), Node->GetName());
                Edge->SetStringField(TEXT("ToPin"), PinName(*Pin));
                Edge->SetStringField(TEXT("Type"), UEdGraphSchema_Niagara::PinToTypeDefinition(Pin).GetName());
                Edges.Add(MakeShared<FJsonValueObject>(Edge));
                Pending.Add(Source);
            }
        }
        if (!Inputs.IsEmpty()) Record->SetArrayField(TEXT("LiteralInputs"), MoveTemp(Inputs));
        Nodes.Add(MakeShared<FJsonValueObject>(Record));
    }
    Info.SetArrayField(TEXT("Nodes"), MoveTemp(Nodes));
    Info.SetArrayField(TEXT("Connections"), MoveTemp(Edges));
}
}

void AddNiagaraScriptAnalysis(TSharedRef<FJsonObject> Json, UNiagaraScript* Script,
    const FString& EmitterName, TSharedRef<FJsonObject> Definitions)
{
    using namespace NiagaraAnalyzerGraph;
    if (!Script)
    {
        Json->SetBoolField(TEXT("SourceAvailable"), false);
        return;
    }
    if (!EmitterName.IsEmpty()) Json->SetStringField(TEXT("EmitterNamespace"), EmitterName);
    Json->SetArrayField(TEXT("RapidIterationParameters"), RapidIterationValues(Script->RapidIterationParameters));
    const auto* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
    const UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
    const UNiagaraNodeOutput* Output = Graph ? Graph->FindEquivalentOutputNode(Script->GetUsage(), Script->GetUsageId()) : nullptr;
    Json->SetBoolField(TEXT("SourceAvailable"), Output != nullptr);
    if (Output) ReadGraph(*Json, *Graph, *Output, Script->GetOutermost(), Definitions, true);
}
