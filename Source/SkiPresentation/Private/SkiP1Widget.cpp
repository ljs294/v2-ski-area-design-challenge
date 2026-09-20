#include "SkiP1Widget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Misc/Guid.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SkiSelectorBrowser.h"

void USkiP1Widget::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    if (!WidgetTree || WidgetTree->RootWidget) return;

    UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("P1Shell"));
    WidgetTree->RootWidget = Root;

    UVerticalBox* SelectorPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("SelectorPanel"));
    if (UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(SelectorPanel))
    {
        PanelSlot->SetPosition({24.0, 24.0});
        PanelSlot->SetSize({640.0, 600.0});
    }
    UTextBlock* Heading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("P1Heading"));
    Heading->SetText(FText::FromString(TEXT("Mountain Planner — Terrain Preparation")));
    SelectorPanel->AddChildToVerticalBox(Heading);

    Selector = WidgetTree->ConstructWidget<USkiSelectorBrowser>(USkiSelectorBrowser::StaticClass(), TEXT("MapSelector"));
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("P1Selector/index.html"));
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    const FString AutoTest = FParse::Param(FCommandLine::Get(), TEXT("SkiP1SelectorSmoke")) ? TEXT("&autotest=1") : TEXT("");
    const FString Url = FString::Printf(TEXT("file:///%s?token=%s&generation=1%s"), *Path, *Token, *AutoTest);
    Selector->Configure(Url, Token, 1,
        [this](const SkiPreparation::Request& Request) { AcceptSelection(Request); },
        [this](const FString& Error) { SetTransientStatus(Error); });
    if (UVerticalBoxSlot* BrowserSlot = SelectorPanel->AddChildToVerticalBox(Selector)) BrowserSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

    UVerticalBox* StatusPanel = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("StatusPanel"));
    if (UCanvasPanelSlot* PanelSlot = Root->AddChildToCanvas(StatusPanel))
    {
        PanelSlot->SetAnchors(FAnchors(1.0, 0.0));
        PanelSlot->SetAlignment({1.0, 0.0});
        PanelSlot->SetPosition({-24.0, 24.0});
        PanelSlot->SetSize({440.0, 190.0});
    }
    ProgressText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("PreparationProgress"));
    ProgressText->SetText(FText::FromString(TEXT("Selected — awaiting bounds")));
    StatusPanel->AddChildToVerticalBox(ProgressText);
    ProgressBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("PreparationProgressBar"));
    ProgressBar->SetPercent(0.0f);
    StatusPanel->AddChildToVerticalBox(ProgressBar);
    StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TransientStatus"));
    StatusText->SetText(FText::FromString(TEXT("Selector owns input until preparation starts.")));
    StatusPanel->AddChildToVerticalBox(StatusText);
    UTextBlock* NodeView = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NodeViewPrototype"));
    NodeView->SetText(FText::FromString(TEXT("Runtime node view\n• Source bounds\n• Installed terrain\n• Render/query revision")));
    StatusPanel->AddChildToVerticalBox(NodeView);
}

void USkiP1Widget::SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> Handler)
{
    SelectionHandler = std::move(Handler);
}

void USkiP1Widget::AcceptSelection(const SkiPreparation::Request& Request)
{
    SetTransientStatus(TEXT("Selection accepted; browser resources released."));
    if (SelectionHandler) SelectionHandler(Request);
}

void USkiP1Widget::SetPreparationProgress(const SkiPreparation::Progress& Progress)
{
    if (ProgressText) ProgressText->SetText(FText::FromString(FString::Printf(TEXT("%s — %s"),
        SkiPreparation::StateName(Progress.Phase), *Progress.Detail)));
    if (ProgressBar)
    {
        ProgressBar->SetPercent(Progress.Total.IsSet() && Progress.Total.GetValue() > 0
            ? static_cast<float>(Progress.Completed) / static_cast<float>(Progress.Total.GetValue()) : 0.0f);
    }
}

void USkiP1Widget::SetTransientStatus(const FString& Status)
{
    if (StatusText) StatusText->SetText(FText::FromString(Status));
}

void USkiP1Widget::CloseSelector()
{
    if (Selector) Selector->Close();
}

bool USkiP1Widget::IsP1Ready() const
{
    return WidgetTree && WidgetTree->RootWidget && Selector && ProgressText && ProgressBar && StatusText;
}
