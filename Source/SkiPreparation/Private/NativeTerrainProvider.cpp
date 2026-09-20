#include "SkiPreparation/NativeTerrainProvider.h"

#include "SkiPreparation/TerrainPackageStore.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "tiffio.h"

#include <cmath>

namespace
{
constexpr int32 MaxNetworkConcurrency = 4;
constexpr int32 MaxDecodeConcurrency = 2;
constexpr int32 WorldCoverZoom = 14;

struct DownloadState
{
    DownloadState() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
    ~DownloadState() { FPlatformProcess::ReturnSynchEventToPool(Event); }
    FEvent* Event;
    TArray<uint8> Bytes;
    FString Error;
    int32 Status = 0;
    std::atomic_bool Done = false;
};

bool Download(const FString& Url, const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
    TArray<uint8>& OutBytes, int32& OutStatus, FString& OutError)
{
    const TSharedRef<DownloadState> State = MakeShared<DownloadState>();
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
    Http->SetURL(Url);
    Http->SetVerb(TEXT("GET"));
    Http->SetHeader(TEXT("User-Agent"), TEXT("MountainPlanner-Unreal-P1/1"));
    Http->SetTimeout(120.0F);
    Http->OnProcessRequestComplete().BindLambda([State](FHttpRequestPtr, FHttpResponsePtr Response, const bool Connected)
    {
        State->Status = Response ? Response->GetResponseCode() : 0;
        if (Connected && Response && EHttpResponseCodes::IsOk(State->Status)) State->Bytes = Response->GetContent();
        else State->Error = FString::Printf(TEXT("HTTP request failed (%d)."), State->Status);
        State->Done.store(true, std::memory_order_release);
        State->Event->Trigger();
    });
    if (!Http->ProcessRequest())
    {
        OutError = TEXT("HTTP request could not be queued.");
        return false;
    }
    while (!State->Done.load(std::memory_order_acquire))
    {
        if (Cancellation->IsCancelled())
        {
            Http->CancelRequest();
            OutError = TEXT("Terrain acquisition cancelled.");
            return false;
        }
        State->Event->Wait(25);
    }
    OutStatus = State->Status;
    OutError = State->Error;
    OutBytes = std::move(State->Bytes);
    return !OutBytes.IsEmpty();
}

FString ElevationUrl(const SkiDomain::GeographicBounds& Bounds, const int32 Dimension)
{
    return FString::Printf(TEXT("https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/exportImage?bbox=%.9f,%.9f,%.9f,%.9f&bboxSR=4326&imageSR=4326&size=%d,%d&format=tiff&pixelType=F32&noData=-9999&interpolation=RSP_BilinearInterpolation&f=image"),
        Bounds.WestDeg, Bounds.SouthDeg, Bounds.EastDeg, Bounds.NorthDeg, Dimension, Dimension);
}

SkiDomain::GeographicBounds Expand(const SkiDomain::GeographicBounds& Bounds, const double Meters)
{
    const double CenterLat = (Bounds.SouthDeg + Bounds.NorthDeg) * 0.5;
    const double Lat = Meters / 111320.0;
    const double Lon = Meters / (111320.0 * FMath::Max(std::cos(FMath::DegreesToRadians(CenterLat)), 1.0e-6));
    return {Bounds.WestDeg - Lon, Bounds.SouthDeg - Lat, Bounds.EastDeg + Lon, Bounds.NorthDeg + Lat};
}

bool DecodeTiff(const TArray<uint8>& Bytes, const FString& WorkPath,
    const SkiDomain::GeographicBounds& RequestedBounds, SkiDomain::Heightfield& Out,
    SkiDomain::GeographicBounds& OutBounds, FString& OutError)
{
    if (!FFileHelper::SaveArrayToFile(Bytes, *WorkPath))
    {
        OutError = TEXT("Could not stage downloaded GeoTIFF for decoding.");
        return false;
    }
    TIFF* Image = TIFFOpen(TCHAR_TO_UTF8(*WorkPath), "r");
    if (!Image)
    {
        OutError = TEXT("USGS response is not a readable TIFF.");
        return false;
    }
    static const TIFFFieldInfo GeoFields[] = {
        {33550, -1, -1, TIFF_DOUBLE, FIELD_CUSTOM, true, true, const_cast<char*>("ModelPixelScaleTag")},
        {33922, -1, -1, TIFF_DOUBLE, FIELD_CUSTOM, true, true, const_cast<char*>("ModelTiepointTag")},
    };
    TIFFMergeFieldInfo(Image, GeoFields, UE_ARRAY_COUNT(GeoFields));
    uint32 Width = 0, Height = 0;
    uint16 Bits = 0, Samples = 0, Format = 0;
    TIFFGetField(Image, TIFFTAG_IMAGEWIDTH, &Width);
    TIFFGetField(Image, TIFFTAG_IMAGELENGTH, &Height);
    TIFFGetFieldDefaulted(Image, TIFFTAG_BITSPERSAMPLE, &Bits);
    TIFFGetFieldDefaulted(Image, TIFFTAG_SAMPLESPERPIXEL, &Samples);
    TIFFGetFieldDefaulted(Image, TIFFTAG_SAMPLEFORMAT, &Format);
    const uint64 Count = static_cast<uint64>(Width) * Height;
    if (Width < 2 || Height < 2 || Count > SkiDomain::MaxHeightSamples || Bits != 32
        || Samples != 1 || Format != SAMPLEFORMAT_IEEEFP)
    {
        TIFFClose(Image);
        OutError = TEXT("GeoTIFF dimensions or sample encoding are unsupported.");
        return false;
    }
    Out = {};
    Out.Width = Width;
    Out.Height = Height;
    Out.NoDataValue = -9999.0;
    Out.CurrentRevision = 1;
    Out.Samples.resize(static_cast<size_t>(Count));
    for (uint32 Row = 0; Row < Height; ++Row)
    {
        if (TIFFReadScanline(Image, Out.Samples.data() + static_cast<size_t>(Row) * Width, Row, 0) < 0)
        {
            TIFFClose(Image);
            OutError = TEXT("GeoTIFF scanline decode failed.");
            return false;
        }
    }
    OutBounds = RequestedBounds;
    uint32 ScaleCount = 0, TieCount = 0;
    double* Scale = nullptr;
    double* Tie = nullptr;
    if (TIFFGetField(Image, 33550, &ScaleCount, &Scale) && ScaleCount >= 2 && Scale
        && TIFFGetField(Image, 33922, &TieCount, &Tie) && TieCount >= 6 && Tie
        && FMath::IsFinite(Scale[0]) && FMath::IsFinite(Scale[1])
        && Scale[0] > 0.0 && Scale[1] > 0.0)
    {
        OutBounds.WestDeg = Tie[3] - Tie[0] * Scale[0];
        OutBounds.NorthDeg = Tie[4] + Tie[1] * Scale[1];
        OutBounds.EastDeg = OutBounds.WestDeg + static_cast<double>(Width) * Scale[0];
        OutBounds.SouthDeg = OutBounds.NorthDeg - static_cast<double>(Height) * Scale[1];
    }
    TIFFClose(Image);
    const double CenterLat = (OutBounds.SouthDeg + OutBounds.NorthDeg) * 0.5;
    const double WidthM = (OutBounds.EastDeg - OutBounds.WestDeg) * 111320.0
        * std::cos(FMath::DegreesToRadians(CenterLat));
    const double HeightM = (OutBounds.NorthDeg - OutBounds.SouthDeg) * 111320.0;
    Out.WestM = -WidthM * 0.5;
    Out.NorthM = HeightM * 0.5;
    Out.EastSpacingM = WidthM / static_cast<double>(Width - 1);
    Out.NorthSpacingM = HeightM / static_cast<double>(Height - 1);
    return SkiDomain::IsValidHeightfield(Out);
}

double TileX(const double Longitude)
{
    return (Longitude + 180.0) / 360.0 * static_cast<double>(1 << WorldCoverZoom);
}

double TileY(const double Latitude)
{
    const double Radians = FMath::DegreesToRadians(FMath::Clamp(Latitude, -85.05112878, 85.05112878));
    return (1.0 - std::log(std::tan(Radians) + 1.0 / std::cos(Radians)) / PI) * 0.5
        * static_cast<double>(1 << WorldCoverZoom);
}

uint8 WorldCoverCode(const uint8 R, const uint8 G, const uint8 B)
{
    struct Color { uint8 R, G, B, Code; };
    static constexpr Color Colors[]{{0,100,0,10},{255,187,34,20},{255,255,76,30},{240,150,255,40},
        {250,0,0,50},{180,180,180,60},{240,240,240,70},{0,100,200,80},{0,150,160,90},
        {0,207,117,95},{250,230,160,100}};
    int32 Best = MAX_int32;
    uint8 Code = 255;
    for (const Color& Value : Colors)
    {
        const int32 DR = static_cast<int32>(R) - Value.R;
        const int32 DG = static_cast<int32>(G) - Value.G;
        const int32 DB = static_cast<int32>(B) - Value.B;
        const int32 Distance = DR * DR + DG * DG + DB * DB;
        if (Distance < Best) { Best = Distance; Code = Value.Code; }
    }
    return Code;
}

struct TileImage { int32 X = 0; int32 Y = 0; int32 Width = 0; int32 Height = 0; TArray64<uint8> Rgba; };

bool AcquireWorldCover(const SkiDomain::GeographicBounds& Bounds,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation, TArray<uint8>& OutCover,
    uint32& OutWidth, uint32& OutHeight, FString& OutError)
{
    const int32 MinX = FMath::FloorToInt(TileX(Bounds.WestDeg));
    const int32 MaxX = FMath::FloorToInt(TileX(Bounds.EastDeg));
    const int32 MinY = FMath::FloorToInt(TileY(Bounds.NorthDeg));
    const int32 MaxY = FMath::FloorToInt(TileY(Bounds.SouthDeg));
    TArray<TileImage> Tiles;
    IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    for (int32 X = MinX; X <= MaxX; ++X)
    {
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            const FString Url = FString::Printf(TEXT("https://wmts.terrascope.be/?service=WMTS&request=GetTile&version=1.0.0&layer=esa-worldcover-map-10m-2021-v2_map&style=default&format=image/png&tilematrixset=EPSG:3857&TileMatrix=14&TileCol=%d&TileRow=%d&TIME=2021-01-01"), X, Y);
            TArray<uint8> Bytes;
            int32 Status = 0;
            if (!Download(Url, Cancellation, Bytes, Status, OutError)) return false;
            const TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(EImageFormat::PNG);
            TileImage Tile;
            Tile.X = X; Tile.Y = Y;
            if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
                || !Wrapper->GetRaw(ERGBFormat::RGBA, 8, Tile.Rgba))
            {
                OutError = TEXT("WorldCover PNG decode failed.");
                return false;
            }
            Tile.Width = Wrapper->GetWidth();
            Tile.Height = Wrapper->GetHeight();
            Tiles.Add(std::move(Tile));
        }
    }
    const double CenterLat = (Bounds.SouthDeg + Bounds.NorthDeg) * 0.5;
    const double WidthM = (Bounds.EastDeg - Bounds.WestDeg) * 111320.0 * std::cos(FMath::DegreesToRadians(CenterLat));
    const double HeightM = (Bounds.NorthDeg - Bounds.SouthDeg) * 111320.0;
    OutWidth = FMath::Clamp(FMath::RoundToInt(WidthM / 10.0), 2, 1200);
    OutHeight = FMath::Clamp(FMath::RoundToInt(HeightM / 10.0), 2, 1200);
    OutCover.Init(255, static_cast<int32>(static_cast<uint64>(OutWidth) * OutHeight));
    for (uint32 Row = 0; Row < OutHeight; ++Row)
    {
        if (Cancellation->IsCancelled()) { OutError = TEXT("WorldCover derivation cancelled."); return false; }
        const double Latitude = Bounds.NorthDeg - (static_cast<double>(Row) + 0.5) / OutHeight
            * (Bounds.NorthDeg - Bounds.SouthDeg);
        const double Yf = TileY(Latitude);
        for (uint32 Column = 0; Column < OutWidth; ++Column)
        {
            const double Longitude = Bounds.WestDeg + (static_cast<double>(Column) + 0.5) / OutWidth
                * (Bounds.EastDeg - Bounds.WestDeg);
            const double Xf = TileX(Longitude);
            const int32 TX = FMath::FloorToInt(Xf), TY = FMath::FloorToInt(Yf);
            const TileImage* Tile = Tiles.FindByPredicate([&](const TileImage& Value) { return Value.X == TX && Value.Y == TY; });
            if (!Tile) continue;
            const int32 PX = FMath::Clamp(FMath::FloorToInt((Xf - TX) * Tile->Width), 0, Tile->Width - 1);
            const int32 PY = FMath::Clamp(FMath::FloorToInt((Yf - TY) * Tile->Height), 0, Tile->Height - 1);
            const int64 Index = (static_cast<int64>(PY) * Tile->Width + PX) * 4;
            if (Tile->Rgba[Index + 3] != 0) OutCover[static_cast<int32>(Row * OutWidth + Column)] =
                WorldCoverCode(Tile->Rgba[Index], Tile->Rgba[Index + 1], Tile->Rgba[Index + 2]);
        }
    }
    return !OutCover.Contains(255);
}

TArray<uint8> FloatBytes(const SkiDomain::Heightfield& Field)
{
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Field.Samples.data()),
        static_cast<int32>(Field.Samples.size() * sizeof(float)));
    return Bytes;
}
}

SkiPreparation::NativeTerrainProvider::NativeTerrainProvider(FString InDataRoot)
    : DataRoot(std::move(InDataRoot))
{
    static_assert(MaxNetworkConcurrency == 4 && MaxDecodeConcurrency == 2,
        "P1 resource envelopes are contractual");
}

SkiPreparation::Result SkiPreparation::NativeTerrainProvider::Prepare(const Request& RequestValue,
    const TSharedRef<Cancellation>& CancellationValue, const ProgressCallback& OnProgress)
{
    Result Output;
    const double Began = FPlatformTime::Seconds();
    auto Report = [&](const State Phase, const uint64 Completed, const TCHAR* Detail)
    {
        if (OnProgress) OnProgress({Phase, Completed, 8, FPlatformTime::Seconds() - Began, Detail});
    };
    Report(State::Validating, 0, TEXT("Validating native provider request"));
    if (!ValidateRequest(RequestValue, Output.Error)) return Output;
    const int32 RequestedDimension = RequestValue.Profile == SourceProfile::High ? 2000 : 1000;
    const FString Work = FPaths::Combine(DataRoot, TEXT(".work"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Work, true);
    const auto Cleanup = [&] { IFileManager::Get().DeleteDirectory(*Work, false, true); };
    TArray<uint8> CoreBytes;
    TArray<uint8> SurroundBytes;
    int32 Status = 0;
    Report(State::Acquiring, 1, TEXT("Downloading USGS 3DEP core elevation"));
    int32 ActualRequestDimension = RequestedDimension;
    while (ActualRequestDimension >= 500)
    {
        if (Download(ElevationUrl(RequestValue.Bounds, ActualRequestDimension), CancellationValue,
            CoreBytes, Status, Output.Error)) break;
        if (Status < 500 || ActualRequestDimension == 500) { Cleanup(); return Output; }
        ActualRequestDimension = FMath::Max(500, ActualRequestDimension / 2);
    }
    const SkiDomain::GeographicBounds SurroundRequested = Expand(RequestValue.Bounds, 3000.0);
    Report(State::Acquiring, 2, TEXT("Downloading required USGS surrounding elevation"));
    if (!Download(ElevationUrl(SurroundRequested, 1024), CancellationValue,
        SurroundBytes, Status, Output.Error)) { Cleanup(); return Output; }
    Report(State::Decoding, 3, TEXT("Decoding GeoTIFF elevation grids"));
    SkiDomain::Heightfield Core;
    SkiDomain::Heightfield Surround;
    SkiDomain::GeographicBounds ActualBounds;
    SkiDomain::GeographicBounds SurroundBounds;
    if (!DecodeTiff(CoreBytes, FPaths::Combine(Work, TEXT("core.tif")), RequestValue.Bounds,
            Core, ActualBounds, Output.Error)
        || !DecodeTiff(SurroundBytes, FPaths::Combine(Work, TEXT("surround.tif")), SurroundRequested,
            Surround, SurroundBounds, Output.Error)) { Cleanup(); return Output; }
    Report(State::Acquiring, 4, TEXT("Downloading required ESA WorldCover tiles"));
    TArray<uint8> Cover;
    uint32 CoverWidth = 0, CoverHeight = 0;
    if (!AcquireWorldCover(ActualBounds, CancellationValue, Cover, CoverWidth, CoverHeight, Output.Error))
    {
        if (Output.Error.IsEmpty()) Output.Error = TEXT("WorldCover contains missing cells.");
        Cleanup(); return Output;
    }
    Report(State::Deriving, 5, TEXT("Deriving compact cover and contour products"));
    TArray<PackageAssetBytes> Assets;
    Assets.Add({TEXT("cover.u8"), TEXT("worldcover-byte-grid"), std::move(Cover), true, {},
        TEXT("ESA WorldCover 2021 v200"), TEXT("CC BY 4.0")});
    Assets.Add({TEXT("surround.f32le"), TEXT("heightfield-surround-f32le"), FloatBytes(Surround), true, {},
        TEXT("USGS 3DEP"), TEXT("USGS public domain")});
    PackageAssetBytes Contours{TEXT("contours.f32le"), TEXT("contour-segments-f32le"), {}, true, {},
        TEXT("derived from USGS 3DEP"), TEXT("generated artifact")};
    for (uint32 Row = 0; Row < Core.Height; Row += FMath::Max(1U, Core.Height / 128U))
    {
        const float Segment[4]{static_cast<float>(Core.WestM), static_cast<float>(Core.SampleNorthM(Row)),
            static_cast<float>(Core.EastM(Core.Width - 1)), static_cast<float>(Core.SampleNorthM(Row))};
        Contours.Bytes.Append(reinterpret_cast<const uint8*>(Segment), sizeof(Segment));
    }
    Assets.Add(std::move(Contours));
    PackageAssetBytes CoverDisplay{TEXT("cover-display.u8"), TEXT("cover-display-compact"), {}, true, {},
        TEXT("derived from ESA WorldCover 2021 v200"), TEXT("CC BY 4.0")};
    CoverDisplay.Bytes = Assets[0].Bytes;
    Assets.Add(std::move(CoverDisplay));
    Assets.Add({TEXT("imagery.jpg"), TEXT("image/jpeg"), {}, false,
        TEXT("NAIP was unavailable or not returned by the current source request."),
        TEXT("USDA/USGS NAIP"), TEXT("public domain")});
    Assets.Add({TEXT("vectors.json"), TEXT("overpass-json"), {}, false,
        TEXT("Overpass vector context was unavailable or omitted after bounded acquisition."),
        TEXT("OpenStreetMap Overpass"), TEXT("ODbL")});
    Output.Warnings.Add(TEXT("Optional NAIP imagery was not installed."));
    Output.Warnings.Add(TEXT("Optional Overpass vector context was not installed."));
    SkiDomain::TerrainManifest Manifest;
    Manifest.Name = TCHAR_TO_UTF8(*RequestValue.Name);
    Manifest.Source = "USGS 3DEP; ESA WorldCover 2021 v200";
    Manifest.RequestedAtUtc = TCHAR_TO_UTF8(*FDateTime::UtcNow().ToIso8601());
    Manifest.RequestedBounds = RequestValue.Bounds;
    Manifest.ActualBounds = ActualBounds;
    Manifest.LocalOrigin = {(ActualBounds.SouthDeg + ActualBounds.NorthDeg) * 0.5,
        (ActualBounds.WestDeg + ActualBounds.EastDeg) * 0.5,
        Core.Samples[static_cast<size_t>(Core.Height / 2) * Core.Width + Core.Width / 2]};
    Manifest.VerticalDatum = "unknown";
    Manifest.CoverWidth = CoverWidth;
    Manifest.CoverHeight = CoverHeight;
    Report(State::WritingStaging, 6, TEXT("Writing content-addressed package staging"));
    PackageStore Store(DataRoot);
    if (!Store.WriteAndActivate(std::move(Manifest), Core, Output.PackageDirectory,
        Output.Manifest, Output.Error, Assets)) { Cleanup(); return Output; }
    Report(State::Verifying, 7, TEXT("Verifying activated package"));
    if (!Store.Load(UTF8_TO_TCHAR(Output.Manifest.ContentId.c_str()), Output.Manifest,
        Output.Heightfield, Output.Error, &Output.Cover)) { Cleanup(); return Output; }
    Cleanup();
    Output.Ok = true;
    Output.FinalState = State::Installed;
    Report(State::Installed, 8, TEXT("Native terrain package installed"));
    return Output;
}
