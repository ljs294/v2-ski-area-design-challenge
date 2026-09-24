#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/SiteContext.h"

#include <cstdint>

namespace SkiPreparation
{
constexpr std::uint32_t SiteContextVectorAssetMaxFeatures = 500'000U;
constexpr std::uint64_t SiteContextVectorAssetMaxPoints = 2'000'000ULL;
constexpr std::uint32_t SiteContextVectorAssetMaxPointsPerPolyline = 100'000U;
constexpr double SiteContextVectorAssetMaxCoordinateMagnitudeM = 100'000.0;

enum class ESiteContextVectorKind : std::uint8_t
{
    Road,
    Lift,
    Trail,
};

/** Horizontal coordinates are TerrainCore-local ENU metres. */
struct SKIPREPARATION_API FSiteContextVectorPoint
{
    double EastM = 0.0;
    double NorthM = 0.0;
};

/** One ordered, namespaced OSM element part. V1 identities are preserved verbatim. */
struct SKIPREPARATION_API FSiteContextVectorPolyline
{
    FString OsmId;
    ESiteContextVectorKind Kind = ESiteContextVectorKind::Road;
    std::uint32_t PartIndex = 0;
    TArray<FSiteContextVectorPoint> Points;
};

/** Fully parsed and validated vector asset. The reader publishes no partial result. */
struct SKIPREPARATION_API FSiteContextVectorAsset
{
    std::uint32_t SchemaVersion = 0;
    std::uint64_t PointCount = 0;
    TArray<FSiteContextVectorPolyline> Polylines;

    bool IsValid() const noexcept
    {
        return SchemaVersion == 1U || SchemaVersion == 2U;
    }
};

/**
 * Parses bytes returned by SiteContextStore::ReadAsset for the declared vector asset.
 * The caller supplies its already-open, verified manifest and TerrainCore. This function
 * performs no file access, hashing, or network access; it rechecks the flat JSON structure,
 * version, ordering, limits, and TerrainCore outer bounds before returning any geometry.
 */
SKIPREPARATION_API bool ParseSiteContextVectorAsset(
    const TArray<uint8>& Utf8JsonBytes, const SiteContextManifest& Manifest,
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    FSiteContextVectorAsset& OutAsset, FString& OutError);
}
