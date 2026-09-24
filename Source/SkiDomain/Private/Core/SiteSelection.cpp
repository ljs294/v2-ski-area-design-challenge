#include "SkiDomain/SiteSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double Pi = 3.14159265358979323846264338327950288;
constexpr double MaximumLatitudeDeg = 85.0511287798066;
constexpr double MinimumSideM = 2000.0;

bool ValidCeiling(const double MaximumSideM) noexcept
{
    return std::isfinite(MaximumSideM) && MaximumSideM >= MinimumSideM
        && MaximumSideM <= 10000.0;
}

double SignedClampedSpan(const double Span, const double MaximumSideM) noexcept
{
    return std::copysign(std::clamp(std::abs(Span), MinimumSideM, MaximumSideM),
        Span == 0.0 ? 1.0 : Span);
}
}

bool SkiDomain::TryWebMercatorPixel(const double LatitudeDeg, const double LongitudeDeg,
    const std::uint8_t Zoom, WebMercatorPixel& OutPixel) noexcept
{
    OutPixel = {};
    if (!std::isfinite(LatitudeDeg) || !std::isfinite(LongitudeDeg)
        || LatitudeDeg < -MaximumLatitudeDeg || LatitudeDeg > MaximumLatitudeDeg
        || LongitudeDeg < -180.0 || LongitudeDeg > 180.0 || Zoom > 22) return false;
    const double LatitudeRad = LatitudeDeg * Pi / 180.0;
    const double Extent = std::ldexp(256.0, Zoom);
    OutPixel.X = (LongitudeDeg + 180.0) / 360.0 * Extent;
    OutPixel.Y = std::clamp((1.0 - std::asinh(std::tan(LatitudeRad)) / Pi)
        * 0.5 * Extent, 0.0, Extent);
    return std::isfinite(OutPixel.X) && std::isfinite(OutPixel.Y);
}

bool SkiDomain::TryGeodeticFromWebMercatorPixel(const WebMercatorPixel Pixel,
    const std::uint8_t Zoom, double& OutLatitudeDeg, double& OutLongitudeDeg) noexcept
{
    OutLatitudeDeg = 0.0;
    OutLongitudeDeg = 0.0;
    if (!std::isfinite(Pixel.X) || !std::isfinite(Pixel.Y) || Zoom > 22) return false;
    const double Extent = std::ldexp(256.0, Zoom);
    if (Pixel.X < 0.0 || Pixel.X > Extent || Pixel.Y < 0.0 || Pixel.Y > Extent) return false;
    OutLongitudeDeg = Pixel.X / Extent * 360.0 - 180.0;
    OutLatitudeDeg = std::atan(std::sinh(Pi * (1.0 - 2.0 * Pixel.Y / Extent))) * 180.0 / Pi;
    return std::isfinite(OutLatitudeDeg) && std::isfinite(OutLongitudeDeg);
}

bool SkiDomain::TryWebMercatorTile(const double LatitudeDeg, const double LongitudeDeg,
    const std::uint8_t Zoom, WebMercatorTile& OutTile) noexcept
{
    OutTile = {};
    WebMercatorPixel Pixel;
    if (!TryWebMercatorPixel(LatitudeDeg, LongitudeDeg, Zoom, Pixel)) return false;
    const std::uint32_t Count = 1U << Zoom;
    OutTile.X = static_cast<std::uint32_t>(std::clamp(std::floor(Pixel.X / 256.0),
        0.0, static_cast<double>(Count - 1U)));
    OutTile.Y = static_cast<std::uint32_t>(std::clamp(std::floor(Pixel.Y / 256.0),
        0.0, static_cast<double>(Count - 1U)));
    OutTile.Zoom = Zoom;
    return true;
}

bool SkiDomain::IsValidSiteRectangle(const SiteRectangleM& Rectangle,
    const double MaximumSideM) noexcept
{
    return ValidCeiling(MaximumSideM)
        && std::isfinite(Rectangle.WestM) && std::isfinite(Rectangle.SouthM)
        && std::isfinite(Rectangle.EastM) && std::isfinite(Rectangle.NorthM)
        && Rectangle.EastM - Rectangle.WestM >= MinimumSideM
        && Rectangle.NorthM - Rectangle.SouthM >= MinimumSideM
        && Rectangle.EastM - Rectangle.WestM <= MaximumSideM
        && Rectangle.NorthM - Rectangle.SouthM <= MaximumSideM;
}

bool SkiDomain::TryDrawSiteRectangle(const double AnchorEastM, const double AnchorNorthM,
    const double PointerEastM, const double PointerNorthM, SiteRectangleM& OutRectangle,
    const double MaximumSideM) noexcept
{
    OutRectangle = {};
    if (!ValidCeiling(MaximumSideM) || !std::isfinite(AnchorEastM)
        || !std::isfinite(AnchorNorthM) || !std::isfinite(PointerEastM)
        || !std::isfinite(PointerNorthM)) return false;
    const double East = AnchorEastM + SignedClampedSpan(PointerEastM - AnchorEastM, MaximumSideM);
    const double North = AnchorNorthM + SignedClampedSpan(PointerNorthM - AnchorNorthM, MaximumSideM);
    OutRectangle = {std::min(AnchorEastM, East), std::min(AnchorNorthM, North),
        std::max(AnchorEastM, East), std::max(AnchorNorthM, North)};
    return IsValidSiteRectangle(OutRectangle, MaximumSideM);
}

bool SkiDomain::TryMoveSiteRectangle(const SiteRectangleM& Rectangle,
    const double DeltaEastM, const double DeltaNorthM, SiteRectangleM& OutRectangle,
    const double MaximumSideM) noexcept
{
    const SiteRectangleM Original = Rectangle;
    OutRectangle = {};
    if (!IsValidSiteRectangle(Original, MaximumSideM)
        || !std::isfinite(DeltaEastM) || !std::isfinite(DeltaNorthM)) return false;
    OutRectangle = {Original.WestM + DeltaEastM, Original.SouthM + DeltaNorthM,
        Original.EastM + DeltaEastM, Original.NorthM + DeltaNorthM};
    return IsValidSiteRectangle(OutRectangle, MaximumSideM);
}

bool SkiDomain::TryResizeSiteRectangle(const SiteRectangleM& Rectangle,
    const SiteResizeHandle Handle, const double PointerEastM, const double PointerNorthM,
    SiteRectangleM& OutRectangle, const double MaximumSideM) noexcept
{
    const SiteRectangleM Original = Rectangle;
    OutRectangle = {};
    if (!IsValidSiteRectangle(Original, MaximumSideM)
        || !std::isfinite(PointerEastM) || !std::isfinite(PointerNorthM)
        || static_cast<std::uint8_t>(Handle)
            > static_cast<std::uint8_t>(SiteResizeHandle::NorthEast)) return false;
    OutRectangle = Original;
    switch (Handle)
    {
    case SiteResizeHandle::West:
    case SiteResizeHandle::SouthWest:
    case SiteResizeHandle::NorthWest:
        OutRectangle.WestM = Original.EastM - std::clamp(Original.EastM - PointerEastM,
            MinimumSideM, MaximumSideM);
        break;
    case SiteResizeHandle::East:
    case SiteResizeHandle::SouthEast:
    case SiteResizeHandle::NorthEast:
        OutRectangle.EastM = Original.WestM + std::clamp(PointerEastM - Original.WestM,
            MinimumSideM, MaximumSideM);
        break;
    default: break;
    }
    switch (Handle)
    {
    case SiteResizeHandle::South:
    case SiteResizeHandle::SouthWest:
    case SiteResizeHandle::SouthEast:
        OutRectangle.SouthM = Original.NorthM - std::clamp(Original.NorthM - PointerNorthM,
            MinimumSideM, MaximumSideM);
        break;
    case SiteResizeHandle::North:
    case SiteResizeHandle::NorthWest:
    case SiteResizeHandle::NorthEast:
        OutRectangle.NorthM = Original.SouthM + std::clamp(PointerNorthM - Original.SouthM,
            MinimumSideM, MaximumSideM);
        break;
    default: break;
    }
    return IsValidSiteRectangle(OutRectangle, MaximumSideM);
}
