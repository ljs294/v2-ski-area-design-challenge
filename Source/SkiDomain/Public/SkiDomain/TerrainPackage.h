#pragma once

#include "SkiDomain/Coordinates.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkiDomain
{
constexpr std::uint32_t TerrainPackageSchema = 1;
constexpr std::uint64_t MaxManifestBytes = 1024ULL * 1024ULL;
constexpr std::uint64_t MaxAssetBytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaxPackageBytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaxHeightSamples = 4'000'000ULL;
constexpr std::uint64_t MaxCoverCells = 16'000'000ULL;
constexpr std::size_t MaxPackageAssets = 64;

struct GeographicBounds
{
    double WestDeg = 0.0;
    double SouthDeg = 0.0;
    double EastDeg = 0.0;
    double NorthDeg = 0.0;
};

struct TerrainAsset
{
    std::string Path;
    std::string Type;
    std::string Sha256;
    std::uint64_t Length = 0;
    bool Required = true;
    std::string MissingReason;
    std::string Source;
    std::string License;
};

struct TerrainManifest
{
    std::uint32_t SchemaVersion = TerrainPackageSchema;
    std::string ContentId;
    std::string Name;
    std::string Source;
    std::string RequestedAtUtc;
    std::string GeneratorVersion = "mountain-planner-unreal-p1";
    GeographicBounds RequestedBounds;
    GeographicBounds ActualBounds;
    GeodeticPoint LocalOrigin;
    std::string HorizontalFrame = "WGS84/local-ENU";
    std::string VerticalDatum = "unknown";
    std::uint32_t HeightWidth = 0;
    std::uint32_t HeightHeight = 0;
    std::uint32_t CoverWidth = 0;
    std::uint32_t CoverHeight = 0;
    double EastSpacingM = 0.0;
    double NorthSpacingM = 0.0;
    double NoDataValue = -9999.0;
    std::string RowOrientation = "north-to-south";
    std::vector<TerrainAsset> Assets;
};

enum class ManifestError
{
    None,
    UnsupportedSchema,
    InvalidContentId,
    InvalidBounds,
    InvalidOrigin,
    InvalidDimensions,
    InvalidSpacing,
    TooManyAssets,
    InvalidAssetPath,
    DuplicateAssetPath,
    InvalidAssetHash,
    AssetTooLarge,
    PackageTooLarge,
    MissingRequiredAsset,
};

struct ManifestValidation
{
    ManifestError Error = ManifestError::None;
    std::size_t AssetIndex = 0;
    bool Ok() const noexcept { return Error == ManifestError::None; }
};

SKI_DOMAIN_API bool IsValidSha256(const std::string& Value) noexcept;
SKI_DOMAIN_API bool IsSafeRelativeAssetPath(const std::string& Value) noexcept;
SKI_DOMAIN_API ManifestValidation ValidateManifest(const TerrainManifest& Manifest) noexcept;
}
