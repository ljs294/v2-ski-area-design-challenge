#pragma once

#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation { class IAcquisitionTransport; }

namespace SkiPreparation
{
class SKIPREPARATION_API NativeTerrainProvider final : public Provider
{
public:
    explicit NativeTerrainProvider(FString InDataRoot,
        TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> InTransport = nullptr);
    Result Prepare(const Request& RequestValue, const TSharedRef<Cancellation>& CancellationValue,
        const ProgressCallback& OnProgress) override;

private:
    FString DataRoot;
    TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> Transport;
};
}
