#pragma once

#include "SkiDomain/Export.h"
#include "SkiDomain/Revision.h"
#include "SkiDomain/TerrainPackage.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SkiDomain
{
constexpr std::uint32_t CoverEcologySchema = 1;
// Schema 2 adds the required surround TerrainCore component. Schema 1 receipts were never
// accepted and are rejected; the terrain must be re-prepared.
constexpr std::uint32_t InstalledTerrainSchema = 2;
constexpr std::uint64_t CoverEcologyMaxCells = 16'000'000ULL;
constexpr std::size_t CoverEcologyMaxAssets = 16;
constexpr std::size_t InstalledTerrainMaxOptionalSources = 32;
constexpr std::uint64_t CoverEcologyMaxManifestBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t InstalledTerrainMaxReceiptBytes = 256ULL * 1024ULL;
constexpr std::uint64_t CoverEcologyMaxAssetBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t CoverEcologyMaxPackageBytes = 1024ULL * 1024ULL * 1024ULL;

enum class CoverEcologyError : std::uint8_t
{
    None,
    UnsupportedSchema,
    ManifestTooLarge,
    InvalidContentId,
    InvalidDimensions,
    InvalidTransform,
    InvalidMetadata,
    TooManyAssets,
    InvalidAssetPath,
    DuplicateAssetPath,
    InvalidAssetHash,
    InvalidAssetLength,
    AssetTooLarge,
    PackageTooLarge,
    MissingRequiredChannel,
    DuplicateRequiredChannel,
    InvalidOptionalSource,
    DuplicateOptionalSource,
};

struct CoverEcologySource
{
    std::string SourceId;
    std::string Product;
    std::string AcquisitionEpoch;
    std::string Provenance;
    std::string License;
    std::string Attribution;
};

/**
 * Geographic transform for a north-to-south raster. Sample-center bounds and
 * outer pixel bounds are both explicit so consumers never have to guess pixel
 * registration.
 */
struct CoverEcologyGridTransform
{
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    double LongitudeStepDeg = 0.0;
    double LatitudeStepDeg = 0.0;
    GeographicBounds SampleCenterBounds;
    GeographicBounds OuterBounds;
    std::string HorizontalCrs = "EPSG:4326";
    std::string PixelRegistration = "sample-center";
    std::string RowOrientation = "north-to-south";
};

struct CoverEcologyAsset
{
    std::string Path;
    std::string Type;
    std::string Sha256;
    std::uint64_t Length = 0;
};

struct CoverEcologyManifest
{
    std::uint32_t SchemaVersion = CoverEcologySchema;
    std::string ContentId;
    std::string GeneratorVersion;
    Revision CoverRevision = 1;
    CoverEcologySource Source;
    CoverEcologyGridTransform Transform;
    std::string SemanticClassEncoding = "uint8-worldcover-class-v1";
    std::string ValidityEncoding = "bitset-lsb0-v1";
    std::vector<CoverEcologyAsset> Assets;
};

enum class OptionalSourceStatus : std::uint8_t
{
    Acquired,
    Unavailable,
    NotRequested,
    FailedOptional,
};

struct OptionalSourceOutcome
{
    std::string SourceId;
    std::string Product;
    OptionalSourceStatus Status = OptionalSourceStatus::NotRequested;
    /** A content-addressed artifact ID when Status is Acquired. */
    std::string ArtifactId;
    /** Stable safe reason code when Status is not Acquired. */
    std::string ReasonCode;
    std::string License;
    std::string Attribution;
};

struct InstalledTerrainReceipt
{
    std::uint32_t SchemaVersion = InstalledTerrainSchema;
    std::string ContentId;
    std::string GeneratorVersion;
    std::string TerrainCoreId;
    /** Required coarse surrounding elevation, stored as its own immutable TerrainCore. */
    std::string SurroundTerrainCoreId;
    std::string CoverEcologyId;
    std::vector<OptionalSourceOutcome> OptionalSources;
};

struct CoverEcologyValidation
{
    CoverEcologyError Error = CoverEcologyError::None;
    std::size_t Index = 0;
    bool Ok() const noexcept { return Error == CoverEcologyError::None; }
};

SKI_DOMAIN_API bool ComputeCoverEcologyOuterBounds(std::uint32_t Width,
    std::uint32_t Height, double LongitudeStepDeg, double LatitudeStepDeg,
    const GeographicBounds& SampleCenters, GeographicBounds& OutOuterBounds) noexcept;
/** Nearest categorical source pixel; false outside the grid or for an invalid cell. */
SKI_DOMAIN_API bool SampleCoverEcologyClass(const CoverEcologyGridTransform& Transform,
    const std::vector<std::uint8_t>& Classes,
    const std::vector<std::uint8_t>& PackedValidity,
    double LatitudeDeg, double LongitudeDeg, std::uint8_t& OutClass) noexcept;
SKI_DOMAIN_API CoverEcologyValidation ValidateCoverEcology(
    const CoverEcologyManifest& Manifest,
    std::uint64_t SerializedManifestBytes = 0) noexcept;
SKI_DOMAIN_API CoverEcologyValidation ValidateInstalledTerrainReceipt(
    const InstalledTerrainReceipt& Receipt,
    std::uint64_t SerializedReceiptBytes = 0) noexcept;
}
