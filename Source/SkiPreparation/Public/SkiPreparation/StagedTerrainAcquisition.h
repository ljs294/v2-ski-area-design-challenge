#pragma once

#include "CoreMinimal.h"

#include "SkiDomain/ElevationSources.h"
#include "SkiPreparation/CogTerrainSampler.h"
#include "SkiPreparation/SkiNetGateway.h"

namespace SkiPreparation
{
/** M4 is staging-only: this production-visible coordinator never installs or transitions. */
enum class EStagedTerrainStage : uint8
{
    Catalog,
    SourcePreflight,
    CanonicalLod0,
    CoarseLodCopy,
    WorldCover,
    TerrainCoreStaging,
    TerrainCoreVerification,
    Complete,
};

enum class EStagedTerrainJobState : uint8
{
    Ready,
    Running,
    Paused,
    Failed,
    Complete,
};

enum class EStagedTerrainStageOutcome : uint8
{
    Succeeded,
    Cancelled,
    RetryableFailure,
    FatalFailure,
};

/** Aggregate caps for the whole resumable M4 operation. */
struct SKIPREPARATION_API FStagedTerrainAcquisitionLimits
{
    uint64 MaxRequests = 131072;
    uint64 MaxTransferredBytes = 1024ULL * 1024ULL * 1024ULL;
    uint64 MaxResidentBytes = 128ULL * 1024ULL * 1024ULL;
    uint64 MaxScratchStorageBytes = TerrainScratchMaximumBytes;
    uint64 MaxStagedTerrainBytes = SkiDomain::TerrainCoreMaxInstalledBytes;
    FCogTerrainSamplerLimits Sampler;
};

struct SKIPREPARATION_API FStagedTerrainAcquisitionRequest
{
    FString JobId;
    SkiDomain::GeographicBounds Bounds;
    uint32 Width = 0;
    uint32 Height = 0;
    FStagedTerrainAcquisitionLimits Limits;
};

/** Catalog evidence is retained with the exact selected product and URL. */
struct SKIPREPARATION_API FStagedTerrainSelectedSource
{
    SkiDomain::ElevationSourceCandidate Candidate;
    FString DownloadUrl;
    FString HorizontalCrs;
    FString VerticalDatum;
    bool bSiteCoverageVerified = false;
    FString CoverageEvidenceId;
};

/** A strong object version pin. Every later observation of this object must match it. */
struct SKIPREPARATION_API FStagedTerrainObjectPin
{
    FString ProductCode;
    FString SourceId;
    FString Url;
    FString ETag;
    uint64 ObjectBytes = 0;
};

struct SKIPREPARATION_API FStagedTerrainCogObservation
{
    FString SourceId;
    FString Url;
    FString StrongETag;
    FCogPreflightReport Preflight;
};

/** The adapter's evidence for the stage it just ran. */
struct SKIPREPARATION_API FStagedTerrainStageProof
{
    bool bSupportedGeography = false;
    bool bSiteCoverageVerified = false;
    bool bCanonicalLod0IsOneMetre = false;
    bool bCanonicalLod0StoreSealed = false;
    uint64 CanonicalLod0Samples = 0;
    bool bCoarseLodsAreIndexedCopies = false;
    bool bCoarseLodsBitExactVerified = false;
    bool bWorldCoverWindowVerified = false;
    bool bTerrainCoreStagedOnly = false;
    bool bTerrainCoreVerificationPassed = false;
    bool bLibraryEntryWritten = false;
    bool bMountainTransitioned = false;
};

struct SKIPREPARATION_API FStagedTerrainResourceUsage
{
    uint64 Requests = 0;
    uint64 TransferredBytes = 0;
    uint64 PeakResidentBytes = 0;
    uint64 ScratchBytesPresent = 0;
    uint64 StagedTerrainBytesPresent = 0;
};

struct SKIPREPARATION_API FStagedTerrainStageResult
{
    EStagedTerrainStageOutcome Outcome = EStagedTerrainStageOutcome::FatalFailure;
    FString FailureCode;
    FString FailureDetail;
    /** Opaque durable cursor owned by the adapter; reused when the same stage resumes. */
    FString CheckpointToken;
    /** SHA-256 of this stage's durable output or receipt. */
    FString OutputSha256;
    uint64 CompletedUnits = 0;
    uint64 TotalUnits = 0;
    FStagedTerrainResourceUsage Usage;
    FStagedTerrainStageProof Proof;
    TArray<FStagedTerrainSelectedSource> SelectedSources;
    TArray<FStagedTerrainCogObservation> CogObservations;
    TArray<FStagedTerrainObjectPin> ObjectPins;
};

struct SKIPREPARATION_API FStagedTerrainCompletedStage
{
    EStagedTerrainStage Stage = EStagedTerrainStage::Catalog;
    FString OutputSha256;
    uint64 CompletedUnits = 0;
    uint64 TotalUnits = 0;
};

/** Serializable job receipt. It contains no installed-library or level-transition handle. */
struct SKIPREPARATION_API FStagedTerrainAcquisitionReceipt
{
    uint32 SchemaVersion = 1;
    FStagedTerrainAcquisitionRequest Request;
    EStagedTerrainJobState State = EStagedTerrainJobState::Ready;
    EStagedTerrainStage NextStage = EStagedTerrainStage::Catalog;
    FString CheckpointToken;
    FString FailureCode;
    FString FailureDetail;
    TArray<FStagedTerrainSelectedSource> SelectedSources;
    TArray<FStagedTerrainObjectPin> PinnedObjects;
    TArray<FStagedTerrainCompletedStage> CompletedStages;
    FStagedTerrainResourceUsage Usage;
    bool bTerrainCoreStaged = false;
    bool bTerrainCoreVerified = false;
    /** These stay false by contract; M4 has no install or Mountain transition stage. */
    bool bLibraryEntryWritten = false;
    bool bMountainTransitioned = false;
};

/**
 * Component adapters are responsible for invoking the existing catalog, preflight,
 * sampler/scratch, WorldCover, and staging/verification modules. The only transport made
 * available here is SkiNetGateway. Each stage must persist its checkpoint before returning
 * Cancelled or RetryableFailure, then continue from that checkpoint when invoked again.
 */
class SKIPREPARATION_API IStagedTerrainAcquisitionAdapter
{
public:
    virtual ~IStagedTerrainAcquisitionAdapter() = default;
    virtual FStagedTerrainStageResult RunStage(EStagedTerrainStage Stage,
        const FStagedTerrainAcquisitionRequest& Request,
        const FStagedTerrainAcquisitionReceipt& Receipt,
        SkiNetGateway& Gateway,
        const TSharedRef<Cancellation>& Cancellation,
        const FString& CheckpointToken) = 0;
};

class SKIPREPARATION_API FStagedTerrainAcquisitionJob final
{
public:
    FStagedTerrainAcquisitionJob() = default;
    static bool Create(const FStagedTerrainAcquisitionRequest& Request,
        FStagedTerrainAcquisitionJob& OutJob, FString& OutError);
    static bool Restore(const FStagedTerrainAcquisitionReceipt& Receipt,
        FStagedTerrainAcquisitionJob& OutJob, FString& OutError);
    static bool SerializeReceipt(const FStagedTerrainAcquisitionReceipt& Receipt,
        FString& OutJson, FString& OutError);
    static bool DeserializeReceipt(const FString& Json,
        FStagedTerrainAcquisitionReceipt& OutReceipt, FString& OutError);

    /**
     * Runs at most MaxStages stage calls. Reaching that cap checkpoints the job as Paused.
     * A later call or restored receipt resumes at the same stage/cursor. A value of zero
     * does no work and is rejected.
     */
    bool Run(IStagedTerrainAcquisitionAdapter& Adapter,
        const TSharedRef<Cancellation>& Cancellation, uint32 MaxStages,
        FString& OutError);

    const FStagedTerrainAcquisitionReceipt& GetReceipt() const noexcept { return Receipt; }
    EStagedTerrainStage GetNextStage() const noexcept { return Receipt.NextStage; }
    bool IsComplete() const noexcept { return Receipt.State == EStagedTerrainJobState::Complete; }

private:
    explicit FStagedTerrainAcquisitionJob(FStagedTerrainAcquisitionReceipt&& InReceipt)
        : Receipt(MoveTemp(InReceipt)) {}

    bool ValidateStageResult(EStagedTerrainStage Stage,
        const FStagedTerrainStageResult& Result, FString& OutError);
    bool RememberPins(const TArray<FStagedTerrainObjectPin>& Pins,
        FString& OutError);
    bool CheckResourceUsage(const FStagedTerrainResourceUsage& Added,
        FString& OutError);
    bool HasCompleted(EStagedTerrainStage Stage) const noexcept;
    bool ValidateReceipt(FString& OutError) const;

    FStagedTerrainAcquisitionReceipt Receipt;
};
}
