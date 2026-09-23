#include "SkiP1Widget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "SkiSelectorBrowser.h"

double USkiP1Widget::CalculateStatusPanelWidth(const double ViewportWidth) noexcept
{
    return FMath::Clamp(ViewportWidth - 48.0, 360.0, 520.0);
}

FVector2D USkiP1Widget::CalculateSelectorPanelSize(const FIntPoint ViewportSize) noexcept
{
    return {FMath::Max(280.0, FMath::Min(640.0, ViewportSize.X - 48.0)),
        FMath::Max(320.0, FMath::Min(600.0, ViewportSize.Y - 48.0))};
}

void USkiP1Widget::NativeTick(const FGeometry& MyGeometry, const float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);
    if (bOpenInstalledPending)
    {
        if (IsSelectorClosed())
        {
            bOpenInstalledPending = false;
            if (OpenInstalledButton) OpenInstalledButton->SetIsEnabled(true);
            if (OpenInstalledHandler) OpenInstalledHandler();
        }
        else if (FPlatformTime::Seconds() - OpenInstalledCloseStartedSeconds > 2.0)
        {
            bOpenInstalledPending = false;
            if (OpenInstalledButton) OpenInstalledButton->SetIsEnabled(true);
            SetSelectorStatus(TEXT("The map browser did not release; installed terrain was not opened."));
        }
    }
    if (StatusPanel)
    {
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(StatusPanel->Slot))
            CanvasSlot->SetOffsets(FMargin(-24, 24, CalculateStatusPanelWidth(MyGeometry.GetLocalSize().X), 24));
    }
    if (SelectorPanel)
    {
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(SelectorPanel->Slot))
            CanvasSlot->SetSize(CalculateSelectorPanelSize(FIntPoint(
                FMath::RoundToInt(MyGeometry.GetLocalSize().X),
                FMath::RoundToInt(MyGeometry.GetLocalSize().Y))));
    }
}

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
    SelectorPanel->AddChildToVerticalBox(Heading);
    OpenInstalledButton = AddCommandButton(SelectorPanel, TEXT("Open last installed terrain (offline)"), TEXT("OpenInstalled"));
    SelectorStatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SelectorStatus"));
    SelectorStatusText->SetAutoWrapText(true);
    SelectorPanel->AddChildToVerticalBox(SelectorStatusText);
    StatusPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StatusPanel"));
    if (UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(StatusPanel))
    {
        CanvasSlot->SetAnchors(FAnchors(1,0,1,1));
        CanvasSlot->SetAlignment({1,0});
        CanvasSlot->SetOffsets(FMargin(-24,24,520,24));
    }
    ProgressText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PreparationProgress"));
    ProgressText->SetAutoWrapText(false); ProgressText->SetText(FText::FromString(TEXT("Selected — awaiting bounds"))); StatusPanel->AddChildToVerticalBox(ProgressText);
    ProgressBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("PreparationProgressBar")); StatusPanel->AddChildToVerticalBox(ProgressBar);
    RetryButton = AddCommandButton(StatusPanel, TEXT("Retry preparation"), TEXT("Retry")); RetryButton->SetVisibility(ESlateVisibility::Collapsed);
    ChangeSelectionButton = AddCommandButton(StatusPanel, TEXT("Change selection"), TEXT("ChangeSelection")); ChangeSelectionButton->SetVisibility(ESlateVisibility::Collapsed);
    StatusScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("StatusScroll"));
    if (UVerticalBoxSlot* ScrollSlot = StatusPanel->AddChildToVerticalBox(StatusScroll))
        ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    UVerticalBox* ScrollContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StatusScrollContent"));
    StatusScroll->AddChild(ScrollContent);
    StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TransientStatus"));
    StatusText->SetAutoWrapText(true); StatusText->SetText(FText::FromString(TEXT("Selector owns input until preparation starts."))); ScrollContent->AddChildToVerticalBox(StatusText);
    DetailsText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TerrainDetails"));
    DetailsText->SetAutoWrapText(true); DetailsText->SetText(FText::FromString(TEXT("No terrain installed."))); ScrollContent->AddChildToVerticalBox(DetailsText);
    ProbeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ProbeDetails"));
    ProbeText->SetAutoWrapText(true); ProbeText->SetText(FText::FromString(TEXT("Left-click terrain to inspect canonical samples."))); ScrollContent->AddChildToVerticalBox(ProbeText);
    NodeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("RuntimeNodeView"));
    NodeText->SetAutoWrapText(true); NodeText->SetText(FText::FromString(TEXT("Runtime node view\n• Source bounds\n• Required ground + cover\n• Render/query revisions"))); ScrollContent->AddChildToVerticalBox(NodeText);
    ReadyControls = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ReadyControls"));
    ScrollContent->AddChildToVerticalBox(ReadyControls);
    UUniformGridPanel* Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(), TEXT("TerrainControlGrid"));
    ReadyControls->AddChildToVerticalBox(Grid);
    AddGridCommandButton(Grid, TEXT("Presentation"), TEXT("Presentation"), 0, 0); AddGridCommandButton(Grid, TEXT("Elevation"), TEXT("Elevation"), 0, 1);
    AddGridCommandButton(Grid, TEXT("Slope classes"), TEXT("Slope"), 1, 0); AddGridCommandButton(Grid, TEXT("Cover classes"), TEXT("Cover"), 1, 1);
    AddGridCommandButton(Grid, TEXT("Tile / LOD"), TEXT("Lod"), 2, 0); AddGridCommandButton(Grid, TEXT("LOD Auto"), TEXT("LodAuto"), 2, 1);
    AddGridCommandButton(Grid, TEXT("LOD 0 (full)"), TEXT("Lod0"), 3, 0); AddGridCommandButton(Grid, TEXT("LOD 1 (half)"), TEXT("Lod1"), 3, 1);
    AddGridCommandButton(Grid, TEXT("LOD 2 (quarter)"), TEXT("Lod2"), 4, 0); AddGridCommandButton(Grid, TEXT("Vertical 1×"), TEXT("Vertical1"), 4, 1);
    AddGridCommandButton(Grid, TEXT("Vertical 2×"), TEXT("Vertical2"), 5, 0); AddGridCommandButton(Grid, TEXT("Vertical 4×"), TEXT("Vertical4"), 5, 1);
    AddGridCommandButton(Grid, TEXT("Clear midday"), TEXT("Midday"), 6, 0); AddGridCommandButton(Grid, TEXT("Low angle"), TEXT("LowAngle"), 6, 1);
    AddGridCommandButton(Grid, TEXT("Overcast"), TEXT("Overcast"), 7, 0);
    UTextBlock* Controls = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Controls")); Controls->SetAutoWrapText(true);
    Controls->SetText(FText::FromString(TEXT("RMB orbit • MMB pan • wheel zoom • F/Home frame all • 1 full • 2 close • 3 last probe • Esc release"))); ReadyControls->AddChildToVerticalBox(Controls);
    SetShellState(EP1ShellState::Selecting);
}

void USkiP1Widget::ConfigureSelector()
{
    if (!SelectorPanel) return; if (Selector) { Selector->Close(); Selector->RemoveFromParent(); }
    Selector = WidgetTree->ConstructWidget<USkiSelectorBrowser>(USkiSelectorBrowser::StaticClass(), TEXT("MapSelector"));
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("P1Selector/index.html")); Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    ++SelectorGeneration; const FString Auto = FParse::Param(FCommandLine::Get(), TEXT("SkiP1SelectorSmoke")) ? TEXT("&autotest=1") : TEXT("");
    FString DiagnosticPhase;
    const bool bDiagnostic = FParse::Value(FCommandLine::Get(), TEXT("SkiP1SelectorDiagnostic="), DiagnosticPhase);
    const FString Diagnostic = bDiagnostic && DiagnosticPhase == TEXT("no-tiles")
        ? TEXT("&diagnostic=no-tiles") : TEXT("");
    const FString Url = FString::Printf(TEXT("file:///%s?token=%s&generation=%llu%s%s"), *Path, *Token, SelectorGeneration, *Auto, *Diagnostic);
    Selector->Configure(Url, Token, SelectorGeneration, [this](const SkiPreparation::Request& R){ AcceptSelection(R); }, [this](const FString& E){ SetSelectorStatus(E); });
    if (UVerticalBoxSlot* BrowserSlot = SelectorPanel->AddChildToVerticalBox(Selector)) BrowserSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
}

UButton* USkiP1Widget::AddCommandButton(UVerticalBox* Parent, const TCHAR* Label, const FName Name)
{
    UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name); UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
    Text->SetText(FText::FromString(Label)); Button->AddChild(Text);
    if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Text->Slot)) ButtonSlot->SetPadding(FMargin(8.0F));
    Parent->AddChildToVerticalBox(Button);
    BindCommandButton(Button, Name);
    return Button;
}

UButton* USkiP1Widget::AddGridCommandButton(UUniformGridPanel* Parent, const TCHAR* Label,
    const FName Name, const int32 Row, const int32 Column)
{
    UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
    UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(); Text->SetText(FText::FromString(Label));
    Button->AddChild(Text);
    if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Text->Slot)) ButtonSlot->SetPadding(FMargin(8.0F));
    Parent->AddChildToUniformGrid(Button, Row, Column);
    BindCommandButton(Button, Name);
    return Button;
}

void USkiP1Widget::BindCommandButton(UButton* Button, const FName Name)
{
    if (Name==TEXT("Retry")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::RetryClicked); else if (Name==TEXT("ChangeSelection")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ChangeSelectionClicked);
    else if (Name==TEXT("OpenInstalled")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::OpenInstalledClicked);
    else if (Name==TEXT("Presentation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::PresentationClicked); else if (Name==TEXT("Elevation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ElevationClicked);
    else if (Name==TEXT("Slope")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::SlopeClicked); else if (Name==TEXT("Cover")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::CoverClicked);
    else if (Name==TEXT("Lod")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::LodClicked); else if(Name==TEXT("LodAuto"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::LodAutoClicked); else if(Name==TEXT("Lod0"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod0Clicked); else if(Name==TEXT("Lod1"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod1Clicked); else if(Name==TEXT("Lod2"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod2Clicked);
    else if(Name==TEXT("Vertical1"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical1Clicked); else if(Name==TEXT("Vertical2"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical2Clicked); else if(Name==TEXT("Vertical4"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical4Clicked);
    else if(Name==TEXT("Midday"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::MiddayClicked); else if(Name==TEXT("LowAngle"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::LowAngleClicked); else Button->OnClicked.AddDynamic(this,&USkiP1Widget::OvercastClicked);
}

void USkiP1Widget::SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> H){SelectionHandler=std::move(H);} void USkiP1Widget::AcceptSelection(const SkiPreparation::Request& R){SetTransientStatus(TEXT("Selection accepted; browser resources released."));if(SelectionHandler)SelectionHandler(R);}
void USkiP1Widget::SetOpenInstalledHandler(TFunction<void()> H){OpenInstalledHandler=std::move(H);}
void USkiP1Widget::SetSelectorStatus(const FString& Status){if(SelectorStatusText)SelectorStatusText->SetText(FText::FromString(Status));}
void USkiP1Widget::SetPreparationProgress(const SkiPreparation::Progress& P){SetShellState(EP1ShellState::Preparing);if(ProgressText){FString Detail=P.Detail.Left(120);Detail.ReplaceInline(TEXT("\n"),TEXT(" "));ProgressText->SetText(FText::FromString(FString::Printf(TEXT("%s — %s"),SkiPreparation::StateName(P.Phase),*Detail)));}if(ProgressBar)ProgressBar->SetPercent(P.Total.IsSet()&&P.Total.GetValue()?static_cast<float>(P.Completed)/P.Total.GetValue():0);}
void USkiP1Widget::SetTransientStatus(const FString& S){if(StatusText)StatusText->SetText(FText::FromString(S));} void USkiP1Widget::SetProbeStatus(const FString& S){if(ProbeText)ProbeText->SetText(FText::FromString(S));}
void USkiP1Widget::SetNodeStatus(const FString& S){if(NodeText)NodeText->SetText(FText::FromString(S));}
void USkiP1Widget::SetTerrainDetails(const FString& D,const bool Synthetic){SetShellState(EP1ShellState::Ready);if(DetailsText)DetailsText->SetText(FText::FromString((Synthetic?TEXT("SYNTHETIC FIXTURE — not representative of selected terrain\n"):TEXT(""))+D));}
void USkiP1Widget::BeginPreparationUI(TFunction<void()> Change){ChangeSelectionHandler=std::move(Change);SetShellState(EP1ShellState::Preparing);if(ChangeSelectionButton)ChangeSelectionButton->SetVisibility(ESlateVisibility::Visible);}
void USkiP1Widget::ShowPreparationFailure(const TOptional<SkiPreparation::ProviderFailure>& Failure,const FString& Fallback,TFunction<void()> Retry,TFunction<void()> Change)
{RetryHandler=std::move(Retry);ChangeSelectionHandler=std::move(Change);SetShellState(EP1ShellState::Failed);FString M=Fallback;if(Failure.IsSet()){const auto& F=Failure.GetValue();const FString Receipt=F.DiagnosticReceipt.IsEmpty()?TEXT("not written"):FPaths::GetCleanFilename(F.DiagnosticReceipt);if(F.Stage==SkiPreparation::FailureStage::Acquisition){M=FString::Printf(TEXT("%s\n%s — %s (HTTP %d)\n%ux%u · tile %d/%d · attempt %d/%d · %.1f s\nDiagnostic: %s"),SkiPreparation::ProviderProductName(F.Product),F.TransportFailure.IsEmpty()?TEXT("Acquisition failed"):*F.TransportFailure,*F.RequestStatus,F.HttpStatus,F.RequestedWidth,F.RequestedHeight,F.TileIndex,F.TileCount,F.Attempt,F.MaximumAttempts,F.ElapsedSeconds,*Receipt);}else{const FString Metadata=F.MetadataTag?FString::Printf(TEXT("\nMetadata tag %u type %d count %d pass-count %s"),F.MetadataTag,F.MetadataType,F.MetadataReadCount,F.MetadataPassCount?TEXT("yes"):TEXT("no")):FString();M=FString::Printf(TEXT("%s\n%s — %s\n%s%s\nDiagnostic: %s"),*F.Code,SkiPreparation::ProviderProductName(F.Product),SkiPreparation::FailureStageName(F.Stage),F.Width?*FString::Printf(TEXT("%s %ux%u compression %u"),*F.Organization,F.Width,F.Height,F.Compression):TEXT("No decoded raster"),*Metadata,*Receipt);}}SetTransientStatus(M);RetryButton->SetVisibility(Failure.IsSet()&&Failure->Retry==SkiPreparation::RetryClassification::Retryable?ESlateVisibility::Visible:ESlateVisibility::Collapsed);RetryButton->SetIsEnabled(true);ChangeSelectionButton->SetVisibility(ESlateVisibility::Visible);}
// CEF is created only while the Selecting state owns the shell: every other state (and every
// scene-only capture or regression) runs without a browser process or network-capable profile.
void USkiP1Widget::OpenSelector(){bOpenInstalledPending=false;if(OpenInstalledButton)OpenInstalledButton->SetIsEnabled(true);SetShellState(EP1ShellState::Selecting);ConfigureSelector();}
void USkiP1Widget::NativeDestruct(){bOpenInstalledPending=false;CloseSelector();Super::NativeDestruct();}
bool USkiP1Widget::HasLiveSelector()const{return Selector&&!Selector->IsClosed();}
bool USkiP1Widget::GetSelectorTeardownProof(bool& OutBridgeUnbound,bool& OutCefClosed,bool& OutReleased,double& OutCloseMs)const{if(!Selector)return false;OutBridgeUnbound=Selector->WasBridgeUnbound();OutCefClosed=Selector->WasCefBrowserClosed();OutReleased=Selector->WasWindowReleased();OutCloseMs=Selector->GetCloseMilliseconds();return true;}
void USkiP1Widget::ResetSelector(){bOpenInstalledPending=false;if(OpenInstalledButton)OpenInstalledButton->SetIsEnabled(true);SetShellState(EP1ShellState::Selecting);ConfigureSelector();SetSelectorStatus(TEXT("Choose new bounds; selector token and generation were replaced."));}
void USkiP1Widget::SetViewCommandHandler(TFunction<void(FName)> H){ViewCommandHandler=std::move(H);} void USkiP1Widget::CloseSelector(){if(Selector&&!Selector->IsClosed())Selector->Close();} bool USkiP1Widget::IsP1Ready()const{return WidgetTree&&WidgetTree->RootWidget&&SelectorPanel&&ProgressText&&ProgressBar&&StatusText;}
bool USkiP1Widget::IsSelectorClosed()const{return !Selector||Selector->IsClosed();}
int32 USkiP1Widget::GetBlockedSelectorNavigationCount()const{return Selector?Selector->GetBlockedNavigationCount():0;}
int32 USkiP1Widget::GetBlockedSelectorPopupCount()const{return Selector?Selector->GetBlockedPopupCount():0;}
bool USkiP1Widget::WasSelectorPopupDelegateProbeDenied()const{return Selector&&Selector->WasPopupDelegateProbeDenied();}
bool USkiP1Widget::IsPointerOverStatusPanel()const{return StatusPanel&&FSlateApplication::IsInitialized()&&StatusPanel->GetCachedGeometry().IsUnderLocation(FSlateApplication::Get().GetCursorPos());}
bool USkiP1Widget::DoesUiOwnKeyboardInput()const{APlayerController* Owner=GetOwningPlayer();return (Selector&&Selector->HasAnyUserFocus())||(StatusPanel&&(StatusPanel->HasAnyUserFocus()||(Owner&&StatusPanel->HasUserFocusedDescendants(Owner))));}
double USkiP1Widget::GetRightPanelInsetPixels()const{return StatusPanel?StatusPanel->GetCachedGeometry().GetAbsoluteSize().X+24.0:0.0;}
FVector2D USkiP1Widget::GetStatusPanelCenterAbsolute()const{if(!StatusPanel)return FVector2D::ZeroVector;const FGeometry Geometry=StatusPanel->GetCachedGeometry();return Geometry.GetAbsolutePosition()+Geometry.GetAbsoluteSize()*0.5;}
FVector2D USkiP1Widget::GetUnobstructedCenterAbsolute()const{const FGeometry Root=GetCachedGeometry();const FVector2D Position=Root.GetAbsolutePosition();const FVector2D Size=Root.GetAbsoluteSize();const double Available=FMath::Max(1.0,Size.X-GetRightPanelInsetPixels());return Position+FVector2D(Available*0.5,Size.Y*0.5);}
void USkiP1Widget::FocusRecoveryAction(){if(!FSlateApplication::IsInitialized())return;UButton* Target=RetryButton&&RetryButton->GetVisibility()==ESlateVisibility::Visible?RetryButton:ChangeSelectionButton;if(Target&&Target->GetVisibility()==ESlateVisibility::Visible)FSlateApplication::Get().SetUserFocus(0,Target->TakeWidget(),EFocusCause::SetDirectly);}
namespace
{
FVector4 GeometryRect(const UWidget* Widget)
{
    if (!Widget) return FVector4(0, 0, 0, 0);
    const FGeometry Geometry = Widget->GetCachedGeometry();
    const FVector2D Position = Geometry.GetAbsolutePosition();
    const FVector2D Size = Geometry.GetAbsoluteSize();
    return FVector4(Position.X, Position.Y, Size.X, Size.Y);
}
}
FVector4 USkiP1Widget::GetStatusPanelRectAbsolute()const{return GeometryRect(StatusPanel);}
FVector4 USkiP1Widget::GetSelectorPanelRectAbsolute()const{return GeometryRect(SelectorPanel);}
FVector4 USkiP1Widget::GetStatusScrollRectAbsolute()const{return GeometryRect(StatusScroll);}
FVector4 USkiP1Widget::GetRetryRectAbsolute()const{return GeometryRect(RetryButton);}
FVector4 USkiP1Widget::GetChangeSelectionRectAbsolute()const{return GeometryRect(ChangeSelectionButton);}
bool USkiP1Widget::ValidateShellLayout(const FIntPoint ViewportSize,const EP1ShellState ExpectedState,FString& OutError)const
{
    if(ShellState!=ExpectedState||!SelectorPanel||!StatusPanel||!StatusScroll||!RetryButton||!ChangeSelectionButton||!ReadyControls){OutError=TEXT("Required shell widgets or state are missing.");return false;}
    const FGeometry RootGeometry=GetCachedGeometry();const FVector2D RootPosition=RootGeometry.GetAbsolutePosition();const FVector2D RootSize=RootGeometry.GetAbsoluteSize();
    const auto InsideRoot=[&](const UWidget* Widget){const FVector4 R=GeometryRect(Widget);return R.Z>0&&R.W>0&&R.X>=RootPosition.X-1&&R.Y>=RootPosition.Y-1&&R.X+R.Z<=RootPosition.X+RootSize.X+1&&R.Y+R.W<=RootPosition.Y+RootSize.Y+1;};
    const bool Selecting=ExpectedState==EP1ShellState::Selecting;
    const bool RetryVisible=RetryButton->GetVisibility()==ESlateVisibility::Visible;
    const bool ChangeVisible=ChangeSelectionButton->GetVisibility()==ESlateVisibility::Visible;
    const bool ReadyVisible=ReadyControls->GetVisibility()==ESlateVisibility::Visible;
    const bool VisibilityValid=(SelectorPanel->GetVisibility()==(Selecting?ESlateVisibility::Visible:ESlateVisibility::Collapsed))
        &&(StatusPanel->GetVisibility()==(Selecting?ESlateVisibility::Collapsed:ESlateVisibility::Visible))
        &&(RetryVisible==(ExpectedState==EP1ShellState::Failed))
        &&(ChangeVisible==(ExpectedState==EP1ShellState::Preparing||ExpectedState==EP1ShellState::Failed||ExpectedState==EP1ShellState::Ready))
        &&(ReadyVisible==(ExpectedState==EP1ShellState::Ready))
        &&(Selecting?HasLiveSelector():!HasLiveSelector());
    const bool GeometryOk=RootSize.X>0&&RootSize.Y>0&&ViewportSize.X>0&&ViewportSize.Y>0
        &&(Selecting?InsideRoot(SelectorPanel):(InsideRoot(StatusPanel)&&StatusScroll->GetCachedGeometry().GetAbsoluteSize().Y>0));
    if(!(VisibilityValid&&GeometryOk))OutError=FString::Printf(TEXT("state %d root %.0fx%.0f selector %d status %d retry %d change %d ready %d scroll %.0f"),static_cast<int32>(ExpectedState),RootSize.X,RootSize.Y,SelectorPanel->GetVisibility()==ESlateVisibility::Visible,StatusPanel->GetVisibility()==ESlateVisibility::Visible,RetryVisible,ChangeVisible,ReadyVisible,StatusScroll->GetCachedGeometry().GetAbsoluteSize().Y);
    return VisibilityValid&&GeometryOk;
}
bool USkiP1Widget::ValidateRecoveryLayout(const FIntPoint ViewportSize,FString& OutError)const
{
    if(!StatusPanel||!StatusScroll||!RetryButton||!ChangeSelectionButton||!StatusText){OutError=TEXT("Required status widgets are missing.");return false;}
    const FGeometry RootGeometry=GetCachedGeometry();const FVector2D RootPosition=RootGeometry.GetAbsolutePosition();const FVector2D RootSize=RootGeometry.GetAbsoluteSize();
    const FGeometry PanelGeometry=StatusPanel->GetCachedGeometry();const FVector2D PanelPosition=PanelGeometry.GetAbsolutePosition();const FVector2D PanelSize=PanelGeometry.GetAbsoluteSize();
    const auto ActionInside=[&](const UButton* Button){const FGeometry Geometry=Button->GetCachedGeometry();const FVector2D Position=Geometry.GetAbsolutePosition();const FVector2D Size=Geometry.GetAbsoluteSize();return Size.X>0&&Size.Y>0&&Position.X>=PanelPosition.X&&Position.Y>=PanelPosition.Y&&Position.X+Size.X<=PanelPosition.X+PanelSize.X+1&&Position.Y+Size.Y<=PanelPosition.Y+PanelSize.Y+1;};
    const bool RootValid=RootSize.X>0&&RootSize.Y>0&&ViewportSize.X>0&&ViewportSize.Y>0;
    const bool PanelInside=PanelPosition.X>=RootPosition.X-1&&PanelPosition.Y>=RootPosition.Y-1&&PanelPosition.X+PanelSize.X<=RootPosition.X+RootSize.X+1&&PanelPosition.Y+PanelSize.Y<=RootPosition.Y+RootSize.Y+1;
    const bool ActionsVisible=RetryButton->GetVisibility()==ESlateVisibility::Visible&&ChangeSelectionButton->GetVisibility()==ESlateVisibility::Visible;
    const bool Valid=RootValid&&PanelInside&&ActionsVisible&&ActionInside(RetryButton)&&ActionInside(ChangeSelectionButton)&&StatusScroll->GetCachedGeometry().GetAbsoluteSize().Y>0&&StatusText->GetParent()==StatusScroll->GetChildAt(0);
    if(!Valid)OutError=FString::Printf(TEXT("root %.0fx%.0f panel %.0fx%.0f retry %d change %d scroll %.0f"),RootSize.X,RootSize.Y,PanelSize.X,PanelSize.Y,ActionInside(RetryButton),ActionInside(ChangeSelectionButton),StatusScroll->GetCachedGeometry().GetAbsoluteSize().Y);
    return Valid;
}
void USkiP1Widget::SetShellState(const EP1ShellState State){ShellState=State;const bool Selecting=State==EP1ShellState::Selecting;if(!Selecting)CloseSelector();const bool Ready=State==EP1ShellState::Ready;if(SelectorPanel)SelectorPanel->SetVisibility(Selecting?ESlateVisibility::Visible:ESlateVisibility::Collapsed);if(StatusPanel)StatusPanel->SetVisibility(Selecting?ESlateVisibility::Collapsed:ESlateVisibility::Visible);if(ReadyControls)ReadyControls->SetVisibility(Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);if(DetailsText)DetailsText->SetVisibility(Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);if(ProbeText)ProbeText->SetVisibility(Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);if(NodeText)NodeText->SetVisibility(Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);if(RetryButton&&State!=EP1ShellState::Failed)RetryButton->SetVisibility(ESlateVisibility::Collapsed);if(ChangeSelectionButton)ChangeSelectionButton->SetVisibility(State==EP1ShellState::Preparing||State==EP1ShellState::Failed||State==EP1ShellState::Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);}
void USkiP1Widget::RetryClicked(){if(RetryButton)RetryButton->SetIsEnabled(false);SetShellState(EP1ShellState::Preparing);if(RetryHandler)RetryHandler();} void USkiP1Widget::ChangeSelectionClicked(){if(ChangeSelectionHandler)ChangeSelectionHandler();}
void USkiP1Widget::OpenInstalledClicked()
{
    if (bOpenInstalledPending || !OpenInstalledHandler) return;
    bOpenInstalledPending = true;
    OpenInstalledCloseStartedSeconds = FPlatformTime::Seconds();
    if (OpenInstalledButton) OpenInstalledButton->SetIsEnabled(false);
    SetSelectorStatus(TEXT("Closing the map before opening installed terrain..."));
    CloseSelector();
}
void USkiP1Widget::PresentationClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Presentation"));} void USkiP1Widget::ElevationClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Elevation"));}
void USkiP1Widget::SlopeClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Slope"));} void USkiP1Widget::CoverClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Cover"));} void USkiP1Widget::LodClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod"));}
void USkiP1Widget::LodAutoClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("LodAuto"));}
void USkiP1Widget::Lod0Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod0"));} void USkiP1Widget::Lod1Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod1"));} void USkiP1Widget::Lod2Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod2"));}
void USkiP1Widget::Vertical1Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical1"));} void USkiP1Widget::Vertical2Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical2"));} void USkiP1Widget::Vertical4Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical4"));}
void USkiP1Widget::MiddayClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Midday"));} void USkiP1Widget::LowAngleClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("LowAngle"));} void USkiP1Widget::OvercastClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Overcast"));}
