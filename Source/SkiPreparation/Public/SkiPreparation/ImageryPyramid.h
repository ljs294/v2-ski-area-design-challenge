#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainCore.h"

#include <cstdint>
#include <string>

namespace SkiPreparation
{
class Cancellation;

constexpr std::uint32_t ImageryPyramidSchema = 1;
constexpr std::uint16_t ImageryPyramidTilePixels = 256;
constexpr std::uint64_t ImageryPyramidMaxTileBytes = 16ULL * 1024ULL * 1024ULL;

enum class ImageryPyramidError : std::uint8_t
{
    None,
    Cancelled,
    InvalidTerrainCoreGeometry,
    InvalidSourceMetadata,
    UnsupportedSchema,
    UnsupportedEncoding,
    UnsupportedCoordinateFrame,
    TerrainCoreMismatch,
    TooManyTiles,
    MissingTile,
    DuplicateTile,
    MisalignedTile,
    TileOverlap,
    InvalidAssetPath,
    DuplicateAssetPath,
    InvalidAssetHash,
    InvalidAssetSize,
    PackageTooLarge,
    AssetInspectionFailed,
    AssetEncodingMismatch,
    AssetDimensionsMismatch,
    AssetSizeMismatch,
    AssetHashMismatch,
};

enum class ImageryStorageSizeCertainty : std::uint8_t
{
    Unknown,
    Estimated,
    Exact,
};

/** Attribution facts retained with every installed imagery pyramid. */
struct SKIPREPARATION_API ImagerySourceMetadata
{
    std::string SourceId;
    std::string Product;
    std::string ServiceUrlTemplate;
    std::string License;
    std::string Attribution;
    std::string TermsUrl;
};

/** One required image payload for a TerrainCore tile at the same LOD and grid key. */
struct SKIPREPARATION_API ImageryPyramidTile
{
    std::uint8_t LodIndex = 0;
    std::uint32_t LodFactor = 1;
    std::uint32_t TileX = 0;
    std::uint32_t TileY = 0;
    std::uint32_t StartColumn = 0;
    std::uint32_t StartRow = 0;
    std::uint16_t TerrainCoreWidth = 0;
    std::uint16_t TerrainCoreHeight = 0;
    std::uint16_t RasterWidth = ImageryPyramidTilePixels;
    std::uint16_t RasterHeight = ImageryPyramidTilePixels;
    double MetersPerPixelEast = 0.0;
    double MetersPerPixelNorth = 0.0;
    /** Full 256x256 affine grid; edge padding may extend beyond the TerrainCore extent. */
    SkiDomain::MetricBounds RasterSampleCenterBounds;
    /** Clipped TerrainCore sample-center extent. Adjacent tiles share only their seam samples. */
    SkiDomain::MetricBounds SampleCenterBounds;
    /** Canonical relative JPEG path; hash and size are filled after the image is written. */
    std::string Path;
    std::string Sha256;
    std::uint64_t Bytes = 0;
};

/** Geometry identity copied from TerrainCore so the imagery cannot drift to another grid. */
struct SKIPREPARATION_API ImageryPyramidManifest
{
    std::uint32_t SchemaVersion = ImageryPyramidSchema;
    std::string TileEncoding = "jpeg-rgb8-v1";
    std::string CoordinateFrame = "terraincore-local-enu-v1";
    std::string TerrainCoreId;
    SkiDomain::GeodeticPoint LocalOrigin;
    std::uint32_t TerrainCoreWidth = 0;
    std::uint32_t TerrainCoreHeight = 0;
    double TerrainCoreEastSpacingM = 0.0;
    double TerrainCoreNorthSpacingM = 0.0;
    SkiDomain::MetricBounds SampleCenterBounds;
    SkiDomain::MetricBounds OuterBounds;
    ImagerySourceMetadata Source;
    TArray<ImageryPyramidTile> Tiles;
};

struct SKIPREPARATION_API ImageryPyramidValidation
{
    ImageryPyramidError Error = ImageryPyramidError::None;
    int32 TileIndex = INDEX_NONE;
    bool Ok() const noexcept { return Error == ImageryPyramidError::None; }
};

/** A storage figure always states whether it is a range or verified exact bytes. */
struct SKIPREPARATION_API ImageryStorageEstimate
{
    std::uint64_t MinimumBytes = 0;
    std::uint64_t MaximumBytes = 0;
    ImageryStorageSizeCertainty Certainty = ImageryStorageSizeCertainty::Unknown;
    FString Basis;
    bool IsExact() const noexcept
    {
        return Certainty == ImageryStorageSizeCertainty::Exact
            && MinimumBytes == MaximumBytes;
    }
};

struct SKIPREPARATION_API ImageryPyramidVerificationReport
{
    ImageryPyramidError Error = ImageryPyramidError::None;
    int32 TileIndex = INDEX_NONE;
    int32 VerifiedTiles = 0;
    std::uint64_t VerifiedBytes = 0;
    FString FailureDetail;
    ImageryStorageEstimate Storage;
    bool Ok() const noexcept { return Error == ImageryPyramidError::None; }
};

struct SKIPREPARATION_API ImageryPyramidAssetFacts
{
    std::string Encoding;
    std::uint16_t Width = 0;
    std::uint16_t Height = 0;
    std::uint64_t Bytes = 0;
    std::string Sha256;
};

/**
 * Installer/storage adapter seam. Implementations must inspect the exact stored bytes,
 * compute SHA-256, and honor cancellation while doing so. No network access belongs here.
 */
class SKIPREPARATION_API IImageryPyramidAssetReader
{
public:
    virtual ~IImageryPyramidAssetReader() = default;
    virtual bool Inspect(const std::string& RelativePath, std::uint64_t MaximumBytes,
        const Cancellation& CancellationValue, ImageryPyramidAssetFacts& OutFacts,
        FString& OutError) = 0;
};

/** Builds the complete required tile set from the canonical TerrainCore tile plan. */
SKIPREPARATION_API ImageryPyramidValidation BuildRequiredImageryPyramid(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const ImagerySourceMetadata& Source,
    ImageryPyramidManifest& OutManifest, const Cancellation* CancellationValue = nullptr);

/** Checks exact grid binding, canonical tile keys, one-sample seams, and bounded topology. */
SKIPREPARATION_API ImageryPyramidValidation ValidateImageryPyramidLayout(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest);

/** Checks layout and all declared paths, hashes, and exact positive file sizes. */
SKIPREPARATION_API ImageryPyramidValidation ValidateImageryPyramidManifest(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest);

/** Produces a preview interval from explicit per-tile bounds; no codec size is guessed here. */
SKIPREPARATION_API bool EstimateImageryPyramidStorage(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest,
    std::uint64_t MinimumBytesPerTile, std::uint64_t MaximumBytesPerTile,
    ImageryStorageEstimate& OutEstimate);

/** Reads every required asset through the adapter and publishes exact totals only on success. */
SKIPREPARATION_API bool VerifyImageryPyramid(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest,
    IImageryPyramidAssetReader& AssetReader, const Cancellation& CancellationValue,
    ImageryPyramidVerificationReport& OutReport);
}
