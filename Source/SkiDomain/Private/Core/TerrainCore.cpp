#include "SkiDomain/TerrainCore.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
using namespace SkiDomain;

bool FiniteBounds(const MetricBounds& Bounds) noexcept
{
    return std::isfinite(Bounds.WestM) && std::isfinite(Bounds.SouthM)
        && std::isfinite(Bounds.EastM) && std::isfinite(Bounds.NorthM)
        && Bounds.WestM <= Bounds.EastM && Bounds.SouthM <= Bounds.NorthM;
}

bool NearlyEqual(const double A, const double B) noexcept
{
    const double Scale = std::max({1.0, std::abs(A), std::abs(B)});
    return std::abs(A - B) <= Scale * 1.0e-9;
}

std::uint32_t LodDimension(const std::uint32_t BaseDimension,
    const std::uint32_t Factor) noexcept
{
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(BaseDimension) - 1ULL
        + Factor - 1ULL) / Factor + 1ULL);
}

std::uint64_t TileIdentity(const std::uint8_t Lod, const std::uint32_t X,
    const std::uint32_t Y) noexcept
{
    return (static_cast<std::uint64_t>(Lod) << 56U)
        | (static_cast<std::uint64_t>(Y) << 28U) | X;
}

std::string LowerAscii(std::string Value)
{
    std::transform(Value.begin(), Value.end(), Value.begin(), [](const unsigned char Character)
    {
        return static_cast<char>(std::tolower(Character));
    });
    return Value;
}

bool IsWindowsReservedSegment(const std::string& Segment)
{
    const std::size_t Extension = Segment.find('.');
    const std::string Base = LowerAscii(Segment.substr(0, Extension));
    if (Base == "con" || Base == "prn" || Base == "aux" || Base == "nul"
        || Base == "clock$")
    {
        return true;
    }
    return Base.size() == 4 && (Base.starts_with("com") || Base.starts_with("lpt"))
        && Base[3] >= '1' && Base[3] <= '9';
}
}

bool SkiDomain::IsSafeTerrainCorePath(const std::string& Value) noexcept
{
    if (Value.empty() || Value.front() == '/' || Value.front() == '\\'
        || Value.find('\\') != std::string::npos || Value.find(':') != std::string::npos
        || std::any_of(Value.begin(), Value.end(), [](const unsigned char Character)
            { return Character < 0x20U || Character == 0x7fU; }))
    {
        return false;
    }
    std::size_t Start = 0;
    while (Start <= Value.size())
    {
        const std::size_t End = Value.find('/', Start);
        const std::string Segment = Value.substr(Start,
            End == std::string::npos ? std::string::npos : End - Start);
        if (Segment.empty() || Segment == "." || Segment == ".."
            || Segment.back() == '.' || Segment.back() == ' '
            || IsWindowsReservedSegment(Segment))
        {
            return false;
        }
        if (End == std::string::npos)
        {
            break;
        }
        Start = End + 1;
    }
    return true;
}

bool SkiDomain::IsTerrainCoreSha256(const std::string& Value) noexcept
{
    if (Value.size() != 64)
    {
        return false;
    }
    return std::all_of(Value.begin(), Value.end(), [](const char Character)
    {
        return (Character >= '0' && Character <= '9')
            || (Character >= 'a' && Character <= 'f');
    });
}

bool SkiDomain::ComputeTerrainCoreBounds(const std::uint32_t Width,
    const std::uint32_t Height, const double EastSpacingM, const double NorthSpacingM,
    const MetricBounds& SampleCenters, MetricBounds& OutOuterBounds) noexcept
{
    OutOuterBounds = {};
    if (Width < 2 || Height < 2 || !FiniteBounds(SampleCenters)
        || !std::isfinite(EastSpacingM) || !std::isfinite(NorthSpacingM)
        || EastSpacingM <= 0.0 || NorthSpacingM <= 0.0)
    {
        return false;
    }
    const double EastSpan = static_cast<double>(Width - 1U) * EastSpacingM;
    const double NorthSpan = static_cast<double>(Height - 1U) * NorthSpacingM;
    if (!std::isfinite(EastSpan) || !std::isfinite(NorthSpan)
        || !NearlyEqual(SampleCenters.EastM - SampleCenters.WestM, EastSpan)
        || !NearlyEqual(SampleCenters.NorthM - SampleCenters.SouthM, NorthSpan))
    {
        return false;
    }
    OutOuterBounds = {SampleCenters.WestM - EastSpacingM * 0.5,
        SampleCenters.SouthM - NorthSpacingM * 0.5,
        SampleCenters.EastM + EastSpacingM * 0.5,
        SampleCenters.NorthM + NorthSpacingM * 0.5};
    return FiniteBounds(OutOuterBounds);
}

bool SkiDomain::PlanTerrainCoreTiles(const std::uint32_t Width,
    const std::uint32_t Height, TerrainCoreTilePlan& OutPlan) noexcept
{
    OutPlan = {};
    if (Width < 2 || Height < 2
        || static_cast<std::uint64_t>(Width) * Height > TerrainCoreMaxSamples)
    {
        return false;
    }
    OutPlan.Width = Width;
    OutPlan.Height = Height;
    for (std::uint8_t Lod = 0; Lod < TerrainCoreLodFactors.size(); ++Lod)
    {
        const std::uint32_t Factor = TerrainCoreLodFactors[Lod];
        const std::uint32_t LodWidth = LodDimension(Width, Factor);
        const std::uint32_t LodHeight = LodDimension(Height, Factor);
        const std::uint32_t TilesX = (LodWidth - 1U + TerrainCoreTileCells - 1U)
            / TerrainCoreTileCells;
        const std::uint32_t TilesY = (LodHeight - 1U + TerrainCoreTileCells - 1U)
            / TerrainCoreTileCells;
        if (static_cast<std::uint64_t>(OutPlan.Tiles.size())
            + static_cast<std::uint64_t>(TilesX) * TilesY > TerrainCoreMaxTiles)
        {
            OutPlan = {};
            return false;
        }
        for (std::uint32_t Y = 0; Y < TilesY; ++Y)
        {
            for (std::uint32_t X = 0; X < TilesX; ++X)
            {
                TerrainCoreTileDescriptor Tile;
                Tile.LodIndex = Lod;
                Tile.LodFactor = Factor;
                Tile.TileX = X;
                Tile.TileY = Y;
                Tile.StartColumn = X * TerrainCoreTileCells;
                Tile.StartRow = Y * TerrainCoreTileCells;
                Tile.CoreWidth = static_cast<std::uint16_t>(std::min(
                    TerrainCoreTileSamples, LodWidth - Tile.StartColumn));
                Tile.CoreHeight = static_cast<std::uint16_t>(std::min(
                    TerrainCoreTileSamples, LodHeight - Tile.StartRow));
                Tile.HaloWest = Tile.StartColumn > 0 ? TerrainCoreNormalHaloSamples : 0;
                Tile.HaloNorth = Tile.StartRow > 0 ? TerrainCoreNormalHaloSamples : 0;
                Tile.HaloEast = Tile.StartColumn + Tile.CoreWidth < LodWidth
                    ? TerrainCoreNormalHaloSamples : 0;
                Tile.HaloSouth = Tile.StartRow + Tile.CoreHeight < LodHeight
                    ? TerrainCoreNormalHaloSamples : 0;
                OutPlan.Tiles.push_back(std::move(Tile));
            }
        }
    }
    return true;
}

SkiDomain::TerrainCoreValidation SkiDomain::ValidateTerrainCore(
    const TerrainCoreManifest& Manifest, const std::uint64_t SerializedManifestBytes) noexcept
{
    if (Manifest.SchemaVersion != TerrainCoreSchema)
    {
        return {TerrainCoreError::UnsupportedSchema, 0};
    }
    if (SerializedManifestBytes > TerrainCoreMaxManifestBytes)
    {
        return {TerrainCoreError::ManifestTooLarge, 0};
    }
    if (!IsTerrainCoreSha256(Manifest.ContentId))
    {
        return {TerrainCoreError::InvalidContentId, 0};
    }
    if (Manifest.Width < 2 || Manifest.Height < 2
        || static_cast<std::uint64_t>(Manifest.Width) * Manifest.Height > TerrainCoreMaxSamples)
    {
        return {TerrainCoreError::InvalidDimensions, 0};
    }
    if (!std::isfinite(Manifest.DeliveredEastSpacingM)
        || !std::isfinite(Manifest.DeliveredNorthSpacingM)
        || Manifest.DeliveredEastSpacingM <= 0.0 || Manifest.DeliveredNorthSpacingM <= 0.0
        || !std::isfinite(Manifest.Source.NativeEastSpacingM)
        || !std::isfinite(Manifest.Source.NativeNorthSpacingM)
        || Manifest.Source.NativeEastSpacingM <= 0.0 || Manifest.Source.NativeNorthSpacingM <= 0.0)
    {
        return {TerrainCoreError::InvalidSpacing, 0};
    }
    MetricBounds ExpectedOuter;
    if (Manifest.Registration != PixelRegistration::SampleCenter
        || !ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height,
            Manifest.DeliveredEastSpacingM, Manifest.DeliveredNorthSpacingM,
            Manifest.SampleCenterBounds, ExpectedOuter)
        || !FiniteBounds(Manifest.OuterBounds)
        || !NearlyEqual(ExpectedOuter.WestM, Manifest.OuterBounds.WestM)
        || !NearlyEqual(ExpectedOuter.SouthM, Manifest.OuterBounds.SouthM)
        || !NearlyEqual(ExpectedOuter.EastM, Manifest.OuterBounds.EastM)
        || !NearlyEqual(ExpectedOuter.NorthM, Manifest.OuterBounds.NorthM))
    {
        return {TerrainCoreError::InvalidBounds, 0};
    }
    const TerrainCoreSource& Source = Manifest.Source;
    const auto ValidSource = [](const TerrainCoreSource& Candidate) noexcept
    {
        return !Candidate.SourceId.empty() && !Candidate.Product.empty()
            && !Candidate.AcquisitionEpoch.empty() && !Candidate.HorizontalCrs.empty()
            && !Candidate.HorizontalDatum.empty() && !Candidate.VerticalDatum.empty()
            && !Candidate.License.empty() && !Candidate.Attribution.empty()
            && std::isfinite(Candidate.NativeEastSpacingM)
            && std::isfinite(Candidate.NativeNorthSpacingM)
            && Candidate.NativeEastSpacingM > 0.0 && Candidate.NativeNorthSpacingM > 0.0
            && (!Candidate.HasHorizontalAccuracy
                || (std::isfinite(Candidate.HorizontalAccuracyM)
                    && Candidate.HorizontalAccuracyM >= 0.0))
            && (!Candidate.HasVerticalAccuracy
                || (std::isfinite(Candidate.VerticalAccuracyM)
                    && Candidate.VerticalAccuracyM >= 0.0));
    };
    if (Manifest.GeneratorVersion.empty() || Manifest.ProcessingVersions.empty()
        || Manifest.ProcessingVersions.size() > TerrainCoreMaxProcessingVersions
        || Manifest.AdditionalSources.size() > TerrainCoreMaxAdditionalSources
        || !ValidSource(Source)
        || !IsValidGeodetic(Manifest.LocalOrigin)
        || Manifest.RowOrientation != "north-to-south"
        || Manifest.HeightEncoding != "f32le-deflate-fixed-v1"
        || Manifest.ValidityEncoding != "bitset-deflate-fixed-v1"
        || std::any_of(Manifest.ProcessingVersions.begin(), Manifest.ProcessingVersions.end(),
            [](const std::string& Version) { return Version.empty(); }))
    {
        return {TerrainCoreError::InvalidMetadata, 0};
    }
    std::unordered_set<std::string> ProcessingVersions;
    for (const std::string& Version : Manifest.ProcessingVersions)
    {
        if (!ProcessingVersions.insert(Version).second)
        {
            return {TerrainCoreError::InvalidMetadata, 0};
        }
    }
    std::unordered_set<std::string> ProvenanceIds{Source.SourceId};
    for (const TerrainCoreSource& Additional : Manifest.AdditionalSources)
    {
        if (!ValidSource(Additional) || !ProvenanceIds.insert(Additional.SourceId).second)
        {
            return {TerrainCoreError::InvalidMetadata, 0};
        }
    }
    if (Manifest.Tiles.empty() || Manifest.Tiles.size() > TerrainCoreMaxTiles)
    {
        return {TerrainCoreError::TooManyTiles, 0};
    }

    if (Manifest.Shards.empty() || Manifest.Shards.size() > TerrainCoreMaxShards)
    {
        return {TerrainCoreError::TooManyShards, 0};
    }
    std::unordered_set<std::string> ShardPaths;
    std::uint64_t TotalBytes = SerializedManifestBytes;
    for (std::size_t Index = 0; Index < Manifest.Shards.size(); ++Index)
    {
        const TerrainCoreShardDescriptor& Shard = Manifest.Shards[Index];
        if (!IsSafeTerrainCorePath(Shard.Path))
        {
            return {TerrainCoreError::InvalidAssetPath, Index};
        }
        if (!ShardPaths.insert(LowerAscii(Shard.Path)).second)
        {
            return {TerrainCoreError::DuplicateAssetPath, Index};
        }
        if (!IsTerrainCoreSha256(Shard.Sha256))
        {
            return {TerrainCoreError::InvalidAssetHash, Index};
        }
        if (Shard.Bytes == 0 || Shard.Bytes > TerrainCoreMaxAssetBytes)
        {
            return {Shard.Bytes == 0 ? TerrainCoreError::InvalidShard
                                    : TerrainCoreError::AssetTooLarge, Index};
        }
        if (TotalBytes > TerrainCoreMaxInstalledBytes - Shard.Bytes)
        {
            return {TerrainCoreError::PackageTooLarge, Index};
        }
        TotalBytes += Shard.Bytes;
    }

    TerrainCoreTilePlan Expected;
    if (!PlanTerrainCoreTiles(Manifest.Width, Manifest.Height, Expected)
        || Expected.Tiles.size() != Manifest.Tiles.size())
    {
        return {TerrainCoreError::MissingLod, 0};
    }
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> Ranges(
        Manifest.Shards.size());
    for (std::size_t Index = 0; Index < Manifest.Tiles.size(); ++Index)
    {
        const TerrainCoreTileDescriptor& Tile = Manifest.Tiles[Index];
        const TerrainCoreTileDescriptor& Geometry = Expected.Tiles[Index];
        if (Tile.LodIndex != Geometry.LodIndex || Tile.LodFactor != Geometry.LodFactor
            || Tile.TileX != Geometry.TileX || Tile.TileY != Geometry.TileY
            || Tile.StartColumn != Geometry.StartColumn || Tile.StartRow != Geometry.StartRow
            || Tile.CoreWidth != Geometry.CoreWidth || Tile.CoreHeight != Geometry.CoreHeight
            || Tile.HaloWest != Geometry.HaloWest || Tile.HaloNorth != Geometry.HaloNorth
            || Tile.HaloEast != Geometry.HaloEast || Tile.HaloSouth != Geometry.HaloSouth)
        {
            return {TerrainCoreError::TileCoverage, Index};
        }
        const std::uint64_t StoredWidth = static_cast<std::uint64_t>(Tile.CoreWidth)
            + Tile.HaloWest + Tile.HaloEast;
        const std::uint64_t StoredHeight = static_cast<std::uint64_t>(Tile.CoreHeight)
            + Tile.HaloNorth + Tile.HaloSouth;
        const std::uint64_t StoredSamples = StoredWidth * StoredHeight;
        if (ProvenanceIds.find(Tile.ProvenanceId) == ProvenanceIds.end()
            || ProcessingVersions.find(Tile.ProcessingVersion) == ProcessingVersions.end()
            || Tile.HeightBytes == 0 || Tile.ValidityBytes == 0
            || Tile.HeightRawBytes != StoredSamples * sizeof(float)
            || Tile.ValidityRawBytes != (StoredSamples + 7ULL) / 8ULL
            || Tile.HeightShardIndex >= Manifest.Shards.size()
            || Tile.ValidityShardIndex >= Manifest.Shards.size())
        {
            return {TerrainCoreError::InvalidTile, Index};
        }
        if (Tile.HeightBytes > TerrainCoreDeflateBound(Tile.HeightRawBytes)
            || Tile.ValidityBytes > TerrainCoreDeflateBound(Tile.ValidityRawBytes)
            || Tile.HeightBytes > TerrainCoreMaxEncodedTileBytes
            || Tile.ValidityBytes > TerrainCoreMaxEncodedTileBytes
            || Tile.HeightBytes > TerrainCoreMaxEncodedTileBytes - Tile.ValidityBytes)
        {
            return {TerrainCoreError::AssetTooLarge, Index};
        }
        const std::array<const std::string*, 2> TilePaths{&Tile.HeightPath, &Tile.ValidityPath};
        const std::array<const std::string*, 2> TileHashes{&Tile.HeightSha256, &Tile.ValiditySha256};
        const std::array<std::uint64_t, 2> TileBytes{Tile.HeightBytes, Tile.ValidityBytes};
        const std::array<std::uint64_t, 2> TileOffsets{Tile.HeightOffset, Tile.ValidityOffset};
        const std::array<std::uint32_t, 2> TileShards{Tile.HeightShardIndex,
            Tile.ValidityShardIndex};
        for (std::size_t Asset = 0; Asset < TilePaths.size(); ++Asset)
        {
            const TerrainCoreShardDescriptor& Shard = Manifest.Shards[TileShards[Asset]];
            if (*TilePaths[Asset] != Shard.Path)
            {
                return {TerrainCoreError::InvalidAssetPath, Index};
            }
            if (!IsTerrainCoreSha256(*TileHashes[Asset]))
            {
                return {TerrainCoreError::InvalidAssetHash, Index};
            }
            if (TileBytes[Asset] > TerrainCoreMaxAssetBytes
                || TileOffsets[Asset] > Shard.Bytes
                || TileBytes[Asset] > Shard.Bytes - TileOffsets[Asset])
            {
                return {TerrainCoreError::AssetTooLarge, Index};
            }
            Ranges[TileShards[Asset]].push_back({TileOffsets[Asset],
                TileOffsets[Asset] + TileBytes[Asset]});
        }
    }
    for (std::size_t ShardIndex = 0; ShardIndex < Ranges.size(); ++ShardIndex)
    {
        auto& ShardRanges = Ranges[ShardIndex];
        std::sort(ShardRanges.begin(), ShardRanges.end());
        std::uint64_t Cursor = 0;
        for (const auto& Range : ShardRanges)
        {
            if (Range.first != Cursor || Range.second < Range.first)
            {
                return {TerrainCoreError::ShardCoverage, ShardIndex};
            }
            Cursor = Range.second;
        }
        if (Cursor != Manifest.Shards[ShardIndex].Bytes)
        {
            return {TerrainCoreError::ShardCoverage, ShardIndex};
        }
    }
    return {};
}

SkiDomain::TerrainCoreValidation SkiDomain::ValidateTerrainEditSet(
    const TerrainEditSet& Edits, const std::uint32_t Width,
    const std::uint32_t Height) noexcept
{
    if (Edits.SchemaVersion != 1 || !IsTerrainCoreSha256(Edits.TerrainCoreId)
        || (Edits.Deltas.empty() ? Edits.EditRevision != Edits.BaseRevision
            : Edits.EditRevision <= Edits.BaseRevision)
        || Width < 2 || Height < 2
        || Edits.Deltas.size() > TerrainCoreMaxEdits)
    {
        return {TerrainCoreError::InvalidEdit, 0};
    }
    std::unordered_set<std::uint64_t> Cells;
    for (std::size_t Index = 0; Index < Edits.Deltas.size(); ++Index)
    {
        const TerrainEditDelta& Delta = Edits.Deltas[Index];
        if (Delta.Column >= Width || Delta.Row >= Height || !std::isfinite(Delta.DeltaM))
        {
            return {TerrainCoreError::InvalidEdit, Index};
        }
        const std::uint64_t Key = static_cast<std::uint64_t>(Delta.Row) * Width + Delta.Column;
        if (!Cells.insert(Key).second)
        {
            return {TerrainCoreError::DuplicateEdit, Index};
        }
    }
    return {};
}

bool SkiDomain::BuildTerrainChangeReceipt(const TerrainEditSet& Before,
    const TerrainEditSet& After, const std::uint32_t Width, const std::uint32_t Height,
    TerrainChangeReceipt& OutReceipt) noexcept
{
    OutReceipt = {};
    if (!ValidateTerrainEditSet(Before, Width, Height).Ok()
        || !ValidateTerrainEditSet(After, Width, Height).Ok()
        || Before.TerrainCoreId != After.TerrainCoreId
        || Before.EditRevision != After.BaseRevision || After.Deltas.empty())
    {
        return false;
    }
    OutReceipt.TerrainCoreId = After.TerrainCoreId;
    OutReceipt.BeforeRevision = Before.EditRevision;
    OutReceipt.AfterRevision = After.EditRevision;
    OutReceipt.MinColumn = OutReceipt.MaxColumn = After.Deltas.front().Column;
    OutReceipt.MinRow = OutReceipt.MaxRow = After.Deltas.front().Row;
    std::unordered_set<std::uint64_t> TileIds;
    const std::uint32_t TilesX = (Width - 1U + TerrainCoreTileCells - 1U)
        / TerrainCoreTileCells;
    const std::uint32_t TilesY = (Height - 1U + TerrainCoreTileCells - 1U)
        / TerrainCoreTileCells;
    for (const TerrainEditDelta& Delta : After.Deltas)
    {
        OutReceipt.MinColumn = std::min(OutReceipt.MinColumn, Delta.Column);
        OutReceipt.MinRow = std::min(OutReceipt.MinRow, Delta.Row);
        OutReceipt.MaxColumn = std::max(OutReceipt.MaxColumn, Delta.Column);
        OutReceipt.MaxRow = std::max(OutReceipt.MaxRow, Delta.Row);
        const std::uint32_t TileX = Delta.Column / TerrainCoreTileCells;
        const std::uint32_t TileY = Delta.Row / TerrainCoreTileCells;
        for (int Y = static_cast<int>(TileY) - 1; Y <= static_cast<int>(TileY) + 1; ++Y)
        {
            for (int X = static_cast<int>(TileX) - 1; X <= static_cast<int>(TileX) + 1; ++X)
            {
                if (X >= 0 && Y >= 0 && static_cast<std::uint32_t>(X) < TilesX
                    && static_cast<std::uint32_t>(Y) < TilesY)
                {
                    TileIds.insert(TileIdentity(0, static_cast<std::uint32_t>(X),
                        static_cast<std::uint32_t>(Y)));
                }
            }
        }
    }
    OutReceipt.AffectedTileIds.assign(TileIds.begin(), TileIds.end());
    std::sort(OutReceipt.AffectedTileIds.begin(), OutReceipt.AffectedTileIds.end());
    return true;
}

std::uint8_t SkiDomain::SelectTerrainCoreLod(const double FinestSamplePixels,
    const std::uint8_t CurrentLod, const LodHysteresisPolicy& Policy) noexcept
{
    const std::uint8_t Maximum = std::min<std::uint8_t>(Policy.MaximumLod,
        static_cast<std::uint8_t>(TerrainCoreLodFactors.size() - 1U));
    if (!std::isfinite(FinestSamplePixels) || FinestSamplePixels < 0.0
        || !std::isfinite(Policy.RefineAbovePixels)
        || !std::isfinite(Policy.CoarsenBelowPixels)
        || Policy.RefineAbovePixels <= Policy.CoarsenBelowPixels
        || Policy.CoarsenBelowPixels < 0.0)
    {
        return std::min(CurrentLod, Maximum);
    }
    const std::uint8_t Current = std::min(CurrentLod, Maximum);
    const double CurrentPixels = FinestSamplePixels * TerrainCoreLodFactors[Current];
    if (CurrentPixels > Policy.RefineAbovePixels && Current > 0)
    {
        return static_cast<std::uint8_t>(Current - 1U);
    }
    if (CurrentPixels < Policy.CoarsenBelowPixels && Current < Maximum)
    {
        return static_cast<std::uint8_t>(Current + 1U);
    }
    return Current;
}

bool SkiDomain::AreTerrainCoreLodsAdjacent(const std::uint8_t First,
    const std::uint8_t Second) noexcept
{
    return First < TerrainCoreLodFactors.size() && Second < TerrainCoreLodFactors.size()
        && (First > Second ? First - Second : Second - First) <= 1U;
}
