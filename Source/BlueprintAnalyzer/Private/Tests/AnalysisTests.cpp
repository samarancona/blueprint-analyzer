// Copyright (c) 2025 keemminxu. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintAnalyzerLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_DynamicCast.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintIntermediateGraphsTest,
    "BlueprintAnalyzer.Analysis.CompilerIntermediates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintIntermediateGraphsTest::RunTest(const FString& Parameters)
{
    const TStrongObjectPtr<UBlueprint> Blueprint(NewObject<UBlueprint>());
    Blueprint->ParentClass = UObject::StaticClass();
    UEdGraph* SourceGraph = NewObject<UEdGraph>(Blueprint.Get(), TEXT("SourceFunction"));
    SourceGraph->Schema = UEdGraphSchema_K2::StaticClass();
    Blueprint->FunctionGraphs.Add(SourceGraph);

    // Compiler copies would turn 11 source casts into 22 and trigger the >20 cast penalty.
    constexpr int32 SourceCastCount = 11;
    for (int32 Index = 0; Index < SourceCastCount; ++Index)
    {
        UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(SourceGraph);
        CastNode->TargetType = UObject::StaticClass();
        CastNode->CreateNewGuid();
        SourceGraph->AddNode(CastNode, false, false);
    }

    const FBPPerformanceReport Baseline = UBlueprintAnalyzerLibrary::AnalyzeBlueprintPerformance(Blueprint.Get());
    TestEqual(TEXT("Source cast count"), Baseline.CastCount, SourceCastCount);
    TestEqual(TEXT("Source performance score"), Baseline.PerformanceScore, 100);

    UEdGraph* IntermediateGraph = DuplicateObject<UEdGraph>(SourceGraph, Blueprint.Get(), TEXT("SourceFunction_MERGED"));
    Blueprint->IntermediateGeneratedGraphs.Add(IntermediateGraph);

    const FBPPerformanceReport WithIntermediates = UBlueprintAnalyzerLibrary::AnalyzeBlueprintPerformance(Blueprint.Get());
    TestEqual(TEXT("Compiler copies do not increase node count"), WithIntermediates.TotalNodes, Baseline.TotalNodes);
    TestEqual(TEXT("Compiler copies do not increase cast count"), WithIntermediates.CastCount, Baseline.CastCount);
    TestEqual(TEXT("Compiler copies do not change the score"), WithIntermediates.PerformanceScore, Baseline.PerformanceScore);
    TestEqual(TEXT("Compiler copies do not introduce issues"), WithIntermediates.Issues.Num(), Baseline.Issues.Num());

    const FBlueprintAnalysisResult StructuralAnalysis = UBlueprintAnalyzerLibrary::AnalyzeBlueprint(Blueprint.Get());
    TestEqual(TEXT("Structural analysis retains compiler intermediates"), StructuralAnalysis.Nodes.Num(), SourceCastCount * 2);
    return true;
}

#endif
