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
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <algorithm>

namespace
{
using namespace SkiPreparation;

struct FCompositeFixtureAsset
{
    FString Path;
    FString Type;
    TArray<uint8> Bytes;
};

FString CompositeInstallTestRoot()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("InstalledTerrainCompositeStore"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

std::string Utf8String(const FString& Value)
{
    const FTCHARToUTF8 Encoded(*Value);
    return std::string(Encoded.Get(), Encoded.Length());
}

FString Sha256Text(const FString& Value)
{
    const FTCHARToUTF8 Encoded(*Value);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
    return SkiPreparation::Sha256(Bytes);
}

bool MakeCore(const FString& Root, const char* SourceId,
    SkiDomain::TerrainCoreManifest& Out, FString& Error)
{
    SkiDomain::Heightfield Field;
    Field.Width = 2;
    Field.Height = 2;
    Field.EastSpacingM = 1.0;
    Field.NorthSpacingM = 1.0;
    Field.NoDataValue = -9999.0;
    Field.CurrentRevision = 1;
    Field.Samples = {100, 101, 102, 103};

    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "composite-install-test-v1";
    Manifest.ProcessingVersions = {"terraincore-derivation-v1"};
    Manifest.Source.SourceId = SourceId;
    Manifest.Source.Product = "synthetic";
    Manifest.Source.AcquisitionEpoch = "2026-09-24";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "WGS84";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "CC0";
    Manifest.Source.Attribution = "composite install fixture";
    Manifest.Source.NativeEastSpacingM = 1.0;
    Manifest.Source.NativeNorthSpacingM = 1.0;
    Manifest.LocalOrigin = {44.0, -71.0, 100.0};
    FString Directory;
    return SkiPreparation::TerrainCorePackageStore(Root).WriteAndActivate(
        Manifest, Field, Directory, Out, Error);
}

bool MakeCover(const FString& Root, SkiDomain::CoverEcologyManifest& Out,
    FString& Error)
{
    SkiDomain::CoverEcologyManifest Manifest;
    Manifest.GeneratorVersion = "composite-cover-test-v1";
    Manifest.Source = {"worldcover", "synthetic", "2021", "fixture", "CC0", "fixture"};
    Manifest.Transform.Width = 2;
    Manifest.Transform.Height = 2;
    Manifest.Transform.LongitudeStepDeg = 0.01;
    Manifest.Transform.LatitudeStepDeg = 0.01;
    Manifest.Transform.SampleCenterBounds = {-71.01, 43.99, -71.0, 44.0};
    SkiDomain::ComputeCoverEcologyOuterBounds(2, 2, 0.01, 0.01,
        Manifest.Transform.SampleCenterBounds, Manifest.Transform.OuterBounds);
    const TArray<uint8> Classes{10, 10, 10, 10};
    const TArray<uint8> Validity{0x0f};
    FString Directory;
    return SkiPreparation::CoverEcologyStore(Root).WriteAndActivate(
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
    Wrapper->SetRaw(Raw.GetData(), Raw.Num(), SiteContextImageryTilePixels,
        SiteContextImageryTilePixels, ERGBFormat::BGRA, 8);
    const auto& Compressed = Wrapper->GetCompressed(90);
    TArray<uint8> Result;
    if (Compressed.Num() > MAX_int32) return Result;
    Result.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
    return Result;
}

FString ImageryPath(const SkiDomain::TerrainCoreTileDescriptor& Tile)
{
    return FString::Printf(TEXT("imagery/lod%u/%u/%u.jpg"),
        static_cast<uint32>(Tile.LodIndex), Tile.TileX, Tile.TileY);
}

bool MakeSiteContext(const FString& Root, const SkiDomain::TerrainCoreManifest& Core,
    SiteContextPackageIndex& Out, FString& Error)
{
    const TArray<uint8> Jpeg = MakeJpegTile();
    if (Jpeg.IsEmpty())
    {
        Error = TEXT("Unable to encode the SiteContext JPEG fixture.");
        return false;
    }
    TArray<FCompositeFixtureAsset> Assets;
    SiteContextManifest Manifest;
    Manifest.GeneratorVersion = "composite-site-context-test-v1";
    Manifest.TerrainCoreId = Core.ContentId;
    Manifest.Attributions = {
        {"OpenStreetMap contributors", "ODbL-1.0", "© OpenStreetMap contributors"},
        {"USGS", "Public domain", "USGS ImageryOnly"},
    };
    SkiDomain::TerrainCoreTilePlan Plan;
    if (!SkiDomain::PlanTerrainCoreTiles(Core.Width, Core.Height, Plan))
    {
        Error = TEXT("Unable to plan the SiteContext fixture pyramid.");
        return false;
    }
    for (const SkiDomain::TerrainCoreTileDescriptor& Key : Plan.Tiles)
    {
        SiteContextImageryTile Tile;
        Tile.LodIndex = Key.LodIndex;
        Tile.LodFactor = Key.LodFactor;
        Tile.TileX = Key.TileX;
        Tile.TileY = Key.TileY;
        Tile.EastMetersPerPixel = Core.DeliveredEastSpacingM * Key.LodFactor;
        Tile.NorthMetersPerPixel = Core.DeliveredNorthSpacingM * Key.LodFactor;
        Tile.AssetPath = Utf8String(ImageryPath(Key));
        Manifest.ImageryTiles.push_back(Tile);
        Assets.Add({ImageryPath(Key), TEXT("jpeg-rgb8-v1"), Jpeg});
    }
    const FString VectorJson = TEXT(
        "{\"schemaVersion\":1,\"features\":[{\"osmId\":\"way/1\",\"kind\":\"road\","
        "\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]}]}");
    const FTCHARToUTF8 VectorBytes(*VectorJson);
    TArray<uint8> VectorAssetBytes;
    VectorAssetBytes.Append(reinterpret_cast<const uint8*>(VectorBytes.Get()), VectorBytes.Length());
    Assets.Add({TEXT("vectors/osm-enu-polylines.json"),
        TEXT("osm-enu-polylines-json-v1"), MoveTemp(VectorAssetBytes)});

    for (const FCompositeFixtureAsset& Asset : Assets)
    {
        Manifest.Assets.push_back({Utf8String(Asset.Path), Utf8String(Asset.Type),
            Utf8String(SkiPreparation::Sha256(Asset.Bytes)),
            static_cast<std::uint64_t>(Asset.Bytes.Num())});
    }
    std::sort(Manifest.Assets.begin(), Manifest.Assets.end(),
        [](const SiteContextAsset& A, const SiteContextAsset& B) { return A.Path < B.Path; });

    SiteContextStore Store(Root);
    FString Directory;
    const SiteContextAssetReader ReadAsset = [&](const FString& RelativePath,
        TArray<uint8>& OutBytes, FString& ReadError)
    {
        const auto Found = std::find_if(Assets.begin(), Assets.end(),
            [&](const FCompositeFixtureAsset& Asset) { return Asset.Path == RelativePath; });
        if (Found == Assets.end())
        {
            ReadError = TEXT("SiteContext fixture asset is missing.");
            return false;
        }
        OutBytes = Found->Bytes;
        return true;
    };
    SiteContextManifest Written;
    return Store.WriteAndActivate(MoveTemp(Manifest), Core, ReadAsset,
        Directory, Written, Error)
        && Store.Open(UTF8_TO_TCHAR(Written.ContentId.c_str()), Core, Out, Error)
        && Store.Verify(Out, Error);
}

CompositeInstallReceipt BuildCompositeReceipt(const TerrainCorePackageIndex& Core,
    const CoverEcologyPackageIndex& Cover, const SiteContextPackageIndex& SiteContext)
{
    CompositeInstallReceipt Receipt;
    Receipt.GeneratorVersion = "composite-install-test-v1";
    Receipt.ProvenanceCounts.S1MNativeQualified = 4;
    SkiDomain::TrySummarizeTerrainQuality(Receipt.ProvenanceCounts, Receipt.Quality);
    const FString QualityId = ComputeTerrainQualityReportId(
        Receipt.ProvenanceCounts, Receipt.Quality);
    const std::string QualityIdUtf8 = Utf8String(QualityId);
    Receipt.Components = {
        {CompositeInstallComponentKind::TerrainCore, CompositeInstallComponentStatus::Verified,
            Core.Manifest.ContentId,
            Utf8String(Sha256Text(SerializeTerrainCoreManifest(Core.Manifest, true)))},
        {CompositeInstallComponentKind::CoverEcology, CompositeInstallComponentStatus::Verified,
            Cover.Manifest.ContentId,
            Utf8String(Sha256Text(SerializeCoverEcologyManifest(Cover.Manifest, true)))},
        {CompositeInstallComponentKind::SiteContext, CompositeInstallComponentStatus::Verified,
            SiteContext.Manifest.ContentId,
            Utf8String(Sha256Text(SerializeSiteContextManifest(SiteContext.Manifest, true)))},
        {CompositeInstallComponentKind::QualityReport, CompositeInstallComponentStatus::Verified,
            QualityIdUtf8, QualityIdUtf8},
    };
    Receipt.ContentId = Utf8String(ComputeCompositeInstallReceiptId(Receipt));
    return Receipt;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstalledTerrainCompositeStoreTest,
    "MountainPlanner.M5.InstalledTerrain.CompositeActivationAndLegacyRead",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInstalledTerrainCompositeStoreTest::RunTest(const FString& Parameters)
{
    using namespace SkiPreparation;
    const FString Root = CompositeInstallTestRoot();
    InstalledTerrainStore Store(Root);
    FString Error;
    TArray<InstalledTerrainLibraryEntry> Entries;
    TestTrue(TEXT("Initial library listing succeeds"), Store.ListVerified(Entries, Error));
    TestEqual(TEXT("Initial library has no playable entries"), Entries.Num(), 0);
    const FString InterruptedStage = FPaths::Combine(Root, TEXT(".installedterrain-staging"),
        TEXT("interrupted.work"));
    TestTrue(TEXT("Interrupted staging directory is created"),
        IFileManager::Get().MakeDirectory(*InterruptedStage, true));
    TestTrue(TEXT("Interrupted receipt fixture is written"), FFileHelper::SaveStringToFile(
        TEXT("partial"), *FPaths::Combine(InterruptedStage, TEXT("receipt.json"))));
    TestTrue(TEXT("Interrupted staging work does not break library enumeration"),
        Store.ListVerified(Entries, Error));
    TestEqual(TEXT("Interrupted staging work never appears playable"), Entries.Num(), 0);

    TerrainCorePackageIndex Core;
    CoverEcologyPackageIndex Cover;
    SiteContextPackageIndex SiteContext;
    SkiDomain::TerrainCoreManifest CoreManifest;
    SkiDomain::CoverEcologyManifest CoverManifest;
    TestTrue(TEXT("TerrainCore fixture activates"), MakeCore(Root, "composite-primary",
        CoreManifest, Error));
    TestTrue(TEXT("TerrainCore fixture reopens"), TerrainCorePackageStore(Root).Open(
        UTF8_TO_TCHAR(CoreManifest.ContentId.c_str()), Core, Error));
    TestTrue(TEXT("CoverEcology fixture activates"), MakeCover(Root, CoverManifest, Error));
    TestTrue(TEXT("CoverEcology fixture reopens"), CoverEcologyStore(Root).Open(
        UTF8_TO_TCHAR(CoverManifest.ContentId.c_str()), Cover, Error));
    TestTrue(TEXT("SiteContext fixture activates with the TerrainCore reference"),
        MakeSiteContext(Root, Core.Manifest, SiteContext, Error));
    if (Core.Manifest.ContentId.empty() || Cover.Manifest.ContentId.empty()
        || SiteContext.Manifest.ContentId.empty())
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }

    CompositeInstallReceipt Partial = BuildCompositeReceipt(Core, Cover, SiteContext);
    Partial.Components.erase(std::remove_if(Partial.Components.begin(), Partial.Components.end(),
        [](const CompositeInstallComponent& Component)
        { return Component.Kind == CompositeInstallComponentKind::SiteContext; }),
        Partial.Components.end());
    FString PartialDirectory = TEXT("sentinel");
    CompositeInstallReceipt PartialOutput;
    TestFalse(TEXT("A composite missing SiteContext cannot activate"), Store.WriteAndActivate(
        Partial, PartialDirectory, PartialOutput, Error));
    TestTrue(TEXT("A rejected partial install has no receipt directory"), PartialDirectory.IsEmpty());
    TestTrue(TEXT("Partial install leaves the library empty"), Store.ListVerified(Entries, Error));
    TestEqual(TEXT("Partial install creates no playable entry"), Entries.Num(), 0);

    CompositeInstallReceipt Receipt = BuildCompositeReceipt(Core, Cover, SiteContext);
    FString ReceiptDirectory;
    CompositeInstallReceipt Written;
    TestTrue(*FString::Printf(TEXT("Complete composite atomically activates: %s"), *Error),
        Store.WriteAndActivate(Receipt, ReceiptDirectory, Written, Error));
    if (ReceiptDirectory.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }

    const FString CompositeId = UTF8_TO_TCHAR(Written.ContentId.c_str());
    InstalledTerrainIndex Reopened;
    TestTrue(TEXT("Schema-3 composite reopens after verifying its components"),
        Store.Open(CompositeId, Reopened, Error));
    TestEqual(TEXT("Reopened receipt selects schema 3"), Reopened.SchemaVersion,
        static_cast<std::uint32_t>(CompositeInstallReceiptSchema));
    TestEqual(TEXT("Reopened schema-3 content identity is preserved"),
        Reopened.CompositeReceipt.ContentId, Written.ContentId);
    TestTrue(TEXT("Complete schema-3 composite appears in the verified library"),
        Store.ListVerified(Entries, Error));
    TestEqual(TEXT("One complete composite is listed"), Entries.Num(), 1);

    const SiteContextAsset& Imagery = SiteContext.Manifest.Assets.front();
    const FString TamperedPath = FPaths::Combine(SiteContext.PackageDirectory,
        UTF8_TO_TCHAR(Imagery.Path.c_str()));
    TArray<uint8> TamperedBytes;
    TestTrue(TEXT("Tamper fixture reads a SiteContext asset"),
        FFileHelper::LoadFileToArray(TamperedBytes, *TamperedPath) && !TamperedBytes.IsEmpty());
    if (!TamperedBytes.IsEmpty())
    {
        TamperedBytes[0] ^= 0x01;
        TestTrue(TEXT("Tamper fixture updates the installed component"),
            FFileHelper::SaveArrayToFile(TamperedBytes, *TamperedPath));
        TestFalse(TEXT("Tampered SiteContext prevents composite open"),
            Store.Open(CompositeId, Reopened, Error));
        TestTrue(TEXT("Tampered composite is omitted from the playable library"),
            Store.ListVerified(Entries, Error));
        TestEqual(TEXT("Only verified packages are listed after component tampering"),
            Entries.Num(), 0);
    }

    // The schema-2 receipt path stays readable for existing installed packages.
    SkiDomain::TerrainCoreManifest LegacyPrimaryManifest;
    SkiDomain::TerrainCoreManifest LegacySurroundManifest;
    TestTrue(TEXT("Legacy primary TerrainCore activates"), MakeCore(Root,
        "legacy-primary", LegacyPrimaryManifest, Error));
    TestTrue(TEXT("Legacy surround TerrainCore activates"), MakeCore(Root,
        "legacy-surround", LegacySurroundManifest, Error));
    TestTrue(TEXT("Legacy schema-2 CoverEcology is already installed"), !Cover.Manifest.ContentId.empty());
    SkiDomain::InstalledTerrainReceipt Legacy;
    Legacy.GeneratorVersion = "legacy-installed-receipt-v2";
    Legacy.TerrainCoreId = LegacyPrimaryManifest.ContentId;
    Legacy.SurroundTerrainCoreId = LegacySurroundManifest.ContentId;
    Legacy.CoverEcologyId = Cover.Manifest.ContentId;
    FString LegacyDirectory;
    SkiDomain::InstalledTerrainReceipt LegacyWritten;
    TestTrue(*FString::Printf(TEXT("Existing schema-2 receipt still activates: %s"), *Error),
        Store.WriteAndActivate(Legacy, LegacyDirectory, LegacyWritten, Error));
    InstalledTerrainIndex LegacyOpened;
    TestTrue(TEXT("Existing schema-2 receipt still opens through the shared path"),
        Store.Open(UTF8_TO_TCHAR(LegacyWritten.ContentId.c_str()), LegacyOpened, Error));
    TestEqual(TEXT("Legacy install remains schema 2"), LegacyOpened.SchemaVersion,
        static_cast<std::uint32_t>(SkiDomain::InstalledTerrainSchema));
    TestEqual(TEXT("Legacy receipt keeps its TerrainCore reference"),
        LegacyOpened.Receipt.TerrainCoreId, Legacy.TerrainCoreId);
    TestTrue(TEXT("Schema-2 legacy install remains in verified library listings"),
        Store.ListVerified(Entries, Error));
    TestEqual(TEXT("Tampered schema-3 package is omitted while schema-2 remains listed"),
        Entries.Num(), 1);
    if (Entries.Num() == 1)
        TestEqual(TEXT("Legacy listing selects the schema-2 receipt"),
            Entries[0].ContentId, UTF8_TO_TCHAR(LegacyWritten.ContentId.c_str()));

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif
