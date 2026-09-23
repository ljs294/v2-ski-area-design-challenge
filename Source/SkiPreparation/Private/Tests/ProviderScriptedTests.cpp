#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/NativeTerrainProvider.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "tiffio.h"

#include <cmath>

namespace
{
constexpr uint32 ScriptedModelPixelScaleTag = 33550;
constexpr uint32 ScriptedModelTiepointTag = 33922;
constexpr uint32 ScriptedGeoKeyDirectoryTag = 34735;
constexpr double WorldCoverStep = 1.0 / 12000.0;

void RegisterGeoFields(TIFF* Image)
{
    static const TIFFFieldInfo Fields[] = {
        {ScriptedModelPixelScaleTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, true, true,
            const_cast<char*>("ModelPixelScaleTag")},
        {ScriptedModelTiepointTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE, FIELD_CUSTOM, true, true,
            const_cast<char*>("ModelTiepointTag")},
        {ScriptedGeoKeyDirectoryTag, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT, FIELD_CUSTOM, true, true,
            const_cast<char*>("GeoKeyDirectoryTag")},
        {TIFFTAG_GDAL_NODATA, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII, FIELD_CUSTOM, true, true,
            const_cast<char*>("GDALNoDataValue")},
    };
    for (const TIFFFieldInfo& Field : Fields)
        if (!TIFFFindField(Image, Field.field_tag, TIFF_ANY)) TIFFMergeFieldInfo(Image, &Field, 1);
}

void SetGeoreference(TIFF* Image, const double West, const double North,
    const double ScaleX, const double ScaleY, const char* NoData)
{
    double Scale[]{ScaleX, ScaleY, 0.0};
    double Tie[]{0.0, 0.0, 0.0, West, North, 0.0};
    uint16 Keys[]{1, 1, 0, 3, 1024, 0, 1, 2, 1025, 0, 1, 1, 2048, 0, 1, 4326};
    TIFFSetField(Image, ScriptedModelPixelScaleTag, 3U, Scale);
    TIFFSetField(Image, ScriptedModelTiepointTag, 6U, Tie);
    TIFFSetField(Image, ScriptedGeoKeyDirectoryTag, 16U, Keys);
    TIFFSetField(Image, TIFFTAG_GDAL_NODATA, static_cast<uint32>(FCStringAnsi::Strlen(NoData) + 1), NoData);
}

bool ReadAndDelete(const FString& Path, TArray<uint8>& OutBytes)
{
    const bool Loaded = FFileHelper::LoadFileToArray(OutBytes, *Path);
    IFileManager::Get().Delete(*Path);
    return Loaded;
}

/** A 3DEP-shaped float32 export whose georeference is exactly the requested bbox. */
bool BuildElevationExport(const FString& Scratch, const SkiDomain::GeographicBounds& Bounds,
    const uint32 Width, const uint32 Height, TArray<uint8>& OutBytes)
{
    const FString Path = FPaths::Combine(Scratch, FGuid::NewGuid().ToString() + TEXT(".tif"));
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    RegisterGeoFields(Image);
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, Width);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, Height);
    TIFFSetField(Image, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(Image, TIFFTAG_BITSPERSAMPLE, 32);
    TIFFSetField(Image, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(Image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(Image, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(Image, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(Image, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(Image, TIFFTAG_ROWSPERSTRIP, 1);
    SetGeoreference(Image, Bounds.WestDeg, Bounds.NorthDeg,
        (Bounds.EastDeg - Bounds.WestDeg) / Width, (Bounds.NorthDeg - Bounds.SouthDeg) / Height, "-9999");
    TArray<float> Row;
    Row.SetNumUninitialized(Width);
    bool Ok = true;
    for (uint32 Y = 0; Ok && Y < Height; ++Y)
    {
        const double Latitude = Bounds.NorthDeg - (Y + 0.5) * (Bounds.NorthDeg - Bounds.SouthDeg) / Height;
        for (uint32 X = 0; X < Width; ++X)
        {
            const double Longitude = Bounds.WestDeg + (X + 0.5) * (Bounds.EastDeg - Bounds.WestDeg) / Width;
            Row[X] = static_cast<float>(1500.0 + 4000.0 * (Latitude - 46.9) + 2500.0 * (Longitude + 121.5));
        }
        Ok = TIFFWriteEncodedStrip(Image, Y, Row.GetData(), Width * sizeof(float)) >= 0;
    }
    TIFFClose(Image);
    return ReadAndDelete(Path, OutBytes) && Ok;
}

/**
 * WorldCover-shaped class COG on the real 1/12000 degree lattice: tiled, DEFLATE, uint8,
 * GDAL_NODATA 0 and PixelIsArea. NoDataColumn (when set) marks one lattice column as nodata.
 */
bool BuildWorldCoverCog(const FString& Scratch, const double West, const double North,
    const uint32 Width, const uint32 Height, const int64 NoDataColumn, TArray<uint8>& OutBytes)
{
    const FString Path = FPaths::Combine(Scratch, FGuid::NewGuid().ToString() + TEXT(".tif"));
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*Path), "w");
    if (!Image) return false;
    RegisterGeoFields(Image);
    constexpr uint32 Tile = 256;
    TIFFSetField(Image, TIFFTAG_IMAGEWIDTH, Width);
    TIFFSetField(Image, TIFFTAG_IMAGELENGTH, Height);
    TIFFSetField(Image, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(Image, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(Image, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
    TIFFSetField(Image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(Image, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(Image, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(Image, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(Image, TIFFTAG_TILEWIDTH, Tile);
    TIFFSetField(Image, TIFFTAG_TILELENGTH, Tile);
    SetGeoreference(Image, West, North, WorldCoverStep, WorldCoverStep, "0");
    static constexpr uint8 Classes[]{10, 20, 30, 60, 70, 80};
    TArray<uint8> Buffer;
    Buffer.SetNumZeroed(Tile * Tile);
    bool Ok = true;
    for (uint32 Y = 0; Ok && Y < Height; Y += Tile)
    {
        for (uint32 X = 0; Ok && X < Width; X += Tile)
        {
            for (uint32 LocalY = 0; LocalY < Tile; ++LocalY)
            {
                for (uint32 LocalX = 0; LocalX < Tile; ++LocalX)
                {
                    const uint32 Column = X + LocalX, RowIndex = Y + LocalY;
                    uint8 Value = Classes[((Column / 7) + (RowIndex / 5)) % UE_ARRAY_COUNT(Classes)];
                    if (Column >= Width || RowIndex >= Height || static_cast<int64>(Column) == NoDataColumn) Value = 0;
                    Buffer[LocalY * Tile + LocalX] = Value;
                }
            }
            Ok = TIFFWriteEncodedTile(Image, TIFFComputeTile(Image, X, Y, 0, 0), Buffer.GetData(), Buffer.Num()) >= 0;
        }
    }
    TIFFClose(Image);
    return ReadAndDelete(Path, OutBytes) && Ok;
}

FString UrlParameter(const FString& Url, const TCHAR* Name)
{
    const FString Key = FString(Name) + TEXT("=");
    int32 Start = Url.Find(TEXT("?") + Key);
    if (Start == INDEX_NONE) Start = Url.Find(TEXT("&") + Key);
    if (Start == INDEX_NONE) return {};
    Start += Key.Len() + 1;
    int32 End = Url.Find(TEXT("&"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
    return Url.Mid(Start, End == INDEX_NONE ? MAX_int32 : End - Start);
}

class FRoutedProviderTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    FString Scratch;
    TArray<uint8> WorldCover;
    FString SamplesJson;
    int32 SamplesStatus = 200;
    bool bCancelOnWorldCover = false;
    int32 ElevationRequests = 0;
    int32 MetadataRequests = 0;
    int32 CoverRangeRequests = 0;

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation) override
    {
        SkiPreparation::HttpAcquisitionResult Result;
        Result.Attempt = Request.Attempt;
        if (Request.Url.Contains(TEXT("/exportImage?")))
        {
            ++ElevationRequests;
            TArray<FString> Parts, Size;
            UrlParameter(Request.Url, TEXT("bbox")).ParseIntoArray(Parts, TEXT(","));
            UrlParameter(Request.Url, TEXT("size")).ParseIntoArray(Size, TEXT(","));
            if (Parts.Num() == 4 && Size.Num() == 2)
            {
                const SkiDomain::GeographicBounds Bounds{FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]),
                    FCString::Atod(*Parts[2]), FCString::Atod(*Parts[3])};
                if (BuildElevationExport(Scratch, Bounds, FCString::Atoi(*Size[0]), FCString::Atoi(*Size[1]), Result.Bytes))
                {
                    Result.HttpStatus = 200;
                    Result.ContentType = TEXT("image/tiff");
                }
            }
        }
        else if (Request.Url.Contains(TEXT("/getSamples?")))
        {
            ++MetadataRequests;
            Result.HttpStatus = SamplesStatus;
            Result.ContentType = TEXT("application/json");
            const FTCHARToUTF8 Utf8(*SamplesJson);
            Result.Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        }
        else if (Request.Url.Contains(TEXT("esa-worldcover")) && Request.ByteRange.IsSet())
        {
            ++CoverRangeRequests;
            if (bCancelOnWorldCover) Cancellation->Cancel();
            const uint64 Total = WorldCover.Num();
            const uint64 Offset = Request.ByteRange->Offset;
            const uint64 End = FMath::Min(Total, Offset + Request.ByteRange->Length);
            if (Offset < Total)
            {
                Result.HttpStatus = 206;
                Result.Bytes.Append(WorldCover.GetData() + Offset, static_cast<int32>(End - Offset));
                Result.ContentRange = FString::Printf(TEXT("bytes %llu-%llu/%llu"), Offset, End - 1, Total);
            }
            else Result.HttpStatus = 416;
        }
        else Result.HttpStatus = 404;
        Result.BytesReceived = Result.Bytes.Num();
        if (!Result.Ok()) Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
        return Result;
    }
};

FString SamplesJson(const bool bMixed)
{
    FString Samples;
    for (int32 Index = 0; Index < 25; ++Index)
    {
        const bool bSecond = bMixed && Index % 2 == 1;
        Samples += FString::Printf(TEXT("%s{\"location\":{\"x\":0,\"y\":0},\"locationId\":%d,\"value\":\"1500\",\"attributes\":{\"Name\":\"%s\",\"ProductName\":\"%s\",\"VerticalDatum\":\"%s\",\"Resolution_X\":%s,\"Resolution_Y\":%s,\"AcquisitionDate\":%s}}"),
            Samples.IsEmpty() ? TEXT("") : TEXT(","), Index,
            bSecond ? TEXT("WA_EasternCascades_2019") : TEXT("n47w122"),
            bSecond ? TEXT("1 meter DEM") : TEXT("1/3 arc-second DEM"),
            bSecond ? TEXT("NAVD88 GEOID18") : TEXT("North American Vertical Datum of 1988 (NAVD 88)"),
            bSecond ? TEXT("1.0") : TEXT("9.2592592593e-05"), bSecond ? TEXT("1.0") : TEXT("9.2592592593e-05"),
            bSecond ? TEXT("1567296000000") : TEXT("1598054400000"));
    }
    return FString::Printf(TEXT("{\"samples\":[%s]}"), *Samples);
}

struct FScriptedRun
{
    FString Root;
    TSharedPtr<FRoutedProviderTransport, ESPMode::ThreadSafe> Transport;
    SkiPreparation::Result Result;
};

bool RunScriptedMedium(FAutomationTestBase& Test, const TCHAR* Label, FScriptedRun& Run,
    const bool bMixedMetadata, const int32 SamplesStatus, const bool bNoDataInFootprint,
    const bool bCancelOnCover)
{
    Run.Root = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("P1ProviderScripted"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Run.Root, true);
    Run.Transport = MakeShared<FRoutedProviderTransport, ESPMode::ThreadSafe>();
    Run.Transport->Scratch = FPaths::Combine(Run.Root, TEXT("scratch"));
    IFileManager::Get().MakeDirectory(*Run.Transport->Scratch, true);
    Run.Transport->SamplesJson = SamplesJson(bMixedMetadata);
    Run.Transport->SamplesStatus = SamplesStatus;
    Run.Transport->bCancelOnWorldCover = bCancelOnCover;

    SkiPreparation::Request Request;
    Request.Name = Label;
    Request.Bounds = {-121.4875, 46.9260, -121.4605, 46.9440};
    Request.Profile = SkiPreparation::SourceProfile::Medium;
    Request.SessionGeneration = 3;
    Request.OperationGeneration = 5;
    // The COG covers the request with a margin, anchored on the global lattice.
    const double CogWest = FMath::FloorToDouble((Request.Bounds.WestDeg - 0.01) / WorldCoverStep) * WorldCoverStep;
    const double CogNorth = FMath::CeilToDouble((Request.Bounds.NorthDeg + 0.01) / WorldCoverStep) * WorldCoverStep;
    const int64 NoDataColumn = bNoDataInFootprint
        ? FMath::FloorToInt64((((Request.Bounds.WestDeg + Request.Bounds.EastDeg) * 0.5) - CogWest) / WorldCoverStep)
        : -1;
    if (!BuildWorldCoverCog(Run.Transport->Scratch, CogWest, CogNorth, 600, 480, NoDataColumn,
            Run.Transport->WorldCover))
    {
        Test.AddError(TEXT("Scripted WorldCover COG could not be generated"));
        return false;
    }
    SkiPreparation::NativeTerrainProvider Provider(Run.Root, Run.Transport);
    Run.Result = Provider.Prepare(Request, MakeShared<SkiPreparation::Cancellation>(), {});
    return true;
}

bool DirectoryHasEntries(const FString& Directory)
{
    TArray<FString> Entries;
    IFileManager::Get().FindFiles(Entries, *FPaths::Combine(Directory, TEXT("*")), true, true);
    return !Entries.IsEmpty();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1ScriptedMediumProviderTest,
    "MountainPlanner.P1.Product.Medium.ScriptedProvider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1ScriptedMediumProviderTest::RunTest(const FString&)
{
    FScriptedRun Run;
    if (!RunScriptedMedium(*this, TEXT("Scripted Medium"), Run, false, 200, false, false)) return false;
    const SkiPreparation::Result& Result = Run.Result;
    TestTrue(*FString::Printf(TEXT("Scripted live Medium installs: %s"), *Result.Error), Result.Ok);
    TestEqual(TEXT("Final state is Installed"), static_cast<int32>(Result.FinalState),
        static_cast<int32>(SkiPreparation::State::Installed));
    TestEqual(TEXT("Medium core is a 2x2 export plan plus surround"), Run.Transport->ElevationRequests, 5);
    TestEqual(TEXT("Provider metadata queried once"), Run.Transport->MetadataRequests, 1);
    TestTrue(TEXT("WorldCover read through byte ranges"), Run.Transport->CoverRangeRequests >= 1);

    // Delivered geometry is shown from real data, not the legacy zero fields.
    TestTrue(TEXT("Legacy runtime manifest carries delivered dimensions and spacing"),
        Result.Manifest.HeightWidth == Result.TerrainCoreManifest.Width
        && Result.Manifest.HeightHeight == Result.TerrainCoreManifest.Height
        && Result.Manifest.EastSpacingM > 0.0 && Result.Manifest.NorthSpacingM > 0.0);
    const SkiDomain::TerrainCoreSource& Source = Result.TerrainCoreManifest.Source;
    TestEqual(TEXT("Vertical datum comes from the provider"), FString(UTF8_TO_TCHAR(Source.VerticalDatum.c_str())),
        FString(TEXT("North American Vertical Datum of 1988 (NAVD 88)")));
    TestTrue(TEXT("Uniform native spacing is reported"), Source.NativeSpacingReported);
    const double ExpectedEast = 9.2592592593e-05 * 111320.0 * std::cos(FMath::DegreesToRadians(46.935));
    TestTrue(TEXT("Native east spacing converts degrees at the site latitude"),
        FMath::IsNearlyEqual(Source.NativeEastSpacingM, ExpectedEast, 0.05));
    TestEqual(TEXT("Source epoch is the provider acquisition date, not the request time"),
        FString(UTF8_TO_TCHAR(Source.AcquisitionEpoch.c_str())), FString(TEXT("2020-08-22")));
    TestTrue(TEXT("Core no longer claims the surround as an additional source"),
        Result.TerrainCoreManifest.AdditionalSources.empty());

    // Required surround is installed as its own TerrainCore and referenced by the receipt.
    const SkiDomain::InstalledTerrainReceipt& Receipt = Result.InstallationReceipt;
    TestTrue(TEXT("Receipt references a separate surround TerrainCore"),
        SkiDomain::IsTerrainCoreSha256(Receipt.SurroundTerrainCoreId)
        && Receipt.SurroundTerrainCoreId == Result.SurroundTerrainCoreManifest.ContentId
        && Receipt.SurroundTerrainCoreId != Receipt.TerrainCoreId);
    TestTrue(TEXT("Surround extends beyond the core in the core's local frame"),
        Result.SurroundTerrainCoreManifest.OuterBounds.WestM < Result.TerrainCoreManifest.OuterBounds.WestM - 2000.0
        && Result.SurroundTerrainCoreManifest.OuterBounds.NorthM > Result.TerrainCoreManifest.OuterBounds.NorthM + 2000.0);
    SkiPreparation::TerrainCorePackageIndex SurroundIndex;
    FString Error;
    TestTrue(TEXT("Surround TerrainCore reopens independently"), SkiPreparation::TerrainCorePackageStore(Run.Root).Open(
        UTF8_TO_TCHAR(Receipt.SurroundTerrainCoreId.c_str()), SurroundIndex, Error));

    // Optional sources are recorded honestly.
    const SkiDomain::OptionalSourceOutcome* Naip = nullptr;
    for (const SkiDomain::OptionalSourceOutcome& Outcome : Receipt.OptionalSources)
        if (Outcome.SourceId == "naip") Naip = &Outcome;
    TestTrue(TEXT("NAIP is NotRequested with a reason, not falsely Unavailable"),
        Naip && Naip->Status == SkiDomain::OptionalSourceStatus::NotRequested
        && Naip->ReasonCode == "NOT_REQUESTED_IN_P1_MEDIUM" && Naip->ArtifactId.empty());

    // Cover is an unresampled window on the native lattice.
    const SkiDomain::CoverEcologyGridTransform& Transform = Result.CoverEcologyManifest.Transform;
    TestTrue(TEXT("Cover step is the native WorldCover lattice"),
        FMath::IsNearlyEqual(Transform.LongitudeStepDeg, WorldCoverStep, 1.0e-12)
        && FMath::IsNearlyEqual(Transform.LatitudeStepDeg, WorldCoverStep, 1.0e-12));
    const double WestCells = Transform.OuterBounds.WestDeg / WorldCoverStep;
    const double NorthCells = Transform.OuterBounds.NorthDeg / WorldCoverStep;
    TestTrue(TEXT("Cover window edges fall on the global lattice"),
        FMath::IsNearlyEqual(WestCells, FMath::RoundToDouble(WestCells), 1.0e-6)
        && FMath::IsNearlyEqual(NorthCells, FMath::RoundToDouble(NorthCells), 1.0e-6));
    TestTrue(TEXT("Cover window covers the terrain footprint"),
        Transform.OuterBounds.WestDeg <= Result.Manifest.ActualBounds.WestDeg
        && Transform.OuterBounds.EastDeg >= Result.Manifest.ActualBounds.EastDeg
        && Transform.OuterBounds.SouthDeg <= Result.Manifest.ActualBounds.SouthDeg
        && Transform.OuterBounds.NorthDeg >= Result.Manifest.ActualBounds.NorthDeg);
    TestTrue(TEXT("Cover provenance names the native-grid window"),
        FString(UTF8_TO_TCHAR(Result.CoverEcologyManifest.Source.Provenance.c_str())).Contains(TEXT("native-grid-window")));
    IFileManager::Get().DeleteDirectory(*Run.Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1ScriptedMediumHonestyTest,
    "MountainPlanner.P1.Product.Medium.ProvenanceHonesty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1ScriptedMediumHonestyTest::RunTest(const FString&)
{
    {
        FScriptedRun Mixed;
        if (!RunScriptedMedium(*this, TEXT("Scripted mixed"), Mixed, true, 200, false, false)) return false;
        const SkiDomain::TerrainCoreSource& Source = Mixed.Result.TerrainCoreManifest.Source;
        TestTrue(TEXT("Mixed-source Medium still installs"), Mixed.Result.Ok);
        TestFalse(TEXT("Mixed native spacing is not claimed"), Source.NativeSpacingReported);
        TestTrue(TEXT("Mixed vertical datums are labelled mixed"),
            FString(UTF8_TO_TCHAR(Source.VerticalDatum.c_str())).StartsWith(TEXT("mixed:")));
        TestEqual(TEXT("Mixed epochs are a range"), FString(UTF8_TO_TCHAR(Source.AcquisitionEpoch.c_str())),
            FString(TEXT("2019-09-01/2020-08-22")));
        IFileManager::Get().DeleteDirectory(*Mixed.Root, false, true);
    }
    {
        FScriptedRun Unreported;
        if (!RunScriptedMedium(*this, TEXT("Scripted unreported"), Unreported, false, 500, false, false)) return false;
        const SkiDomain::TerrainCoreSource& Source = Unreported.Result.TerrainCoreManifest.Source;
        TestTrue(TEXT("Metadata failure does not fail Medium"), Unreported.Result.Ok);
        TestFalse(TEXT("Unreported native spacing is not claimed"), Source.NativeSpacingReported);
        TestEqual(TEXT("Unreported datum is labelled"), FString(UTF8_TO_TCHAR(Source.VerticalDatum.c_str())),
            FString(TEXT("not reported by provider")));
        IFileManager::Get().DeleteDirectory(*Unreported.Root, false, true);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1ScriptedRequiredCoverTest,
    "MountainPlanner.P1.Product.Medium.RequiredCoverBlocksActivation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1ScriptedRequiredCoverTest::RunTest(const FString&)
{
    {
        FScriptedRun NoData;
        if (!RunScriptedMedium(*this, TEXT("Scripted nodata"), NoData, false, 200, true, false)) return false;
        TestFalse(TEXT("Nodata inside the footprint fails preparation"), NoData.Result.Ok);
        TestTrue(TEXT("Nodata failure has a stable code"), NoData.Result.Failure.IsSet()
            && NoData.Result.Failure->Code == TEXT("WORLDCOVER_NODATA_IN_FOOTPRINT"));
        TestFalse(TEXT("No TerrainCore is written before required cover validates"),
            DirectoryHasEntries(FPaths::Combine(NoData.Root, TEXT("TerrainCore"))));
        TestFalse(TEXT("No composite installation activates"),
            DirectoryHasEntries(FPaths::Combine(NoData.Root, TEXT("InstalledTerrain"))));
        IFileManager::Get().DeleteDirectory(*NoData.Root, false, true);
    }
    {
        FScriptedRun Cancelled;
        if (!RunScriptedMedium(*this, TEXT("Scripted cancel"), Cancelled, false, 200, false, true)) return false;
        TestFalse(TEXT("Cancelled preparation does not install"), Cancelled.Result.Ok);
        TestEqual(TEXT("Cancellation reports Cancelled"), static_cast<int32>(Cancelled.Result.FinalState),
            static_cast<int32>(SkiPreparation::State::Cancelled));
        TestTrue(TEXT("Cancellation carries a stable code"), Cancelled.Result.Failure.IsSet()
            && Cancelled.Result.Failure->Code == TEXT("PREPARATION_CANCELLED"));
        TestFalse(TEXT("Cancelled preparation activates nothing"),
            DirectoryHasEntries(FPaths::Combine(Cancelled.Root, TEXT("InstalledTerrain"))));
        IFileManager::Get().DeleteDirectory(*Cancelled.Root, false, true);
    }
    return true;
}

#endif
