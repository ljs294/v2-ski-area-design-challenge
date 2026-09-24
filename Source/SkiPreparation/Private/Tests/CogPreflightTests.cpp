#include "SkiPreparation/CogPreflight.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "tiffio.h"

#include <cstdint>

namespace
{
constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;

uint16 ReadU16(const TArray<uint8>& Bytes, const int32 Offset)
{
    return static_cast<uint16>(Bytes[Offset]) | (static_cast<uint16>(Bytes[Offset + 1]) << 8);
}

uint32 ReadU32(const TArray<uint8>& Bytes, const int32 Offset)
{
    return static_cast<uint32>(Bytes[Offset]) | (static_cast<uint32>(Bytes[Offset + 1]) << 8)
        | (static_cast<uint32>(Bytes[Offset + 2]) << 16) | (static_cast<uint32>(Bytes[Offset + 3]) << 24);
}

void WriteU16(TArray<uint8>& Bytes, const int32 Offset, const uint16 Value)
{
    Bytes[Offset] = static_cast<uint8>(Value);
    Bytes[Offset + 1] = static_cast<uint8>(Value >> 8);
}

void WriteU32(TArray<uint8>& Bytes, const int32 Offset, const uint32 Value)
{
    for (int32 Index = 0; Index < 4; ++Index) Bytes[Offset + Index] = static_cast<uint8>(Value >> (Index * 8));
}

bool MutateUnsignedEntryValue(TArray<uint8>& Bytes, const int32 Entry, const uint16 Type,
    const uint32 Count, const uint32 Value)
{
    const uint32 ElementBytes = Type == TIFF_SHORT ? 2U : Type == TIFF_LONG ? 4U : 0U;
    if (ElementBytes == 0 || (ElementBytes == 2 && Value > MAX_uint16) || Count == 0)
        return false;
    const uint64 ArrayBytes = static_cast<uint64>(Count) * ElementBytes;
    const int32 ValuesOffset = ArrayBytes <= 4 ? Entry + 8 : static_cast<int32>(ReadU32(Bytes, Entry + 8));
    if (ValuesOffset < 0 || static_cast<uint64>(ValuesOffset) + ElementBytes > static_cast<uint64>(Bytes.Num()))
        return false;
    if (ElementBytes == 2) WriteU16(Bytes, ValuesOffset, static_cast<uint16>(Value));
    else WriteU32(Bytes, ValuesOffset, Value);
    return true;
}

bool MutateFirstIfdEntry(TArray<uint8>& Bytes, const uint16 Tag,
    const TFunction<bool(TArray<uint8>&, int32, uint16, uint32)>& Mutator)
{
    if (Bytes.Num() < 8 || Bytes[0] != 'I' || Bytes[1] != 'I' || ReadU16(Bytes, 2) != 42) return false;
    const uint32 IfdOffset = ReadU32(Bytes, 4);
    if (IfdOffset > static_cast<uint32>(Bytes.Num()) || static_cast<uint64>(IfdOffset) + 2 > static_cast<uint64>(Bytes.Num())) return false;
    const uint16 EntryCount = ReadU16(Bytes, static_cast<int32>(IfdOffset));
    const uint64 End = static_cast<uint64>(IfdOffset) + 2 + static_cast<uint64>(EntryCount) * 12;
    if (End > static_cast<uint64>(Bytes.Num())) return false;
    for (uint16 Index = 0; Index < EntryCount; ++Index)
    {
        const int32 Entry = static_cast<int32>(IfdOffset + 2 + static_cast<uint32>(Index) * 12);
        if (ReadU16(Bytes, Entry) != Tag) continue;
        return Mutator(Bytes, Entry, ReadU16(Bytes, Entry + 2), ReadU32(Bytes, Entry + 4));
    }
    return false;
}

bool MutateFirstTileOffset(TArray<uint8>& Bytes, const uint32 Value)
{
    return MutateFirstIfdEntry(Bytes, TIFFTAG_TILEOFFSETS,
        [Value](TArray<uint8>& Data, const int32 Entry, const uint16 Type, const uint32 Count)
        {
            return MutateUnsignedEntryValue(Data, Entry, Type, Count, Value);
        });
}

bool MutateFirstTileByteCount(TArray<uint8>& Bytes, const uint32 Value)
{
    return MutateFirstIfdEntry(Bytes, TIFFTAG_TILEBYTECOUNTS,
        [Value](TArray<uint8>& Data, const int32 Entry, const uint16 Type, const uint32 Count)
        {
            return MutateUnsignedEntryValue(Data, Entry, Type, Count, Value);
        });
}

bool MutateCompression(TArray<uint8>& Bytes, const uint16 Value)
{
    return MutateFirstIfdEntry(Bytes, TIFFTAG_COMPRESSION,
        [Value](TArray<uint8>& Data, const int32 Entry, const uint16 Type, const uint32 Count)
        {
            if (Type != TIFF_SHORT || Count != 1) return false;
            WriteU16(Data, Entry + 8, Value);
            return true;
        });
}

bool MutateImageWidth(TArray<uint8>& Bytes, const uint32 Value)
{
    return MutateFirstIfdEntry(Bytes, TIFFTAG_IMAGEWIDTH,
        [Value](TArray<uint8>& Data, const int32 Entry, const uint16 Type, const uint32 Count)
        {
            return Count == 1 && MutateUnsignedEntryValue(Data, Entry, Type, Count, Value);
        });
}

bool WriteDirectory(TIFF* Image, const uint32 Width, const uint32 Height, const bool bOverview,
    const uint32 Epsg, const bool bIncludeNavd88)
{
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, Width);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, Height);
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
    if (bOverview) TIFFSetField(Image, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
    if (!bOverview)
    {
        const double Scale[] = {1.0, 1.0, 0.0};
        const double Tie[] = {0.0, 0.0, 0.0, 500000.0, 5000000.0, 0.0};
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
        // The optional NAVD88 keys are explicit GeoTIFF vertical CRS/datum proof.
        TIFFSetField(Image, GeoKeyDirectoryTag, static_cast<uint32>(Keys.Num()), Keys.GetData());
        TIFFSetField(Image, TIFFTAG_GDAL_NODATA, 8U, const_cast<char*>("-999999"));
    }

    TArray<float> Tile;
    Tile.SetNumUninitialized(16 * 16);
    const uint32 TilesX = FMath::DivideAndRoundUp(Width, 16U);
    const uint32 TilesY = FMath::DivideAndRoundUp(Height, 16U);
    for (uint32 TileY = 0; TileY < TilesY; ++TileY)
    {
        for (uint32 TileX = 0; TileX < TilesX; ++TileX)
        {
            for (int32 Index = 0; Index < Tile.Num(); ++Index)
                Tile[Index] = static_cast<float>(1200 + TileX * 17 + TileY * 31 + Index);
            if (TIFFWriteEncodedTile(Image, TIFFComputeTile(Image, TileX * 16, TileY * 16, 0, 0),
                Tile.GetData(), static_cast<tmsize_t>(Tile.Num() * sizeof(float))) < 0)
                return false;
        }
    }
    return TIFFWriteDirectory(Image) != 0;
}

bool WriteCogFixture(const FString& Path, const uint32 Epsg = 6350, const bool bIncludeNavd88 = true)
{
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    const bool bBase = WriteDirectory(Image, 256, 256, false, Epsg, bIncludeNavd88);
    const bool bOverview = bBase && WriteDirectory(Image, 128, 128, true, Epsg, false);
    TIFFClose(Image);
    return bBase && bOverview;
}

class FScriptedCogTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    enum class EMutation : uint8 { None, BadRangeStart, BadRangeTotal, MissingETag, ChangeETag, BadReceivedLength, PreconditionFailed };
    explicit FScriptedCogTransport(TArray<uint8> InObject) : Object(MoveTemp(InObject)) {}

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>&) override
    {
        Requests.Add(Request);
        SkiPreparation::HttpAcquisitionResult Result;
        Result.HttpStatus = 206;
        Result.FailureReason = SkiPreparation::TransportFailureReason::None;
        const uint64 Index = Requests.Num() - 1;
        Result.ETag = Index == 0 || Mutation != EMutation::ChangeETag ? TEXT("\"fixture-v1\"") : TEXT("\"fixture-v2\"");
        if (Mutation == EMutation::MissingETag) Result.ETag.Empty();
        if (Mutation == EMutation::PreconditionFailed && Index > 0)
        {
            Result.HttpStatus = 412;
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            return Result;
        }
        if (!Request.ByteRange.IsSet())
        {
            Result.HttpStatus = 400;
            Result.FailureReason = SkiPreparation::TransportFailureReason::Other;
            return Result;
        }
        const SkiPreparation::HttpByteRange Range = Request.ByteRange.GetValue();
        if (Range.Offset > static_cast<uint64>(Object.Num())
            || Range.Length > static_cast<uint64>(Object.Num()) - Range.Offset)
        {
            Result.HttpStatus = 416;
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            return Result;
        }
        Result.Bytes.Append(Object.GetData() + Range.Offset, static_cast<int32>(Range.Length));
        Result.BytesReceived = Result.Bytes.Num();
        uint64 Start = Range.Offset;
        uint64 End = Range.Offset + Range.Length - 1;
        uint64 Total = Object.Num();
        if (Mutation == EMutation::BadRangeStart) ++Start;
        if (Mutation == EMutation::BadRangeTotal && Index > 0) ++Total;
        Result.ContentRange = FString::Printf(TEXT("bytes %llu-%llu/%llu"), Start, End, Total);
        if (Mutation == EMutation::BadReceivedLength) ++Result.BytesReceived;
        return Result;
    }

    TArray<uint8> Object;
    TArray<SkiPreparation::HttpAcquisitionRequest> Requests;
    EMutation Mutation = EMutation::None;
};

SkiPreparation::FCogPreflightLimits TestLimits()
{
    SkiPreparation::FCogPreflightLimits Limits;
    Limits.MaxObjectBytes = 8ULL * 1024ULL * 1024ULL;
    Limits.MaxTransferredBytes = 8ULL * 1024ULL * 1024ULL;
    Limits.MaxRequests = 512;
    Limits.MaxRangeBytes = 4096;
    Limits.MaxCachedBytes = 16384;
    Limits.RangeBlockBytes = 4096;
    Limits.MaxDimension = 512;
    Limits.MaxDirectories = 8;
    Limits.MaxTilesPerDirectory = 2048;
    Limits.MaxTotalTiles = 4096;
    Limits.ExpectedTileWidth = 16;
    Limits.ExpectedTileHeight = 16;
    return Limits;
}

SkiPreparation::FCogPreflightMetadata S1MMetadata()
{
    SkiPreparation::FCogPreflightMetadata Metadata;
    Metadata.CatalogHorizontalCrs = TEXT("EPSG:6350");
    Metadata.CatalogVerticalDatum = TEXT("NAVD88");
    return Metadata;
}

bool RunPreflight(FScriptedCogTransport& Transport, const SkiPreparation::FCogPreflightMetadata& Metadata,
    SkiPreparation::FCogPreflightReport& Report)
{
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    return SkiPreparation::PreflightElevationCog(Transport,
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/fixture.tif"),
        SkiDomain::ElevationProduct::S1M, Metadata, TestLimits(), Cancellation, Report);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCogPreflightValidTest,
    "MountainPlanner.M3.CogPreflight.ValidS1MHeaderAndRanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCogPreflightValidTest::RunTest(const FString&)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CogPreflightFixtures"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Path = FPaths::Combine(Directory, TEXT("s1m-preflight.tif"));
    IFileManager::Get().Delete(*Path);
    TestTrue(TEXT("write synthetic tiled float32 COG"), WriteCogFixture(Path));
    TArray<uint8> Bytes;
    TestTrue(TEXT("load synthetic COG"), FFileHelper::LoadFileToArray(Bytes, *Path));
    IFileManager::Get().Delete(*Path);

    FScriptedCogTransport Transport(MoveTemp(Bytes));
    SkiPreparation::FCogPreflightReport Report;
    TestTrue(TEXT("preflight accepted"), RunPreflight(Transport, S1MMetadata(), Report));
    TestTrue(TEXT("report is passed"), Report.bPassed);
    TestEqual(TEXT("horizontal EPSG proof"), Report.HorizontalCrs, FString(TEXT("EPSG:6350")));
    TestEqual(TEXT("vertical NAVD88 proof"), Report.VerticalDatum, FString(TEXT("NAVD88")));
    TestEqual(TEXT("vertical proof origin"), Report.VerticalDatumOrigin, FString(TEXT("GeoTIFF GeoKey")));
    TestEqual(TEXT("nodata"), Report.NoDataText, FString(TEXT("-999999")));
    TestEqual(TEXT("base width"), Report.Width, 256U);
    TestEqual(TEXT("base height"), Report.Height, 256U);
    TestEqual(TEXT("overview count"), Report.Directories.Num(), 2);
    TestTrue(TEXT("strong ETag pinned"), Report.bStrongETagPinned);
    TestTrue(TEXT("request budget used"), Report.Requests > 0 && Report.Requests <= 512);
    TestTrue(TEXT("transfer budget used"), Report.TransferredBytes <= TestLimits().MaxTransferredBytes);
    for (int32 Index = 1; Index < Transport.Requests.Num(); ++Index)
        TestEqual(TEXT("subsequent ranges use If-Match"), Transport.Requests[Index].IfMatchETag,
            FString(TEXT("\"fixture-v1\"")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCogPreflightRejectsMutationsTest,
    "MountainPlanner.M3.CogPreflight.FailClosedMetadataAndRangeMutations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCogPreflightRejectsMutationsTest::RunTest(const FString&)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CogPreflightFixtures"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Path = FPaths::Combine(Directory, TEXT("s1m-mutated.tif"));
    IFileManager::Get().Delete(*Path);
    TestTrue(TEXT("write mutation source"), WriteCogFixture(Path));
    TArray<uint8> Fixture;
    TestTrue(TEXT("load mutation source"), FFileHelper::LoadFileToArray(Fixture, *Path));
    IFileManager::Get().Delete(*Path);

    {
        FScriptedCogTransport Transport(Fixture);
        Transport.Mutation = FScriptedCogTransport::EMutation::BadRangeStart;
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("wrong range start rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("wrong range reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_CONTENT_RANGE_INVALID")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        Transport.Mutation = FScriptedCogTransport::EMutation::BadRangeTotal;
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("changed range total rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("changed total reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_OBJECT_SIZE_CHANGED")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        Transport.Mutation = FScriptedCogTransport::EMutation::MissingETag;
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("missing ETag rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("missing ETag reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_STRONG_ETAG_REQUIRED")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        Transport.Mutation = FScriptedCogTransport::EMutation::ChangeETag;
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("changed ETag rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("changed ETag reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_ETAG_CHANGED")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        Transport.Mutation = FScriptedCogTransport::EMutation::PreconditionFailed;
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("412 rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("412 reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_ETAG_PRECONDITION_FAILED")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        Transport.Mutation = FScriptedCogTransport::EMutation::BadReceivedLength;
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("received length mismatch rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("length reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_RANGE_LENGTH_INVALID")));
    }
    {
        TArray<uint8> Mutated = Fixture;
        WriteU32(Mutated, 4, 0xfffffff0U);
        FScriptedCogTransport Transport(MoveTemp(Mutated));
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("out-of-file first IFD rejected"), RunPreflight(Transport, S1MMetadata(), Report));
    }
    {
        TArray<uint8> Mutated = Fixture;
        TestTrue(TEXT("ImageWidth fixture entry is found and mutated"), MutateImageWidth(Mutated, 65535U));
        FScriptedCogTransport Transport(MoveTemp(Mutated));
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("oversize dimensions rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("dimensions reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_ENCODING_UNSUPPORTED")));
    }
    {
        TArray<uint8> Mutated = Fixture;
        TestTrue(TEXT("mutate tile offset"), MutateFirstTileOffset(Mutated, 0xfffffff0U));
        FScriptedCogTransport Transport(MoveTemp(Mutated));
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("out-of-file tile offset rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("offset reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_TILE_RANGE_INVALID")));
    }
    {
        TArray<uint8> Mutated = Fixture;
        TestTrue(TEXT("TileByteCounts fixture entry is found and mutated"), MutateFirstTileByteCount(Mutated, 0));
        FScriptedCogTransport Transport(MoveTemp(Mutated));
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("zero tile bytecount rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("bytecount reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_TILE_RANGE_INVALID")));
    }
    {
        TArray<uint8> Mutated = Fixture;
        TestTrue(TEXT("mutate codec"), MutateCompression(Mutated, COMPRESSION_JPEG));
        FScriptedCogTransport Transport(MoveTemp(Mutated));
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("unsupported compression rejected"), RunPreflight(Transport, S1MMetadata(), Report));
        TestEqual(TEXT("compression reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_ENCODING_UNSUPPORTED")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        SkiPreparation::FCogPreflightMetadata WrongCrs = S1MMetadata();
        WrongCrs.CatalogHorizontalCrs = TEXT("EPSG:26910");
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("catalog CRS mismatch rejected"), RunPreflight(Transport, WrongCrs, Report));
        TestEqual(TEXT("CRS mismatch reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_CATALOG_CRS_MISMATCH")));
    }
    {
        FScriptedCogTransport Transport(Fixture);
        SkiPreparation::FCogPreflightLimits Limits = TestLimits();
        Limits.MaxTransferredBytes = 32;
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("transfer budget enforced before excess range"), SkiPreparation::PreflightElevationCog(Transport,
            TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/fixture.tif"),
            SkiDomain::ElevationProduct::S1M, S1MMetadata(), Limits, Cancellation, Report));
        TestEqual(TEXT("budget reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_BUDGET_EXCEEDED")));
        TestEqual(TEXT("only initial probe sent"), Transport.Requests.Num(), 1);
    }
    {
        const FString NoVerticalPath = FPaths::Combine(Directory, TEXT("project-no-vertical.tif"));
        IFileManager::Get().Delete(*NoVerticalPath);
        TestTrue(TEXT("write project COG without vertical GeoKey"), WriteCogFixture(NoVerticalPath, 26910, false));
        TArray<uint8> NoVerticalBytes;
        TestTrue(TEXT("load project COG"), FFileHelper::LoadFileToArray(NoVerticalBytes, *NoVerticalPath));
        IFileManager::Get().Delete(*NoVerticalPath);
        FScriptedCogTransport Transport(MoveTemp(NoVerticalBytes));
        SkiPreparation::FCogPreflightMetadata Metadata;
        Metadata.CatalogHorizontalCrs = TEXT("EPSG:26910");
        Metadata.CatalogVerticalDatum = TEXT("NAVD88");
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::FCogPreflightLimits Limits = TestLimits();
        SkiPreparation::FCogPreflightReport Report;
        TestTrue(TEXT("project COG uses external datum proof"), SkiPreparation::PreflightElevationCog(Transport,
            TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/Project1m/fixture.tif"),
            SkiDomain::ElevationProduct::Project1m, Metadata, Limits, Cancellation, Report));
        TestEqual(TEXT("external datum origin"), Report.VerticalDatumOrigin, FString(TEXT("catalog metadata")));
    }
    {
        const FString NoVerticalPath = FPaths::Combine(Directory, TEXT("project-unproven.tif"));
        IFileManager::Get().Delete(*NoVerticalPath);
        TestTrue(TEXT("write unproven project COG"), WriteCogFixture(NoVerticalPath, 26910, false));
        TArray<uint8> NoVerticalBytes;
        TestTrue(TEXT("load unproven project COG"), FFileHelper::LoadFileToArray(NoVerticalBytes, *NoVerticalPath));
        IFileManager::Get().Delete(*NoVerticalPath);
        FScriptedCogTransport Transport(MoveTemp(NoVerticalBytes));
        SkiPreparation::FCogPreflightMetadata Metadata;
        Metadata.CatalogHorizontalCrs = TEXT("EPSG:26910");
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::FCogPreflightLimits Limits = TestLimits();
        SkiPreparation::FCogPreflightReport Report;
        TestFalse(TEXT("unproven Project1m datum rejected"), SkiPreparation::PreflightElevationCog(Transport,
            TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/Project1m/fixture.tif"),
            SkiDomain::ElevationProduct::Project1m, Metadata, Limits, Cancellation, Report));
        TestEqual(TEXT("datum proof reason"), Report.FailureCode, FString(TEXT("COG_PREFLIGHT_DATUM_UNPROVEN")));
    }
    return true;
}
