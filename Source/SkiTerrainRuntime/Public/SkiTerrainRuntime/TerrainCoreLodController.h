#pragma once

#include "SkiDomain/TerrainCore.h"
#include "SkiApplication/TerrainCoreRepository.h"

#include <cstdint>
#include <vector>

namespace SkiTerrainRuntime
{
class SKITERRAINRUNTIME_API TerrainCoreLodController
{
public:
    explicit TerrainCoreLodController(SkiDomain::LodHysteresisPolicy Policy = {});

    // Samples are row-major finest-level pixels per sample. Results retain hysteresis from
    // the previous update and are relaxed so every four-neighbor differs by at most one LOD.
    bool Update(std::uint32_t TilesX, std::uint32_t TilesY,
        const std::vector<double>& FinestSamplePixels,
        std::vector<std::uint8_t>& OutLods);
    // Produces spatially correct pyramid keys. A LOD N key covers Factor x Factor finest
    // tile regions; output coverage is complete and non-overlapping in finest-cell space,
    // and spatially adjacent emitted keys differ by at most one LOD after coalescing.
    bool SelectTileKeys(std::uint32_t FinestTilesX, std::uint32_t FinestTilesY,
        const std::vector<double>& FinestSamplePixels,
        std::vector<SkiApplication::TerrainCoreTileKey>& OutKeys);
    // Seeds the first update from the already requested overview instead of implicitly
    // walking outward from LOD 0 over several frames.
    void Reset(std::uint8_t InitialLod = 0);

private:
    SkiDomain::LodHysteresisPolicy Policy;
    std::uint32_t PreviousWidth = 0;
    std::uint32_t PreviousHeight = 0;
    std::uint8_t InitialLod = 0;
    std::vector<std::uint8_t> Previous;
};
}
