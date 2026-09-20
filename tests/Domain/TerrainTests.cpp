#include "SkiDomain/Coordinates.h"
#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiDomain/TerrainTile.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
int Failures = 0;

void Require(const bool Condition, const char* Message)
{
    if (!Condition)
    {
        std::cerr << Message << '\n';
        ++Failures;
    }
}

SkiDomain::TerrainManifest Manifest()
{
    SkiDomain::TerrainManifest Value;
    Value.ContentId = std::string(64, 'a');
    Value.Name = "Fixture";
    Value.Source = "fixture";
    Value.RequestedBounds = {-121.52, 46.91, -121.48, 46.95};
    Value.ActualBounds = Value.RequestedBounds;
    Value.LocalOrigin = {46.93, -121.50, 1500.0};
    Value.HeightWidth = 4;
    Value.HeightHeight = 3;
    Value.EastSpacingM = 10.0;
    Value.NorthSpacingM = 10.0;
    Value.Assets.push_back({"elevation.f32le", "heightfield", std::string(64, 'b'), 48, true, {}});
    Value.Assets.push_back({"optional/context.json", "vector-context", {}, 0, false, "provider unavailable"});
    return Value;
}

SkiDomain::Heightfield Field()
{
    SkiDomain::Heightfield Value;
    Value.Width = 4;
    Value.Height = 3;
    Value.WestM = 100.0;
    Value.NorthM = 200.0;
    Value.EastSpacingM = 10.0;
    Value.NorthSpacingM = 20.0;
    Value.CurrentRevision = 1;
    Value.Samples = {100, 100, 100, 100, 90, 90, 90, 90, 80, 80, 80, 80};
    return Value;
}
}

int main()
{
    SkiDomain::LocalFrame Frame;
    Require(SkiDomain::TryMakeLocalFrame({46.93, -121.50, 1500.0}, Frame), "valid local frame rejected");
    const SkiDomain::EnuPoint Origin = SkiDomain::ToEnu(Frame, Frame.Origin);
    Require(std::abs(Origin.EastM) < 1e-7 && std::abs(Origin.NorthM) < 1e-7
        && std::abs(Origin.UpM) < 1e-7, "ENU origin did not round-trip to zero");
    const SkiDomain::UnrealPointCm Unreal = SkiDomain::ToUnrealCentimeters({2.0, 3.0, 4.0});
    Require(Unreal.XNorthCm == 300.0 && Unreal.YEastCm == 200.0 && Unreal.ZUpCm == 400.0,
        "Unreal axis/centimeter mapping is wrong");
    Require(!SkiDomain::TryMakeLocalFrame({91.0, 0.0, 0.0}, Frame), "invalid latitude accepted");

    SkiDomain::TerrainManifest Package = Manifest();
    Require(SkiDomain::ValidateManifest(Package).Ok(), "valid manifest rejected");
    Package.Assets[0].Path = "../escape.bin";
    Require(SkiDomain::ValidateManifest(Package).Error == SkiDomain::ManifestError::InvalidAssetPath,
        "path traversal accepted");
    Package = Manifest();
    Package.Assets.push_back(Package.Assets.front());
    Require(SkiDomain::ValidateManifest(Package).Error == SkiDomain::ManifestError::DuplicateAssetPath,
        "duplicate asset accepted");
    Package = Manifest();
    Package.HeightWidth = 4000000000U;
    Require(SkiDomain::ValidateManifest(Package).Error == SkiDomain::ManifestError::InvalidDimensions,
        "overflowing dimensions accepted");

    SkiDomain::Heightfield Terrain = Field();
    Require(SkiDomain::IsValidHeightfield(Terrain), "valid heightfield rejected");
    const SkiDomain::RayHit Hit = SkiDomain::QueryHeightfield(Terrain,
        {{115.0, 190.0, 200.0}, {0.0, 0.0, -1.0}});
    Require(Hit.Hit && std::abs(Hit.Position.Up - 95.0) < 1e-5 && Hit.SourceRevision == 1,
        "vertical heightfield query missed or returned wrong elevation");
    Terrain.Samples[5] = -9999.0F;
    Require(!SkiDomain::QueryHeightfield(Terrain, {{115.0, 190.0, 200.0}, {0, 0, -1}}).Hit,
        "nodata cell was pickable");
    Terrain = Field();
    const auto Before = Terrain.Samples;
    SkiDomain::MutationBounds Bounds;
    Require(!SkiDomain::ApplyCircularHeightDelta(Terrain, 0, 115.0, 180.0, 20.0, 5.0, Bounds)
        && Terrain.Samples == Before && Terrain.CurrentRevision == 1, "stale mutation changed terrain");
    Require(SkiDomain::ApplyCircularHeightDelta(Terrain, 1, 115.0, 180.0, 20.0, 5.0, Bounds)
        && Terrain.CurrentRevision == 2, "valid mutation did not advance revision");

    SkiDomain::TerrainTileMesh Full;
    SkiDomain::TerrainTileMesh Coarse;
    Require(SkiDomain::BuildTerrainTile(Terrain, {0, 0, 0}, true, 5.0, Full), "full tile build failed");
    Require(SkiDomain::BuildTerrainTile(Terrain, {0, 0, 1}, false, 0.0, Coarse), "coarse tile build failed");
    Require(!Full.Vertices.empty() && !Full.Indices.empty() && Full.SourceRevision == 2,
        "tile output is empty or stale");
    Require(Full.Vertices.front().EastM == Coarse.Vertices.front().EastM
        && Full.Vertices.front().NorthM == Coarse.Vertices.front().NorthM,
        "LOD tile did not share the north-west sample");
    Require(Full.Vertices[11].EastM == Coarse.Vertices[5].EastM
        && Full.Vertices[11].NorthM == Coarse.Vertices[5].NorthM,
        "LOD tile did not share the south-east sample");
    Require(SkiDomain::ValidateAdjacentLods({{0, 0, 0}, {1, 0, 1}, {1, 1, 2}}),
        "adjacent LOD delta one rejected");
    Require(!SkiDomain::ValidateAdjacentLods({{0, 0, 0}, {1, 0, 2}}),
        "adjacent LOD delta two accepted");

    SkiDomain::TerrainReadiness Readiness{2, 2, 2, 0, false};
    Require(Readiness.IsReady(), "ready revisions rejected");
    Readiness.Query = 1;
    Require(!Readiness.IsReady(), "stale query revision accepted");
    return Failures == 0 ? 0 : 1;
}
