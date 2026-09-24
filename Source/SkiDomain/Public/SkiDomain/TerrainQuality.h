#pragma once

#include "SkiDomain/ElevationSources.h"
#include "SkiDomain/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkiDomain
{
/** Counts are taken from the final canonical LOD0 grid, after source fallback. */
struct TerrainProvenanceCounts
{
    std::uint64_t S1MNativeQualified = 0;
    std::uint64_t S1MNativeOther = 0;
    std::uint64_t S1MBlend = 0;
    std::uint64_t S1MBackfill = 0;
    std::uint64_t S1MInterpolated = 0;
    std::uint64_t Project1mQualified = 0;
    std::uint64_t Project1mOther = 0;
    std::uint64_t ArcSec13 = 0;
    std::uint64_t NoData = 0;
    /** Samples whose source lacks a documented quality level or datum budget. */
    std::uint64_t UnknownMetadata = 0;
};

enum class TerrainGrade : std::uint8_t { A, B, C, D };

enum class TerrainVerticalDatum : std::uint8_t { Unknown, NAVD88, Other };

/** Identifies the authoritative input that proved (or asserted) a source datum. */
enum class TerrainDatumProofOrigin : std::uint8_t
{
    Unknown,
    CogMetadata,
    GeoPackageSourceInputs,
    XmlSidecar,
    CatalogMetadata,
    VerifiedS1mLineage,
};

/** Normalized lineage facts for one resolved elevation source. */
struct TerrainQualitySourceFacts
{
    /** Stable catalog source ID or acquired product/tile ID. */
    std::string SourceId;
    ElevationProduct Product = ElevationProduct::ArcSec13;

    /** USGS quality level; zero is only valid when QualityLevelUnknown is true. */
    std::uint8_t QualityLevel = 0;
    bool QualityLevelUnknown = true;
    bool QualityLevelEstimated = false;

    /** ISO-8601 calendar dates (YYYY-MM-DD), inclusive. */
    std::string AcquisitionStartDate;
    std::string AcquisitionEndDate;
    bool AcquisitionDateRangeUnknown = true;
    bool AcquisitionDateRangeEstimated = false;

    double VerticalRmseMeters = 0.0;
    bool VerticalRmseUnknown = true;
    bool VerticalRmseEstimated = false;

    TerrainVerticalDatum VerticalDatum = TerrainVerticalDatum::Unknown;
    TerrainDatumProofOrigin DatumProofOrigin = TerrainDatumProofOrigin::Unknown;
    bool DatumUnknown = true;
    bool DatumEstimated = false;
    bool DatumProven = false;

    /** Share of all canonical LOD0 samples selected from this source. */
    double SampleFraction = 0.0;
    bool SampleFractionUnknown = true;
    bool SampleFractionEstimated = false;
};

/** Product fractions use TotalSamples as their denominator and exclude NoData. */
struct TerrainSourceMixFractions
{
    double S1M = 0.0;
    double Project1m = 0.0;
    double ArcSec13 = 0.0;
    bool Unknown = true;
    bool Estimated = false;
};

struct TerrainQualityReport
{
    TerrainGrade Grade = TerrainGrade::D;
    std::uint64_t TotalSamples = 0;
    double QualifiedLidarFraction = 0.0;
    double BlendInterpolatedFraction = 0.0;
    double BackfillFraction = 0.0;
    double CoarseFraction = 0.0;
    double NoDataFraction = 0.0;
    double UnknownMetadataFraction = 0.0;
    bool HasNoDataWarning = false;
    TerrainSourceMixFractions SourceMix;
    std::vector<TerrainQualitySourceFacts> Sources;
};

/** Rejects inconsistent or overflowing tallies; never infers accuracy from sample spacing. */
SKI_DOMAIN_API bool TrySummarizeTerrainQuality(const TerrainProvenanceCounts& Counts,
    TerrainQualityReport& OutReport) noexcept;

/** Validates normalized, per-source lineage facts; unknown values must be explicitly marked. */
SKI_DOMAIN_API bool TryValidateTerrainQualitySourceFacts(
    const std::vector<TerrainQualitySourceFacts>& Sources) noexcept;

/** Adds validated source facts to the aggregate report without changing its grade rules. */
SKI_DOMAIN_API bool TrySummarizeTerrainQuality(const TerrainProvenanceCounts& Counts,
    const std::vector<TerrainQualitySourceFacts>& Sources,
    TerrainQualityReport& OutReport);
}
