// Copyright (c) 2025 keemminxu. All Rights Reserved.

#include "BlueprintAnalyzerMenuExtension.h"
#include "BlueprintAnalyzerLibrary.h"
#include "BlueprintEditorContext.h"
#include "Engine/Blueprint.h"
#include "Blueprint/UserWidget.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserMenuContexts.h"
#include "IContentBrowserSingleton.h"
#include "IContentBrowserDataModule.h"
#include "ContentBrowserDataSubsystem.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ToolMenus.h"
#include "Misc/MessageDialog.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/ScopedSlowTask.h"
#include "Styling/AppStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

namespace
{
    FString GetLLMAnalysisDirectory()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LLMAnalisys")));
    }

    FString GetLLMAnalysisPath(const FString& PackageFolder, const FString& Filename)
    {
        // Preserve Unreal mount names (Game, Engine, plugin name), never local Content directory paths.
        if (!FPackageName::IsValidLongPackageName(PackageFolder + TEXT("/LLMAnalysis"), true))
        {
            return FString();
        }
        return FPaths::Combine(GetLLMAnalysisDirectory(), PackageFolder.RightChop(1), Filename);
    }

    bool SaveLLMTextFile(const FString& Content, const FString& OutputPath)
    {
        // The default file-writer flags replace an existing report without an overwrite dialog.
        return !OutputPath.IsEmpty()
            && IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true)
            && FFileHelper::SaveStringToFile(Content, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
}

void FBlueprintAnalyzerMenuExtension::Initialize()
{
    RegisterMenuExtensions();
}

void FBlueprintAnalyzerMenuExtension::Shutdown()
{
    // Cleanup
}

void FBlueprintAnalyzerMenuExtension::RegisterMenuExtensions()
{
    UToolMenus* ToolMenus = UToolMenus::Get();
    if (!ToolMenus)
    {
        return;
    }

    RegisterBlueprintEditorToolbarExtension(ToolMenus);

    // Folder context menu (Phase 4: batch analyze)
    if (UToolMenu* FolderMenu = ToolMenus->ExtendMenu("ContentBrowser.FolderContextMenu"))
    {
        FToolMenuSection& FolderSection = FolderMenu->FindOrAddSection("PathContextBulkOperations");
        FolderSection.AddSubMenu(
            "BlueprintAnalyzerFolder",
            FText::FromString("Blueprint Analyzer"),
            FText::FromString("Analyze Blueprints in the selected folders"),
            FNewToolMenuDelegate::CreateLambda([](UToolMenu* SubMenu)
            {
                const UContentBrowserFolderContext* FolderContext = SubMenu->FindContext<UContentBrowserFolderContext>();
                const TArray<FString> FolderPaths = FolderContext ? FolderContext->GetSelectedPackagePaths() : TArray<FString>();
                FToolMenuSection& ExportSection = SubMenu->AddSection("FolderBlueprintLLMExports", FText::FromString("Blueprint LLM Exports"));
                ExportSection.AddMenuEntry(
                    "ExportFolderBlueprintsToLLMText",
                    FText::FromString("Analyze Blueprints in Selected Folders for LLM"),
                    FText::FromString("Recursively export one LLM report per Blueprint in all selected folders to Saved/LLMAnalisys, replacing previous reports."),
                    FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
                    FUIAction(FExecuteAction::CreateLambda([FolderPaths]()
                    {
                        FBlueprintAnalyzerMenuExtension::ExecuteExportFolderBlueprintsToLLMText(FolderPaths);
                    }), FCanExecuteAction::CreateLambda([FolderPaths]()
                    {
                        return FolderPaths.ContainsByPredicate([](const FString& Path)
                        {
                            return FPackageName::IsValidLongPackageName(Path + TEXT("/LLMAnalysis"), true);
                        });
                    }))
                );

                FToolMenuSection& ProjectSection = SubMenu->AddSection("ProjectAnalysis", FText::FromString("Project Analysis"));
                ProjectSection.AddMenuEntry(
                    "AnalyzeFolder",
                    FText::FromString("Analyze Folder"),
                    FText::FromString("Analyze all Blueprints in this folder and show summary"),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeFolder))
                );
                ProjectSection.AddMenuEntry(
                    "ExportProjectToJSON",
                    FText::FromString("Export Project Analysis to JSON"),
                    FText::FromString("Save folder analysis as JSON"),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportProjectToJSON))
                );
                ProjectSection.AddMenuEntry(
                    "ExportProjectToLLMText",
                    FText::FromString("Export Project Analysis to LLM Text"),
                    FText::FromString("Save folder analysis as LLM-friendly text"),
                    FSlateIcon(),
                    FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportProjectToLLMText))
                );
            })
        );
    }

    // Use the base menu so selections containing different Blueprint subclasses or other asset types work too.
    UToolMenu* ContentBrowserAssetMenu = ToolMenus->ExtendMenu("ContentBrowser.AssetContextMenu");
    if (ContentBrowserAssetMenu)
    {
        FToolMenuSection& Section = ContentBrowserAssetMenu->FindOrAddSection("GetAssetActions");
        Section.AddDynamicEntry("BlueprintAnalyzerSelection", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
        {
            const UContentBrowserAssetContextMenuContext* Context = InSection.FindContext<UContentBrowserAssetContextMenuContext>();
            if (!Context || !Context->SelectedAssets.ContainsByPredicate([](const FAssetData& Asset)
            {
                return Asset.IsInstanceOf(UBlueprint::StaticClass());
            }))
            {
                return;
            }

            // Capture this menu's selection; another Content Browser may have a different active selection.
            const TArray<FAssetData> SelectedAssets = Context->SelectedAssets;
            InSection.AddSubMenu(
                "BlueprintAnalyzer",
                FText::FromString("Blueprint Analyzer"),
                FText::FromString("Blueprint analysis and optimization tools"),
                FNewToolMenuDelegate::CreateLambda([SelectedAssets](UToolMenu* SubMenu)
                {
                    FToolMenuSection& SubSection = SubMenu->AddSection("BlueprintAnalyzerActions", FText::FromString("Analysis Actions"));

                    if (SelectedAssets.Num() > 1)
                    {
                        SubSection.AddMenuEntry(
                            "AnalyzeSelectedBlueprintsForLLM",
                            FText::FromString("Analyze Selected Blueprints for LLM"),
                            FText::FromString("Save a separate LLM text file per selected Blueprint in Saved/LLMAnalisys, preserving asset folders and replacing previous reports."),
                            FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
                            FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
                            {
                                FBlueprintAnalyzerMenuExtension::ExecuteExportToLLMTextForAssets(SelectedAssets);
                            }))
                        );
                        return;
                    }

                    // Regular Blueprint Analysis
                    SubSection.AddMenuEntry(
                        "AnalyzeBlueprint",
                        FText::FromString("Analyze Blueprint"),
                        FText::FromString("Analyze the selected blueprint structure"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeBlueprint))
                    );

                    SubSection.AddMenuEntry(
                        "ExportToJSON",
                        FText::FromString("Export to JSON"),
                        FText::FromString("Export blueprint analysis to JSON format"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportToJSON))
                    );

                    SubSection.AddMenuEntry(
                        "ExportToLLMText",
                        FText::FromString("Export to LLM Text"),
                        FText::FromString("Save blueprint analysis in Saved/LLMAnalisys using its asset folder, replacing the previous report"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([SelectedAssets]()
                        {
                            FBlueprintAnalyzerMenuExtension::ExecuteExportToLLMTextForAssets(SelectedAssets);
                        }))
                    );

                    // Blueprint Performance Analysis (Phase 3)
                    FToolMenuSection& PerfSection = SubMenu->AddSection("BlueprintPerformanceActions", FText::FromString("Performance Analysis"));

                    PerfSection.AddMenuEntry(
                        "AnalyzeBlueprintPerformance",
                        FText::FromString("Analyze Blueprint Performance"),
                        FText::FromString("Detect performance anti-patterns (Tick-heavy calls, Cast abuse, etc.)"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeBlueprintPerformance))
                    );

                    PerfSection.AddMenuEntry(
                        "ExportPerformanceToJSON",
                        FText::FromString("Export Performance Report to JSON"),
                        FText::FromString("Save performance analysis as JSON"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportPerformanceToJSON))
                    );

                    PerfSection.AddMenuEntry(
                        "ExportPerformanceToLLMText",
                        FText::FromString("Export Performance Report to LLM Text"),
                        FText::FromString("Save performance analysis as LLM-friendly text"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportPerformanceToLLMText))
                    );

                    // Widget Blueprint Optimization
                    FToolMenuSection& WidgetSection = SubMenu->AddSection("WidgetAnalyzerActions", FText::FromString("Widget Optimization"));

                    WidgetSection.AddMenuEntry(
                        "AnalyzeWidgetBlueprint",
                        FText::FromString("Analyze Widget Blueprint"),
                        FText::FromString("Analyze widget blueprint for optimization opportunities"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeWidgetBlueprint))
                    );

                    WidgetSection.AddMenuEntry(
                        "ExportWidgetToJSON",
                        FText::FromString("Export Widget Analysis to JSON"),
                        FText::FromString("Export widget optimization report to JSON format"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportWidgetToJSON))
                    );

                    WidgetSection.AddMenuEntry(
                        "ExportWidgetToLLMText",
                        FText::FromString("Export Widget Analysis to LLM Text"),
                        FText::FromString("Export widget optimization report to LLM-friendly text format"),
                        FSlateIcon(),
                        FUIAction(FExecuteAction::CreateStatic(&FBlueprintAnalyzerMenuExtension::ExecuteExportWidgetToLLMText))
                    );
                })
            );
        }));
    }
}

void FBlueprintAnalyzerMenuExtension::RegisterBlueprintEditorToolbarExtension(UToolMenus* ToolMenus)
{
    if (!ToolMenus)
    {
        return;
    }

    static const FName BlueprintToolbarMenus[] =
    {
        FName(TEXT("AssetEditor.BlueprintEditor.ToolBar.GraphName")),
        FName(TEXT("AssetEditor.BlueprintEditor.ToolBar.DefaultsName")),
        FName(TEXT("AssetEditor.BlueprintEditor.ToolBar.ComponentsName")),
        FName(TEXT("AssetEditor.BlueprintEditor.ToolBar.InterfaceName")),
        FName(TEXT("AssetEditor.BlueprintEditor.ToolBar.MacroName")),
        FName(TEXT("AssetEditor.WidgetBlueprintEditor.ToolBar.DesignerName")),
        FName(TEXT("AssetEditor.WidgetBlueprintEditor.ToolBar.GraphName")),
        FName(TEXT("AssetEditor.WidgetBlueprintEditor.ToolBar.PreviewName"))
    };

    for (const FName ToolbarMenuName : BlueprintToolbarMenus)
    {
        if (UToolMenu* ToolbarMenu = ToolMenus->ExtendMenu(ToolbarMenuName))
        {
            FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("BlueprintAnalyzer");
            Section.InsertPosition = FToolMenuInsert("SourceControl", EToolMenuInsertType::After);
            AddBlueprintEditorToolbarButton(Section);
        }
    }
}

void FBlueprintAnalyzerMenuExtension::AddBlueprintEditorToolbarButton(FToolMenuSection& Section)
{
    Section.AddDynamicEntry("BlueprintAnalyzerAnalyzeCurrentBlueprint", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
    {
        const UBlueprintEditorToolMenuContext* Context = InSection.FindContext<UBlueprintEditorToolMenuContext>();
        UBlueprint* Blueprint = Context ? Context->GetBlueprintObj() : nullptr;
        if (!Blueprint)
        {
            return;
        }

        const TWeakObjectPtr<UBlueprint> WeakBlueprint(Blueprint);
        FToolMenuEntry& Entry = InSection.AddEntry(FToolMenuEntry::InitToolBarButton(
            "BlueprintAnalyzerAnalyzeCurrent",
            FUIAction(FExecuteAction::CreateLambda([WeakBlueprint]()
            {
                if (UBlueprint* CurrentBlueprint = WeakBlueprint.Get())
                {
                    FBlueprintAnalyzerMenuExtension::ExecuteExportToLLMTextForBlueprint(CurrentBlueprint);
                }
                else
                {
                    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Blueprint no longer available."), FText::FromString("Blueprint Analyzer"));
                }
            })),
            FText::FromString("Analyze LLM"),
            FText::FromString("Save an LLM-friendly analysis of this Blueprint in Saved/LLMAnalisys using its asset folder, replacing the previous report"),
            FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search")
        ));
        Entry.StyleNameOverride = "CalloutToolbar";
    }));
}

void FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeBlueprint()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    ExecuteAnalyzeBlueprintForBlueprint(SelectedBlueprint);
}

void FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeBlueprintForBlueprint(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint available."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FBlueprintAnalysisResult AnalysisResult = UBlueprintAnalyzerLibrary::AnalyzeBlueprint(Blueprint);
    ShowBlueprintAnalysisSummary(AnalysisResult);
}

void FBlueprintAnalyzerMenuExtension::ShowBlueprintAnalysisSummary(const FBlueprintAnalysisResult& AnalysisResult)
{
    const FBPAnalyzerMetadata& Meta = AnalysisResult.Metadata;
    FString Message;
    Message += FString::Printf(TEXT("Blueprint '%s' analyzed successfully!\n\n"), *AnalysisResult.BlueprintName);
    Message += FString::Printf(TEXT("Type: %s\n"), *Meta.BlueprintType);
    Message += FString::Printf(TEXT("Parent: %s\n"), *Meta.ParentClass);
    Message += FString::Printf(TEXT("Interfaces: %d\n"), Meta.ImplementedInterfaces.Num());
    Message += FString::Printf(TEXT("Variables: %d\n"), Meta.Variables.Num());
    Message += FString::Printf(TEXT("Custom Functions: %d\n"), Meta.CustomFunctions.Num());
    Message += FString::Printf(TEXT("Components: %d\n"), Meta.Components.Num());
    Message += FString::Printf(TEXT("Event Dispatchers: %d\n"), Meta.EventDispatchers.Num());
    Message += FString::Printf(TEXT("Macros: %d\n"), Meta.MacroNames.Num());
    Message += FString::Printf(TEXT("Timelines: %d\n"), Meta.TimelineNames.Num());
    Message += FString::Printf(TEXT("Nodes: %d\n"), AnalysisResult.Nodes.Num());
    Message += FString::Printf(TEXT("Connections: %d\n"), AnalysisResult.Connections.Num());
    Message += FString::Printf(TEXT("Execution Paths: %d"), AnalysisResult.ExecutionPaths.Num());

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportToJSON()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FString DefaultFilename = FString::Printf(TEXT("%s_Analysis.json"), *SelectedBlueprint->GetName());
    FString SavePath = ShowSaveFileDialog(DefaultFilename, TEXT("JSON Files (*.json)|*.json"));
    
    if (!SavePath.IsEmpty())
    {
        FBlueprintAnalysisResult AnalysisResult = UBlueprintAnalyzerLibrary::AnalyzeBlueprint(SelectedBlueprint);
        bool bSuccess = UBlueprintAnalyzerLibrary::SaveAnalysisToFile(AnalysisResult, SavePath, TEXT("JSON"));
        
        if (bSuccess)
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("Analysis exported to: %s"), *SavePath)));
        }
        else
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to export analysis."), FText::FromString("Blueprint Analyzer"));
        }
    }
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportToLLMTextForAssets(const TArray<FAssetData>& SelectedAssets)
{
    if (SelectedAssets.Num() == 1)
    {
        const TStrongObjectPtr<UBlueprint> Blueprint(Cast<UBlueprint>(SelectedAssets[0].GetAsset()));
        ExecuteExportToLLMTextForBlueprint(Blueprint.Get());
        return;
    }

    TArray<FAssetData> BlueprintAssets;
    TSet<FString> SeenAssets;
    int32 SkippedAssets = 0;
    for (const FAssetData& Asset : SelectedAssets)
    {
        if (!Asset.IsInstanceOf(UBlueprint::StaticClass()))
        {
            ++SkippedAssets;
            continue;
        }
        const FString AssetPath = Asset.GetObjectPathString();
        if (!SeenAssets.Contains(AssetPath))
        {
            SeenAssets.Add(AssetPath);
            BlueprintAssets.Add(Asset);
        }
    }
    if (BlueprintAssets.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No Blueprints selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    int32 ExportedFiles = 0;
    bool bCanceled = false;
    TArray<FString> FailedAssets;
    {
        FScopedSlowTask Progress(BlueprintAssets.Num(), FText::FromString("Exporting one LLM report per selected Blueprint"));
        Progress.MakeDialog(true);
        for (int32 Index = 0; Index < BlueprintAssets.Num(); ++Index)
        {
            if (Progress.ShouldCancel())
            {
                bCanceled = true;
                break;
            }
            const FAssetData& Asset = BlueprintAssets[Index];
            Progress.EnterProgressFrame(1, FText::FromString(FString::Printf(TEXT("Analyzing %s (%d/%d)"),
                *Asset.AssetName.ToString(), Index + 1, BlueprintAssets.Num())));

            const TStrongObjectPtr<UBlueprint> Blueprint(Cast<UBlueprint>(Asset.GetAsset()));
            if (!Blueprint.IsValid())
            {
                FailedAssets.Add(Asset.GetObjectPathString() + TEXT(" (failed to load)"));
                continue;
            }

            const FBlueprintAnalysisResult Analysis = UBlueprintAnalyzerLibrary::AnalyzeBlueprint(Blueprint.Get());
            if (Progress.ShouldCancel())
            {
                bCanceled = true;
                break;
            }

            const FString OutputPath = GetLLMAnalysisPath(Asset.PackagePath.ToString(),
                Asset.AssetName.ToString() + TEXT("_LLM_Analysis.txt"));
            if (!SaveLLMTextFile(UBlueprintAnalyzerLibrary::ExportToLLMText(Analysis), OutputPath))
            {
                FailedAssets.Add(Asset.GetObjectPathString() + TEXT(" (failed to save: ") + OutputPath + TEXT(")"));
                continue;
            }
            ++ExportedFiles;
        }
    }

    FString Message = FString::Printf(TEXT("%s\n\nLLM files exported: %d/%d\nNon-Blueprint assets skipped: %d\nOutput folder: %s"),
        bCanceled ? TEXT("Batch LLM export canceled. Files already exported are kept.") : TEXT("Batch LLM export finished."),
        ExportedFiles, BlueprintAssets.Num(), SkippedAssets, *GetLLMAnalysisDirectory());
    if (!FailedAssets.IsEmpty())
    {
        Message += TEXT("\n\nFailed exports:\n") + FString::Join(FailedAssets, TEXT("\n"));
    }
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message), FText::FromString("Blueprint Analyzer"));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportToLLMTextForBlueprint(UBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint available."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    const FString SavePath = GetLLMAnalysisPath(FPackageName::GetLongPackagePath(Blueprint->GetOutermost()->GetName()),
        Blueprint->GetName() + TEXT("_LLM_Analysis.txt"));
    const FBlueprintAnalysisResult AnalysisResult = UBlueprintAnalyzerLibrary::AnalyzeBlueprint(Blueprint);
    const bool bSuccess = SaveLLMTextFile(UBlueprintAnalyzerLibrary::ExportToLLMText(AnalysisResult), SavePath);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(bSuccess
        ? FString::Printf(TEXT("LLM-friendly analysis exported to: %s"), *SavePath)
        : TEXT("Failed to export analysis.")), FText::FromString("Blueprint Analyzer"));
}

void FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeWidgetBlueprint()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    if (!IsWidgetBlueprint(SelectedBlueprint))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Selected blueprint is not a Widget Blueprint."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FWidgetOptimizationReport Report = UBlueprintAnalyzerLibrary::AnalyzeWidgetBlueprint(SelectedBlueprint);
    
    FString SeverityText;
    if (Report.OptimizationScore >= 90)
    {
        SeverityText = TEXT("Excellent - Very Well Optimized");
    }
    else if (Report.OptimizationScore >= 70)
    {
        SeverityText = TEXT("Good - Well Optimized");
    }
    else if (Report.OptimizationScore >= 50)
    {
        SeverityText = TEXT("Average - Some Improvements Needed");
    }
    else if (Report.OptimizationScore >= 30)
    {
        SeverityText = TEXT("Poor - Significant Optimization Required");
    }
    else
    {
        SeverityText = TEXT("Critical - Complete Redesign Required");
    }
    
    FString Message = FString::Printf(TEXT("Widget Blueprint '%s' analyzed successfully!\n\n")
        TEXT("Total Widgets: %d\n")
        TEXT("Max Depth: %d\n")
        TEXT("Total Bindings: %d\n")
        TEXT("Optimization Issues: %d\n")
        TEXT("Optimization Score: %d/100 (%s)\n")
        TEXT("Estimated Memory: %.2f KB"), 
        *Report.WidgetBlueprintName,
        Report.TotalWidgets,
        Report.MaxDepth,
        Report.TotalBindings,
        Report.OptimizationIssues.Num(),
        Report.OptimizationScore,
        *SeverityText,
        Report.EstimatedMemoryUsage);
        
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportWidgetToJSON()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    if (!IsWidgetBlueprint(SelectedBlueprint))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Selected blueprint is not a Widget Blueprint."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FString DefaultFilename = FString::Printf(TEXT("%s_WidgetOptimization.json"), *SelectedBlueprint->GetName());
    FString SavePath = ShowSaveFileDialog(DefaultFilename, TEXT("JSON Files (*.json)|*.json"));
    
    if (!SavePath.IsEmpty())
    {
        FWidgetOptimizationReport Report = UBlueprintAnalyzerLibrary::AnalyzeWidgetBlueprint(SelectedBlueprint);
        bool bSuccess = UBlueprintAnalyzerLibrary::SaveWidgetAnalysisToFile(Report, SavePath, TEXT("JSON"));
        
        if (bSuccess)
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("Widget optimization report exported to: %s"), *SavePath)));
        }
        else
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to export widget analysis."), FText::FromString("Blueprint Analyzer"));
        }
    }
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportWidgetToLLMText()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    if (!IsWidgetBlueprint(SelectedBlueprint))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Selected blueprint is not a Widget Blueprint."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    const FString SavePath = GetLLMAnalysisPath(FPackageName::GetLongPackagePath(SelectedBlueprint->GetOutermost()->GetName()),
        SelectedBlueprint->GetName() + TEXT("_WidgetOptimization_LLM.txt"));
    const FWidgetOptimizationReport Report = UBlueprintAnalyzerLibrary::AnalyzeWidgetBlueprint(SelectedBlueprint);
    const bool bSuccess = SaveLLMTextFile(UBlueprintAnalyzerLibrary::ExportWidgetAnalysisToLLMText(Report), SavePath);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(bSuccess
        ? FString::Printf(TEXT("LLM-friendly widget optimization report exported to: %s"), *SavePath)
        : TEXT("Failed to export widget analysis.")), FText::FromString("Blueprint Analyzer"));
}

//void FBlueprintAnalyzerMenuExtension::ExecuteGenerateOptimizedCode()
//{
//    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
//    if (!SelectedBlueprint)
//    {
//        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
//        return;
//    }
//
//    if (!IsWidgetBlueprint(SelectedBlueprint))
//    {
//        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Selected blueprint is not a Widget Blueprint."), FText::FromString("Blueprint Analyzer"));
//        return;
//    }
//
//    FString DefaultFilename = FString::Printf(TEXT("Optimized%s.h"), *SelectedBlueprint->GetName());
//    FString SavePath = ShowSaveFileDialog(DefaultFilename, TEXT("Header Files (*.h)|*.h"));
//    
//    if (!SavePath.IsEmpty())
//    {
//        FWidgetOptimizationReport Report = UBlueprintAnalyzerLibrary::AnalyzeWidgetBlueprint(SelectedBlueprint);
//        FString OptimizedCode = UBlueprintAnalyzerLibrary::GenerateOptimizedWidgetCode(Report);
//        
//        bool bSuccess = FFileHelper::SaveStringToFile(OptimizedCode, *SavePath);
//        
//        if (bSuccess)
//        {
//            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("Optimized C++ code generated at: %s"), *SavePath)));
//        }
//        else
//        {
//            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("ExportFailed", "Failed to generate optimized code."));
//        }
//    }
//}

void FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeBlueprintPerformance()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FBPPerformanceReport Report = UBlueprintAnalyzerLibrary::AnalyzeBlueprintPerformance(SelectedBlueprint);

    FString Grade;
    if (Report.PerformanceScore >= 90) Grade = TEXT("Excellent");
    else if (Report.PerformanceScore >= 70) Grade = TEXT("Good");
    else if (Report.PerformanceScore >= 50) Grade = TEXT("Average");
    else if (Report.PerformanceScore >= 30) Grade = TEXT("Poor");
    else Grade = TEXT("Critical");

    FString Message;
    Message += FString::Printf(TEXT("Performance Report for '%s'\n\n"), *Report.BlueprintName);
    Message += FString::Printf(TEXT("Score: %d/100 (%s)\n"), Report.PerformanceScore, *Grade);
    Message += FString::Printf(TEXT("Total Nodes: %d\n"), Report.TotalNodes);
    Message += FString::Printf(TEXT("Events: %d\n"), Report.EventCount);
    Message += FString::Printf(TEXT("Casts: %d\n"), Report.CastCount);
    Message += FString::Printf(TEXT("Tick downstream: %d nodes\n"), Report.TickNodeCount);
    Message += FString::Printf(TEXT("BeginPlay downstream: %d nodes\n"), Report.BeginPlayNodeCount);
    Message += FString::Printf(TEXT("Issues Found: %d"), Report.Issues.Num());

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportPerformanceToJSON()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FString DefaultFilename = FString::Printf(TEXT("%s_Performance.json"), *SelectedBlueprint->GetName());
    FString SavePath = ShowSaveFileDialog(DefaultFilename, TEXT("JSON Files (*.json)|*.json"));
    if (SavePath.IsEmpty()) return;

    FBPPerformanceReport Report = UBlueprintAnalyzerLibrary::AnalyzeBlueprintPerformance(SelectedBlueprint);
    const FString Content = UBlueprintAnalyzerLibrary::ExportPerformanceReportToJSON(Report);
    const bool bSuccess = FFileHelper::SaveStringToFile(Content, *SavePath);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(bSuccess
        ? FString::Printf(TEXT("Performance report exported to: %s"), *SavePath)
        : TEXT("Failed to export performance report.")));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportPerformanceToLLMText()
{
    UBlueprint* SelectedBlueprint = GetSelectedBlueprint();
    if (!SelectedBlueprint)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No blueprint selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    const FString SavePath = GetLLMAnalysisPath(FPackageName::GetLongPackagePath(SelectedBlueprint->GetOutermost()->GetName()),
        SelectedBlueprint->GetName() + TEXT("_Performance_LLM.txt"));

    FBPPerformanceReport Report = UBlueprintAnalyzerLibrary::AnalyzeBlueprintPerformance(SelectedBlueprint);
    const FString Content = UBlueprintAnalyzerLibrary::ExportPerformanceReportToLLMText(Report);
    const bool bSuccess = SaveLLMTextFile(Content, SavePath);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(bSuccess
        ? FString::Printf(TEXT("LLM-friendly performance report exported to: %s"), *SavePath)
        : TEXT("Failed to export performance report.")));
}

// ============================================================
// Phase 4: Folder Analysis Execute Functions
// ============================================================

void FBlueprintAnalyzerMenuExtension::ExecuteExportFolderBlueprintsToLLMText(const TArray<FString>& FolderPaths)
{
    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());

    TArray<FString> ValidFolderPaths;
    for (const FString& FolderPath : FolderPaths)
    {
        if (FPackageName::IsValidLongPackageName(FolderPath + TEXT("/LLMAnalysis"), true))
        {
            Filter.PackagePaths.AddUnique(FName(*FolderPath));
            ValidFolderPaths.AddUnique(FolderPath);
        }
    }
    if (ValidFolderPaths.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No asset folders selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    TArray<FAssetData> BlueprintAssets;
    bool bQuerySucceeded = false;
    {
        FScopedSlowTask Progress(1, FText::FromString("Finding Blueprints in the selected folders and subfolders"));
        Progress.MakeDialog(true);
        Progress.EnterProgressFrame(1);
        if (Progress.ShouldCancel()) return;

        // Finish discovery in these paths even if the editor's initial registry scan is still running.
        AssetRegistry.ScanPathsSynchronous(ValidFolderPaths, false);
        if (Progress.ShouldCancel()) return;
        bQuerySucceeded = AssetRegistry.GetAssets(Filter, BlueprintAssets);
        if (Progress.ShouldCancel()) return;
    }
    if (!bQuerySucceeded)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("Failed to find Blueprints in the selected folders."), FText::FromString("Blueprint Analyzer"));
        return;
    }
    if (BlueprintAssets.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No Blueprints found in the selected folders or subfolders."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    BlueprintAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.GetObjectPathString() < B.GetObjectPathString();
    });
    ExecuteExportToLLMTextForAssets(BlueprintAssets);
}

FString FBlueprintAnalyzerMenuExtension::GetSelectedFolderPath()
{
    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
    IContentBrowserSingleton& ContentBrowser = ContentBrowserModule.Get();

    FString RawPath;

    TArray<FString> SelectedPaths;
    ContentBrowser.GetSelectedPathViewFolders(SelectedPaths);
    if (SelectedPaths.Num() > 0)
    {
        RawPath = SelectedPaths[0];
    }

    if (RawPath.IsEmpty())
    {
        TArray<FString> SelectedFolders;
        ContentBrowser.GetSelectedFolders(SelectedFolders);
        if (SelectedFolders.Num() > 0)
        {
            RawPath = SelectedFolders[0];
        }
    }

    if (RawPath.IsEmpty())
    {
        return FString();
    }

    // UE 5.1+: ContentBrowser returns virtual paths like "/All/Game/MyFolder".
    // AssetRegistry expects internal paths like "/Game/MyFolder".
    if (UContentBrowserDataSubsystem* DataSubsystem = IContentBrowserDataModule::Get().GetSubsystem())
    {
        FString InternalPath;
        if (DataSubsystem->TryConvertVirtualPath(RawPath, InternalPath) == EContentBrowserPathType::Internal)
        {
            return InternalPath;
        }
    }

    // Fallback: strip "/All" virtual root if conversion is unavailable.
    if (RawPath.StartsWith(TEXT("/All/")))
    {
        return RawPath.RightChop(4);
    }

    return RawPath;
}

void FBlueprintAnalyzerMenuExtension::ExecuteAnalyzeFolder()
{
    const FString FolderPath = GetSelectedFolderPath();
    if (FolderPath.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No folder selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FBPProjectAnalysis Analysis = UBlueprintAnalyzerLibrary::AnalyzeFolder(FolderPath);

    FString Message;
    Message += FString::Printf(TEXT("Project Analysis: %s\n\n"), *Analysis.FolderPath);
    Message += FString::Printf(TEXT("Blueprints: %d\n"), Analysis.BlueprintsAnalyzed);
    Message += FString::Printf(TEXT("Total nodes: %d\n"), Analysis.TotalNodes);
    Message += FString::Printf(TEXT("Average score: %.1f/100\n"), Analysis.AveragePerformanceScore);
    Message += FString::Printf(TEXT("Dependencies: %d\n"), Analysis.Dependencies.Num());
    Message += FString::Printf(TEXT("Circular chains: %d"), Analysis.CircularDependencyChains.Num());

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportProjectToJSON()
{
    const FString FolderPath = GetSelectedFolderPath();
    if (FolderPath.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No folder selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    FString SavePath = ShowSaveFileDialog(TEXT("ProjectAnalysis.json"), TEXT("JSON Files (*.json)|*.json"));
    if (SavePath.IsEmpty()) return;

    FBPProjectAnalysis Analysis = UBlueprintAnalyzerLibrary::AnalyzeFolder(FolderPath);
    const FString Content = UBlueprintAnalyzerLibrary::ExportProjectAnalysisToJSON(Analysis);
    const bool bSuccess = FFileHelper::SaveStringToFile(Content, *SavePath);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(bSuccess
        ? FString::Printf(TEXT("Project analysis exported to: %s"), *SavePath)
        : TEXT("Failed to export project analysis.")));
}

void FBlueprintAnalyzerMenuExtension::ExecuteExportProjectToLLMText()
{
    const FString FolderPath = GetSelectedFolderPath();
    if (FolderPath.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString("No folder selected."), FText::FromString("Blueprint Analyzer"));
        return;
    }

    const FString SavePath = GetLLMAnalysisPath(FolderPath, TEXT("ProjectAnalysis_LLM.txt"));

    FBPProjectAnalysis Analysis = UBlueprintAnalyzerLibrary::AnalyzeFolder(FolderPath);
    const FString Content = UBlueprintAnalyzerLibrary::ExportProjectAnalysisToLLMText(Analysis);
    const bool bSuccess = SaveLLMTextFile(Content, SavePath);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(bSuccess
        ? FString::Printf(TEXT("LLM-friendly project analysis exported to: %s"), *SavePath)
        : TEXT("Failed to export project analysis.")));
}

UBlueprint* FBlueprintAnalyzerMenuExtension::GetSelectedBlueprint()
{
    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
    TArray<FAssetData> SelectedAssets;
    ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
    
    UE_LOG(LogTemp, Warning, TEXT("BlueprintAnalyzer: Selected assets count: %d"), SelectedAssets.Num());
    
    for (const FAssetData& AssetData : SelectedAssets)
    {
        UE_LOG(LogTemp, Warning, TEXT("BlueprintAnalyzer: Asset: %s, Class: %s"), *AssetData.AssetName.ToString(), *AssetData.AssetClassPath.ToString());
        
        if (AssetData.AssetClassPath == UBlueprint::StaticClass()->GetClassPathName())
        {
            UE_LOG(LogTemp, Warning, TEXT("BlueprintAnalyzer: Found Blueprint: %s"), *AssetData.AssetName.ToString());
            return Cast<UBlueprint>(AssetData.GetAsset());
        }
        
        if (AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint")))
        {
            UE_LOG(LogTemp, Warning, TEXT("BlueprintAnalyzer: Found Blueprint variant: %s"), *AssetData.AssetName.ToString());
            if (UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset()))
            {
                return Blueprint;
            }
        }
    }
    
    return nullptr;
}

bool FBlueprintAnalyzerMenuExtension::IsWidgetBlueprint(UBlueprint* Blueprint)
{
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        return false;
    }
    
    return Blueprint->GeneratedClass->IsChildOf<UUserWidget>();
}

FString FBlueprintAnalyzerMenuExtension::ShowSaveFileDialog(const FString& DefaultFilename, const FString& FileTypes)
{
    TArray<FString> SaveFilenames;
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    
    if (DesktopPlatform)
    {
        TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
        void* ParentWindowHandle = (ParentWindow.IsValid() && ParentWindow->GetNativeWindow().IsValid()) ? ParentWindow->GetNativeWindow()->GetOSWindowHandle() : nullptr;
        
        bool bSaved = DesktopPlatform->SaveFileDialog(
            ParentWindowHandle,
            TEXT("Save Analysis"),
            FPaths::ProjectSavedDir(),
            DefaultFilename,
            FileTypes,
            EFileDialogFlags::None,
            SaveFilenames
        );
        
        if (bSaved && SaveFilenames.Num() > 0)
        {
            return SaveFilenames[0];
        }
    }
    
    return FString();
}
