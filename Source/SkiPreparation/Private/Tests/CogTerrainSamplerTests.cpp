#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiPreparation/CogTerrainSampler.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "tiffio.h"

namespace
{
constexpr uint32 GeoKeyDirectoryTag = 34735;
constexpr uint32 ModelPixelScaleTag = 33550;
constexpr uint32 ModelTiepointTag = 33922;

bool WriteDirectory(TIFF* Image, const uint32 Width, const uint32 Height,
    const bool bOverview, const bool bPrimaryHasNoData, const uint16 HorizontalEpsg,
    const bool bIncludeHeaderNavd88)
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
        const uint16 GeoKeyCount = bIncludeHeaderNavd88 ? 5 : 3;
        TArray<uint16> Keys;
        Keys.Add(1); Keys.Add(1); Keys.Add(0); Keys.Add(GeoKeyCount);
        const auto AddKey = [&Keys](const uint16 Key, const uint16 Value)
        {
            Keys.Add(Key); Keys.Add(0); Keys.Add(1); Keys.Add(Value);
        };
        AddKey(1024, 1);
        AddKey(1025, 1);
        AddKey(3072, HorizontalEpsg);
        if (bIncludeHeaderNavd88)
        {
            AddKey(4096, 5703);
            AddKey(4098, 5103);
        }
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
            for (uint32 Row = 0; Row < 16; ++Row)
            {
                for (uint32 Column = 0; Column < 16; ++Column)
                {
                    const uint32 SourceRow = TileY * 16 + Row;
                    const uint32 SourceColumn = TileX * 16 + Column;
                    const int32 Index = static_cast<int32>(Row * 16 + Column);
                    const bool bNoData = bPrimaryHasNoData && SourceColumn <= 2 && SourceRow <= 2;
                    Tile[Index] = bNoData ? -999999.0F
                        : static_cast<float>((bPrimaryHasNoData ? 100.0 : 900.0)
                            + SourceColumn + 2.0 * SourceRow);
                }
            }
            if (TIFFWriteEncodedTile(Image, TIFFComputeTile(Image, TileX * 16, TileY * 16, 0, 0),
                Tile.GetData(), static_cast<tmsize_t>(Tile.Num() * sizeof(float))) < 0)
                return false;
        }
    }
    return TIFFWriteDirectory(Image) != 0;
}

bool WriteCogFixture(const FString& Path, const bool bPrimaryHasNoData,
    const uint16 HorizontalEpsg, const bool bIncludeHeaderNavd88)
{
    SkiPreparation::EnsureGeoTiffTagsRegistered();
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    const bool bBase = WriteDirectory(Image, 32, 32, false, bPrimaryHasNoData,
        HorizontalEpsg, bIncludeHeaderNavd88);
    const bool bOverview = bBase && WriteDirectory(Image, 16, 16, true, false,
        HorizontalEpsg, bIncludeHeaderNavd88);
    TIFFClose(Image);
    return bBase && bOverview;
}

FString FixtureRoot(const TCHAR* Name)
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

bool LoadCogFixture(const TCHAR* Name, const bool bPrimaryHasNoData, TArray<uint8>& OutBytes,
    const uint16 HorizontalEpsg = 6350, const bool bIncludeHeaderNavd88 = true)
{
    const FString Directory = FixtureRoot(Name);
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Path = FPaths::Combine(Directory, TEXT("fixture.tif"));
    IFileManager::Get().Delete(*Path);
    const bool bWrote = WriteCogFixture(Path, bPrimaryHasNoData,
        HorizontalEpsg, bIncludeHeaderNavd88);
    const bool bLoaded = bWrote && FFileHelper::LoadFileToArray(OutBytes, *Path);
    IFileManager::Get().Delete(*Path);
    IFileManager::Get().DeleteDirectory(*Directory, false, true);
    return bLoaded;
}

class FScriptedSamplerTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    enum class EMutation : uint8 { None, ChangedETag, BadContentRange, ShortBody, PreconditionFailed };

    FScriptedSamplerTransport(TArray<uint8> InPrimary, TArray<uint8> InFallback = {})
        : Primary(MoveTemp(InPrimary)), Fallback(MoveTemp(InFallback)) {}

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>&) override
    {
        const int32 CallIndex = Requests.Num();
        Requests.Add(Request);
        SkiPreparation::HttpAcquisitionResult Result;
        Result.HttpStatus = 206;
        Result.FailureReason = SkiPreparation::TransportFailureReason::None;
        Result.ETag = TEXT("\"sampler-v1\"");
        const TArray<uint8>& Object = Request.Url.Contains(TEXT("fallback")) ? Fallback : Primary;
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
        const uint64 End = Range.Offset + Range.Length - 1;
        const uint64 Total = Object.Num();
        if (CallIndex >= MutateAtCall)
        {
            if (Mutation == EMutation::ChangedETag) Result.ETag = TEXT("\"sampler-v2\"");
            if (Mutation == EMutation::BadContentRange) ++Start;
            if (Mutation == EMutation::ShortBody && !Result.Bytes.IsEmpty())
            {
                Result.Bytes.Pop(EAllowShrinking::No);
                Result.BytesReceived = Result.Bytes.Num();
            }
            if (Mutation == EMutation::PreconditionFailed)
            {
                Result.Bytes.Reset();
                Result.BytesReceived = 0;
                Result.HttpStatus = 412;
                Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
                return Result;
            }
        }
        Result.ContentRange = FString::Printf(TEXT("bytes %llu-%llu/%llu"), Start, End, Total);
        return Result;
    }

    TArray<SkiPreparation::HttpAcquisitionRequest> Requests;
    EMutation Mutation = EMutation::None;
    int32 MutateAtCall = MAX_int32;

private:
    TArray<uint8> Primary;
    TArray<uint8> Fallback;
};

SkiPreparation::FCogTerrainSamplerLimits TestLimits()
{
    SkiPreparation::FCogTerrainSamplerLimits Limits;
    Limits.MaxRequests = 4096;
    Limits.MaxTransferredBytes = 8ULL * 1024ULL * 1024ULL;
    Limits.MaxResidentBytes = 16ULL * 1024ULL * 1024ULL;
    Limits.MaxEncodedTileBytes = 64ULL * 1024ULL;
    Limits.MaxDecodedTileBytes = 64ULL * 1024ULL;
    Limits.Preflight.MaxObjectBytes = 4ULL * 1024ULL * 1024ULL;
    Limits.Preflight.MaxTransferredBytes = 4ULL * 1024ULL * 1024ULL;
    Limits.Preflight.MaxRequests = 4096;
    Limits.Preflight.MaxRangeBytes = 4096;
    Limits.Preflight.MaxCachedBytes = 8192;
    Limits.Preflight.RangeBlockBytes = 256;
    Limits.Preflight.MaxDimension = 512;
    Limits.Preflight.MaxDirectories = 4;
    Limits.Preflight.MaxTilesPerDirectory = 1024;
    Limits.Preflight.MaxTotalTiles = 2048;
    Limits.Preflight.ExpectedTileWidth = 16;
    Limits.Preflight.ExpectedTileHeight = 16;
    return Limits;
}

SkiPreparation::FCogTerrainSamplerSource MakePrimarySource()
{
    SkiPreparation::FCogTerrainSamplerSource Source;
    Source.Url = TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/sampler.tif");
    Source.Product = SkiDomain::ElevationProduct::S1M;
    Source.Metadata.CatalogHorizontalCrs = TEXT("EPSG:6350");
    Source.Metadata.CatalogVerticalDatum = TEXT("NAVD88");
    Source.ScratchSourceIndex = 0;
    Source.MapTargetToSourcePixel = [](uint32 Column, uint32 Row, double& X, double& Y)
    {
        if (Column == 2 && Row == 0) return false;
        X = 1.25 + 2.0 * Column;
        Y = 1.25 + 2.0 * Row;
        return true;
    };
    Source.ResolveS1mProvenance = [](double X, double Y,
        SkiPreparation::TerrainScratchProvenance& Out)
    {
        Out = X >= 3.0 || Y >= 3.0
            ? SkiPreparation::TerrainScratchProvenance::S1MBlend
            : SkiPreparation::TerrainScratchProvenance::S1MNative;
        return true;
    };
    return Source;
}

SkiPreparation::FCogTerrainSamplerSource MakeFallbackSource()
{
    SkiPreparation::FCogTerrainSamplerSource Source;
    Source.Url = TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/Project1m/fallback-sampler.tif");
    Source.Product = SkiDomain::ElevationProduct::Project1m;
    Source.Metadata.CatalogHorizontalCrs = TEXT("EPSG:26910");
    Source.Metadata.CatalogVerticalDatum = TEXT("NAVD88");
    Source.ScratchSourceIndex = 1;
    Source.MapTargetToSourcePixel = [](uint32 Column, uint32 Row, double& X, double& Y)
    {
        if (Column == 2 && Row == 0) return false;
        X = 1.25 + 2.0 * Column;
        Y = 1.25 + 2.0 * Row;
        return true;
    };
    return Source;
}

bool BuildScratch(SkiPreparation::TerrainScratchStore& Store, const FString& Root,
    const bool bIncludeFallback, FString& OutError)
{
    TArray<SkiDomain::TerrainCoreSource> Sources;
    SkiDomain::TerrainCoreSource Primary;
    Primary.SourceId = "sampler-s1m";
    Primary.Product = "S1M";
    Sources.Add(Primary);
    if (bIncludeFallback)
    {
        SkiDomain::TerrainCoreSource Fallback;
        Fallback.SourceId = "sampler-project";
        Fallback.Product = "Project1m";
        Sources.Add(Fallback);
    }
    uint64 Required = 0;
    if (!SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(3, 3, Required)) return false;
    return Store.Create(Root, 3, 3, 1.0, 1.0, Sources, Required, OutError);
}

bool ReadSample(SkiPreparation::TerrainScratchStore& Store, const uint32 Column, const uint32 Row,
    SkiPreparation::TerrainScratchLodTile& OutTile, int32& OutIndex, FString& OutError)
{
    SkiDomain::TerrainCoreTilePlan Plan;
    if (!SkiDomain::PlanTerrainCoreTiles(Store.Width(), Store.Height(), Plan)) return false;
    for (const SkiDomain::TerrainCoreTileDescriptor& Descriptor : Plan.Tiles)
    {
        if (Descriptor.LodIndex != 0) continue;
        if (!Store.ReadLodTile(Descriptor, OutTile, OutError)) return false;
        OutIndex = static_cast<int32>(static_cast<uint64>(Row + Descriptor.HaloNorth) * OutTile.Width
            + Column + Descriptor.HaloWest);
        return OutIndex >= 0 && OutIndex < OutTile.Heights.Num();
    }
    return false;
}

uint64 CountPreflightRequests(const TArray<uint8>& Fixture,
    const SkiPreparation::FCogTerrainSamplerLimits& Limits)
{
    FScriptedSamplerTransport Transport(Fixture);
    SkiPreparation::FCogPreflightReport Report;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::PreflightElevationCog(Transport,
        TEXT("https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/S1M/sampler.tif"),
        SkiDomain::ElevationProduct::S1M, MakePrimarySource().Metadata,
        Limits.Preflight, Cancellation, Report);
    return Report.bPassed ? static_cast<uint64>(Transport.Requests.Num()) : 0;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCogTerrainSamplerFallbackProvenanceAndNoDataTest,
    "MountainPlanner.M4.CogTerrainSampler.FallbackPerSampleProvenanceAndNoData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCogTerrainSamplerFallbackProvenanceAndNoDataTest::RunTest(const FString&)
{
    TArray<uint8> PrimaryBytes, FallbackBytes;
    TestTrue(TEXT("primary S1M COG fixture is written"), LoadCogFixture(TEXT("CogTerrainSamplerPrimary"), true, PrimaryBytes));
    TestTrue(TEXT("fallback Project1m COG fixture is written"), LoadCogFixture(
        TEXT("CogTerrainSamplerFallback"), false, FallbackBytes, 26910, false));
    if (PrimaryBytes.IsEmpty() || FallbackBytes.IsEmpty()) return false;

    const FString Root = FixtureRoot(TEXT("CogTerrainSamplerScratch"));
    SkiPreparation::TerrainScratchStore Scratch;
    FString Error;
    if (!BuildScratch(Scratch, Root, true, Error))
    {
        TestFalse(TEXT("canonical 1 m scratch store is created"), true);
        return false;
    }

    FScriptedSamplerTransport Transport(MoveTemp(PrimaryBytes), MoveTemp(FallbackBytes));
    TArray<SkiPreparation::FCogTerrainSamplerSource> Sources;
    Sources.Add(MakePrimarySource());
    Sources.Add(MakeFallbackSource());
    SkiPreparation::FCogTerrainSamplerReport Report;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    const bool bSampled = SkiPreparation::SampleVerifiedElevationCogsToScratch(
        Transport, Sources, Scratch, TestLimits(), Cancellation, Report);
    TestTrue(*FString::Printf(TEXT("verified range sampling succeeds: %s (%s); requests=%llu bytes=%llu"),
        *Report.FailureCode, *Report.FailureDetail, Report.Requests, Report.TransferredBytes), bSampled);
    if (!bSampled)
    {
        Scratch.Cleanup(Error);
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }
    TestTrue(*FString::Printf(TEXT("sampler finalized: %s %s"), *Report.FailureCode, *Report.FailureDetail), Report.bPassed);
    TestEqual(TEXT("all target cells are counted"), Report.TotalSamples, uint64(9));
    TestEqual(TEXT("one target cell is outside every mapped source"), Report.NoDataSamples, uint64(1));
    TestEqual(TEXT("one cell falls to Project1m because all S1M taps are nodata"), Report.SamplesBySource[1], uint64(1));
    TestTrue(TEXT("preflight and sample range requests remain bounded"), Report.Requests <= TestLimits().MaxRequests);
    TestTrue(TEXT("preflight and sample transfer bytes remain bounded"), Report.TransferredBytes <= TestLimits().MaxTransferredBytes);

    SkiPreparation::TerrainScratchLodTile Tile;
    int32 Index = INDEX_NONE;
    TestTrue(TEXT("fallback sample can be read back"), ReadSample(Scratch, 0, 0, Tile, Index, Error));
    TestEqual(TEXT("fallback height comes from the lower-priority source"), Tile.Heights[Index], 903.75F);
    TestEqual(TEXT("fallback validity is retained"), Tile.Validity[Index], uint8(1));
    TestEqual(TEXT("fallback provenance is Project1m"), Tile.Provenance[Index],
        static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::Project1m));
    TestEqual(TEXT("fallback source-table identity is retained"), Tile.SourceIndices[Index], uint8(1));

    TestTrue(TEXT("primary S1M sample can be read back"), ReadSample(Scratch, 2, 2, Tile, Index, Error));
    TestEqual(TEXT("primary height uses deterministic bilinear interpolation"), Tile.Heights[Index], 115.75F);
    TestEqual(TEXT("lineage callback's blend class is retained"), Tile.Provenance[Index],
        static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::S1MBlend));
    TestEqual(TEXT("primary source-table identity is retained"), Tile.SourceIndices[Index], uint8(0));

    TestTrue(TEXT("uncovered sample can be read back"), ReadSample(Scratch, 2, 0, Tile, Index, Error));
    TestEqual(TEXT("uncovered sample validity is zero"), Tile.Validity[Index], uint8(0));
    TestEqual(TEXT("uncovered sample carries NoData provenance"), Tile.Provenance[Index],
        static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::NoData));
    TestEqual(TEXT("uncovered sample has no source identity"), Tile.SourceIndices[Index],
        SkiPreparation::TerrainScratchNoSourceIndex);
    TestTrue(TEXT("canonical scratch cleanup succeeds"), Scratch.Cleanup(Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCogTerrainSamplerRejectsChangedAndMalformedSampleRangesTest,
    "MountainPlanner.M4.CogTerrainSampler.RejectsChangedAndMalformedSampleRanges",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCogTerrainSamplerRejectsChangedAndMalformedSampleRangesTest::RunTest(const FString&)
{
    TArray<uint8> Fixture;
    TestTrue(TEXT("S1M COG fixture is written"), LoadCogFixture(TEXT("CogTerrainSamplerMutations"), false, Fixture));
    if (Fixture.IsEmpty()) return false;
    const SkiPreparation::FCogTerrainSamplerLimits Limits = TestLimits();
    const int32 FirstSampleRangeCall = static_cast<int32>(CountPreflightRequests(Fixture, Limits));
    TestTrue(TEXT("preflight fixture required at least one request"), FirstSampleRangeCall > 0);

    const auto RunMutation = [&](const FScriptedSamplerTransport::EMutation Mutation,
        const TCHAR* TestDescription, const TCHAR* ExpectedDetail)
    {
        const FString Root = FixtureRoot(TEXT("CogTerrainSamplerMutationScratch"));
        SkiPreparation::TerrainScratchStore Scratch;
        FString Error;
        if (!BuildScratch(Scratch, Root, false, Error))
        {
            TestFalse(TestDescription, false);
            return;
        }
        FScriptedSamplerTransport Transport(Fixture);
        Transport.Mutation = Mutation;
        Transport.MutateAtCall = FirstSampleRangeCall;
        TArray<SkiPreparation::FCogTerrainSamplerSource> Sources;
        Sources.Add(MakePrimarySource());
        SkiPreparation::FCogTerrainSamplerReport Report;
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        TestFalse(TestDescription, SkiPreparation::SampleVerifiedElevationCogsToScratch(
            Transport, Sources, Scratch, Limits, Cancellation, Report));
        if (Transport.Requests.IsValidIndex(FirstSampleRangeCall))
        {
            TestEqual(TEXT("sample reads pin the preflight ETag"),
                Transport.Requests[FirstSampleRangeCall].IfMatchETag, FString(TEXT("\"sampler-v1\"")));
        }
        TestTrue(TEXT("failure identifies the offending sample range"), Report.FailureDetail.Contains(ExpectedDetail));
        TestTrue(TEXT("failed scratch is removed"), Scratch.Cleanup(Error));
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    };

    RunMutation(FScriptedSamplerTransport::EMutation::ChangedETag,
        TEXT("changed ETag during sampling is rejected"), TEXT("COG_SAMPLER_ETAG_CHANGED"));
    RunMutation(FScriptedSamplerTransport::EMutation::BadContentRange,
        TEXT("malformed Content-Range during sampling is rejected"), TEXT("COG_SAMPLER_CONTENT_RANGE_INVALID"));
    RunMutation(FScriptedSamplerTransport::EMutation::ShortBody,
        TEXT("short sample range body is rejected"), TEXT("COG_SAMPLER_CONTENT_RANGE_INVALID"));
    RunMutation(FScriptedSamplerTransport::EMutation::PreconditionFailed,
        TEXT("ETag precondition failure during sampling is rejected"), TEXT("COG_SAMPLER_ETAG_PRECONDITION_FAILED"));
    return true;
}

#endif
