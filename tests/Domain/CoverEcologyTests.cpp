#include "SkiDomain/CoverEcology.h"

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

SkiDomain::CoverEcologyManifest Manifest()
{
    SkiDomain::CoverEcologyManifest Result;
    Result.ContentId = std::string(64, 'a');
    Result.GeneratorVersion = "cover-test-v1";
    Result.CoverRevision = 7;
    Result.Source = {"worldcover-2021", "ESA WorldCover analytical COG", "2021",
        "esa-worldcover-v200", "CC BY 4.0", "ESA WorldCover project"};
    Result.Transform.Width = 4;
    Result.Transform.Height = 3;
    Result.Transform.LongitudeStepDeg = 0.01;
    Result.Transform.LatitudeStepDeg = 0.02;
    Result.Transform.SampleCenterBounds = {-71.3, 44.2, -71.27, 44.24};
    SkiDomain::ComputeCoverEcologyOuterBounds(Result.Transform.Width,
        Result.Transform.Height, Result.Transform.LongitudeStepDeg,
        Result.Transform.LatitudeStepDeg, Result.Transform.SampleCenterBounds,
        Result.Transform.OuterBounds);
    Result.Assets = {
        {"channels/classes.u8", "semantic-cover-u8", std::string(64, 'b'), 12},
        {"channels/validity.bits", "validity-bitset", std::string(64, 'c'), 2},
    };
    return Result;
}
}

int main()
{
    SkiDomain::GeographicBounds Outer;
    Require(SkiDomain::ComputeCoverEcologyOuterBounds(4, 3, 0.01, 0.02,
        {-71.3, 44.2, -71.27, 44.24}, Outer), "valid transform rejected");
    Require(std::abs(Outer.WestDeg - -71.305) < 1.0e-12
        && std::abs(Outer.EastDeg - -71.265) < 1.0e-12
        && std::abs(Outer.SouthDeg - 44.19) < 1.0e-12
        && std::abs(Outer.NorthDeg - 44.25) < 1.0e-12,
        "sample-center/outer bounds math is wrong");

    auto Cover = Manifest();
    Require(SkiDomain::ValidateCoverEcology(Cover).Ok(), "valid cover rejected");
    Cover.Transform.OuterBounds.EastDeg += 0.1;
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::InvalidTransform,
        "inconsistent transform accepted");
    Cover = Manifest();
    Cover.Transform.Width = 0xffffffffU;
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::InvalidDimensions,
        "overflow dimensions accepted");
    Cover = Manifest();
    Cover.Assets[0].Path = "../classes.u8";
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::InvalidAssetPath,
        "traversing channel path accepted");
    Cover = Manifest();
    Cover.Assets[1].Path = "CHANNELS/CLASSES.U8";
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::DuplicateAssetPath,
        "case-colliding channel paths accepted");
    Cover = Manifest();
    Cover.Assets[0].Length = 11;
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::InvalidAssetLength,
        "wrong class grid length accepted");
    Cover = Manifest();
    Cover.Assets.pop_back();
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::MissingRequiredChannel,
        "missing validity channel accepted");
    Cover = Manifest();
    Cover.Assets.push_back(Cover.Assets.front());
    Cover.Assets.back().Path = "channels/classes-2.u8";
    Require(SkiDomain::ValidateCoverEcology(Cover).Error
        == SkiDomain::CoverEcologyError::DuplicateRequiredChannel,
        "duplicate categorical channel accepted");
    Cover = Manifest();
    Require(SkiDomain::ValidateCoverEcology(Cover,
        SkiDomain::CoverEcologyMaxManifestBytes + 1).Error
        == SkiDomain::CoverEcologyError::ManifestTooLarge,
        "oversized manifest accepted");

    SkiDomain::InstalledTerrainReceipt Receipt;
    Receipt.ContentId = std::string(64, 'd');
    Receipt.GeneratorVersion = "install-test-v1";
    Receipt.TerrainCoreId = std::string(64, 'e');
    Receipt.CoverEcologyId = std::string(64, 'f');
    Receipt.OptionalSources = {
        {"naip", "USDA NAIP RGBN", SkiDomain::OptionalSourceStatus::Unavailable,
            {}, "NO_COMPLETE_COVERAGE", "Public domain", "USDA"},
        {"overpass", "OpenStreetMap vector context",
            SkiDomain::OptionalSourceStatus::Acquired, std::string(64, '1'), {},
            "ODbL 1.0", "OpenStreetMap contributors"},
    };
    Require(SkiDomain::ValidateInstalledTerrainReceipt(Receipt).Ok(),
        "valid composite receipt rejected");
    Receipt.OptionalSources[0].ArtifactId = std::string(64, '2');
    Require(SkiDomain::ValidateInstalledTerrainReceipt(Receipt).Error
        == SkiDomain::CoverEcologyError::InvalidOptionalSource,
        "placeholder artifact accepted for unavailable source");
    Receipt.OptionalSources[0].ArtifactId.clear();
    Receipt.OptionalSources[1].SourceId = "naip";
    Require(SkiDomain::ValidateInstalledTerrainReceipt(Receipt).Error
        == SkiDomain::CoverEcologyError::DuplicateOptionalSource,
        "duplicate optional source accepted");

    return Failures == 0 ? 0 : 1;
}
