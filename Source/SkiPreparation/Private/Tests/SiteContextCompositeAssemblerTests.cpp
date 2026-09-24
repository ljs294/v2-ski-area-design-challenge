#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/ImageryPyramid.h"
#include "SkiPreparation/OsmVectorPackage.h"
#include "SkiPreparation/OsmVectorSiteContextConverter.h"
#include "SkiPreparation/SiteContextCompositeAssembler.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <algorithm>
#include <unordered_map>

namespace
{
using namespace SkiPreparation;

FString CompositeAssemblerTestRoot()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("SiteContextCompositeAssembler"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

std::string Utf8String(const FString& Value)
{
    const FTCHARToUTF8 Encoded(*Value);
    return std::string(Encoded.Get(), Encoded.Length());
}

SkiDomain::Heightfield MakeCompositeField()
{
    SkiDomain::Heightfield Field;
    Field.Width = 300;
    Field.Height = 2;
    Field.WestM = 0.0;
    Field.NorthM = 1.0;
    Field.EastSpacingM = 1.0;
    Field.NorthSpacingM = 1.0;
    Field.NoDataValue = -9999.0;
    Field.CurrentRevision = 1;
    Field.Samples.resize(static_cast<std::uint64_t>(Field.Width) * Field.Height);
    for (uint32 Column = 0; Column < Field.Width; ++Column)
    {
        Field.Samples[Column] = 100.0F + static_cast<float>(Column) * 0.25F;
        Field.Samples[Field.Width + Column] = 99.5F + static_cast<float>(Column) * 0.25F;
    }
    return Field;
}

bool ActivateCore(const FString& Root, SkiDomain::TerrainCoreManifest& Out, FString& Error)
{
    const SkiDomain::Heightfield Field = MakeCompositeField();
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "m5-composite-assembler-test-v1";
    Manifest.ProcessingVersions = {"terraincore-derivation-v1"};
    Manifest.Source.SourceId = "m5-primary-fixture";
    Manifest.Source.Product = "synthetic bare earth";
    Manifest.Source.AcquisitionEpoch = "2026-09-24";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "WGS84";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "CC0";
    Manifest.Source.Attribution = "Mountain Planner offline fixture";
    Manifest.Source.NativeEastSpacingM = Field.EastSpacingM;
    Manifest.Source.NativeNorthSpacingM = Field.NorthSpacingM;
    Manifest.LocalOrigin = {44.0, -71.0, 100.0};
    Manifest.Width = Field.Width;
    Manifest.Height = Field.Height;
    Manifest.DeliveredEastSpacingM = Field.EastSpacingM;
    Manifest.DeliveredNorthSpacingM = Field.NorthSpacingM;
    Manifest.SampleCenterBounds = {0.0, 0.0, 299.0, 1.0};
    if (!SkiDomain::ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height,
            Manifest.DeliveredEastSpacingM, Manifest.DeliveredNorthSpacingM,
            Manifest.SampleCenterBounds, Manifest.OuterBounds))
    {
        Error = TEXT("Could not construct TerrainCore fixture bounds.");
        return false;
    }
    FString Directory;
    return TerrainCorePackageStore(Root).WriteAndActivate(
        Manifest, Field, Directory, Out, Error);
}

bool ActivateCover(const FString& Root, SkiDomain::CoverEcologyManifest& Out, FString& Error)
{
    SkiDomain::CoverEcologyManifest Manifest;
    Manifest.GeneratorVersion = "m5-cover-fixture-v1";
    Manifest.Source = {"worldcover-fixture", "WorldCover fixture", "2021",
        "offline-fixture", "CC BY 4.0", "ESA WorldCover"};
    Manifest.Transform.Width = 2;
    Manifest.Transform.Height = 2;
    Manifest.Transform.LongitudeStepDeg = 0.01;
    Manifest.Transform.LatitudeStepDeg = 0.01;
    Manifest.Transform.SampleCenterBounds = {-71.01, 43.99, -71.0, 44.0};
    if (!SkiDomain::ComputeCoverEcologyOuterBounds(2, 2, 0.01, 0.01,
            Manifest.Transform.SampleCenterBounds, Manifest.Transform.OuterBounds))
    {
        Error = TEXT("Could not construct CoverEcology fixture bounds.");
        return false;
    }
    const TArray<uint8> Classes{10, 20, 30, 40};
    const TArray<uint8> Validity{0x0f};
    FString Directory;
    return CoverEcologyStore(Root).WriteAndActivate(
        Manifest, Classes, Validity, Directory, Out, Error);
}

TArray<uint8> MakeJpegTile()
{
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
    if (!Wrapper.IsValid()) return {};
    TArray<uint8> Raw;
    Raw.SetNumZeroed(SiteContextImageryTilePixels * SiteContextImageryTilePixels * sizeof(FColor));
    if (!Wrapper->SetRaw(Raw.GetData(), Raw.Num(), SiteContextImageryTilePixels,
            SiteContextImageryTilePixels, ERGBFormat::BGRA, 8)) return {};
    const TArray64<uint8>& Compressed = Wrapper->GetCompressed(90);
    if (Compressed.Num() <= 0 || Compressed.Num() > MAX_int32) return {};
    TArray<uint8> Bytes;
    Bytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
    return Bytes;
}

class FMemoryImagerySource final : public ISiteContextCompositeImagerySource
{
public:
    explicit FMemoryImagerySource(TArray<uint8> InJpeg)
        : Jpeg(MoveTemp(InJpeg)) {}

    void AddPath(const std::string& Path)
    {
        Assets.emplace(Path, Jpeg);
    }

    const TArray<uint8>& FixtureJpeg() const { return Jpeg; }

    bool Inspect(const std::string& RelativePath, const std::uint64_t MaximumBytes,
        const Cancellation& CancellationValue, ImageryPyramidAssetFacts& OutFacts,
        FString& OutError) override
    {
        OutFacts = {};
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("Fixture imagery inspection was cancelled.");
            return false;
        }
        if (MaximumBytes != ImageryPyramidMaxTileBytes)
        {
            OutError = TEXT("Unexpected imagery byte limit.");
            return false;
        }
        const auto Found = Assets.find(RelativePath);
        if (Found == Assets.end())
        {
            OutError = TEXT("Fixture imagery path is missing.");
            return false;
        }
        IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
            TEXT("ImageWrapper"));
        const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
        if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Found->second.GetData(), Found->second.Num()))
        {
            OutError = TEXT("Fixture imagery is not a decodable JPEG.");
            return false;
        }
        OutFacts.Encoding = "jpeg-rgb8-v1";
        OutFacts.Width = static_cast<std::uint16_t>(Wrapper->GetWidth());
        OutFacts.Height = static_cast<std::uint16_t>(Wrapper->GetHeight());
        OutFacts.Bytes = static_cast<std::uint64_t>(Found->second.Num());
        OutFacts.Sha256 = Utf8String(Sha256(Found->second));
        return true;
    }

    bool Read(const std::string& RelativePath, const Cancellation& CancellationValue,
        TArray<uint8>& OutBytes, FString& OutError) override
    {
        OutBytes.Reset();
        if (CancellationValue.IsCancelled())
        {
            OutError = TEXT("Fixture imagery read was cancelled.");
            return false;
        }
        const auto Found = Assets.find(RelativePath);
        if (Found == Assets.end())
        {
            OutError = TEXT("Fixture imagery path is missing.");
            return false;
        }
        OutBytes = Found->second;
        if (bCorruptNextRead && !OutBytes.IsEmpty())
        {
            OutBytes[0] ^= 0x01;
            bCorruptNextRead = false;
        }
        return true;
    }

    bool bCorruptNextRead = false;

private:
    TArray<uint8> Jpeg;
    std::unordered_map<std::string, TArray<uint8>> Assets;
};

bool BuildImagery(const SkiDomain::TerrainCoreManifest& Core, FMemoryImagerySource& Reader,
    ImageryPyramidManifest& OutManifest)
{
    ImagerySourceMetadata Source;
    Source.SourceId = "usgs-imagery-only";
    Source.Product = "USGSImageryOnly";
    Source.ServiceUrlTemplate = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}";
    Source.License = "Public domain, U.S. Government work";
    Source.Attribution = "USGS The National Map";
    Source.TermsUrl = "https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits";
    if (!BuildRequiredImageryPyramid(Core, Source, OutManifest).Ok()) return false;
    for (int32 Index = 0; Index < OutManifest.Tiles.Num(); ++Index)
    {
        ImageryPyramidTile& Tile = OutManifest.Tiles[Index];
        Reader.AddPath(Tile.Path);
        Tile.Sha256 = Utf8String(Sha256(Reader.FixtureJpeg()));
        Tile.Bytes = static_cast<std::uint64_t>(Reader.FixtureJpeg().Num());
    }
    return true;
}

OsmVectorPackage MakeVectorPackage(const SkiDomain::TerrainCoreManifest& Core)
{
    OsmVectorPackage Package;
    Package.TerrainCoreId = Core.ContentId;
    Package.LocalOrigin = Core.LocalOrigin;
    Package.TerrainCoreWidth = Core.Width;
    Package.TerrainCoreHeight = Core.Height;
    Package.TerrainCoreEastSpacingM = Core.DeliveredEastSpacingM;
    Package.TerrainCoreNorthSpacingM = Core.DeliveredNorthSpacingM;
    Package.ExtentM = Core.OuterBounds;
    Package.Source.SourceTimestampUtc = "2026-09-24T12:00:00Z";
    Package.Source.RetrievedAtUtc = "2026-09-24T12:01:00.125Z";
    for (std::uint8_t LayerIndex = 0; LayerIndex < 3; ++LayerIndex)
    {
        OsmVectorLayer& Layer = Package.Layers.emplace_back();
        Layer.Kind = static_cast<OsmVectorLayerKind>(LayerIndex);
        OsmVectorFeature& Feature = Layer.Features.emplace_back();
        Feature.ElementKind = OsmElementKind::Way;
        Feature.OsmId = 100U + LayerIndex;
        Feature.Points = {
            {Core.OuterBounds.WestM, Core.OuterBounds.SouthM},
            {Core.OuterBounds.EastM, Core.OuterBounds.NorthM},
        };
    }
    Package.ContentId = Utf8String(ComputeOsmVectorPackageContentId(Package));
    return Package;
}

bool ListVerified(const FString& Root, TArray<InstalledTerrainLibraryEntry>& Entries,
    FString& Error)
{
    return InstalledTerrainStore(Root).ListVerified(Entries, Error);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextCompositeAssemblerAtomicInstallTest,
    "MountainPlanner.M5.SiteContextCompositeAssembler.AtomicInstallAndPathMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextCompositeAssemblerAtomicInstallTest::RunTest(const FString& Parameters)
{
    const FString Root = CompositeAssemblerTestRoot();
    FString Error;
    SkiDomain::TerrainCoreManifest CoreManifest;
    TestTrue(TEXT("Verified TerrainCore fixture writes"), ActivateCore(Root, CoreManifest, Error));
    if (CoreManifest.ContentId.empty()) return false;
    TerrainCorePackageIndex CoreIndex;
    TestTrue(TEXT("TerrainCore fixture reopens and verifies"),
        TerrainCorePackageStore(Root).Open(UTF8_TO_TCHAR(CoreManifest.ContentId.c_str()), CoreIndex, Error)
            && TerrainCorePackageStore(Root).Verify(CoreIndex, Error));

    SkiDomain::CoverEcologyManifest CoverManifest;
    TestTrue(TEXT("Verified CoverEcology fixture writes"), ActivateCover(Root, CoverManifest, Error));
    if (CoverManifest.ContentId.empty()) return false;
    CoverEcologyPackageIndex CoverIndex;
    TestTrue(TEXT("CoverEcology fixture reopens and verifies"),
        CoverEcologyStore(Root).Open(UTF8_TO_TCHAR(CoverManifest.ContentId.c_str()), CoverIndex, Error)
            && CoverEcologyStore(Root).Verify(CoverIndex, Error));

    const FString InterruptedStage = FPaths::Combine(Root, TEXT(".installedterrain-staging"),
        TEXT("interrupted.work"));
    TestTrue(TEXT("Interrupted install staging directory is created"),
        IFileManager::Get().MakeDirectory(*InterruptedStage, true));
    TestTrue(TEXT("Interrupted staging has a partial receipt"), FFileHelper::SaveStringToFile(
        TEXT("partial"), *FPaths::Combine(InterruptedStage, TEXT("receipt.json"))));
    TArray<InstalledTerrainLibraryEntry> Entries;
    TestTrue(TEXT("Interrupted stage does not break listing"), ListVerified(Root, Entries, Error));
    TestEqual(TEXT("Interrupted stage is absent from the installed library"), Entries.Num(), 0);

    TArray<uint8> Jpeg = MakeJpegTile();
    TestTrue(TEXT("Fixture JPEG encodes"), !Jpeg.IsEmpty());
    if (Jpeg.IsEmpty()) return false;
    FMemoryImagerySource ImagerySource(Jpeg);
    ImageryPyramidManifest Imagery;
    TestTrue(TEXT("Complete fixture imagery pyramid builds"), BuildImagery(
        CoreIndex.Manifest, ImagerySource, Imagery));

    const OsmVectorPackage VectorPackage = MakeVectorPackage(CoreIndex.Manifest);
    TestTrue(TEXT("OSM fixture validates before conversion"),
        ValidateOsmVectorPackage(CoreIndex.Manifest, VectorPackage).Ok());
    Cancellation CancellationValue;
    OsmVectorSiteContextAsset Vectors;
    TestTrue(TEXT("Verified OSM package converts to SiteContext schema 2 asset"),
        ConvertVerifiedOsmVectorPackageToSiteContextAsset(CoreIndex.Manifest,
            VectorPackage, CancellationValue, Vectors, Error));
    if (Vectors.VectorJsonUtf8.IsEmpty()) return false;

    SkiDomain::TerrainProvenanceCounts Counts;
    Counts.S1MNativeQualified = CoreIndex.Manifest.Width * CoreIndex.Manifest.Height;
    SkiDomain::TerrainQualitySourceFacts SourceFacts;
    SourceFacts.SourceId = "m5-primary-fixture";
    SourceFacts.Product = SkiDomain::ElevationProduct::S1M;
    SourceFacts.SampleFraction = 1.0;
    SourceFacts.SampleFractionUnknown = false;
    SkiDomain::TerrainQualityReport Quality;
    TestTrue(TEXT("Fixture terrain quality report is valid"),
        SkiDomain::TrySummarizeTerrainQuality(Counts, {SourceFacts}, Quality));

    // The Y/X input is intentionally asymmetric for LOD0 tile (1,0).
    ImageryPyramidManifest WrongConvention = Imagery;
    bool ChangedPath = false;
    for (int32 Index = 0; Index < WrongConvention.Tiles.Num(); ++Index)
    {
        ImageryPyramidTile& Tile = WrongConvention.Tiles[Index];
        if (Tile.LodIndex == 0 && Tile.TileX == 1 && Tile.TileY == 0)
        {
            Tile.Path = "imagery/lod0/1/0.jpg";
            ChangedPath = true;
            break;
        }
    }
    TestTrue(TEXT("Fixture includes an asymmetric imagery tile key"), ChangedPath);
    SiteContextCompositeAssemblyResult FailedResult;
    TestFalse(TEXT("X/Y-swapped imagery input path is rejected fail-closed"),
        AssembleAndActivateSiteContextComposite(Root,
            UTF8_TO_TCHAR(CoreIndex.Manifest.ContentId.c_str()),
            UTF8_TO_TCHAR(CoverIndex.Manifest.ContentId.c_str()),
            TEXT("m5-composite-assembler-test-v1"), WrongConvention, ImagerySource,
            Vectors, Counts, Quality, CancellationValue, FailedResult, Error));
    TestTrue(TEXT("Rejected path mapping leaves no partial result"),
        FailedResult.Receipt.ContentId.empty() && FailedResult.SiteContext.ContentId.empty());
    TestTrue(TEXT("Rejected path mapping leaves no installed entry"),
        ListVerified(Root, Entries, Error));
    TestEqual(TEXT("No playable entry appears after an invalid imagery manifest"), Entries.Num(), 0);

    FMemoryImagerySource CorruptImagerySource(Jpeg);
    ImageryPyramidManifest CorruptImagery;
    TestTrue(TEXT("Corrupt-read fixture imagery pyramid builds"), BuildImagery(
        CoreIndex.Manifest, CorruptImagerySource, CorruptImagery));
    CorruptImagerySource.bCorruptNextRead = true;
    TestFalse(TEXT("Bytes changing between imagery verification and staging are rejected"),
        AssembleAndActivateSiteContextComposite(Root,
            UTF8_TO_TCHAR(CoreIndex.Manifest.ContentId.c_str()),
            UTF8_TO_TCHAR(CoverIndex.Manifest.ContentId.c_str()),
            TEXT("m5-composite-assembler-test-v1"), CorruptImagery, CorruptImagerySource,
            Vectors, Counts, Quality, CancellationValue, FailedResult, Error));
    TestTrue(TEXT("Invalid component leaves no playable entry"),
        ListVerified(Root, Entries, Error));
    TestEqual(TEXT("No playable entry appears before all components verify"), Entries.Num(), 0);

    FMemoryImagerySource ValidImagerySource(Jpeg);
    ImageryPyramidManifest ValidImagery;
    TestTrue(TEXT("Final fixture imagery pyramid builds"), BuildImagery(
        CoreIndex.Manifest, ValidImagerySource, ValidImagery));
    SiteContextCompositeAssemblyResult Installed;
    const bool bInstalled = AssembleAndActivateSiteContextComposite(Root,
        UTF8_TO_TCHAR(CoreIndex.Manifest.ContentId.c_str()),
        UTF8_TO_TCHAR(CoverIndex.Manifest.ContentId.c_str()),
        TEXT("m5-composite-assembler-test-v1"), ValidImagery, ValidImagerySource,
        Vectors, Counts, Quality, CancellationValue, Installed, Error);
    TestTrue(*FString::Printf(TEXT("Verified components publish the schema-3 composite: %s"), *Error),
        bInstalled);
    if (!bInstalled)
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }

    TestEqual(TEXT("Installed component uses SiteContext schema 2"),
        Installed.SiteContext.SchemaVersion, static_cast<std::uint32_t>(SiteContextSchema));
    TestTrue(TEXT("Installed vector asset uses the schema-2 multipart encoding"),
        Installed.SiteContext.VectorEncoding == SiteContextVectorEncodingV2);
    TestEqual(TEXT("Atomic receipt uses composite schema 3"),
        Installed.Receipt.SchemaVersion,
        static_cast<std::uint32_t>(CompositeInstallReceiptSchema));
    const SiteContextVectorSourceLineage& StoredLineage = Installed.SiteContext.VectorSource;
    TestTrue(TEXT("OSM source package, TerrainCore binding, and extent persist in schema 2"),
        StoredLineage.SourcePackageContentId == Vectors.SourcePackageContentId
            && StoredLineage.TerrainCoreId == Vectors.TerrainCoreId
            && StoredLineage.ExtentM.WestM == Vectors.ExtentM.WestM
            && StoredLineage.ExtentM.SouthM == Vectors.ExtentM.SouthM
            && StoredLineage.ExtentM.EastM == Vectors.ExtentM.EastM
            && StoredLineage.ExtentM.NorthM == Vectors.ExtentM.NorthM);
    TestTrue(TEXT("OSM provider, endpoint, timestamps, license, and attribution persist"),
        StoredLineage.Provider == Vectors.Source.Provider
            && StoredLineage.Endpoint == Vectors.Source.Endpoint
            && StoredLineage.SourceTimestampUtc == Vectors.Source.SourceTimestampUtc
            && StoredLineage.RetrievedAtUtc == Vectors.Source.RetrievedAtUtc
            && StoredLineage.License == Vectors.Source.License
            && StoredLineage.Attribution == Vectors.Source.Attribution
            && StoredLineage.AttributionUrl == Vectors.Source.AttributionUrl);
    TestTrue(TEXT("Complete composite appears after reopen verification"),
        ListVerified(Root, Entries, Error));
    TestEqual(TEXT("Exactly one playable composite is listed"), Entries.Num(), 1);

    const SiteContextImageryTile* AsymmetricOutputTile = nullptr;
    for (const SiteContextImageryTile& Tile : Installed.SiteContext.ImageryTiles)
    {
        if (Tile.LodIndex == 0 && Tile.TileX == 1 && Tile.TileY == 0)
        {
            AsymmetricOutputTile = &Tile;
            break;
        }
    }
    TestTrue(TEXT("Mapped SiteContext tile remains indexed as X=1,Y=0"),
        AsymmetricOutputTile != nullptr);
    if (AsymmetricOutputTile)
    {
        TestEqual(TEXT("SiteContext uses its own lod/X/Y output path"),
            UTF8_TO_TCHAR(AsymmetricOutputTile->AssetPath.c_str()), TEXT("imagery/lod0/1/0.jpg"));
        SiteContextPackageIndex SiteIndex;
        SiteContextStore SiteStore(Root);
        TestTrue(TEXT("SiteContext package reopens against verified TerrainCore"),
            SiteStore.Open(UTF8_TO_TCHAR(Installed.SiteContext.ContentId.c_str()),
                CoreIndex.Manifest, SiteIndex, Error));
        TArray<uint8> StoredJpeg;
        TestTrue(TEXT("Translated tile asset reads through the SiteContext index"),
            SiteStore.ReadAsset(SiteIndex, TEXT("imagery/lod0/1/0.jpg"), StoredJpeg, Error));
        TestEqual(TEXT("Translated tile contains bytes from imagery input lod/Y/X path"),
            StoredJpeg, Jpeg);
    }

    InstalledTerrainIndex Reopened;
    TestTrue(TEXT("Installed receipt reopens with verified components"),
        InstalledTerrainStore(Root).Open(
            UTF8_TO_TCHAR(Installed.Receipt.ContentId.c_str()), Reopened, Error));
    TestEqual(TEXT("Receipt links the reopened SiteContext component"),
        Reopened.CompositeReceipt.Components[2].ContentId, Installed.SiteContext.ContentId);

    SiteContextManifest TamperedLineage = Installed.SiteContext;
    TamperedLineage.VectorSource.SourcePackageContentId = std::string(64, 'e');
    SiteContextManifest RejectedTamperedLineage;
    FString TamperedLineageError;
    TestFalse(TEXT("Changing structured OSM lineage without changing content identity is rejected"),
        ParseSiteContextManifest(SerializeSiteContextManifest(TamperedLineage, true),
            CoreIndex.Manifest, RejectedTamperedLineage, TamperedLineageError));

    const FString ExistingReceiptDirectory = Installed.InstalledReceiptDirectory;
    TestTrue(TEXT("Fixture receipt directory exists before activation-failure setup"),
        IFileManager::Get().DirectoryExists(*ExistingReceiptDirectory));
    TestTrue(TEXT("Fixture removes its known-good composite receipt for the failure case"),
        IFileManager::Get().DeleteDirectory(*ExistingReceiptDirectory, false, true));
    TestTrue(TEXT("Fixture creates a corrupt pre-existing receipt target"),
        IFileManager::Get().MakeDirectory(*ExistingReceiptDirectory, true));
    TestTrue(TEXT("Fixture writes a noncanonical collision receipt"),
        FFileHelper::SaveStringToFile(TEXT("corrupt activation target"),
            *FPaths::Combine(ExistingReceiptDirectory, TEXT("receipt.json"))));
    TestTrue(TEXT("Corrupt pre-existing receipt is omitted from verified listing"),
        ListVerified(Root, Entries, Error));
    TestEqual(TEXT("No installed entry remains before the failed activation attempt"), Entries.Num(), 0);

    FMemoryImagerySource BlockedActivationSource(Jpeg);
    ImageryPyramidManifest BlockedActivationImagery;
    TestTrue(TEXT("Activation-failure fixture imagery rebuilds identically"),
        BuildImagery(CoreIndex.Manifest, BlockedActivationSource, BlockedActivationImagery));
    SiteContextCompositeAssemblyResult BlockedActivationResult;
    TestFalse(TEXT("Corrupt receipt collision prevents final composite activation"),
        AssembleAndActivateSiteContextComposite(Root,
            UTF8_TO_TCHAR(CoreIndex.Manifest.ContentId.c_str()),
            UTF8_TO_TCHAR(CoverIndex.Manifest.ContentId.c_str()),
            TEXT("m5-composite-assembler-test-v1"), BlockedActivationImagery,
            BlockedActivationSource, Vectors, Counts, Quality, CancellationValue,
            BlockedActivationResult, Error));
    TestTrue(TEXT("Failed final activation leaves the installed library readable"),
        ListVerified(Root, Entries, Error));
    TestEqual(TEXT("Failed final activation publishes no installed library entry"), Entries.Num(), 0);
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif
