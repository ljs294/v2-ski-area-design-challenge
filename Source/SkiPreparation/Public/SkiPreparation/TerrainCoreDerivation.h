#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainCore.h"

namespace SkiPreparation
{
/** One independently compressed TerrainCore tile. Samples include the declared normal halo. */
struct SKIPREPARATION_API TerrainCoreEncodedTile
{
    SkiDomain::TerrainCoreTileDescriptor Descriptor;
    TArray<uint8> CompressedHeights;
    TArray<uint8> CompressedValidity;
};

/** Decoded tile storage in north-to-south row order, including its normal halo. */
struct SKIPREPARATION_API TerrainCoreDecodedTile
{
    SkiDomain::TerrainCoreTileDescriptor Descriptor;
    uint16 StoredWidth = 0;
    uint16 StoredHeight = 0;
    TArray<float> Heights;
    TArray<uint8> Validity;
};

/** Build a single planned tile without retaining any other pyramid tile. */
SKIPREPARATION_API bool DeriveTerrainCoreTile(const SkiDomain::Heightfield& Finest,
    const SkiDomain::TerrainCoreTileDescriptor& Planned,
    TerrainCoreEncodedTile& OutTile, FString& OutError);

/** Decode and strictly validate one tile. Hash validation belongs to the package store. */
SKIPREPARATION_API bool DecodeTerrainCoreTile(
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    TArrayView<const uint8> CompressedHeights,
    TArrayView<const uint8> CompressedValidity,
    TerrainCoreDecodedTile& OutTile, FString& OutError);

SKIPREPARATION_API uint64 TerrainCoreStoredSampleCount(
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor) noexcept;
}
