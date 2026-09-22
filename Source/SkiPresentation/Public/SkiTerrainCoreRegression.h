#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation
{
class PreparationOperationLease;
}

namespace SkiPresentation
{
enum class TerrainCoreRegressionPhase : uint8
{
    ImportEdit,
    OfflineReopen,
};

/** Values proved by the packaged TerrainCore v2 regression path. */
struct SKIPRESENTATION_API TerrainCoreRegressionProof
{
    FString ContentId;
    FString EditSetId;
    FString Error;
    TArray<uint32> LodFactors;
    uint32 Width = 0;
    uint32 Height = 0;
    uint64 CacheBudgetBytes = 0;
    uint64 PeakResidentBytes = 0;
    uint32 ObservedEvictions = 0;
    int32 LocalTileReads = 0;
    int32 NetworkAttempts = 0;
    int32 AcquisitionTransportCalls = 0;
    double FinestQueryHeightM = 0.0;
    double EditedQueryHeightM = 0.0;
    double OfflineReopenMilliseconds = 0.0;
    bool bPartialEdgeTile = false;
    bool bSharedBorder = false;
    bool bNormalHalo = false;
    bool bFinestQuery = false;
    bool bBaseImmutable = false;
    bool bEditPersisted = false;
    bool bEditDeltaReconstructed = false;
    bool bStalePublicationRejected = false;
    bool bOfflineReopened = false;
    bool bAcquisitionPortGuardInstalled = false;
};

/**
 * Runs one phase of the production schema-2 store, repository and streaming-cache proof in a
 * supplied isolated data root. The Shipping harness runs ImportEdit and OfflineReopen in two
 * separate processes, passing only the exact content/edit IDs. No provider or network transport
 * is constructed by this regression.
 */
SKIPRESENTATION_API bool RunTerrainCoreRegression(
    TerrainCoreRegressionPhase Phase,
    const FString& DataRoot,
    const TSharedPtr<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    uint64 SessionGeneration,
    uint64 OperationGeneration,
    const FString& ExistingContentId,
    const FString& ExistingEditSetId,
    TerrainCoreRegressionProof& OutProof);
}
