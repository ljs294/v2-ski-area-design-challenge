#include "SkiDomain/SiteSelection.h"

#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    int Total = 0;
    int Failed = 0;
    const auto Check = [&](const bool Condition, const char* Name)
    {
        ++Total;
        if (!Condition) { ++Failed; std::cerr << "FAIL " << Name << '\n'; }
    };

    SkiDomain::WebMercatorPixel Pixel;
    double Latitude = 0.0, Longitude = 0.0;
    Check(SkiDomain::TryWebMercatorPixel(44.2706, -71.3033, 17, Pixel)
        && SkiDomain::TryGeodeticFromWebMercatorPixel(Pixel, 17, Latitude, Longitude)
        && std::abs(Latitude - 44.2706) < 1.0e-10
        && std::abs(Longitude + 71.3033) < 1.0e-10,
        "Web Mercator round trip at Mount Washington");
    SkiDomain::WebMercatorTile Tile;
    Check(SkiDomain::TryWebMercatorTile(0.0, 180.0, 2, Tile)
        && Tile.X == 3 && Tile.Y == 2 && Tile.Zoom == 2,
        "positive antimeridian stays in last XYZ tile");
    Check(!SkiDomain::TryWebMercatorPixel(90.0, 0.0, 2, Pixel)
        && !SkiDomain::TryWebMercatorPixel(0.0,
            std::numeric_limits<double>::infinity(), 2, Pixel),
        "unprojectable map points are rejected");
    Check(SkiDomain::TryWebMercatorPixel(85.0511287798066, 0.0, 22, Pixel)
        && SkiDomain::TryGeodeticFromWebMercatorPixel(Pixel, 22, Latitude, Longitude)
        && std::abs(Latitude - 85.0511287798066) < 1.0e-10,
        "Web Mercator latitude limit remains reversible at zoom 22");

    SkiDomain::SiteRectangleM Rectangle;
    Check(SkiDomain::TryDrawSiteRectangle(0.0, 0.0, 3500.0, -2400.0, Rectangle)
        && Rectangle.WestM == 0.0 && Rectangle.EastM == 3500.0
        && Rectangle.SouthM == -2400.0 && Rectangle.NorthM == 0.0,
        "draw preserves free aspect and north-up bounds");
    Check(SkiDomain::TryDrawSiteRectangle(0.0, 0.0, -10.0, 9000.0, Rectangle)
        && Rectangle.WestM == -2000.0 && Rectangle.EastM == 0.0
        && Rectangle.SouthM == 0.0 && Rectangle.NorthM == 4000.0,
        "draw clamps each side independently to 2-4 km");
    Check(SkiDomain::TryResizeSiteRectangle(Rectangle,
            SkiDomain::SiteResizeHandle::NorthWest, -9000.0, 2500.0, Rectangle)
        && Rectangle.EastM == 0.0 && Rectangle.SouthM == 0.0
        && Rectangle.WestM == -4000.0 && Rectangle.NorthM == 2500.0,
        "corner resize fixes opposite corner and preserves free aspect");
    SkiDomain::SiteRectangleM Moved;
    Check(SkiDomain::TryMoveSiteRectangle(Rectangle, 500.0, -100.0, Moved)
        && Moved.WestM == -3500.0 && Moved.EastM == 500.0
        && Moved.SouthM == -100.0 && Moved.NorthM == 2400.0,
        "move preserves dimensions");
    Check(!SkiDomain::TryDrawSiteRectangle(0.0, 0.0, 1000.0, 1000.0,
            Moved, 1500.0)
        && !SkiDomain::IsValidSiteRectangle({0.0, 0.0, 1000.0, 3000.0}),
        "invalid ceiling and undersized side are rejected");
    Check(!SkiDomain::TryResizeSiteRectangle(Rectangle,
            static_cast<SkiDomain::SiteResizeHandle>(255), 0.0, 0.0, Moved),
        "unknown resize handle is rejected");
    std::cout << "SKI_TEST_RESULT {\"total\":" << Total << ",\"passed\":"
        << Total - Failed << ",\"failed\":" << Failed << "}\n";
    return Failed ? 1 : 0;
}
