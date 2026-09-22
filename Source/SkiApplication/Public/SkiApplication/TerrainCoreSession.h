#pragma once

#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiDomain/Revision.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace SkiApplication
{
struct SKIAPPLICATION_API TerrainCoreRevisions
{
    SkiDomain::Revision Canonical = 0;
    SkiDomain::Revision Render = 0;
    SkiDomain::Revision Query = 0;
    SkiDomain::Revision Edit = 0;
};

struct SKIAPPLICATION_API TerrainCoreResidency
{
    std::uint64_t ResidentBytes = 0;
    std::uint32_t ResidentTiles = 0;
    std::uint32_t PendingTiles = 0;
};

struct SKIAPPLICATION_API TerrainCoreSnapshot
{
    std::shared_ptr<const ITerrainCoreRepository> Repository;
    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata;
    std::uint64_t Generation = 0;
    TerrainCoreRevisions Revisions;
    TerrainCoreResidency Residency;

    bool CanonicalReady() const noexcept
    {
        return Repository && Metadata && Revisions.Canonical != 0;
    }
    bool RenderReady() const noexcept
    {
        return CanonicalReady() && Revisions.Render == Revisions.Canonical;
    }
    bool QueryReady() const noexcept
    {
        return CanonicalReady() && Revisions.Query == Revisions.Canonical;
    }
};

class SKIAPPLICATION_API TerrainCoreSession
{
public:
    bool Install(std::shared_ptr<const ITerrainCoreRepository> Repository,
        SkiDomain::Revision InitialRevision = 1);
    // Retained as a compatibility rejection path: a revision cannot advance without the
    // repository that owns its edited samples. Use PublishEdit for all edit transitions.
    bool AdvanceEdit(SkiDomain::Revision ExpectedRevision,
        SkiDomain::Revision& OutRevision);
    // Atomically publishes an immutable edited repository and advances both generation and
    // canonical/edit revision. A stale generation or revision leaves the session unchanged.
    bool PublishEdit(std::uint64_t ExpectedGeneration,
        SkiDomain::Revision ExpectedRevision,
        std::shared_ptr<const ITerrainCoreRepository> Repository,
        SkiDomain::Revision PublishedRevision,
        TerrainCoreSnapshot& OutSnapshot);
    bool AcknowledgeRender(std::uint64_t ExpectedGeneration,
        SkiDomain::Revision Revision);
    bool AcknowledgeQuery(std::uint64_t ExpectedGeneration,
        SkiDomain::Revision Revision);
    bool ReportResidency(std::uint64_t ExpectedGeneration,
        const TerrainCoreResidency& Residency);
    TerrainCoreSnapshot Snapshot() const;

private:
    mutable std::mutex Mutex;
    std::shared_ptr<const ITerrainCoreRepository> CurrentRepository;
    std::shared_ptr<const SkiDomain::TerrainCoreManifest> CurrentMetadata;
    std::uint64_t Generation = 0;
    TerrainCoreRevisions Revisions;
    TerrainCoreResidency Residency;
};
}
