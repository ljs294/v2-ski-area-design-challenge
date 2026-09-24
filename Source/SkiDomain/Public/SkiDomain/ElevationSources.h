#pragma once

#include "SkiDomain/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkiDomain
{
enum class ElevationProduct : std::uint8_t { S1M, Project1m, ArcSec13 };

/** Catalog facts only; the adapter must prove each support flag from authoritative metadata. */
struct ElevationSourceCandidate
{
    ElevationProduct Product = ElevationProduct::ArcSec13;
    std::string SourceId;
    bool SupportedHorizontalCrs = false;
    bool Navd88Proven = false;
    bool SupportedEncoding = false;
    /** USGS QL: lower positive values are better; zero means unreported. */
    std::uint8_t QualityLevel = 0;
    /** ISO calendar dates compare lexicographically; empty means unreported. */
    std::string CollectionEndDate;
    std::string PublicationDate;
};

/** S1M first, then proven Project 1 m, then 1/3 arcsecond; stable within a tier. */
SKI_DOMAIN_API bool ResolveElevationSources(
    const std::vector<ElevationSourceCandidate>& Candidates,
    std::vector<ElevationSourceCandidate>& OutSources);
}
