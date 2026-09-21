#include "SkiP1Widget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SkiSelectorBrowser.h"

void USkiP1Widget::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    if (!WidgetTree || WidgetTree->RootWidget) return;
    UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("P1Shell"));
    WidgetTree->RootWidget = Root;
    SelectorPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SelectorPanel"));
    if (UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(SelectorPanel)) { CanvasSlot->SetPosition({24,24}); CanvasSlot->SetSize({640,600}); }
    UTextBlock* Heading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("P1Heading"));
    Heading->SetText(FText::FromString(TEXT("Mountain Planner — Terrain Preparation")));
    SelectorPanel->AddChildToVerticalBox(Heading); ConfigureSelector();
    UVerticalBox* StatusPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StatusPanel"));
    if (UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(StatusPanel))
    { CanvasSlot->SetAnchors(FAnchors(1,0)); CanvasSlot->SetAlignment({1,0}); CanvasSlot->SetPosition({-24,24}); CanvasSlot->SetSize({520,620}); }
    ProgressText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PreparationProgress"));
    ProgressText->SetAutoWrapText(true); ProgressText->SetText(FText::FromString(TEXT("Selected — awaiting bounds"))); StatusPanel->AddChildToVerticalBox(ProgressText);
    ProgressBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("PreparationProgressBar")); StatusPanel->AddChildToVerticalBox(ProgressBar);
    StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TransientStatus"));
    StatusText->SetAutoWrapText(true); StatusText->SetText(FText::FromString(TEXT("Selector owns input until preparation starts."))); StatusPanel->AddChildToVerticalBox(StatusText);
    DetailsText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TerrainDetails"));
    DetailsText->SetAutoWrapText(true); DetailsText->SetText(FText::FromString(TEXT("No terrain installed."))); StatusPanel->AddChildToVerticalBox(DetailsText);
    ProbeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ProbeDetails"));
    ProbeText->SetAutoWrapText(true); ProbeText->SetText(FText::FromString(TEXT("Left-click terrain to inspect canonical samples."))); StatusPanel->AddChildToVerticalBox(ProbeText);
    RetryButton = AddCommandButton(StatusPanel, TEXT("Retry preparation"), TEXT("Retry")); RetryButton->SetVisibility(ESlateVisibility::Collapsed);
    ChangeSelectionButton = AddCommandButton(StatusPanel, TEXT("Change selection"), TEXT("ChangeSelection")); ChangeSelectionButton->SetVisibility(ESlateVisibility::Collapsed);
    AddCommandButton(StatusPanel, TEXT("Presentation"), TEXT("Presentation")); AddCommandButton(StatusPanel, TEXT("Hypsometric elevation"), TEXT("Elevation"));
    AddCommandButton(StatusPanel, TEXT("Slope classes"), TEXT("Slope")); AddCommandButton(StatusPanel, TEXT("Cover classes"), TEXT("Cover"));
    AddCommandButton(StatusPanel, TEXT("Tile / LOD colors"), TEXT("Lod"));
    AddCommandButton(StatusPanel, TEXT("LOD 0 (full)"), TEXT("Lod0")); AddCommandButton(StatusPanel, TEXT("LOD 1 (half)"), TEXT("Lod1")); AddCommandButton(StatusPanel, TEXT("LOD 2 (quarter)"), TEXT("Lod2"));
    AddCommandButton(StatusPanel, TEXT("Vertical 1×"), TEXT("Vertical1")); AddCommandButton(StatusPanel, TEXT("Vertical 2×"), TEXT("Vertical2")); AddCommandButton(StatusPanel, TEXT("Vertical 4×"), TEXT("Vertical4"));
    AddCommandButton(StatusPanel, TEXT("Clear midday"), TEXT("Midday")); AddCommandButton(StatusPanel, TEXT("Low angle"), TEXT("LowAngle")); AddCommandButton(StatusPanel, TEXT("Overcast"), TEXT("Overcast"));
    UTextBlock* Controls = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Controls")); Controls->SetAutoWrapText(true);
    Controls->SetText(FText::FromString(TEXT("RMB orbit • MMB pan • wheel zoom • F/Home frame all • 1 full • 2 close • 3 last probe • Esc release"))); StatusPanel->AddChildToVerticalBox(Controls);
}

void USkiP1Widget::ConfigureSelector()
{
    if (!SelectorPanel) return; if (Selector) Selector->RemoveFromParent();
    Selector = WidgetTree->ConstructWidget<USkiSelectorBrowser>(USkiSelectorBrowser::StaticClass(), TEXT("MapSelector"));
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("P1Selector/index.html")); Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    ++SelectorGeneration; const FString Auto = FParse::Param(FCommandLine::Get(), TEXT("SkiP1SelectorSmoke")) ? TEXT("&autotest=1") : TEXT("");
    const FString Url = FString::Printf(TEXT("file:///%s?token=%s&generation=%llu%s"), *Path, *Token, SelectorGeneration, *Auto);
    Selector->Configure(Url, Token, SelectorGeneration, [this](const SkiPreparation::Request& R){ AcceptSelection(R); }, [this](const FString& E){ SetTransientStatus(E); });
    if (UVerticalBoxSlot* BrowserSlot = SelectorPanel->AddChildToVerticalBox(Selector)) BrowserSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
}

UButton* USkiP1Widget::AddCommandButton(UVerticalBox* Parent, const TCHAR* Label, const FName Name)
{
    UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name); UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
    Text->SetText(FText::FromString(Label)); Button->AddChild(Text); Parent->AddChildToVerticalBox(Button);
    if (Name==TEXT("Retry")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::RetryClicked); else if (Name==TEXT("ChangeSelection")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ChangeSelectionClicked);
    else if (Name==TEXT("Presentation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::PresentationClicked); else if (Name==TEXT("Elevation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ElevationClicked);
    else if (Name==TEXT("Slope")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::SlopeClicked); else if (Name==TEXT("Cover")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::CoverClicked);
    else if (Name==TEXT("Lod")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::LodClicked); else if(Name==TEXT("Lod0"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod0Clicked); else if(Name==TEXT("Lod1"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod1Clicked); else if(Name==TEXT("Lod2"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod2Clicked);
    else if(Name==TEXT("Vertical1"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical1Clicked); else if(Name==TEXT("Vertical2"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical2Clicked); else if(Name==TEXT("Vertical4"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical4Clicked);
    else if(Name==TEXT("Midday"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::MiddayClicked); else if(Name==TEXT("LowAngle"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::LowAngleClicked); else Button->OnClicked.AddDynamic(this,&USkiP1Widget::OvercastClicked);
    return Button;
}

void USkiP1Widget::SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> H){SelectionHandler=std::move(H);} void USkiP1Widget::AcceptSelection(const SkiPreparation::Request& R){SetTransientStatus(TEXT("Selection accepted; browser resources released."));if(SelectionHandler)SelectionHandler(R);}
void USkiP1Widget::SetPreparationProgress(const SkiPreparation::Progress& P){if(ProgressText)ProgressText->SetText(FText::FromString(FString::Printf(TEXT("%s — %s"),SkiPreparation::StateName(P.Phase),*P.Detail)));if(ProgressBar)ProgressBar->SetPercent(P.Total.IsSet()&&P.Total.GetValue()?static_cast<float>(P.Completed)/P.Total.GetValue():0);}
void USkiP1Widget::SetTransientStatus(const FString& S){if(StatusText)StatusText->SetText(FText::FromString(S));} void USkiP1Widget::SetProbeStatus(const FString& S){if(ProbeText)ProbeText->SetText(FText::FromString(S));}
void USkiP1Widget::SetTerrainDetails(const FString& D,const bool Synthetic){if(DetailsText)DetailsText->SetText(FText::FromString((Synthetic?TEXT("SYNTHETIC FIXTURE — not representative of selected terrain\n"):TEXT(""))+D));}
void USkiP1Widget::ShowPreparationFailure(const TOptional<SkiPreparation::ProviderFailure>& Failure,const FString& Fallback,TFunction<void()> Retry,TFunction<void()> Change)
{RetryHandler=std::move(Retry);ChangeSelectionHandler=std::move(Change);FString M=Fallback;if(Failure.IsSet()){const auto& F=Failure.GetValue();const FString Metadata=F.MetadataTag?FString::Printf(TEXT("\nMetadata tag %u type %d count %d pass-count %s"),F.MetadataTag,F.MetadataType,F.MetadataReadCount,F.MetadataPassCount?TEXT("yes"):TEXT("no")):FString();M=FString::Printf(TEXT("%s\n%s — %s\n%s %ux%u compression %u%s\nGeoreference: %s\nDiagnostic: Saved/%s"),*F.Code,SkiPreparation::ProviderProductName(F.Product),SkiPreparation::FailureStageName(F.Stage),*F.Organization,F.Width,F.Height,F.Compression,*Metadata,F.GeoreferenceStatus.IsEmpty()?TEXT("not reported"):*F.GeoreferenceStatus,F.DiagnosticReceipt.IsEmpty()?TEXT("diagnostic was not written"):*F.DiagnosticReceipt);}SetTransientStatus(M);RetryButton->SetVisibility(ESlateVisibility::Visible);ChangeSelectionButton->SetVisibility(ESlateVisibility::Visible);}
void USkiP1Widget::ResetSelector(){RetryButton->SetVisibility(ESlateVisibility::Collapsed);ChangeSelectionButton->SetVisibility(ESlateVisibility::Collapsed);ConfigureSelector();SetTransientStatus(TEXT("Choose new bounds; selector token and generation were replaced."));}
void USkiP1Widget::SetViewCommandHandler(TFunction<void(FName)> H){ViewCommandHandler=std::move(H);} void USkiP1Widget::CloseSelector(){if(Selector)Selector->Close();} bool USkiP1Widget::IsP1Ready()const{return WidgetTree&&WidgetTree->RootWidget&&Selector&&ProgressText&&ProgressBar&&StatusText;}
void USkiP1Widget::RetryClicked(){if(RetryHandler)RetryHandler();} void USkiP1Widget::ChangeSelectionClicked(){if(ChangeSelectionHandler)ChangeSelectionHandler();}
void USkiP1Widget::PresentationClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Presentation"));} void USkiP1Widget::ElevationClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Elevation"));}
void USkiP1Widget::SlopeClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Slope"));} void USkiP1Widget::CoverClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Cover"));} void USkiP1Widget::LodClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod"));}
void USkiP1Widget::Lod0Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod0"));} void USkiP1Widget::Lod1Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod1"));} void USkiP1Widget::Lod2Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod2"));}
void USkiP1Widget::Vertical1Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical1"));} void USkiP1Widget::Vertical2Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical2"));} void USkiP1Widget::Vertical4Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical4"));}
void USkiP1Widget::MiddayClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Midday"));} void USkiP1Widget::LowAngleClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("LowAngle"));} void USkiP1Widget::OvercastClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Overcast"));}
