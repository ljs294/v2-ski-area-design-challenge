#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/PlaceCoordinates.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
class IAcquisitionTransport;

struct SKIPREPARATION_API PlaceSearchResult
{
    FString Name;
    FString DisplayName;
    SkiDomain::PlaceCoordinates Point;
    SkiDomain::GeographicBounds BoundingBox;
    FString Region;
    FString Country;
};

/**
 * Search is an explicit, submit-only operation. Callers should invoke it for Go/Enter,
 * rather than on text changes.
 */
class SKIPREPARATION_API IPlaceSearchProvider
{
public:
    virtual ~IPlaceSearchProvider() = default;
    virtual bool Search(const FString& Query,
        const TSharedRef<Cancellation>& Cancellation,
        TArray<PlaceSearchResult>& OutResults,
        FString& OutError) = 0;
};

/** Nominatim adapter. The injected transport keeps tests offline and routes production through SkiNetGateway. */
class SKIPREPARATION_API NominatimSearchProvider final : public IPlaceSearchProvider
{
public:
    explicit NominatimSearchProvider(
        TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> InTransport = nullptr);

    bool Search(const FString& Query,
        const TSharedRef<Cancellation>& Cancellation,
        TArray<PlaceSearchResult>& OutResults,
        FString& OutError) override;

private:
    TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> Transport;
    FCriticalSection CacheMutex;
    TMap<FString, TArray<PlaceSearchResult>> Cache;
    TArray<FString> CacheOrder;
};
}
