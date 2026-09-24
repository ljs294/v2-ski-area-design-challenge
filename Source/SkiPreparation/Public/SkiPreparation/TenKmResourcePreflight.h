#pragma once

#include "CoreMinimal.h"
#include "SkiPreparation/ElevationCatalog.h"

namespace SkiPreparation
{
enum class ETenKmResourcePreflightStatus : uint8
{
    Ready,
    InvalidSiteSize,
    CoverageUnproven,
    RequiredSizeUnknown,
    InvalidEvidence,
    ArithmeticOverflow,
    ComponentLimitExceeded,
    InsufficientDisk,
};

enum class ETenKmStorageCertainty : uint8
{
    Unknown,
    Estimated,
    Exact,
};

/** Bytes required or reserved for a specific part of the acquisition/install ledger. */
struct SKIPREPARATION_API FTenKmByteEstimate
{
    uint64 Bytes = 0;
    ETenKmStorageCertainty Certainty = ETenKmStorageCertainty::Unknown;
    FString Basis;
};

struct SKIPREPARATION_API FTenKmResourceLedgerRow
{
    FString Component;
    FTenKmByteEstimate Download;
    FTenKmByteEstimate Stage;
    FTenKmByteEstimate Scratch;
    FTenKmByteEstimate Install;
};

struct SKIPREPARATION_API FTenKmMemoryPlan
{
    /** Application-owned preparation buffers; engine baseline and OS memory are excluded. */
    uint64 PreparationPeakBytes = 0;
    /** Existing runtime cache limit; worker reservations are included in this ceiling. */
    uint64 RuntimeTileCacheBudgetBytes = 0;
    uint64 RuntimeWorkerReservationBytes = 0;
    uint64 PeakAcrossSequentialPhasesBytes = 0;
    uint32 RuntimeMaximumWorkerJobs = 0;
    uint32 MaximumConcurrentAcquisitionRequests = 0;
    uint64 MaximumTerrainTilePayloadBytes = 0;
    uint64 MaximumTerrainPlanMetadataBytes = 0;
};

struct SKIPREPARATION_API FTenKmResumeStorageSemantics
{
    /** Partial component sets never become an installed-library entry. */
    bool bPartialCompositeActivationProhibited = true;
    /** The resumable workspace must retain only ETag-pinned, hash-validated chunks/checkpoints. */
    bool bValidatedChunksAndCheckpointsRequiredForResume = true;
    /** The current TerrainScratchStore removes its owned directory on cleanup/destruction. */
    bool bCanonicalScratchDurableResumeImplemented = false;
    bool bResumeStorageSemanticsQualified = false;
    uint64 RetainedWorkspaceUpperBoundBytes = 0;
    FString QualificationGap;
};

struct SKIPREPARATION_API FTenKmResourcePreflightRequest
{
    /** TerrainCore LOD0 sample dimensions after projection planning. */
    uint32 WidthSamples = 0;
    uint32 HeightSamples = 0;
    /** Exact WorldCover window dimensions and source object size from its COG header. */
    uint32 CoverWidth = 0;
    uint32 CoverHeight = 0;
    uint64 WorldCoverSourceObjectBytes = 0;
    StorageSizeCertainty WorldCoverSourceSizeCertainty = StorageSizeCertainty::Unknown;
    bool bWorldCoverCoverageVerified = false;

    /** Catalog/COG evidence must already prove source identity, lineage, quality and site coverage. */
    TerrainAvailabilityReport Elevation;
    /** Exact aggregate S1M GeoPackage/XML sidecar size; zero is valid when no S1M source is resolved. */
    uint64 ElevationSidecarObjectBytes = 0;
    StorageSizeCertainty ElevationSidecarSizeCertainty = StorageSizeCertainty::Unknown;

    /** Estimated encoded JPEG interval for each required TerrainCore-aligned image tile. */
    uint64 ImageryMinimumBytesPerTile = 0;
    uint64 ImageryMaximumBytesPerTile = 0;

    /** Expected OSM counts must come from a site-specific query/estimate, not a global default. */
    bool bVectorCountEstimateAvailable = false;
    uint64 ExpectedVectorFeatureCount = 0;
    uint64 ExpectedVectorPointCount = 0;

    /** Disk free space on the volume that will hold the workspace and installed package. */
    uint64 FreeDiskBytes = 0;
};

struct SKIPREPARATION_API FTenKmResourcePreflightReport
{
    ETenKmResourcePreflightStatus Status = ETenKmResourcePreflightStatus::InvalidEvidence;
    FString FailureCode;
    FString FailureDetail;
    TArray<FTenKmResourceLedgerRow> Ledger;
    FTenKmByteEstimate DownloadBytes;
    FTenKmByteEstimate StageBytes;
    FTenKmByteEstimate ScratchBytes;
    FTenKmByteEstimate InstallBytes;
    FTenKmByteEstimate RollbackReserveBytes;
    FTenKmByteEstimate RequiredFreeDiskBytes;
    uint64 AvailableFreeDiskBytes = 0;
    bool bHasEnoughFreeDisk = false;
    uint64 TerrainCoreTileCount = 0;
    uint64 ImageryPyramidTileCount = 0;
    bool bWithinCurrentImageryAcquisitionLimits = false;
    bool bTenKmQualified = false;
    FTenKmMemoryPlan Memory;
    FTenKmResumeStorageSemantics Resume;
};

/**
 * Builds a fail-closed 2–10 km storage and application-memory budget from the existing
 * TerrainCore, scratch, catalog, imagery, OSM and composite component contracts. The result
 * is a preflight estimate only; it does not qualify a live 10 km acquisition or install.
 */
SKIPREPARATION_API ::SkiPreparation::ETenKmResourcePreflightStatus PlanTenKmResourcePreflight(
    const FTenKmResourcePreflightRequest& Request,
    FTenKmResourcePreflightReport& OutReport);
}
