#pragma once

#include "SkiApplication/TerrainCoreRepository.h"

#include <memory>
#include <string>

namespace SkiApplication
{
// Immutable adapter for an offline-reopened TerrainEditSet. The wrapped repository remains
// unchanged; sparse deltas are projected onto every finest-level core/halo occurrence.
class SKIAPPLICATION_API TerrainCoreEditedRepository final : public ITerrainCoreRepository
{
public:
    static std::shared_ptr<TerrainCoreEditedRepository> Create(
        std::shared_ptr<const ITerrainCoreRepository> BaseRepository,
        SkiDomain::TerrainEditSet Edits, SkiDomain::Revision ExpectedBaseRevision,
        std::string& OutError);

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata() const override;
    bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError) const override;
    bool ReadTile(const TerrainCoreTileKey& Key, TerrainCoreTilePayload& OutTile,
        std::string& OutError, const TerrainCoreReadCancellation& Cancellation) const override;
    bool EditTransition(SkiDomain::Revision& OutBaseRevision,
        SkiDomain::Revision& OutEditRevision) const noexcept override;
    bool FlattenEditOverlay(std::shared_ptr<const ITerrainCoreRepository>& OutBaseRepository,
        std::shared_ptr<const SkiDomain::TerrainEditSet>& OutEdits) const noexcept override;
    SkiDomain::Revision EditRevision() const noexcept;
    std::shared_ptr<const SkiDomain::TerrainEditSet> EditSet() const;

private:
    struct Index;
    TerrainCoreEditedRepository(std::shared_ptr<const ITerrainCoreRepository> InBase,
        std::shared_ptr<const SkiDomain::TerrainEditSet> InEdits,
        std::shared_ptr<const Index> InIndex,
        SkiDomain::Revision InTransitionBaseRevision);

    std::shared_ptr<const ITerrainCoreRepository> Base;
    std::shared_ptr<const SkiDomain::TerrainEditSet> Edits;
    std::shared_ptr<const Index> EditIndex;
    SkiDomain::Revision TransitionBaseRevision = 0;
};
}
