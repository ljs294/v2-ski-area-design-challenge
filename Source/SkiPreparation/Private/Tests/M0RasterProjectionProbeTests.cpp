#include "SkiPreparation/M0RasterProjectionProbe.h"
#include "SkiPreparation/GeoTiffDecoder.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Serialization/Archive.h"
#include "SQLiteDatabase.h"
#include "tiffio.h"

namespace
{
class FFileCogSource final : public SkiPreparation::ICogByteSource
{
public:
    explicit FFileCogSource(const FString& InPath) : Path(InPath)
    {
        Reader.Reset(IFileManager::Get().CreateFileReader(*Path));
    }
    uint64 Size() const noexcept override { return Reader ? Reader->TotalSize() : 0; }
    bool Read(uint64 Offset, uint64 Length, TArray<uint8>& Out, FString& Error) override
    {
        if (!Reader || Offset > Size() || Length > Size() - Offset || Length > MAX_int32)
        {
            Error = TEXT("FIXTURE_RANGE_INVALID"); return false;
        }
        Reader->Seek(Offset);
        Out.SetNumUninitialized(static_cast<int32>(Length));
        Reader->Serialize(Out.GetData(), Length);
        if (Reader->IsError()) { Error = TEXT("FIXTURE_READ_FAILED"); return false; }
        return true;
    }
private:
    FString Path;
    TUniquePtr<FArchive> Reader;
};

class FMemoryCogSource final : public SkiPreparation::ICogByteSource
{
public:
    explicit FMemoryCogSource(TArray<uint8> InBytes, int32 InFailAfterReads = MAX_int32)
        : Bytes(MoveTemp(InBytes)), FailAfterReads(InFailAfterReads) {}
    uint64 Size() const noexcept override { return Bytes.Num(); }
    bool Read(uint64 Offset, uint64 Length, TArray<uint8>& Out, FString& Error) override
    {
        if (Reads++ >= FailAfterReads)
        { Error = TEXT("OBJECT_CHANGED_BETWEEN_RANGES"); return false; }
        if (Offset > Size() || Length > Size() - Offset || Length > MAX_int32)
        { Error = TEXT("FIXTURE_RANGE_INVALID"); return false; }
        Out.SetNumUninitialized(static_cast<int32>(Length));
        FMemory::Memcpy(Out.GetData(), Bytes.GetData() + Offset, static_cast<SIZE_T>(Length));
        return true;
    }
private:
    TArray<uint8> Bytes;
    int32 Reads = 0;
    int32 FailAfterReads;
};

class FScriptedETagTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    FString FirstETag = TEXT("\"v1\"");
    FString LaterETag = TEXT("\"v1\"");
    int32 LaterStatus = 206;
    FString ObservedIfMatch;
    int32 Calls = 0;
    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>&) override
    {
        SkiPreparation::HttpAcquisitionResult Result;
        ObservedIfMatch = Request.IfMatchETag;
        Result.ETag = Calls++ == 0 ? FirstETag : LaterETag;
        Result.HttpStatus = Calls == 1 ? 206 : LaterStatus;
        if (Result.HttpStatus == 412)
        {
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            return Result;
        }
        if (!Request.ByteRange.IsSet()) return Result;
        const SkiPreparation::HttpByteRange Range = Request.ByteRange.GetValue();
        constexpr uint64 ObjectSize = 64;
        Result.ContentRange = FString::Printf(TEXT("bytes %llu-%llu/%llu"),
            Range.Offset, Range.Offset + Range.Length - 1, ObjectSize);
        Result.Bytes.Init(0x42, static_cast<int32>(Range.Length));
        return Result;
    }
};

bool WriteCogFixture(const FString& Path)
{
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, 32);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, 32);
    TIFFSetField(Image, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(Image, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(Image, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(Image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(Image, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(Image, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(Image, TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(Image, TIFFTAG_TILELENGTH, 16);
    TIFFSetField(Image, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(Image, TIFFTAG_PREDICTOR, 3);
    TIFFSetField(Image, TIFFTAG_GDAL_NODATA, 8, "-999999");
    float Tile[256];
    for (int32 TileY = 0; TileY < 2; ++TileY)
    for (int32 TileX = 0; TileX < 2; ++TileX)
    {
        for (int32 Y = 0; Y < 16; ++Y)
        for (int32 X = 0; X < 16; ++X)
        {
            const int32 GlobalX = TileX * 16 + X;
            const int32 GlobalY = TileY * 16 + Y;
            Tile[Y * 16 + X] = GlobalX == 0 && GlobalY == 0 ? -999999.0f : 1400.0f + GlobalX + GlobalY;
        }
        if (TIFFWriteEncodedTile(Image, TIFFComputeTile(Image, TileX * 16, TileY * 16, 0, 0),
            Tile, sizeof(Tile)) < 0) { TIFFClose(Image); return false; }
    }
    if (!TIFFWriteDirectory(Image)) { TIFFClose(Image); return false; }
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, 16);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, 16);
    TIFFSetField(Image, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(Image, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(Image, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(Image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(Image, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(Image, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(Image, TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(Image, TIFFTAG_TILELENGTH, 16);
    TIFFSetField(Image, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(Image, TIFFTAG_PREDICTOR, 3);
    TIFFSetField(Image, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
    for (int32 Index = 0; Index < 256; ++Index) Tile[Index] = 1400.0f + Index;
    if (TIFFWriteEncodedTile(Image, 0, Tile, sizeof(Tile)) < 0) { TIFFClose(Image); return false; }
    TIFFClose(Image);
    return true;
}

void AppendU32(TArray<uint8>& Bytes, uint32 Value)
{
    for (int32 I = 0; I < 4; ++I) Bytes.Add(static_cast<uint8>(Value >> (I * 8)));
}

void AppendF64(TArray<uint8>& Bytes, double Value)
{
    uint64 Bits = 0;
    FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
    for (int32 I = 0; I < 8; ++I) Bytes.Add(static_cast<uint8>(Bits >> (I * 8)));
}

TArray<uint8> MakeGpkgPolygon(bool bClosed = true)
{
    TArray<uint8> Blob = {'G', 'P', 0, 1}; // version 0, little endian, no envelope
    AppendU32(Blob, 6350);
    Blob.Add(1); // WKB little endian
    AppendU32(Blob, 3); // Polygon
    AppendU32(Blob, 1); // one exterior ring
    AppendU32(Blob, 5);
    const double Coordinates[5][2] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {bClosed ? 0.0 : 1.0, 0}};
    for (const auto& Point : Coordinates) { AppendF64(Blob, Point[0]); AppendF64(Blob, Point[1]); }
    return Blob;
}

bool CreateGpkgMetadata(FSQLiteDatabase& Db, int32 SourceSrs = 6350)
{
    return Db.Execute(TEXT("CREATE TABLE gpkg_contents (table_name TEXT PRIMARY KEY, data_type TEXT, srs_id INTEGER)"))
        && Db.Execute(TEXT("CREATE TABLE gpkg_geometry_columns (table_name TEXT PRIMARY KEY, column_name TEXT, geometry_type_name TEXT, srs_id INTEGER)"))
        && Db.Execute(*FString::Printf(TEXT("INSERT INTO gpkg_contents VALUES ('s1m_source_inputs','features',%d)"), SourceSrs))
        && Db.Execute(TEXT("INSERT INTO gpkg_contents VALUES ('s1m_blending_area_statistics','features',6350)"))
        && Db.Execute(TEXT("INSERT INTO gpkg_geometry_columns VALUES ('s1m_source_inputs','geom','POLYGON',6350)"))
        && Db.Execute(TEXT("INSERT INTO gpkg_geometry_columns VALUES ('s1m_blending_area_statistics','geom','POLYGON',6350)"));
}

bool InsertPolygon(FSQLiteDatabase& Db, const TCHAR* Sql, bool bClosed = true)
{
    FSQLitePreparedStatement Statement = Db.PrepareStatement(Sql);
    const TArray<uint8> Blob = MakeGpkgPolygon(bClosed);
    return Statement.IsValid() && Statement.SetBindingValueByIndex(1, Blob.GetData(), Blob.Num())
        && Statement.Execute();
}

bool WriteGeoPackageFixture(const FString& Path)
{
    FSQLiteDatabase Db;
    if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWriteCreate)) return false;
    const bool Ok = CreateGpkgMetadata(Db)
        && Db.Execute(TEXT("CREATE TABLE s1m_source_inputs (id INTEGER PRIMARY KEY, quality_level INTEGER, geom BLOB)"))
        && Db.Execute(TEXT("CREATE TABLE s1m_blending_area_statistics (id INTEGER PRIMARY KEY, geom BLOB)"))
        && InsertPolygon(Db, TEXT("INSERT INTO s1m_source_inputs VALUES (1, 2, ?)"))
        && InsertPolygon(Db, TEXT("INSERT INTO s1m_blending_area_statistics VALUES (1, ?)"));
    return Db.Close() && Ok;
}

bool WriteInvalidGeoPackageFixture(const FString& Path, bool bMissingGeometry)
{
    FSQLiteDatabase Db;
    if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWriteCreate)) return false;
    const bool Ok = CreateGpkgMetadata(Db) && (bMissingGeometry
        ? Db.Execute(TEXT("CREATE TABLE s1m_source_inputs (id INTEGER PRIMARY KEY, quality_level INTEGER)"))
          && Db.Execute(TEXT("INSERT INTO s1m_source_inputs VALUES (1, 2)"))
        : Db.Execute(TEXT("CREATE TABLE s1m_source_inputs (id INTEGER PRIMARY KEY, quality_level INTEGER, geom BLOB)")));
    const bool Complete = Ok
        && Db.Execute(TEXT("CREATE TABLE s1m_blending_area_statistics (id INTEGER PRIMARY KEY, geom BLOB)"));
    return Db.Close() && Complete;
}

bool WriteBadGeoPackageFixture(const FString& Path, bool bBadSrs)
{
    FSQLiteDatabase Db;
    if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWriteCreate)) return false;
    const bool Ok = CreateGpkgMetadata(Db, bBadSrs ? 4326 : 6350)
        && Db.Execute(TEXT("CREATE TABLE s1m_source_inputs (id INTEGER PRIMARY KEY, quality_level INTEGER, geom BLOB)"))
        && Db.Execute(TEXT("CREATE TABLE s1m_blending_area_statistics (id INTEGER PRIMARY KEY, geom BLOB)"))
        && InsertPolygon(Db, TEXT("INSERT INTO s1m_source_inputs VALUES (1, 2, ?)"), bBadSrs)
        && InsertPolygon(Db, TEXT("INSERT INTO s1m_blending_area_statistics VALUES (1, ?)"));
    return Db.Close() && Ok;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM0RasterProjectionFixtureTest,
    "MountainPlanner.M0.RasterProjection.SyntheticCogAndGeoPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FM0RasterProjectionFixtureTest::RunTest(const FString&)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("M0RasterProjectionFixture"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString CogPath = FPaths::Combine(Directory, TEXT("s1m-like.tif"));
    const FString GpkgPath = FPaths::Combine(Directory, TEXT("s1m-trimmed.gpkg"));
    IFileManager::Get().Delete(*CogPath);
    IFileManager::Get().Delete(*GpkgPath);
    TestTrue(TEXT("COG fixture write"), WriteCogFixture(CogPath));
    TestTrue(TEXT("GeoPackage fixture write"), WriteGeoPackageFixture(GpkgPath));
    FFileCogSource Source(CogPath);
    const SkiPreparation::FM0RasterProjectionReceipt Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, GpkgPath);
    TestTrue(TEXT("float LZW predictor 3 decodes"), Receipt.bCogPassed);
    TestEqual(TEXT("width"), Receipt.Width, 32u);
    TestEqual(TEXT("height"), Receipt.Height, 32u);
    TestEqual(TEXT("overview IFDs"), Receipt.OverviewCount, 1u);
    TestEqual(TEXT("nodata"), Receipt.NoDataSamples, 1ull);
    TestEqual(TEXT("valid"), Receipt.ValidSamples, 1023ull);
    TestTrue(TEXT("SQLiteCore reads both source tables"), Receipt.bGeoPackagePassed);
    TestEqual(TEXT("source rows"), Receipt.SourceInputRows, 1ll);
    TestEqual(TEXT("blend rows"), Receipt.BlendingRows, 1ll);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM0RasterProjectionNegativeTest,
    "MountainPlanner.M0.RasterProjection.NegativeInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FM0RasterProjectionNegativeTest::RunTest(const FString&)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("M0RasterProjectionNegative"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString CogPath = FPaths::Combine(Directory, TEXT("s1m-like.tif"));
    const FString GoodGpkg = FPaths::Combine(Directory, TEXT("good.gpkg"));
    const FString EmptyGpkg = FPaths::Combine(Directory, TEXT("empty-lineage.gpkg"));
    const FString NoGeomGpkg = FPaths::Combine(Directory, TEXT("missing-geometry.gpkg"));
    const FString BadSrsGpkg = FPaths::Combine(Directory, TEXT("wrong-srs.gpkg"));
    const FString BadWkbGpkg = FPaths::Combine(Directory, TEXT("bad-polygon.gpkg"));
    for (const FString& Path : {CogPath, GoodGpkg, EmptyGpkg, NoGeomGpkg, BadSrsGpkg, BadWkbGpkg}) IFileManager::Get().Delete(*Path);
    if (!TestTrue(TEXT("COG fixture"), WriteCogFixture(CogPath))
        || !TestTrue(TEXT("valid GPKG fixture"), WriteGeoPackageFixture(GoodGpkg))
        || !TestTrue(TEXT("empty lineage fixture"), WriteInvalidGeoPackageFixture(EmptyGpkg, false))
        || !TestTrue(TEXT("missing geometry fixture"), WriteInvalidGeoPackageFixture(NoGeomGpkg, true))
        || !TestTrue(TEXT("wrong SRS fixture"), WriteBadGeoPackageFixture(BadSrsGpkg, true))
        || !TestTrue(TEXT("bad WKB fixture"), WriteBadGeoPackageFixture(BadWkbGpkg, false))) return false;
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("load fixture"), FFileHelper::LoadFileToArray(Bytes, *CogPath))) return false;
    {
        TArray<uint8> Malformed = Bytes;
        // Classic TIFF bytes 4-7 point to the first IFD; force an out-of-object offset.
        for (int32 I = 4; I < 8; ++I) Malformed[I] = 0xff;
        FMemoryCogSource Source(MoveTemp(Malformed));
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, GoodGpkg);
        TestFalse(TEXT("out-of-object IFD rejected"), Receipt.bCogPassed);
    }
    {
        TArray<uint8> Truncated = Bytes;
        Truncated.SetNum(FMath::Min(256, Truncated.Num()));
        FMemoryCogSource Source(MoveTemp(Truncated));
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, GoodGpkg);
        TestFalse(TEXT("truncated COG rejected"), Receipt.bCogPassed);
    }
    {
        FMemoryCogSource Source(Bytes, 1);
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, GoodGpkg);
        TestFalse(TEXT("changed object / failed later range rejected"), Receipt.bCogPassed);
    }
    {
        FMemoryCogSource Source(Bytes);
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, EmptyGpkg);
        TestFalse(TEXT("empty source lineage rejected"), Receipt.bGeoPackagePassed);
        TestEqual(TEXT("empty source lineage code"), Receipt.GeoPackageError,
            FString(TEXT("GPKG_SOURCE_INPUTS_EMPTY")));
    }
    {
        FMemoryCogSource Source(MoveTemp(Bytes));
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, NoGeomGpkg);
        TestFalse(TEXT("missing geometry rejected"), Receipt.bGeoPackagePassed);
        TestTrue(TEXT("missing geometry reason"), Receipt.GeoPackageError.Contains(TEXT("GPKG_LINEAGE_GEOMETRY_MISSING")));
    }
    {
        FFileCogSource Source(CogPath);
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, BadSrsGpkg);
        TestFalse(TEXT("non-6350 metadata rejected"), Receipt.bGeoPackagePassed);
        TestTrue(TEXT("SRS failure code"), Receipt.GeoPackageError.Contains(TEXT("GPKG_FEATURE_METADATA_INVALID")));
    }
    {
        FFileCogSource Source(CogPath);
        const auto Receipt = SkiPreparation::RunM0RasterProjectionProbe(Source, BadWkbGpkg);
        TestFalse(TEXT("unclosed WKB ring rejected"), Receipt.bGeoPackagePassed);
        TestTrue(TEXT("WKB failure code"), Receipt.GeoPackageError.Contains(TEXT("GPKG_POLYGON_INVALID")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM0GatewayETagTest,
    "MountainPlanner.M0.RasterProjection.GatewayETagPinning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FM0GatewayETagTest::RunTest(const FString&)
{
    const FString Url = TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/test.tif");
    {
        FScriptedETagTransport Transport;
        SkiPreparation::FM0GatewayCogByteSource Source(Url, 1024, 4, &Transport);
        FString Error; TArray<uint8> Bytes;
        TestTrue(TEXT("strong initial ETag accepted"), Source.Initialize(Error));
        TestTrue(TEXT("stable ETag range accepted"), Source.Read(16, 8, Bytes, Error));
        TestEqual(TEXT("subsequent If-Match pinned"), Transport.ObservedIfMatch, FString(TEXT("\"v1\"")));
        TestEqual(TEXT("range bytes"), Bytes.Num(), 8);
    }
    {
        FScriptedETagTransport Transport;
        Transport.FirstETag = TEXT("W/\"weak\"");
        SkiPreparation::FM0GatewayCogByteSource Source(Url, 1024, 4, &Transport);
        FString Error;
        TestFalse(TEXT("weak initial ETag rejected"), Source.Initialize(Error));
        TestEqual(TEXT("weak ETag reason"), Error, FString(TEXT("GATEWAY_COG_STRONG_ETAG_REQUIRED")));
    }
    {
        FScriptedETagTransport Transport;
        Transport.LaterETag = TEXT("\"v2\"");
        SkiPreparation::FM0GatewayCogByteSource Source(Url, 1024, 4, &Transport);
        FString Error; TArray<uint8> Bytes;
        TestTrue(TEXT("changed-object setup"), Source.Initialize(Error));
        TestFalse(TEXT("changed response ETag rejected"), Source.Read(16, 8, Bytes, Error));
        TestEqual(TEXT("changed ETag reason"), Error, FString(TEXT("GATEWAY_COG_ETAG_CHANGED")));
        TestEqual(TEXT("changed object invalidates size"), Source.Size(), 0ull);
    }
    {
        FScriptedETagTransport Transport;
        Transport.LaterStatus = 412;
        SkiPreparation::FM0GatewayCogByteSource Source(Url, 1024, 4, &Transport);
        FString Error; TArray<uint8> Bytes;
        TestTrue(TEXT("precondition setup"), Source.Initialize(Error));
        TestFalse(TEXT("HTTP 412 rejected"), Source.Read(16, 8, Bytes, Error));
        TestEqual(TEXT("HTTP 412 reason"), Error, FString(TEXT("GATEWAY_COG_ETAG_PRECONDITION_FAILED")));
    }
    return true;
}

#if !UE_BUILD_SHIPPING
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM0GatewayCogIntegrationTest,
    "MountainPlanner.M0.RasterProjection.GatewayRangeDecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FM0GatewayCogIntegrationTest::RunTest(const FString&)
{
    FString CogUrl, CaPath, GpkgPath;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiM0CogUrl="), CogUrl)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiM0CogCA="), CaPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiM0GpkgPath="), GpkgPath))
    {
        AddWarning(TEXT("TLS COG harness arguments absent; packaged M0-B integration must invoke RunM0GatewayRasterProbe."));
        return true;
    }
    const SkiPreparation::FM0RasterProjectionReceipt Receipt =
        SkiPreparation::RunM0GatewayRasterProbe(CogUrl, CaPath, GpkgPath);
    TestTrue(TEXT("gateway ranged COG decode"), Receipt.bCogPassed);
    TestTrue(TEXT("multiple validated 206 ranges"), Receipt.GatewayRangeRequests > 1);
    TestEqual(TEXT("source cells"), Receipt.ValidSamples + Receipt.NoDataSamples,
        static_cast<uint64>(Receipt.Width) * Receipt.Height);
    return true;
}
#endif
#endif
