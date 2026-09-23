#include "SkiDomain/CoverEcology.h"
#include "SkiDomain/Coordinates.h"

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

    // This cover grid is shifted relative to a terrain grid and has a masked
    // interior cell. Its geographic transform, not normalized terrain UVs,
    // determines the categorical sample.
    {
        const auto Transform = Manifest().Transform;
        const std::vector<std::uint8_t> Classes{
            10, 20, 30, 40,
            50, 60, 70, 80,
            90, 95, 100, 1};
        std::vector<std::uint8_t> Validity{0xff, 0x0f};
        std::uint8_t Class = 0;
        Require(SkiDomain::SampleCoverEcologyClass(Transform, Classes, Validity,
            44.22, -71.28, Class) && Class == 70,
            "geographic source-pixel sampling selected the wrong cell");
        Validity[0] &= static_cast<std::uint8_t>(~(1U << 6U));
        Require(!SkiDomain::SampleCoverEcologyClass(Transform, Classes, Validity,
            44.22, -71.28, Class) && Class == 0,
            "masked cover cell was sampled");
        Require(!SkiDomain::SampleCoverEcologyClass(Transform, Classes, Validity,
            44.22, -71.31, Class), "outside cover footprint was clamped to an edge");
        Require(SkiDomain::SampleCoverEcologyClass(Transform, Classes, Validity,
            44.24, -71.30, Class) && Class == 10,
            "northwestern sample center was not selected");
        Validity.pop_back();
        Require(!SkiDomain::SampleCoverEcologyClass(Transform, Classes, Validity,
            44.24, -71.30, Class), "short validity channel was accepted");
    }
    {
        SkiDomain::LocalFrame Frame;
        Require(SkiDomain::TryMakeLocalFrame({46.935, -121.475, 0.0}, Frame),
            "local frame for cover sampling is invalid");
        const SkiDomain::GeodeticPoint Expected{46.942, -121.463, 0.0};
        const SkiDomain::EnuPoint Offset = SkiDomain::ToEnu(Frame, Expected);
        SkiDomain::GeodeticPoint Recovered;
        Require(SkiDomain::TrySeaLevelGeodeticFromEnu(Frame, Offset.EastM,
            Offset.NorthM, Recovered)
            && std::abs(Recovered.LatitudeDeg - Expected.LatitudeDeg) < 1.0e-8
            && std::abs(Recovered.LongitudeDeg - Expected.LongitudeDeg) < 1.0e-8,
            "terrain ENU coordinates did not recover source geography");
    }

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
    Receipt.SurroundTerrainCoreId = std::string(64, 'a');
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
    {
        SkiDomain::InstalledTerrainReceipt Legacy = Receipt;
        Legacy.SchemaVersion = 1;
        Require(SkiDomain::ValidateInstalledTerrainReceipt(Legacy).Error
            == SkiDomain::CoverEcologyError::UnsupportedSchema,
            "schema-1 receipt without surround accepted");
        SkiDomain::InstalledTerrainReceipt Missing = Receipt;
        Missing.SurroundTerrainCoreId.clear();
        Require(SkiDomain::ValidateInstalledTerrainReceipt(Missing).Error
            == SkiDomain::CoverEcologyError::InvalidContentId,
            "receipt without required surround accepted");
        SkiDomain::InstalledTerrainReceipt Aliased = Receipt;
        Aliased.SurroundTerrainCoreId = Aliased.TerrainCoreId;
        Require(SkiDomain::ValidateInstalledTerrainReceipt(Aliased).Error
            == SkiDomain::CoverEcologyError::InvalidContentId,
            "surround aliased to the core terrain accepted");
    }
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
