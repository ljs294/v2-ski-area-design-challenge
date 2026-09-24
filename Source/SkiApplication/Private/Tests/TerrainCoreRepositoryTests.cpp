#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiApplication/TerrainCoreRepository.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr const char* ManifestHash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

SkiDomain::TerrainCoreManifest MakeProvenanceManifest()
{
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.ContentId = ManifestHash;
    Manifest.GeneratorVersion = "repository-provenance-test";
    Manifest.ProcessingVersions = {"repository-provenance-test"};
    Manifest.Source.SourceId = "s1m-native";
    Manifest.Source.Product = "S1M";
    Manifest.Source.AcquisitionEpoch = "2026-09-24";
    Manifest.Source.HorizontalCrs = "EPSG:6350";
    Manifest.Source.HorizontalDatum = "NAD83(2011)";
    Manifest.Source.VerticalDatum = "NAVD88";
    Manifest.Source.License = "public-domain";
    Manifest.Source.Attribution = "USGS";
    Manifest.Source.NativeEastSpacingM = 1.0;
    Manifest.Source.NativeNorthSpacingM = 1.0;
    SkiDomain::TerrainCoreSource Project;
    Project.SourceId = "project-1m";
    Project.Product = "Project1m";
    Project.AcquisitionEpoch = "2026-09-24";
    Project.HorizontalCrs = "EPSG:6350";
    Project.HorizontalDatum = "NAD83(2011)";
    Project.VerticalDatum = "NAVD88";
    Project.License = "public-domain";
    Project.Attribution = "USGS";
    Project.NativeEastSpacingM = 1.0;
    Project.NativeNorthSpacingM = 1.0;
    Manifest.AdditionalSources.push_back(std::move(Project));
    Manifest.Width = 4;
    Manifest.Height = 3;
    Manifest.DeliveredEastSpacingM = 1.0;
    Manifest.DeliveredNorthSpacingM = 1.0;
    Manifest.SampleCenterBounds = {0.0, 0.0, 3.0, 2.0};
    SkiDomain::ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height, 1.0, 1.0,
        Manifest.SampleCenterBounds, Manifest.OuterBounds);
    SkiDomain::TerrainCoreTilePlan Plan;
    SkiDomain::PlanTerrainCoreTiles(Manifest.Width, Manifest.Height, Plan);
    Manifest.Tiles = std::move(Plan.Tiles);
    std::uint64_t Cursor = 0;
    for (SkiDomain::TerrainCoreTileDescriptor& Tile : Manifest.Tiles)
    {
        const std::uint64_t StoredWidth = Tile.CoreWidth + Tile.HaloWest + Tile.HaloEast;
        const std::uint64_t StoredHeight = Tile.CoreHeight + Tile.HaloNorth + Tile.HaloSouth;
        const std::uint64_t Samples = StoredWidth * StoredHeight;
        Tile.HeightPath = "tiles.bin";
        Tile.HeightSha256 = ManifestHash;
        Tile.HeightOffset = Cursor++;
        Tile.HeightBytes = 1;
        Tile.HeightRawBytes = Samples * sizeof(float);
        Tile.ValidityPath = "tiles.bin";
        Tile.ValiditySha256 = ManifestHash;
        Tile.ValidityOffset = Cursor++;
        Tile.ValidityBytes = 1;
        Tile.ValidityRawBytes = (Samples + 7ULL) / 8ULL;
        // TerrainCore requires every tile provenance ID to resolve to the
        // manifest's source table. Per-sample source identity is recorded by
        // the repository sidecar below.
        Tile.ProvenanceId = Manifest.Source.SourceId;
        Tile.ProcessingVersion = "repository-provenance-test";
    }
    Manifest.Shards.push_back({"tiles.bin", ManifestHash, Cursor});
    return Manifest;
}

void MakeTile(const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    SkiApplication::TerrainCoreTilePayload& Out)
{
    const std::size_t Width = Descriptor.CoreWidth + Descriptor.HaloWest + Descriptor.HaloEast;
    const std::size_t Height = Descriptor.CoreHeight + Descriptor.HaloNorth + Descriptor.HaloSouth;
    Out.Heights.assign(Width * Height, 100.0F);
    Out.Validity.assign(Width * Height, 1);
    const std::uint32_t GlobalWidth = 4;
    const std::uint32_t GlobalHeight = 3;
    for (std::size_t Row = 0; Row < Height; ++Row)
    {
        for (std::size_t Column = 0; Column < Width; ++Column)
        {
            const std::int64_t GlobalColumn = static_cast<std::int64_t>(Descriptor.StartColumn)
                + static_cast<std::int64_t>(Column) - Descriptor.HaloWest;
            const std::int64_t GlobalRow = static_cast<std::int64_t>(Descriptor.StartRow)
                + static_cast<std::int64_t>(Row) - Descriptor.HaloNorth;
            const std::size_t Index = Row * Width + Column;
            if (GlobalColumn < 0 || GlobalRow < 0
                || GlobalColumn >= GlobalWidth || GlobalRow >= GlobalHeight)
            {
                Out.Validity[Index] = 0;
                continue;
            }
            // The fourth canonical sample is NoData. The remaining samples split
            // between the two sources and exercise more than one quality class.
            const std::uint32_t Canonical = static_cast<std::uint32_t>(GlobalRow) * GlobalWidth
                + static_cast<std::uint32_t>(GlobalColumn);
            if (Canonical == 3U) Out.Validity[Index] = 0;
        }
    }
}

bool MakeSidecar(const SkiDomain::TerrainCoreManifest& Manifest,
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    std::vector<std::uint8_t>& OutBytes, std::string& OutError,
    std::vector<std::uint8_t> Provenance = {}, std::vector<std::uint8_t> SourceIndices = {})
{
    const std::size_t Width = Descriptor.CoreWidth + Descriptor.HaloWest + Descriptor.HaloEast;
    const std::size_t Height = Descriptor.CoreHeight + Descriptor.HaloNorth + Descriptor.HaloSouth;
    const std::size_t Samples = Width * Height;
    SkiApplication::TerrainCoreTilePayload Tile;
    MakeTile(Descriptor, Tile);
    if (Provenance.empty()) Provenance.assign(Samples, 0U);
    if (SourceIndices.empty()) SourceIndices.assign(Samples, 0U);
    for (std::size_t Index = 0; Index < Samples; ++Index)
    {
        if (Tile.Validity[Index] == 0)
        {
            Provenance[Index] = static_cast<std::uint8_t>(SkiApplication::TerrainSampleProvenance::NoData);
            SourceIndices[Index] = SkiApplication::TerrainCoreNoSourceIndex;
        }
        else
        {
            const std::uint32_t WidthValue = Descriptor.CoreWidth + Descriptor.HaloWest + Descriptor.HaloEast;
            const std::size_t Row = Index / WidthValue;
            const std::size_t Column = Index % WidthValue;
            const std::int64_t GlobalColumn = static_cast<std::int64_t>(Descriptor.StartColumn)
                + static_cast<std::int64_t>(Column) - Descriptor.HaloWest;
            const std::int64_t GlobalRow = static_cast<std::int64_t>(Descriptor.StartRow)
                + static_cast<std::int64_t>(Row) - Descriptor.HaloNorth;
            if (GlobalColumn >= 0 && GlobalRow >= 0 && GlobalColumn < 4 && GlobalRow < 3)
            {
                const std::uint32_t Canonical = static_cast<std::uint32_t>(GlobalRow) * 4U
                    + static_cast<std::uint32_t>(GlobalColumn);
                SourceIndices[Index] = static_cast<std::uint8_t>(Canonical % 2U);
                Provenance[Index] = Canonical % 2U == 0U ? 0U : 4U;
            }
            else
            {
                // Halo records outside the global grid must be nodata.
                return false;
            }
        }
    }
    return SkiApplication::EncodeTerrainCoreTileProvenanceSidecar(Manifest,
        {Descriptor.LodIndex, Descriptor.TileX, Descriptor.TileY}, Tile.Validity,
        Provenance, SourceIndices, OutBytes, OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreRepositoryLegacyProvenanceTest,
    "MountainPlanner.M5.TerrainCoreRepository.LegacyProvenanceUnavailable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreRepositoryLegacyProvenanceTest::RunTest(const FString&)
{
    std::string Error;
    const SkiDomain::TerrainCoreManifest Manifest = MakeProvenanceManifest();
    TestTrue(TEXT("legacy fixture is a valid TerrainCore manifest"),
        SkiDomain::ValidateTerrainCore(Manifest).Ok());
    const auto Repository = SkiApplication::TerrainCoreRepository::Create(Manifest,
        [](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& Out, std::string&)
        {
            MakeTile(Descriptor, Out);
            return true;
        }, Error);
    TestNotNull(TEXT("legacy TerrainCore repository opens without a provenance sidecar"), Repository.get());
    if (!Repository) return false;
    SkiApplication::TerrainCoreTilePayload Tile;
    TestTrue(TEXT("legacy tile decoding remains available"), Repository->ReadTile({0, 0, 0}, Tile, Error));
    TestEqual(TEXT("legacy height sample count is unchanged"), Tile.Heights.size(), std::size_t(12));
    TestEqual(TEXT("legacy validity sample count is unchanged"), Tile.Validity.size(), std::size_t(12));
    SkiApplication::TerrainCoreProvenanceSummary Summary;
    TestFalse(TEXT("legacy package cannot claim a verified provenance summary"),
        Repository->ReadVerifiedProvenanceSummary(Summary, Error));
    TestTrue(TEXT("legacy provenance summary remains empty"),
        Summary.TerrainCoreId.empty() && Summary.TotalSamples == 0U);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreRepositoryProvenanceSidecarTest,
    "MountainPlanner.M5.TerrainCoreRepository.VerifiedProvenanceSidecar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreRepositoryProvenanceSidecarTest::RunTest(const FString&)
{
    const SkiDomain::TerrainCoreManifest Manifest = MakeProvenanceManifest();
    TestTrue(TEXT("sidecar fixture is a valid TerrainCore manifest"),
        SkiDomain::ValidateTerrainCore(Manifest).Ok());
    const auto TileReader = [](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
        SkiApplication::TerrainCoreTilePayload& Out, std::string&)
    {
        MakeTile(Descriptor, Out);
        return true;
    };
    std::string Error;
    const SkiDomain::TerrainCoreTileDescriptor& Descriptor = Manifest.Tiles.front();
    std::vector<std::uint8_t> Bytes;
    TestTrue(TEXT("canonical provenance sidecar encodes"),
        MakeSidecar(Manifest, Descriptor, Bytes, Error));
    std::vector<std::uint8_t> RepeatedBytes;
    TestTrue(TEXT("identical provenance sidecar encodes again"),
        MakeSidecar(Manifest, Descriptor, RepeatedBytes, Error));
    TestTrue(TEXT("sidecar encoding and integrity are deterministic"), Bytes == RepeatedBytes);
    TestEqual(TEXT("per-tile record path is stable"),
        SkiApplication::TerrainCoreProvenanceSidecarPath({0, 7, 9}),
        std::string("provenance/lod0/7/9.tcp"));
    TestTrue(TEXT("provenance sidecars do not claim a noncanonical LOD path"),
        SkiApplication::TerrainCoreProvenanceSidecarPath({1, 0, 0}).empty());

    const auto Reader = [Bytes](const SkiDomain::TerrainCoreTileDescriptor&,
        const std::uint64_t MaximumBytes, std::vector<std::uint8_t>& Out,
        std::string& OutError, const SkiApplication::TerrainCoreReadCancellation&)
    {
        if (Bytes.size() > MaximumBytes)
        {
            OutError = "test sidecar exceeds requested bound";
            return false;
        }
        Out = Bytes;
        return true;
    };
    const auto Repository = SkiApplication::TerrainCoreRepository::Create(
        Manifest, TileReader, Reader, Error);
    TestNotNull(TEXT("repository with durable provenance sidecar reader opens"), Repository.get());
    if (!Repository) return false;
    SkiApplication::TerrainCoreProvenanceSummary Summary;
    TestTrue(TEXT("verified summary is reconstructed from persisted tile sidecar"),
        Repository->ReadVerifiedProvenanceSummary(Summary, Error));
    TestEqual(TEXT("summary binds to immutable TerrainCore content ID"),
        Summary.TerrainCoreId, Manifest.ContentId);
    TestEqual(TEXT("summary counts exact canonical grid samples"), Summary.TotalSamples, std::uint64_t(12));
    TestEqual(TEXT("summary separates canonical NoData from valid samples"), Summary.NoDataSamples, std::uint64_t(1));
    TestEqual(TEXT("summary exposes a source dictionary fingerprint"), Summary.SourceDictionarySha256.size(), std::size_t(64));
    TestEqual(TEXT("native and project source counts are verified from core samples"),
        Summary.Sources.size(), std::size_t(2));
    if (Summary.Sources.size() == 2)
    {
        TestEqual(TEXT("primary source has six core samples"), Summary.Sources[0].SampleCount, std::uint64_t(6));
        TestEqual(TEXT("additional source has five core samples"), Summary.Sources[1].SampleCount, std::uint64_t(5));
        TestEqual(TEXT("source and native class counts retain their cross-tab"),
            Summary.Sources[0].SamplesByProvenance[0], std::uint64_t(6));
        TestEqual(TEXT("source and Project1m class counts retain their cross-tab"),
            Summary.Sources[1].SamplesByProvenance[4], std::uint64_t(5));
    }

    std::vector<std::uint8_t> Tampered = Bytes;
    if (!Tampered.empty()) Tampered[0] ^= 1U;
    const auto TamperedRepository = SkiApplication::TerrainCoreRepository::Create(
        Manifest, TileReader,
        [Tampered](const SkiDomain::TerrainCoreTileDescriptor&, const std::uint64_t,
            std::vector<std::uint8_t>& Out, std::string&, const SkiApplication::TerrainCoreReadCancellation&)
        {
            Out = Tampered;
            return true;
        }, Error);
    TestNotNull(TEXT("tamper fixture repository constructs"), TamperedRepository.get());
    if (TamperedRepository)
    {
        SkiApplication::TerrainCoreProvenanceSummary Rejected;
        TestFalse(TEXT("tampered sidecar is rejected"),
            TamperedRepository->ReadVerifiedProvenanceSummary(Rejected, Error));
        TestTrue(TEXT("tampered summary is not exposed"), Rejected.TerrainCoreId.empty());
    }

    const auto TruncatedRepository = SkiApplication::TerrainCoreRepository::Create(
        Manifest, TileReader,
        [Bytes](const SkiDomain::TerrainCoreTileDescriptor&, const std::uint64_t,
            std::vector<std::uint8_t>& Out, std::string&, const SkiApplication::TerrainCoreReadCancellation&)
        {
            Out.assign(Bytes.begin(), Bytes.empty() ? Bytes.end() : Bytes.end() - 1);
            return true;
        }, Error);
    TestNotNull(TEXT("truncation fixture repository constructs"), TruncatedRepository.get());
    if (TruncatedRepository)
    {
        SkiApplication::TerrainCoreProvenanceSummary Rejected;
        TestFalse(TEXT("truncated sidecar is rejected"),
            TruncatedRepository->ReadVerifiedProvenanceSummary(Rejected, Error));
    }

    SkiDomain::TerrainCoreManifest ChangedDictionary = Manifest;
    ChangedDictionary.AdditionalSources[0].SourceId = "different-project-source";
    const auto DictionaryMismatch = SkiApplication::TerrainCoreRepository::Create(
        ChangedDictionary, TileReader,
        [Bytes](const SkiDomain::TerrainCoreTileDescriptor&, const std::uint64_t,
            std::vector<std::uint8_t>& Out, std::string&, const SkiApplication::TerrainCoreReadCancellation&)
        {
            Out = Bytes;
            return true;
        }, Error);
    TestNotNull(TEXT("dictionary mismatch fixture repository constructs"), DictionaryMismatch.get());
    if (DictionaryMismatch)
    {
        SkiApplication::TerrainCoreProvenanceSummary Rejected;
        TestFalse(TEXT("source dictionary mismatch is rejected even when the core ID is unchanged"),
            DictionaryMismatch->ReadVerifiedProvenanceSummary(Rejected, Error));
    }

    SkiDomain::TerrainCoreManifest ChangedContent = Manifest;
    ChangedContent.ContentId[0] = ChangedContent.ContentId[0] == '0' ? '1' : '0';
    const auto ContentMismatch = SkiApplication::TerrainCoreRepository::Create(
        ChangedContent, TileReader,
        [Bytes](const SkiDomain::TerrainCoreTileDescriptor&, const std::uint64_t,
            std::vector<std::uint8_t>& Out, std::string&, const SkiApplication::TerrainCoreReadCancellation&)
        {
            Out = Bytes;
            return true;
        }, Error);
    TestNotNull(TEXT("content mismatch fixture repository constructs"), ContentMismatch.get());
    if (ContentMismatch)
    {
        SkiApplication::TerrainCoreProvenanceSummary Rejected;
        TestFalse(TEXT("sidecar from another TerrainCore content ID is rejected"),
            ContentMismatch->ReadVerifiedProvenanceSummary(Rejected, Error));
    }

    SkiDomain::TerrainCoreManifest ChangedGrid = Manifest;
    ChangedGrid.DeliveredEastSpacingM = 2.0;
    ChangedGrid.SampleCenterBounds.EastM = 6.0;
    SkiDomain::ComputeTerrainCoreBounds(ChangedGrid.Width, ChangedGrid.Height,
        ChangedGrid.DeliveredEastSpacingM, ChangedGrid.DeliveredNorthSpacingM,
        ChangedGrid.SampleCenterBounds, ChangedGrid.OuterBounds);
    const auto GridMismatch = SkiApplication::TerrainCoreRepository::Create(
        ChangedGrid, TileReader,
        [Bytes](const SkiDomain::TerrainCoreTileDescriptor&, const std::uint64_t,
            std::vector<std::uint8_t>& Out, std::string&, const SkiApplication::TerrainCoreReadCancellation&)
        {
            Out = Bytes;
            return true;
        }, Error);
    TestNotNull(TEXT("grid mismatch fixture repository constructs"), GridMismatch.get());
    if (GridMismatch)
    {
        SkiApplication::TerrainCoreProvenanceSummary Rejected;
        TestFalse(TEXT("sidecar for a different grid is rejected"),
            GridMismatch->ReadVerifiedProvenanceSummary(Rejected, Error));
    }

    std::vector<std::uint8_t> BadPlane(12U, 0U);
    std::vector<std::uint8_t> BadSources(12U, 0U);
    std::vector<std::uint8_t> BadValidity(12U, 1U);
    BadSources[0] = 65U;
    std::vector<std::uint8_t> RejectedEncoding;
    TestFalse(TEXT("encoder rejects a provenance source index outside the bounded source table"),
        SkiApplication::EncodeTerrainCoreTileProvenanceSidecar(Manifest, {0, 0, 0},
            BadValidity, BadPlane, BadSources, RejectedEncoding, Error));
    BadSources.assign(12U, 0U);
    TestFalse(TEXT("encoder rejects truncated per-sample provenance planes"),
        SkiApplication::EncodeTerrainCoreTileProvenanceSidecar(Manifest, {0, 0, 0},
            BadValidity, std::vector<std::uint8_t>(11U, 0U), BadSources,
            RejectedEncoding, Error));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
