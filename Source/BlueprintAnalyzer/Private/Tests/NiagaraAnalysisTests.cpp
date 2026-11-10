#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintAnalyzerLibrary.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
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
        TestTrue(TEXT("Authored standard module stack"), bFoundStack);
    }
    const FString Text = UBlueprintAnalyzerLibrary::ExportNiagaraSystemToLLMText(System);
    TestTrue(TEXT("AI text identifies asset and emitter mode"), Text.Contains(Path) && Text.Contains(Mode));
    return true;
}

#endif
