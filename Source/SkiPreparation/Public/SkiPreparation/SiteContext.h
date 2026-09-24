#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainCore.h"
#include "SkiDomain/TerrainQuality.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SkiPreparation
{
constexpr std::uint32_t SiteContextLegacySchema = 1;
constexpr std::uint32_t SiteContextSchema = 2;
constexpr std::uint32_t CompositeInstallReceiptSchema = 3;
constexpr const char* SiteContextVectorEncodingV1 = "osm-enu-polylines-json-v1";
constexpr const char* SiteContextVectorEncodingV2 = "osm-enu-polylines-json-v2";
constexpr std::uint32_t SiteContextImageryTilePixels = 256;
constexpr std::size_t SiteContextMaxTiles = SkiDomain::TerrainCoreMaxTiles;
constexpr std::uint64_t SiteContextMaxManifestBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t SiteContextMaxAssetBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t SiteContextMaxPackageBytes = SkiDomain::TerrainCoreMaxInstalledBytes;
constexpr std::uint64_t CompositeInstallReceiptMaxBytes = 1024ULL * 1024ULL;

struct SiteContextAsset
{
    std::string Path;
    std::string Type;
    std::string Sha256;
    std::uint64_t Length = 0;
};

struct SiteContextImageryTile
{
    std::uint8_t LodIndex = 0;
    std::uint32_t LodFactor = 1;
    std::uint32_t TileX = 0;
    std::uint32_t TileY = 0;
    std::uint32_t PixelWidth = SiteContextImageryTilePixels;
    std::uint32_t PixelHeight = SiteContextImageryTilePixels;
    double EastMetersPerPixel = 1.0;
    double NorthMetersPerPixel = 1.0;
    std::string AssetPath;
};

struct SiteContextAttribution
{
    std::string Provider;
    std::string License;
    std::string Text;
};

/** Verified OSM package lineage for a SiteContext schema-2 vector asset. */
struct SiteContextVectorSourceLineage
{
    std::string SourcePackageContentId;
    std::string TerrainCoreId;
    SkiDomain::MetricBounds ExtentM;
    std::string Provider;
    std::string Endpoint;
    std::string SourceTimestampUtc;
    std::string RetrievedAtUtc;
    std::string License;
    std::string Attribution;
    std::string AttributionUrl;
};

/**
 * Immutable SiteContext index. Imagery keys mirror every TerrainCore tile key,
 * and the vector asset stores OSM line features as JSON schema 1 or 2. Schema 1
 * implies a single part per (kind, osmId); schema 2 carries partIndex and requires
 * parts to be sorted and contiguous from zero for each element identity.
 */
struct SiteContextManifest
{
    std::uint32_t SchemaVersion = SiteContextSchema;
    std::string ContentId;
    std::string GeneratorVersion;
    std::string TerrainCoreId;
    std::uint32_t ImageryTilePixels = SiteContextImageryTilePixels;
    std::string ImageryEncoding = "jpeg-rgb8-v1";
    std::string ImageryFrame = "terraincore-local-enu-v1";
    std::string VectorEncoding = SiteContextVectorEncodingV2;
    std::string VectorAssetPath = "vectors/osm-enu-polylines.json";
    std::vector<SiteContextImageryTile> ImageryTiles;
    std::vector<SiteContextAsset> Assets;
    std::vector<SiteContextAttribution> Attributions;
    /** Required and content-addressed for schema 2; absent in legacy schema 1. */
    SiteContextVectorSourceLineage VectorSource;
};

enum class SiteContextError : std::uint8_t
{
    None,
    UnsupportedSchema,
    ManifestTooLarge,
    InvalidIdentity,
    InvalidMetadata,
    InvalidTerrainCore,
    InvalidPyramid,
    InvalidVectorSourceLineage,
    ImageryCoverage,
    InvalidAssetPath,
    DuplicateAssetPath,
    InvalidAssetHash,
    InvalidAssetLength,
    AssetTooLarge,
    PackageTooLarge,
    MissingVectors,
    MissingAttribution,
};

struct SiteContextValidation
{
    SiteContextError Error = SiteContextError::None;
    std::size_t Index = 0;
    bool Ok() const noexcept { return Error == SiteContextError::None; }
};

/** Reads one bounded asset at a time so the complete imagery pyramid stays disk-backed. */
using SiteContextAssetReader = TFunction<bool(
    const FString& RelativePath, TArray<uint8>& OutBytes, FString& OutError)>;

struct SKIPREPARATION_API SiteContextPackageIndex
{
    SiteContextManifest Manifest;
    SkiDomain::TerrainCoreManifest TerrainCore;
    FString PackageDirectory;
};

SKIPREPARATION_API SiteContextValidation ValidateSiteContextManifest(
    const SiteContextManifest& Manifest, const SkiDomain::TerrainCoreManifest& TerrainCore,
    std::uint64_t SerializedManifestBytes = 0) noexcept;

SKIPREPARATION_API FString SerializeSiteContextManifest(
    const SiteContextManifest& Manifest, bool IncludeContentId);
SKIPREPARATION_API bool ParseSiteContextManifest(const FString& Json,
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    SiteContextManifest& OutManifest, FString& OutError);

/**
 * Content-addressed component store. Callers must pass a TerrainCore manifest
 * that was reopened and verified by TerrainCorePackageStore before writing or
 * opening SiteContext against it. Composite activation remains the installer’s
 * responsibility through VerifyCompositeInstallReceiptForActivation.
 */
class SKIPREPARATION_API SiteContextStore
{
public:
    explicit SiteContextStore(FString InDataRoot);

    bool WriteAndActivate(SiteContextManifest Manifest,
        const SkiDomain::TerrainCoreManifest& VerifiedTerrainCore,
        const SiteContextAssetReader& ReadAsset, FString& OutPackageDirectory,
        SiteContextManifest& OutManifest, FString& OutError) const;
    bool Open(const FString& ContentId,
        const SkiDomain::TerrainCoreManifest& VerifiedTerrainCore,
        SiteContextPackageIndex& OutIndex, FString& OutError) const;
    bool Verify(const SiteContextPackageIndex& Index, FString& OutError) const;
    bool ReadAsset(const SiteContextPackageIndex& Index, const FString& RelativePath,
        TArray<uint8>& OutBytes, FString& OutError) const;

private:
    FString Root;
};

enum class CompositeInstallComponentKind : std::uint8_t
{
    TerrainCore,
    CoverEcology,
    SiteContext,
    QualityReport,
};

enum class CompositeInstallComponentStatus : std::uint8_t
{
    Missing,
    Staged,
    Verified,
    Failed,
};

struct CompositeInstallComponent
{
    CompositeInstallComponentKind Kind = CompositeInstallComponentKind::TerrainCore;
    CompositeInstallComponentStatus Status = CompositeInstallComponentStatus::Missing;
    /** Content identity of the immutable component or canonical report. */
    std::string ContentId;
    /** SHA-256 of the component's canonical manifest bytes. */
    std::string ManifestSha256;
};

/** Schema 3 is the first receipt shape that requires the complete M5 composite. */
struct CompositeInstallReceipt
{
    std::uint32_t SchemaVersion = CompositeInstallReceiptSchema;
    std::string ContentId;
    std::string GeneratorVersion;
    std::vector<CompositeInstallComponent> Components;
    SkiDomain::TerrainProvenanceCounts ProvenanceCounts;
    SkiDomain::TerrainQualityReport Quality;
};

enum class CompositeReceiptError : std::uint8_t
{
    None,
    UnsupportedSchema,
    ReceiptTooLarge,
    InvalidIdentity,
    InvalidMetadata,
    MissingComponent,
    DuplicateComponent,
    InvalidComponent,
    ComponentNotVerified,
    InvalidQuality,
};

struct CompositeReceiptValidation
{
    CompositeReceiptError Error = CompositeReceiptError::None;
    std::size_t Index = 0;
    bool Ok() const noexcept { return Error == CompositeReceiptError::None; }
};

using CompositeComponentVerifier = TFunction<bool(
    const CompositeInstallReceipt& Receipt, const CompositeInstallComponent& Component,
    FString& OutError)>;

SKIPREPARATION_API CompositeReceiptValidation ValidateCompositeInstallReceipt(
    const CompositeInstallReceipt& Receipt,
    std::uint64_t SerializedReceiptBytes = 0) noexcept;
SKIPREPARATION_API FString SerializeCompositeInstallReceipt(
    const CompositeInstallReceipt& Receipt, bool IncludeContentId);
SKIPREPARATION_API bool ParseCompositeInstallReceipt(const FString& Json,
    CompositeInstallReceipt& OutReceipt, FString& OutError);
SKIPREPARATION_API FString ComputeTerrainQualityReportId(
    const SkiDomain::TerrainProvenanceCounts& Counts,
    const SkiDomain::TerrainQualityReport& Report);
SKIPREPARATION_API FString ComputeCompositeInstallReceiptId(
    const CompositeInstallReceipt& Receipt);

/**
 * Final activation gate. It rechecks the report hash locally and calls the
 * supplied adapter for TerrainCore, CoverEcology and SiteContext byte/manifest
 * verification. The adapter receives the full receipt so it can verify
 * cross-component references such as SiteContext's TerrainCore identity. A
 * status field by itself never authorizes activation.
 */
SKIPREPARATION_API bool VerifyCompositeInstallReceiptForActivation(
    const CompositeInstallReceipt& Receipt,
    const CompositeComponentVerifier& VerifyComponent, FString& OutError);
}
