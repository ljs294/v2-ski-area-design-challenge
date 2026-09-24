#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <algorithm>

namespace
{
FString SiteContextTestRoot(const TCHAR* Label)
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Label,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

struct SiteContextFixtureAsset
{
    FString Path;
    FString Type;
    TArray<uint8> Bytes;
};

SkiDomain::TerrainCoreManifest TestTerrainCore()
{
    SkiDomain::TerrainCoreManifest Core;
    Core.ContentId = std::string(64, 'c');
    Core.Width = 2;
    Core.Height = 2;
    Core.DeliveredEastSpacingM = 1.0;
    Core.DeliveredNorthSpacingM = 2.0;
    Core.OuterBounds = {-0.5, -1.0, 1.5, 3.0};
    return Core;
}

TArray<uint8> MakeJpegTile()
{
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
    TArray<uint8> Raw;
    Raw.SetNumZeroed(SkiPreparation::SiteContextImageryTilePixels
        * SkiPreparation::SiteContextImageryTilePixels * sizeof(FColor));
    if (!Wrapper.IsValid()) return {};
    Wrapper->SetRaw(Raw.GetData(), Raw.Num(), SkiPreparation::SiteContextImageryTilePixels,
        SkiPreparation::SiteContextImageryTilePixels, ERGBFormat::BGRA, 8);
    const auto& Compressed = Wrapper->GetCompressed(90);
    TArray<uint8> Bytes;
    if (Compressed.Num() > MAX_int32) return {};
    Bytes.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
    return Bytes;
}

FString TestImageryPath(const SkiDomain::TerrainCoreTileDescriptor& Tile)
{
    return FString::Printf(TEXT("imagery/lod%u/%u/%u.jpg"),
        static_cast<uint32>(Tile.LodIndex), Tile.TileX, Tile.TileY);
}

SkiPreparation::SiteContextManifest TestSiteContextManifest(
    const SkiDomain::TerrainCoreManifest& Core,
    TArray<SiteContextFixtureAsset>& OutAssets,
    const TArray<uint8>& Jpeg)
{
    using namespace SkiPreparation;
    SiteContextManifest Manifest;
    Manifest.SchemaVersion = 1; // Characterize the pre-lineage schema as read-only legacy.
    Manifest.ContentId = std::string(64, '0');
    Manifest.GeneratorVersion = "site-context-test-v1";
    Manifest.TerrainCoreId = Core.ContentId;
    Manifest.VectorEncoding = SiteContextVectorEncodingV1;
    Manifest.Attributions = {
        {"OpenStreetMap contributors", "ODbL-1.0", "© OpenStreetMap contributors"},
        {"USGS", "Public domain", "USGS ImageryOnly"},
    };
    SkiDomain::TerrainCoreTilePlan Plan;
    SkiDomain::PlanTerrainCoreTiles(Core.Width, Core.Height, Plan);
    for (const SkiDomain::TerrainCoreTileDescriptor& Key : Plan.Tiles)
    {
        SiteContextImageryTile Tile;
        Tile.LodIndex = Key.LodIndex;
        Tile.LodFactor = Key.LodFactor;
        Tile.TileX = Key.TileX;
        Tile.TileY = Key.TileY;
        Tile.EastMetersPerPixel = Core.DeliveredEastSpacingM * Key.LodFactor;
        Tile.NorthMetersPerPixel = Core.DeliveredNorthSpacingM * Key.LodFactor;
        Tile.AssetPath = TCHAR_TO_UTF8(*TestImageryPath(Key));
        Manifest.ImageryTiles.push_back(Tile);

        SiteContextFixtureAsset Asset;
        Asset.Path = UTF8_TO_TCHAR(Tile.AssetPath.c_str());
        Asset.Type = TEXT("jpeg-rgb8-v1");
        Asset.Bytes = Jpeg;
        OutAssets.Add(MoveTemp(Asset));
    }
    SiteContextFixtureAsset Vectors;
    Vectors.Path = TEXT("vectors/osm-enu-polylines.json");
    Vectors.Type = UTF8_TO_TCHAR(Manifest.VectorEncoding.c_str());
    const FString VectorJson = TEXT(
        "{\"schemaVersion\":1,\"features\":[{\"osmId\":\"way/1\",\"kind\":\"road\","
        "\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]}]}");
    const FTCHARToUTF8 VectorBytes(*VectorJson);
    Vectors.Bytes.Append(reinterpret_cast<const uint8*>(VectorBytes.Get()), VectorBytes.Length());
    OutAssets.Add(MoveTemp(Vectors));
    return Manifest;
}

SkiPreparation::SiteContextVectorSourceLineage TestVectorSourceLineage(
    const SkiDomain::TerrainCoreManifest& Core)
{
    using namespace SkiPreparation;
    SiteContextVectorSourceLineage Lineage;
    Lineage.SourcePackageContentId = std::string(64, 'd');
    Lineage.TerrainCoreId = Core.ContentId;
    Lineage.ExtentM = Core.OuterBounds;
    Lineage.Provider = "openstreetmap-overpass";
    Lineage.Endpoint = "https://overpass-api.de/api/interpreter";
    Lineage.SourceTimestampUtc = "2026-09-24T12:00:00Z";
    Lineage.RetrievedAtUtc = "2026-09-24T12:01:00.125Z";
    Lineage.License = "ODbL-1.0";
    Lineage.Attribution = "© OpenStreetMap contributors";
    Lineage.AttributionUrl = "https://www.openstreetmap.org/copyright";
    return Lineage;
}

void AddDeclaredAssets(SkiPreparation::SiteContextManifest& Manifest,
    const TArray<SiteContextFixtureAsset>& Inputs)
{
    for (const SiteContextFixtureAsset& Input : Inputs)
    {
        const FTCHARToUTF8 Path(*Input.Path);
        const FTCHARToUTF8 Type(*Input.Type);
        Manifest.Assets.push_back({std::string(Path.Get(), Path.Length()),
            std::string(Type.Get(), Type.Length()),
            std::string(TCHAR_TO_UTF8(*SkiPreparation::Sha256(Input.Bytes))),
            static_cast<std::uint64_t>(Input.Bytes.Num())});
    }
    std::sort(Manifest.Assets.begin(), Manifest.Assets.end(),
        [](const SkiPreparation::SiteContextAsset& A,
            const SkiPreparation::SiteContextAsset& B) { return A.Path < B.Path; });
}

bool ReplaceVectorAssetJson(SkiPreparation::SiteContextManifest& Manifest,
    TArray<SiteContextFixtureAsset>& Inputs, const FString& Json, const FString& Encoding)
{
    const FString VectorPath = TEXT("vectors/osm-enu-polylines.json");
    const auto Fixture = std::find_if(Inputs.begin(), Inputs.end(),
        [&](const SiteContextFixtureAsset& Candidate) { return Candidate.Path == VectorPath; });
    const auto Descriptor = std::find_if(Manifest.Assets.begin(), Manifest.Assets.end(),
        [&](const SkiPreparation::SiteContextAsset& Candidate)
        { return Candidate.Path == "vectors/osm-enu-polylines.json"; });
    if (Fixture == Inputs.end() || Descriptor == Manifest.Assets.end()) return false;

    const FTCHARToUTF8 EncodedJson(*Json);
    Fixture->Type = Encoding;
    Fixture->Bytes.Reset();
    Fixture->Bytes.Append(reinterpret_cast<const uint8*>(EncodedJson.Get()), EncodedJson.Length());
    Manifest.VectorEncoding = TCHAR_TO_UTF8(*Encoding);
    Descriptor->Type = Manifest.VectorEncoding;
    Descriptor->Length = static_cast<std::uint64_t>(Fixture->Bytes.Num());
    Descriptor->Sha256 = TCHAR_TO_UTF8(*SkiPreparation::Sha256(Fixture->Bytes));
    return true;
}

bool EditVectorSourceJsonField(FString& Json, const TCHAR* FieldName,
    const FString& Replacement, const bool RemoveField)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root) return false;
    const TSharedPtr<FJsonObject>* Source = nullptr;
    if (!Root->TryGetObjectField(TEXT("vectorSource"), Source) || !Source || !*Source
        || !(*Source)->HasField(FieldName))
        return false;
    if (RemoveField) (*Source)->RemoveField(FieldName);
    else (*Source)->SetStringField(FieldName, Replacement);
    Json.Reset();
    return FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json, 0));
}

SkiPreparation::CompositeInstallReceipt CompleteReceipt()
{
    using namespace SkiPreparation;
    CompositeInstallReceipt Receipt;
    Receipt.GeneratorVersion = "composite-install-test-v1";
    Receipt.ProvenanceCounts.S1MNativeQualified = 100;
    SkiDomain::TerrainQualitySourceFacts Source;
    Source.SourceId = "s1m-fixture-tile";
    Source.Product = SkiDomain::ElevationProduct::S1M;
    Source.SampleFraction = 1.0;
    Source.SampleFractionUnknown = false;
    SkiDomain::TrySummarizeTerrainQuality(Receipt.ProvenanceCounts, {Source}, Receipt.Quality);
    const FString QualityId = ComputeTerrainQualityReportId(
        Receipt.ProvenanceCounts, Receipt.Quality);
    const std::string QualityIdUtf8 = TCHAR_TO_UTF8(*QualityId);
    Receipt.Components = {
        {CompositeInstallComponentKind::TerrainCore, CompositeInstallComponentStatus::Verified,
            std::string(64, 'a'), std::string(64, 'b')},
        {CompositeInstallComponentKind::CoverEcology, CompositeInstallComponentStatus::Verified,
            std::string(64, 'c'), std::string(64, 'd')},
        {CompositeInstallComponentKind::SiteContext, CompositeInstallComponentStatus::Verified,
            std::string(64, 'e'), std::string(64, 'f')},
        {CompositeInstallComponentKind::QualityReport, CompositeInstallComponentStatus::Verified,
            QualityIdUtf8, QualityIdUtf8},
    };
    Receipt.ContentId = TCHAR_TO_UTF8(*ComputeCompositeInstallReceiptId(Receipt));
    return Receipt;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextPyramidStoreTest,
    "MountainPlanner.M5.SiteContext.PyramidStoreAndIntegrity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextPyramidStoreTest::RunTest(const FString& Parameters)
{
    using namespace SkiPreparation;
    const SkiDomain::TerrainCoreManifest Core = TestTerrainCore();
    const TArray<uint8> Jpeg = MakeJpegTile();
    TestTrue(TEXT("Test imagery encoder produced bytes"), !Jpeg.IsEmpty());
    if (Jpeg.IsEmpty()) return false;

    TArray<SiteContextFixtureAsset> Inputs;
    SiteContextManifest Input = TestSiteContextManifest(Core, Inputs, Jpeg);
    AddDeclaredAssets(Input, Inputs);
    SiteContextValidation Validation = ValidateSiteContextManifest(Input, Core);
    TestTrue(TEXT("Pyramid keys and per-LOD spacing match TerrainCore"), Validation.Ok());
    if (!Validation.Ok()) return false;

    FString Root = SiteContextTestRoot(TEXT("SiteContextStore"));
    SiteContextStore Store(Root);
    FString Directory, Error;
    SiteContextManifest Written;
    const SiteContextAssetReader Reader = [&](const FString& RelativePath,
        TArray<uint8>& OutBytes, FString& ReaderError)
    {
        const auto Found = std::find_if(Inputs.begin(), Inputs.end(),
            [&](const SiteContextFixtureAsset& Candidate)
            { return Candidate.Path == RelativePath; });
        if (Found == Inputs.end())
        {
            ReaderError = TEXT("Fixture asset is missing.");
            return false;
        }
        OutBytes = Found->Bytes;
        return true;
    };
    TestTrue(*FString::Printf(TEXT("SiteContext activates after verification: %s"), *Error),
        Store.WriteAndActivate(Input, Core, Reader, Directory, Written, Error));
    if (Directory.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }
    SiteContextPackageIndex Index;
    const FString Id = UTF8_TO_TCHAR(Written.ContentId.c_str());
    TestEqual(TEXT("Legacy fixture remains schema 1"), Written.SchemaVersion, 1U);
    const FString LegacyManifestJson = SerializeSiteContextManifest(Written, true);
    TestFalse(TEXT("Legacy manifest shape omits the new vector-source lineage field"),
        LegacyManifestJson.Contains(TEXT("vectorSource")));
    SiteContextManifest ParsedLegacyManifest;
    TestTrue(TEXT("Pre-lineage SiteContext schema 1 remains readable"),
        ParseSiteContextManifest(LegacyManifestJson, Core, ParsedLegacyManifest, Error));
    TestEqual(TEXT("Legacy round-trip retains its TerrainCore binding"),
        ParsedLegacyManifest.TerrainCoreId, Written.TerrainCoreId);
    TestTrue(TEXT("SiteContext reopens against its TerrainCore identity"),
        Store.Open(Id, Core, Index, Error));
    TestTrue(TEXT("SiteContext verifies all declared files and decoded image tiles"),
        Store.Verify(Index, Error));
    TArray<uint8> Loaded;
    TestTrue(TEXT("A declared imagery asset can be read with its hash checked"),
        Store.ReadAsset(Index, UTF8_TO_TCHAR(Written.Assets[0].Path.c_str()), Loaded, Error));
    TestEqual(TEXT("Read imagery bytes match the fixture"), Loaded, Jpeg);

    SkiDomain::TerrainCoreManifest WrongCore = Core;
    WrongCore.ContentId = std::string(64, 'd');
    SiteContextPackageIndex Mismatched;
    TestFalse(TEXT("A SiteContext package cannot be rebound to another TerrainCore"),
        Store.Open(Id, WrongCore, Mismatched, Error));

    const FString TamperedPath = FPaths::Combine(Directory,
        UTF8_TO_TCHAR(Written.Assets[0].Path.c_str()));
    TArray<uint8> Tampered = Jpeg;
    Tampered[0] ^= 0x01;
    TestTrue(TEXT("Tamper fixture writes"), FFileHelper::SaveArrayToFile(Tampered, *TamperedPath));
    TestFalse(TEXT("A changed component hash prevents SiteContext verification"),
        Store.Verify(Index, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);

    TArray<SiteContextFixtureAsset> InvalidVectorInputs = Inputs;
    const FString InvalidVectorPath = TEXT("vectors/osm-enu-polylines.json");
    const auto VectorFixture = std::find_if(InvalidVectorInputs.begin(), InvalidVectorInputs.end(),
        [&](const SiteContextFixtureAsset& Asset) { return Asset.Path == InvalidVectorPath; });
    TestTrue(TEXT("Vector fixture is present"), VectorFixture != InvalidVectorInputs.end());
    if (VectorFixture == InvalidVectorInputs.end()) return false;
    const FString InvalidVectorJson = TEXT("{\"schemaVersion\":1,\"features\":[{}]}");
    const FTCHARToUTF8 InvalidVectorBytes(*InvalidVectorJson);
    VectorFixture->Bytes.Reset();
    VectorFixture->Bytes.Append(reinterpret_cast<const uint8*>(InvalidVectorBytes.Get()),
        InvalidVectorBytes.Length());
    SiteContextManifest InvalidVectors = Input;
    const auto VectorDescriptor = std::find_if(InvalidVectors.Assets.begin(), InvalidVectors.Assets.end(),
        [&](const SiteContextAsset& Asset) { return Asset.Path == "vectors/osm-enu-polylines.json"; });
    TestTrue(TEXT("Vector manifest descriptor is present"), VectorDescriptor != InvalidVectors.Assets.end());
    if (VectorDescriptor == InvalidVectors.Assets.end()) return false;
    VectorDescriptor->Length = static_cast<std::uint64_t>(VectorFixture->Bytes.Num());
    VectorDescriptor->Sha256 = TCHAR_TO_UTF8(*Sha256(VectorFixture->Bytes));
    const SiteContextAssetReader InvalidReader = [&](const FString& RelativePath,
        TArray<uint8>& OutBytes, FString& ReaderError)
    {
        const auto Found = std::find_if(InvalidVectorInputs.begin(), InvalidVectorInputs.end(),
            [&](const SiteContextFixtureAsset& Candidate)
            { return Candidate.Path == RelativePath; });
        if (Found == InvalidVectorInputs.end())
        {
            ReaderError = TEXT("Fixture asset is missing.");
            return false;
        }
        OutBytes = Found->Bytes;
        return true;
    };
    FString RejectedDirectory = TEXT("sentinel");
    SiteContextManifest RejectedManifest;
    TestFalse(TEXT("Malformed ENU polyline records prevent SiteContext activation"),
        Store.WriteAndActivate(InvalidVectors, Core, InvalidReader,
            RejectedDirectory, RejectedManifest, Error));
    TestTrue(TEXT("Malformed vector install leaves no activated component"),
        RejectedDirectory.IsEmpty() && RejectedManifest.ContentId.empty());
    IFileManager::Get().DeleteDirectory(*Root, false, true);

    TArray<SiteContextFixtureAsset> OutOfBoundsInputs = Inputs;
    const auto OutOfBoundsVector = std::find_if(OutOfBoundsInputs.begin(), OutOfBoundsInputs.end(),
        [&](const SiteContextFixtureAsset& Asset) { return Asset.Path == InvalidVectorPath; });
    TestTrue(TEXT("Out-of-bounds vector fixture is present"),
        OutOfBoundsVector != OutOfBoundsInputs.end());
    if (OutOfBoundsVector == OutOfBoundsInputs.end()) return false;
    const FString OutOfBoundsJson = TEXT(
        "{\"schemaVersion\":1,\"features\":[{\"osmId\":\"way/2\",\"kind\":\"road\","
        "\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1.5001,\"northM\":1}]}]}");
    const FTCHARToUTF8 OutOfBoundsBytes(*OutOfBoundsJson);
    OutOfBoundsVector->Bytes.Reset();
    OutOfBoundsVector->Bytes.Append(reinterpret_cast<const uint8*>(OutOfBoundsBytes.Get()),
        OutOfBoundsBytes.Length());
    SiteContextManifest OutOfBoundsManifest = Input;
    const auto OutOfBoundsDescriptor = std::find_if(OutOfBoundsManifest.Assets.begin(),
        OutOfBoundsManifest.Assets.end(), [&](const SiteContextAsset& Asset)
        { return Asset.Path == "vectors/osm-enu-polylines.json"; });
    TestTrue(TEXT("Out-of-bounds vector descriptor is present"),
        OutOfBoundsDescriptor != OutOfBoundsManifest.Assets.end());
    if (OutOfBoundsDescriptor == OutOfBoundsManifest.Assets.end()) return false;
    OutOfBoundsDescriptor->Length = static_cast<std::uint64_t>(OutOfBoundsVector->Bytes.Num());
    OutOfBoundsDescriptor->Sha256 = TCHAR_TO_UTF8(*Sha256(OutOfBoundsVector->Bytes));
    const SiteContextAssetReader OutOfBoundsReader = [&](const FString& RelativePath,
        TArray<uint8>& OutBytes, FString& ReaderError)
    {
        const auto Found = std::find_if(OutOfBoundsInputs.begin(), OutOfBoundsInputs.end(),
            [&](const SiteContextFixtureAsset& Candidate)
            { return Candidate.Path == RelativePath; });
        if (Found == OutOfBoundsInputs.end())
        {
            ReaderError = TEXT("Fixture asset is missing.");
            return false;
        }
        OutBytes = Found->Bytes;
        return true;
    };
    const FString OutOfBoundsRoot = SiteContextTestRoot(TEXT("SiteContextOutOfBounds"));
    SiteContextStore OutOfBoundsStore(OutOfBoundsRoot);
    FString OutOfBoundsDirectory = TEXT("sentinel");
    SiteContextManifest RejectedOutOfBoundsManifest;
    TestFalse(TEXT("Vectors beyond the actual TerrainCore outer bounds are rejected at storage"),
        OutOfBoundsStore.WriteAndActivate(OutOfBoundsManifest, Core, OutOfBoundsReader,
            OutOfBoundsDirectory, RejectedOutOfBoundsManifest, Error));
    TestTrue(TEXT("Out-of-bounds vectors leave no activated SiteContext"),
        OutOfBoundsDirectory.IsEmpty() && RejectedOutOfBoundsManifest.ContentId.empty());
    IFileManager::Get().DeleteDirectory(*OutOfBoundsRoot, false, true);

    SiteContextManifest MissingTile = Input;
    MissingTile.ImageryTiles.pop_back();
    TestFalse(TEXT("A missing TerrainCore-aligned imagery tile is rejected"),
        ValidateSiteContextManifest(MissingTile, Core).Ok());
    SiteContextManifest MissingVectors = Input;
    MissingVectors.Assets.erase(std::remove_if(MissingVectors.Assets.begin(),
        MissingVectors.Assets.end(), [](const SiteContextAsset& Asset)
        { return Asset.Type == "osm-enu-polylines-json-v1"; }), MissingVectors.Assets.end());
    TestFalse(TEXT("The OSM vectors component is required"),
        ValidateSiteContextManifest(MissingVectors, Core).Ok());
    SiteContextManifest BadPath = Input;
    BadPath.Assets[0].Path = "../outside.jpg";
    TestFalse(TEXT("Traversal paths are rejected"), ValidateSiteContextManifest(BadPath, Core).Ok());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextVectorSchema2StoreTest,
    "MountainPlanner.M5.SiteContext.VectorSchema2PartsAndLegacy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextVectorSchema2StoreTest::RunTest(const FString& Parameters)
{
    using namespace SkiPreparation;
    const SkiDomain::TerrainCoreManifest Core = TestTerrainCore();
    const TArray<uint8> Jpeg = MakeJpegTile();
    TestTrue(TEXT("Test imagery encoder produced bytes"), !Jpeg.IsEmpty());
    if (Jpeg.IsEmpty()) return false;

    TArray<SiteContextFixtureAsset> Inputs;
    SiteContextManifest Manifest = TestSiteContextManifest(Core, Inputs, Jpeg);
    AddDeclaredAssets(Manifest, Inputs);
    Manifest.SchemaVersion = SiteContextSchema;
    TestFalse(TEXT("Schema 2 requires source lineage before accepting a new component"),
        ValidateSiteContextManifest(Manifest, Core).Ok());
    Manifest.VectorSource = TestVectorSourceLineage(Core);
    const FString Schema2Json = TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,"
        "\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]},"
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":1,"
        "\"points\":[{\"eastM\":0.5,\"northM\":0.5},{\"eastM\":1.5,\"northM\":2}]}]}");
    TestTrue(TEXT("Schema-2 vector fixture is installed into the manifest"),
        ReplaceVectorAssetJson(Manifest, Inputs, Schema2Json,
            UTF8_TO_TCHAR(SiteContextVectorEncodingV2)));
    TestTrue(TEXT("Schema-2 multipart manifest validates"),
        ValidateSiteContextManifest(Manifest, Core).Ok());
    const FString UnsignedManifest = SerializeSiteContextManifest(Manifest, false);
    SiteContextManifest ChangedLineage = Manifest;
    ChangedLineage.VectorSource.SourcePackageContentId = std::string(64, 'e');
    TestFalse(TEXT("Changing source-package lineage changes the canonical manifest bytes"),
        SerializeSiteContextManifest(ChangedLineage, false) == UnsignedManifest);
    SiteContextManifest NegativeZeroLineage = Manifest;
    SiteContextManifest PositiveZeroLineage = Manifest;
    NegativeZeroLineage.VectorSource.ExtentM.WestM = -0.0;
    PositiveZeroLineage.VectorSource.ExtentM.WestM = 0.0;
    TestEqual(TEXT("Lineage extent canonicalization normalizes negative zero"),
        SerializeSiteContextManifest(NegativeZeroLineage, false),
        SerializeSiteContextManifest(PositiveZeroLineage, false));
    SiteContextManifest MissingLineage = Manifest;
    MissingLineage.VectorSource.SourcePackageContentId.clear();
    TestFalse(TEXT("Partial schema-2 lineage is rejected"),
        ValidateSiteContextManifest(MissingLineage, Core).Ok());
    SiteContextManifest MismatchedExtent = Manifest;
    MismatchedExtent.VectorSource.ExtentM.EastM += 0.001;
    TestFalse(TEXT("Vector lineage extent must exactly bind to TerrainCore"),
        ValidateSiteContextManifest(MismatchedExtent, Core).Ok());
    SiteContextManifest InvalidTimestamp = Manifest;
    InvalidTimestamp.VectorSource.SourceTimestampUtc = "yesterday";
    TestFalse(TEXT("Malformed source timestamps are rejected"),
        ValidateSiteContextManifest(InvalidTimestamp, Core).Ok());

    const FString Root = SiteContextTestRoot(TEXT("SiteContextSchema2"));
    SiteContextStore Store(Root);
    const SiteContextAssetReader Reader = [&](const FString& RelativePath,
        TArray<uint8>& OutBytes, FString& ReaderError)
    {
        const auto Found = std::find_if(Inputs.begin(), Inputs.end(),
            [&](const SiteContextFixtureAsset& Candidate) { return Candidate.Path == RelativePath; });
        if (Found == Inputs.end())
        {
            ReaderError = TEXT("Fixture asset is missing.");
            return false;
        }
        OutBytes = Found->Bytes;
        return true;
    };
    FString Directory, Error;
    SiteContextManifest Written;
    TestTrue(TEXT("Schema-2 multipart vectors activate"),
        Store.WriteAndActivate(Manifest, Core, Reader, Directory, Written, Error));
    if (Directory.IsEmpty())
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }
    SiteContextPackageIndex Index;
    const FString ContentId = UTF8_TO_TCHAR(Written.ContentId.c_str());
    TestEqual(TEXT("Written new SiteContext uses schema 2"), Written.SchemaVersion,
        static_cast<std::uint32_t>(SiteContextSchema));
    TestEqual(TEXT("OSM source package identity survives storage"),
        Written.VectorSource.SourcePackageContentId, Manifest.VectorSource.SourcePackageContentId);
    TestEqual(TEXT("OSM source timestamps survive storage"),
        Written.VectorSource.SourceTimestampUtc, Manifest.VectorSource.SourceTimestampUtc);
    TestTrue(TEXT("OSM source binding and extent survive storage exactly"),
        Written.VectorSource.TerrainCoreId == Manifest.VectorSource.TerrainCoreId
        && Written.VectorSource.ExtentM.WestM == Core.OuterBounds.WestM
        && Written.VectorSource.ExtentM.SouthM == Core.OuterBounds.SouthM
        && Written.VectorSource.ExtentM.EastM == Core.OuterBounds.EastM
        && Written.VectorSource.ExtentM.NorthM == Core.OuterBounds.NorthM);
    TestTrue(TEXT("OSM provider, endpoint, retrieval time, license and attribution survive storage"),
        Written.VectorSource.Provider == Manifest.VectorSource.Provider
        && Written.VectorSource.Endpoint == Manifest.VectorSource.Endpoint
        && Written.VectorSource.RetrievedAtUtc == Manifest.VectorSource.RetrievedAtUtc
        && Written.VectorSource.License == Manifest.VectorSource.License
        && Written.VectorSource.Attribution == Manifest.VectorSource.Attribution
        && Written.VectorSource.AttributionUrl == Manifest.VectorSource.AttributionUrl);
    TestTrue(TEXT("Serialized schema-2 manifest requires its vectorSource object"),
        SerializeSiteContextManifest(Written, true).Contains(TEXT("vectorSource")));
    FString MissingLineageJson = SerializeSiteContextManifest(Written, true).Replace(
        TEXT("\"vectorSource\":"), TEXT("\"vectorSourceOmitted\":"),
        ESearchCase::CaseSensitive);
    SiteContextManifest MissingLineageParsed;
    TestFalse(TEXT("A schema-2 manifest cannot be parsed without vectorSource"),
        ParseSiteContextManifest(MissingLineageJson, Core, MissingLineageParsed, Error));
    FString PartialLineageJson = SerializeSiteContextManifest(Written, true);
    TestTrue(TEXT("Partial lineage fixture removes the required source timestamp"),
        EditVectorSourceJsonField(PartialLineageJson, TEXT("sourceTimestampUtc"), FString(), true));
    SiteContextManifest PartialLineageParsed;
    TestFalse(TEXT("A schema-2 manifest cannot omit one required source fact"),
        ParseSiteContextManifest(PartialLineageJson, Core, PartialLineageParsed, Error));
    FString TamperedLineageJson = SerializeSiteContextManifest(Written, true).Replace(
        TEXT("\"sourcePackageContentId\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\""),
        TEXT("\"sourcePackageContentId\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\""),
        ESearchCase::CaseSensitive);
    const std::string TamperedSourcePackageId(64, 'e');
    TestTrue(TEXT("Tamper fixture changes a valid source package identity"),
        EditVectorSourceJsonField(TamperedLineageJson, TEXT("sourcePackageContentId"),
            UTF8_TO_TCHAR(TamperedSourcePackageId.c_str()), false));
    SiteContextManifest TamperedLineageParsed;
    TestFalse(TEXT("Tampering with otherwise valid lineage fails the manifest content hash"),
        ParseSiteContextManifest(TamperedLineageJson, Core, TamperedLineageParsed, Error));
    TestTrue(TEXT("Schema-2 component reopens against its TerrainCore"),
        Store.Open(ContentId, Core, Index, Error));
    TArray<uint8> StoredVectorBytes;
    TestTrue(TEXT("Schema-2 vector component remains readable"),
        Store.ReadAsset(Index, TEXT("vectors/osm-enu-polylines.json"), StoredVectorBytes, Error));
    TestEqual(TEXT("Multipart JSON round-trips byte-for-byte through storage"),
        StoredVectorBytes, Inputs.Last().Bytes);
    const FString TamperedJson = Schema2Json.Replace(TEXT("\"partIndex\":1"),
        TEXT("\"partIndex\":2"), ESearchCase::CaseSensitive);
    const FTCHARToUTF8 TamperedEncoded(*TamperedJson);
    TArray<uint8> TamperedBytes;
    TamperedBytes.Append(reinterpret_cast<const uint8*>(TamperedEncoded.Get()), TamperedEncoded.Length());
    const FString VectorFile = FPaths::Combine(Directory, TEXT("vectors/osm-enu-polylines.json"));
    TestTrue(TEXT("Tampered multipart asset writes"), FFileHelper::SaveArrayToFile(TamperedBytes, *VectorFile));
    TestFalse(TEXT("Changing a part index after installation fails the asset hash check"),
        Store.Verify(Index, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);

    const auto RejectVectorJson = [&](const FString& Label, const FString& Json,
        const FString& Encoding)
    {
        TArray<SiteContextFixtureAsset> InvalidInputs = Inputs;
        SiteContextManifest InvalidManifest = Manifest;
        if (!ReplaceVectorAssetJson(InvalidManifest, InvalidInputs, Json, Encoding))
        {
            TestFalse(*FString::Printf(TEXT("%s fixture is structurally present"), *Label), true);
            return;
        }
        const FString InvalidRoot = SiteContextTestRoot(TEXT("SiteContextInvalidVectorParts"));
        SiteContextStore InvalidStore(InvalidRoot);
        const SiteContextAssetReader InvalidReader = [&](const FString& RelativePath,
            TArray<uint8>& OutBytes, FString& ReaderError)
        {
            const auto Found = std::find_if(InvalidInputs.begin(), InvalidInputs.end(),
                [&](const SiteContextFixtureAsset& Candidate)
                { return Candidate.Path == RelativePath; });
            if (Found == InvalidInputs.end())
            {
                ReaderError = TEXT("Fixture asset is missing.");
                return false;
            }
            OutBytes = Found->Bytes;
            return true;
        };
        FString InvalidDirectory = TEXT("sentinel");
        SiteContextManifest Rejected;
        TestFalse(*FString::Printf(TEXT("%s is rejected before activation"), *Label),
            InvalidStore.WriteAndActivate(InvalidManifest, Core, InvalidReader,
                InvalidDirectory, Rejected, Error));
        TestTrue(*FString::Printf(TEXT("%s leaves no activated package"), *Label),
            InvalidDirectory.IsEmpty() && Rejected.ContentId.empty());
        IFileManager::Get().DeleteDirectory(*InvalidRoot, false, true);
    };

    const FString DuplicatePartJson = TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]},"
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]}]}");
    const FString GapPartJson = TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]},"
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":2,\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]}]}");
    const FString UnsortedPartJson = TEXT(
        "{\"schemaVersion\":2,\"features\":["
        "{\"osmId\":\"way/2\",\"kind\":\"road\",\"partIndex\":0,\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]},"
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"partIndex\":0,\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]}]}");
    const FString DuplicateLegacyJson = TEXT(
        "{\"schemaVersion\":1,\"features\":["
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]},"
        "{\"osmId\":\"way/1\",\"kind\":\"road\",\"points\":[{\"eastM\":0,\"northM\":0},{\"eastM\":1,\"northM\":1}]}]}");
    RejectVectorJson(TEXT("Duplicate schema-2 part index"), DuplicatePartJson,
        UTF8_TO_TCHAR(SiteContextVectorEncodingV2));
    RejectVectorJson(TEXT("Gap in schema-2 part indices"), GapPartJson,
        UTF8_TO_TCHAR(SiteContextVectorEncodingV2));
    RejectVectorJson(TEXT("Unsorted schema-2 element IDs"), UnsortedPartJson,
        UTF8_TO_TCHAR(SiteContextVectorEncodingV2));
    RejectVectorJson(TEXT("Duplicate schema-1 implied part"), DuplicateLegacyJson,
        UTF8_TO_TCHAR(SiteContextVectorEncodingV1));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCompositeInstallReceiptGateAndLegacyReadTest,
    "MountainPlanner.M5.SiteContext.CompositeReceiptGateAndLegacyRead",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCompositeInstallReceiptGateAndLegacyReadTest::RunTest(const FString& Parameters)
{
    using namespace SkiPreparation;
    CompositeInstallReceipt Receipt = CompleteReceipt();
    TestTrue(TEXT("Schema-3 complete receipt validates"),
        ValidateCompositeInstallReceipt(Receipt).Ok());
    const FString Serialized = SerializeCompositeInstallReceipt(Receipt, true);
    CompositeInstallReceipt Parsed;
    FString Error;
    TestTrue(TEXT("Schema-3 receipt round-trips canonically"),
        ParseCompositeInstallReceipt(Serialized, Parsed, Error));
    TestEqual(TEXT("Composite content ID survives its read"), Parsed.ContentId, Receipt.ContentId);
    TestEqual(TEXT("Source facts round-trip with the receipt"), Parsed.Quality.Sources.size(),
        Receipt.Quality.Sources.size());
    if (!Parsed.Quality.Sources.empty())
    {
        const SkiDomain::TerrainQualitySourceFacts& ParsedSource = Parsed.Quality.Sources.front();
        TestEqual(TEXT("Source identity survives round-trip"), ParsedSource.SourceId,
            Receipt.Quality.Sources.front().SourceId);
        TestTrue(TEXT("Unknown QL status is explicit"), ParsedSource.QualityLevelUnknown);
        TestTrue(TEXT("Unknown acquisition dates status is explicit"),
            ParsedSource.AcquisitionDateRangeUnknown);
        TestTrue(TEXT("Unknown RMSE status is explicit"), ParsedSource.VerticalRmseUnknown);
        TestTrue(TEXT("Unknown datum status is explicit"), ParsedSource.DatumUnknown);
        TestFalse(TEXT("Known sample fraction is not marked unknown"),
            ParsedSource.SampleFractionUnknown);
        TestEqual(TEXT("Sample fraction survives round-trip"), ParsedSource.SampleFraction, 1.0);
    }
    const FString TamperedSourceJson = Serialized.Replace(TEXT("s1m-fixture-tile"),
        TEXT("s1m-tampered-tile"), ESearchCase::CaseSensitive);
    TestFalse(TEXT("Source fact tampering invalidates the committed receipt hash"),
        TamperedSourceJson == Serialized || ParseCompositeInstallReceipt(TamperedSourceJson, Parsed, Error));
    FString MissingStateJson = Serialized;
    const FString MissingStateKey = TEXT("\"sampleFractionUnknown\"");
    const int32 MissingStateFieldStart = MissingStateJson.Find(*MissingStateKey,
        ESearchCase::CaseSensitive);
    const int32 MissingStateValueStart = MissingStateFieldStart == INDEX_NONE ? INDEX_NONE
        : MissingStateJson.Find(TEXT("false"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, MissingStateFieldStart + MissingStateKey.Len());
    TestTrue(TEXT("Serialized source fixture contains its known-fraction state"),
        MissingStateFieldStart != INDEX_NONE && MissingStateValueStart != INDEX_NONE);
    if (MissingStateFieldStart != INDEX_NONE && MissingStateValueStart != INDEX_NONE)
    {
        const int32 MissingStateFieldEnd = MissingStateValueStart + 5;
        const int32 PreviousComma = MissingStateJson.Find(TEXT(","), ESearchCase::CaseSensitive,
            ESearchDir::FromEnd, MissingStateFieldStart);
        if (PreviousComma != INDEX_NONE)
            MissingStateJson.RemoveAt(PreviousComma, MissingStateFieldEnd - PreviousComma);
    }
    TestFalse(TEXT("A modern source fact cannot omit its explicit unknown state"),
        MissingStateFieldStart == INDEX_NONE || MissingStateValueStart == INDEX_NONE
            || MissingStateJson == Serialized
            || ParseCompositeInstallReceipt(MissingStateJson, Parsed, Error));
    TestFalse(TEXT("Malformed receipt JSON is rejected"),
        ParseCompositeInstallReceipt(TEXT("{}"), Parsed, Error));

    CompositeInstallReceipt LegacyComposite = Receipt;
    LegacyComposite.Quality.Sources.clear();
    const FString LegacyQualityId = ComputeTerrainQualityReportId(
        LegacyComposite.ProvenanceCounts, LegacyComposite.Quality);
    const std::string LegacyQualityIdUtf8 = TCHAR_TO_UTF8(*LegacyQualityId);
    for (CompositeInstallComponent& Component : LegacyComposite.Components)
    {
        if (Component.Kind == CompositeInstallComponentKind::QualityReport)
        {
            Component.ContentId = LegacyQualityIdUtf8;
            Component.ManifestSha256 = LegacyQualityIdUtf8;
        }
    }
    LegacyComposite.ContentId = TCHAR_TO_UTF8(*ComputeCompositeInstallReceiptId(LegacyComposite));
    const FString LegacyCompositeJson = SerializeCompositeInstallReceipt(LegacyComposite, true);
    TestFalse(TEXT("Legacy schema-3 quality payload keeps its old field shape"),
        LegacyCompositeJson.Contains(TEXT("sourceFacts")));
    CompositeInstallReceipt ParsedLegacyComposite;
    TestTrue(TEXT("Schema-3 receipts with legacy aggregate-only quality remain readable"),
        ParseCompositeInstallReceipt(LegacyCompositeJson, ParsedLegacyComposite, Error));
    TestTrue(TEXT("Legacy receipts do not invent per-source facts"),
        ParsedLegacyComposite.Quality.Sources.empty());

    int32 VerifiedCount = 0;
    CompositeComponentVerifier AcceptComponents = [&](const CompositeInstallReceipt&,
        const CompositeInstallComponent& Component, FString&)
    {
        ++VerifiedCount;
        return Component.Status == CompositeInstallComponentStatus::Verified
            && Component.ManifestSha256.size() == 64;
    };
    TestTrue(TEXT("Activation requires all storage adapters to verify"),
        VerifyCompositeInstallReceiptForActivation(Receipt, AcceptComponents, Error));
    TestEqual(TEXT("Only material components are delegated to storage adapters"), VerifiedCount, 3);

    CompositeInstallReceipt Partial = Receipt;
    Partial.Components.erase(std::remove_if(Partial.Components.begin(), Partial.Components.end(),
        [](const CompositeInstallComponent& Component)
        { return Component.Kind == CompositeInstallComponentKind::SiteContext; }),
        Partial.Components.end());
    Partial.ContentId = TCHAR_TO_UTF8(*ComputeCompositeInstallReceiptId(Partial));
    TestFalse(TEXT("Interrupted partial install has no playable receipt"),
        ValidateCompositeInstallReceipt(Partial).Ok());

    CompositeInstallReceipt Staged = Receipt;
    Staged.Components[2].Status = CompositeInstallComponentStatus::Staged;
    Staged.ContentId = TCHAR_TO_UTF8(*ComputeCompositeInstallReceiptId(Staged));
    TestFalse(TEXT("A staged but unverified SiteContext cannot activate"),
        VerifyCompositeInstallReceiptForActivation(Staged, AcceptComponents, Error));

    CompositeInstallReceipt HashMismatch = Receipt;
    HashMismatch.Components[1].ManifestSha256 = std::string(64, '0');
    HashMismatch.ContentId = TCHAR_TO_UTF8(*ComputeCompositeInstallReceiptId(HashMismatch));
    CompositeComponentVerifier RejectCover = [](const CompositeInstallReceipt&,
        const CompositeInstallComponent& Component, FString& ComponentError)
    {
        if (Component.Kind == CompositeInstallComponentKind::CoverEcology)
        {
            ComponentError = TEXT("Cover manifest hash mismatch.");
            return false;
        }
        return true;
    };
    TestFalse(TEXT("Component identity/hash mismatch prevents activation"),
        VerifyCompositeInstallReceiptForActivation(HashMismatch, RejectCover, Error));
    TestTrue(TEXT("Adapter mismatch reason is surfaced"), Error.Contains(TEXT("hash mismatch")));

    CompositeInstallReceipt BadQuality = Receipt;
    BadQuality.Quality.NoDataFraction = 0.25;
    BadQuality.ContentId = TCHAR_TO_UTF8(*ComputeCompositeInstallReceiptId(BadQuality));
    TestFalse(TEXT("Quality values inconsistent with provenance counts are rejected"),
        ValidateCompositeInstallReceipt(BadQuality).Ok());

    CompositeInstallReceipt WrongIdentity = Receipt;
    WrongIdentity.ContentId = std::string(64, '0');
    TestFalse(TEXT("A mismatched composite content ID is rejected"),
        ValidateCompositeInstallReceipt(WrongIdentity).Ok());
    CompositeInstallReceipt WrongSchema = Receipt;
    WrongSchema.SchemaVersion = 2;
    TestFalse(TEXT("Schema 2 cannot be misread as the new complete receipt"),
        ValidateCompositeInstallReceipt(WrongSchema).Ok());

    SkiDomain::InstalledTerrainReceipt LegacyReceipt;
    LegacyReceipt.SchemaVersion = 2;
    LegacyReceipt.ContentId = std::string(64, 'a');
    LegacyReceipt.GeneratorVersion = "legacy-installed-receipt-v2";
    LegacyReceipt.TerrainCoreId = std::string(64, 'b');
    LegacyReceipt.SurroundTerrainCoreId = std::string(64, 'c');
    LegacyReceipt.CoverEcologyId = std::string(64, 'd');
    SkiDomain::InstalledTerrainReceipt ParsedLegacyReceipt;
    TestTrue(TEXT("Existing installed receipt schema 2 remains readable"),
        ParseInstalledTerrainReceipt(SerializeInstalledTerrainReceipt(LegacyReceipt, true),
            ParsedLegacyReceipt, Error));
    TestEqual(TEXT("Legacy receipt schema is preserved"), ParsedLegacyReceipt.SchemaVersion, uint32(2));

    SkiDomain::TerrainManifest LegacyTerrain;
    LegacyTerrain.SchemaVersion = 1;
    LegacyTerrain.ContentId = std::string(64, 'e');
    LegacyTerrain.Name = "legacy terrain fixture";
    LegacyTerrain.Source = "fixture";
    LegacyTerrain.RequestedBounds = {-72.0, 43.0, -71.0, 44.0};
    LegacyTerrain.ActualBounds = LegacyTerrain.RequestedBounds;
    LegacyTerrain.LocalOrigin = {43.5, -71.5, 100.0};
    LegacyTerrain.HeightWidth = 2;
    LegacyTerrain.HeightHeight = 2;
    LegacyTerrain.EastSpacingM = 1.0;
    LegacyTerrain.NorthSpacingM = 1.0;
    LegacyTerrain.Assets.push_back({"elevation.f32le", "heightfield-f32le",
        std::string(64, 'f'), 16, true, {}, "fixture", "CC0"});
    SkiDomain::TerrainManifest ParsedLegacyTerrain;
    TestTrue(TEXT("Existing terrain package schema 1 remains readable"),
        ParseManifest(SerializeManifest(LegacyTerrain, true), ParsedLegacyTerrain, Error));
    TestEqual(TEXT("Legacy terrain schema is preserved"), ParsedLegacyTerrain.SchemaVersion, uint32(1));
    return true;
}

#endif
