#pragma once

#include "CoreMinimal.h"

namespace SkiPreparation
{
constexpr uint32 SiteContextPhotoTilePixels = 256;
constexpr uint64 SiteContextPhotoTileDecodedBytes =
    static_cast<uint64>(SiteContextPhotoTilePixels) * SiteContextPhotoTilePixels * sizeof(FColor);
/** Matches the hard compressed-tile cap used by the M5 imagery acquisition path. */
constexpr uint64 SiteContextPhotoTileMaxCompressedBytes = 2ULL * 1024ULL * 1024ULL;

/** One decoded, north-up SiteContext JPEG tile in BGRA8 memory layout. */
struct SKIPREPARATION_API FSiteContextPhotoTile
{
    TArray<FColor> Pixels;

    bool IsValid() const noexcept
    {
        return Pixels.Num() == static_cast<int32>(SiteContextPhotoTilePixels
            * SiteContextPhotoTilePixels);
    }
};

/**
 * Decodes already hash-verified SiteContext JPEG bytes into exactly 256x256 BGRA8.
 * This helper performs no package access or network I/O. The compressed and decoded
 * allocations are fixed by the limits above. Cancellation is checked before parsing,
 * before raw decode and before publishing the decoded pixels.
 */
SKIPREPARATION_API bool DecodeSiteContextPhotoTile(
    const TArray<uint8>& CompressedJpeg, FSiteContextPhotoTile& OutTile,
    FString& OutError, const TFunction<bool()>& IsCancelled = {});

/**
 * Samples tile-local normalized UV coordinates with deterministic bilinear filtering.
 * UV (0,0) is the north-west pixel center; (1,1) is the south-east pixel center.
 * Non-finite or out-of-range UVs and invalid tiles fail without returning a color.
 */
SKIPREPARATION_API bool SampleSiteContextPhotoTileBilinear(
    const FSiteContextPhotoTile& Tile, double U, double V,
    FColor& OutColor) noexcept;
}
