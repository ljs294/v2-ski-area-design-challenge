#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/ElevationSources.h"
#include "SkiDomain/TerrainQuality.h"
#include "SkiPreparation/CogPreflight.h"
#include "SkiPreparation/TerrainAcquisition.h"

namespace SkiPreparation
{
enum class ElevationCatalogStatus : uint8
{
    /** Catalog, COG, site coverage, source-lineage, and sampler-quality gates all pass. */
    Ready,
    /** TNM catalog and identity-bound COG resolver completed; full-download gates remain. */
    CatalogReady,
    UnsupportedGeography,
    NoCoverage,
    NoDatumProvenSource,
    CatalogFailure,
};

enum class StorageSizeCertainty : uint8 { Unknown, Estimated, Exact };
enum class ElevationProofOrigin : uint8 { None, TnmMetadata, CogHeader, XmlSidecar };

/** A byte count is always accompanied by the evidence that gives it that certainty. */
struct SKIPREPARATION_API ElevationStorageEstimate
{
    uint64 Bytes = 0;
    StorageSizeCertainty Certainty = StorageSizeCertainty::Unknown;
    FString Basis;
};

/** Populated only from an identified metadata source; the origin is retained for audit. */
struct SKIPREPARATION_API ElevationSourceProofs
{
    ElevationProofOrigin HorizontalCrsOrigin = ElevationProofOrigin::None;
    ElevationProofOrigin VerticalDatumOrigin = ElevationProofOrigin::None;
    ElevationProofOrigin EncodingOrigin = ElevationProofOrigin::None;
    ElevationProofOrigin ObjectSizeOrigin = ElevationProofOrigin::None;
    FString HorizontalCrs;
    FString VerticalDatum;
    FString SampleFormat;
    FString Compression;
    int32 BitsPerSample = 0;
    int32 Predictor = 0;
};

/** Normalized catalog facts. This is not a COG or source-lineage verification receipt. */
struct SKIPREPARATION_API ElevationCatalogSource
{
    SkiDomain::ElevationSourceCandidate Candidate;
    FString Title;
    FString DownloadUrl;
    FString MetadataUrl;
    FString ReportedHorizontalCrs;
    FString ReportedVerticalDatum;
    FString ReportedSampleFormat;
    FString ReportedCompression;
    ElevationSourceProofs Proofs;
    /** Stable fail-closed reason when this record was excluded from the resolver. */
    FString EligibilityReasonCode;
    /** The exact bounds whose TNM bbox query returned this catalog record. */
    SkiDomain::GeographicBounds CatalogQueryBounds;
    /** TNM bbox results indicate intersection candidates; raster extent is not compared here. */
    FString CoverageStatusCode = TEXT("CATALOG_COVERAGE_NOT_QUERIED");
    /** COG proof is retained beside the identity and URL it was fetched from. */
    FCogPreflightReport CogPreflight;
    FString CogStatusCode = TEXT("COG_HEADERS_NOT_READ");
    FString CogProofSourceId;
    FString CogProofDownloadUrl;
    uint64 VerifiedCogObjectBytes = 0;
    bool HasVerifiedCogHeader = false;
    uint64 ExactObjectBytes = 0;
    bool HasExactObjectBytes = false;
    bool HasValidatedDownloadUrl = false;

    /** Recomputes resolver flags only from identity-bound COG facts and explicit datum proofs. */
    void RefreshResolverEligibility();
};

struct SKIPREPARATION_API TerrainAvailabilityReport
{
    ElevationCatalogStatus Status = ElevationCatalogStatus::CatalogFailure;
    bool IsSupportedGeography = false;
    bool CatalogComplete = false;
    /** At least one product record was returned by TNM's bbox query; not a full raster coverage proof. */
    bool HasElevationCoverage = false;
    /** Full-download eligibility; requires COG, sidecar lineage, site coverage, and sampler quality proof. */
    bool DownloadEnabled = false;
    FString CoverageStatusCode = TEXT("SITE_COVERAGE_NOT_PROVEN");
    FString GeographyRegion;
    FString FailureCode;
    FString FailureDetail;

    /** All returned records with catalog-level normalized facts. */
    TArray<ElevationCatalogSource> Sources;
    /** The safe fallback chain returned by SkiDomain::ResolveElevationSources. */
    TArray<ElevationCatalogSource> ResolvedSources;

    /** Exact COG-cross-checked object sizes for resolved candidates, not a download plan. */
    ElevationStorageEstimate ResolvedCandidateObjectBytes;
    ElevationStorageEstimate CanonicalScratchBytes;

    /** True only after per-sample provenance has been sampled and quality tallied. */
    bool HasQualityReport = false;
    SkiDomain::TerrainQualityReport QualityReport;
    FString QualityStatusCode = TEXT("QUALITY_REQUIRES_SAMPLER_PROVENANCE");
    /** This bounded catalog adapter does not read the S1M GeoPackage/XML sidecars. */
    bool HasCompleteSourceLineage = false;
    FString LineageStatusCode = TEXT("LINEAGE_SIDECARS_NOT_READ");
    /** True only when every retained resolver source has an identity- and size-bound COG receipt. */
    bool HasVerifiedCogHeaders = false;
    FString CogStatusCode = TEXT("COG_HEADERS_NOT_READ");
    /** COG preflight validates georeferencing but does not compare the raster extent to the site. */
    bool HasVerifiedSiteCoverage = false;
};

/**
 * Normalized USGS elevation preflight backed by TNM Access. Inject the production
 * SkiNetGateway in application code; scripted transports can exercise it offline.
 */
class SKIPREPARATION_API ElevationCatalog final
{
public:
    explicit ElevationCatalog(IAcquisitionTransport& Transport);

    /**
     * Returns false only when a request or response is unusable. A successful report can
     * still disable download due to geography, coverage, or unproven source metadata.
     */
    bool Preflight(const SkiDomain::GeographicBounds& Bounds,
        const TSharedRef<Cancellation>& Cancellation,
        TerrainAvailabilityReport& OutReport) const;

    /** Quality is supplied later by the sampler after real LOD0 provenance is known. */
    static bool TryBuildQualityReport(const SkiDomain::TerrainProvenanceCounts& Counts,
        SkiDomain::TerrainQualityReport& OutReport) noexcept;

private:
    IAcquisitionTransport& Transport;
    mutable FCriticalSection CacheMutex;
    mutable TMap<FString, TerrainAvailabilityReport> QuantizedBoundsCache;
    mutable TArray<FString> QuantizedBoundsCacheOrder;
};
}
