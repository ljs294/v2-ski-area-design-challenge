#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiP1Widget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1ResponsiveUiLayoutTest,
    "MountainPlanner.P1.Presentation.UI.ResponsiveLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1ResponsiveUiLayoutTest::RunTest(const FString&)
{
    struct FCase { FIntPoint Viewport; double ExpectedPanelWidth; };
    const FCase Cases[]{{{1280,720},520},{{1920,1080},520},{{2560,1080},520},
        {{2560,1440},520},{{576,1024},520}};
    for (const FCase& Value : Cases)
    {
        const double PanelWidth = USkiP1Widget::CalculateStatusPanelWidth(Value.Viewport.X);
        TestEqual(FString::Printf(TEXT("Panel width at %dx%d"), Value.Viewport.X, Value.Viewport.Y),
            PanelWidth, Value.ExpectedPanelWidth);
        TestTrue(FString::Printf(TEXT("Panel leaves viewport margin at %dx%d"),
            Value.Viewport.X, Value.Viewport.Y), PanelWidth <= Value.Viewport.X - 48.0);
        const FVector2D Selector = USkiP1Widget::CalculateSelectorPanelSize(Value.Viewport);
        TestTrue(FString::Printf(TEXT("Selector fits viewport at %dx%d"),
            Value.Viewport.X, Value.Viewport.Y), Selector.X <= Value.Viewport.X - 48.0
                && Selector.Y <= Value.Viewport.Y - 48.0);
        TestTrue(TEXT("Selector retains a usable minimum"), Selector.X >= 280.0
            && Selector.Y >= 320.0);
    }
    return true;
}

#endif
