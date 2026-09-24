#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiPreparation/SkiProjectionFallback.h"

using namespace SkiPreparation::ProjectionFallback;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkiProjectionFallbackTest,
    "MountainPlanner.M0.ProjectionFallback.GN72AndConus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkiProjectionFallbackTest::RunTest(const FString& Parameters)
{
    // IOGP GN7-2 (2019), section 3.2.3.1, OSGB 1936 / BNG JHS numeric control.
    TransverseMercator GnTM;
    GnTM.Surface = {6377563.396, 299.32496};
    GnTM.OriginLatitudeDeg = 49.0;
    GnTM.CentralMeridianDeg = -2.0;
    GnTM.Scale = 0.9996012717;
    GnTM.FalseEastingM = 400000.0;
    GnTM.FalseNorthingM = -100000.0;
    Projected British;
    TestTrue(TEXT("GN7-2 TM forward"), ForwardTransverseMercator(GnTM, {50.5, 0.5}, British));
    TestTrue(TEXT("GN7-2 TM easting within 1 cm"), FMath::Abs(British.EastingM - 577274.99) <= 0.01);
    TestTrue(TEXT("GN7-2 TM northing within 1 cm"), FMath::Abs(British.NorthingM - 69740.50) <= 0.01);
    Geodetic BritishBack;
    TestTrue(TEXT("GN7-2 TM reverse"), InverseTransverseMercator(GnTM, British, BritishBack));
    TestTrue(TEXT("GN7-2 TM reverse latitude"), FMath::Abs(BritishBack.LatitudeDeg - 50.5) < 1e-8);
    TestTrue(TEXT("GN7-2 TM reverse longitude"), FMath::Abs(BritishBack.LongitudeDeg - 0.5) < 1e-8);

    const AlbersEqualArea Conus = Epsg6350();
    TestEqual(TEXT("6350 origin latitude"), Conus.OriginLatitudeDeg, 23.0);
    TestEqual(TEXT("6350 central meridian"), Conus.CentralMeridianDeg, -96.0);
    TestEqual(TEXT("6350 first parallel"), Conus.FirstStandardParallelDeg, 29.5);
    TestEqual(TEXT("6350 second parallel"), Conus.SecondStandardParallelDeg, 45.5);
    Projected AlbersOrigin;
    TestTrue(TEXT("6350 origin converts"), ForwardAlbers(Conus, {23.0,-96.0}, AlbersOrigin));
    TestTrue(TEXT("6350 origin easting zero"), FMath::Abs(AlbersOrigin.EastingM) < 1e-7);
    TestTrue(TEXT("6350 origin northing zero"), FMath::Abs(AlbersOrigin.NorthingM) < 1e-7);
    for (const Geodetic Point : {Geodetic{46.93,-121.50}, Geodetic{44.0,-71.0}, Geodetic{35.0,-105.0}})
    {
        Projected Map;
        Geodetic Back;
        TestTrue(TEXT("6350 forward"), ForwardAlbers(Conus, Point, Map));
        TestTrue(TEXT("6350 reverse"), InverseAlbers(Conus, Map, Back));
        TestTrue(TEXT("6350 latitude roundtrip"), FMath::Abs(Back.LatitudeDeg-Point.LatitudeDeg)<1e-8);
        TestTrue(TEXT("6350 longitude roundtrip"), FMath::Abs(Back.LongitudeDeg-Point.LongitudeDeg)<1e-8);
    }

    // IOGP GN7-2 (2024), Albers example 1, EPSG:3174 NAD83 / Great Lakes Albers.
    // This is an independent forward control, separate from the 6350 round trips.
    AlbersEqualArea GreatLakes;
    GreatLakes.OriginLatitudeDeg = 45.0 + 34.0/60.0 + 8.3172/3600.0;
    GreatLakes.CentralMeridianDeg = -(84.0 + 27.0/60.0 + 21.4380/3600.0);
    GreatLakes.FirstStandardParallelDeg = 42.0 + 7.0/60.0 + 21.9864/3600.0;
    GreatLakes.SecondStandardParallelDeg = 49.0 + 54.6480/3600.0;
    GreatLakes.FalseEastingM = 1000000.0;
    GreatLakes.FalseNorthingM = 1000000.0;
    Projected GreatLakesControl;
    TestTrue(TEXT("GN7-2 Great Lakes Albers forward"),
        ForwardAlbers(GreatLakes, {42.75,-78.75}, GreatLakesControl));
    TestTrue(TEXT("GN7-2 Great Lakes easting within 1 cm"),
        FMath::Abs(GreatLakesControl.EastingM - 1466493.492) <= 0.01);
    TestTrue(TEXT("GN7-2 Great Lakes northing within 1 cm"),
        FMath::Abs(GreatLakesControl.NorthingM - 702903.006) <= 0.01);
    Geodetic GreatLakesBack;
    TestTrue(TEXT("GN7-2 Great Lakes Albers reverse"),
        InverseAlbers(GreatLakes, GreatLakesControl, GreatLakesBack));
    TestTrue(TEXT("GN7-2 Great Lakes reverse latitude"),
        FMath::Abs(GreatLakesBack.LatitudeDeg - 42.75) < 1e-8);
    TestTrue(TEXT("GN7-2 Great Lakes reverse longitude"),
        FMath::Abs(GreatLakesBack.LongitudeDeg + 78.75) < 1e-8);

    const TransverseMercator Project = Epsg26910();
    TestEqual(TEXT("26910 central meridian"), Project.CentralMeridianDeg, -123.0);
    Projected UTMOrigin;
    TestTrue(TEXT("26910 equatorial central meridian"),
        ForwardTransverseMercator(Project, {0.0,-123.0}, UTMOrigin));
    TestTrue(TEXT("26910 false easting"), FMath::Abs(UTMOrigin.EastingM-500000.0)<1e-6);
    TestFalse(TEXT("Unsupported UTM zone rejected"),
        ForwardTransverseMercator(Nad83UtmNorth(19), {45.0,-69.0}, UTMOrigin));

    HorizontalErrorBudget Budget;
    Geodetic Approximation;
    TestTrue(TEXT("WGS84 approximation records uncertainty"),
        ApproximateWgs84AsNad83({46.0,-121.0}, Approximation, Budget));
    TestEqual(TEXT("Datum approximation is explicitly 1.5 m"), Budget.DatumApproximationM, 1.5);
    return true;
}

#endif
