#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkiPreparation
{
class Cancellation;

constexpr std::uint32_t OsmVectorPackageSchema = 2;
constexpr std::uint32_t OsmVectorPackageLegacySchema = 1;
constexpr std::uint64_t OsmVectorPackageMaxBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t OsmVectorPackageMaxFeatures = 100'000ULL;
constexpr std::uint64_t OsmVectorPackageMaxPoints = 500'000ULL;
constexpr std::uint32_t OsmVectorFeatureMaxPoints = 4'096U;
constexpr const char* OsmVectorCoordinateFrame = "terraincore-local-enu-v1";
constexpr const char* OsmVectorEncoding = "osm-enu-polylines-v2";
constexpr const char* OsmVectorLegacyEncoding = "osm-enu-polylines-v1";
constexpr const char* OsmVectorOsmLicense = "ODbL-1.0";
constexpr const char* OsmVectorAttribution = "© OpenStreetMap contributors";
constexpr const char* OsmVectorAttributionUrl = "https://www.openstreetmap.org/copyright";

enum class OsmVectorLayerKind : std::uint8_t
{
    Road,
    Lift,
    Trail,
};

enum class OsmElementKind : std::uint8_t
{
    Way,
    Relation,
};

/** Normalized display layers and their source OSM tag selectors. */
SKIPREPARATION_API const char* OsmVectorLayerName(OsmVectorLayerKind Layer) noexcept;
SKIPREPARATION_API const char* OsmVectorLayerSelector(OsmVectorLayerKind Layer) noexcept;

struct SKIPREPARATION_API OsmVectorPoint
{
    double EastM = 0.0;
    double NorthM = 0.0;
};

/** OSM identifiers are namespaced by element kind; IDs are serialized as decimal strings. */
struct SKIPREPARATION_API OsmVectorFeature
{
    OsmElementKind ElementKind = OsmElementKind::Way;
    std::uint64_t OsmId = 0;
    /** Zero-based, deterministic order when one OSM element yields multiple line parts. */
    std::uint32_t PartIndex = 0;
    std::vector<OsmVectorPoint> Points;
};

struct SKIPREPARATION_API OsmVectorLayer
{
    OsmVectorLayerKind Kind = OsmVectorLayerKind::Road;
    std::vector<OsmVectorFeature> Features;
};

struct SKIPREPARATION_API OsmVectorSourceProvenance
{
    std::string Provider = "openstreetmap-overpass";
    std::string Endpoint = "https://overpass-api.de/api/interpreter";
    /** Overpass `osm3s.timestamp_osm_base`, normalized to an ISO-8601 UTC instant. */
    std::string SourceTimestampUtc;
    /** Time at which the adapter received the response, also ISO-8601 UTC. */
    std::string RetrievedAtUtc;
    std::string License = OsmVectorOsmLicense;
    std::string Attribution = OsmVectorAttribution;
    std::string AttributionUrl = OsmVectorAttributionUrl;
};

/** Immutable OSM vector component bound to one verified TerrainCore ENU extent. */
struct SKIPREPARATION_API OsmVectorPackage
{
    std::uint32_t SchemaVersion = OsmVectorPackageSchema;
    std::string ContentId;
    std::string TerrainCoreId;
    SkiDomain::GeodeticPoint LocalOrigin;
    std::uint32_t TerrainCoreWidth = 0;
    std::uint32_t TerrainCoreHeight = 0;
    double TerrainCoreEastSpacingM = 0.0;
    double TerrainCoreNorthSpacingM = 0.0;
    /** Exact TerrainCore OuterBounds; OSM geometry may not escape these edges. */
    SkiDomain::MetricBounds ExtentM;
    OsmVectorSourceProvenance Source;
    /** Must contain each of Road, Lift and Trail exactly once. Empty layers are explicit. */
    std::vector<OsmVectorLayer> Layers;
};

enum class OsmVectorPackageError : std::uint8_t
{
    None,
    Cancelled,
    UnsupportedSchema,
    PackageTooLarge,
    InvalidTerrainCore,
    TerrainCoreMismatch,
    InvalidMetadata,
    InvalidSourceTimestamp,
    InvalidLicense,
    InvalidAttribution,
    MissingLayer,
    DuplicateLayer,
    InvalidLayer,
    TooManyFeatures,
    TooManyPoints,
    DuplicateFeatureId,
    InvalidFeatureId,
    MalformedGeometry,
    OutOfBounds,
    InvalidContentId,
    ContentHashMismatch,
    InvalidSerializedPackage,
    InvalidPartIndex,
};

struct SKIPREPARATION_API OsmVectorPackageValidation
{
    OsmVectorPackageError Error = OsmVectorPackageError::None;
    int32 LayerIndex = INDEX_NONE;
    int32 FeatureIndex = INDEX_NONE;
    int32 PointIndex = INDEX_NONE;
    bool Ok() const noexcept { return Error == OsmVectorPackageError::None; }
};

enum class OsmVectorStorageCertainty : std::uint8_t
{
    Estimated,
    Exact,
};

/** Byte figures refer only to the canonical UTF-8 vector package asset. */
struct SKIPREPARATION_API OsmVectorStorageEstimate
{
    std::uint64_t MinimumBytes = 0;
    std::uint64_t MaximumBytes = 0;
    OsmVectorStorageCertainty Certainty = OsmVectorStorageCertainty::Estimated;
    FString Basis;
    bool IsExact() const noexcept
    {
        return Certainty == OsmVectorStorageCertainty::Exact
            && MinimumBytes == MaximumBytes;
    }
};

struct SKIPREPARATION_API OsmVectorPackageVerificationReport
{
    OsmVectorPackageError Error = OsmVectorPackageError::None;
    std::uint64_t FeatureCount = 0;
    std::uint64_t PointCount = 0;
    OsmVectorStorageEstimate Storage;
    FString FailureDetail;
    bool Ok() const noexcept { return Error == OsmVectorPackageError::None; }
};

/** Request passed to the sole adapter seam. Its extent is in TerrainCore local ENU. */
struct SKIPREPARATION_API OsmVectorProviderQuery
{
    std::string TerrainCoreId;
    SkiDomain::GeodeticPoint LocalOrigin;
    SkiDomain::MetricBounds ExtentM;
    std::vector<std::string> RequiredSelectors;
};

struct SKIPREPARATION_API OsmVectorProviderResponse
{
    OsmVectorSourceProvenance Source;
    std::vector<OsmVectorLayer> Layers;
};

/**
 * Acquisition seam for one logical Overpass query. Implementations must route all network
 * access through SkiNetGateway, transform/clamp features into the supplied ENU extent,
 * preserve OSM way/relation IDs, and stop promptly when Cancellation is set. No live
 * provider is implemented in this model module.
 */
class SKIPREPARATION_API IOsmVectorProvider
{
public:
    virtual ~IOsmVectorProvider() = default;
    virtual bool Acquire(const OsmVectorProviderQuery& Query,
        const Cancellation& CancellationValue, OsmVectorProviderResponse& OutResponse,
        FString& OutError) = 0;
};

SKIPREPARATION_API OsmVectorPackageValidation ValidateOsmVectorPackage(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation* CancellationValue = nullptr) noexcept;

/** Canonical JSON serialization with fixed field order and sorted layers/features. */
SKIPREPARATION_API FString SerializeOsmVectorPackage(
    const OsmVectorPackage& Package, bool IncludeContentId);
SKIPREPARATION_API FString ComputeOsmVectorPackageContentId(const OsmVectorPackage& Package);
SKIPREPARATION_API bool ParseOsmVectorPackage(const FString& Json,
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    OsmVectorPackage& OutPackage, FString& OutError,
    const Cancellation* CancellationValue = nullptr);

/** Calls the provider once, binds its response to TerrainCore, hashes, then validates. */
SKIPREPARATION_API bool AcquireOsmVectorPackage(IOsmVectorProvider& Provider,
    const SkiDomain::TerrainCoreManifest& TerrainCore, const Cancellation& CancellationValue,
    OsmVectorPackage& OutPackage, FString& OutError);

/** Preview interval for an expected count; does not claim exact storage. */
SKIPREPARATION_API bool EstimateOsmVectorStorage(
    std::uint64_t ExpectedFeatureCount, std::uint64_t ExpectedPointCount,
    OsmVectorStorageEstimate& OutEstimate) noexcept;

/** Verifies package identity and publishes exact canonical serialized byte count. */
SKIPREPARATION_API bool VerifyOsmVectorPackage(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const OsmVectorPackage& Package,
    const Cancellation& CancellationValue, OsmVectorPackageVerificationReport& OutReport);
}
