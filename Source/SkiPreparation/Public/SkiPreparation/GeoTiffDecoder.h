#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiPreparation/TerrainPreparation.h"

namespace SkiPreparation
{
enum class TiffStorageOrganization : uint8
{
    Stripped,
    Tiled,
};

struct SKIPREPARATION_API DecodedElevationRaster
{
    SkiDomain::Heightfield Heightfield;
    SkiDomain::GeographicBounds ActualOuterBounds;
    SkiDomain::GeographicBounds SampleCenterBounds;
    double NoDataValue = -9999.0;
    uint16 Orientation = 0;
    TiffStorageOrganization Storage = TiffStorageOrganization::Stripped;
    uint16 Compression = 0;
    uint32 SourceWidth = 0;
    uint32 SourceHeight = 0;
};

SKIPREPARATION_API bool DecodeElevationGeoTiff(const TArray<uint8>& Bytes,
    const SkiDomain::GeographicBounds& RequestedBounds,
    ProviderProduct Product, DecodedElevationRaster& OutRaster, ProviderFailure& OutFailure,
    const TFunction<bool()>& IsCancelled = {});
}
