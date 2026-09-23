#include "SkiDomain/CoverEcology.h"

#include "SkiDomain/TerrainCore.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace
{
using namespace SkiDomain;

bool FiniteGeographicBounds(const GeographicBounds& Bounds) noexcept
{
    return std::isfinite(Bounds.WestDeg) && std::isfinite(Bounds.SouthDeg)
        && std::isfinite(Bounds.EastDeg) && std::isfinite(Bounds.NorthDeg)
        && Bounds.WestDeg >= -180.0 && Bounds.EastDeg <= 180.0
        && Bounds.SouthDeg >= -90.0 && Bounds.NorthDeg <= 90.0
        && Bounds.WestDeg < Bounds.EastDeg && Bounds.SouthDeg < Bounds.NorthDeg;
}

bool NearlyEqual(const double A, const double B) noexcept
{
    const double Scale = std::max({1.0, std::abs(A), std::abs(B)});
    return std::abs(A - B) <= Scale * 1.0e-10;
}

bool ValidMetadata(const std::string& Value, const std::size_t Maximum = 4096) noexcept
{
    return !Value.empty() && Value.size() <= Maximum
        && std::none_of(Value.begin(), Value.end(), [](const unsigned char Character)
        {
            return Character < 0x20U || Character == 0x7fU;
        });
}
}

bool SkiDomain::ComputeCoverEcologyOuterBounds(const std::uint32_t Width,
    const std::uint32_t Height, const double LongitudeStepDeg,
    const double LatitudeStepDeg, const GeographicBounds& SampleCenters,
    GeographicBounds& OutOuterBounds) noexcept
{
    OutOuterBounds = {};
    if (Width < 2 || Height < 2 || !FiniteGeographicBounds(SampleCenters)
        || !std::isfinite(LongitudeStepDeg) || !std::isfinite(LatitudeStepDeg)
        || LongitudeStepDeg <= 0.0 || LatitudeStepDeg <= 0.0)
    {
        return false;
    }
    const double LongitudeSpan = static_cast<double>(Width - 1U) * LongitudeStepDeg;
    const double LatitudeSpan = static_cast<double>(Height - 1U) * LatitudeStepDeg;
    if (!std::isfinite(LongitudeSpan) || !std::isfinite(LatitudeSpan)
        || !NearlyEqual(SampleCenters.EastDeg - SampleCenters.WestDeg, LongitudeSpan)
        || !NearlyEqual(SampleCenters.NorthDeg - SampleCenters.SouthDeg, LatitudeSpan))
    {
        return false;
    }
    OutOuterBounds = {SampleCenters.WestDeg - LongitudeStepDeg * 0.5,
        SampleCenters.SouthDeg - LatitudeStepDeg * 0.5,
        SampleCenters.EastDeg + LongitudeStepDeg * 0.5,
        SampleCenters.NorthDeg + LatitudeStepDeg * 0.5};
    return FiniteGeographicBounds(OutOuterBounds);
}

SkiDomain::CoverEcologyValidation SkiDomain::ValidateCoverEcology(
    const CoverEcologyManifest& Manifest, const std::uint64_t SerializedManifestBytes) noexcept
{
    if (Manifest.SchemaVersion != CoverEcologySchema)
    {
        return {CoverEcologyError::UnsupportedSchema, 0};
    }
    if (SerializedManifestBytes > CoverEcologyMaxManifestBytes)
    {
        return {CoverEcologyError::ManifestTooLarge, 0};
    }
    if (!IsTerrainCoreSha256(Manifest.ContentId))
    {
        return {CoverEcologyError::InvalidContentId, 0};
    }
    const CoverEcologyGridTransform& Transform = Manifest.Transform;
    const std::uint64_t Cells = static_cast<std::uint64_t>(Transform.Width) * Transform.Height;
    if (Transform.Width < 2 || Transform.Height < 2 || Cells > CoverEcologyMaxCells)
    {
        return {CoverEcologyError::InvalidDimensions, 0};
    }
    GeographicBounds ExpectedOuter;
    if (Transform.HorizontalCrs != "EPSG:4326"
        || Transform.PixelRegistration != "sample-center"
        || Transform.RowOrientation != "north-to-south"
        || !ComputeCoverEcologyOuterBounds(Transform.Width, Transform.Height,
            Transform.LongitudeStepDeg, Transform.LatitudeStepDeg,
            Transform.SampleCenterBounds, ExpectedOuter)
        || !NearlyEqual(ExpectedOuter.WestDeg, Transform.OuterBounds.WestDeg)
        || !NearlyEqual(ExpectedOuter.SouthDeg, Transform.OuterBounds.SouthDeg)
        || !NearlyEqual(ExpectedOuter.EastDeg, Transform.OuterBounds.EastDeg)
        || !NearlyEqual(ExpectedOuter.NorthDeg, Transform.OuterBounds.NorthDeg))
    {
        return {CoverEcologyError::InvalidTransform, 0};
    }
    const CoverEcologySource& Source = Manifest.Source;
    if (!ValidMetadata(Manifest.GeneratorVersion) || Manifest.CoverRevision == 0
        || !ValidMetadata(Source.SourceId) || !ValidMetadata(Source.Product)
        || !ValidMetadata(Source.AcquisitionEpoch) || !ValidMetadata(Source.Provenance)
        || !ValidMetadata(Source.License) || !ValidMetadata(Source.Attribution)
        || Manifest.SemanticClassEncoding != "uint8-worldcover-class-v1"
        || Manifest.ValidityEncoding != "bitset-lsb0-v1")
    {
        return {CoverEcologyError::InvalidMetadata, 0};
    }
    if (Manifest.Assets.empty() || Manifest.Assets.size() > CoverEcologyMaxAssets)
    {
        return {CoverEcologyError::TooManyAssets, 0};
    }
    std::unordered_set<std::string> Paths;
    std::uint64_t Total = SerializedManifestBytes;
    std::size_t Classes = 0;
    std::size_t Validity = 0;
    for (std::size_t Index = 0; Index < Manifest.Assets.size(); ++Index)
    {
        const CoverEcologyAsset& Asset = Manifest.Assets[Index];
        if (!IsSafeTerrainCorePath(Asset.Path))
        {
            return {CoverEcologyError::InvalidAssetPath, Index};
        }
        std::string Lower = Asset.Path;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](const unsigned char C)
        {
            return static_cast<char>(std::tolower(C));
        });
        if (!Paths.insert(std::move(Lower)).second)
        {
            return {CoverEcologyError::DuplicateAssetPath, Index};
        }
        if (!IsTerrainCoreSha256(Asset.Sha256))
        {
            return {CoverEcologyError::InvalidAssetHash, Index};
        }
        if (Asset.Length == 0)
        {
            return {CoverEcologyError::InvalidAssetLength, Index};
        }
        if (Asset.Length > CoverEcologyMaxAssetBytes)
        {
            return {CoverEcologyError::AssetTooLarge, Index};
        }
        if (Total > CoverEcologyMaxPackageBytes - Asset.Length)
        {
            return {CoverEcologyError::PackageTooLarge, Index};
        }
        Total += Asset.Length;
        if (Asset.Type == "semantic-cover-u8")
        {
            ++Classes;
            if (Asset.Length != Cells)
                return {CoverEcologyError::InvalidAssetLength, Index};
        }
        else if (Asset.Type == "validity-bitset")
        {
            ++Validity;
            if (Asset.Length != (Cells + 7ULL) / 8ULL)
                return {CoverEcologyError::InvalidAssetLength, Index};
        }
        else
        {
            return {CoverEcologyError::InvalidMetadata, Index};
        }
    }
    if (Classes > 1 || Validity > 1)
        return {CoverEcologyError::DuplicateRequiredChannel, 0};
    if (Classes != 1 || Validity != 1)
        return {CoverEcologyError::MissingRequiredChannel, 0};
    return {};
}

SkiDomain::CoverEcologyValidation SkiDomain::ValidateInstalledTerrainReceipt(
    const InstalledTerrainReceipt& Receipt, const std::uint64_t SerializedReceiptBytes) noexcept
{
    if (Receipt.SchemaVersion != InstalledTerrainSchema)
        return {CoverEcologyError::UnsupportedSchema, 0};
    if (SerializedReceiptBytes > InstalledTerrainMaxReceiptBytes)
        return {CoverEcologyError::ManifestTooLarge, 0};
    if (!IsTerrainCoreSha256(Receipt.ContentId)
        || !IsTerrainCoreSha256(Receipt.TerrainCoreId)
        || !IsTerrainCoreSha256(Receipt.CoverEcologyId))
        return {CoverEcologyError::InvalidContentId, 0};
    if (!ValidMetadata(Receipt.GeneratorVersion)
        || Receipt.OptionalSources.size() > InstalledTerrainMaxOptionalSources)
        return {CoverEcologyError::InvalidMetadata, 0};

    std::unordered_set<std::string> Sources;
    for (std::size_t Index = 0; Index < Receipt.OptionalSources.size(); ++Index)
    {
        const OptionalSourceOutcome& Outcome = Receipt.OptionalSources[Index];
        if (!ValidMetadata(Outcome.SourceId) || !ValidMetadata(Outcome.Product)
            || !ValidMetadata(Outcome.License) || !ValidMetadata(Outcome.Attribution))
            return {CoverEcologyError::InvalidOptionalSource, Index};
        std::string FoldedSource = Outcome.SourceId;
        std::transform(FoldedSource.begin(), FoldedSource.end(), FoldedSource.begin(),
            [](const unsigned char Character)
            {
                return static_cast<char>(std::tolower(Character));
            });
        if (!Sources.insert(std::move(FoldedSource)).second)
            return {CoverEcologyError::DuplicateOptionalSource, Index};
        if (Outcome.Status == OptionalSourceStatus::Acquired)
        {
            if (!IsTerrainCoreSha256(Outcome.ArtifactId) || !Outcome.ReasonCode.empty())
                return {CoverEcologyError::InvalidOptionalSource, Index};
        }
        else if (!Outcome.ArtifactId.empty() || !ValidMetadata(Outcome.ReasonCode, 256))
        {
            return {CoverEcologyError::InvalidOptionalSource, Index};
        }
    }
    return {};
}
