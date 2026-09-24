#pragma once

#include "SkiDomain/Export.h"

#include <string_view>

namespace SkiDomain
{
struct PlaceCoordinates
{
    double LatitudeDeg = 0.0;
    double LongitudeDeg = 0.0;
};

/** Parses a lat,lon decimal pair or a pair of DMS components ending N/S and E/W. */
SKI_DOMAIN_API bool TryParsePlaceCoordinates(std::string_view Text,
    PlaceCoordinates& OutCoordinates) noexcept;
}
