#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintAnalyzerLibrary.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraScriptRuntimeData.h"
#include "NiagaraSystem.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FNiagaraEmitterReportTest, "BlueprintAnalyzer.Analysis.Niagara",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FNiagaraEmitterReportTest::GetTests(TArray<FString>& Names, TArray<FString>& Assets) const
{
    Names.Add(TEXT("Standard"));
    Assets.Add(TEXT("DirectionalBurst"));
    Names.Add(TEXT("Lightweight"));
    Assets.Add(TEXT("DirectionalBurstLightweight"));
}

bool FNiagaraEmitterReportTest::RunTest(const FString& AssetName)
{
    const FString Path = TEXT("/Niagara/DefaultAssets/Templates/Systems/") + AssetName + TEXT(".") + AssetName;
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
    if (!TestNotNull(TEXT("Niagara template"), System)) return false;
    System->RequestCompile(false);
    System->WaitForCompilationComplete(false, false);

    const FString Json = UBlueprintAnalyzerLibrary::ExportNiagaraSystemToJSON(System);
    TSharedPtr<FJsonObject> Report;
    if (!TestTrue(TEXT("Valid report JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report) && Report.IsValid()))
        return false;

    const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
    if (!TestTrue(TEXT("Emitter handles"), Report->TryGetArrayField(TEXT("Emitters"), Emitters) && !Emitters->IsEmpty()))
        return false;

    const TSharedPtr<FJsonObject> Emitter = (*Emitters)[0]->AsObject();
    const bool bLightweight = AssetName.EndsWith(TEXT("Lightweight"));
    const FString Mode = bLightweight ? TEXT("Stateless") : TEXT("Standard");
    TestEqual(TEXT("Mode of the selected emitter"), Emitter->GetStringField(TEXT("Mode")), Mode);
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    TestTrue(TEXT("Renderers included"), Emitter->TryGetArrayField(TEXT("Renderers"), Items) && !Items->IsEmpty());
    if (bLightweight)
    {
        TestTrue(TEXT("Lightweight spawn configuration"), Emitter->TryGetArrayField(TEXT("SpawnInfos"), Items) && !Items->IsEmpty());
        TestTrue(TEXT("Lightweight module settings"), Emitter->TryGetArrayField(TEXT("Modules"), Items) && !Items->IsEmpty());
    }
    else
    {
        const bool bFoundStack = Emitter->TryGetArrayField(TEXT("Scripts"), Items) && Items->ContainsByPredicate(
            [](const TSharedPtr<FJsonValue>& Stage)
            {
                const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
                return Stage->AsObject()->TryGetArrayField(TEXT("Modules"), Modules) && !Modules->IsEmpty();
            });
        if (!TestTrue(TEXT("Authored standard module stack"), bFoundStack)) return false;

        // UE 5.8 resolves compiled bindings using the owning system and emitter handle.
        const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[0];
        TArray<UNiagaraScript*> Scripts;
        Handle.GetEmitterData()->GetScripts(Scripts, false, false);
        int32 CheckedScripts = 0;
        for (UNiagaraScript* Script : Scripts)
        {
            if (!Script || Script->GetUsage() == ENiagaraScriptUsage::ParticleGPUComputeScript) continue;
            const auto RuntimeData = System->GetScriptRuntimeData(&Handle, *Script);

            if (!RuntimeData.IsValid()) continue;
            const auto ExpectedBindings = RuntimeData->GetResolvedDataInterfaces();

            const auto* ScriptReport = Items->FindByPredicate([Script](const TSharedPtr<FJsonValue>& Value)
            {
                return Value->AsObject()->GetStringField(TEXT("Path")) == Script->GetPathName();
            });
            if (!TestNotNull(TEXT("Compiled script included in report"), ScriptReport)) continue;
            const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
            const bool bHasBindings = (*ScriptReport)->AsObject()->TryGetArrayField(TEXT("LastCompiledDataInterfaceBindings"), Bindings);
            // This template can optimize all interfaces away; empty bindings must be omitted.
            TestEqual(TEXT("Compiled bindings included only when present"), bHasBindings, !ExpectedBindings.IsEmpty());
            if (!TestEqual(TEXT("Compiled binding count"), Bindings ? Bindings->Num() : 0, ExpectedBindings.Num())) continue;
            ++CheckedScripts;
            for (int32 Index = 0; Index < ExpectedBindings.Num(); ++Index)
            {
                const auto& Expected = ExpectedBindings[Index];
                const auto Binding = (*Bindings)[Index]->AsObject();
                TestEqual(TEXT("Compiled binding name"), Binding->GetStringField(TEXT("Name")), Expected.Name.ToString());
                TestEqual(TEXT("Compiled binding variable"), Binding->GetStringField(TEXT("Variable")), Expected.ResolvedVariable.GetName().ToString());
                TestEqual(TEXT("Compiled binding source emitter"), Binding->GetStringField(TEXT("SourceEmitter")), Expected.ResolvedSourceEmitterName);

            }
        }
        TestTrue(TEXT("Standard fixture exercises compiled scripts"), CheckedScripts > 0);
    }
    const FString Text = UBlueprintAnalyzerLibrary::ExportNiagaraSystemToLLMText(System);
    TestTrue(TEXT("AI text identifies asset and emitter mode"), Text.Contains(Path) && Text.Contains(Mode));
    for (const auto& Definition : Report->GetObjectField(TEXT("ScriptDefinitions"))->Values)
    {
        TestTrue(TEXT("AI text includes script definition headings"),
            Text.Contains(FString::Printf(TEXT("\n### %s\n"), *Definition.Key)));
    }
    return true;
}

#endif
