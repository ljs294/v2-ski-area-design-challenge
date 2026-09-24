#include "SkiM0ShippingProjectionProbe.h"

#include "SkiPreparation/M0RasterProjectionProbe.h"
#include "SkiPreparation/SkiProjectionFallback.h"
#include "GeoReferencingSystem.h"
#include "GeographicCoordinates.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

#include <cmath>

namespace
{
void ProbeFallbackProjection(SkiPreparation::FM0RasterProjectionReceipt& Receipt)
{
    using namespace SkiPreparation::ProjectionFallback;
    TransverseMercator British;
    British.Surface = {6377563.396, 299.32496};
    British.OriginLatitudeDeg = 49.0;
    British.CentralMeridianDeg = -2.0;
    British.Scale = 0.9996012717;
    British.FalseEastingM = 400000.0;
    British.FalseNorthingM = -100000.0;
    Projected BritishControl;
    if (!ForwardTransverseMercator(British, {50.5, 0.5}, BritishControl))
    { Receipt.FallbackProjectionError = TEXT("FALLBACK_TM_CONTROL_FAILED"); return; }
    Receipt.FallbackGnTransverseMercatorErrorMeters = std::hypot(
        BritishControl.EastingM - 577274.99, BritishControl.NorthingM - 69740.50);

    AlbersEqualArea GreatLakes;
    GreatLakes.OriginLatitudeDeg = 45.0 + 34.0 / 60.0 + 8.3172 / 3600.0;
    GreatLakes.CentralMeridianDeg = -(84.0 + 27.0 / 60.0 + 21.4380 / 3600.0);
    GreatLakes.FirstStandardParallelDeg = 42.0 + 7.0 / 60.0 + 21.9864 / 3600.0;
    GreatLakes.SecondStandardParallelDeg = 49.0 + 54.6480 / 3600.0;
    GreatLakes.FalseEastingM = 1000000.0;
    GreatLakes.FalseNorthingM = 1000000.0;
    Projected GreatLakesControl;
    if (!ForwardAlbers(GreatLakes, {42.75, -78.75}, GreatLakesControl))
    { Receipt.FallbackProjectionError = TEXT("FALLBACK_ALBERS_CONTROL_FAILED"); return; }
    Receipt.FallbackGnAlbersErrorMeters = std::hypot(
        GreatLakesControl.EastingM - 1466493.492, GreatLakesControl.NorthingM - 702903.006);

    const Geodetic MountWashington{44.2706, -71.3033};
    Projected S1M;
    Geodetic S1MBack;
    if (!ForwardAlbers(Epsg6350(), MountWashington, S1M)
        || !InverseAlbers(Epsg6350(), S1M, S1MBack))
    { Receipt.FallbackProjectionError = TEXT("FALLBACK_S1M_ROUNDTRIP_FAILED"); return; }
    Receipt.FallbackS1MRoundTripMeters = std::hypot(
        (S1MBack.LatitudeDeg - MountWashington.LatitudeDeg) * 111132.0,
        (S1MBack.LongitudeDeg - MountWashington.LongitudeDeg) * 111320.0
            * std::cos(MountWashington.LatitudeDeg * 3.14159265358979323846 / 180.0));

    const Geodetic CrystalMountain{46.9282, -121.5045};
    Projected Project1M;
    Geodetic ProjectBack;
    if (!ForwardTransverseMercator(Epsg26910(), CrystalMountain, Project1M)
        || !InverseTransverseMercator(Epsg26910(), Project1M, ProjectBack))
    { Receipt.FallbackProjectionError = TEXT("FALLBACK_PROJECT_UTM_ROUNDTRIP_FAILED"); return; }
    Receipt.FallbackProjectRoundTripMeters = std::hypot(
        (ProjectBack.LatitudeDeg - CrystalMountain.LatitudeDeg) * 111132.0,
        (ProjectBack.LongitudeDeg - CrystalMountain.LongitudeDeg) * 111320.0
            * std::cos(CrystalMountain.LatitudeDeg * 3.14159265358979323846 / 180.0));

    HorizontalErrorBudget Budget;
    Geodetic Approximate;
    if (!ApproximateWgs84AsNad83(MountWashington, Approximate, Budget))
    { Receipt.FallbackProjectionError = TEXT("FALLBACK_DATUM_BUDGET_FAILED"); return; }
    Receipt.FallbackDatumApproximationMeters = Budget.DatumApproximationM;
    Receipt.bFallbackProjectionPassed = std::isfinite(Receipt.FallbackGnAlbersErrorMeters)
        && Receipt.FallbackGnAlbersErrorMeters <= 0.01
        && std::isfinite(Receipt.FallbackGnTransverseMercatorErrorMeters)
        && Receipt.FallbackGnTransverseMercatorErrorMeters <= 0.01
        && std::isfinite(Receipt.FallbackS1MRoundTripMeters)
        && Receipt.FallbackS1MRoundTripMeters <= 0.01
        && std::isfinite(Receipt.FallbackProjectRoundTripMeters)
        && Receipt.FallbackProjectRoundTripMeters <= 0.01
        && Receipt.FallbackDatumApproximationMeters == 1.5;
    if (!Receipt.bFallbackProjectionPassed)
        Receipt.FallbackProjectionError = TEXT("FALLBACK_NUMERIC_BUDGET_FAILED");
}
}

bool ProbeM0ShippingProjection(UWorld& World, SkiPreparation::FM0RasterProjectionReceipt& Receipt)
{
    check(IsInGameThread());
    ProbeFallbackProjection(Receipt);
    AGeoReferencingSystem* System = World.SpawnActor<AGeoReferencingSystem>();
    if (!System)
    {
        Receipt.ProjError = TEXT("GEOREFERENCING_ACTOR_SPAWN_FAILED"); return false;
    }
    // Independent spherical Web Mercator control, then restore the source/datum pair.
    System->GeographicCRS = TEXT("EPSG:4326");
    System->ProjectedCRS = TEXT("EPSG:3857");
    System->ApplySettings();
    FVector Mercator;
    System->GeographicToProjected(FGeographicCoordinates(-71.3033, 44.2706, 0.0), Mercator);
    // UE_PI and FMath::DegreesToRadians use float constants; at projected coordinates
    // near 8,000 km their rounding contributes decimetres. Keep this analytic
    // control entirely in double precision so it can test a centimetre budget.
    constexpr double Pi64 = 3.141592653589793238462643383279502884;
    const double LonRad = -71.3033 * Pi64 / 180.0;
    const double LatRad = 44.2706 * Pi64 / 180.0;
    const double ExpectedX = 6378137.0 * LonRad;
    const double ExpectedY = 6378137.0 * std::log(std::tan(Pi64 / 4.0 + LatRad / 2.0));
    Receipt.ProjControlErrorMeters = FVector2D::Distance(FVector2D(Mercator.X, Mercator.Y),
        FVector2D(ExpectedX, ExpectedY));
    // IOGP EPSG Guidance Note 7-2 (Dec 2024), p.40. NAD83 / Great Lakes Albers.
    System->GeographicCRS = TEXT("EPSG:4269");
    System->ProjectedCRS = TEXT("EPSG:3174");
    System->ApplySettings();
    FVector Albers;
    System->GeographicToProjected(FGeographicCoordinates(-78.75, 42.75, 0.0), Albers);
    Receipt.GnAlbersErrorMeters = FVector2D::Distance(FVector2D(Albers.X, Albers.Y),
        FVector2D(1466493.492, 702903.006));
    // Same note, pp.62-63. OSGB36 / British National Grid; input is OSGB36,
    // so a WGS84 datum transformation must not be folded into this control.
    System->GeographicCRS = TEXT("EPSG:4277");
    System->ProjectedCRS = TEXT("EPSG:27700");
    System->ApplySettings();
    FVector TransverseMercator;
    System->GeographicToProjected(FGeographicCoordinates(0.5, 50.5, 0.0), TransverseMercator);
    Receipt.GnTransverseMercatorErrorMeters = FVector2D::Distance(
        FVector2D(TransverseMercator.X, TransverseMercator.Y), FVector2D(577274.99, 69740.50));
    // NOAA NGS NCAT (NADCON 5.0) control at 40 N, 80 W:
    // NAD83(1986) -> NAD83(2011) = 39.9999983008 N, 79.9999976143 W.
    // Compare both geodetic inputs in the same EPSG:6350 projected frame to
    // isolate the datum operation. NGS lists 1-sigma components 0.0081/0.0052 m;
    // 0.03 m is approximately three times their combined horizontal uncertainty.
    // Source: https://geodesy.noaa.gov/api/ncat/llh?lat=40.0&lon=-80.0&eht=100.0&inDatum=nad83(1986)&outDatum=nad83(2011)
    System->GeographicCRS = TEXT("EPSG:4269");
    System->ProjectedCRS = TEXT("EPSG:6350");
    System->ApplySettings();
    FVector FromOriginalNad83;
    System->GeographicToProjected(FGeographicCoordinates(-80.0, 40.0, 0.0), FromOriginalNad83);
    System->GeographicCRS = TEXT("EPSG:6318");
    System->ApplySettings();
    FVector FromNgs2011;
    System->GeographicToProjected(
        FGeographicCoordinates(-79.9999976143, 39.9999983008, 0.0), FromNgs2011);
    Receipt.NgsDatumErrorMeters = FVector2D::Distance(
        FVector2D(FromOriginalNad83.X, FromOriginalNad83.Y),
        FVector2D(FromNgs2011.X, FromNgs2011.Y));
    System->GeographicCRS = TEXT("EPSG:6318");
    System->ProjectedCRS = TEXT("EPSG:6350");
    System->ApplySettings();
    const FGeographicCoordinates Input(-71.3033, 44.2706, 0.0);
    FVector Projected;
    System->GeographicToProjected(Input, Projected);
    FGeographicCoordinates Back;
    System->ProjectedToGeographic(Projected, Back);
    const double Error = FMath::Sqrt(FMath::Square((Back.Longitude - Input.Longitude) * 78500.0)
        + FMath::Square((Back.Latitude - Input.Latitude) * 111000.0));
    Receipt.ProjRoundTripMeters = Error;
    FGeographicCoordinates Corners[2][2];
    for (int32 Y = 0; Y < 2; ++Y)
    for (int32 X = 0; X < 2; ++X)
        System->ProjectedToGeographic(Projected + FVector(X * 32.0, Y * 32.0, 0.0), Corners[Y][X]);
    const double Start = FPlatformTime::Seconds();
    for (int32 Y = 0; Y <= 4; ++Y)
    for (int32 X = 0; X <= 4; ++X)
    {
        const double U = X / 4.0, V = Y / 4.0;
        FGeographicCoordinates Exact;
        System->ProjectedToGeographic(Projected + FVector(U * 32.0, V * 32.0, 0.0), Exact);
        const double InterpolatedLon = FMath::Lerp(FMath::Lerp(Corners[0][0].Longitude, Corners[0][1].Longitude, U),
            FMath::Lerp(Corners[1][0].Longitude, Corners[1][1].Longitude, U), V);
        const double InterpolatedLat = FMath::Lerp(FMath::Lerp(Corners[0][0].Latitude, Corners[0][1].Latitude, U),
            FMath::Lerp(Corners[1][0].Latitude, Corners[1][1].Latitude, U), V);
        const double MetersPerLonDegree = 111320.0 * FMath::Cos(FMath::DegreesToRadians(Exact.Latitude));
        const double LatticeError = FMath::Sqrt(FMath::Square((Exact.Longitude - InterpolatedLon) * MetersPerLonDegree)
            + FMath::Square((Exact.Latitude - InterpolatedLat) * 111132.0));
        Receipt.ProjLatticeMaxErrorMeters = FMath::Max(Receipt.ProjLatticeMaxErrorMeters, LatticeError);
    }
    const double Elapsed = FPlatformTime::Seconds() - Start;
    Receipt.ProjSamplesPerSecond = Elapsed > 0 ? 25.0 / Elapsed : 0;
    Receipt.bShippingProjPassed = FMath::IsFinite(Projected.X) && FMath::IsFinite(Projected.Y)
        && FMath::IsFinite(Error) && Error <= 0.01
        && FMath::IsFinite(Receipt.ProjControlErrorMeters) && Receipt.ProjControlErrorMeters <= 0.01
        && FMath::IsFinite(Receipt.GnAlbersErrorMeters) && Receipt.GnAlbersErrorMeters <= 0.005
        && FMath::IsFinite(Receipt.GnTransverseMercatorErrorMeters)
        && Receipt.GnTransverseMercatorErrorMeters <= 0.02
        && FMath::IsFinite(Receipt.NgsDatumErrorMeters) && Receipt.NgsDatumErrorMeters <= 0.03
        && FMath::IsFinite(Receipt.ProjLatticeMaxErrorMeters) && Receipt.ProjLatticeMaxErrorMeters <= 0.01;
    if (!Receipt.bShippingProjPassed)
        Receipt.ProjError = !FMath::IsFinite(Receipt.NgsDatumErrorMeters) || Receipt.NgsDatumErrorMeters > 0.03
            ? TEXT("NGS_NAD83_1986_TO_2011_CONTROL_FAILED")
            : TEXT("GEOREFERENCING_PACKAGED_CONTROL_FAILED");
    System->Destroy();
    return Receipt.bShippingProjPassed;
}
