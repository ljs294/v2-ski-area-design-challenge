#include "SkiP1Widget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "SkiDomain/PlaceCoordinates.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiSiteMap.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "Framework/Application/SlateApplication.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"

#include <string_view>

namespace
{
const FLinearColor PickerInk(0.10f, 0.16f, 0.22f, 1.0f);
const FLinearColor PickerMutedInk(0.31f, 0.39f, 0.46f, 1.0f);
const FLinearColor PickerAccent(0.04f, 0.39f, 0.55f, 1.0f);

void StylePickerText(UTextBlock* Text, const int32 Size, const FLinearColor Color,
    const bool bBold = false)
{
    if (!Text) return;
    Text->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), Size));
    Text->SetColorAndOpacity(FSlateColor(Color));
}

void StylePickerButton(UButton* Button, const bool bPrimary = false)
{
    if (!Button) return;
    const FLinearColor Base = bPrimary ? FLinearColor(0.035f, 0.37f, 0.53f, 1.0f)
        : FLinearColor(0.92f, 0.95f, 0.97f, 1.0f);
    const FLinearColor Hover = bPrimary ? FLinearColor(0.02f, 0.45f, 0.63f, 1.0f)
        : FLinearColor(0.86f, 0.92f, 0.96f, 1.0f);
    const FLinearColor Pressed = bPrimary ? FLinearColor(0.025f, 0.30f, 0.43f, 1.0f)
        : FLinearColor(0.81f, 0.88f, 0.93f, 1.0f);
    const FLinearColor Border = bPrimary ? FLinearColor(0.035f, 0.37f, 0.53f, 1.0f)
        : FLinearColor(0.75f, 0.81f, 0.86f, 1.0f);
    FButtonStyle Style;
    Style.SetNormal(FSlateRoundedBoxBrush(Base, 6.0f, Border, 1.0f));
    Style.SetHovered(FSlateRoundedBoxBrush(Hover, 6.0f, Border, 1.0f));
    Style.SetPressed(FSlateRoundedBoxBrush(Pressed, 6.0f, Border, 1.0f));
    Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(0.89f, 0.92f, 0.94f, 1.0f),
        6.0f, FLinearColor(0.83f, 0.87f, 0.90f, 1.0f), 1.0f));
    Style.SetNormalPadding(FMargin(12.0f, 7.0f));
    Style.SetPressedPadding(FMargin(12.0f, 7.0f));
    Button->SetStyle(Style);
    Button->SetColorAndOpacity(FLinearColor::White);
    if (UTextBlock* Label = Cast<UTextBlock>(Button->GetChildAt(0)))
        StylePickerText(Label, 14, bPrimary ? FLinearColor::White : PickerInk, true);
}

void StylePickerTextBox(UEditableTextBox* TextBox)
{
    if (!TextBox) return;
    FEditableTextBoxStyle Style = TextBox->GetWidgetStyle();
    Style.SetBackgroundImageNormal(FSlateRoundedBoxBrush(FLinearColor::White, 5.0f,
        FLinearColor(0.71f, 0.78f, 0.83f, 1.0f), 1.0f));
    Style.SetBackgroundImageHovered(FSlateRoundedBoxBrush(FLinearColor(0.99f, 0.995f, 1.0f, 1.0f),
        5.0f, FLinearColor(0.43f, 0.57f, 0.67f, 1.0f), 1.0f));
    Style.SetBackgroundImageFocused(FSlateRoundedBoxBrush(FLinearColor::White, 5.0f,
        FLinearColor(0.05f, 0.43f, 0.61f, 1.0f), 2.0f));
    Style.SetBackgroundImageReadOnly(FSlateRoundedBoxBrush(FLinearColor(0.95f, 0.96f, 0.97f, 1.0f),
        5.0f, FLinearColor(0.82f, 0.86f, 0.89f, 1.0f), 1.0f));
    Style.SetBackgroundColor(FSlateColor(FLinearColor::White));
    Style.SetForegroundColor(FSlateColor(PickerInk));
    Style.SetFocusedForegroundColor(FSlateColor(PickerInk));
    Style.SetPadding(FMargin(12.0f, 7.0f));
    Style.TextStyle.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 14));
    Style.TextStyle.SetColorAndOpacity(FSlateColor(PickerInk));
    TextBox->SetWidgetStyle(Style);
    TextBox->SetForegroundColor(PickerInk);
}
}

double USkiP1Widget::CalculateStatusPanelWidth(const double ViewportWidth) noexcept
{
    return FMath::Clamp(ViewportWidth - 48.0, 360.0, 520.0);
}

FVector2D USkiP1Widget::CalculateSelectorPanelSize(const FIntPoint ViewportSize) noexcept
{
    return {FMath::Max(280.0, FMath::Min(640.0, ViewportSize.X - 48.0)),
        FMath::Max(320.0, FMath::Min(600.0, ViewportSize.Y - 48.0))};
}

FVector2D USkiP1Widget::CalculateSitePickerPanelSize(const FIntPoint ViewportSize) noexcept
{
    const double AvailableWidth = FMath::Max(1.0, ViewportSize.X - 48.0);
    const double AvailableHeight = FMath::Max(1.0, ViewportSize.Y - 48.0);
    const double Width = AvailableWidth <= 340.0 ? AvailableWidth
        : FMath::Clamp(AvailableWidth, 340.0, 440.0);
    const double Height = AvailableHeight <= 360.0 ? AvailableHeight
        : FMath::Clamp(AvailableHeight, 360.0, 740.0);
    return {Width, Height};
}

void USkiP1Widget::NativeTick(const FGeometry& MyGeometry, const float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);
    if (StatusPanel)
    {
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(StatusPanel->Slot))
            CanvasSlot->SetOffsets(FMargin(-24, 24, CalculateStatusPanelWidth(MyGeometry.GetLocalSize().X), 24));
    }
    if (SelectorPanel)
    {
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(SelectorPanel->Slot))
        {
            const FIntPoint ViewportSize(FMath::RoundToInt(MyGeometry.GetLocalSize().X),
                FMath::RoundToInt(MyGeometry.GetLocalSize().Y));
            if (bSitePickerOpen)
            {
                const FVector2D LocalSize = MyGeometry.GetLocalSize();
                const FVector2D AbsoluteSize = MyGeometry.GetAbsoluteSize();
                const FVector2D SlateScale(
                    LocalSize.X > 0.0 ? AbsoluteSize.X / LocalSize.X : 1.0,
                    LocalSize.Y > 0.0 ? AbsoluteSize.Y / LocalSize.Y : 1.0);
                const FIntPoint ScreenSize(FMath::RoundToInt(AbsoluteSize.X),
                    FMath::RoundToInt(AbsoluteSize.Y));
                FVector2D PickerSizePixels = CalculateSitePickerPanelSize(ScreenSize);
                // Picker content uses the viewport's logical layout scale. Match the card
                // to that scale so Step 3 does not get clipped on high-DPI viewports.
                PickerSizePixels.X = FMath::Min(PickerSizePixels.X * FMath::Max(1.0, SlateScale.X),
                    FMath::Max(1.0, ScreenSize.X - 48.0));
                PickerSizePixels.Y = FMath::Min(PickerSizePixels.Y * FMath::Max(1.0, SlateScale.Y),
                    FMath::Max(1.0, ScreenSize.Y - 48.0));
                const FVector2D PickerPositionLocal = FVector2D(14.0, 14.0) / SlateScale;
                const FVector2D PickerSizeLocal = PickerSizePixels / SlateScale;
                CanvasSlot->SetPosition(PickerPositionLocal);
                CanvasSlot->SetSize(PickerSizeLocal);
                if (PickerCardBackdrop)
                {
                    if (UCanvasPanelSlot* BackdropSlot = Cast<UCanvasPanelSlot>(PickerCardBackdrop->Slot))
                    {
                        BackdropSlot->SetPosition(PickerPositionLocal);
                        BackdropSlot->SetSize(PickerSizeLocal);
                    }
                }
            }
            else
            {
                CanvasSlot->SetPosition(FVector2D(24.0, 24.0));
                CanvasSlot->SetSize(CalculateSelectorPanelSize(ViewportSize));
            }
        }
    }
}

void USkiP1Widget::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    if (!WidgetTree || WidgetTree->RootWidget) return;
    UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("P1Shell"));
    WidgetTree->RootWidget = Root;
    SiteMapWidget = WidgetTree->ConstructWidget<USkiSiteMapWidget>(USkiSiteMapWidget::StaticClass(), TEXT("SiteMap"));
    if (UCanvasPanelSlot* MapSlot = Root->AddChildToCanvas(SiteMapWidget))
    {
        MapSlot->SetAnchors(FAnchors(0, 0, 1, 1));
        MapSlot->SetAlignment(FVector2D::ZeroVector);
        MapSlot->SetOffsets(FMargin(0));
        MapSlot->SetZOrder(-10);
    }
    SiteMapWidget->SetVisibility(ESlateVisibility::Collapsed);
    PickerCardBackdrop = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PickerCardBackdrop"));
    PickerCardBackdrop->SetBrush(FSlateRoundedBoxBrush(FLinearColor(0.99f, 0.995f, 1.0f, 1.0f),
        12.0f, FLinearColor(0.82f, 0.87f, 0.91f, 1.0f), 1.0f));
    PickerCardBackdrop->SetPadding(FMargin(0.0f));
    if (UCanvasPanelSlot* BackdropSlot = Root->AddChildToCanvas(PickerCardBackdrop))
    {
        BackdropSlot->SetPosition(FVector2D(24.0, 24.0));
        BackdropSlot->SetSize(FVector2D(440.0, 740.0));
        BackdropSlot->SetZOrder(-1);
    }
    PickerCardBackdrop->SetVisibility(ESlateVisibility::Collapsed);
    SelectorPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SelectorPanel"));
    if (UCanvasPanelSlot* CanvasSlot = Root->AddChildToCanvas(SelectorPanel)) { CanvasSlot->SetPosition({24,24}); CanvasSlot->SetSize({640,600}); }
    TitleContents = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("NativeTitle"));
    if (UVerticalBoxSlot* TitleSlot = SelectorPanel->AddChildToVerticalBox(TitleContents))
    {
        TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        TitleSlot->SetPadding(FMargin(18.0f));
    }
    UTextBlock* Heading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("P1Heading"));
    Heading->SetText(FText::FromString(TEXT("SKI AREA DESIGN CHALLENGE\nMountain Planner")));
    Heading->SetAutoWrapText(true);
    TitleContents->AddChildToVerticalBox(Heading);
    AddCommandButton(TitleContents, TEXT("New Resort"), TEXT("NewResort"));
    UTextBlock* LibraryHeading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("LibraryHeading"));
    LibraryHeading->SetText(FText::FromString(TEXT("Installed resorts")));
    TitleContents->AddChildToVerticalBox(LibraryHeading);
    UScrollBox* LibraryScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("InstalledResortScroll"));
    if (UVerticalBoxSlot* LibrarySlot = TitleContents->AddChildToVerticalBox(LibraryScroll))
        LibrarySlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    InstalledResortList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InstalledResortList"));
    LibraryScroll->AddChild(InstalledResortList);
    OpenInstalledButton = AddCommandButton(TitleContents, TEXT("Open last installed terrain (offline)"), TEXT("OpenInstalled"));
    ResumeDownloadButton = AddCommandButton(TitleContents, TEXT("Resume download"), TEXT("ResumeDownload"));
    ResumeDownloadButton->SetIsEnabled(false);
    SelectorStatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SelectorStatus"));
    SelectorStatusText->SetAutoWrapText(true);
    TitleContents->AddChildToVerticalBox(SelectorStatusText);
    PickerContents = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("NativeSitePicker"));
    if (UVerticalBoxSlot* PickerContentsSlot = SelectorPanel->AddChildToVerticalBox(PickerContents))
    {
        PickerContentsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        PickerContentsSlot->SetPadding(FMargin(18.0f));
    }
    UTextBlock* PickerEyebrow = WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("PickerEyebrow"));
    PickerEyebrow->SetText(FText::FromString(TEXT("SKI AREA DESIGN CHALLENGE")));
    StylePickerText(PickerEyebrow, 11, PickerAccent, true);
    PickerContents->AddChildToVerticalBox(PickerEyebrow);
    UTextBlock* PickerHeading = WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(), TEXT("PickerHeading"));
    PickerHeading->SetText(FText::FromString(TEXT("New resort")));
    StylePickerText(PickerHeading, 28, PickerInk, true);
    PickerContents->AddChildToVerticalBox(PickerHeading);
    UTextBlock* PickerSubtitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PickerSubtitle"));
    PickerSubtitle->SetText(FText::FromString(TEXT("Find the mountain you want to make your own.")));
    PickerSubtitle->SetAutoWrapText(true);
    StylePickerText(PickerSubtitle, 14, PickerMutedInk);
    PickerContents->AddChildToVerticalBox(PickerSubtitle);
    PickerSteps = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PickerSteps"));
    const TCHAR* StepNames[] = {TEXT("Choose location"), TEXT("Define boundary"), TEXT("Name resort"), TEXT("Download")};
    for (int32 StepIndex = 0; StepIndex < UE_ARRAY_COUNT(StepNames); ++StepIndex)
    {
        UTextBlock* StepLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
        StepLabel->SetText(FText::FromString(FString::Printf(TEXT("○  %d  %s"), StepIndex + 1, StepNames[StepIndex])));
        StylePickerText(StepLabel, 12, PickerMutedInk);
        PickerSteps->AddChildToVerticalBox(StepLabel);
        PickerStepLabels.Add(StepLabel);
    }
    PickerContents->AddChildToVerticalBox(PickerSteps);
    UScrollBox* PickerScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("PickerScroll"));
    if (UVerticalBoxSlot* PickerScrollSlot = PickerContents->AddChildToVerticalBox(PickerScroll))
        PickerScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    UVerticalBox* PickerForm = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PickerForm"));
    PickerScroll->AddChild(PickerForm);
    LocationControls = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("LocationControls"));
    PickerForm->AddChildToVerticalBox(LocationControls);
    UTextBlock* LocationHeading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("LocationHeading"));
    LocationHeading->SetText(FText::FromString(TEXT("Search a place or enter lat, lon / DMS")));
    LocationHeading->SetAutoWrapText(true);
    StylePickerText(LocationHeading, 16, PickerInk, true);
    LocationControls->AddChildToVerticalBox(LocationHeading);
    LocationSearchBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("LocationSearchBox"));
    LocationSearchBox->SetHintText(FText::FromString(TEXT("47.6062, -122.3321 or 47°36'22\"N, 122°20'08\"W")));
    LocationSearchBox->OnTextCommitted.AddDynamic(this, &USkiP1Widget::CoordinateCommitted);
    StylePickerTextBox(LocationSearchBox);
    LocationControls->AddChildToVerticalBox(LocationSearchBox);
    UButton* SearchButton = AddCommandButton(LocationControls,
        TEXT("Search / go to coordinates"), TEXT("SearchLocation"));
    StylePickerButton(SearchButton);
    PlaceSearchResults = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("PlaceSearchResults"));
    LocationControls->AddChildToVerticalBox(PlaceSearchResults);
    PickerSearchStatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PickerSearchStatus"));
    PickerSearchStatusText->SetText(FText::FromString(TEXT("Showing USGS imagery. Right or middle drag to pan; scroll to zoom.")));
    PickerSearchStatusText->SetAutoWrapText(true);
    StylePickerText(PickerSearchStatusText, 12, PickerMutedInk);
    LocationControls->AddChildToVerticalBox(PickerSearchStatusText);
    SelectSiteButton = AddCommandButton(LocationControls, TEXT("Select site"), TEXT("SelectSite"));
    StylePickerButton(SelectSiteButton, true);
    SelectSiteButton->SetIsEnabled(false);
    UUniformGridPanel* ContourControls = WidgetTree->ConstructWidget<UUniformGridPanel>(
        UUniformGridPanel::StaticClass(), TEXT("ContourControls"));
    PickerForm->AddChildToVerticalBox(ContourControls);
    ToggleContoursButton = AddGridCommandButton(ContourControls, TEXT("Contours: on"),
        TEXT("ToggleContours"), 0, 0);
    ContourUnitsButton = AddGridCommandButton(ContourControls, TEXT("Labels: ft"),
        TEXT("ToggleContourUnits"), 0, 1);
    StylePickerButton(ToggleContoursButton);
    StylePickerButton(ContourUnitsButton);
    BoundaryControls = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("BoundaryControls"));
    PickerForm->AddChildToVerticalBox(BoundaryControls);
    UTextBlock* BoundaryHeading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BoundaryHeading"));
    BoundaryHeading->SetText(FText::FromString(TEXT("Define your boundary")));
    StylePickerText(BoundaryHeading, 16, PickerInk, true);
    BoundaryControls->AddChildToVerticalBox(BoundaryHeading);
    UTextBlock* BoundaryInstructions = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BoundaryInstructions"));
    BoundaryInstructions->SetText(FText::FromString(TEXT("Drag on the map to draw a 2–4 km site. Drag inside to move it or use the handles to resize.")));
    BoundaryInstructions->SetAutoWrapText(true);
    StylePickerText(BoundaryInstructions, 12, PickerMutedInk);
    BoundaryControls->AddChildToVerticalBox(BoundaryInstructions);
    UButton* ClearBoundary = AddCommandButton(BoundaryControls, TEXT("Clear boundary"), TEXT("ClearBoundary"));
    StylePickerButton(ClearBoundary);
    ClearBoundaryButton = Cast<UButton>(WidgetTree->FindWidget(TEXT("ClearBoundary")));
    PickerBoundaryStatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PickerBoundaryStatus"));
    PickerBoundaryStatusText->SetText(FText::FromString(TEXT("Drag on the map to draw the site boundary.")));
    PickerBoundaryStatusText->SetAutoWrapText(true);
    StylePickerText(PickerBoundaryStatusText, 13, PickerInk, true);
    BoundaryControls->AddChildToVerticalBox(PickerBoundaryStatusText);
    PickerPreviewStatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PickerPreviewStatus"));
    PickerPreviewStatusText->SetText(FText::FromString(TEXT("M3 will add coverage and source-quality details.")));
    PickerPreviewStatusText->SetAutoWrapText(true);
    StylePickerText(PickerPreviewStatusText, 12, PickerMutedInk);
    BoundaryControls->AddChildToVerticalBox(PickerPreviewStatusText);
    ChangeLocationButton = AddCommandButton(BoundaryControls, TEXT("Change location"), TEXT("ChangeLocation"));
    StylePickerButton(ChangeLocationButton);
    BoundaryControls->SetVisibility(ESlateVisibility::Collapsed);
    ResortNameControls = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ResortNameControls"));
    PickerForm->AddChildToVerticalBox(ResortNameControls);
    UTextBlock* ResortNameHeading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ResortNameHeading"));
    ResortNameHeading->SetText(FText::FromString(TEXT("Name your resort")));
    StylePickerText(ResortNameHeading, 16, PickerInk, true);
    ResortNameControls->AddChildToVerticalBox(ResortNameHeading);
    ResortNameBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), TEXT("ResortNameBox"));
    ResortNameBox->SetHintText(FText::FromString(TEXT("Name your resort")));
    StylePickerTextBox(ResortNameBox);
    ResortNameControls->AddChildToVerticalBox(ResortNameBox);
    PickerDownloadButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("PickerDownload"));
    UTextBlock* DownloadLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PickerDownloadLabel"));
    DownloadLabel->SetText(FText::FromString(TEXT("Download unavailable until M5")));
    StylePickerText(DownloadLabel, 13, FLinearColor(0.40f, 0.47f, 0.52f, 1.0f), true);
    PickerDownloadButton->AddChild(DownloadLabel);
    StylePickerButton(PickerDownloadButton);
    PickerDownloadButton->SetIsEnabled(false);
    ResortNameControls->AddChildToVerticalBox(PickerDownloadButton);
    ResortNameControls->SetVisibility(ESlateVisibility::Collapsed);
    UButton* BackToTitleButton = AddCommandButton(PickerContents, TEXT("Back to title"), TEXT("BackToTitle"));
    StylePickerButton(BackToTitleButton);
    PickerContents->SetVisibility(ESlateVisibility::Collapsed);
    const TWeakObjectPtr<USkiP1Widget> WeakThis(this);
    SiteMapWidget->SetPreviewStatusHandler([WeakThis](const FString& Status)
    {
        if (WeakThis.IsValid() && WeakThis->PickerPreviewStatusText)
            WeakThis->PickerPreviewStatusText->SetText(FText::FromString(Status));
    });
    SiteMapWidget->SetSelectionStatusHandler([WeakThis](const FString& Status)
    {
        if (WeakThis.IsValid() && WeakThis->PickerBoundaryStatusText)
            WeakThis->PickerBoundaryStatusText->SetText(FText::FromString(Status));
        if (WeakThis.IsValid()) WeakThis->UpdatePickerSteps();
    });
    SiteMapWidget->SetCenter(47.25, -121.55);
    SiteMapWidget->SetContourUnits(bMetricContourUnits);
    RebuildInstalledResorts();
    UpdatePickerSteps();
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
    StatusText->SetAutoWrapText(true); StatusText->SetText(FText::FromString(TEXT("Terrain preparation status."))); ScrollContent->AddChildToVerticalBox(StatusText);
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
    PhotoButton = AddGridCommandButton(Grid, TEXT("Photo"), TEXT("Photo"), 8, 0);
    SetPhotoCommandAvailable(false);
    UTextBlock* Controls = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Controls")); Controls->SetAutoWrapText(true);
    Controls->SetText(FText::FromString(TEXT("RMB orbit • MMB pan • wheel zoom • F/Home frame all • 1 full • 2 close • 3 last probe • Esc release"))); ReadyControls->AddChildToVerticalBox(Controls);
    SetShellState(EP1ShellState::Selecting);
}

void USkiP1Widget::ConfigureSelector()
{
    bSitePickerOpen = false;
    bHasChosenLocation = false;
    bBoundaryStepActive = false;
    if (PlaceSearchCancellation.IsValid()) PlaceSearchCancellation->Cancel();
    PlaceSearchCancellation.Reset();
    ++PlaceSearchGeneration;
    if (SiteMapWidget)
    {
        SiteMapWidget->SetPickerActive(false);
        SiteMapWidget->SetVisibility(ESlateVisibility::Collapsed);
    }
    if (PickerCardBackdrop) PickerCardBackdrop->SetVisibility(ESlateVisibility::Collapsed);
    if (TitleContents) TitleContents->SetVisibility(ESlateVisibility::Visible);
    if (PickerContents) PickerContents->SetVisibility(ESlateVisibility::Collapsed);
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
    else if (Name==TEXT("NewResort")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::NewResortClicked);
    else if (Name==TEXT("BackToTitle")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::BackToTitleClicked);
    else if (Name==TEXT("ResumeDownload")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ResumeDownloadClicked);
    else if (Name==TEXT("SearchLocation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::SearchLocationClicked);
    else if (Name==TEXT("SelectSite")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::SelectSiteClicked);
    else if (Name==TEXT("ChangeLocation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ChangeLocationClicked);
    else if (Name==TEXT("ToggleContours")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ToggleContoursClicked);
    else if (Name==TEXT("ToggleContourUnits")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ToggleContourUnitsClicked);
    else if (Name==TEXT("ClearBoundary")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ClearBoundaryClicked);
    else if (Name==TEXT("Presentation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::PresentationClicked); else if (Name==TEXT("Elevation")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::ElevationClicked);
    else if (Name==TEXT("Photo")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::PhotoClicked);
    else if (Name==TEXT("Slope")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::SlopeClicked); else if (Name==TEXT("Cover")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::CoverClicked);
    else if (Name==TEXT("Lod")) Button->OnClicked.AddDynamic(this,&USkiP1Widget::LodClicked); else if(Name==TEXT("LodAuto"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::LodAutoClicked); else if(Name==TEXT("Lod0"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod0Clicked); else if(Name==TEXT("Lod1"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod1Clicked); else if(Name==TEXT("Lod2"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Lod2Clicked);
    else if(Name==TEXT("Vertical1"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical1Clicked); else if(Name==TEXT("Vertical2"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical2Clicked); else if(Name==TEXT("Vertical4"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::Vertical4Clicked);
    else if(Name==TEXT("Midday"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::MiddayClicked); else if(Name==TEXT("LowAngle"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::LowAngleClicked); else if(Name==TEXT("Overcast"))Button->OnClicked.AddDynamic(this,&USkiP1Widget::OvercastClicked);
}

void USkiInstalledResortAction::Clicked()
{
    if (Owner.IsValid()) Owner->OpenInstalledById(ContentId);
}

void USkiP1Widget::RebuildInstalledResorts()
{
    if (!InstalledResortList || !WidgetTree) return;
    InstalledResortList->ClearChildren();
    InstalledActions.Empty();
    if (InstalledResorts.IsEmpty())
    {
        UTextBlock* Empty = WidgetTree->ConstructWidget<UTextBlock>();
        Empty->SetText(FText::FromString(TEXT("No installed resorts yet.")));
        InstalledResortList->AddChildToVerticalBox(Empty);
        return;
    }
    for (const FSkiInstalledResortItem& Item : InstalledResorts)
    {
        if (Item.ContentId.IsEmpty()) continue;
        UButton* Button = WidgetTree->ConstructWidget<UButton>();
        UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
        const FString Name = Item.DisplayName.IsEmpty() ? Item.ContentId.Left(12) : Item.DisplayName;
        Label->SetText(FText::FromString(Name + (Item.Detail.IsEmpty() ? FString() : TEXT("\n") + Item.Detail)));
        Label->SetAutoWrapText(true);
        Button->AddChild(Label);
        InstalledResortList->AddChildToVerticalBox(Button);
        USkiInstalledResortAction* Action = NewObject<USkiInstalledResortAction>(this);
        Action->Owner = this;
        Action->ContentId = Item.ContentId;
        Button->OnClicked.AddDynamic(Action, &USkiInstalledResortAction::Clicked);
        InstalledActions.Add(Action);
    }
}

void USkiP1Widget::SetInstalledResorts(const TArray<FSkiInstalledResortItem>& Items)
{
    InstalledResorts = Items;
    RebuildInstalledResorts();
}

void USkiP1Widget::SetOpenInstalledByIdHandler(TFunction<void(const FString&)> Handler)
{
    OpenInstalledByIdHandler = MoveTemp(Handler);
}

void USkiP1Widget::SetNavigationHandler(TFunction<void()> Handler)
{
    NavigationHandler = MoveTemp(Handler);
}

void USkiP1Widget::SetResumeDownloadHandler(TFunction<void()> Handler)
{
    ResumeDownloadHandler = MoveTemp(Handler);
    if (ResumeDownloadButton) ResumeDownloadButton->SetIsEnabled(static_cast<bool>(ResumeDownloadHandler));
}

void USkiP1Widget::SetPlaceSearchHandler(FSkiPlaceSearchHandler Handler)
{
    PlaceSearchHandler = MoveTemp(Handler);
}

void USkiP1Widget::OpenInstalledById(const FString& ContentId)
{
    if (OpenInstalledByIdHandler && !ContentId.IsEmpty()) OpenInstalledByIdHandler(ContentId);
}

bool USkiP1Widget::IsNativeTitleReady() const
{
    return ShellState == EP1ShellState::Selecting && !bSitePickerOpen
        && TitleContents && TitleContents->GetVisibility() == ESlateVisibility::Visible
        && InstalledResortList && PickerContents;
}

bool USkiP1Widget::IsNativePickerPlaceholder() const
{
    return IsNativeSitePickerReady();
}

bool USkiP1Widget::IsNativeSitePickerReady() const
{
    return ShellState == EP1ShellState::Selecting && bSitePickerOpen
        && PickerContents && PickerContents->GetVisibility() == ESlateVisibility::Visible
        && SiteMapWidget && SiteMapWidget->GetVisibility() == ESlateVisibility::Visible
        && PickerCardBackdrop && PickerCardBackdrop->GetVisibility() == ESlateVisibility::HitTestInvisible
        && LocationSearchBox && PickerBoundaryStatusText
        && PickerPreviewStatusText && ContourUnitsButton && PickerDownloadButton
        && !PickerDownloadButton->GetIsEnabled();
}

bool USkiP1Widget::RunNativeFrontEndSmoke(const FString& ExpectedContentId,
    FString& OutError, const bool bInvokeProductionOpen)
{
    OutError.Empty();
    if (ExpectedContentId.IsEmpty() || InstalledResorts.Num() != 1
        || InstalledResorts[0].ContentId != ExpectedContentId)
    { OutError = TEXT("Verified installed resort is missing from the library."); return false; }
    if (!IsNativeTitleReady()) { OutError = TEXT("Native title is not ready."); return false; }
    TFunction<void()> PreviousNavigationHandler = MoveTemp(NavigationHandler);
    int32 NavigationCount = 0;
    SetNavigationHandler([&NavigationCount] { ++NavigationCount; });
    UButton* NewResortButton = WidgetTree
        ? Cast<UButton>(WidgetTree->FindWidget(TEXT("NewResort"))) : nullptr;
    UButton* BackButton = WidgetTree
        ? Cast<UButton>(WidgetTree->FindWidget(TEXT("BackToTitle"))) : nullptr;
    if (NewResortButton) NewResortButton->OnClicked.Broadcast();
    const bool bPickerOpened = IsNativeSitePickerReady();
    if (BackButton) BackButton->OnClicked.Broadcast();
    const bool bTitleRestored = IsNativeTitleReady();
    const bool bNavigationSignalled = NavigationCount == 2;
    SetNavigationHandler(MoveTemp(PreviousNavigationHandler));
    TFunction<void(const FString&)> PreviousHandler = MoveTemp(OpenInstalledByIdHandler);
    FString OpenedId;
    SetOpenInstalledByIdHandler([&OpenedId](const FString& ContentId) { OpenedId = ContentId; });
    UButton* InstalledButton = InstalledResortList && InstalledResortList->GetChildrenCount() == 1
        ? Cast<UButton>(InstalledResortList->GetChildAt(0)) : nullptr;
    if (InstalledButton) InstalledButton->OnClicked.Broadcast();
    const bool bCorrectId = OpenedId == ExpectedContentId;
    SetOpenInstalledByIdHandler(MoveTemp(PreviousHandler));
    if (!bPickerOpened) OutError = TEXT("New Resort did not open the native site picker.");
    else if (!bTitleRestored) OutError = TEXT("Back did not restore the native title.");
    else if (!bNavigationSignalled) OutError = TEXT("Native navigation buttons did not notify the flow coordinator.");
    else if (!bCorrectId) OutError = TEXT("Installed resort action did not forward its ContentId.");
    if (OutError.IsEmpty() && bInvokeProductionOpen && InstalledButton)
        InstalledButton->OnClicked.Broadcast();
    return OutError.IsEmpty();
}

void USkiP1Widget::SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> H){SelectionHandler=std::move(H);} void USkiP1Widget::AcceptSelection(const SkiPreparation::Request& R){SetTransientStatus(TEXT("Selection accepted."));if(SelectionHandler)SelectionHandler(R);}
void USkiP1Widget::SetOpenInstalledHandler(TFunction<void()> H){OpenInstalledHandler=std::move(H);}
void USkiP1Widget::SetSelectorStatus(const FString& Status){if(SelectorStatusText)SelectorStatusText->SetText(FText::FromString(Status));}
void USkiP1Widget::SetPreparationProgress(const SkiPreparation::Progress& P){SetShellState(EP1ShellState::Preparing);if(ProgressText){FString Detail=P.Detail.Left(120);Detail.ReplaceInline(TEXT("\n"),TEXT(" "));ProgressText->SetText(FText::FromString(FString::Printf(TEXT("%s — %s"),SkiPreparation::StateName(P.Phase),*Detail)));}if(ProgressBar)ProgressBar->SetPercent(P.Total.IsSet()&&P.Total.GetValue()?static_cast<float>(P.Completed)/P.Total.GetValue():0);}
void USkiP1Widget::SetTransientStatus(const FString& S){if(StatusText)StatusText->SetText(FText::FromString(S));} void USkiP1Widget::SetProbeStatus(const FString& S){if(ProbeText)ProbeText->SetText(FText::FromString(S));}
void USkiP1Widget::SetNodeStatus(const FString& S){if(NodeText)NodeText->SetText(FText::FromString(S));}
void USkiP1Widget::SetTerrainDetails(const FString& D,const bool Synthetic){SetShellState(EP1ShellState::Ready);if(DetailsText)DetailsText->SetText(FText::FromString((Synthetic?TEXT("SYNTHETIC FIXTURE — not representative of selected terrain\n"):TEXT(""))+D));}
void USkiP1Widget::SetPhotoCommandAvailable(const bool bAvailable)
{
    bPhotoCommandAvailable = bAvailable;
    if (PhotoButton)
    {
        PhotoButton->SetVisibility(bAvailable ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
        PhotoButton->SetIsEnabled(bAvailable);
    }
}
bool USkiP1Widget::ShouldShowPhotoCommandForInstallation(const uint32 InstallationSchema,
    const bool bHasVerifiedSiteContext) noexcept
{
    return InstallationSchema == SkiPreparation::CompositeInstallReceiptSchema
        && bHasVerifiedSiteContext;
}
void USkiP1Widget::BeginPreparationUI(TFunction<void()> Change){SetPhotoCommandAvailable(false);ChangeSelectionHandler=std::move(Change);SetShellState(EP1ShellState::Preparing);if(ChangeSelectionButton)ChangeSelectionButton->SetVisibility(ESlateVisibility::Visible);}
void USkiP1Widget::ShowPreparationFailure(const TOptional<SkiPreparation::ProviderFailure>& Failure,const FString& Fallback,TFunction<void()> Retry,TFunction<void()> Change)
{RetryHandler=std::move(Retry);ChangeSelectionHandler=std::move(Change);SetShellState(EP1ShellState::Failed);FString M=Fallback;if(Failure.IsSet()){const auto& F=Failure.GetValue();const FString Receipt=F.DiagnosticReceipt.IsEmpty()?TEXT("not written"):FPaths::GetCleanFilename(F.DiagnosticReceipt);if(F.Stage==SkiPreparation::FailureStage::Acquisition){M=FString::Printf(TEXT("%s\n%s — %s (HTTP %d)\n%ux%u · tile %d/%d · attempt %d/%d · %.1f s\nDiagnostic: %s"),SkiPreparation::ProviderProductName(F.Product),F.TransportFailure.IsEmpty()?TEXT("Acquisition failed"):*F.TransportFailure,*F.RequestStatus,F.HttpStatus,F.RequestedWidth,F.RequestedHeight,F.TileIndex,F.TileCount,F.Attempt,F.MaximumAttempts,F.ElapsedSeconds,*Receipt);}else{const FString Metadata=F.MetadataTag?FString::Printf(TEXT("\nMetadata tag %u type %d count %d pass-count %s"),F.MetadataTag,F.MetadataType,F.MetadataReadCount,F.MetadataPassCount?TEXT("yes"):TEXT("no")):FString();M=FString::Printf(TEXT("%s\n%s — %s\n%s%s\nDiagnostic: %s"),*F.Code,SkiPreparation::ProviderProductName(F.Product),SkiPreparation::FailureStageName(F.Stage),F.Width?*FString::Printf(TEXT("%s %ux%u compression %u"),*F.Organization,F.Width,F.Height,F.Compression):TEXT("No decoded raster"),*Metadata,*Receipt);}}SetTransientStatus(M);RetryButton->SetVisibility(Failure.IsSet()&&Failure->Retry==SkiPreparation::RetryClassification::Retryable?ESlateVisibility::Visible:ESlateVisibility::Collapsed);RetryButton->SetIsEnabled(true);ChangeSelectionButton->SetVisibility(ESlateVisibility::Visible);}
void USkiP1Widget::OpenSelector(){SetPhotoCommandAvailable(false);SetShellState(EP1ShellState::Selecting);ConfigureSelector();}
void USkiP1Widget::NativeDestruct()
{
    if (PlaceSearchCancellation.IsValid()) PlaceSearchCancellation->Cancel();
    PlaceSearchCancellation.Reset();
    ++PlaceSearchGeneration;
    if (SiteMapWidget) SiteMapWidget->SetPickerActive(false);
    Super::NativeDestruct();
}
bool USkiP1Widget::HasLiveSelector()const{return false;}
bool USkiP1Widget::GetSelectorTeardownProof(bool& OutBridgeUnbound,bool& OutCefClosed,bool& OutReleased,double& OutCloseMs)const
{OutBridgeUnbound=true;OutCefClosed=true;OutReleased=true;OutCloseMs=0.0;return false;}
void USkiP1Widget::ResetSelector(){OpenSelector();SetSelectorStatus(TEXT("Back at the resort library."));}
void USkiP1Widget::SetViewCommandHandler(TFunction<void(FName)> H){ViewCommandHandler=std::move(H);}
void USkiP1Widget::CloseSelector(){}
bool USkiP1Widget::IsP1Ready()const{return WidgetTree&&WidgetTree->RootWidget&&SelectorPanel&&TitleContents&&PickerContents&&InstalledResortList&&SiteMapWidget&&LocationSearchBox&&PickerBoundaryStatusText&&PickerPreviewStatusText&&ContourUnitsButton&&PickerDownloadButton&&ProgressText&&ProgressBar&&StatusText;}
bool USkiP1Widget::IsSelectorClosed()const{return true;}
int32 USkiP1Widget::GetBlockedSelectorNavigationCount()const{return 0;}
int32 USkiP1Widget::GetBlockedSelectorPopupCount()const{return 0;}
bool USkiP1Widget::WasSelectorPopupDelegateProbeDenied()const{return false;}
bool USkiP1Widget::IsPointerOverStatusPanel()const{return StatusPanel&&FSlateApplication::IsInitialized()&&StatusPanel->GetCachedGeometry().IsUnderLocation(FSlateApplication::Get().GetCursorPos());}
bool USkiP1Widget::DoesUiOwnKeyboardInput()const{APlayerController* Owner=GetOwningPlayer();return (SelectorPanel&&(SelectorPanel->HasAnyUserFocus()||(Owner&&SelectorPanel->HasUserFocusedDescendants(Owner))))||(StatusPanel&&(StatusPanel->HasAnyUserFocus()||(Owner&&StatusPanel->HasUserFocusedDescendants(Owner))));}
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
        &&(!HasLiveSelector())
        &&(!Selecting || IsNativeTitleReady() || IsNativeSitePickerReady());
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
void USkiP1Widget::SetShellState(const EP1ShellState State)
{
    ShellState = State;
    const bool bSelecting = State == EP1ShellState::Selecting;
    const bool bReady = State == EP1ShellState::Ready;
    if (SelectorPanel) SelectorPanel->SetVisibility(bSelecting ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (StatusPanel) StatusPanel->SetVisibility(bSelecting ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
    if (ReadyControls) ReadyControls->SetVisibility(bReady ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (DetailsText) DetailsText->SetVisibility(bReady ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (ProbeText) ProbeText->SetVisibility(bReady ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (NodeText) NodeText->SetVisibility(bReady ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (RetryButton && State != EP1ShellState::Failed) RetryButton->SetVisibility(ESlateVisibility::Collapsed);
    if (ChangeSelectionButton)
        ChangeSelectionButton->SetVisibility(State == EP1ShellState::Preparing
            || State == EP1ShellState::Failed || State == EP1ShellState::Ready
            ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    if (!bSelecting && SiteMapWidget)
    {
        SiteMapWidget->SetPickerActive(false);
        SiteMapWidget->SetVisibility(ESlateVisibility::Collapsed);
        bSitePickerOpen = false;
    }
}
void USkiP1Widget::RetryClicked(){if(RetryButton)RetryButton->SetIsEnabled(false);SetShellState(EP1ShellState::Preparing);if(RetryHandler)RetryHandler();} void USkiP1Widget::ChangeSelectionClicked(){if(ChangeSelectionHandler)ChangeSelectionHandler();}
void USkiP1Widget::OpenInstalledClicked()
{
    if (OpenInstalledHandler) OpenInstalledHandler();
}
void USkiP1Widget::NewResortClicked()
{
    if (NavigationHandler) NavigationHandler();
    bSitePickerOpen = true;
    bHasChosenLocation = false;
    bBoundaryStepActive = false;
    if (ResortNameBox) ResortNameBox->SetText(FText::GetEmpty());
    if (TitleContents) TitleContents->SetVisibility(ESlateVisibility::Collapsed);
    if (PickerContents) PickerContents->SetVisibility(ESlateVisibility::Visible);
    if (PlaceSearchCancellation.IsValid()) PlaceSearchCancellation->Cancel();
    PlaceSearchCancellation.Reset();
    ++PlaceSearchGeneration;
    PlaceSearchResultsData.Reset();
    RebuildPlaceSearchResults();
    if (SiteMapWidget)
    {
        SiteMapWidget->ClearSelection();
        SiteMapWidget->SetVisibility(ESlateVisibility::Visible);
        SiteMapWidget->SetPickerActive(true);
    }
    if (PickerCardBackdrop) PickerCardBackdrop->SetVisibility(ESlateVisibility::HitTestInvisible);
    UpdatePickerSteps();
}
void USkiP1Widget::BackToTitleClicked()
{
    if (NavigationHandler) NavigationHandler();
    bHasChosenLocation = false;
    if (SiteMapWidget) SiteMapWidget->ClearSelection();
    ConfigureSelector();
    if (PlaceSearchResultsData.Num() > 0)
    {
        PlaceSearchResultsData.Reset();
        RebuildPlaceSearchResults();
    }
}
void USkiP1Widget::ResumeDownloadClicked(){if(ResumeDownloadHandler)ResumeDownloadHandler();}
void USkiP1Widget::CoordinateCommitted(const FText&, const ETextCommit::Type CommitMethod)
{
    if (CommitMethod == ETextCommit::OnEnter) RunLocationSearch();
}

void USkiP1Widget::SearchLocationClicked()
{
    RunLocationSearch();
}

void USkiP1Widget::SelectSiteClicked()
{
    if (!bSitePickerOpen || !bHasChosenLocation || !SiteMapWidget) return;
    bBoundaryStepActive = true;
    if (PickerBoundaryStatusText)
        PickerBoundaryStatusText->SetText(FText::FromString(TEXT("Drag on the map to draw the site boundary.")));
    UpdatePickerSteps();
    if (UScrollBox* PickerScroll = Cast<UScrollBox>(WidgetTree->FindWidget(TEXT("PickerScroll"))))
        PickerScroll->ScrollToStart();
}

void USkiP1Widget::ChangeLocationClicked()
{
    if (!bSitePickerOpen) return;
    bBoundaryStepActive = false;
    if (SiteMapWidget) SiteMapWidget->ClearSelection();
    if (PickerSearchStatusText)
        PickerSearchStatusText->SetText(FText::FromString(
            TEXT("Location centered. Select site to continue, or search another place.")));
    UpdatePickerSteps();
    if (UScrollBox* PickerScroll = Cast<UScrollBox>(WidgetTree->FindWidget(TEXT("PickerScroll"))))
        PickerScroll->ScrollToStart();
}

void USkiP1Widget::RunLocationSearch()
{
    if (!LocationSearchBox || !PickerSearchStatusText || !SiteMapWidget) return;
    bHasChosenLocation = false;
    bBoundaryStepActive = false;
    SiteMapWidget->ClearSelection();
    UpdatePickerSteps();
    if (PlaceSearchCancellation.IsValid()) PlaceSearchCancellation->Cancel();
    PlaceSearchCancellation.Reset();
    const int32 Generation = ++PlaceSearchGeneration;
    PlaceSearchResultsData.Reset();
    RebuildPlaceSearchResults();

    const FString Query = LocationSearchBox->GetText().ToString().TrimStartAndEnd();
    if (Query.IsEmpty())
    {
        PickerSearchStatusText->SetText(FText::FromString(TEXT("Enter a place name or coordinates.")));
        return;
    }

    FTCHARToUTF8 QueryUtf8(*Query);
    SkiDomain::PlaceCoordinates Coordinates;
    if (SkiDomain::TryParsePlaceCoordinates(
            std::string_view(QueryUtf8.Get(), static_cast<size_t>(QueryUtf8.Length())), Coordinates))
    {
        SkiDomain::WebMercatorPixel Pixel;
        if (!SkiDomain::TryWebMercatorPixel(Coordinates.LatitudeDeg,
                Coordinates.LongitudeDeg, 13, Pixel))
        {
            PickerSearchStatusText->SetText(FText::FromString(
                TEXT("These coordinates are outside the Web Mercator map coverage (about 85° N/S).")));
            return;
        }
        bHasChosenLocation = true;
        SiteMapWidget->SetCenter(Coordinates.LatitudeDeg, Coordinates.LongitudeDeg);
        UpdatePickerSteps();
        PickerSearchStatusText->SetText(FText::FromString(FString::Printf(
            TEXT("Centered at %.5f°, %.5f°. Select site to define its boundary."),
            Coordinates.LatitudeDeg, Coordinates.LongitudeDeg)));
        return;
    }

    if (!PlaceSearchHandler)
    {
        PickerSearchStatusText->SetText(FText::FromString(
            TEXT("Place search is unavailable. Coordinate entry still works offline.")));
        return;
    }

    PlaceSearchCancellation = MakeShared<SkiPreparation::Cancellation>();
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = PlaceSearchCancellation.ToSharedRef();
    PickerSearchStatusText->SetText(FText::FromString(TEXT("Searching for places…")));
    const TWeakObjectPtr<USkiP1Widget> WeakThis(this);
    PlaceSearchHandler(Query, Cancellation,
        [WeakThis, Generation](TArray<FSkiPlaceSearchResult> Results, FString Error) mutable
        {
            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, Generation, Results = MoveTemp(Results), Error = MoveTemp(Error)]() mutable
                {
                    if (WeakThis.IsValid())
                        WeakThis->CompletePlaceSearch(Generation, MoveTemp(Results), Error);
                });
        });
}

void USkiP1Widget::CompletePlaceSearch(const int32 Generation,
    TArray<FSkiPlaceSearchResult> Results, const FString& Error)
{
    if (Generation != PlaceSearchGeneration || !PickerSearchStatusText) return;
    PlaceSearchCancellation.Reset();
    for (int32 Index = Results.Num() - 1; Index >= 0; --Index)
    {
        SkiDomain::WebMercatorPixel Pixel;
        const FSkiPlaceSearchResult& Result = Results[Index];
        if (Result.Name.IsEmpty() || !SkiDomain::TryWebMercatorPixel(Result.LatitudeDeg,
                Result.LongitudeDeg, 8, Pixel)) Results.RemoveAt(Index);
    }
    PlaceSearchResultsData = MoveTemp(Results);
    RebuildPlaceSearchResults();
    if (!Error.IsEmpty()) PickerSearchStatusText->SetText(FText::FromString(Error));
    else if (PlaceSearchResultsData.IsEmpty())
        PickerSearchStatusText->SetText(FText::FromString(TEXT("No matching places were found.")));
    else PickerSearchStatusText->SetText(FText::FromString(TEXT("Choose a result to center the map.")));
}

void USkiP1Widget::RebuildPlaceSearchResults()
{
    if (!PlaceSearchResults || !WidgetTree) return;
    PlaceSearchResults->ClearChildren();
    PlaceSearchActions.Empty();
    for (int32 Index = 0; Index < PlaceSearchResultsData.Num(); ++Index)
    {
        const FSkiPlaceSearchResult& Result = PlaceSearchResultsData[Index];
        const FString LabelText = Result.Region.IsEmpty() ? Result.Name
            : Result.Name + TEXT(" · ") + Result.Region;
        UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
        UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>();
        Label->SetText(FText::FromString(LabelText));
        Label->SetAutoWrapText(true);
        Button->AddChild(Label);
        StylePickerButton(Button);
        PlaceSearchResults->AddChildToVerticalBox(Button);
        USkiPlaceSearchAction* Action = NewObject<USkiPlaceSearchAction>(this);
        Action->Owner = this;
        Action->SearchGeneration = PlaceSearchGeneration;
        Action->ResultIndex = Index;
        Button->OnClicked.AddDynamic(Action, &USkiPlaceSearchAction::Clicked);
        PlaceSearchActions.Add(Action);
    }
}

void USkiP1Widget::UpdatePickerSteps()
{
    const bool bHasBoundary = SiteMapWidget && SiteMapWidget->HasValidSelection();
    const int32 ActiveStep = bHasBoundary ? 2 : bBoundaryStepActive ? 1 : 0;
    if (LocationControls)
        LocationControls->SetVisibility(bBoundaryStepActive ? ESlateVisibility::Collapsed
            : ESlateVisibility::Visible);
    if (SelectSiteButton) SelectSiteButton->SetIsEnabled(bHasChosenLocation);
    if (BoundaryControls)
        BoundaryControls->SetVisibility(bBoundaryStepActive ? ESlateVisibility::Visible
            : ESlateVisibility::Collapsed);
    if (ResortNameControls)
        ResortNameControls->SetVisibility(bHasBoundary ? ESlateVisibility::Visible
            : ESlateVisibility::Collapsed);
    if (SiteMapWidget) SiteMapWidget->SetBoundaryEditingEnabled(bBoundaryStepActive);
    const TCHAR* StepNames[] = {TEXT("Choose location"), TEXT("Define boundary"), TEXT("Name resort"), TEXT("Download")};
    for (int32 StepIndex = 0; StepIndex < PickerStepLabels.Num(); ++StepIndex)
    {
        UTextBlock* Label = PickerStepLabels[StepIndex];
        if (!Label) continue;
        const bool bComplete = StepIndex < ActiveStep;
        const bool bActive = StepIndex == ActiveStep;
        const TCHAR* Marker = bComplete ? TEXT("✓") : bActive ? TEXT("●") : TEXT("○");
        Label->SetText(FText::FromString(FString::Printf(TEXT("%s  %d  %s"),
            Marker, StepIndex + 1, StepNames[StepIndex])));
        Label->SetColorAndOpacity(FSlateColor(bActive ? FLinearColor(0.04f, 0.35f, 0.49f, 1.0f)
            : bComplete ? FLinearColor(0.11f, 0.44f, 0.30f, 1.0f) : PickerMutedInk));
        Label->SetFont(FCoreStyle::GetDefaultFontStyle(bActive ? TEXT("Bold") : TEXT("Regular"),
            bActive ? 13 : 12));
    }
}

void USkiP1Widget::SelectPlaceSearchResult(const int32 Generation, const int32 ResultIndex)
{
    if (Generation != PlaceSearchGeneration || !PlaceSearchResultsData.IsValidIndex(ResultIndex)
        || !SiteMapWidget || !PickerSearchStatusText) return;
    const FSkiPlaceSearchResult& Result = PlaceSearchResultsData[ResultIndex];
    bHasChosenLocation = true;
    bBoundaryStepActive = false;
    const bool bHasValidBounds = FMath::IsFinite(Result.BoundingBox.WestDeg)
        && FMath::IsFinite(Result.BoundingBox.SouthDeg)
        && FMath::IsFinite(Result.BoundingBox.EastDeg)
        && FMath::IsFinite(Result.BoundingBox.NorthDeg)
        && Result.BoundingBox.WestDeg >= -180.0 && Result.BoundingBox.EastDeg <= 180.0
        && Result.BoundingBox.SouthDeg >= -90.0 && Result.BoundingBox.NorthDeg <= 90.0
        && Result.BoundingBox.WestDeg <= Result.BoundingBox.EastDeg
        && Result.BoundingBox.SouthDeg <= Result.BoundingBox.NorthDeg;
    SiteMapWidget->ClearSelection();
    const bool bBoundsFit = bHasValidBounds && SiteMapWidget->FitToBounds(Result.BoundingBox);
    if (!bBoundsFit)
        SiteMapWidget->SetCenter(Result.LatitudeDeg, Result.LongitudeDeg);
    if (ResortNameBox) ResortNameBox->SetText(FText::FromString(Result.Name));
    UpdatePickerSteps();
    PickerSearchStatusText->SetText(FText::FromString(FString::Printf(
        TEXT("Centered over %s. Select site to define its boundary."), *Result.Name)));
}

void USkiPlaceSearchAction::Clicked()
{
    if (Owner.IsValid()) Owner->SelectPlaceSearchResult(SearchGeneration, ResultIndex);
}

void USkiP1Widget::ToggleContoursClicked()
{
    bContoursVisible = !bContoursVisible;
    if (SiteMapWidget) SiteMapWidget->SetContoursEnabled(bContoursVisible);
    if (ToggleContoursButton)
    {
        if (UTextBlock* Label = Cast<UTextBlock>(ToggleContoursButton->GetChildAt(0)))
            Label->SetText(FText::FromString(bContoursVisible ? TEXT("Contours: on") : TEXT("Contours: off")));
    }
}

void USkiP1Widget::ToggleContourUnitsClicked()
{
    bMetricContourUnits = !bMetricContourUnits;
    if (SiteMapWidget) SiteMapWidget->SetContourUnits(bMetricContourUnits);
    if (ContourUnitsButton)
    {
        if (UTextBlock* Label = Cast<UTextBlock>(ContourUnitsButton->GetChildAt(0)))
            Label->SetText(FText::FromString(bMetricContourUnits
                ? TEXT("Labels: m") : TEXT("Labels: ft")));
    }
}

void USkiP1Widget::ClearBoundaryClicked()
{
    if (SiteMapWidget) SiteMapWidget->ClearSelection();
    if (PickerBoundaryStatusText)
        PickerBoundaryStatusText->SetText(FText::FromString(TEXT("Drag on the map to draw the site boundary.")));
    UpdatePickerSteps();
}

void USkiP1Widget::PresentationClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Presentation"));} void USkiP1Widget::ElevationClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Elevation"));}
void USkiP1Widget::PhotoClicked(){if(bPhotoCommandAvailable&&ViewCommandHandler)ViewCommandHandler(TEXT("Photo"));}
void USkiP1Widget::SlopeClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Slope"));} void USkiP1Widget::CoverClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Cover"));} void USkiP1Widget::LodClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod"));}
void USkiP1Widget::LodAutoClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("LodAuto"));}
void USkiP1Widget::Lod0Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod0"));} void USkiP1Widget::Lod1Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod1"));} void USkiP1Widget::Lod2Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Lod2"));}
void USkiP1Widget::Vertical1Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical1"));} void USkiP1Widget::Vertical2Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical2"));} void USkiP1Widget::Vertical4Clicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Vertical4"));}
void USkiP1Widget::MiddayClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Midday"));} void USkiP1Widget::LowAngleClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("LowAngle"));} void USkiP1Widget::OvercastClicked(){if(ViewCommandHandler)ViewCommandHandler(TEXT("Overcast"));}
