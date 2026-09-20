#include "SkiDomain/Coordinates.h"

#include <cmath>

namespace
{
constexpr double Pi = 3.1415926535897932384626433832795;
constexpr double Wgs84SemiMajorM = 6378137.0;
constexpr double Wgs84EccentricitySquared = 6.6943799901413165e-3;

double Radians(const double Degrees) noexcept
{
    return Degrees * Pi / 180.0;
}
}

bool SkiDomain::IsValidGeodetic(const GeodeticPoint& Point) noexcept
{
    return std::isfinite(Point.LatitudeDeg) && std::isfinite(Point.LongitudeDeg)
        && std::isfinite(Point.HeightM) && Point.LatitudeDeg >= -90.0
        && Point.LatitudeDeg <= 90.0 && Point.LongitudeDeg >= -180.0
        && Point.LongitudeDeg <= 180.0;
}

SkiDomain::Cartesian3 SkiDomain::GeodeticToEcef(const GeodeticPoint& Point) noexcept
{
    const double Latitude = Radians(Point.LatitudeDeg);
    const double Longitude = Radians(Point.LongitudeDeg);
    const double SinLatitude = std::sin(Latitude);
    const double CosLatitude = std::cos(Latitude);
    const double PrimeVertical = Wgs84SemiMajorM
        / std::sqrt(1.0 - Wgs84EccentricitySquared * SinLatitude * SinLatitude);
    return {
        (PrimeVertical + Point.HeightM) * CosLatitude * std::cos(Longitude),
        (PrimeVertical + Point.HeightM) * CosLatitude * std::sin(Longitude),
        (PrimeVertical * (1.0 - Wgs84EccentricitySquared) + Point.HeightM) * SinLatitude,
    };
}

bool SkiDomain::TryMakeLocalFrame(const GeodeticPoint& Origin, LocalFrame& OutFrame) noexcept
{
    if (!IsValidGeodetic(Origin))
    {
        return false;
    }
    const double Latitude = Radians(Origin.LatitudeDeg);
    const double Longitude = Radians(Origin.LongitudeDeg);
    OutFrame = {Origin, GeodeticToEcef(Origin), std::sin(Latitude), std::cos(Latitude),
        std::sin(Longitude), std::cos(Longitude)};
    return true;
}

SkiDomain::EnuPoint SkiDomain::ToEnu(const LocalFrame& Frame, const GeodeticPoint& Point) noexcept
{
    const Cartesian3 Ecef = GeodeticToEcef(Point);
    const double Dx = Ecef.X - Frame.OriginEcef.X;
    const double Dy = Ecef.Y - Frame.OriginEcef.Y;
    const double Dz = Ecef.Z - Frame.OriginEcef.Z;
    return {
        -Frame.SinLongitude * Dx + Frame.CosLongitude * Dy,
        -Frame.SinLatitude * Frame.CosLongitude * Dx
            - Frame.SinLatitude * Frame.SinLongitude * Dy + Frame.CosLatitude * Dz,
        Frame.CosLatitude * Frame.CosLongitude * Dx
            + Frame.CosLatitude * Frame.SinLongitude * Dy + Frame.SinLatitude * Dz,
    };
}

SkiDomain::UnrealPointCm SkiDomain::ToUnrealCentimeters(const EnuPoint& Point) noexcept
{
    return {100.0 * Point.NorthM, 100.0 * Point.EastM, 100.0 * Point.UpM};
}
