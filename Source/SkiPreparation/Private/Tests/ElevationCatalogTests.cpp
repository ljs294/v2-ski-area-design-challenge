#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SkiPreparation/CogPreflight.h"
#include "SkiPreparation/ElevationCatalog.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "tiffio.h"

namespace
{
constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;

bool WriteCogDirectory(TIFF* Image, const uint32 Width, const bool bOverview,
    const uint32 Epsg, const bool bIncludeNavd88)
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
        const double Tie[] = {0.0, 0.0, 0.0,
            Epsg == 4269 ? -122.0 : 500000.0,
            Epsg == 4269 ? 47.0 : 5000000.0, 0.0};
        TIFFSetField(Image, ModelPixelScaleTag, 3U, const_cast<double*>(Scale));
        TIFFSetField(Image, ModelTiepointTag, 6U, const_cast<double*>(Tie));
        const uint16 KeyCount = bIncludeNavd88 ? 5 : 3;
        TArray<uint16> Keys;
        Keys.Add(1); Keys.Add(1); Keys.Add(0); Keys.Add(KeyCount);
        const auto AddKey = [&Keys](const uint16 Key, const uint16 Value)
        { Keys.Add(Key); Keys.Add(0); Keys.Add(1); Keys.Add(Value); };
        AddKey(1024, Epsg == 4269 ? 2 : 1);
        AddKey(1025, 1);
        AddKey(Epsg == 4269 ? 2048 : 3072, static_cast<uint16>(Epsg));
        if (bIncludeNavd88)
        {
            AddKey(4096, 5703);
            AddKey(4098, 5103);
        }
        TIFFSetField(Image, GeoKeyDirectoryTag, static_cast<uint32>(Keys.Num()), Keys.GetData());
        TIFFSetField(Image, TIFFTAG_GDAL_NODATA, 8U, const_cast<char*>("-999999"));
    }

    TArray<float> Tile;
    Tile.SetNumUninitialized(TileSide * TileSide);
    for (int32 Index = 0; Index < Tile.Num(); ++Index)
        Tile[Index] = 1200.0F + static_cast<float>(Index % 17);
    if (TIFFWriteEncodedTile(Image, TIFFComputeTile(Image, 0, 0, 0, 0), Tile.GetData(),
            static_cast<tmsize_t>(Tile.Num() * sizeof(float))) < 0) return false;
    return TIFFWriteDirectory(Image) != 0;
}

bool WriteCogFixture(const FString& Path, TArray<uint8>& OutBytes,
    const uint32 Epsg, const bool bIncludeNavd88)
{
    OutBytes.Reset();
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    const bool bBase = WriteCogDirectory(Image, 512, false, Epsg, bIncludeNavd88);
    const bool bOverview = bBase && WriteCogDirectory(Image, 256, true, Epsg, false);
    TIFFClose(Image);
    if (!bBase || !bOverview || !FFileHelper::LoadFileToArray(OutBytes, *Path)) return false;
    IFileManager::Get().Delete(*Path);
    return !OutBytes.IsEmpty();
}

class FScriptedElevationCatalogTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    enum class ECogMutation : uint8 { None, ChangedRangeTotal };
    TArray<FString> Responses;
    TArray<FString> RequestedUrls;
    TArray<FString> CatalogUrls;
    TArray<SkiPreparation::HttpAcquisitionRequest> CogRequests;
    TArray<uint8> S1mCog;
    TArray<uint8> ProjectCog;
    TArray<uint8> ArcSecCog;
    int32 ResponseIndex = 0;
    int32 CancelOnRequestIndex = INDEX_NONE;
    int32 CancelOnCogRequestIndex = INDEX_NONE;
    ECogMutation CogMutation = ECogMutation::None;

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation) override
    {
        RequestedUrls.Add(Request.Url);
        if (Request.ByteRange.IsSet()) return GetCogRange(Request, Cancellation);
        CatalogUrls.Add(Request.Url);
        const int32 RequestIndex = RequestedUrls.Num() - 1;
        SkiPreparation::HttpAcquisitionResult Result;
        Result.HttpStatus = 200;
        Result.ContentType = TEXT("application/json; charset=utf-8");
        Result.Attempt = Request.Attempt;
        if (!Responses.IsValidIndex(ResponseIndex))
        {
            Result.HttpStatus = 500;
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            Result.RequestStatus = TEXT("SCRIPT_EMPTY");
            return Result;
        }
        const FTCHARToUTF8 Utf8(*Responses[ResponseIndex++]);
        Result.Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        Result.BytesReceived = Result.Bytes.Num();
        if (RequestIndex == CancelOnRequestIndex) Cancellation->Cancel();
        return Result;
    }

private:
    SkiPreparation::HttpAcquisitionResult GetCogRange(
        const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation)
    {
        CogRequests.Add(Request);
        const int32 CogRequestIndex = CogRequests.Num() - 1;
        const TArray<uint8>* Object = nullptr;
        if (Request.Url.Contains(TEXT("/S1M/"))) Object = &S1mCog;
        else if (Request.Url.Contains(TEXT("/Project1m/"))) Object = &ProjectCog;
        else if (Request.Url.Contains(TEXT("/13/"))) Object = &ArcSecCog;

        SkiPreparation::HttpAcquisitionResult Result;
        Result.HttpStatus = 206;
        Result.ContentType = TEXT("image/tiff");
        Result.ETag = TEXT("\"catalog-cog-v1\"");
        Result.Attempt = Request.Attempt;
        if (!Object || Object->IsEmpty() || !Request.ByteRange.IsSet())
        {
            Result.HttpStatus = 404;
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            return Result;
        }
        const SkiPreparation::HttpByteRange Range = Request.ByteRange.GetValue();
        if (Range.Offset > static_cast<uint64>(Object->Num())
            || Range.Length > static_cast<uint64>(Object->Num()) - Range.Offset)
        {
            Result.HttpStatus = 416;
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            return Result;
        }
        Result.Bytes.Append(Object->GetData() + Range.Offset, static_cast<int32>(Range.Length));
        Result.BytesReceived = Result.Bytes.Num();
        const uint64 Start = Range.Offset;
        const uint64 End = Range.Offset + Range.Length - 1;
        uint64 Total = Object->Num();
        if (CogMutation == ECogMutation::ChangedRangeTotal && CogRequestIndex > 0) ++Total;
        Result.ContentRange = FString::Printf(TEXT("bytes %llu-%llu/%llu"), Start, End, Total);
        if (CogRequestIndex == CancelOnCogRequestIndex) Cancellation->Cancel();
        return Result;
    }
};

FString CatalogItem(const TCHAR* Id, const TCHAR* Crs, const TCHAR* Datum,
    const int32 Quality, const TCHAR* CollectionEnd, const TCHAR* Publication,
    const uint64 SizeBytes)
{
    // EPSG:4269 contains the substring "269", so test the full CRS token first.
    const TCHAR* ProductPath = FCString::Strstr(Crs, TEXT("4269")) ? TEXT("13")
        : FCString::Strstr(Crs, TEXT("269")) ? TEXT("Project1m") : TEXT("S1M");
    const FString SizeField = SizeBytes > 0
        ? FString::Printf(TEXT(",\"sizeInBytes\":%llu"), SizeBytes) : FString();
    return FString::Printf(
        TEXT("{\"sourceId\":\"%s\",\"title\":\"DEM %s\",\"downloadURL\":\"https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/%s/%s.tif\"%s,\"horizontalCrs\":\"%s\",\"verticalDatum\":\"%s\",\"sampleFormat\":\"float32\",\"bitsPerSample\":32,\"compression\":\"LZW\",\"predictor\":3,\"qualityLevel\":%d,\"collectionEndDate\":\"%s\",\"publicationDate\":\"%s\"}"),
        Id, Id, ProductPath, Id, *SizeField, Crs, Datum, Quality, CollectionEnd, Publication);
}

bool InstallCogFixtures(FAutomationTestBase& Test, FScriptedElevationCatalogTransport& Transport)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("ElevationCatalogFixtures"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const bool bS1m = WriteCogFixture(FPaths::Combine(Directory, TEXT("s1m.tif")), Transport.S1mCog, 6350, true);
    const bool bProject = WriteCogFixture(FPaths::Combine(Directory, TEXT("project.tif")), Transport.ProjectCog, 26910, false);
    const bool bArcSec = WriteCogFixture(FPaths::Combine(Directory, TEXT("arcsec.tif")), Transport.ArcSecCog, 4269, true);
    Test.TestTrue(TEXT("write a synthetic S1M COG"), bS1m);
    Test.TestTrue(TEXT("write a synthetic Project1m COG"), bProject);
    Test.TestTrue(TEXT("write a synthetic 1/3 arc-second COG"), bArcSec);
    return bS1m && bProject && bArcSec;
}

uint64 CogSizeForCrs(const FScriptedElevationCatalogTransport& Transport, const TCHAR* Crs)
{
    if (FCString::Strstr(Crs, TEXT("4269"))) return Transport.ArcSecCog.Num();
    if (FCString::Strstr(Crs, TEXT("269"))) return Transport.ProjectCog.Num();
    return Transport.S1mCog.Num();
}

FString Page(const TArray<FString>& Items)
{
    FString Joined;
    for (const FString& Item : Items)
    {
        if (!Joined.IsEmpty()) Joined += TEXT(",");
        Joined += Item;
    }
    return FString::Printf(TEXT("{\"total\":%d,\"items\":[%s]}"), Items.Num(), *Joined);
}

void AddPage(FScriptedElevationCatalogTransport& Transport,
    std::initializer_list<FString> Items)
{
    TArray<FString> ItemArray;
    for (const FString& Item : Items) ItemArray.Add(Item);
    Transport.Responses.Add(Page(ItemArray));
}

SkiDomain::GeographicBounds TestSite()
{
    return {-122.51, 47.00, -122.49, 47.02};
}

FString ProductId(const SkiPreparation::ElevationCatalogSource& Source)
{
    return FString(UTF8_TO_TCHAR(Source.Candidate.SourceId.c_str()));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogOrderingTest,
    "MountainPlanner.M3.ElevationCatalog.SourceOrderingAndPreflight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogOrderingTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    if (!InstallCogFixtures(*this, Transport)) return false;
    AddPage(Transport, {CatalogItem(TEXT("s1m-tile"), TEXT("EPSG:6350"), TEXT("NAVD88"),
        0, TEXT("2023-01-01"), TEXT("2024-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:6350")))});
    AddPage(Transport, {
        CatalogItem(TEXT("project-old"), TEXT("EPSG:26910"), TEXT("NAVD 88"),
            2, TEXT("2018-01-01"), TEXT("2019-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:26910"))),
        CatalogItem(TEXT("project-new"), TEXT("EPSG:26910"), TEXT("North American Vertical Datum of 1988 (NAVD 88)"),
            1, TEXT("2022-01-01"), TEXT("2023-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:26910"))),
        CatalogItem(TEXT("project-unproven"), TEXT("EPSG:26910"), TEXT(""),
            1, TEXT("2024-01-01"), TEXT("2024-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:26910")))});
    AddPage(Transport, {CatalogItem(TEXT("arcsec"), TEXT("EPSG:4269"), TEXT("NAVD88"),
        0, TEXT("2021-01-01"), TEXT("2022-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:4269")))});

    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestTrue(TEXT("Scripted TNM response completes preflight"), Catalog.Preflight(TestSite(), Cancellation, Report));
    TestEqual(TEXT("One catalog query per elevation tier"), Transport.CatalogUrls.Num(), 3);
    TestTrue(TEXT("All requests remain inside the TNM gateway allow-list"),
        Transport.RequestedUrls.ContainsByPredicate([](const FString& Url)
        {
            FString Reason;
            return !SkiPreparation::SkiNetGateway::ValidateUrl(Url, Reason);
        }) == false);
    TestTrue(TEXT("US geography is supported"), Report.IsSupportedGeography);
    TestTrue(TEXT("Catalog coverage is present"), Report.HasElevationCoverage);
    TestFalse(TEXT("Verified COG headers alone do not enable a full download"), Report.DownloadEnabled);
    TestEqual(TEXT("Catalog completion is distinct from full-download readiness"),
        static_cast<uint8>(Report.Status),
        static_cast<uint8>(SkiPreparation::ElevationCatalogStatus::CatalogReady));
    TestEqual(TEXT("Full download waits for site coverage, raw lineage, and sampled quality proof"), Report.FailureCode,
        FString(TEXT("ELEVATION_SITE_COVERAGE_LINEAGE_AND_QUALITY_PROOF_REQUIRED")));
    TestEqual(TEXT("Resolved chain has S1M, two proven projects, then 1/3 arcsecond"),
        Report.ResolvedSources.Num(), 4);
    if (Report.ResolvedSources.Num() == 4)
    {
        TestEqual(TEXT("S1M wins first"), ProductId(Report.ResolvedSources[0]), FString(TEXT("S1M:s1m-tile")));
        TestEqual(TEXT("Higher-quality newer project follows S1M"), ProductId(Report.ResolvedSources[1]),
            FString(TEXT("PROJECT1M:project-new")));
        TestEqual(TEXT("Older project is a deterministic fallback"), ProductId(Report.ResolvedSources[2]),
            FString(TEXT("PROJECT1M:project-old")));
        TestEqual(TEXT("1/3 arcsecond is the final tier"), ProductId(Report.ResolvedSources[3]),
            FString(TEXT("ARCSEC13:arcsec")));
        TestTrue(TEXT("The ArcSec catalog item uses its 1/3-arcsecond object URL"),
            Report.ResolvedSources[3].DownloadUrl.Contains(TEXT("/13/")));
        TestEqual(TEXT("The ArcSec receipt is bound to the matching EPSG:4269 fixture"),
            Report.ResolvedSources[3].CogPreflight.HorizontalCrs, FString(TEXT("EPSG:4269")));
        TestEqual(TEXT("The ArcSec exact size comes from its own object ranges"),
            Report.ResolvedSources[3].VerifiedCogObjectBytes,
            static_cast<uint64>(Transport.ArcSecCog.Num()));
    }
    const SkiPreparation::ElevationCatalogSource* OfficialDatumProject = Report.Sources.FindByPredicate(
        [](const SkiPreparation::ElevationCatalogSource& Source)
        { return ProductId(Source) == TEXT("PROJECT1M:project-new"); });
    TestTrue(TEXT("Project with the full official NAVD88 name remains in catalog facts"),
        OfficialDatumProject != nullptr);
    if (OfficialDatumProject)
    {
        TestTrue(TEXT("The official North American Vertical Datum name proves NAVD88"),
            OfficialDatumProject->Candidate.Navd88Proven);
        TestEqual(TEXT("The original catalog datum remains available for audit"),
            OfficialDatumProject->ReportedVerticalDatum,
            FString(TEXT("North American Vertical Datum of 1988 (NAVD 88)")));
        TestEqual(TEXT("The recognized alias is canonicalized before the stricter COG check"),
            OfficialDatumProject->CogPreflight.VerticalDatum, FString(TEXT("NAVD88")));
        TestEqual(TEXT("The vertical proof still records TNM metadata as its source"),
            static_cast<uint8>(OfficialDatumProject->Proofs.VerticalDatumOrigin),
            static_cast<uint8>(SkiPreparation::ElevationProofOrigin::TnmMetadata));
        TestTrue(TEXT("The proven higher-quality project remains in the ordered fallback chain"),
            Report.ResolvedSources.ContainsByPredicate([](const SkiPreparation::ElevationCatalogSource& Source)
            { return ProductId(Source) == TEXT("PROJECT1M:project-new"); }));
    }
    const SkiPreparation::ElevationCatalogSource* RejectedProject = Report.Sources.FindByPredicate(
        [](const SkiPreparation::ElevationCatalogSource& Source)
        { return ProductId(Source) == TEXT("PROJECT1M:project-unproven"); });
    TestTrue(TEXT("Project without explicit NAVD88 stays in catalog facts"), RejectedProject != nullptr);
    if (RejectedProject)
    {
        TestFalse(TEXT("Project without datum proof is not resolver eligible"),
            RejectedProject->Candidate.Navd88Proven);
        TestEqual(TEXT("Missing datum retains the COG preflight's specific fail-closed reason"),
            RejectedProject->EligibilityReasonCode, FString(TEXT("COG_PREFLIGHT_DATUM_UNPROVEN")));
        TestTrue(TEXT("The excluded project still has an exact catalog object size"),
            RejectedProject->HasExactObjectBytes);
        TestTrue(TEXT("The failed candidate retains its exact TNM size fact"),
            RejectedProject->ExactObjectBytes > 0);
        TestEqual(TEXT("Missing COG datum proof has a stable rejection reason"),
            RejectedProject->CogStatusCode, FString(TEXT("COG_PREFLIGHT_DATUM_UNPROVEN")));
        TestFalse(TEXT("No COG receipt passes without an external datum proof"),
            RejectedProject->CogPreflight.bPassed);
        TestFalse(TEXT("Project with missing datum is excluded from the resolved fallback chain"),
            Report.ResolvedSources.ContainsByPredicate([](const SkiPreparation::ElevationCatalogSource& Source)
            { return ProductId(Source) == TEXT("PROJECT1M:project-unproven"); }));
    }
    TestEqual(TEXT("Exact resolved candidate object sizes are distinguished"),
        static_cast<uint8>(Report.ResolvedCandidateObjectBytes.Certainty),
        static_cast<uint8>(SkiPreparation::StorageSizeCertainty::Exact));
    const uint64 ExpectedResolvedBytes = Transport.S1mCog.Num()
        + static_cast<uint64>(Transport.ProjectCog.Num()) * 2
        + Transport.ArcSecCog.Num();
    TestEqual(TEXT("Exact size sum follows the resolved COG-proven chain"), Report.ResolvedCandidateObjectBytes.Bytes,
        ExpectedResolvedBytes);
    TestTrue(TEXT("Resolved candidate size basis is not described as a planned download"),
        Report.ResolvedCandidateObjectBytes.Basis.Contains(TEXT("not a planned download total")));
    TestEqual(TEXT("Canonical scratch is explicitly estimated"),
        static_cast<uint8>(Report.CanonicalScratchBytes.Certainty),
        static_cast<uint8>(SkiPreparation::StorageSizeCertainty::Estimated));
    TestFalse(TEXT("Catalog does not claim sidecar lineage"), Report.HasCompleteSourceLineage);
    TestEqual(TEXT("Lineage gap is explicit"), Report.LineageStatusCode, FString(TEXT("LINEAGE_SIDECARS_NOT_READ")));
    TestTrue(TEXT("Every retained resolver source has a bound COG header receipt"), Report.HasVerifiedCogHeaders);
    TestEqual(TEXT("The unproven project is recorded as a rejected COG candidate"), Report.CogStatusCode,
        FString(TEXT("COG_PREFLIGHT_COMPLETED_WITH_REJECTIONS")));
    TestFalse(TEXT("COG georeferencing does not claim full site coverage"), Report.HasVerifiedSiteCoverage);
    TestEqual(TEXT("The TNM bbox query is not promoted to raster coverage"), Report.CoverageStatusCode,
        FString(TEXT("TNM_BBOX_QUERY_RETURNED_PRODUCTS_RASTER_EXTENT_UNCHECKED")));
    if (!Report.ResolvedSources.IsEmpty())
    {
        const SkiPreparation::ElevationCatalogSource& Proven = Report.ResolvedSources[0];
        TestTrue(TEXT("COG identity points back to the resolved source ID"),
            Proven.CogProofSourceId == ProductId(Proven));
        TestEqual(TEXT("COG proof URL remains the catalog URL"), Proven.CogProofDownloadUrl, Proven.DownloadUrl);
        TestEqual(TEXT("COG size equals the exact range total"), Proven.VerifiedCogObjectBytes,
            Proven.CogPreflight.ObjectBytes);
        TestEqual(TEXT("The source remembers the exact bbox query used for catalog coverage"),
            Proven.CoverageStatusCode, FString(TEXT("TNM_BBOX_QUERY_RETURNED_CANDIDATE")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogCogReceiptBindingTest,
    "MountainPlanner.M3.ElevationCatalog.CogReceiptBindsIdentitySizeAndQueryBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogCogReceiptBindingTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    if (!InstallCogFixtures(*this, Transport)) return false;
    AddPage(Transport, {CatalogItem(TEXT("s1m-proof"), TEXT("EPSG:6350"), TEXT("NAVD88"),
        0, TEXT("2023-01-01"), TEXT("2024-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:6350")))});
    AddPage(Transport, {});
    AddPage(Transport, {});

    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestTrue(TEXT("Catalog and COG proof complete"), Catalog.Preflight(TestSite(), Cancellation, Report));
    TestEqual(TEXT("One matching S1M candidate resolves"), Report.ResolvedSources.Num(), 1);
    TestTrue(TEXT("Resolved COG receipts are reported"), Report.HasVerifiedCogHeaders);
    TestEqual(TEXT("COG request used the candidate's catalog URL"), Transport.CogRequests.Num() > 0
        ? Transport.CogRequests[0].Url : FString(),
        Report.Sources.Num() > 0 ? Report.Sources[0].DownloadUrl : FString());
    if (!Report.ResolvedSources.IsEmpty())
    {
        const SkiPreparation::ElevationCatalogSource& Source = Report.ResolvedSources[0];
        TestEqual(TEXT("Receipt is bound to the same source ID"), Source.CogProofSourceId, ProductId(Source));
        TestEqual(TEXT("Receipt is bound to the same URL"), Source.CogProofDownloadUrl, Source.DownloadUrl);
        TestEqual(TEXT("Exact source size equals the range total"), Source.ExactObjectBytes, Source.CogPreflight.ObjectBytes);
        TestEqual(TEXT("The size proof records its origin as the COG range response"),
            static_cast<uint8>(Source.Proofs.ObjectSizeOrigin),
            static_cast<uint8>(SkiPreparation::ElevationProofOrigin::CogHeader));
        TestTrue(TEXT("Catalog coverage evidence retains outward-quantized query bounds"),
            Source.CatalogQueryBounds.WestDeg <= TestSite().WestDeg
                && Source.CatalogQueryBounds.EastDeg >= TestSite().EastDeg);
    }
    TestFalse(TEXT("COG header proof does not assert raster coverage of the full site"),
        Report.HasVerifiedSiteCoverage);
    TestFalse(TEXT("COG alone cannot enable a full download"), Report.DownloadEnabled);
    TestEqual(TEXT("Missing raw lineage and sampled quality remain explicit"), Report.FailureCode,
        FString(TEXT("ELEVATION_SITE_COVERAGE_LINEAGE_AND_QUALITY_PROOF_REQUIRED")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogCogBindingFailuresTest,
    "MountainPlanner.M3.ElevationCatalog.CogSizeAndChangedRangesFailClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogCogBindingFailuresTest::RunTest(const FString&)
{
    {
        FScriptedElevationCatalogTransport Transport;
        if (!InstallCogFixtures(*this, Transport)) return false;
        const uint64 WrongSize = CogSizeForCrs(Transport, TEXT("EPSG:6350")) + 1;
        AddPage(Transport, {CatalogItem(TEXT("s1m-size-mismatch"), TEXT("EPSG:6350"), TEXT("NAVD88"),
            0, TEXT("2023-01-01"), TEXT("2024-01-01"), WrongSize)});
        AddPage(Transport, {});
        AddPage(Transport, {});

        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::ElevationCatalog Catalog(Transport);
        SkiPreparation::TerrainAvailabilityReport Report;
        TestTrue(TEXT("Catalog query completes with a size mismatch"), Catalog.Preflight(TestSite(), Cancellation, Report));
        TestEqual(TEXT("Mismatched object size removes the source from the fallback chain"), Report.ResolvedSources.Num(), 0);
        TestFalse(TEXT("A mismatched COG is not counted as verified"), Report.HasVerifiedCogHeaders);
        if (!Report.Sources.IsEmpty())
            TestEqual(TEXT("The source records the size binding failure"), Report.Sources[0].CogStatusCode,
                FString(TEXT("CATALOG_COG_OBJECT_SIZE_MISMATCH")));
        TestFalse(TEXT("Size mismatch cannot enable download"), Report.DownloadEnabled);
    }
    {
        FScriptedElevationCatalogTransport Transport;
        if (!InstallCogFixtures(*this, Transport)) return false;
        AddPage(Transport, {CatalogItem(TEXT("s1m-range-change"), TEXT("EPSG:6350"), TEXT("NAVD88"),
            0, TEXT("2023-01-01"), TEXT("2024-01-01"), 0)});
        AddPage(Transport, {});
        AddPage(Transport, {});
        Transport.CogMutation = FScriptedElevationCatalogTransport::ECogMutation::ChangedRangeTotal;

        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::ElevationCatalog Catalog(Transport);
        SkiPreparation::TerrainAvailabilityReport Report;
        TestTrue(TEXT("Catalog query completes when a later COG range changes total"),
            Catalog.Preflight(TestSite(), Cancellation, Report));
        TestTrue(TEXT("The scripted preflight requests multiple ranges"), Transport.CogRequests.Num() > 1);
        if (!Report.Sources.IsEmpty())
            TestEqual(TEXT("Changed range total is retained as the source rejection reason"),
                Report.Sources[0].CogStatusCode, FString(TEXT("COG_PREFLIGHT_OBJECT_SIZE_CHANGED")));
        TestEqual(TEXT("Changed object cannot remain in the resolver output"), Report.ResolvedSources.Num(), 0);
        TestFalse(TEXT("Changed object total cannot enable download"), Report.DownloadEnabled);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogCogCancellationTest,
    "MountainPlanner.M3.ElevationCatalog.CancellationDuringCogRangesCannotPublishReport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogCogCancellationTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    if (!InstallCogFixtures(*this, Transport)) return false;
    AddPage(Transport, {CatalogItem(TEXT("s1m-cancel-cog"), TEXT("EPSG:6350"), TEXT("NAVD88"),
        0, TEXT("2023-01-01"), TEXT("2024-01-01"), 0)});
    AddPage(Transport, {});
    AddPage(Transport, {});
    Transport.CancelOnCogRequestIndex = 0;

    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestFalse(TEXT("Cancellation after the initial COG range aborts the catalog proof"),
        Catalog.Preflight(TestSite(), Cancellation, Report));
    TestEqual(TEXT("Cancellation retains its stable top-level reason"), Report.FailureCode,
        FString(TEXT("PREPARATION_CANCELLED")));
    TestEqual(TEXT("Cancelled proof never publishes a catalog-ready status"),
        static_cast<uint8>(Report.Status), static_cast<uint8>(SkiPreparation::ElevationCatalogStatus::CatalogFailure));
    TestFalse(TEXT("Cancelled proof never enables download"), Report.DownloadEnabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogArcSecondQuerySlashTest,
    "MountainPlanner.M3.ElevationCatalog.ArcSecondDatasetSlashIsQueryData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogArcSecondQuerySlashTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    AddPage(Transport, {});
    AddPage(Transport, {});
    AddPage(Transport, {});
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestTrue(TEXT("All three catalog tiers complete with empty coverage"),
        Catalog.Preflight(TestSite(), Cancellation, Report));
    TestEqual(TEXT("The 1/3 arc-second tier reaches the transport"), Transport.CatalogUrls.Num(), 3);

    if (Transport.CatalogUrls.Num() == 3)
    {
        const FString& ArcSecondUrl = Transport.CatalogUrls[2];
        TestTrue(TEXT("The dataset label keeps its slash literal in the query value"),
            ArcSecondUrl.Contains(TEXT("1/3")) && !ArcSecondUrl.ToLower().Contains(TEXT("%2f")));
        FString Reason;
        TestTrue(TEXT("The gateway accepts the safe query slash"),
            SkiPreparation::SkiNetGateway::ValidateUrl(ArcSecondUrl, Reason));
    }

    FString TraversalReason;
    TestFalse(TEXT("The gateway still rejects encoded slashes in the request path"),
        SkiPreparation::SkiNetGateway::ValidateUrl(
            TEXT("https://tnmaccess.nationalmap.gov/api/v1/products%2F..%2Fproducts?bbox=test"),
            TraversalReason));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogQuantizedBoundsCacheTest,
    "MountainPlanner.M3.ElevationCatalog.QuantizedBoundsCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogQuantizedBoundsCacheTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    if (!InstallCogFixtures(*this, Transport)) return false;
    AddPage(Transport, {CatalogItem(TEXT("s1m-cache"), TEXT("EPSG:6350"), TEXT("NAVD88"),
        0, TEXT("2023-01-01"), TEXT("2024-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:6350")))});
    AddPage(Transport, {});
    AddPage(Transport, {});
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport FirstReport;
    SkiPreparation::TerrainAvailabilityReport CachedReport;
    const SkiDomain::GeographicBounds FirstBounds{-122.51005, 47.00005, -122.48995, 47.02005};
    const SkiDomain::GeographicBounds SameQuantizedBounds{-122.51004, 47.00006, -122.48994, 47.02006};

    TestTrue(TEXT("Initial catalog lookup succeeds"), Catalog.Preflight(FirstBounds, Cancellation, FirstReport));
    TestEqual(TEXT("Initial lookup makes one catalog request per tier"), Transport.CatalogUrls.Num(), 3);
    const int32 TotalRequestsAfterFirst = Transport.RequestedUrls.Num();
    TestTrue(TEXT("Nearby bounds in the same outward-quantized cell reuse the cached report"),
        Catalog.Preflight(SameQuantizedBounds, Cancellation, CachedReport));
    TestEqual(TEXT("Cache hit makes no additional transport calls"), Transport.RequestedUrls.Num(), TotalRequestsAfterFirst);
    TestEqual(TEXT("Cached result retains catalog-ready status"), static_cast<uint8>(CachedReport.Status),
        static_cast<uint8>(SkiPreparation::ElevationCatalogStatus::CatalogReady));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogFinalPageCancellationTest,
    "MountainPlanner.M3.ElevationCatalog.FinalPageCancellationCannotPublishReady",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogFinalPageCancellationTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    AddPage(Transport, {CatalogItem(TEXT("s1m-cancel"), TEXT("EPSG:6350"), TEXT("NAVD88"),
        0, TEXT("2023-01-01"), TEXT("2024-01-01"), 120)});
    AddPage(Transport, {});
    AddPage(Transport, {});
    Transport.CancelOnRequestIndex = 2;

    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestFalse(TEXT("Cancellation raised during the final successful transport response aborts preflight"),
        Catalog.Preflight(TestSite(), Cancellation, Report));
    TestEqual(TEXT("Cancellation remains the reported failure"), Report.FailureCode,
        FString(TEXT("PREPARATION_CANCELLED")));
    TestEqual(TEXT("Cancelled preflight never publishes ready status"), static_cast<uint8>(Report.Status),
        static_cast<uint8>(SkiPreparation::ElevationCatalogStatus::CatalogFailure));
    TestFalse(TEXT("Cancelled preflight never enables download"), Report.DownloadEnabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogMalformedResponseTest,
    "MountainPlanner.M3.ElevationCatalog.MalformedResponseFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogMalformedResponseTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    Transport.Responses.Add(TEXT("{ this is not TNM JSON"));
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestFalse(TEXT("Malformed first response rejects preflight"), Catalog.Preflight(TestSite(), Cancellation, Report));
    TestFalse(TEXT("Malformed catalog never enables download"), Report.DownloadEnabled);
    TestEqual(TEXT("Malformed response has stable failure code"), Report.FailureCode,
        FString(TEXT("CATALOG_MALFORMED_JSON")));
    TestEqual(TEXT("Failure stops before querying lower tiers"), Transport.RequestedUrls.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogUnsupportedGeographyTest,
    "MountainPlanner.M3.ElevationCatalog.UnsupportedGeographyDisablesDownload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogUnsupportedGeographyTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    const SkiDomain::GeographicBounds France{2.30, 48.80, 2.32, 48.82};
    TestTrue(TEXT("Unsupported geography is a completed local preflight"),
        Catalog.Preflight(France, Cancellation, Report));
    TestFalse(TEXT("Unsupported geography is rejected"), Report.IsSupportedGeography);
    TestFalse(TEXT("Download is disabled outside supported US envelopes"), Report.DownloadEnabled);
    TestEqual(TEXT("Unsupported geography is identified"), Report.FailureCode,
        FString(TEXT("GEOGRAPHY_OUTSIDE_SUPPORTED_US_ENVELOPES")));
    TestEqual(TEXT("No network transport call occurs outside supported geography"),
        Transport.RequestedUrls.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogDatumRejectionTest,
    "MountainPlanner.M3.ElevationCatalog.DatumRejectionDisablesDownload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogDatumRejectionTest::RunTest(const FString&)
{
    FScriptedElevationCatalogTransport Transport;
    if (!InstallCogFixtures(*this, Transport)) return false;
    AddPage(Transport, {CatalogItem(TEXT("s1m-egm"), TEXT("EPSG:6350"), TEXT("EGM2008"),
        0, TEXT("2023-01-01"), TEXT("2024-01-01"), CogSizeForCrs(Transport, TEXT("EPSG:6350")))});
    AddPage(Transport, {});
    AddPage(Transport, {});
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::ElevationCatalog Catalog(Transport);
    SkiPreparation::TerrainAvailabilityReport Report;
    TestTrue(TEXT("Catalog response itself is valid"), Catalog.Preflight(TestSite(), Cancellation, Report));
    TestTrue(TEXT("Catalog can report footprint coverage without a safe source"), Report.HasElevationCoverage);
    TestFalse(TEXT("Unproven or non-NAVD88 datum rejects download"), Report.DownloadEnabled);
    TestEqual(TEXT("Datum failure has a stable reason"), Report.FailureCode,
        FString(TEXT("NO_SOURCE_PASSED_COG_AND_REQUIRED_METADATA_PROOFS")));
    TestEqual(TEXT("No datum-incompatible source is resolved"), Report.ResolvedSources.Num(), 0);
    TestEqual(TEXT("One source record remains visible for diagnosis"), Report.Sources.Num(), 1);
    if (!Report.Sources.IsEmpty())
    {
        TestEqual(TEXT("The source record explains the catalog/header conflict"), Report.Sources[0].EligibilityReasonCode,
            FString(TEXT("COG_PREFLIGHT_CATALOG_DATUM_UNSUPPORTED")));
        TestEqual(TEXT("The source COG receipt exposes the same reason"), Report.Sources[0].CogStatusCode,
            FString(TEXT("COG_PREFLIGHT_CATALOG_DATUM_UNSUPPORTED")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FElevationCatalogQualityHookTest,
    "MountainPlanner.M3.ElevationCatalog.QualityReportHook",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FElevationCatalogQualityHookTest::RunTest(const FString&)
{
    SkiDomain::TerrainProvenanceCounts Counts;
    Counts.S1MNativeQualified = 100;
    SkiDomain::TerrainQualityReport Report;
    TestTrue(TEXT("Quality hook delegates to the authoritative domain model"),
        SkiPreparation::ElevationCatalog::TryBuildQualityReport(Counts, Report));
    TestEqual(TEXT("100 percent qualified LOD0 is grade A"), static_cast<uint8>(Report.Grade),
        static_cast<uint8>(SkiDomain::TerrainGrade::A));
    return true;
}

#endif
