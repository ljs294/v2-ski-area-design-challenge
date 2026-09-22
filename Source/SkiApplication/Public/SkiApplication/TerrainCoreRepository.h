#pragma once

#include "SkiDomain/TerrainCore.h"

#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace SkiApplication
{
class SKIAPPLICATION_API TerrainCoreReadCancellation
{
public:
    TerrainCoreReadCancellation() = default;
    explicit TerrainCoreReadCancellation(std::shared_ptr<const std::atomic_bool> InCancelled)
        : Cancelled(std::move(InCancelled))
    {
    }

    bool IsCancellationRequested() const noexcept
    {
        return Cancelled && Cancelled->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<const std::atomic_bool> Cancelled;
};

struct SKIAPPLICATION_API TerrainCoreTileKey
{
    std::uint8_t Lod = 0;
    std::uint32_t X = 0;
    std::uint32_t Y = 0;

    bool operator==(const TerrainCoreTileKey& Other) const noexcept
    {
        return Lod == Other.Lod && X == Other.X && Y == Other.Y;
    }
    bool operator<(const TerrainCoreTileKey& Other) const noexcept
    {
        if (Lod != Other.Lod) return Lod < Other.Lod;
        if (Y != Other.Y) return Y < Other.Y;
        return X < Other.X;
    }
};

struct SKIAPPLICATION_API TerrainCoreTilePayload
{
    TerrainCoreTileKey Key;
    SkiDomain::TerrainCoreTileDescriptor Descriptor;
    std::vector<float> Heights;
    std::vector<std::uint8_t> Validity;

    std::uint64_t ResidentBytes() const noexcept
    {
        return static_cast<std::uint64_t>(Heights.size()) * sizeof(float)
            + static_cast<std::uint64_t>(Validity.size());
    }
};

class SKIAPPLICATION_API ITerrainCoreRepository
{
public:
    virtual ~ITerrainCoreRepository() = default;
    virtual std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata() const = 0;
    virtual bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError) const = 0;
    virtual bool EditTransition(SkiDomain::Revision& OutBaseRevision,
        SkiDomain::Revision& OutEditRevision) const noexcept
    {
        OutBaseRevision = 0;
        OutEditRevision = 0;
        return false;
    }
    // Edited repositories expose their immutable package repository and cumulative sparse
    // sidecar so another edit can replace, rather than wrap, the existing overlay. Ordinary
    // package repositories deliberately return false.
    virtual bool FlattenEditOverlay(
        std::shared_ptr<const ITerrainCoreRepository>& OutBaseRepository,
        std::shared_ptr<const SkiDomain::TerrainEditSet>& OutEdits) const noexcept
    {
        OutBaseRepository.reset();
        OutEdits.reset();
        return false;
    }
    virtual bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError, const TerrainCoreReadCancellation& Cancellation) const
    {
        if (Cancellation.IsCancellationRequested())
        {
            OutTile = {};
            OutError = "TerrainCore tile read cancelled";
            return false;
        }
        const bool Result = ReadTile(Key, OutTile, OutError);
        if (Cancellation.IsCancellationRequested())
        {
            OutTile = {};
            OutError = "TerrainCore tile read cancelled";
            return false;
        }
        return Result;
    }
};

// A validated repository port. Storage adapters supply the reader; callers cannot mutate
// the manifest after construction. The reader may decode compressed disk assets, but must
// return one complete tile or fail without exposing partial data.
class SKIAPPLICATION_API TerrainCoreRepository final : public ITerrainCoreRepository
{
public:
    using TileReader = std::function<bool(const SkiDomain::TerrainCoreTileDescriptor&,
        TerrainCoreTilePayload&, std::string&)>;

    static std::shared_ptr<TerrainCoreRepository> Create(
        SkiDomain::TerrainCoreManifest Manifest, TileReader Reader,
        std::string& OutError);

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata() const override;
    bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError) const override;
    bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError, const TerrainCoreReadCancellation& Cancellation) const override;

private:
    TerrainCoreRepository(std::shared_ptr<const SkiDomain::TerrainCoreManifest> InManifest,
        TileReader InReader);

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Manifest;
    TileReader Reader;
};
}
