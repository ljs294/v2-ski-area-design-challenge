#include "SkiDomain/PlaceCoordinates.h"
#include "SkiDomain/Coordinates.h"

#include <cassert>
#include <cmath>

int main()
{
    SkiDomain::PlaceCoordinates Point;
    assert(SkiDomain::TryParsePlaceCoordinates("44.2706, -71.3033", Point));
    assert(std::abs(Point.LatitudeDeg - 44.2706) < 1e-9);
    assert(std::abs(Point.LongitudeDeg + 71.3033) < 1e-9);
    assert(SkiDomain::TryParsePlaceCoordinates("44d 16' 14.16\" N, 71d 18' 11.88\" W", Point));
    assert(std::abs(Point.LatitudeDeg - 44.2706) < 1e-9);
    assert(std::abs(Point.LongitudeDeg + 71.3033) < 1e-9);
    assert(SkiDomain::TryParsePlaceCoordinates("0, 180", Point));
    const auto LastGood = Point;
    assert(!SkiDomain::TryParsePlaceCoordinates("91, 0", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("45, -181", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("44, -71, 4", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("44 N, 71", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("44 60 0 N, 71 0 0 W", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("44 0 60 N, 71 0 0 W", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("nan, 0", Point));
    assert(!SkiDomain::TryParsePlaceCoordinates("44x, -71", Point));
    assert(Point.LatitudeDeg == LastGood.LatitudeDeg
        && Point.LongitudeDeg == LastGood.LongitudeDeg);

    // M2: a 2.2 km rectangle centered on the antimeridian has corners on both
    // canonical sides of the seam, while its local ENU dimensions remain small.
    SkiDomain::LocalFrame AntimeridianFrame;
    assert(SkiDomain::TryMakeLocalFrame({0.0, 180.0, 0.0}, AntimeridianFrame));
    SkiDomain::GeodeticPoint WestCorner, EastCorner;
    assert(SkiDomain::TrySeaLevelGeodeticFromEnu(AntimeridianFrame,
        -1113.2, -1000.0, WestCorner));
    assert(SkiDomain::TrySeaLevelGeodeticFromEnu(AntimeridianFrame,
        1113.2, 1000.0, EastCorner));
    assert(SkiDomain::IsValidGeodetic(WestCorner)
        && SkiDomain::IsValidGeodetic(EastCorner));
    assert(std::abs(WestCorner.LongitudeDeg - 179.99) < 1.0e-5
        && std::abs(EastCorner.LongitudeDeg + 179.99) < 1.0e-5
        && WestCorner.LongitudeDeg > 0.0 && EastCorner.LongitudeDeg < 0.0);
    const SkiDomain::EnuPoint WestCheck = SkiDomain::ToEnu(AntimeridianFrame, WestCorner);
    const SkiDomain::EnuPoint EastCheck = SkiDomain::ToEnu(AntimeridianFrame, EastCorner);
    assert(std::abs(WestCheck.EastM + 1113.2) < 0.01
        && std::abs(WestCheck.NorthM + 1000.0) < 0.01
        && std::abs(EastCheck.EastM - 1113.2) < 0.01
        && std::abs(EastCheck.NorthM - 1000.0) < 0.01);

    SkiDomain::GeodeticPoint InvalidLatitudeFramePoint{90.01, 180.0, 0.0};
    assert(!SkiDomain::TryMakeLocalFrame(InvalidLatitudeFramePoint, AntimeridianFrame));
}
