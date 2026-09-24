#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SSkiSiteMap.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM2SiteMapAntimeridianPixelWrappingTest,
    "MountainPlanner.M2.Presentation.SitePicker.AntimeridianPixelWrapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FM2SiteMapAntimeridianPixelWrappingTest::RunTest(const FString&)
{
    constexpr double WorldPixels = 4096.0;
    constexpr double TilePixels = 256.0;
    TestTrue(TEXT("Negative screen X wraps into the canonical world"),
        FMath::IsNearlyEqual(SkiSiteMapMath::WrapWorldPixelX(-1.0, WorldPixels),
            WorldPixels - 1.0));
    TestTrue(TEXT("Screen X beyond the eastern edge wraps into the canonical world"),
        FMath::IsNearlyEqual(SkiSiteMapMath::WrapWorldPixelX(WorldPixels + 7.0,
            WorldPixels), 7.0));
    TestTrue(TEXT("The eastern world edge wraps to the western edge"),
        FMath::IsNearlyEqual(SkiSiteMapMath::WrapWorldPixelX(WorldPixels,
            WorldPixels), 0.0));

    TestTrue(TEXT("A canonical western-edge tile paints west of a near-west center"),
        FMath::IsNearlyEqual(SkiSiteMapMath::UnwrapWorldPixelXNear(
            WorldPixels - TilePixels, 10.0, WorldPixels), -TilePixels));
    TestTrue(TEXT("A canonical western tile paints east of a near-east center"),
        FMath::IsNearlyEqual(SkiSiteMapMath::UnwrapWorldPixelXNear(0.0,
            WorldPixels - 10.0, WorldPixels), WorldPixels));
    TestTrue(TEXT("A center stored at longitude 180 keeps its canonical tile nearby"),
        FMath::IsNearlyEqual(SkiSiteMapMath::UnwrapWorldPixelXNear(0.0,
            WorldPixels, WorldPixels), WorldPixels));

    TestEqual(TEXT("Negative raw tile X retains its canonical request key"),
        SkiSiteMapMath::WrapWorldTileX(-1, 16), 15);
    TestEqual(TEXT("Eastern overflow retains its canonical request key"),
        SkiSiteMapMath::WrapWorldTileX(16, 16), 0);
    return true;
}

#endif
