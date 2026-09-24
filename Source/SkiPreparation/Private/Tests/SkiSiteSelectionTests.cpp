#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiPreparation/SkiSiteSelection.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkiSiteSelectionTest,
    "MountainPlanner.M1.SiteSelection.PureRules",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkiSiteSelectionTest::RunTest(const FString& Parameters)
{
    FString Name, Error;
    TestTrue(TEXT("Trimmed name accepted"),
        SkiPreparation::ValidateResortName(TEXT("  Crystal Mountain  "), Name, Error));
    TestEqual(TEXT("Name canonicalized"), Name, FString(TEXT("Crystal Mountain")));
    TestFalse(TEXT("Blank name rejected"),
        SkiPreparation::ValidateResortName(TEXT("  "), Name, Error));
    TestFalse(TEXT("Control character rejected"),
        SkiPreparation::ValidateResortName(TEXT("Bad\nName"), Name, Error));
    TestTrue(TEXT("Sixty characters accepted"),
        SkiPreparation::ValidateResortName(FString::ChrN(60, TEXT('a')), Name, Error));
    TestFalse(TEXT("Sixty-one characters rejected"),
        SkiPreparation::ValidateResortName(FString::ChrN(61, TEXT('a')), Name, Error));

    using namespace SkiPreparation;
    const SiteSelectionDecision Accepted = ValidateSiteSelection(
        {0.0, 0.0, 4000.0, 2000.0}, SiteCoverage::SupportedUsUsgs3Dep);
    TestTrue(TEXT("Free-aspect 4 by 2 km accepted"), Accepted.bMayDownload);
    TestEqual(TEXT("Area is eight square kilometres"), Accepted.AreaSqKm, 8.0);
    TestFalse(TEXT("No confirmed coverage blocks download"), ValidateSiteSelection(
        {0.0, 0.0, 3000.0, 3000.0}, SiteCoverage::Unknown).bMayDownload);
    TestFalse(TEXT("Unsupported geography blocks download"), ValidateSiteSelection(
        {0.0, 0.0, 3000.0, 3000.0}, SiteCoverage::Unsupported).bMayDownload);
    TestFalse(TEXT("Short side rejected"), ValidateSiteSelection(
        {0.0, 0.0, 1999.0, 3000.0}, SiteCoverage::SupportedUsUsgs3Dep).bValidGeometry);
    TestFalse(TEXT("Over-ceiling side rejected"), ValidateSiteSelection(
        {0.0, 0.0, 4001.0, 3000.0}, SiteCoverage::SupportedUsUsgs3Dep).bValidGeometry);
    TestFalse(TEXT("Inverted rectangle rejected"), ValidateSiteSelection(
        {3000.0, 0.0, 0.0, 3000.0}, SiteCoverage::SupportedUsUsgs3Dep).bValidGeometry);
    return true;
}

#endif
