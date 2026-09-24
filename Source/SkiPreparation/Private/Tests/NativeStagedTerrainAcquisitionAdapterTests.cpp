#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/NativeStagedTerrainAcquisitionAdapter.h"
#include "tiffio.h"

namespace
{
using namespace SkiPreparation;

constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;

SkiDomain::GeographicBounds SupportedSite()
{
    return {-122.51, 47.00, -122.49, 47.02};
}

FStagedTerrainAcquisitionRequest MakeRequest(const SkiDomain::GeographicBounds& Bounds = SupportedSite())
{
    FStagedTerrainAcquisitionRequest Request;
    Request.JobId = TEXT("native-staged-adapter-fixture");
    Request.Bounds = Bounds;
    Request.Width = 2;
    Request.Height = 2;
    return Request;
}

bool WriteCogDirectory(TIFF* Image, const uint32 Width, const bool bOverview)
{
    constexpr uint32 TileSide = 512;
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, Width);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, Width);
    TIFFSetField(Image, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(Image, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(Image, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(Image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(Image, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(Image, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(Image, TIFFTAG_TILEWIDTH, TileSide);
    TIFFSetField(Image, TIFFTAG_TILELENGTH, TileSide);
    TIFFSetField(Image, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(Image, TIFFTAG_PREDICTOR, 3);
    if (bOverview) TIFFSetField(Image, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
    if (!bOverview)
    {
        const double Scale[] = {1.0, 1.0, 0.0};
        const double Tie[] = {0.0, 0.0, 0.0, 500000.0, 5000000.0, 0.0};
        TIFFSetField(Image, ModelPixelScaleTag, 3U, const_cast<double*>(Scale));
        TIFFSetField(Image, ModelTiepointTag, 6U, const_cast<double*>(Tie));
        const uint16 Keys[] = {
            1, 1, 0, 5,
            1024, 0, 1, 1,
            1025, 0, 1, 1,
            3072, 0, 1, 6350,
            4096, 0, 1, 5703,
            4098, 0, 1, 5103,
        };
        TIFFSetField(Image, GeoKeyDirectoryTag, static_cast<uint32>(UE_ARRAY_COUNT(Keys)), Keys);
        TIFFSetField(Image, TIFFTAG_GDAL_NODATA, 8U, const_cast<char*>("-999999"));
    }

    TArray<float> Tile;
    Tile.SetNumUninitialized(TileSide * TileSide);
    for (int32 Index = 0; Index < Tile.Num(); ++Index)
        Tile[Index] = 1100.0F + static_cast<float>(Index % 13);
    return TIFFWriteEncodedTile(Image, TIFFComputeTile(Image, 0, 0, 0, 0), Tile.GetData(),
        static_cast<tmsize_t>(Tile.Num() * sizeof(float))) >= 0 && TIFFWriteDirectory(Image) != 0;
}

bool MakeCogFixture(TArray<uint8>& OutBytes)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NativeStagedAdapterFixtures"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Path = FPaths::Combine(Directory, TEXT("s1m.tif"));
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    const bool bBase = WriteCogDirectory(Image, 512, false);
    const bool bOverview = bBase && WriteCogDirectory(Image, 256, true);
    TIFFClose(Image);
    const bool bRead = bBase && bOverview && FFileHelper::LoadFileToArray(OutBytes, *Path);
    IFileManager::Get().Delete(*Path);
    return bRead && !OutBytes.IsEmpty();
}

FString CogUrl()
{
    return TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/n26e19/n2620e1940/S1M_s1m-fixture.tif");
}

FString CatalogItem(const uint64 ObjectBytes, const TCHAR* Datum)
{
    return FString::Printf(
        TEXT("{\"sourceId\":\"s1m-fixture\",\"title\":\"S1M fixture\",\"downloadURL\":\"%s\","
             "\"sizeInBytes\":%llu,\"horizontalCrs\":\"EPSG:6350\",\"verticalDatum\":\"%s\","
             "\"sampleFormat\":\"float32\",\"bitsPerSample\":32,\"compression\":\"LZW\",\"predictor\":3}"),
        *CogUrl(), ObjectBytes, Datum);
}

class FScriptedNativeAdapterTransport final : public IAcquisitionTransport
{
public:
    enum class ETagBehavior : uint8 { Stable, ChangeAfterFirstRange };

    TArray<uint8> Cog;
    FString Datum = TEXT("NAVD88");
    FString StableETag = TEXT("\"native-adapter-v1\"");
    FString ChangedETag = TEXT("\"native-adapter-v2\"");
    ETagBehavior Behavior = ETagBehavior::Stable;
    int32 CatalogRequests = 0;
    int32 CogRequests = 0;
    TArray<HttpAcquisitionRequest> Requests;

    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>&) override
    {
        Requests.Add(Request);
        if (!Request.ByteRange.IsSet()) return CatalogResponse();
        return CogRange(Request);
    }

private:
    HttpAcquisitionResult CatalogResponse()
    {
        HttpAcquisitionResult Result;
        Result.HttpStatus = 200;
        Result.ContentType = TEXT("application/json; charset=utf-8");
        const FString Json = CatalogRequests++ == 0
            ? FString::Printf(TEXT("{\"total\":1,\"items\":[%s]}"), *CatalogItem(Cog.Num(), *Datum))
            : TEXT("{\"total\":0,\"items\":[]}");
        const FTCHARToUTF8 Encoded(*Json);
        Result.Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
        Result.BytesReceived = Result.Bytes.Num();
        return Result;
    }

    HttpAcquisitionResult CogRange(const HttpAcquisitionRequest& Request)
    {
        HttpAcquisitionResult Result;
        Result.HttpStatus = 206;
        Result.ContentType = TEXT("image/tiff");
        const int32 RangeIndex = CogRequests++;
        Result.ETag = Behavior == ETagBehavior::ChangeAfterFirstRange && RangeIndex > 0
            ? ChangedETag : StableETag;
        if (!Request.ByteRange.IsSet())
        {
            Result.HttpStatus = 400;
            Result.FailureReason = TransportFailureReason::Other;
            return Result;
        }
        if (!Request.IfMatchETag.IsEmpty() && Request.IfMatchETag != Result.ETag)
        {
            Result.HttpStatus = 412;
            Result.FailureReason = TransportFailureReason::HttpStatus;
            return Result;
        }
        const HttpByteRange Range = Request.ByteRange.GetValue();
        if (Range.Offset > static_cast<uint64>(Cog.Num())
            || Range.Length > static_cast<uint64>(Cog.Num()) - Range.Offset)
        {
            Result.HttpStatus = 416;
            Result.FailureReason = TransportFailureReason::HttpStatus;
            return Result;
        }
        Result.Bytes.Append(Cog.GetData() + Range.Offset, static_cast<int32>(Range.Length));
        Result.BytesReceived = Result.Bytes.Num();
        Result.ContentRange = FString::Printf(TEXT("bytes %llu-%llu/%llu"), Range.Offset,
            Range.Offset + Range.Length - 1, static_cast<uint64>(Cog.Num()));
        return Result;
    }
};

FStagedTerrainSelectedSource ValidSelectedSource()
{
    FStagedTerrainSelectedSource Source;
    Source.Candidate.Product = SkiDomain::ElevationProduct::S1M;
    Source.Candidate.SourceId = "s1m-fixture";
    Source.Candidate.SupportedHorizontalCrs = true;
    Source.Candidate.Navd88Proven = true;
    Source.Candidate.SupportedEncoding = true;
    Source.DownloadUrl = CogUrl();
    Source.HorizontalCrs = TEXT("EPSG:6350");
    Source.VerticalDatum = TEXT("NAVD88");
    Source.bSiteCoverageVerified = true;
    Source.CoverageEvidenceId = TEXT("fixture-raster-site-intersection");
    return Source;
}

FStagedTerrainAcquisitionReceipt CatalogCompletedReceipt(const uint64 ObjectBytes,
    const FString& ETag = TEXT("\"native-adapter-v1\""))
{
    FStagedTerrainAcquisitionReceipt Receipt;
    FStagedTerrainCompletedStage Catalog;
    Catalog.Stage = EStagedTerrainStage::Catalog;
    Catalog.OutputSha256 = FString::ChrN(64, TEXT('a'));
    Catalog.CompletedUnits = 1;
    Catalog.TotalUnits = 1;
    Receipt.CompletedStages.Add(Catalog);
    Receipt.SelectedSources.Add(ValidSelectedSource());
    FStagedTerrainObjectPin Pin;
    Pin.ProductCode = TEXT("S1M");
    Pin.SourceId = TEXT("s1m-fixture");
    Pin.Url = CogUrl();
    Pin.ETag = ETag;
    Pin.ObjectBytes = ObjectBytes;
    Receipt.PinnedObjects.Add(Pin);
    return Receipt;
}

FStagedTerrainStageResult RunCatalog(FScriptedNativeAdapterTransport& Transport,
    const SkiDomain::GeographicBounds& Bounds = SupportedSite())
{
    FStagedTerrainAcquisitionRequest Request = MakeRequest(Bounds);
    FStagedTerrainAcquisitionReceipt Receipt;
    SkiNetGateway Gateway;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    FNativeStagedTerrainAcquisitionAdapter Adapter(Transport);
    return Adapter.RunStage(EStagedTerrainStage::Catalog, Request, Receipt, Gateway, CancelToken, {});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeStagedAdapterUnsupportedGeographyTest,
    "SkiPreparation.M4.NativeStagedAdapter.UnsupportedGeographyFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeStagedAdapterUnsupportedGeographyTest::RunTest(const FString&)
{
    FScriptedNativeAdapterTransport Transport;
    FStagedTerrainStageResult Result = RunCatalog(Transport, {2.30, 48.80, 2.31, 48.81});
    TestEqual(TEXT("Unsupported geography is a fatal catalog result"),
        static_cast<uint8>(Result.Outcome), static_cast<uint8>(EStagedTerrainStageOutcome::FatalFailure));
    TestEqual(TEXT("Unsupported geography has a stable reason"), Result.FailureCode,
        FString(TEXT("STAGED_TERRAIN_UNSUPPORTED_GEOGRAPHY")));
    TestFalse(TEXT("The unsupported site is not marked supported"), Result.Proof.bSupportedGeography);
    TestFalse(TEXT("No full-site coverage is inferred"), Result.Proof.bSiteCoverageVerified);
    TestEqual(TEXT("Out-of-scope geography performs no network requests"), Transport.Requests.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeStagedAdapterDatumRejectionTest,
    "SkiPreparation.M4.NativeStagedAdapter.RejectsNonNavd88Datum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeStagedAdapterDatumRejectionTest::RunTest(const FString&)
{
    FScriptedNativeAdapterTransport Transport;
    TestTrue(TEXT("Synthetic S1M COG fixture is written"), MakeCogFixture(Transport.Cog));
    Transport.Datum = TEXT("EGM2008");
    FStagedTerrainStageResult Result = RunCatalog(Transport);
    TestEqual(TEXT("A non-NAVD88 source fails closed"), Result.FailureCode,
        FString(TEXT("STAGED_TERRAIN_NAVD88_DATUM_NOT_PROVEN")));
    TestTrue(TEXT("Datum rejection cannot select a source"), Result.SelectedSources.IsEmpty());
    TestTrue(TEXT("COG preflight explains the rejected datum"),
        Result.FailureDetail.Contains(TEXT("COG_PREFLIGHT_CATALOG_DATUM_UNSUPPORTED")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeStagedAdapterMissingXmlTest,
    "SkiPreparation.M4.NativeStagedAdapter.MissingS1mXmlAndGpkgFailClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeStagedAdapterMissingXmlTest::RunTest(const FString&)
{
    FScriptedNativeAdapterTransport Transport;
    TestTrue(TEXT("Synthetic S1M COG fixture is written"), MakeCogFixture(Transport.Cog));
    FStagedTerrainStageResult Result = RunCatalog(Transport);
    TestEqual(TEXT("Raw source lineage is a hard catalog-stage gate"), Result.FailureCode,
        FString(TEXT("STAGED_TERRAIN_S1M_XML_GPKG_NOT_PROVEN")));
    TestTrue(TEXT("The missing raw sidecars are reported explicitly"),
        Result.FailureDetail.Contains(TEXT("LINEAGE_SIDECARS_NOT_READ")));
    TestTrue(TEXT("No source is promoted without sidecar lineage"), Result.SelectedSources.IsEmpty());
    TestEqual(TEXT("COG-only catalog evidence still captures its observed object pin"), Result.ObjectPins.Num(), 1);
    if (Result.ObjectPins.Num() == 1)
    {
        TestEqual(TEXT("The catalog pin retains the strong ETag"), Result.ObjectPins[0].ETag, Transport.StableETag);
        TestEqual(TEXT("The catalog pin retains the exact object byte count"), Result.ObjectPins[0].ObjectBytes,
            static_cast<uint64>(Transport.Cog.Num()));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeStagedAdapterNoCoveragePromotionTest,
    "SkiPreparation.M4.NativeStagedAdapter.TnmBboxAndCogDoNotProveFullSiteCoverage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeStagedAdapterNoCoveragePromotionTest::RunTest(const FString&)
{
    FScriptedNativeAdapterTransport Transport;
    TestTrue(TEXT("Synthetic S1M COG fixture is written"), MakeCogFixture(Transport.Cog));
    FStagedTerrainStageResult Result = RunCatalog(Transport);
    TestEqual(TEXT("Bbox and COG evidence cannot complete the Catalog stage"),
        static_cast<uint8>(Result.Outcome), static_cast<uint8>(EStagedTerrainStageOutcome::FatalFailure));
    TestTrue(TEXT("The supported US geography envelope is reported"), Result.Proof.bSupportedGeography);
    TestFalse(TEXT("TNM bbox candidates and COG georeferencing are not full-site raster coverage"),
        Result.Proof.bSiteCoverageVerified);
    TestTrue(TEXT("The failure calls out the unchecked TNM bbox coverage"),
        Result.FailureDetail.Contains(TEXT("TNM_BBOX_QUERY_RETURNED_PRODUCTS_RASTER_EXTENT_UNCHECKED")));
    TestTrue(TEXT("A matching COG header was actually checked"),
        Result.FailureDetail.Contains(TEXT("COG_PREFLIGHT_PASSED")));
    TestTrue(TEXT("Missing proof gates remain in the stage detail"),
        Result.FailureDetail.Contains(TEXT("raw-gpkg-xml-lineage="))
            && Result.FailureDetail.Contains(TEXT("sampled-quality=")));
    TestTrue(TEXT("No selected source is promoted past the missing proof gates"), Result.SelectedSources.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeStagedAdapterStaleEtagTest,
    "SkiPreparation.M4.NativeStagedAdapter.StaleCatalogEtagFailsSourcePreflight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeStagedAdapterStaleEtagTest::RunTest(const FString&)
{
    FScriptedNativeAdapterTransport Transport;
    TestTrue(TEXT("Synthetic S1M COG fixture is written"), MakeCogFixture(Transport.Cog));
    Transport.StableETag = TEXT("\"native-adapter-v2\"");
    const FStagedTerrainAcquisitionRequest Request = MakeRequest();
    const FStagedTerrainAcquisitionReceipt Receipt = CatalogCompletedReceipt(
        static_cast<uint64>(Transport.Cog.Num()), TEXT("\"native-adapter-v1\""));
    SkiNetGateway Gateway;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    FNativeStagedTerrainAcquisitionAdapter Adapter(Transport);
    const FStagedTerrainStageResult Result = Adapter.RunStage(EStagedTerrainStage::SourcePreflight,
        Request, Receipt, Gateway, CancelToken, {});
    TestEqual(TEXT("The fresh strong ETag cannot replace Catalog's stale object pin"), Result.FailureCode,
        FString(TEXT("STAGED_TERRAIN_ETAG_OR_OBJECT_IDENTITY_CHANGED")));
    TestTrue(TEXT("Ranges used a consistent replacement object ETag"), Transport.CogRequests > 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeStagedAdapterSourcePreflightTest,
    "SkiPreparation.M4.NativeStagedAdapter.SourcePreflightBindsStrongEtagAndExactSize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeStagedAdapterSourcePreflightTest::RunTest(const FString&)
{
    FScriptedNativeAdapterTransport Transport;
    TestTrue(TEXT("Synthetic S1M COG fixture is written"), MakeCogFixture(Transport.Cog));
    const FStagedTerrainAcquisitionRequest Request = MakeRequest();
    const FStagedTerrainAcquisitionReceipt Receipt = CatalogCompletedReceipt(
        static_cast<uint64>(Transport.Cog.Num()));
    SkiNetGateway Gateway;
    const TSharedRef<Cancellation> CancelToken = MakeShared<Cancellation, ESPMode::ThreadSafe>();
    FNativeStagedTerrainAcquisitionAdapter Adapter(Transport);
    const FStagedTerrainStageResult Result = Adapter.RunStage(EStagedTerrainStage::SourcePreflight,
        Request, Receipt, Gateway, CancelToken, {});
    TestEqual(TEXT("Matching source facts complete source preflight"),
        static_cast<uint8>(Result.Outcome), static_cast<uint8>(EStagedTerrainStageOutcome::Succeeded));
    TestEqual(TEXT("One COG observation is returned per selected source"), Result.CogObservations.Num(), 1);
    TestEqual(TEXT("The preflight pin is retained"), Result.ObjectPins.Num(), 1);
    if (Result.CogObservations.Num() == 1)
    {
        TestTrue(TEXT("The source observation passed COG validation"), Result.CogObservations[0].Preflight.bPassed);
        TestEqual(TEXT("The observation retains the catalog ETag"), Result.CogObservations[0].StrongETag,
            Transport.StableETag);
        TestEqual(TEXT("The observation retains the exact COG size"), Result.CogObservations[0].Preflight.ObjectBytes,
            static_cast<uint64>(Transport.Cog.Num()));
    }
    return true;
}

#endif
