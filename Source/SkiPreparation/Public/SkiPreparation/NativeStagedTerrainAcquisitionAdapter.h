#pragma once

#include "SkiPreparation/StagedTerrainAcquisition.h"

namespace SkiPreparation
{
/**
 * Production M4 adapter for the catalog and COG source-preflight stages.
 *
 * Catalog selection remains fail-closed until ElevationCatalog proves full-site raster
 * coverage, raw S1M sidecar lineage, and sampled quality. Later terrain stages are
 * intentionally unsupported by this adapter slice.
 */
class SKIPREPARATION_API FNativeStagedTerrainAcquisitionAdapter final
    : public IStagedTerrainAcquisitionAdapter
{
public:
    FNativeStagedTerrainAcquisitionAdapter() = default;

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    /** Scripted transport injection for deterministic automation tests only. */
    explicit FNativeStagedTerrainAcquisitionAdapter(IAcquisitionTransport& TestTransport)
        : TestTransportOverride(&TestTransport) {}
#endif

    FStagedTerrainStageResult RunStage(EStagedTerrainStage Stage,
        const FStagedTerrainAcquisitionRequest& Request,
        const FStagedTerrainAcquisitionReceipt& Receipt,
        SkiNetGateway& Gateway,
        const TSharedRef<Cancellation>& Cancellation,
        const FString& CheckpointToken) override;

private:
    IAcquisitionTransport* TestTransportOverride = nullptr;
};
}
