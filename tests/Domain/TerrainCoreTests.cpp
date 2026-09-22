#include "SkiDomain/TerrainCore.h"

#include <cmath>
#include <iostream>
#include <string>

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

SkiDomain::TerrainCoreManifest Manifest(const std::uint32_t Width = 511,
    const std::uint32_t Height = 300)
{
    SkiDomain::TerrainCoreManifest Value;
    Value.ContentId = std::string(64, 'a');
    Value.GeneratorVersion = "terraincore-test";
    Value.ProcessingVersions = {"height-v1", "lod-v1"};
    Value.Source.SourceId = "fixture";
    Value.Source.Product = "synthetic-bare-earth";
    Value.Source.AcquisitionEpoch = "2026";
    Value.Source.HorizontalCrs = "EPSG:32610";
    Value.Source.HorizontalDatum = "NAD83";
    Value.Source.VerticalDatum = "NAVD88";
    Value.Source.License = "CC0";
    Value.Source.Attribution = "fixture";
    Value.Source.NativeEastSpacingM = 1.0;
    Value.Source.NativeNorthSpacingM = 1.0;
    Value.LocalOrigin = {46.93, -121.50, 1500.0};
    Value.Width = Width;
    Value.Height = Height;
    Value.DeliveredEastSpacingM = 1.0;
    Value.DeliveredNorthSpacingM = 2.0;
    Value.SampleCenterBounds = {100.0, 200.0, 100.0 + Width - 1.0,
        200.0 + (Height - 1.0) * 2.0};
    SkiDomain::ComputeTerrainCoreBounds(Width, Height, 1.0, 2.0,
        Value.SampleCenterBounds, Value.OuterBounds);
    SkiDomain::TerrainCoreTilePlan Plan;
    SkiDomain::PlanTerrainCoreTiles(Width, Height, Plan);
    Value.Tiles = std::move(Plan.Tiles);
    Value.Shards.push_back({"shards/terrain-00.bin", std::string(64, 'f'), 0});
    std::uint64_t Offset = 0;
    for (std::size_t Index = 0; Index < Value.Tiles.size(); ++Index)
    {
        auto& Tile = Value.Tiles[Index];
        const std::string Stem = "tiles/" + std::to_string(Tile.LodFactor) + "/"
            + std::to_string(Tile.TileY) + "-" + std::to_string(Tile.TileX);
        (void)Stem;
        Tile.HeightPath = Value.Shards[0].Path;
        Tile.ValidityPath = Value.Shards[0].Path;
        Tile.HeightSha256 = std::string(64, static_cast<char>('a' + Index % 6));
        Tile.ValiditySha256 = std::string(64, static_cast<char>('1' + Index % 8));
        Tile.HeightShardIndex = 0;
        Tile.HeightOffset = Offset;
        Tile.HeightBytes = 1024;
        Tile.HeightRawBytes = (static_cast<std::uint64_t>(Tile.CoreWidth) + Tile.HaloWest
            + Tile.HaloEast) * (static_cast<std::uint64_t>(Tile.CoreHeight) + Tile.HaloNorth
            + Tile.HaloSouth) * sizeof(float);
        Offset += Tile.HeightBytes;
        Tile.ValidityShardIndex = 0;
        Tile.ValidityOffset = Offset;
        Tile.ValidityBytes = 64;
        const std::uint64_t StoredSamples = (static_cast<std::uint64_t>(Tile.CoreWidth)
            + Tile.HaloWest + Tile.HaloEast) * (static_cast<std::uint64_t>(Tile.CoreHeight)
            + Tile.HaloNorth + Tile.HaloSouth);
        Tile.ValidityRawBytes = (StoredSamples + 7ULL) / 8ULL;
        Offset += Tile.ValidityBytes;
        Tile.ProvenanceId = "fixture";
        Tile.ProcessingVersion = "lod-v1";
    }
    Value.Shards[0].Bytes = Offset;
    return Value;
}
}

int main()
{
    SkiDomain::MetricBounds Outer;
    Require(SkiDomain::ComputeTerrainCoreBounds(4, 3, 2.0, 5.0,
        {10.0, 20.0, 16.0, 30.0}, Outer), "asymmetric bounds rejected");
    Require(Outer.WestM == 9.0 && Outer.EastM == 17.0
        && Outer.SouthM == 17.5 && Outer.NorthM == 32.5,
        "outer/sample-center bounds math is wrong");
    Require(!SkiDomain::ComputeTerrainCoreBounds(4, 3, 2.0, 5.0,
        {10.0, 20.0, 15.0, 30.0}, Outer), "inconsistent center span accepted");

    SkiDomain::TerrainCoreTilePlan Plan;
    Require(SkiDomain::PlanTerrainCoreTiles(511, 300, Plan), "tile plan failed");
    Require(Plan.Tiles.size() == 8, "unexpected asymmetric multi-LOD tile count");
    Require(Plan.Tiles[0].CoreWidth == 256 && Plan.Tiles[1].StartColumn == 255
        && Plan.Tiles[1].CoreWidth == 256, "shared border was not planned at sample 255");
    Require(Plan.Tiles[2].StartRow == 255 && Plan.Tiles[2].CoreHeight == 45,
        "partial south edge tile is wrong");
    Require(Plan.Tiles[0].HaloEast == 1 && Plan.Tiles[0].HaloSouth == 1
        && Plan.Tiles[3].HaloWest == 1 && Plan.Tiles[3].HaloNorth == 1,
        "normal-neighbor halo is wrong");
    Require(Plan.Tiles.back().LodFactor == 16,
        "deterministic 1/2/4/8/16 LOD plan missing");

    SkiDomain::TerrainCoreManifest Core = Manifest();
    Require(SkiDomain::ValidateTerrainCore(Core).Ok(), "valid TerrainCore rejected");
    Core.Shards[0].Path = "../escape.f32z";
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidAssetPath, "traversal path accepted");
    Require(!SkiDomain::IsSafeTerrainCorePath("tiles/NUL.bin")
        && !SkiDomain::IsSafeTerrainCorePath("tiles/value. ")
        && !SkiDomain::IsSafeTerrainCorePath(std::string("tiles/bad\x1f.bin", 14)),
        "Windows-reserved, trailing-dot/space, or control-character path accepted");
    Core = Manifest();
    Core.Shards[0].Sha256[0] = 'F';
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidAssetHash, "noncanonical uppercase hash accepted");
    Core = Manifest();
    Core.Shards.push_back(Core.Shards.front());
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::DuplicateAssetPath, "duplicate path accepted");
    Core = Manifest();
    Core.Shards.push_back(Core.Shards.front());
    Core.Shards.back().Path = "SHARDS/TERRAIN-00.BIN";
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::DuplicateAssetPath,
        "case-colliding Windows shard path accepted");
    Core = Manifest();
    ++Core.Tiles[1].StartColumn;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::TileCoverage, "gap/overlap geometry accepted");
    Core = Manifest();
    Core.Tiles.pop_back();
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::MissingLod, "missing LOD accepted");
    Core = Manifest();
    Core.OuterBounds.EastM += 1.0;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidBounds, "inconsistent outer bounds accepted");
    Core = Manifest();
    Core.Width = 0xffffffffU;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidDimensions, "overflow dimensions accepted");
    Core = Manifest();
    ++Core.Tiles[0].HeightOffset;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::ShardCoverage, "shard gap/overlap accepted");
    Core = Manifest();
    ++Core.Tiles[0].HeightRawBytes;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidTile, "incorrect raw tile length accepted");
    Core = Manifest();
    Core.Tiles[0].HeightBytes = SkiDomain::TerrainCoreDeflateBound(
        Core.Tiles[0].HeightRawBytes) + 1ULL;
    Core.Shards[0].Bytes += Core.Tiles[0].HeightBytes - 1024ULL;
    for (std::size_t Index = 1; Index < Core.Tiles.size(); ++Index)
    {
        Core.Tiles[Index].HeightOffset += Core.Tiles[0].HeightBytes - 1024ULL;
        Core.Tiles[Index].ValidityOffset += Core.Tiles[0].HeightBytes - 1024ULL;
    }
    Core.Tiles[0].ValidityOffset = Core.Tiles[0].HeightBytes;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::AssetTooLarge,
        "codec-impossible compressed height declaration accepted");
    Core = Manifest();
    Core.LocalOrigin.LatitudeDeg = 91.0;
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidMetadata,
        "invalid TerrainCore local origin accepted");
    Core = Manifest();
    Core.ProcessingVersions.push_back(Core.ProcessingVersions.front());
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidMetadata,
        "duplicate processing version accepted");
    Core = Manifest();
    Core.ProcessingVersions.resize(SkiDomain::TerrainCoreMaxProcessingVersions + 1,
        "amplification");
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidMetadata,
        "oversized processing-version array accepted");
    Core = Manifest();
    Core.AdditionalSources.resize(SkiDomain::TerrainCoreMaxAdditionalSources + 1,
        Core.Source);
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidMetadata,
        "oversized additional-source array accepted");
    Core = Manifest();
    Core.Tiles[0].ProvenanceId = "undeclared-source";
    Require(SkiDomain::ValidateTerrainCore(Core).Error
        == SkiDomain::TerrainCoreError::InvalidTile, "undeclared provenance accepted");
    Core = Manifest();
    Require(SkiDomain::ValidateTerrainCore(Core,
        SkiDomain::TerrainCoreMaxManifestBytes + 1).Error
        == SkiDomain::TerrainCoreError::ManifestTooLarge, "oversized manifest accepted");
    Core = Manifest();
    Core.Shards.clear();
    for (std::size_t Index = 0; Index < 32; ++Index)
    {
        Core.Shards.push_back({"shards/" + std::to_string(Index) + ".tcs",
            std::string(64, 'a'), SkiDomain::TerrainCoreMaxAssetBytes});
    }
    Require(SkiDomain::ValidateTerrainCore(Core, 1).Error
        == SkiDomain::TerrainCoreError::PackageTooLarge,
        "manifest bytes were omitted from installed-size accounting");

    SkiDomain::LodHysteresisPolicy Policy;
    Require(SkiDomain::SelectTerrainCoreLod(0.1, 0, Policy) == 1,
        "LOD did not coarsen one level");
    Require(SkiDomain::SelectTerrainCoreLod(0.6, 2, Policy) == 1,
        "LOD did not refine one level");
    Require(SkiDomain::SelectTerrainCoreLod(0.3, 2, Policy) == 2,
        "LOD hysteresis did not hold");
    Require(SkiDomain::AreTerrainCoreLodsAdjacent(2, 3)
        && !SkiDomain::AreTerrainCoreLodsAdjacent(1, 3), "LOD adjacency policy is wrong");

    SkiDomain::TerrainEditSet Before;
    Before.TerrainCoreId = std::string(64, 'a');
    Before.BaseRevision = 4;
    Before.EditRevision = 4;
    SkiDomain::TerrainEditSet After;
    After.TerrainCoreId = Before.TerrainCoreId;
    After.BaseRevision = 4;
    After.EditRevision = 5;
    After.Deltas = {{255, 255, 2.0F}, {256, 255, -1.0F}};
    Require(SkiDomain::ValidateTerrainEditSet(After, 511, 300).Ok(),
        "valid sparse edit set rejected");
    SkiDomain::TerrainChangeReceipt Receipt;
    Require(SkiDomain::BuildTerrainChangeReceipt(Before, After, 511, 300, Receipt)
        && Receipt.BeforeRevision == 4 && Receipt.AfterRevision == 5
        && Receipt.MinColumn == 255 && Receipt.MaxColumn == 256
        && Receipt.AffectedTileIds.size() == 4, "bounded change receipt/ring was not produced");
    After.Deltas.push_back(After.Deltas.front());
    Require(SkiDomain::ValidateTerrainEditSet(After, 511, 300).Error
        == SkiDomain::TerrainCoreError::DuplicateEdit, "duplicate sparse edit accepted");
    After.Deltas.pop_back();
    After.Deltas[0].Column = 511;
    Require(SkiDomain::ValidateTerrainEditSet(After, 511, 300).Error
        == SkiDomain::TerrainCoreError::InvalidEdit, "out-of-range sparse edit accepted");

    return Failures == 0 ? 0 : 1;
}
