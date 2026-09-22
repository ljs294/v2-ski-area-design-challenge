#include "SkiApplication/TerrainCoreRepository.h"

#include <algorithm>
#include <cmath>

namespace
{
const SkiDomain::TerrainCoreTileDescriptor* FindTile(
    const SkiDomain::TerrainCoreManifest& Manifest,
    const SkiApplication::TerrainCoreTileKey& Key)
{
    const auto It = std::find_if(Manifest.Tiles.begin(), Manifest.Tiles.end(),
        [&Key](const SkiDomain::TerrainCoreTileDescriptor& Tile)
        {
            return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X && Tile.TileY == Key.Y;
        });
    return It == Manifest.Tiles.end() ? nullptr : &*It;
}
}

std::shared_ptr<SkiApplication::TerrainCoreRepository>
SkiApplication::TerrainCoreRepository::Create(SkiDomain::TerrainCoreManifest InManifest,
    TileReader InReader, std::string& OutError)
{
    OutError.clear();
    if (!InReader)
    {
        OutError = "tile reader is required";
        return {};
    }
    const SkiDomain::TerrainCoreValidation Validation = SkiDomain::ValidateTerrainCore(InManifest);
    if (!Validation.Ok())
    {
        OutError = "TerrainCore manifest validation failed";
        return {};
    }
    auto Immutable = std::make_shared<const SkiDomain::TerrainCoreManifest>(std::move(InManifest));
    return std::shared_ptr<TerrainCoreRepository>(
        new TerrainCoreRepository(std::move(Immutable), std::move(InReader)));
}

SkiApplication::TerrainCoreRepository::TerrainCoreRepository(
    std::shared_ptr<const SkiDomain::TerrainCoreManifest> InManifest, TileReader InReader)
    : Manifest(std::move(InManifest)), Reader(std::move(InReader))
{
}

std::shared_ptr<const SkiDomain::TerrainCoreManifest>
SkiApplication::TerrainCoreRepository::Metadata() const
{
    return Manifest;
}

bool SkiApplication::TerrainCoreRepository::ReadTile(const TerrainCoreTileKey& Key,
    TerrainCoreTilePayload& OutTile, std::string& OutError) const
{
    return ReadTile(Key, OutTile, OutError, {});
}

bool SkiApplication::TerrainCoreRepository::ReadTile(const TerrainCoreTileKey& Key,
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
    if (!Manifest || Key.Lod >= SkiDomain::TerrainCoreLodFactors.size())
    {
        OutError = "invalid TerrainCore tile key";
        return false;
    }
    const SkiDomain::TerrainCoreTileDescriptor* Descriptor = FindTile(*Manifest, Key);
    if (!Descriptor)
    {
        OutError = "TerrainCore tile not found";
        return false;
    }
    TerrainCoreTilePayload Candidate;
    if (!Reader(*Descriptor, Candidate, OutError))
    {
        Candidate = {};
        if (OutError.empty()) OutError = "TerrainCore tile read failed";
        return false;
    }
    if (Cancellation.IsCancellationRequested())
    {
        OutError = "TerrainCore tile read cancelled";
        return false;
    }
    const std::uint64_t StoredWidth = static_cast<std::uint64_t>(Descriptor->CoreWidth)
        + Descriptor->HaloWest + Descriptor->HaloEast;
    const std::uint64_t StoredHeight = static_cast<std::uint64_t>(Descriptor->CoreHeight)
        + Descriptor->HaloNorth + Descriptor->HaloSouth;
    const std::uint64_t Expected = StoredWidth * StoredHeight;
    if (Expected == 0 || Expected > SkiDomain::TerrainCoreMaxSamples
        || Candidate.Heights.size() != Expected || Candidate.Validity.size() != Expected)
    {
        OutError = "TerrainCore tile payload dimensions do not match metadata";
        return false;
    }
    for (std::size_t Index = 0; Index < Candidate.Validity.size(); ++Index)
    {
        if (Candidate.Validity[Index] > 1
            || (Candidate.Validity[Index] != 0 && !std::isfinite(Candidate.Heights[Index])))
        {
            OutError = "TerrainCore tile payload contains invalid samples";
            return false;
        }
    }
    Candidate.Key = Key;
    Candidate.Descriptor = *Descriptor;
    OutTile = std::move(Candidate);
    return true;
}
