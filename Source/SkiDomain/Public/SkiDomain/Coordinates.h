#pragma once

#include "SkiDomain/Export.h"

namespace SkiDomain
{
struct GeodeticPoint
{
    double LatitudeDeg = 0.0;
    double LongitudeDeg = 0.0;
    double HeightM = 0.0;
};

struct Cartesian3
{
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
};

struct EnuPoint
{
    double EastM = 0.0;
    double NorthM = 0.0;
    double UpM = 0.0;
};

struct UnrealPointCm
{
    double XNorthCm = 0.0;
    double YEastCm = 0.0;
    double ZUpCm = 0.0;
};

struct LocalFrame
{
    GeodeticPoint Origin;
    Cartesian3 OriginEcef;
    double SinLatitude = 0.0;
    double CosLatitude = 1.0;
    double SinLongitude = 0.0;
    double CosLongitude = 1.0;
};

SKI_DOMAIN_API bool IsValidGeodetic(const GeodeticPoint& Point) noexcept;
SKI_DOMAIN_API Cartesian3 GeodeticToEcef(const GeodeticPoint& Point) noexcept;
SKI_DOMAIN_API bool TryMakeLocalFrame(const GeodeticPoint& Origin, LocalFrame& OutFrame) noexcept;
SKI_DOMAIN_API EnuPoint ToEnu(const LocalFrame& Frame, const GeodeticPoint& Point) noexcept;
/** Invert horizontal ENU coordinates for a point on the WGS84 ellipsoid. */
SKI_DOMAIN_API bool TrySeaLevelGeodeticFromEnu(const LocalFrame& Frame,
    double EastM, double NorthM, GeodeticPoint& OutPoint) noexcept;
SKI_DOMAIN_API UnrealPointCm ToUnrealCentimeters(const EnuPoint& Point) noexcept;
}
