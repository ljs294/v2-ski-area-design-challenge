#include "SkiPreparation/ImageryPyramid.h"

#include "SkiPreparation/TerrainPreparation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace SkiPreparation;

constexpr std::uint64_t KeyCoordinateLimit = 1ULL << 28U;

bool NearlyEqual(const double A, const double B) noexcept
{
    const double Scale = std::max({1.0, std::abs(A), std::abs(B)});
    return std::abs(A - B) <= Scale * 1.0e-9;
}

double BoundsTolerance(const SkiDomain::MetricBounds& A,
    const SkiDomain::MetricBounds& B) noexcept
{
    const double Scale = std::max({1.0, std::abs(A.WestM), std::abs(A.SouthM),
        std::abs(A.EastM), std::abs(A.NorthM), std::abs(B.WestM), std::abs(B.SouthM),
        std::abs(B.EastM), std::abs(B.NorthM)});
    return Scale * 1.0e-9;
}

bool SameBounds(const SkiDomain::MetricBounds& A,
    const SkiDomain::MetricBounds& B) noexcept
{
    return NearlyEqual(A.WestM, B.WestM) && NearlyEqual(A.SouthM, B.SouthM)
        && NearlyEqual(A.EastM, B.EastM) && NearlyEqual(A.NorthM, B.NorthM);
}

bool FiniteBounds(const SkiDomain::MetricBounds& Bounds) noexcept
{
    return std::isfinite(Bounds.WestM) && std::isfinite(Bounds.SouthM)
        && std::isfinite(Bounds.EastM) && std::isfinite(Bounds.NorthM)
        && Bounds.WestM <= Bounds.EastM && Bounds.SouthM <= Bounds.NorthM;
}

bool SameGeodeticPoint(const SkiDomain::GeodeticPoint& A,
    const SkiDomain::GeodeticPoint& B) noexcept
{
    return NearlyEqual(A.LatitudeDeg, B.LatitudeDeg)
        && NearlyEqual(A.LongitudeDeg, B.LongitudeDeg)
        && NearlyEqual(A.HeightM, B.HeightM);
}

bool HasNoControls(const std::string& Value) noexcept
{
    return std::none_of(Value.begin(), Value.end(), [](const unsigned char Character)
    {
        return Character < 0x20U || Character == 0x7fU;
    });
}

bool ValidMetadataText(const std::string& Value, const std::size_t MaximumLength) noexcept
{
    return !Value.empty() && Value.size() <= MaximumLength && HasNoControls(Value);
}

bool ValidSource(const ImagerySourceMetadata& Source) noexcept
{
    // The request path itself is checked by SkiNetGateway at acquisition time. This
    // metadata pins which service/product produced the stored, reprojected JPEG bytes.
    return ValidMetadataText(Source.SourceId, 128)
        && ValidMetadataText(Source.Product, 128)
        && ValidMetadataText(Source.ServiceUrlTemplate, 1024)
        && ValidMetadataText(Source.License, 512)
        && ValidMetadataText(Source.Attribution, 512)
        && ValidMetadataText(Source.TermsUrl, 1024);
}

std::uint32_t LodDimension(const std::uint32_t BaseDimension,
    const std::uint32_t Factor) noexcept
{
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(BaseDimension) - 1ULL
        + Factor - 1ULL) / Factor + 1ULL);
}

std::uint64_t TileKey(const std::uint8_t Lod, const std::uint32_t X,
    const std::uint32_t Y) noexcept
{
    return (static_cast<std::uint64_t>(Lod) << 56U)
        | (static_cast<std::uint64_t>(Y) << 28U) | X;
}

std::string CanonicalTilePath(const ImageryPyramidTile& Tile)
{
    return "imagery/lod" + std::to_string(Tile.LodIndex) + "/"
        + std::to_string(Tile.TileY) + "/" + std::to_string(Tile.TileX) + ".jpg";
}

bool ValidTileKey(const ImageryPyramidTile& Tile) noexcept
{
    return Tile.LodIndex < SkiDomain::TerrainCoreLodFactors.size()
        && Tile.TileX < KeyCoordinateLimit && Tile.TileY < KeyCoordinateLimit;
}

bool GetCorePlan(const SkiDomain::TerrainCoreManifest& TerrainCore,
    SkiDomain::TerrainCoreTilePlan& OutPlan) noexcept
{
    if (TerrainCore.SchemaVersion != SkiDomain::TerrainCoreSchema
        || TerrainCore.Registration != SkiDomain::PixelRegistration::SampleCenter
        || TerrainCore.RowOrientation != "north-to-south"
        || !SkiDomain::IsTerrainCoreSha256(TerrainCore.ContentId)
        || TerrainCore.Width < 2 || TerrainCore.Height < 2
        || !std::isfinite(TerrainCore.DeliveredEastSpacingM)
        || !std::isfinite(TerrainCore.DeliveredNorthSpacingM)
        || TerrainCore.DeliveredEastSpacingM <= 0.0
        || TerrainCore.DeliveredNorthSpacingM <= 0.0
        || !SkiDomain::IsValidGeodetic(TerrainCore.LocalOrigin)
        || !FiniteBounds(TerrainCore.SampleCenterBounds)
        || !FiniteBounds(TerrainCore.OuterBounds))
    {
        return false;
    }

    SkiDomain::MetricBounds ExpectedOuter;
    return SkiDomain::ComputeTerrainCoreBounds(TerrainCore.Width, TerrainCore.Height,
            TerrainCore.DeliveredEastSpacingM, TerrainCore.DeliveredNorthSpacingM,
            TerrainCore.SampleCenterBounds, ExpectedOuter)
        && SameBounds(ExpectedOuter, TerrainCore.OuterBounds)
        && SkiDomain::PlanTerrainCoreTiles(TerrainCore.Width, TerrainCore.Height, OutPlan)
        && !OutPlan.Tiles.empty()
        && OutPlan.Tiles.size() <= SkiDomain::TerrainCoreMaxTiles;
}

bool MakeTileGeometry(const SkiDomain::TerrainCoreManifest& TerrainCore,
    const SkiDomain::TerrainCoreTileDescriptor& CoreTile,
    ImageryPyramidTile& OutTile) noexcept
{
    OutTile = {};
    if (CoreTile.LodIndex >= SkiDomain::TerrainCoreLodFactors.size()
        || CoreTile.LodFactor != SkiDomain::TerrainCoreLodFactors[CoreTile.LodIndex]
        || CoreTile.CoreWidth == 0 || CoreTile.CoreHeight == 0)
    {
        return false;
    }

    const std::uint64_t LastColumnAtLod = static_cast<std::uint64_t>(CoreTile.StartColumn)
        + CoreTile.CoreWidth - 1ULL;
    const std::uint64_t LastRowAtLod = static_cast<std::uint64_t>(CoreTile.StartRow)
        + CoreTile.CoreHeight - 1ULL;
    const std::uint64_t LastBaseColumn = std::min<std::uint64_t>(
        LastColumnAtLod * CoreTile.LodFactor, TerrainCore.Width - 1ULL);
    const std::uint64_t LastBaseRow = std::min<std::uint64_t>(
        LastRowAtLod * CoreTile.LodFactor, TerrainCore.Height - 1ULL);
    const std::uint64_t FirstBaseColumn = static_cast<std::uint64_t>(CoreTile.StartColumn)
        * CoreTile.LodFactor;
    const std::uint64_t FirstBaseRow = static_cast<std::uint64_t>(CoreTile.StartRow)
        * CoreTile.LodFactor;
    if (FirstBaseColumn >= TerrainCore.Width || FirstBaseRow >= TerrainCore.Height
        || LastBaseColumn < FirstBaseColumn || LastBaseRow < FirstBaseRow)
    {
        return false;
    }

    OutTile.LodIndex = CoreTile.LodIndex;
    OutTile.LodFactor = CoreTile.LodFactor;
    OutTile.TileX = CoreTile.TileX;
    OutTile.TileY = CoreTile.TileY;
    OutTile.StartColumn = CoreTile.StartColumn;
    OutTile.StartRow = CoreTile.StartRow;
    OutTile.TerrainCoreWidth = CoreTile.CoreWidth;
    OutTile.TerrainCoreHeight = CoreTile.CoreHeight;
    OutTile.RasterWidth = ImageryPyramidTilePixels;
    OutTile.RasterHeight = ImageryPyramidTilePixels;
    OutTile.MetersPerPixelEast = TerrainCore.DeliveredEastSpacingM * CoreTile.LodFactor;
    OutTile.MetersPerPixelNorth = TerrainCore.DeliveredNorthSpacingM * CoreTile.LodFactor;
    const double RasterWestM = TerrainCore.SampleCenterBounds.WestM
        + static_cast<double>(FirstBaseColumn) * TerrainCore.DeliveredEastSpacingM;
    const double RasterNorthM = TerrainCore.SampleCenterBounds.NorthM
        - static_cast<double>(FirstBaseRow) * TerrainCore.DeliveredNorthSpacingM;
    OutTile.RasterSampleCenterBounds = {
        RasterWestM,
        RasterNorthM - static_cast<double>(ImageryPyramidTilePixels - 1U)
            * OutTile.MetersPerPixelNorth,
        RasterWestM + static_cast<double>(ImageryPyramidTilePixels - 1U)
            * OutTile.MetersPerPixelEast,
        RasterNorthM};
    OutTile.SampleCenterBounds = {
        TerrainCore.SampleCenterBounds.WestM
            + static_cast<double>(FirstBaseColumn) * TerrainCore.DeliveredEastSpacingM,
        TerrainCore.SampleCenterBounds.NorthM
            - static_cast<double>(LastBaseRow) * TerrainCore.DeliveredNorthSpacingM,
        TerrainCore.SampleCenterBounds.WestM
            + static_cast<double>(LastBaseColumn) * TerrainCore.DeliveredEastSpacingM,
        TerrainCore.SampleCenterBounds.NorthM
            - static_cast<double>(FirstBaseRow) * TerrainCore.DeliveredNorthSpacingM};
    return std::isfinite(OutTile.MetersPerPixelEast)
        && std::isfinite(OutTile.MetersPerPixelNorth)
        && OutTile.MetersPerPixelEast > 0.0 && OutTile.MetersPerPixelNorth > 0.0
        && FiniteBounds(OutTile.RasterSampleCenterBounds)
        && FiniteBounds(OutTile.SampleCenterBounds);
}

bool SameTileGeometry(const ImageryPyramidTile& Actual,
    const ImageryPyramidTile& Expected) noexcept
{
    return Actual.LodIndex == Expected.LodIndex
        && Actual.LodFactor == Expected.LodFactor
        && Actual.TileX == Expected.TileX && Actual.TileY == Expected.TileY
        && Actual.StartColumn == Expected.StartColumn
        && Actual.StartRow == Expected.StartRow
        && Actual.TerrainCoreWidth == Expected.TerrainCoreWidth
        && Actual.TerrainCoreHeight == Expected.TerrainCoreHeight
        && Actual.RasterWidth == ImageryPyramidTilePixels
        && Actual.RasterHeight == ImageryPyramidTilePixels
        && NearlyEqual(Actual.MetersPerPixelEast, Expected.MetersPerPixelEast)
        && NearlyEqual(Actual.MetersPerPixelNorth, Expected.MetersPerPixelNorth)
        && SameBounds(Actual.RasterSampleCenterBounds, Expected.RasterSampleCenterBounds)
        && SameBounds(Actual.SampleCenterBounds, Expected.SampleCenterBounds);
}

bool SameCoreBinding(const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest) noexcept
{
    return Manifest.TerrainCoreId == TerrainCore.ContentId
        && Manifest.TerrainCoreWidth == TerrainCore.Width
        && Manifest.TerrainCoreHeight == TerrainCore.Height
        && NearlyEqual(Manifest.TerrainCoreEastSpacingM, TerrainCore.DeliveredEastSpacingM)
        && NearlyEqual(Manifest.TerrainCoreNorthSpacingM, TerrainCore.DeliveredNorthSpacingM)
        && SameGeodeticPoint(Manifest.LocalOrigin, TerrainCore.LocalOrigin)
        && SameBounds(Manifest.SampleCenterBounds, TerrainCore.SampleCenterBounds)
        && SameBounds(Manifest.OuterBounds, TerrainCore.OuterBounds);
}

bool HasPositiveAreaOverlap(const SkiDomain::MetricBounds& A,
    const SkiDomain::MetricBounds& B) noexcept
{
    const double Width = std::min(A.EastM, B.EastM) - std::max(A.WestM, B.WestM);
    const double Height = std::min(A.NorthM, B.NorthM) - std::max(A.SouthM, B.SouthM);
    const double Tolerance = BoundsTolerance(A, B);
    return Width > Tolerance && Height > Tolerance;
}

bool HasUnexpectedNeighborOverlap(const TArray<ImageryPyramidTile>& Tiles)
{
    std::unordered_map<std::uint64_t, std::size_t> Indices;
    Indices.reserve(static_cast<std::size_t>(Tiles.Num()));
    for (int32 Index = 0; Index < Tiles.Num(); ++Index)
    {
        const ImageryPyramidTile& Tile = Tiles[Index];
        if (!ValidTileKey(Tile) || !FiniteBounds(Tile.RasterSampleCenterBounds)
            || !FiniteBounds(Tile.SampleCenterBounds)) continue;
        Indices.emplace(TileKey(Tile.LodIndex, Tile.TileX, Tile.TileY),
            static_cast<std::size_t>(Index));
    }

    // At most eight local neighbors are examined per tile. Any distant overlap is
    // rejected by the later comparison with TerrainCore's canonical tile bounds.
    for (int32 Index = 0; Index < Tiles.Num(); ++Index)
    {
        const ImageryPyramidTile& Tile = Tiles[Index];
        for (int32 DeltaY = -1; DeltaY <= 1; ++DeltaY)
        {
            for (int32 DeltaX = -1; DeltaX <= 1; ++DeltaX)
            {
                if (DeltaX == 0 && DeltaY == 0) continue;
                const int64 NeighborX = static_cast<int64>(Tile.TileX) + DeltaX;
                const int64 NeighborY = static_cast<int64>(Tile.TileY) + DeltaY;
                if (NeighborX < 0 || NeighborY < 0
                    || NeighborX >= static_cast<int64>(KeyCoordinateLimit)
                    || NeighborY >= static_cast<int64>(KeyCoordinateLimit)) continue;
                const auto Neighbor = Indices.find(TileKey(Tile.LodIndex,
                    static_cast<std::uint32_t>(NeighborX),
                    static_cast<std::uint32_t>(NeighborY)));
                if (Neighbor == Indices.end()
                    || Neighbor->second <= static_cast<std::size_t>(Index)) continue;
                const ImageryPyramidTile& NeighborTile = Tiles[static_cast<int32>(Neighbor->second)];
                if (HasPositiveAreaOverlap(Tile.RasterSampleCenterBounds,
                        NeighborTile.RasterSampleCenterBounds)
                    || HasPositiveAreaOverlap(Tile.SampleCenterBounds,
                        NeighborTile.SampleCenterBounds)) return true;
            }
        }
    }
    return false;
}

std::string FoldPathAscii(const std::string& Value)
{
    std::string Folded = Value;
    std::transform(Folded.begin(), Folded.end(), Folded.begin(), [](const unsigned char Character)
    {
        return Character >= 'A' && Character <= 'Z'
            ? static_cast<char>(Character - 'A' + 'a') : static_cast<char>(Character);
    });
    return Folded;
}

ImageryPyramidValidation ValidateCoreBindingAndSource(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest)
{
    SkiDomain::TerrainCoreTilePlan Plan;
    if (!GetCorePlan(TerrainCore, Plan))
        return {ImageryPyramidError::InvalidTerrainCoreGeometry, INDEX_NONE};
    if (Manifest.SchemaVersion != ImageryPyramidSchema)
        return {ImageryPyramidError::UnsupportedSchema, INDEX_NONE};
    if (Manifest.TileEncoding != "jpeg-rgb8-v1")
        return {ImageryPyramidError::UnsupportedEncoding, INDEX_NONE};
    if (Manifest.CoordinateFrame != "terraincore-local-enu-v1")
        return {ImageryPyramidError::UnsupportedCoordinateFrame, INDEX_NONE};
    if (!ValidSource(Manifest.Source))
        return {ImageryPyramidError::InvalidSourceMetadata, INDEX_NONE};
    if (!SameCoreBinding(TerrainCore, Manifest))
        return {ImageryPyramidError::TerrainCoreMismatch, INDEX_NONE};
    if (Plan.Tiles.size() > SkiDomain::TerrainCoreMaxTiles
        || Plan.Tiles.size() > static_cast<std::size_t>(MAX_int32))
        return {ImageryPyramidError::TooManyTiles, INDEX_NONE};
    if (Manifest.Tiles.Num() < static_cast<int32>(Plan.Tiles.size()))
        return {ImageryPyramidError::MissingTile, Manifest.Tiles.Num()};
    if (Manifest.Tiles.Num() > static_cast<int32>(Plan.Tiles.size()))
        return {ImageryPyramidError::TooManyTiles, static_cast<int32>(Plan.Tiles.size())};

    std::unordered_set<std::uint64_t> SeenKeys;
    SeenKeys.reserve(static_cast<std::size_t>(Manifest.Tiles.Num()));
    for (int32 Index = 0; Index < Manifest.Tiles.Num(); ++Index)
    {
        const ImageryPyramidTile& Tile = Manifest.Tiles[Index];
        if (!ValidTileKey(Tile)) return {ImageryPyramidError::MisalignedTile, Index};
        if (!SeenKeys.insert(TileKey(Tile.LodIndex, Tile.TileX, Tile.TileY)).second)
            return {ImageryPyramidError::DuplicateTile, Index};
    }

    if (HasUnexpectedNeighborOverlap(Manifest.Tiles))
        return {ImageryPyramidError::TileOverlap, INDEX_NONE};

    for (int32 Index = 0; Index < Manifest.Tiles.Num(); ++Index)
    {
        ImageryPyramidTile Expected;
        if (!MakeTileGeometry(TerrainCore, Plan.Tiles[Index], Expected)
            || !SameTileGeometry(Manifest.Tiles[Index], Expected))
        {
            return {ImageryPyramidError::MisalignedTile, Index};
        }
    }
    return {};
}

bool AddWithinLimit(const std::uint64_t Current, const std::uint64_t Addition,
    const std::uint64_t Limit, std::uint64_t& Out) noexcept
{
    if (Addition > Limit || Current > Limit - Addition) return false;
    Out = Current + Addition;
    return true;
}
}

SkiPreparation::ImageryPyramidValidation SkiPreparation::BuildRequiredImageryPyramid(
    const SkiDomain::TerrainCoreManifest& TerrainCore, const ImagerySourceMetadata& Source,
    ImageryPyramidManifest& OutManifest, const Cancellation* CancellationValue)
{
    OutManifest = ImageryPyramidManifest{};
    if (CancellationValue != nullptr && CancellationValue->IsCancelled())
        return {ImageryPyramidError::Cancelled, INDEX_NONE};
    if (!ValidSource(Source)) return {ImageryPyramidError::InvalidSourceMetadata, INDEX_NONE};

    SkiDomain::TerrainCoreTilePlan Plan;
    if (!GetCorePlan(TerrainCore, Plan))
        return {ImageryPyramidError::InvalidTerrainCoreGeometry, INDEX_NONE};
    if (CancellationValue != nullptr && CancellationValue->IsCancelled())
        return {ImageryPyramidError::Cancelled, INDEX_NONE};

    ImageryPyramidManifest Candidate;
    Candidate.TerrainCoreId = TerrainCore.ContentId;
    Candidate.LocalOrigin = TerrainCore.LocalOrigin;
    Candidate.TerrainCoreWidth = TerrainCore.Width;
    Candidate.TerrainCoreHeight = TerrainCore.Height;
    Candidate.TerrainCoreEastSpacingM = TerrainCore.DeliveredEastSpacingM;
    Candidate.TerrainCoreNorthSpacingM = TerrainCore.DeliveredNorthSpacingM;
    Candidate.SampleCenterBounds = TerrainCore.SampleCenterBounds;
    Candidate.OuterBounds = TerrainCore.OuterBounds;
    Candidate.Source = Source;
    Candidate.Tiles.Reserve(static_cast<int32>(Plan.Tiles.size()));
    for (std::size_t Index = 0; Index < Plan.Tiles.size(); ++Index)
    {
        if (CancellationValue != nullptr && CancellationValue->IsCancelled())
            return {ImageryPyramidError::Cancelled, static_cast<int32>(Index)};
        ImageryPyramidTile Tile;
        if (!MakeTileGeometry(TerrainCore, Plan.Tiles[Index], Tile))
            return {ImageryPyramidError::InvalidTerrainCoreGeometry, static_cast<int32>(Index)};
        Tile.Path = CanonicalTilePath(Tile);
        Candidate.Tiles.Add(MoveTemp(Tile));
    }
    OutManifest = MoveTemp(Candidate);
    return {};
}

SkiPreparation::ImageryPyramidValidation SkiPreparation::ValidateImageryPyramidLayout(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest)
{
    return ValidateCoreBindingAndSource(TerrainCore, Manifest);
}

SkiPreparation::ImageryPyramidValidation SkiPreparation::ValidateImageryPyramidManifest(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest)
{
    const ImageryPyramidValidation Layout = ValidateImageryPyramidLayout(TerrainCore, Manifest);
    if (!Layout.Ok()) return Layout;

    std::unordered_set<std::string> Paths;
    Paths.reserve(static_cast<std::size_t>(Manifest.Tiles.Num()));
    std::uint64_t TotalBytes = 0;
    for (int32 Index = 0; Index < Manifest.Tiles.Num(); ++Index)
    {
        const ImageryPyramidTile& Tile = Manifest.Tiles[Index];
        if (Tile.Path.size() > 240 || !SkiDomain::IsSafeTerrainCorePath(Tile.Path)
            || !Tile.Path.ends_with(".jpg"))
            return {ImageryPyramidError::InvalidAssetPath, Index};
        if (!Paths.insert(FoldPathAscii(Tile.Path)).second)
            return {ImageryPyramidError::DuplicateAssetPath, Index};
        if (Tile.Path != CanonicalTilePath(Tile))
            return {ImageryPyramidError::InvalidAssetPath, Index};
        if (!SkiDomain::IsTerrainCoreSha256(Tile.Sha256))
            return {ImageryPyramidError::InvalidAssetHash, Index};
        if (Tile.Bytes == 0 || Tile.Bytes > ImageryPyramidMaxTileBytes)
            return {ImageryPyramidError::InvalidAssetSize, Index};
        std::uint64_t UpdatedTotal = 0;
        if (!AddWithinLimit(TotalBytes, Tile.Bytes,
            SkiDomain::TerrainCoreMaxInstalledBytes, UpdatedTotal))
            return {ImageryPyramidError::PackageTooLarge, Index};
        TotalBytes = UpdatedTotal;
    }
    return {};
}

bool SkiPreparation::EstimateImageryPyramidStorage(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest,
    const std::uint64_t MinimumBytesPerTile, const std::uint64_t MaximumBytesPerTile,
    ImageryStorageEstimate& OutEstimate)
{
    OutEstimate = {};
    const ImageryPyramidValidation Layout = ValidateImageryPyramidLayout(TerrainCore, Manifest);
    if (!Layout.Ok() || MinimumBytesPerTile == 0
        || MaximumBytesPerTile < MinimumBytesPerTile
        || MaximumBytesPerTile > ImageryPyramidMaxTileBytes)
    {
        return false;
    }

    const std::uint64_t TileCount = static_cast<std::uint64_t>(Manifest.Tiles.Num());
    if (TileCount == 0 || TileCount > SkiDomain::TerrainCoreMaxTiles
        || TileCount > SkiDomain::TerrainCoreMaxInstalledBytes / MaximumBytesPerTile)
    {
        return false;
    }
    OutEstimate.MinimumBytes = TileCount * MinimumBytesPerTile;
    OutEstimate.MaximumBytes = TileCount * MaximumBytesPerTile;
    OutEstimate.Certainty = ImageryStorageSizeCertainty::Estimated;
    OutEstimate.Basis = TEXT("Per-tile encoded-size interval multiplied by the complete required TerrainCore-aligned tile count; storage headers and installer overhead excluded.");
    return true;
}

bool SkiPreparation::VerifyImageryPyramid(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    const ImageryPyramidManifest& Manifest,
    IImageryPyramidAssetReader& AssetReader, const Cancellation& CancellationValue,
    ImageryPyramidVerificationReport& OutReport)
{
    OutReport = {};
    if (CancellationValue.IsCancelled())
    {
        OutReport.Error = ImageryPyramidError::Cancelled;
        return false;
    }

    const ImageryPyramidValidation Validation =
        ValidateImageryPyramidManifest(TerrainCore, Manifest);
    if (!Validation.Ok())
    {
        OutReport.Error = Validation.Error;
        OutReport.TileIndex = Validation.TileIndex;
        return false;
    }

    std::uint64_t TotalBytes = 0;
    for (int32 Index = 0; Index < Manifest.Tiles.Num(); ++Index)
    {
        if (CancellationValue.IsCancelled())
        {
            OutReport.Error = ImageryPyramidError::Cancelled;
            OutReport.TileIndex = Index;
            return false;
        }

        const ImageryPyramidTile& Tile = Manifest.Tiles[Index];
        ImageryPyramidAssetFacts Actual;
        FString Detail;
        const bool bInspected = AssetReader.Inspect(Tile.Path,
            ImageryPyramidMaxTileBytes, CancellationValue, Actual, Detail);
        if (CancellationValue.IsCancelled())
        {
            OutReport.Error = ImageryPyramidError::Cancelled;
            OutReport.TileIndex = Index;
            return false;
        }
        if (!bInspected)
        {
            OutReport.Error = ImageryPyramidError::AssetInspectionFailed;
            OutReport.TileIndex = Index;
            OutReport.FailureDetail = Detail;
            return false;
        }
        if (Actual.Encoding != Manifest.TileEncoding)
        {
            OutReport.Error = ImageryPyramidError::AssetEncodingMismatch;
            OutReport.TileIndex = Index;
            return false;
        }
        if (Actual.Width != Tile.RasterWidth || Actual.Height != Tile.RasterHeight)
        {
            OutReport.Error = ImageryPyramidError::AssetDimensionsMismatch;
            OutReport.TileIndex = Index;
            return false;
        }
        if (Actual.Bytes != Tile.Bytes)
        {
            OutReport.Error = ImageryPyramidError::AssetSizeMismatch;
            OutReport.TileIndex = Index;
            return false;
        }
        if (!SkiDomain::IsTerrainCoreSha256(Actual.Sha256) || Actual.Sha256 != Tile.Sha256)
        {
            OutReport.Error = ImageryPyramidError::AssetHashMismatch;
            OutReport.TileIndex = Index;
            return false;
        }

        std::uint64_t UpdatedTotal = 0;
        if (!AddWithinLimit(TotalBytes, Actual.Bytes,
            SkiDomain::TerrainCoreMaxInstalledBytes, UpdatedTotal))
        {
            OutReport.Error = ImageryPyramidError::PackageTooLarge;
            OutReport.TileIndex = Index;
            return false;
        }
        TotalBytes = UpdatedTotal;
        OutReport.VerifiedBytes = TotalBytes;
        ++OutReport.VerifiedTiles;
    }

    OutReport.Storage.MinimumBytes = TotalBytes;
    OutReport.Storage.MaximumBytes = TotalBytes;
    OutReport.Storage.Certainty = ImageryStorageSizeCertainty::Exact;
    OutReport.Storage.Basis = TEXT("Sum of exact byte sizes for every required imagery payload after stored-content SHA-256 verification.");
    return true;
}
