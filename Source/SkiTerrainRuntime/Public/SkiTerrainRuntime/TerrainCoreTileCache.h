#pragma once

#include "SkiApplication/TerrainCoreRepository.h"

#include <cstdint>
#include <memory>

namespace SkiTerrainRuntime
{
enum class TerrainCoreQueryStatus : std::uint8_t
{
    Ready,
    Pending,
    Invalid,
    Failed,
};

enum class TerrainCoreRequestStatus : std::uint8_t
{
    NotRequested,
    Pending,
    Resident,
    Failed,
};

struct SKITERRAINRUNTIME_API TerrainCoreCacheConfig
{
    std::uint64_t ByteBudget = 512ULL * 1024ULL * 1024ULL;
    std::uint32_t MaximumWorkerJobs = 2;
    std::uint32_t MaximumPublicationsPerPump = 4;
};

struct SKITERRAINRUNTIME_API TerrainCoreCacheStats
{
    std::uint64_t ConfiguredBudgetBytes = 0;
    std::uint64_t ResidentBytes = 0;
    std::uint64_t ReservedBytes = 0;
    std::uint64_t AccountedBytes = 0;
    std::uint32_t ResidentTiles = 0;
    std::uint32_t PendingTiles = 0;
    std::uint32_t ActiveWorkerJobs = 0;
    std::uint32_t FailedTiles = 0;
    std::uint64_t RejectedPublications = 0;
    std::uint64_t EvictionCount = 0;
    std::uint64_t PeakResidentBytes = 0;
    std::uint64_t PeakAccountedBytes = 0;
    std::uint32_t PeakWorkerJobs = 0;
    std::uint32_t PeakPublicationsPerPump = 0;
};

class SKITERRAINRUNTIME_API TerrainCoreTileCache
{
public:
    struct State;

    TerrainCoreTileCache(std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repository,
        std::uint64_t Generation, TerrainCoreCacheConfig Config = {});
    ~TerrainCoreTileCache();

    TerrainCoreTileCache(const TerrainCoreTileCache&) = delete;
    TerrainCoreTileCache& operator=(const TerrainCoreTileCache&) = delete;

    bool RequestTile(const SkiApplication::TerrainCoreTileKey& Key, bool Pin = false);
    bool ReleasePin(const SkiApplication::TerrainCoreTileKey& Key);
    TerrainCoreRequestStatus TileStatus(const SkiApplication::TerrainCoreTileKey& Key,
        std::string* OutError = nullptr) const;
    // Clears a terminal failure and attempts one new single-flight request.
    bool RetryTile(const SkiApplication::TerrainCoreTileKey& Key, bool Pin = false);
    std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> FindResident(
        const SkiApplication::TerrainCoreTileKey& Key);
    std::uint32_t PumpPublications(std::uint32_t Maximum = 0);
    // Cancels the current generation and drops queued/completed work. In-flight readers
    // retain their reservation until they observe cancellation and exit.
    void CancelPending();
    void ResetGeneration(std::uint64_t Generation,
        std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repository = {});
    TerrainCoreCacheStats Stats() const;

    // Returns Pending and queues the finest installed tile when it is not resident. Visual
    // derivatives are never consulted by this canonical path.
    TerrainCoreQueryStatus QueryCanonicalSample(std::uint32_t Column, std::uint32_t Row,
        float& OutHeightM, bool& OutValid);
    TerrainCoreQueryStatus QueryCanonicalCell(std::uint32_t Column, std::uint32_t Row,
        double EastFraction, double SouthFraction, double& OutHeightM, bool& OutValid);

    // Deterministic test/support hook. It waits for decode jobs, not publication.
    bool WaitForWorkers(double TimeoutSeconds) const;

private:
    std::shared_ptr<State> Shared;
};
}
