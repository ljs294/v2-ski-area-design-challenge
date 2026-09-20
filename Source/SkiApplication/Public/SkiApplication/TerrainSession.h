#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainPackage.h"

#include <memory>
#include <mutex>

namespace SkiApplication
{
struct SKIAPPLICATION_API TerrainSnapshot
{
    std::shared_ptr<const SkiDomain::Heightfield> Heightfield;
    std::shared_ptr<const SkiDomain::TerrainManifest> Manifest;
    std::shared_ptr<const std::vector<std::uint8_t>> Cover;
    std::uint32_t CoverWidth = 0;
    std::uint32_t CoverHeight = 0;
    SkiDomain::TerrainReadiness Readiness;
};

class SKIAPPLICATION_API TerrainSession
{
public:
    bool Install(SkiDomain::Heightfield Heightfield, SkiDomain::TerrainManifest Manifest,
        std::vector<std::uint8_t> Cover = {});
    bool ApplyScratchMutation(SkiDomain::Revision ExpectedRevision, double CenterEastM,
        double CenterNorthM, double RadiusM, double DeltaM, SkiDomain::MutationBounds& OutBounds);
    bool AcknowledgeRender(SkiDomain::Revision Revision);
    bool AcknowledgeQuery(SkiDomain::Revision Revision);
    bool AcknowledgeCollision(SkiDomain::Revision Revision);
    void SetCollisionRequired(bool Required);
    TerrainSnapshot Snapshot() const;

private:
    mutable std::mutex Mutex;
    std::shared_ptr<SkiDomain::Heightfield> CurrentHeightfield;
    std::shared_ptr<SkiDomain::TerrainManifest> CurrentManifest;
    std::shared_ptr<std::vector<std::uint8_t>> CurrentCover;
    SkiDomain::TerrainReadiness Readiness;
};
}
