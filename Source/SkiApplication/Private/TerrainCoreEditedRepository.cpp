#include "SkiApplication/TerrainCoreEditedRepository.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <map>
#include <vector>

namespace
{
std::uint64_t TileIdentity(const std::uint32_t X, const std::uint32_t Y) noexcept
{
    return (static_cast<std::uint64_t>(Y) << 32U) | X;
}

bool ContainsSample(const SkiDomain::TerrainCoreTileDescriptor& Tile,
    const std::uint32_t Column, const std::uint32_t Row) noexcept
{
    const std::int64_t West = static_cast<std::int64_t>(Tile.StartColumn) - Tile.HaloWest;
    const std::int64_t North = static_cast<std::int64_t>(Tile.StartRow) - Tile.HaloNorth;
    const std::int64_t East = static_cast<std::int64_t>(Tile.StartColumn)
        + Tile.CoreWidth - 1 + Tile.HaloEast;
    const std::int64_t South = static_cast<std::int64_t>(Tile.StartRow)
        + Tile.CoreHeight - 1 + Tile.HaloSouth;
    return static_cast<std::int64_t>(Column) >= West
        && static_cast<std::int64_t>(Column) <= East
        && static_cast<std::int64_t>(Row) >= North
        && static_cast<std::int64_t>(Row) <= South;
}
}

struct SkiApplication::TerrainCoreEditedRepository::Index
{
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> ByTile;
};

namespace
{
template <typename IndexType>
bool ApplyFinestEdits(const SkiApplication::TerrainCoreTileKey& Key,
    const SkiDomain::TerrainEditSet& Edits,
    const IndexType& EditIndex,
    SkiApplication::TerrainCoreTilePayload& Candidate, std::string& OutError,
    const SkiApplication::TerrainCoreReadCancellation& Cancellation)
{
    const auto Found = EditIndex.ByTile.find(TileIdentity(Key.X, Key.Y));
    if (Found == EditIndex.ByTile.end()) return true;
    const SkiDomain::TerrainCoreTileDescriptor& Tile = Candidate.Descriptor;
    const std::uint32_t StoredWidth = Tile.CoreWidth + Tile.HaloWest + Tile.HaloEast;
    const std::int64_t StoredWest = static_cast<std::int64_t>(Tile.StartColumn) - Tile.HaloWest;
    const std::int64_t StoredNorth = static_cast<std::int64_t>(Tile.StartRow) - Tile.HaloNorth;
    for (const std::uint32_t DeltaIndex : Found->second)
    {
        if (Cancellation.IsCancellationRequested())
        {
            OutError = "TerrainCore tile read cancelled";
            return false;
        }
        const SkiDomain::TerrainEditDelta& Delta = Edits.Deltas[DeltaIndex];
        const std::uint32_t LocalColumn = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(Delta.Column) - StoredWest);
        const std::uint32_t LocalRow = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(Delta.Row) - StoredNorth);
        const std::uint64_t SampleIndex = static_cast<std::uint64_t>(LocalRow) * StoredWidth
            + LocalColumn;
        if (SampleIndex >= Candidate.Heights.size())
        {
            OutError = "TerrainEditSet index does not match tile geometry";
            return false;
        }
        if (Candidate.Validity[static_cast<std::size_t>(SampleIndex)] == 0) continue;
        const float Edited = Candidate.Heights[static_cast<std::size_t>(SampleIndex)]
            + Delta.DeltaM;
        if (!std::isfinite(Edited))
        {
            OutError = "TerrainEditSet produces a nonfinite height";
            return false;
        }
        Candidate.Heights[static_cast<std::size_t>(SampleIndex)] = Edited;
    }
    return true;
}
}

std::shared_ptr<SkiApplication::TerrainCoreEditedRepository>
SkiApplication::TerrainCoreEditedRepository::Create(
    std::shared_ptr<const ITerrainCoreRepository> BaseRepository,
    SkiDomain::TerrainEditSet InEdits, const SkiDomain::Revision ExpectedBaseRevision,
    std::string& OutError)
{
    OutError.clear();
    const auto Manifest = BaseRepository ? BaseRepository->Metadata() : nullptr;
    if (!Manifest || !SkiDomain::ValidateTerrainCore(*Manifest).Ok())
    {
        OutError = "validated base TerrainCore repository is required";
        return {};
    }
    if (ExpectedBaseRevision == 0 || InEdits.TerrainCoreId != Manifest->ContentId)
    {
        OutError = "TerrainEditSet does not match the installed TerrainCore";
        return {};
    }
    if (InEdits.BaseRevision != ExpectedBaseRevision
        || InEdits.EditRevision <= InEdits.BaseRevision)
    {
        OutError = "TerrainEditSet revision is stale";
        return {};
    }
    if (!SkiDomain::ValidateTerrainEditSet(InEdits, Manifest->Width, Manifest->Height).Ok())
    {
        OutError = "TerrainEditSet validation failed";
        return {};
    }

    // A session publishes each edit against its current repository. If that repository is
    // already edited, retain its immutable package base and merge the new sparse delta into
    // its cumulative sidecar. This keeps every read at one repository hop regardless of edit
    // history length while preserving a sidecar that can be reopened against the package.
    std::shared_ptr<const ITerrainCoreRepository> FlattenedBase;
    std::shared_ptr<const SkiDomain::TerrainEditSet> PreviousEdits;
    if (BaseRepository->FlattenEditOverlay(FlattenedBase, PreviousEdits))
    {
        const auto FlattenedManifest = FlattenedBase ? FlattenedBase->Metadata() : nullptr;
        std::shared_ptr<const ITerrainCoreRepository> NestedBase;
        std::shared_ptr<const SkiDomain::TerrainEditSet> NestedEdits;
        if (!FlattenedBase || !PreviousEdits || !FlattenedManifest
            || !SkiDomain::ValidateTerrainCore(*FlattenedManifest).Ok()
            || FlattenedManifest->ContentId != Manifest->ContentId
            || FlattenedBase->FlattenEditOverlay(NestedBase, NestedEdits)
            || PreviousEdits->TerrainCoreId != InEdits.TerrainCoreId
            || PreviousEdits->EditRevision != ExpectedBaseRevision
            || !SkiDomain::ValidateTerrainEditSet(*PreviousEdits,
                Manifest->Width, Manifest->Height).Ok())
        {
            OutError = "edited TerrainCore repository cannot be flattened";
            return {};
        }

        SkiDomain::TerrainEditSet Merged = *PreviousEdits;
        Merged.EditRevision = InEdits.EditRevision;
        std::unordered_map<std::uint64_t, std::size_t> DeltaBySample;
        DeltaBySample.reserve(Merged.Deltas.size() + InEdits.Deltas.size());
        for (std::size_t Index = 0; Index < Merged.Deltas.size(); ++Index)
        {
            const SkiDomain::TerrainEditDelta& Delta = Merged.Deltas[Index];
            DeltaBySample.emplace(TileIdentity(Delta.Column, Delta.Row), Index);
        }
        for (const SkiDomain::TerrainEditDelta& Delta : InEdits.Deltas)
        {
            const std::uint64_t Sample = TileIdentity(Delta.Column, Delta.Row);
            const auto Existing = DeltaBySample.find(Sample);
            if (Existing == DeltaBySample.end())
            {
                if (Merged.Deltas.size() >= SkiDomain::TerrainCoreMaxEdits)
                {
                    OutError = "cumulative TerrainEditSet exceeds the sparse edit limit";
                    return {};
                }
                DeltaBySample.emplace(Sample, Merged.Deltas.size());
                Merged.Deltas.push_back(Delta);
                continue;
            }
            const float Cumulative = Merged.Deltas[Existing->second].DeltaM + Delta.DeltaM;
            if (!std::isfinite(Cumulative))
            {
                OutError = "cumulative TerrainEditSet produces a nonfinite delta";
                return {};
            }
            Merged.Deltas[Existing->second].DeltaM = Cumulative;
        }
        if (!SkiDomain::ValidateTerrainEditSet(Merged, Manifest->Width, Manifest->Height).Ok())
        {
            OutError = "cumulative TerrainEditSet validation failed";
            return {};
        }
        BaseRepository = std::move(FlattenedBase);
        InEdits = std::move(Merged);
    }

    auto BuiltIndex = std::make_shared<Index>();
    std::unordered_map<std::uint64_t, const SkiDomain::TerrainCoreTileDescriptor*> FinestTiles;
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Manifest->Tiles)
    {
        if (Tile.LodIndex == 0)
        {
            FinestTiles.emplace(TileIdentity(Tile.TileX, Tile.TileY), &Tile);
        }
    }
    for (std::uint32_t EditIndex = 0; EditIndex < InEdits.Deltas.size(); ++EditIndex)
    {
        const SkiDomain::TerrainEditDelta& Delta = InEdits.Deltas[EditIndex];
        const std::uint32_t PrimaryX = Delta.Column / SkiDomain::TerrainCoreTileCells;
        const std::uint32_t PrimaryY = Delta.Row / SkiDomain::TerrainCoreTileCells;
        for (std::int64_t Y = static_cast<std::int64_t>(PrimaryY) - 1;
            Y <= static_cast<std::int64_t>(PrimaryY) + 1; ++Y)
        {
            for (std::int64_t X = static_cast<std::int64_t>(PrimaryX) - 1;
                X <= static_cast<std::int64_t>(PrimaryX) + 1; ++X)
            {
                if (X < 0 || Y < 0) continue;
                const std::uint64_t Identity = TileIdentity(static_cast<std::uint32_t>(X),
                    static_cast<std::uint32_t>(Y));
                const auto Tile = FinestTiles.find(Identity);
                if (Tile != FinestTiles.end()
                    && ContainsSample(*Tile->second, Delta.Column, Delta.Row))
                {
                    BuiltIndex->ByTile[Identity].push_back(EditIndex);
                }
            }
        }
    }

    auto ImmutableEdits = std::make_shared<const SkiDomain::TerrainEditSet>(std::move(InEdits));
    return std::shared_ptr<TerrainCoreEditedRepository>(new TerrainCoreEditedRepository(
        std::move(BaseRepository), std::move(ImmutableEdits), std::move(BuiltIndex),
        ExpectedBaseRevision));
}

SkiApplication::TerrainCoreEditedRepository::TerrainCoreEditedRepository(
    std::shared_ptr<const ITerrainCoreRepository> InBase,
    std::shared_ptr<const SkiDomain::TerrainEditSet> InEdits,
    std::shared_ptr<const Index> InIndex,
    const SkiDomain::Revision InTransitionBaseRevision)
    : Base(std::move(InBase)), Edits(std::move(InEdits)), EditIndex(std::move(InIndex)),
      TransitionBaseRevision(InTransitionBaseRevision)
{
}

std::shared_ptr<const SkiDomain::TerrainCoreManifest>
SkiApplication::TerrainCoreEditedRepository::Metadata() const
{
    return Base->Metadata();
}

bool SkiApplication::TerrainCoreEditedRepository::ReadTile(const TerrainCoreTileKey& Key,
    TerrainCoreTilePayload& OutTile, std::string& OutError) const
{
    return ReadTile(Key, OutTile, OutError, {});
}

bool SkiApplication::TerrainCoreEditedRepository::ReadTile(const TerrainCoreTileKey& Key,
    TerrainCoreTilePayload& OutTile, std::string& OutError,
    const TerrainCoreReadCancellation& Cancellation) const
{
    OutTile = {};
    OutError.clear();
    if (Cancellation.IsCancellationRequested())
    {
        OutError = "TerrainCore tile read cancelled";
        return false;
    }
    TerrainCoreTilePayload Candidate;
    if (Key.Lod == 0)
    {
        if (!Base->ReadTile(Key, Candidate, OutError, Cancellation)
            || !ApplyFinestEdits(Key, *Edits, *EditIndex, Candidate, OutError, Cancellation))
        {
            return false;
        }
        OutTile = std::move(Candidate);
        return true;
    }

    const auto Manifest = Base->Metadata();
    if (!Manifest || Key.Lod >= SkiDomain::TerrainCoreLodFactors.size())
    {
        OutError = "invalid TerrainCore tile key";
        return false;
    }
    const auto DescriptorIt = std::find_if(Manifest->Tiles.begin(), Manifest->Tiles.end(),
        [&Key](const SkiDomain::TerrainCoreTileDescriptor& Tile)
        {
            return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X && Tile.TileY == Key.Y;
        });
    if (DescriptorIt == Manifest->Tiles.end())
    {
        OutError = "TerrainCore tile not found";
        return false;
    }

    Candidate.Key = Key;
    Candidate.Descriptor = *DescriptorIt;
    const std::uint32_t StoredWidth = Candidate.Descriptor.CoreWidth
        + Candidate.Descriptor.HaloWest + Candidate.Descriptor.HaloEast;
    const std::uint32_t StoredHeight = Candidate.Descriptor.CoreHeight
        + Candidate.Descriptor.HaloNorth + Candidate.Descriptor.HaloSouth;
    Candidate.Heights.resize(static_cast<std::size_t>(StoredWidth) * StoredHeight);
    Candidate.Validity.resize(Candidate.Heights.size());
    const std::int64_t FirstLodColumn = static_cast<std::int64_t>(
        Candidate.Descriptor.StartColumn) - Candidate.Descriptor.HaloWest;
    const std::int64_t FirstLodRow = static_cast<std::int64_t>(
        Candidate.Descriptor.StartRow) - Candidate.Descriptor.HaloNorth;
    const std::uint32_t Factor = Candidate.Descriptor.LodFactor;
    const std::uint32_t MaximumTileX = (Manifest->Width - 2U) / SkiDomain::TerrainCoreTileCells;
    const std::uint32_t MaximumTileY = (Manifest->Height - 2U) / SkiDomain::TerrainCoreTileCells;
    std::map<TerrainCoreTileKey, TerrainCoreTilePayload> Finest;
    for (std::uint32_t LocalRow = 0; LocalRow < StoredHeight; ++LocalRow)
    {
        if (Cancellation.IsCancellationRequested())
        {
            OutError = "TerrainCore tile read cancelled";
            return false;
        }
        const std::uint32_t FineRow = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(FirstLodRow + LocalRow) * Factor,
            Manifest->Height - 1U);
        for (std::uint32_t LocalColumn = 0; LocalColumn < StoredWidth; ++LocalColumn)
        {
            const std::uint32_t FineColumn = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(FirstLodColumn + LocalColumn) * Factor,
                Manifest->Width - 1U);
            const TerrainCoreTileKey FinestKey{0,
                std::min(FineColumn / SkiDomain::TerrainCoreTileCells, MaximumTileX),
                std::min(FineRow / SkiDomain::TerrainCoreTileCells, MaximumTileY)};
            auto Found = Finest.find(FinestKey);
            if (Found == Finest.end())
            {
                TerrainCoreTilePayload FinestTile;
                if (!Base->ReadTile(FinestKey, FinestTile, OutError, Cancellation)
                    || !ApplyFinestEdits(FinestKey, *Edits, *EditIndex, FinestTile,
                        OutError, Cancellation))
                {
                    return false;
                }
                Found = Finest.emplace(FinestKey, std::move(FinestTile)).first;
            }
            const TerrainCoreTilePayload& FinestTile = Found->second;
            const std::uint32_t FinestWidth = FinestTile.Descriptor.CoreWidth
                + FinestTile.Descriptor.HaloWest + FinestTile.Descriptor.HaloEast;
            const std::uint32_t FinestLocalColumn = FineColumn
                - FinestTile.Descriptor.StartColumn + FinestTile.Descriptor.HaloWest;
            const std::uint32_t FinestLocalRow = FineRow
                - FinestTile.Descriptor.StartRow + FinestTile.Descriptor.HaloNorth;
            const std::size_t SourceIndex = static_cast<std::size_t>(FinestLocalRow)
                * FinestWidth + FinestLocalColumn;
            const std::size_t TargetIndex = static_cast<std::size_t>(LocalRow)
                * StoredWidth + LocalColumn;
            if (SourceIndex >= FinestTile.Heights.size())
            {
                OutError = "TerrainCore finest tile does not cover derivative sample";
                return false;
            }
            Candidate.Heights[TargetIndex] = FinestTile.Heights[SourceIndex];
            Candidate.Validity[TargetIndex] = FinestTile.Validity[SourceIndex];
        }
    }
    OutTile = std::move(Candidate);
    return true;
}

SkiDomain::Revision SkiApplication::TerrainCoreEditedRepository::EditRevision() const noexcept
{
    return Edits->EditRevision;
}

bool SkiApplication::TerrainCoreEditedRepository::EditTransition(
    SkiDomain::Revision& OutBaseRevision,
    SkiDomain::Revision& OutEditRevision) const noexcept
{
    OutBaseRevision = TransitionBaseRevision;
    OutEditRevision = Edits ? Edits->EditRevision : 0;
    return OutBaseRevision != 0 && OutEditRevision > OutBaseRevision;
}

bool SkiApplication::TerrainCoreEditedRepository::FlattenEditOverlay(
    std::shared_ptr<const ITerrainCoreRepository>& OutBaseRepository,
    std::shared_ptr<const SkiDomain::TerrainEditSet>& OutEdits) const noexcept
{
    OutBaseRepository = Base;
    OutEdits = Edits;
    return OutBaseRepository && OutEdits;
}

std::shared_ptr<const SkiDomain::TerrainEditSet>
SkiApplication::TerrainCoreEditedRepository::EditSet() const
{
    return Edits;
}
