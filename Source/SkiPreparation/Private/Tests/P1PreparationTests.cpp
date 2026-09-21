#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiPreparation/FixtureTerrainProvider.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/NativeTerrainProvider.h"
#include "SkiPreparation/SelectorProtocol.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "Misc/FileHelper.h"
#include "tiffio.h"

namespace
{
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;
constexpr uint32 GeoKeyDirectoryTag = 34735;

bool WriteFloatTiff(const FString& Path, const uint32 Width, const uint32 Height,
    const bool Tiled, const uint16 Compression, const uint16 Orientation,
    const char* NoData = "-9999", const bool IncludeGeoreference = true, const uint16 Epsg = 4326)
{
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    static const TIFFFieldInfo GeoFields[] = {
        {ModelPixelScaleTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, true, true,
            const_cast<char*>("ModelPixelScaleTag")},
        {ModelTiepointTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, true, true,
            const_cast<char*>("ModelTiepointTag")},
        {GeoKeyDirectoryTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT, FIELD_CUSTOM, true, true,
            const_cast<char*>("GeoKeyDirectoryTag")},
        {TIFFTAG_GDAL_NODATA, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, true, true,
            const_cast<char*>("GDALNoDataValue")},
    };
    for (const TIFFFieldInfo& Field : GeoFields)
        if (!TIFFFindField(Image, Field.field_tag, TIFF_ANY)) TIFFMergeFieldInfo(Image, &Field, 1);
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, Width);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, Height);
    TIFFSetField(Image, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(Image, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(Image, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(Image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(Image, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(Image, TIFFTAG_ORIENTATION, Orientation);
    TIFFSetField(Image, TIFFTAG_COMPRESSION, Compression);
    if (IncludeGeoreference)
    {
        double Scale[]{0.001, 0.001, 0.0};
        double Tie[]{0.0, 0.0, 0.0, -121.5, 47.0, 0.0};
        uint16 Keys[]{1, 1, 0, 2, 1024, 0, 1, 2, 2048, 0, 1, Epsg};
        TIFFSetField(Image, ModelPixelScaleTag, 3U, Scale);
        TIFFSetField(Image, ModelTiepointTag, 6U, Tie);
        TIFFSetField(Image, GeoKeyDirectoryTag, 12U, Keys);
    }
    if (NoData) TIFFSetField(Image, TIFFTAG_GDAL_NODATA,
        static_cast<uint32>(FCStringAnsi::Strlen(NoData) + 1), NoData);
    bool Ok = true;
    if (Tiled)
    {
        constexpr uint32 TileWidth = 16, TileHeight = 16;
        TIFFSetField(Image, TIFFTAG_TILEWIDTH, TileWidth);
        TIFFSetField(Image, TIFFTAG_TILELENGTH, TileHeight);
        TArray<float> Tile;
        Tile.SetNumZeroed(TileWidth * TileHeight);
        for (uint32 Y = 0; Ok && Y < Height; Y += TileHeight)
        {
            for (uint32 X = 0; X < Width; X += TileWidth)
            {
                for (uint32 LocalY = 0; LocalY < TileHeight; ++LocalY)
                    for (uint32 LocalX = 0; LocalX < TileWidth; ++LocalX)
                        Tile[LocalY * TileWidth + LocalX] = static_cast<float>((Y + LocalY) * 1000 + X + LocalX);
                const uint32 Index = TIFFComputeTile(Image, X, Y, 0, 0);
                Ok = TIFFWriteEncodedTile(Image, Index, Tile.GetData(), Tile.Num() * sizeof(float)) >= 0;
            }
        }
    }
    else
    {
        constexpr uint32 RowsPerStrip = 3;
        TIFFSetField(Image, TIFFTAG_ROWSPERSTRIP, RowsPerStrip);
        TArray<float> Strip;
        Strip.SetNumZeroed(Width * RowsPerStrip);
        const uint32 StripCount = (Height + RowsPerStrip - 1) / RowsPerStrip;
        for (uint32 Index = 0; Ok && Index < StripCount; ++Index)
        {
            const uint32 FirstRow = Index * RowsPerStrip;
            const uint32 Rows = FMath::Min(RowsPerStrip, Height - FirstRow);
            for (uint32 Row = 0; Row < Rows; ++Row)
                for (uint32 Column = 0; Column < Width; ++Column)
                    Strip[Row * Width + Column] = static_cast<float>((FirstRow + Row) * 1000 + Column);
            Ok = TIFFWriteEncodedStrip(Image, Index, Strip.GetData(), Rows * Width * sizeof(float)) >= 0;
        }
    }
    TIFFClose(Image);
    return Ok;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1PreparationContractTest,
    "MountainPlanner.P1.Preparation.PackageAndProtocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1PreparationContractTest::RunTest(const FString&)
{
    const FString Token = TEXT("13a15f75-5ba7-4ac0-bd1c-fbe114a6843a");
    const FString Valid = FString::Printf(TEXT("{\"token\":\"%s\",\"generation\":7,\"name\":\"Crystal\",\"profile\":\"standard\",\"west\":-121.49,\"south\":46.92,\"east\":-121.46,\"north\":46.95}"), *Token);
    SkiPreparation::Request Request;
    FString Error;
    TestTrue(TEXT("Tokened selector request validates"),
        SkiPreparation::ValidateSelectorMessage(Valid, Token, 7, Request, Error));
    TestFalse(TEXT("Stale selector generation rejected"),
        SkiPreparation::ValidateSelectorMessage(Valid, Token, 8, Request, Error));
    TestFalse(TEXT("Selector rejects added capability fields"),
        SkiPreparation::ValidateSelectorMessage(Valid.LeftChop(1) + TEXT(",\"path\":\"C:/escape\"}"), Token, 7, Request, Error));

    Request = {};
    Request.Name = TEXT("Crystal synthetic");
    Request.Bounds = {-121.49, 46.92, -121.46, 46.95};
    Request.Profile = SkiPreparation::SourceProfile::Standard;
    Request.SessionGeneration = 1;
    Request.OperationGeneration = 1;
    const FString Root = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("P1Tests"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Root, true);
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::FixtureTerrainProvider Provider(Root);
    SkiPreparation::Result Result = Provider.Prepare(Request, Cancellation, {});
    TestTrue(TEXT("Fixture writes, verifies, and activates"), Result.Ok);
    TestEqual(TEXT("Fixture manifest includes required and explicit optional assets"),
        static_cast<int32>(Result.Manifest.Assets.size()), 7);
    TestEqual(TEXT("Verified cover grid is returned for runtime presentation"),
        Result.Cover.Num(), static_cast<int32>(Result.Manifest.CoverWidth * Result.Manifest.CoverHeight));

    SkiApplication::TerrainSession Session;
    std::vector<std::uint8_t> Cover(Result.Cover.GetData(), Result.Cover.GetData() + Result.Cover.Num());
    TestTrue(TEXT("Verified package installs"), Session.Install(std::move(Result.Heightfield),
        Result.Manifest, std::move(Cover)));
    const SkiApplication::TerrainSnapshot Before = Session.Snapshot();
    TestTrue(TEXT("Installed snapshot exposes immutable cover"), Before.Cover != nullptr);
    SkiDomain::MutationBounds Bounds;
    TestFalse(TEXT("Stale edit is rejected"), Session.ApplyScratchMutation(99, 0, 0, 50, 2, Bounds));
    TestTrue(TEXT("Expected revision edit succeeds"), Session.ApplyScratchMutation(
        Before.Readiness.Canonical, 0, 0, 50, 2, Bounds));
    const SkiApplication::TerrainSnapshot After = Session.Snapshot();
    TestTrue(TEXT("Edit advances canonical revision"), After.Readiness.Canonical > Before.Readiness.Canonical);
    TestFalse(TEXT("Readiness is withheld until render/query acknowledge"), After.Readiness.IsReady());
    TestTrue(TEXT("Render acknowledges edited revision"), Session.AcknowledgeRender(After.Readiness.Canonical));
    TestTrue(TEXT("Query acknowledges edited revision"), Session.AcknowledgeQuery(After.Readiness.Canonical));
    TestTrue(TEXT("Edited terrain becomes ready"), Session.Snapshot().Readiness.IsReady());

    const FString Resolved = FPaths::ConvertRelativePathToFull(Root);
    if (Resolved.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())))
    {
        IFileManager::Get().DeleteDirectory(*Resolved, false, true);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1GeoTiffDecoderTest,
    "MountainPlanner.P1.Preparation.GeoTiff.DecodeOrganizations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1GeoTiffDecoderTest::RunTest(const FString&)
{
    const FString Root = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("P1TiffTests"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Root, true);
    const SkiDomain::GeographicBounds Requested{-121.5, 46.9, -121.4, 47.0};
    struct Case { bool Tiled; uint16 Compression; uint16 Orientation; const TCHAR* Name; };
    const Case Cases[] = {
        {false, COMPRESSION_NONE, ORIENTATION_TOPLEFT, TEXT("stripped-none")},
        {false, COMPRESSION_ADOBE_DEFLATE, ORIENTATION_BOTLEFT, TEXT("stripped-deflate-bottom")},
        {true, COMPRESSION_NONE, ORIENTATION_TOPLEFT, TEXT("tiled-edge")},
        {true, COMPRESSION_ADOBE_DEFLATE, ORIENTATION_TOPLEFT, TEXT("tiled-deflate")},
    };
    for (const Case& Value : Cases)
    {
        const FString Source = FPaths::Combine(Root, FString(Value.Name) + TEXT("-source.tif"));
        TestTrue(FString::Printf(TEXT("%s fixture writes"), Value.Name),
            WriteFloatTiff(Source, 19, 18, Value.Tiled, Value.Compression, Value.Orientation));
        TArray<uint8> Bytes;
        TestTrue(FString::Printf(TEXT("%s fixture loads"), Value.Name), FFileHelper::LoadFileToArray(Bytes, *Source));
        SkiPreparation::DecodedElevationRaster Raster;
        SkiPreparation::ProviderFailure Failure;
        TestTrue(FString::Printf(TEXT("%s decodes"), Value.Name),
            SkiPreparation::DecodeElevationGeoTiff(Bytes, Requested,
                SkiPreparation::ProviderProduct::CoreElevation, Raster, Failure));
        TestEqual(FString::Printf(TEXT("%s width"), Value.Name), Raster.SourceWidth, 19U);
        TestEqual(FString::Printf(TEXT("%s height"), Value.Name), Raster.SourceHeight, 18U);
        const float ExpectedFirst = Value.Orientation == ORIENTATION_TOPLEFT ? 0.0F : 17000.0F;
        TestEqual(FString::Printf(TEXT("%s north row normalized"), Value.Name),
            Raster.Heightfield.Samples.front(), ExpectedFirst);
        TestTrue(FString::Printf(TEXT("%s sample spacing finite"), Value.Name),
            Raster.Heightfield.EastSpacingM > 0.0 && Raster.Heightfield.NorthSpacingM > 0.0);
    }
    TArray<uint8> Invalid{'{', '"', 'e', 'r', 'r', 'o', 'r'};
    SkiPreparation::DecodedElevationRaster Raster;
    SkiPreparation::ProviderFailure Failure;
    TestFalse(TEXT("JSON response rejected before TIFF allocation"),
        SkiPreparation::DecodeElevationGeoTiff(Invalid, Requested,
            SkiPreparation::ProviderProduct::SurroundingElevation, Raster, Failure));
    TestEqual(TEXT("Invalid signature has stable code"), Failure.Code, FString(TEXT("TIFF_INVALID_SIGNATURE")));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1GeoTiffLiveNoDataRegressionTest,
    "MountainPlanner.P1.Preparation.GeoTiff.LiveShapedNoData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1GeoTiffLiveNoDataRegressionTest::RunTest(const FString&)
{
    FString Encoded;
    const FString Fixture = FPaths::Combine(FPaths::ProjectContentDir(),
        TEXT("P1Fixtures/usgs-tiled-nodata-synthetic.tif.base64"));
    TestTrue(TEXT("Immutable encoded TIFF fixture loads"), FFileHelper::LoadFileToString(Encoded, *Fixture));
    TArray<uint8> Bytes;
    TestTrue(TEXT("Immutable encoded TIFF fixture decodes"), FBase64::Decode(Encoded.TrimStartAndEnd(), Bytes));
    TestEqual(TEXT("Fixture SHA-256 is immutable"), SkiPreparation::Sha256(Bytes),
        FString(TEXT("4614b0cec77c843a6b00ec7d0a4b3b90511031ee9eb73f185c5656088fe599bf")));
    SkiPreparation::DecodedElevationRaster Raster;
    SkiPreparation::ProviderFailure Failure;
    const SkiDomain::GeographicBounds Requested{-121.5, 46.982, -121.481, 47.0};
    TestTrue(TEXT("Live-shaped tiled nodata TIFF decodes in memory"),
        SkiPreparation::DecodeElevationGeoTiff(Bytes, Requested,
            SkiPreparation::ProviderProduct::CoreElevation, Raster, Failure));
    TestEqual(TEXT("Fixture width"), Raster.SourceWidth, 19U);
    TestEqual(TEXT("Fixture height"), Raster.SourceHeight, 18U);
    TestEqual(TEXT("Nodata metadata"), Raster.NoDataValue, -9999.0);
    TestEqual(TEXT("Storage organization"), static_cast<uint8>(Raster.Storage),
        static_cast<uint8>(SkiPreparation::TiffStorageOrganization::Tiled));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1GeoTiffMetadataValidationTest,
    "MountainPlanner.P1.Preparation.GeoTiff.MetadataValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1GeoTiffMetadataValidationTest::RunTest(const FString&)
{
    const FString Root = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("P1TiffMetadataTests"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Root, true);
    const SkiDomain::GeographicBounds Requested{-121.5, 46.982, -121.481, 47.0};
    auto DecodeFixture = [&](const TCHAR* Name, const char* NoData, const bool IncludeGeo, const uint16 Epsg,
        FString& OutCode)
    {
        const FString Path = FPaths::Combine(Root, FString(Name) + TEXT(".tif"));
        if (!WriteFloatTiff(Path, 19, 18, true, COMPRESSION_NONE, ORIENTATION_TOPLEFT,
                NoData, IncludeGeo, Epsg)) return false;
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
        SkiPreparation::DecodedElevationRaster Raster;
        SkiPreparation::ProviderFailure Failure;
        const bool Ok = SkiPreparation::DecodeElevationGeoTiff(Bytes, Requested,
            SkiPreparation::ProviderProduct::CoreElevation, Raster, Failure);
        OutCode = Failure.Code;
        return Ok;
    };
    FString Code;
    TestTrue(TEXT("Absent nodata retains explicit fallback"), DecodeFixture(TEXT("nodata-absent"), nullptr, true, 4326, Code));
    TestFalse(TEXT("Trailing nodata text is rejected"), DecodeFixture(TEXT("nodata-trailing"), "-9999x", true, 4326, Code));
    TestEqual(TEXT("Malformed nodata stable code"), Code, FString(TEXT("TIFF_METADATA_INVALID")));
    TestFalse(TEXT("Nonfinite nodata is rejected"), DecodeFixture(TEXT("nodata-nan"), "nan", true, 4326, Code));
    TestFalse(TEXT("Missing georeference is rejected"), DecodeFixture(TEXT("georef-missing"), "-9999", false, 4326, Code));
    TestEqual(TEXT("Missing georeference stable code"), Code, FString(TEXT("TIFF_GEOREFERENCE_MISSING")));
    TestFalse(TEXT("Conflicting CRS is rejected"), DecodeFixture(TEXT("crs-conflict"), "-9999", true, 3857, Code));
    TestEqual(TEXT("Conflicting CRS stable code"), Code, FString(TEXT("TIFF_CRS_UNSUPPORTED")));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1ProviderDiagnosticsTest,
    "MountainPlanner.P1.Preparation.ProviderDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1ProviderDiagnosticsTest::RunTest(const FString&)
{
    const FString Root = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("P1ProviderDiagnostics"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Diagnostics = FPaths::Combine(Root, TEXT("TerrainDiagnostics"));
    IFileManager::Get().MakeDirectory(*Diagnostics, true);
    const FString Journal = FPaths::Combine(Diagnostics, TEXT("current-operation.json"));
    TestTrue(TEXT("Stale journal fixture writes"), FFileHelper::SaveStringToFile(
        TEXT("{\"state\":\"Decoding\",\"sessionGeneration\":\"2\",\"operationGeneration\":\"7\"}"), *Journal));

    SkiPreparation::NativeTerrainProvider Provider(Root);
    TestFalse(TEXT("Stale operation journal is consumed"), FPaths::FileExists(Journal));
    SkiPreparation::Request InvalidRequest;
    InvalidRequest.Name = TEXT("Invalid diagnostic request");
    InvalidRequest.SessionGeneration = 2;
    InvalidRequest.OperationGeneration = 8;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    const SkiPreparation::Result Result = Provider.Prepare(InvalidRequest, Cancellation, {});
    TestFalse(TEXT("Invalid provider request fails"), Result.Ok);
    TestTrue(TEXT("Invalid provider request has structured failure"), Result.Failure.IsSet());
    TestEqual(TEXT("Validation failure has stable code"), Result.Failure->Code, FString(TEXT("REQUEST_INVALID")));
    TestFalse(TEXT("Terminal failure clears operation journal"), FPaths::FileExists(Journal));
    FString Events;
    TestTrue(TEXT("Shipping-safe preparation event log exists"), FFileHelper::LoadFileToString(
        Events, *FPaths::Combine(Diagnostics, TEXT("preparation.jsonl"))));
    TestTrue(TEXT("Interrupted operation is recorded conservatively"), Events.Contains(TEXT("PREPARATION_INTERRUPTED")));
    TestTrue(TEXT("Validation failure is recorded"), Events.Contains(TEXT("REQUEST_INVALID")));
    TestFalse(TEXT("Diagnostic event log contains no provider URL"), Events.Contains(TEXT("https://")));
    TArray<FString> Receipts;
    IFileManager::Get().FindFiles(Receipts, *FPaths::Combine(Diagnostics, TEXT("*.json")), true, false);
    TestEqual(TEXT("Exactly one terminal diagnostic receipt remains"), Receipts.Num(), 1);
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif
