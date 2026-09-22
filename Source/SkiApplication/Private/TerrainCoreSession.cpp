#include "SkiApplication/TerrainCoreSession.h"

#include <limits>

bool SkiApplication::TerrainCoreSession::Install(
    std::shared_ptr<const ITerrainCoreRepository> Repository,
    const SkiDomain::Revision InitialRevision)
{
    if (!Repository || InitialRevision == 0)
    {
        return false;
    }
    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata = Repository->Metadata();
    if (!Metadata || !SkiDomain::ValidateTerrainCore(*Metadata).Ok())
    {
        return false;
    }
    std::lock_guard Lock(Mutex);
    if (Generation == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }
    ++Generation;
    CurrentRepository = std::move(Repository);
    CurrentMetadata = std::move(Metadata);
    Revisions = {InitialRevision, 0, 0, InitialRevision};
    Residency = {};
    return true;
}

bool SkiApplication::TerrainCoreSession::AdvanceEdit(
    const SkiDomain::Revision ExpectedRevision, SkiDomain::Revision& OutRevision)
{
    OutRevision = 0;
    (void)ExpectedRevision;
    return false;
}

bool SkiApplication::TerrainCoreSession::PublishEdit(
    const std::uint64_t ExpectedGeneration,
    const SkiDomain::Revision ExpectedRevision,
    std::shared_ptr<const ITerrainCoreRepository> Repository,
    const SkiDomain::Revision PublishedRevision,
    TerrainCoreSnapshot& OutSnapshot)
{
    OutSnapshot = {};
    if (!Repository || PublishedRevision == 0 || PublishedRevision <= ExpectedRevision)
    {
        return false;
    }
    SkiDomain::Revision RepositoryBaseRevision = 0;
    SkiDomain::Revision RepositoryEditRevision = 0;
    if (!Repository->EditTransition(RepositoryBaseRevision, RepositoryEditRevision)
        || RepositoryEditRevision != PublishedRevision
        || RepositoryBaseRevision != ExpectedRevision)
    {
        return false;
    }
    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata = Repository->Metadata();
    if (!Metadata || !SkiDomain::ValidateTerrainCore(*Metadata).Ok()) return false;

    std::lock_guard Lock(Mutex);
    if (!CurrentRepository || ExpectedGeneration != Generation
        || ExpectedRevision != Revisions.Canonical
        || Generation == std::numeric_limits<std::uint64_t>::max()
        || !CurrentMetadata || Metadata->ContentId != CurrentMetadata->ContentId)
    {
        return false;
    }
    ++Generation;
    CurrentRepository = std::move(Repository);
    CurrentMetadata = std::move(Metadata);
    Revisions = {PublishedRevision, 0, 0, PublishedRevision};
    Residency = {};
    OutSnapshot = {CurrentRepository, CurrentMetadata, Generation, Revisions, Residency};
    return true;
}

bool SkiApplication::TerrainCoreSession::AcknowledgeRender(
    const std::uint64_t ExpectedGeneration, const SkiDomain::Revision Revision)
{
    std::lock_guard Lock(Mutex);
    if (!CurrentRepository || Revision == 0 || ExpectedGeneration != Generation
        || Revision != Revisions.Canonical) return false;
    Revisions.Render = Revision;
    return true;
}

bool SkiApplication::TerrainCoreSession::AcknowledgeQuery(
    const std::uint64_t ExpectedGeneration, const SkiDomain::Revision Revision)
{
    std::lock_guard Lock(Mutex);
    if (!CurrentRepository || Revision == 0 || ExpectedGeneration != Generation
        || Revision != Revisions.Canonical) return false;
    Revisions.Query = Revision;
    return true;
}

bool SkiApplication::TerrainCoreSession::ReportResidency(
    const std::uint64_t ExpectedGeneration, const TerrainCoreResidency& InResidency)
{
    std::lock_guard Lock(Mutex);
    if (!CurrentRepository || ExpectedGeneration != Generation) return false;
    Residency = InResidency;
    return true;
}

SkiApplication::TerrainCoreSnapshot SkiApplication::TerrainCoreSession::Snapshot() const
{
    std::lock_guard Lock(Mutex);
    return {CurrentRepository, CurrentMetadata, Generation, Revisions, Residency};
}
