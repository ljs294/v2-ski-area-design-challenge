#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation::ProjectionFallback
{
struct Geodetic { double LatitudeDeg = 0.0; double LongitudeDeg = 0.0; };
struct Projected { double EastingM = 0.0; double NorthingM = 0.0; };
struct Ellipsoid { double SemiMajorM = 6378137.0; double InverseFlattening = 298.257222101; };
struct TransverseMercator
{
    Ellipsoid Surface;
    double OriginLatitudeDeg = 0.0;
    double CentralMeridianDeg = 0.0;
    double Scale = 0.9996;
    double FalseEastingM = 500000.0;
    double FalseNorthingM = 0.0;
};
struct AlbersEqualArea
{
    Ellipsoid Surface;
    double OriginLatitudeDeg = 0.0;
    double CentralMeridianDeg = 0.0;
    double FirstStandardParallelDeg = 0.0;
    double SecondStandardParallelDeg = 0.0;
    double FalseEastingM = 0.0;
    double FalseNorthingM = 0.0;
};
struct HorizontalErrorBudget
{
    double ProjectionMappingM = 0.0;
    double DatumApproximationM = 0.0;
    double SourcePositionalM = 0.0;
    double ConservativeTotalM() const { return ProjectionMappingM + DatumApproximationM + SourcePositionalM; }
};

SKIPREPARATION_API TransverseMercator Nad83UtmNorth(int32 Zone);
/** EPSG:26910: NAD83 / UTM zone 10N. */
SKIPREPARATION_API TransverseMercator Epsg26910();
/** EPSG:6350: NAD83(2011) / Conus Albers, not a UTM CRS. */
SKIPREPARATION_API AlbersEqualArea Epsg6350();
SKIPREPARATION_API bool ForwardTransverseMercator(const TransverseMercator& CRS,
    Geodetic Point, Projected& Out);
SKIPREPARATION_API bool InverseTransverseMercator(const TransverseMercator& CRS,
    Projected Point, Geodetic& Out);
SKIPREPARATION_API bool ForwardAlbers(const AlbersEqualArea& CRS,
    Geodetic Point, Projected& Out);
SKIPREPARATION_API bool InverseAlbers(const AlbersEqualArea& CRS,
    Projected Point, Geodetic& Out);
/** Coordinate-copy approximation only; the NAD83(2011)↔WGS84 uncertainty remains explicit. */
SKIPREPARATION_API bool ApproximateWgs84AsNad83(Geodetic Wgs84, Geodetic& OutNad83,
    HorizontalErrorBudget& InOutBudget);
}
