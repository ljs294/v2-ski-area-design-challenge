#pragma once

#include "SkiPreparation/OsmVectorPackage.h"

namespace SkiPreparation
{
class IAcquisitionTransport;

/**
 * Bounded, single-request Overpass adapter for OSM roads, lifts and pistes.
 * Production defaults to SkiNetGateway; tests can inject a scripted transport.
 */
class SKIPREPARATION_API OverpassVectorProvider final : public IOsmVectorProvider
{
public:
    explicit OverpassVectorProvider(
        TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> InTransport = nullptr);

    bool Acquire(const OsmVectorProviderQuery& Query,
        const Cancellation& CancellationValue, OsmVectorProviderResponse& OutResponse,
        FString& OutError) override;

private:
    TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> Transport;
};
}
