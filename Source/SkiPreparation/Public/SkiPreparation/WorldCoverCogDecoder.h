#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
class SKIPREPARATION_API ICogByteSource
{
public:
    virtual ~ICogByteSource() = default;
    virtual uint64 Size() const noexcept = 0;
    virtual bool Read(uint64 Offset, uint64 Length, TArray<uint8>& OutBytes,
        FString& OutError) = 0;
};

struct SKIPREPARATION_API DecodedCoverWindow
{
    TArray<uint8> Classes;
    TArray<uint8> Validity;
    SkiDomain::GeographicBounds ActualOuterBounds;
    SkiDomain::GeographicBounds SampleCenterBounds;
    uint32 Width = 0;
    uint32 Height = 0;
    double LongitudeSpacingDeg = 0.0;
    double LatitudeSpacingDeg = 0.0;
    uint8 NoDataValue = 0;
    bool bHasNoData = false;
    uint16 Compression = 0;
    uint32 SourceWidth = 0;
    uint32 SourceHeight = 0;
};

/** Reads only tiles intersecting RequestedBounds from the official uint8 class COG. */
SKIPREPARATION_API bool DecodeWorldCoverCogWindow(ICogByteSource& Source,
    const SkiDomain::GeographicBounds& RequestedBounds, DecodedCoverWindow& OutWindow,
    ProviderFailure& OutFailure, const TFunction<bool()>& IsCancelled = {});
}
