#pragma once

#include "SkiDomain/TerrainCore.h"

#include <functional>
#include <array>
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

/** Stable sample-class IDs written by the canonical elevation sampler. */
enum class TerrainSampleProvenance : std::uint8_t
{
    S1MNative = 0,
    S1MBlend = 1,
    S1MBackfill = 2,
    S1MInterpolated = 3,
    Project1m = 4,
    ArcSec13 = 5,
    NoData = 255,
};

inline constexpr std::uint8_t TerrainCoreNoSourceIndex = 0xffU;
inline constexpr std::uint32_t TerrainCoreProvenanceSidecarSchema = 1U;
inline constexpr std::uint64_t TerrainCoreProvenanceSidecarMaxBytes =
    265ULL + SkiDomain::TerrainCoreMaxStoredSamples * 2ULL;

struct SKIAPPLICATION_API TerrainCoreProvenanceSourceCount
{
    std::string SourceId;
    std::uint64_t SampleCount = 0;
    std::array<std::uint64_t, 6> SamplesByProvenance{};
};

/**
 * Verified counts from each LOD0 tile's unique core samples. Halo samples are
 * validated but excluded so a shared border is never counted twice. Source
 * entries remain in manifest order and refer to TerrainCore source-table IDs.
 */
struct SKIAPPLICATION_API TerrainCoreProvenanceSummary
{
    std::string TerrainCoreId;
    std::string GridSha256;
    std::string SourceDictionarySha256;
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::uint64_t TotalSamples = 0;
    std::uint64_t ValidSamples = 0;
    std::uint64_t NoDataSamples = 0;
    std::array<std::uint64_t, 6> SamplesByProvenance{};
    std::vector<TerrainCoreProvenanceSourceCount> Sources;
};

/** Deterministic path for the persisted LOD0 provenance record associated with a tile. */
SKIAPPLICATION_API std::string TerrainCoreProvenanceSidecarPath(
    const TerrainCoreTileKey& Key);

/**
 * Encodes one durable, content/grid-bound LOD0 provenance record. The source
 * index plane uses the TerrainCore manifest source order (primary, then extras).
 */
SKIAPPLICATION_API bool EncodeTerrainCoreTileProvenanceSidecar(
    const SkiDomain::TerrainCoreManifest& Manifest, const TerrainCoreTileKey& Key,
    const std::vector<std::uint8_t>& Validity,
    const std::vector<std::uint8_t>& Provenance,
    const std::vector<std::uint8_t>& SourceIndices,
    std::vector<std::uint8_t>& OutBytes, std::string& OutError);

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
    // Legacy TerrainCore repositories remain readable but cannot report
    // provenance as verified. Concrete repositories override this when they
    // were opened with a durable sidecar reader.
    virtual bool ReadVerifiedProvenanceSummary(TerrainCoreProvenanceSummary& OutSummary,
        std::string& OutError,
        const TerrainCoreReadCancellation& Cancellation = {}) const
    {
        OutSummary = {};
        OutError = "TerrainCore provenance sidecars are unavailable";
        (void)Cancellation;
        return false;
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
    using ProvenanceSidecarReader = std::function<bool(
        const SkiDomain::TerrainCoreTileDescriptor&, std::uint64_t MaximumBytes,
        std::vector<std::uint8_t>&, std::string&,
        const TerrainCoreReadCancellation&)>;

    static std::shared_ptr<TerrainCoreRepository> Create(
        SkiDomain::TerrainCoreManifest Manifest, TileReader Reader,
        std::string& OutError);
    static std::shared_ptr<TerrainCoreRepository> Create(
        SkiDomain::TerrainCoreManifest Manifest, TileReader Reader,
        ProvenanceSidecarReader ProvenanceReader, std::string& OutError);

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata() const override;
    bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError) const override;
    bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError, const TerrainCoreReadCancellation& Cancellation) const override;
    bool ReadVerifiedProvenanceSummary(TerrainCoreProvenanceSummary& OutSummary,
        std::string& OutError,
        const TerrainCoreReadCancellation& Cancellation = {}) const override;

private:
    TerrainCoreRepository(std::shared_ptr<const SkiDomain::TerrainCoreManifest> InManifest,
        TileReader InReader, ProvenanceSidecarReader InProvenanceReader);

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Manifest;
    TileReader Reader;
    ProvenanceSidecarReader ProvenanceReader;
};
}
