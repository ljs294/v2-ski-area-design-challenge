#include "SkiApplication/TerrainSession.h"

bool SkiApplication::TerrainSession::Install(SkiDomain::Heightfield Heightfield,
    SkiDomain::TerrainManifest Manifest, std::vector<std::uint8_t> Cover)
{
    if (!SkiDomain::IsValidHeightfield(Heightfield) || !SkiDomain::ValidateManifest(Manifest).Ok()
        || Heightfield.Width != Manifest.HeightWidth || Heightfield.Height != Manifest.HeightHeight
        || (!Cover.empty() && Cover.size() != static_cast<std::size_t>(Manifest.CoverWidth)
            * Manifest.CoverHeight))
    {
        return false;
    }
    if (Heightfield.CurrentRevision == 0)
    {
        Heightfield.CurrentRevision = 1;
    }
    std::lock_guard Lock(Mutex);
    CurrentHeightfield = std::make_shared<SkiDomain::Heightfield>(std::move(Heightfield));
    CurrentManifest = std::make_shared<SkiDomain::TerrainManifest>(std::move(Manifest));
    CurrentCover = Cover.empty() ? nullptr
        : std::make_shared<std::vector<std::uint8_t>>(std::move(Cover));
    Readiness = {CurrentHeightfield->CurrentRevision, 0, CurrentHeightfield->CurrentRevision, 0, false};
    return true;
}

bool SkiApplication::TerrainSession::ApplyScratchMutation(const SkiDomain::Revision ExpectedRevision,
    const double CenterEastM, const double CenterNorthM, const double RadiusM,
    const double DeltaM, SkiDomain::MutationBounds& OutBounds)
{
    std::lock_guard Lock(Mutex);
    if (!CurrentHeightfield)
    {
        return false;
    }
    auto Candidate = std::make_shared<SkiDomain::Heightfield>(*CurrentHeightfield);
    if (!SkiDomain::ApplyCircularHeightDelta(*Candidate, ExpectedRevision, CenterEastM,
            CenterNorthM, RadiusM, DeltaM, OutBounds))
    {
        return false;
    }
    CurrentHeightfield = std::move(Candidate);
    Readiness.Canonical = CurrentHeightfield->CurrentRevision;
    return true;
}

bool SkiApplication::TerrainSession::AcknowledgeRender(const SkiDomain::Revision Revision)
{
    std::lock_guard Lock(Mutex);
    if (Revision != Readiness.Canonical) return false;
    Readiness.Render = Revision;
    return true;
}

bool SkiApplication::TerrainSession::AcknowledgeQuery(const SkiDomain::Revision Revision)
{
    std::lock_guard Lock(Mutex);
    if (Revision != Readiness.Canonical) return false;
    Readiness.Query = Revision;
    return true;
}

bool SkiApplication::TerrainSession::AcknowledgeCollision(const SkiDomain::Revision Revision)
{
    std::lock_guard Lock(Mutex);
    if (Revision != Readiness.Canonical) return false;
    Readiness.Collision = Revision;
    return true;
}

void SkiApplication::TerrainSession::SetCollisionRequired(const bool Required)
{
    std::lock_guard Lock(Mutex);
    Readiness.CollisionRequired = Required;
}

SkiApplication::TerrainSnapshot SkiApplication::TerrainSession::Snapshot() const
{
    std::lock_guard Lock(Mutex);
    return {CurrentHeightfield, CurrentManifest, CurrentCover,
        CurrentManifest ? CurrentManifest->CoverWidth : 0,
        CurrentManifest ? CurrentManifest->CoverHeight : 0, Readiness};
}
