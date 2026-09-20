#include "SkiBootstrapWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"

bool USkiBootstrapWidget::IsBootstrapReady() const
{
    return WidgetTree && WidgetTree->RootWidget;
}

void USkiBootstrapWidget::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    if (WidgetTree && !WidgetTree->RootWidget)
    {
        UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("BootstrapLabel"));
        Label->SetText(FText::FromString(TEXT("Mountain Planner\nNative bootstrap")));
        WidgetTree->RootWidget = Label;
    }
}
