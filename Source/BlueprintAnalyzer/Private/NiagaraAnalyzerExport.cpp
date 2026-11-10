#include "BlueprintAnalyzerLibrary.h"
#include "NiagaraAnalyzer.h"

#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace NiagaraReportText
{
    using FValue = TSharedPtr<FJsonValue>;

    FString JsonLiteral(const FValue& Value)
    {
        FString Out;
        const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
        return Out;
    }

    void AppendStackSummary(const FJsonObject& Owner, FString& Out)
    {
        const TArray<FValue>* Scripts;
        if (!Owner.TryGetArrayField(TEXT("Scripts"), Scripts)) return;
        for (const FValue& ScriptValue : *Scripts)
        {
            const FJsonObject& Script = *ScriptValue->AsObject();
            const TArray<FValue>* Modules;
            if (!Script.TryGetArrayField(TEXT("Modules"), Modules) || Modules->IsEmpty()) continue;
            TArray<FString> Names;
            for (const FValue& ModuleValue : *Modules)
            {
                const FJsonObject& Module = *ModuleValue->AsObject();
                FString Name = Module.GetStringField(TEXT("Name"));
                if (!Module.GetBoolField(TEXT("Enabled"))) Name += TEXT(" (disabled)");
                Names.Add(MoveTemp(Name));
            }
            Out += TEXT("- ") + Script.GetStringField(TEXT("Usage")) + TEXT("  ") + FString::Join(Names, TEXT(" -> ")) + TEXT("\n");
        }
    }

    void AppendOverview(const FJsonObject& Report, FString& Out)
    {
        Out += TEXT("## Effect overview\n");
        AppendStackSummary(Report, Out);
        for (const FValue& EmitterValue : Report.GetArrayField(TEXT("Emitters")))
        {
            const FJsonObject& Emitter = *EmitterValue->AsObject();
            Out += TEXT("\n") + Emitter.GetStringField(TEXT("Name")) + TEXT(" (") + Emitter.GetStringField(TEXT("Mode"));
            FString SimTarget;
            if (Emitter.TryGetStringField(TEXT("SimTarget"), SimTarget)) Out += TEXT(", ") + SimTarget;
            if (!Emitter.GetBoolField(TEXT("Enabled"))) Out += TEXT(", disabled");
            Out += TEXT(")\n");
            AppendStackSummary(Emitter, Out);
            const TArray<FValue>* Spawns;
            if (Emitter.TryGetArrayField(TEXT("SpawnInfos"), Spawns))
            {
                for (const FValue& SpawnValue : *Spawns)
                {
                    const FJsonObject& Spawn = *SpawnValue->AsObject();
                    FString Type;
                    if (!Spawn.TryGetStringField(TEXT("Type"), Type)) continue;
                    Out += TEXT("- Spawn  ") + Type;
                    const TSharedPtr<FJsonObject>* Distribution;
                    if (Spawn.TryGetObjectField(Type == TEXT("Burst") ? TEXT("Amount") : TEXT("Rate"), Distribution))
                    {
                        FString Mode;
                        (*Distribution)->TryGetStringField(TEXT("Mode"), Mode);
                        double Count;
                        if (Mode == TEXT("UniformConstant") && (*Distribution)->TryGetNumberField(TEXT("Min"), Count))
                            Out += TEXT("  ") + FString::SanitizeFloat(Count, 0);
                        else if (!Mode.IsEmpty())
                            Out += TEXT("  ") + Mode;
                    }
                    bool bEnabled;
                    if (Spawn.TryGetBoolField(TEXT("bEnabled"), bEnabled) && !bEnabled) Out += TEXT(" (disabled)");
                    Out += TEXT("\n");
                }
            }
            const TArray<FValue>* Renderers;
            if (Emitter.TryGetArrayField(TEXT("Renderers"), Renderers))
            {
                for (const FValue& RendererValue : *Renderers)
                {
                    const FJsonObject& Renderer = *RendererValue->AsObject();
                    Out += TEXT("- Renderer  ") + Renderer.GetStringField(TEXT("Class"));
                    if (!Renderer.GetBoolField(TEXT("Enabled"))) Out += TEXT(" (disabled)");
                    Out += TEXT("\n");
                }
            }
        }
    }

    // Keep each parameter, module, node and wire on one line. Array order is significant.
    void AppendFields(const FJsonObject& Object, FString& Out, int32 Level)
    {
        for (const auto& Field : Object.Values)
        {
            const FString& Key = Field.Key;
            const FValue& Value = Field.Value;
            if (Key == TEXT("ScriptDefinitions") && Value->Type == EJson::Object)
            {
                Out += TEXT("\n## Referenced script definitions\n");
                for (const auto& Definition : Value->AsObject()->Values)
                {
                    Out += TEXT("\n### ") + Definition.Key + TEXT("\n");
                    if (Definition.Value->Type == EJson::Object)
                    {
                        AppendFields(*Definition.Value->AsObject(), Out, 3);
                    }
                    else
                    {
                        Out += JsonLiteral(Definition.Value) + TEXT("\n");
                    }
                }
                continue;
            }

            if (Value->Type != EJson::Array)
            {
                Out += Key + TEXT(" = ") + JsonLiteral(Value) + TEXT("\n");
                continue;
            }

            const TArray<FValue>& Items = Value->AsArray();
            Out += FString::Printf(TEXT("%s [%d]\n"), *Key, Items.Num());
            for (int32 Index = 0; Index < Items.Num(); ++Index)
            {
                const FValue& Item = Items[Index];
                if ((Key == TEXT("Emitters") || Key == TEXT("Scripts")) && Item->Type == EJson::Object)
                {
                    const FJsonObject& Record = *Item->AsObject();
                    FString Label;
                    Record.TryGetStringField(Key == TEXT("Emitters") ? TEXT("Name") : TEXT("Usage"), Label);
                    Out += FString::Printf(TEXT("\n%s %s %d  %s\n"), *FString::ChrN(Level + 1, '#'),
                        Key == TEXT("Emitters") ? TEXT("Emitter") : TEXT("Script"), Index + 1, *Label);
                    AppendFields(Record, Out, Level + 1);
                    Out += TEXT("\n");
                }
                else if (Key == TEXT("Modules") && Item->Type == EJson::Object &&
                    Item->AsObject()->HasField(TEXT("Settings")) && Item->AsObject()->HasField(TEXT("Enabled")) &&
                    !Item->AsObject()->GetBoolField(TEXT("Enabled")))
                {
                    Out += TEXT("- Disabled  ") + Item->AsObject()->GetStringField(TEXT("Class")) + TEXT("\n");
                }
                else
                {
                    Out += TEXT("- ") + JsonLiteral(Item) + TEXT("\n");
                }
            }
        }
    }
}

FString UBlueprintAnalyzerLibrary::ExportNiagaraSystemToLLMText(UNiagaraSystem* System)
{
    const TSharedPtr<FJsonObject> Report = FNiagaraAnalyzer::Analyze(System);
    if (!Report) return FString();

    FString Name;
    Report->TryGetStringField(TEXT("Name"), Name);
    FString Out = TEXT("# Niagara System  ") + Name + TEXT("\n\n");
    Out += TEXT("Read each stack in listed order. User parameters and bindings can supply values at runtime. ")
           TEXT("The JSON export also includes settings for disabled lightweight modules.\n\n");
    NiagaraReportText::AppendOverview(*Report, Out);
    Out += TEXT("\n## Authored data\n");
    NiagaraReportText::AppendFields(*Report, Out, 1);
    return Out;
}

FString UBlueprintAnalyzerLibrary::ExportNiagaraSystemToJSON(UNiagaraSystem* System)
{
    const TSharedPtr<FJsonObject> Report = FNiagaraAnalyzer::Analyze(System);
    if (!Report) return FString();

    FString Out;
    const auto Writer = TJsonWriterFactory<>::Create(&Out);
    return FJsonSerializer::Serialize(Report.ToSharedRef(), Writer) ? Out : FString();
}
