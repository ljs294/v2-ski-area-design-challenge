#include "SkiDomain/TerrainPackage.h"

#include <cmath>
#include <limits>
#include <unordered_set>

namespace
{
bool ValidBounds(const SkiDomain::GeographicBounds& Bounds) noexcept
{
    return std::isfinite(Bounds.WestDeg) && std::isfinite(Bounds.EastDeg)
        && std::isfinite(Bounds.SouthDeg) && std::isfinite(Bounds.NorthDeg)
        && Bounds.WestDeg >= -180.0 && Bounds.EastDeg <= 180.0
        && Bounds.SouthDeg >= -90.0 && Bounds.NorthDeg <= 90.0
        && Bounds.WestDeg < Bounds.EastDeg && Bounds.SouthDeg < Bounds.NorthDeg;
}

bool ProductWithin(const std::uint32_t Width, const std::uint32_t Height,
    const std::uint64_t Limit) noexcept
{
    return Width >= 2 && Height >= 2
        && static_cast<std::uint64_t>(Width) * static_cast<std::uint64_t>(Height) <= Limit;
}
}

bool SkiDomain::IsValidSha256(const std::string& Value) noexcept
{
    if (Value.size() != 64)
    {
        return false;
    }
    for (const char Character : Value)
    {
        if (!((Character >= '0' && Character <= '9')
            || (Character >= 'a' && Character <= 'f')
            || (Character >= 'A' && Character <= 'F')))
        {
            return false;
        }
    }
    return true;
}

bool SkiDomain::IsSafeRelativeAssetPath(const std::string& Value) noexcept
{
    if (Value.empty() || Value.front() == '/' || Value.front() == '\\'
        || Value.find('\\') != std::string::npos || Value.find(':') != std::string::npos)
    {
        return false;
    }
    std::size_t Start = 0;
    while (Start <= Value.size())
    {
        const std::size_t End = Value.find('/', Start);
        const std::string Segment = Value.substr(Start, End == std::string::npos
            ? std::string::npos : End - Start);
        if (Segment.empty() || Segment == "." || Segment == "..")
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

SkiDomain::ManifestValidation SkiDomain::ValidateManifest(const TerrainManifest& Manifest) noexcept
{
    if (Manifest.SchemaVersion != TerrainPackageSchema)
    {
        return {ManifestError::UnsupportedSchema, 0};
    }
    if (!IsValidSha256(Manifest.ContentId))
    {
        return {ManifestError::InvalidContentId, 0};
    }
    if (!ValidBounds(Manifest.RequestedBounds) || !ValidBounds(Manifest.ActualBounds))
    {
        return {ManifestError::InvalidBounds, 0};
    }
    if (!IsValidGeodetic(Manifest.LocalOrigin))
    {
        return {ManifestError::InvalidOrigin, 0};
    }
    if (!ProductWithin(Manifest.HeightWidth, Manifest.HeightHeight, MaxHeightSamples)
        || ((Manifest.CoverWidth != 0 || Manifest.CoverHeight != 0)
            && !ProductWithin(Manifest.CoverWidth, Manifest.CoverHeight, MaxCoverCells)))
    {
        return {ManifestError::InvalidDimensions, 0};
    }
    if (!std::isfinite(Manifest.EastSpacingM) || !std::isfinite(Manifest.NorthSpacingM)
        || Manifest.EastSpacingM <= 0.0 || Manifest.NorthSpacingM <= 0.0
        || !std::isfinite(Manifest.NoDataValue))
    {
        return {ManifestError::InvalidSpacing, 0};
    }
    if (Manifest.Assets.empty() || Manifest.Assets.size() > MaxPackageAssets)
    {
        return {ManifestError::TooManyAssets, 0};
    }
    std::unordered_set<std::string> Paths;
    std::uint64_t Total = 0;
    for (std::size_t Index = 0; Index < Manifest.Assets.size(); ++Index)
    {
        const TerrainAsset& Asset = Manifest.Assets[Index];
        if (!IsSafeRelativeAssetPath(Asset.Path))
        {
            return {ManifestError::InvalidAssetPath, Index};
        }
        if (!Paths.insert(Asset.Path).second)
        {
            return {ManifestError::DuplicateAssetPath, Index};
        }
        if (Asset.Required && Asset.Length == 0)
        {
            return {ManifestError::MissingRequiredAsset, Index};
        }
        if (!Asset.Required && Asset.Length == 0)
        {
            if (Asset.MissingReason.empty())
            {
                return {ManifestError::MissingRequiredAsset, Index};
            }
            continue;
        }
        if (!IsValidSha256(Asset.Sha256))
        {
            return {ManifestError::InvalidAssetHash, Index};
        }
        if (Asset.Length > MaxAssetBytes)
        {
            return {ManifestError::AssetTooLarge, Index};
        }
        if (Total > MaxPackageBytes - Asset.Length)
        {
            return {ManifestError::PackageTooLarge, Index};
        }
        Total += Asset.Length;
    }
    return {};
}
