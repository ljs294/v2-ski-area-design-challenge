#include "SkiTerrainRuntime/TerrainCoreLodController.h"

#include <algorithm>
#include <cstddef>
#include <limits>

SkiTerrainRuntime::TerrainCoreLodController::TerrainCoreLodController(
    SkiDomain::LodHysteresisPolicy InPolicy)
    : Policy(InPolicy)
{
}

bool SkiTerrainRuntime::TerrainCoreLodController::Update(const std::uint32_t TilesX,
    const std::uint32_t TilesY, const std::vector<double>& FinestSamplePixels,
    std::vector<std::uint8_t>& OutLods)
{
    OutLods.clear();
    const std::uint64_t Count64 = static_cast<std::uint64_t>(TilesX) * TilesY;
    if (TilesX == 0 || TilesY == 0 || Count64 > std::numeric_limits<std::size_t>::max()
        || FinestSamplePixels.size() != static_cast<std::size_t>(Count64))
    {
        return false;
    }
    const std::size_t Count = static_cast<std::size_t>(Count64);
    const bool SameGrid = TilesX == PreviousWidth && TilesY == PreviousHeight
        && Previous.size() == Count;
    OutLods.resize(Count);
    for (std::size_t Index = 0; Index < Count; ++Index)
    {
        const std::uint8_t Current = SameGrid ? Previous[Index] : InitialLod;
        OutLods[Index] = SkiDomain::SelectTerrainCoreLod(
            FinestSamplePixels[Index], Current, Policy);
    }

    // Pull a coarser neighbor toward the finer one until all four-neighbor constraints hold.
    bool Changed;
    do
    {
        Changed = false;
        for (std::uint32_t Y = 0; Y < TilesY; ++Y)
        {
            for (std::uint32_t X = 0; X < TilesX; ++X)
            {
                const std::size_t Index = static_cast<std::size_t>(Y) * TilesX + X;
                const auto Relax = [&](const std::size_t Neighbor)
                {
                    if (OutLods[Index] > OutLods[Neighbor] + 1U)
                    {
                        OutLods[Index] = static_cast<std::uint8_t>(OutLods[Neighbor] + 1U);
                        Changed = true;
                    }
                    else if (OutLods[Neighbor] > OutLods[Index] + 1U)
                    {
                        OutLods[Neighbor] = static_cast<std::uint8_t>(OutLods[Index] + 1U);
                        Changed = true;
                    }
                };
                if (X + 1U < TilesX) Relax(Index + 1U);
                if (Y + 1U < TilesY) Relax(Index + TilesX);
            }
        }
    } while (Changed);

    PreviousWidth = TilesX;
    PreviousHeight = TilesY;
    Previous = OutLods;
    return true;
}

bool SkiTerrainRuntime::TerrainCoreLodController::SelectTileKeys(
    const std::uint32_t FinestTilesX, const std::uint32_t FinestTilesY,
    const std::vector<double>& FinestSamplePixels,
    std::vector<SkiApplication::TerrainCoreTileKey>& OutKeys)
{
    OutKeys.clear();
    std::vector<std::uint8_t> Desired;
    if (!Update(FinestTilesX, FinestTilesY, FinestSamplePixels, Desired)) return false;
    std::vector<std::uint8_t> Covered(Desired.size(), 0);
    for (std::int32_t Lod = static_cast<std::int32_t>(SkiDomain::TerrainCoreLodFactors.size()) - 1;
        Lod >= 0; --Lod)
    {
        const std::uint32_t Factor = SkiDomain::TerrainCoreLodFactors[static_cast<std::size_t>(Lod)];
        for (std::uint32_t Y = 0; Y < FinestTilesY; Y += Factor)
        {
            for (std::uint32_t X = 0; X < FinestTilesX; X += Factor)
            {
                bool Eligible = true;
                const std::uint32_t EndY = std::min(FinestTilesY, Y + Factor);
                const std::uint32_t EndX = std::min(FinestTilesX, X + Factor);
                for (std::uint32_t CheckY = Y; Eligible && CheckY < EndY; ++CheckY)
                {
                    for (std::uint32_t CheckX = X; CheckX < EndX; ++CheckX)
                    {
                        const std::size_t Index = static_cast<std::size_t>(CheckY)
                            * FinestTilesX + CheckX;
                        if (Covered[Index] != 0 || Desired[Index] < Lod)
                        {
                            Eligible = false;
                            break;
                        }
                    }
                }
                if (!Eligible) continue;
                OutKeys.push_back({static_cast<std::uint8_t>(Lod), X / Factor, Y / Factor});
                for (std::uint32_t FillY = Y; FillY < EndY; ++FillY)
                {
                    for (std::uint32_t FillX = X; FillX < EndX; ++FillX)
                    {
                        Covered[static_cast<std::size_t>(FillY) * FinestTilesX + FillX] = 1;
                    }
                }
            }
        }
    }
    if (!std::all_of(Covered.begin(), Covered.end(), [](const std::uint8_t Value)
    {
        return Value != 0;
    }))
    {
        OutKeys.clear();
        return false;
    }

    // Greedy coalescing can hide the relaxed LOD at the edge of a block. For example,
    // desired LODs [..., 1, 2, 2, 3, 3, ...] can emit a LOD-1 pair immediately beside
    // a LOD-3 block. Split only the offending coarse key until the emitted spatial
    // partition itself satisfies the four-neighbor constraint.
    for (;;)
    {
        std::vector<std::size_t> Owners(Desired.size(), OutKeys.size());
        for (std::size_t KeyIndex = 0; KeyIndex < OutKeys.size(); ++KeyIndex)
        {
            const SkiApplication::TerrainCoreTileKey& Key = OutKeys[KeyIndex];
            const std::uint32_t Factor = SkiDomain::TerrainCoreLodFactors[Key.Lod];
            const std::uint32_t StartX = Key.X * Factor;
            const std::uint32_t StartY = Key.Y * Factor;
            const std::uint32_t EndX = std::min(FinestTilesX, StartX + Factor);
            const std::uint32_t EndY = std::min(FinestTilesY, StartY + Factor);
            for (std::uint32_t Y = StartY; Y < EndY; ++Y)
            {
                for (std::uint32_t X = StartX; X < EndX; ++X)
                {
                    Owners[static_cast<std::size_t>(Y) * FinestTilesX + X] = KeyIndex;
                }
            }
        }

        std::size_t SplitIndex = OutKeys.size();
        for (std::uint32_t Y = 0; Y < FinestTilesY && SplitIndex == OutKeys.size(); ++Y)
        {
            for (std::uint32_t X = 0; X < FinestTilesX; ++X)
            {
                const std::size_t Index = static_cast<std::size_t>(Y) * FinestTilesX + X;
                const auto Inspect = [&](const std::size_t Neighbor)
                {
                    const std::size_t FirstOwner = Owners[Index];
                    const std::size_t SecondOwner = Owners[Neighbor];
                    if (FirstOwner >= OutKeys.size() || SecondOwner >= OutKeys.size()
                        || FirstOwner == SecondOwner
                        || SkiDomain::AreTerrainCoreLodsAdjacent(
                            OutKeys[FirstOwner].Lod, OutKeys[SecondOwner].Lod))
                    {
                        return;
                    }
                    SplitIndex = OutKeys[FirstOwner].Lod > OutKeys[SecondOwner].Lod
                        ? FirstOwner : SecondOwner;
                };
                if (X + 1U < FinestTilesX) Inspect(Index + 1U);
                if (SplitIndex == OutKeys.size() && Y + 1U < FinestTilesY)
                    Inspect(Index + FinestTilesX);
                if (SplitIndex != OutKeys.size()) break;
            }
        }
        if (SplitIndex == OutKeys.size()) return true;

        const SkiApplication::TerrainCoreTileKey Parent = OutKeys[SplitIndex];
        if (Parent.Lod == 0)
        {
            OutKeys.clear();
            return false;
        }
        OutKeys.erase(OutKeys.begin() + static_cast<std::ptrdiff_t>(SplitIndex));
        const std::uint8_t ChildLod = static_cast<std::uint8_t>(Parent.Lod - 1U);
        const std::uint32_t ChildFactor = SkiDomain::TerrainCoreLodFactors[ChildLod];
        for (std::uint32_t ChildY = 0; ChildY < 2U; ++ChildY)
        {
            for (std::uint32_t ChildX = 0; ChildX < 2U; ++ChildX)
            {
                const std::uint32_t X = Parent.X * 2U + ChildX;
                const std::uint32_t Y = Parent.Y * 2U + ChildY;
                if (X * ChildFactor < FinestTilesX && Y * ChildFactor < FinestTilesY)
                    OutKeys.push_back({ChildLod, X, Y});
            }
        }
    }
}

void SkiTerrainRuntime::TerrainCoreLodController::Reset(const std::uint8_t InInitialLod)
{
    PreviousWidth = PreviousHeight = 0;
    InitialLod = std::min<std::uint8_t>(InInitialLod,
        static_cast<std::uint8_t>(SkiDomain::TerrainCoreLodFactors.size() - 1U));
    Previous.clear();
}
