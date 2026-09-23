#pragma once

#include "SkiDomain/Export.h"
#include "SkiDomain/Coordinates.h"
#include "SkiDomain/Revision.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SkiDomain
{
constexpr std::uint32_t TerrainCoreSchema = 2;
constexpr std::uint32_t TerrainCoreTileSamples = 256;
constexpr std::uint32_t TerrainCoreTileCells = TerrainCoreTileSamples - 1;
constexpr std::uint32_t TerrainCoreNormalHaloSamples = 1;
constexpr std::array<std::uint32_t, 5> TerrainCoreLodFactors{1, 2, 4, 8, 16};
constexpr std::uint64_t TerrainCoreMaxSamples = 100'020'001ULL;
constexpr std::size_t TerrainCoreMaxTiles = 262'144;
constexpr std::size_t TerrainCoreMaxShards = 64;
constexpr std::size_t TerrainCoreMaxEdits = 4'000'000;
constexpr std::size_t TerrainCoreMaxProcessingVersions = 64;
constexpr std::size_t TerrainCoreMaxAdditionalSources = 64;
constexpr std::uint64_t TerrainCoreMaxManifestBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t TerrainCoreMaxAssetBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t TerrainCoreMaxInstalledBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t TerrainCoreMaxStoredSamples =
    static_cast<std::uint64_t>(TerrainCoreTileSamples + 2U)
    * static_cast<std::uint64_t>(TerrainCoreTileSamples + 2U);
constexpr std::uint64_t TerrainCoreDeflateBound(const std::uint64_t RawBytes) noexcept
{
    // zlib's codec-specific compressBound() formula. Keeping it here lets the
    // dependency-neutral validator reject hostile declarations before allocation.
    return RawBytes + (RawBytes >> 12U) + (RawBytes >> 14U)
        + (RawBytes >> 25U) + 13ULL;
}
constexpr std::uint64_t TerrainCoreMaxEncodedTileBytes =
    TerrainCoreDeflateBound(TerrainCoreMaxStoredSamples * sizeof(float))
    + TerrainCoreDeflateBound((TerrainCoreMaxStoredSamples + 7ULL) / 8ULL);

enum class PixelRegistration : std::uint8_t
{
    SampleCenter,
    CellArea,
};

enum class TerrainCoreError : std::uint8_t
{
    None,
    UnsupportedSchema,
    ManifestTooLarge,
    InvalidContentId,
    InvalidDimensions,
    InvalidBounds,
    InvalidSpacing,
    InvalidMetadata,
    MissingLod,
    InvalidTile,
    TileCoverage,
    InvalidAssetPath,
    DuplicateAssetPath,
    InvalidAssetHash,
    AssetTooLarge,
    PackageTooLarge,
    TooManyTiles,
    TooManyShards,
    InvalidShard,
    ShardCoverage,
    InvalidEdit,
    DuplicateEdit,
};

struct MetricBounds
{
    double WestM = 0.0;
    double SouthM = 0.0;
    double EastM = 0.0;
    double NorthM = 0.0;
};

struct TerrainCoreSource
{
    std::string SourceId;
    std::string Product;
    std::string AcquisitionEpoch;
    std::string HorizontalCrs;
    std::string HorizontalDatum;
    std::string VerticalDatum;
    std::string License;
    std::string Attribution;
    double NativeEastSpacingM = 0.0;
    double NativeNorthSpacingM = 0.0;
    bool NativeSpacingReported = true;
    double HorizontalAccuracyM = 0.0;
    double VerticalAccuracyM = 0.0;
    bool HasHorizontalAccuracy = false;
    bool HasVerticalAccuracy = false;
};

struct TerrainCoreShardDescriptor
{
    std::string Path;
    std::string Sha256;
    std::uint64_t Bytes = 0;
};

struct TerrainCoreTileDescriptor
{
    std::uint8_t LodIndex = 0;
    std::uint32_t LodFactor = 1;
    std::uint32_t TileX = 0;
    std::uint32_t TileY = 0;
    std::uint32_t StartColumn = 0;
    std::uint32_t StartRow = 0;
    std::uint16_t CoreWidth = 0;
    std::uint16_t CoreHeight = 0;
    std::uint8_t HaloWest = 0;
    std::uint8_t HaloNorth = 0;
    std::uint8_t HaloEast = 0;
    std::uint8_t HaloSouth = 0;
    std::string HeightPath;
    std::string HeightSha256;
    std::uint32_t HeightShardIndex = 0;
    std::uint64_t HeightOffset = 0;
    std::uint64_t HeightBytes = 0;
    std::uint64_t HeightRawBytes = 0;
    std::string ValidityPath;
    std::string ValiditySha256;
    std::uint32_t ValidityShardIndex = 0;
    std::uint64_t ValidityOffset = 0;
    std::uint64_t ValidityBytes = 0;
    std::uint64_t ValidityRawBytes = 0;
    std::string ProvenanceId;
    std::string ProcessingVersion;
};

struct TerrainCoreManifest
{
    std::uint32_t SchemaVersion = TerrainCoreSchema;
    std::string ContentId;
    std::string GeneratorVersion;
    std::vector<std::string> ProcessingVersions;
    TerrainCoreSource Source;
    std::vector<TerrainCoreSource> AdditionalSources;
    /** WGS84 geodetic origin of the manifest's local ENU metric coordinates. */
    GeodeticPoint LocalOrigin;
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    double DeliveredEastSpacingM = 0.0;
    double DeliveredNorthSpacingM = 0.0;
    PixelRegistration Registration = PixelRegistration::SampleCenter;
    MetricBounds SampleCenterBounds;
    MetricBounds OuterBounds;
    std::string RowOrientation = "north-to-south";
    std::string HeightEncoding = "f32le-deflate-fixed-v1";
    std::string ValidityEncoding = "bitset-deflate-fixed-v1";
    std::vector<TerrainCoreShardDescriptor> Shards;
    std::vector<TerrainCoreTileDescriptor> Tiles;
};

struct TerrainCoreValidation
{
    TerrainCoreError Error = TerrainCoreError::None;
    std::size_t Index = 0;
    bool Ok() const noexcept { return Error == TerrainCoreError::None; }
};

struct TerrainCoreTilePlan
{
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::vector<TerrainCoreTileDescriptor> Tiles;
};

struct TerrainEditDelta
{
    std::uint32_t Column = 0;
    std::uint32_t Row = 0;
    float DeltaM = 0.0F;
};

struct TerrainEditSet
{
    std::uint32_t SchemaVersion = 1;
    std::string TerrainCoreId;
    Revision BaseRevision = 0;
    Revision EditRevision = 0;
    std::vector<TerrainEditDelta> Deltas;
};

struct TerrainChangeReceipt
{
    std::string TerrainCoreId;
    Revision BeforeRevision = 0;
    Revision AfterRevision = 0;
    std::uint32_t MinColumn = 0;
    std::uint32_t MinRow = 0;
    std::uint32_t MaxColumn = 0;
    std::uint32_t MaxRow = 0;
    std::vector<std::uint64_t> AffectedTileIds;
};

struct LodHysteresisPolicy
{
    double RefineAbovePixels = 2.0;
    double CoarsenBelowPixels = 0.75;
    std::uint8_t MaximumLod = 4;
};

SKI_DOMAIN_API bool IsSafeTerrainCorePath(const std::string& Value) noexcept;
SKI_DOMAIN_API bool IsTerrainCoreSha256(const std::string& Value) noexcept;
SKI_DOMAIN_API bool ComputeTerrainCoreBounds(std::uint32_t Width, std::uint32_t Height,
    double EastSpacingM, double NorthSpacingM, const MetricBounds& SampleCenters,
    MetricBounds& OutOuterBounds) noexcept;
SKI_DOMAIN_API bool PlanTerrainCoreTiles(std::uint32_t Width, std::uint32_t Height,
    TerrainCoreTilePlan& OutPlan) noexcept;
SKI_DOMAIN_API TerrainCoreValidation ValidateTerrainCore(
    const TerrainCoreManifest& Manifest, std::uint64_t SerializedManifestBytes = 0) noexcept;
SKI_DOMAIN_API TerrainCoreValidation ValidateTerrainEditSet(const TerrainEditSet& Edits,
    std::uint32_t Width, std::uint32_t Height) noexcept;
SKI_DOMAIN_API bool BuildTerrainChangeReceipt(const TerrainEditSet& Before,
    const TerrainEditSet& After, std::uint32_t Width, std::uint32_t Height,
    TerrainChangeReceipt& OutReceipt) noexcept;
SKI_DOMAIN_API std::uint8_t SelectTerrainCoreLod(double FinestSamplePixels,
    std::uint8_t CurrentLod, const LodHysteresisPolicy& Policy = {}) noexcept;
SKI_DOMAIN_API bool AreTerrainCoreLodsAdjacent(std::uint8_t First,
    std::uint8_t Second) noexcept;
}
