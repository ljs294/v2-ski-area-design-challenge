#pragma once

#include "SkiDomain/Export.h"

#include <cstdint>

namespace SkiDomain
{
struct WebMercatorPixel
{
    double X = 0.0;
    double Y = 0.0;
};

struct WebMercatorTile
{
    std::uint32_t X = 0;
    std::uint32_t Y = 0;
    std::uint8_t Zoom = 0;
};

/** Map coordinates use 256-pixel XYZ tiles and a north-up Web Mercator plane. */
SKI_DOMAIN_API bool TryWebMercatorPixel(double LatitudeDeg, double LongitudeDeg,
    std::uint8_t Zoom, WebMercatorPixel& OutPixel) noexcept;
SKI_DOMAIN_API bool TryGeodeticFromWebMercatorPixel(WebMercatorPixel Pixel,
    std::uint8_t Zoom, double& OutLatitudeDeg, double& OutLongitudeDeg) noexcept;
SKI_DOMAIN_API bool TryWebMercatorTile(double LatitudeDeg, double LongitudeDeg,
    std::uint8_t Zoom, WebMercatorTile& OutTile) noexcept;

struct SiteRectangleM
{
    double WestM = 0.0;
    double SouthM = 0.0;
    double EastM = 0.0;
    double NorthM = 0.0;
};

enum class SiteResizeHandle : std::uint8_t
{
    West, East, South, North,
    SouthWest, SouthEast, NorthWest, NorthEast,
};

/** Geometry only: geographic coverage is checked separately by the catalog. */
SKI_DOMAIN_API bool IsValidSiteRectangle(const SiteRectangleM& Rectangle,
    double MaximumSideM = 4000.0) noexcept;
/** Creates a free-aspect rectangle anchored at the initial pointer location. */
SKI_DOMAIN_API bool TryDrawSiteRectangle(double AnchorEastM, double AnchorNorthM,
    double PointerEastM, double PointerNorthM, SiteRectangleM& OutRectangle,
    double MaximumSideM = 4000.0) noexcept;
SKI_DOMAIN_API bool TryMoveSiteRectangle(const SiteRectangleM& Rectangle,
    double DeltaEastM, double DeltaNorthM, SiteRectangleM& OutRectangle,
    double MaximumSideM = 4000.0) noexcept;
/** Keeps the edge or corner opposite Handle fixed while clamping side lengths. */
SKI_DOMAIN_API bool TryResizeSiteRectangle(const SiteRectangleM& Rectangle,
    SiteResizeHandle Handle, double PointerEastM, double PointerNorthM,
    SiteRectangleM& OutRectangle, double MaximumSideM = 4000.0) noexcept;
}
