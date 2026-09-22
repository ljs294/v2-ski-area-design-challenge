#include "SkiPreparation/NativeTerrainProvider.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include <cmath>

namespace
{
constexpr int32 WorldCoverZoom = 14;
constexpr int64 MaxPreparationLogBytes = 1024 * 1024;

FCriticalSection DiagnosticsMutex;
TSet<FString> InitializedDiagnosticsRoots;

FString DiagnosticsDirectory(const FString& DataRoot)
{
    return FPaths::Combine(DataRoot, TEXT("TerrainDiagnostics"));
}

FString SerializeJson(const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    FJsonSerializer::Serialize(Object,
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return Json;
}

FString OperationJournalPath(const FString& DataRoot, const SkiPreparation::Request& Request)
{
    return FPaths::Combine(DiagnosticsDirectory(DataRoot), FString::Printf(
        TEXT("current-operation-%llu-%llu.json"), Request.SessionGeneration, Request.OperationGeneration));
}

void RotatePreparationLog(const FString& Directory)
{
    IFileManager& Files = IFileManager::Get();
    const FString Current = FPaths::Combine(Directory, TEXT("preparation.jsonl"));
    if (Files.FileSize(*Current) < MaxPreparationLogBytes) return;
    Files.Delete(*FPaths::Combine(Directory, TEXT("preparation.3.jsonl")));
    Files.Move(*FPaths::Combine(Directory, TEXT("preparation.3.jsonl")),
        *FPaths::Combine(Directory, TEXT("preparation.2.jsonl")), true, true);
    Files.Move(*FPaths::Combine(Directory, TEXT("preparation.2.jsonl")),
        *FPaths::Combine(Directory, TEXT("preparation.1.jsonl")), true, true);
    Files.Move(*FPaths::Combine(Directory, TEXT("preparation.1.jsonl")), *Current, true, true);
}

void AppendPreparationEvent(const FString& DataRoot, const SkiPreparation::Request* Request,
    const TCHAR* Event, const SkiPreparation::State State, const SkiPreparation::ProviderProduct Product,
    const SkiPreparation::ProviderFailure* Failure = nullptr)
{
    FScopeLock Lock(&DiagnosticsMutex);
    const FString Directory = DiagnosticsDirectory(DataRoot);
    IFileManager& Files = IFileManager::Get();
    if (!Files.MakeDirectory(*Directory, true)) return;
    RotatePreparationLog(Directory);
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("recordedAtUtc"), FDateTime::UtcNow().ToIso8601());
    Root->SetStringField(TEXT("event"), Event);
    Root->SetStringField(TEXT("state"), SkiPreparation::StateName(State));
    Root->SetStringField(TEXT("product"), SkiPreparation::ProviderProductName(Product));
    if (Request)
    {
        Root->SetStringField(TEXT("sessionGeneration"), LexToString(Request->SessionGeneration));
        Root->SetStringField(TEXT("operationGeneration"), LexToString(Request->OperationGeneration));
    }
    if (Failure)
    {
        Root->SetStringField(TEXT("code"), Failure->Code.Left(96));
        Root->SetNumberField(TEXT("httpStatus"), Failure->HttpStatus);
        Root->SetStringField(TEXT("contentType"), Failure->ContentType.Left(128));
        Root->SetNumberField(TEXT("responseBytes"), static_cast<double>(Failure->ResponseBytes));
        Root->SetStringField(TEXT("responseSha256"), Failure->ResponseSha256.Left(64));
        Root->SetNumberField(TEXT("width"), Failure->Width);
        Root->SetNumberField(TEXT("height"), Failure->Height);
        Root->SetStringField(TEXT("organization"), Failure->Organization.Left(32));
        Root->SetStringField(TEXT("transportFailure"), Failure->TransportFailure.Left(64));
        Root->SetStringField(TEXT("requestStatus"), Failure->RequestStatus.Left(64));
        Root->SetNumberField(TEXT("elapsedSeconds"), Failure->ElapsedSeconds);
        Root->SetNumberField(TEXT("attempt"), Failure->Attempt);
        Root->SetNumberField(TEXT("tileIndex"), Failure->TileIndex);
        Root->SetNumberField(TEXT("tileCount"), Failure->TileCount);
    }
    FFileHelper::SaveStringToFile(SerializeJson(Root) + TEXT("\n"),
        *FPaths::Combine(Directory, TEXT("preparation.jsonl")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &Files, FILEWRITE_Append);
}

void WriteOperationJournal(const FString& DataRoot, const SkiPreparation::Request& Request,
    const SkiPreparation::State State, const SkiPreparation::ProviderProduct Product)
{
    FScopeLock Lock(&DiagnosticsMutex);
    const FString Directory = DiagnosticsDirectory(DataRoot);
    IFileManager& Files = IFileManager::Get();
    if (!Files.MakeDirectory(*Directory, true)) return;
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("recordedAtUtc"), FDateTime::UtcNow().ToIso8601());
    Root->SetStringField(TEXT("state"), SkiPreparation::StateName(State));
    Root->SetStringField(TEXT("product"), SkiPreparation::ProviderProductName(Product));
    Root->SetStringField(TEXT("sessionGeneration"), LexToString(Request.SessionGeneration));
    Root->SetStringField(TEXT("operationGeneration"), LexToString(Request.OperationGeneration));
    const FString Journal = OperationJournalPath(DataRoot, Request);
    const FString Temporary = Journal + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(SerializeJson(Root), *Temporary,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Files.Move(*Journal, *Temporary, true, true);
    }
}

void ClearOperationJournal(const FString& DataRoot, const SkiPreparation::Request& Request)
{
    FScopeLock Lock(&DiagnosticsMutex);
    IFileManager::Get().Delete(*OperationJournalPath(DataRoot, Request));
}

void InitializeDiagnosticsInternal(const FString& DataRoot)
{
    const FString Directory = DiagnosticsDirectory(DataRoot);
    TArray<FString> Journals;
    IFileManager& Files = IFileManager::Get();
    Files.FindFiles(Journals, *FPaths::Combine(Directory, TEXT("current-operation-*.json")), true, false);
    const FString LegacyJournal = FPaths::Combine(Directory, TEXT("current-operation.json"));
    if (FPaths::FileExists(LegacyJournal)) Journals.Add(TEXT("current-operation.json"));
    for (const FString& JournalName : Journals)
    {
        AppendPreparationEvent(DataRoot, nullptr, TEXT("PREPARATION_INTERRUPTED"),
            SkiPreparation::State::Failed, SkiPreparation::ProviderProduct::None);
        Files.Delete(*FPaths::Combine(Directory, JournalName));
    }
    const FString WorkRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(DataRoot, TEXT(".work")));
    TArray<FString> Directories;
    Files.FindFiles(Directories, *FPaths::Combine(WorkRoot, TEXT("*")), false, true);
    const FDateTime Cutoff = FDateTime::UtcNow() - FTimespan::FromHours(1.0);
    for (const FString& Name : Directories)
    {
        FGuid Parsed;
        if (!FGuid::ParseExact(Name, EGuidFormats::Digits, Parsed)) continue;
        const FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(WorkRoot, Name));
        FString Prefix = WorkRoot;
        if (!Prefix.EndsWith(TEXT("/")) && !Prefix.EndsWith(TEXT("\\"))) Prefix += TEXT("/");
        FString NormalCandidate = Candidate.Replace(TEXT("\\"), TEXT("/"));
        Prefix.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (NormalCandidate.StartsWith(Prefix) && Files.GetTimeStamp(*Candidate) < Cutoff)
            Files.DeleteDirectory(*Candidate, false, true);
    }
}

void PopulateDownloadFailure(const SkiPreparation::HttpAcquisitionResult& Downloaded,
    const SkiPreparation::HttpAcquisitionRequest& Request, const int32 MaximumAttempts,
    const int32 TileIndex, const int32 TileCount, SkiPreparation::ProviderFailure& Failure)
{
    Failure = {};
    Failure.Code = TEXT("HTTP_ACQUISITION_FAILED");
    Failure.Stage = SkiPreparation::FailureStage::Acquisition;
    Failure.Product = Request.Product;
    Failure.Retry = SkiPreparation::IsRetryableTransportFailure(Downloaded)
        ? SkiPreparation::RetryClassification::Retryable
        : SkiPreparation::RetryClassification::ChangeSelection;
    Failure.Summary = FString::Printf(TEXT("%s before a usable provider response (HTTP %d)."),
        SkiPreparation::TransportFailureReasonName(Downloaded.FailureReason), Downloaded.HttpStatus);
    Failure.HttpStatus = Downloaded.HttpStatus;
    Failure.ContentType = Downloaded.ContentType.Left(128);
    Failure.ResponseBytes = Downloaded.BytesReceived > 0
        ? Downloaded.BytesReceived : static_cast<uint64>(Downloaded.Bytes.Num());
    Failure.ResponseSha256 = SkiPreparation::Sha256(Downloaded.Bytes);
    Failure.TransportFailure = SkiPreparation::TransportFailureReasonName(Downloaded.FailureReason);
    Failure.RequestStatus = Downloaded.RequestStatus.Left(64);
    Failure.TimeToFirstByteSeconds = Downloaded.TimeToFirstByteSeconds;
    Failure.ElapsedSeconds = Downloaded.ElapsedSeconds;
    Failure.Attempt = Request.Attempt;
    Failure.MaximumAttempts = MaximumAttempts;
    Failure.TileIndex = TileIndex;
    Failure.TileCount = TileCount;
    Failure.RequestedWidth = Request.Tile.Width;
    Failure.RequestedHeight = Request.Tile.Height;
    Failure.ActivityTimeoutSeconds = Request.ActivityTimeoutSeconds;
    Failure.TotalTimeoutSeconds = Request.TotalTimeoutSeconds;
    Failure.RetryOutcome = Request.Attempt >= MaximumAttempts ? TEXT("retry limit reached") : TEXT("not retryable");
}

FString WriteFailureDiagnostic(const FString& DataRoot, const SkiPreparation::Request& Request,
    SkiPreparation::ProviderFailure& Failure)
{
    FScopeLock Lock(&DiagnosticsMutex);
    const FString Directory = DiagnosticsDirectory(DataRoot);
    IFileManager& Files = IFileManager::Get();
    if (!Files.MakeDirectory(*Directory, true)) return {};
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("code"), Failure.Code);
    Root->SetStringField(TEXT("stage"), SkiPreparation::FailureStageName(Failure.Stage));
    Root->SetStringField(TEXT("product"), SkiPreparation::ProviderProductName(Failure.Product));
    Root->SetStringField(TEXT("summary"), Failure.Summary.Left(512));
    Root->SetNumberField(TEXT("sessionGeneration"), static_cast<double>(Request.SessionGeneration));
    Root->SetNumberField(TEXT("operationGeneration"), static_cast<double>(Request.OperationGeneration));
    Root->SetNumberField(TEXT("httpStatus"), Failure.HttpStatus);
    Root->SetStringField(TEXT("contentType"), Failure.ContentType.Left(128));
    Root->SetNumberField(TEXT("responseBytes"), static_cast<double>(Failure.ResponseBytes));
    Root->SetStringField(TEXT("responseSha256"), Failure.ResponseSha256);
    Root->SetNumberField(TEXT("width"), Failure.Width);
    Root->SetNumberField(TEXT("height"), Failure.Height);
    Root->SetStringField(TEXT("organization"), Failure.Organization);
    Root->SetNumberField(TEXT("compression"), Failure.Compression);
    Root->SetNumberField(TEXT("orientation"), Failure.Orientation);
    Root->SetNumberField(TEXT("sampleFormat"), Failure.SampleFormat);
    Root->SetStringField(TEXT("nodata"), Failure.NoData.Left(64));
    Root->SetNumberField(TEXT("metadataTag"), Failure.MetadataTag);
    Root->SetNumberField(TEXT("metadataType"), Failure.MetadataType);
    Root->SetNumberField(TEXT("metadataReadCount"), Failure.MetadataReadCount);
    Root->SetBoolField(TEXT("metadataPassCount"), Failure.MetadataPassCount);
    Root->SetStringField(TEXT("georeferenceStatus"), Failure.GeoreferenceStatus.Left(128));
    Root->SetStringField(TEXT("transportFailure"), Failure.TransportFailure.Left(64));
    Root->SetStringField(TEXT("requestStatus"), Failure.RequestStatus.Left(64));
    Root->SetNumberField(TEXT("timeToFirstByteSeconds"), Failure.TimeToFirstByteSeconds);
    Root->SetNumberField(TEXT("elapsedSeconds"), Failure.ElapsedSeconds);
    Root->SetNumberField(TEXT("attempt"), Failure.Attempt);
    Root->SetNumberField(TEXT("maximumAttempts"), Failure.MaximumAttempts);
    Root->SetNumberField(TEXT("tileIndex"), Failure.TileIndex);
    Root->SetNumberField(TEXT("tileCount"), Failure.TileCount);
    Root->SetNumberField(TEXT("requestedWidth"), Failure.RequestedWidth);
    Root->SetNumberField(TEXT("requestedHeight"), Failure.RequestedHeight);
    Root->SetNumberField(TEXT("activityTimeoutSeconds"), Failure.ActivityTimeoutSeconds);
    Root->SetNumberField(TEXT("totalTimeoutSeconds"), Failure.TotalTimeoutSeconds);
    Root->SetStringField(TEXT("retryOutcome"), Failure.RetryOutcome.Left(128));
    Root->SetStringField(TEXT("profile"), Request.Profile == SkiPreparation::SourceProfile::High
        ? TEXT("high") : TEXT("standard"));
    Root->SetNumberField(TEXT("requestedWest"), Request.Bounds.WestDeg);
    Root->SetNumberField(TEXT("requestedSouth"), Request.Bounds.SouthDeg);
    Root->SetNumberField(TEXT("requestedEast"), Request.Bounds.EastDeg);
    Root->SetNumberField(TEXT("requestedNorth"), Request.Bounds.NorthDeg);
    Root->SetStringField(TEXT("recordedAtUtc"), FDateTime::UtcNow().ToIso8601());
    FString Json;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
    const FString Filename = FString::Printf(TEXT("%lld-%s.json"),
        FDateTime::UtcNow().ToUnixTimestamp(), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Path = FPaths::Combine(Directory, Filename);
    if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return {};

    TArray<FString> Names;
    Files.FindFiles(Names, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
    Names.Sort();
    uint64 Total = 0;
    for (const FString& Name : Names) Total += FMath::Max<int64>(0, Files.FileSize(*FPaths::Combine(Directory, Name)));
    while (Names.Num() > 5 || Total > 10ULL * 1024ULL * 1024ULL)
    {
        const FString Oldest = FPaths::Combine(Directory, Names[0]);
        const int64 Size = FMath::Max<int64>(0, Files.FileSize(*Oldest));
        if (!Files.Delete(*Oldest)) break;
        Total -= FMath::Min<uint64>(Total, static_cast<uint64>(Size));
        Names.RemoveAt(0);
    }
    Failure.DiagnosticReceipt = FPaths::Combine(TEXT("TerrainDiagnostics"), Filename);
    return Path;
}

FString ElevationUrl(const SkiDomain::GeographicBounds& Bounds, const uint32 Width, const uint32 Height)
{
    return FString::Printf(TEXT("https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/exportImage?bbox=%.9f,%.9f,%.9f,%.9f&bboxSR=4326&imageSR=4326&size=%u,%u&format=tiff&pixelType=F32&noData=-9999&interpolation=RSP_BilinearInterpolation&f=image"),
        Bounds.WestDeg, Bounds.SouthDeg, Bounds.EastDeg, Bounds.NorthDeg, Width, Height);
}

bool OperationCurrent(const SkiPreparation::Request& Request,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation)
{
    return !Cancellation->IsCancelled() && (!Request.Lease
        || Request.Lease->IsCurrent(Request.SessionGeneration, Request.OperationGeneration));
}

SkiDomain::GeographicBounds TileBounds(const SkiDomain::GeographicBounds& Bounds,
    const SkiPreparation::AcquisitionPlan& Plan, const SkiPreparation::RasterTileKey& Tile)
{
    const double LongitudeSpan = Bounds.EastDeg - Bounds.WestDeg;
    const double LatitudeSpan = Bounds.NorthDeg - Bounds.SouthDeg;
    const double WestFraction = static_cast<double>(Tile.StartColumn) / Plan.Width;
    const double EastFraction = static_cast<double>(Tile.StartColumn + Tile.Width) / Plan.Width;
    const double NorthFraction = static_cast<double>(Tile.StartRow) / Plan.Height;
    const double SouthFraction = static_cast<double>(Tile.StartRow + Tile.Height) / Plan.Height;
    return {Bounds.WestDeg + LongitudeSpan * WestFraction,
        Bounds.NorthDeg - LatitudeSpan * SouthFraction,
        Bounds.WestDeg + LongitudeSpan * EastFraction,
        Bounds.NorthDeg - LatitudeSpan * NorthFraction};
}

bool DownloadWithRetry(SkiPreparation::IAcquisitionTransport& Transport,
    SkiPreparation::HttpAcquisitionRequest AcquisitionRequest,
    const SkiPreparation::RetryPolicy& Policy, const SkiPreparation::Request& PreparationRequest,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation, const double OperationBegan,
    const int32 TileIndex, const int32 TileCount,
    const TFunction<void(const SkiPreparation::HttpAcquisitionRequest&, int32, int32)>& BeforeAttempt,
    SkiPreparation::HttpAcquisitionResult& Out, SkiPreparation::ProviderFailure& OutFailure)
{
    const bool bSucceeded = SkiPreparation::ExecuteAcquisitionWithRetry(Transport,
        AcquisitionRequest, Policy, Cancellation, OperationBegan,
        [&]() { return OperationCurrent(PreparationRequest, Cancellation); },
        [&](const SkiPreparation::HttpAcquisitionRequest& AttemptRequest)
        {
            if (BeforeAttempt) BeforeAttempt(AttemptRequest, TileIndex, TileCount);
        }, Out);
    AcquisitionRequest.Attempt = Out.Attempt;
    if (bSucceeded) return true;
    PopulateDownloadFailure(Out, AcquisitionRequest, Policy.MaximumAttempts, TileIndex, TileCount, OutFailure);
    if (Out.FailureReason == SkiPreparation::TransportFailureReason::Cancelled)
    {
        OutFailure.Code = TEXT("PREPARATION_CANCELLED");
        OutFailure.Summary = TEXT("Preparation was cancelled or exceeded its operation deadline.");
    }
    return false;
}

bool AcquireElevation(SkiPreparation::IAcquisitionTransport& Transport,
    const SkiDomain::GeographicBounds& Bounds, const SkiPreparation::SourceProfile Profile,
    const SkiPreparation::ProviderProduct Product, const SkiPreparation::Request& PreparationRequest,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation, const double OperationBegan,
    const SkiPreparation::RetryPolicy& Policy,
    const TFunction<void(const SkiPreparation::HttpAcquisitionRequest&, int32, int32)>& BeforeAttempt,
    SkiPreparation::DecodedElevationRaster& OutRaster, SkiPreparation::ProviderFailure& OutFailure)
{
    const SkiPreparation::AcquisitionPlan Plan = SkiPreparation::BuildElevationAcquisitionPlan(Bounds, Profile);
    if (Plan.Tiles.IsEmpty())
    {
        OutFailure = {};
        OutFailure.Code = TEXT("ACQUISITION_PLAN_INVALID");
        OutFailure.Stage = SkiPreparation::FailureStage::Validation;
        OutFailure.Product = Product;
        OutFailure.Summary = TEXT("Elevation acquisition plan is invalid.");
        return false;
    }
    TArray<SkiPreparation::DecodedElevationRaster> DecodedTiles;
    DecodedTiles.Reserve(Plan.Tiles.Num());
    for (int32 Index = 0; Index < Plan.Tiles.Num(); ++Index)
    {
        const SkiPreparation::RasterTileKey& Tile = Plan.Tiles[Index];
        const SkiDomain::GeographicBounds RequestedTileBounds = TileBounds(Bounds, Plan, Tile);
        SkiPreparation::HttpAcquisitionRequest HttpRequest;
        HttpRequest.Url = ElevationUrl(RequestedTileBounds, Tile.Width, Tile.Height);
        HttpRequest.Product = Product;
        HttpRequest.Tile = Tile;
        HttpRequest.ActivityTimeoutSeconds = Policy.ActivityTimeoutSeconds;
        HttpRequest.TotalTimeoutSeconds = Policy.TotalTimeoutSeconds;
        HttpRequest.MaximumResponseBytes = Policy.MaximumResponseBytes;
        SkiPreparation::HttpAcquisitionResult Downloaded;
        if (!DownloadWithRetry(Transport, HttpRequest, Policy, PreparationRequest, Cancellation,
                OperationBegan, Index + 1, Plan.Tiles.Num(), BeforeAttempt, Downloaded, OutFailure))
            return false;
        SkiPreparation::DecodedElevationRaster Decoded;
        bool bDecoded = false;
        const bool bDecodeJobRan = SkiPreparation::ExecuteBoundedDecodeJob(Cancellation,
            [&]() { return OperationCurrent(PreparationRequest, Cancellation); },
            [&]()
            {
                bDecoded = SkiPreparation::DecodeElevationGeoTiff(Downloaded.Bytes,
                    RequestedTileBounds, Product, Decoded, OutFailure,
                    [&]() { return !OperationCurrent(PreparationRequest, Cancellation); });
                return true;
            });
        if (!bDecodeJobRan)
        {
            OutFailure = {};
            OutFailure.Code = TEXT("PREPARATION_CANCELLED");
            OutFailure.Stage = SkiPreparation::FailureStage::Decoding;
            OutFailure.Product = Product;
            OutFailure.Retry = SkiPreparation::RetryClassification::Retryable;
            OutFailure.Summary = TEXT("Elevation decode was cancelled before execution.");
            return false;
        }
        if (!bDecoded)
        {
            OutFailure.HttpStatus = Downloaded.HttpStatus;
            OutFailure.ContentType = Downloaded.ContentType;
            OutFailure.ResponseBytes = Downloaded.Bytes.Num();
            OutFailure.ResponseSha256 = SkiPreparation::Sha256(Downloaded.Bytes);
            OutFailure.Attempt = HttpRequest.Attempt;
            OutFailure.TileIndex = Index + 1;
            OutFailure.TileCount = Plan.Tiles.Num();
            OutFailure.RequestedWidth = Tile.Width;
            OutFailure.RequestedHeight = Tile.Height;
            return false;
        }
        DecodedTiles.Add(std::move(Decoded));
    }
    FString StitchError;
    if (!SkiPreparation::StitchElevationTiles(Plan, DecodedTiles, OutRaster, StitchError))
    {
        OutFailure = {};
        OutFailure.Code = TEXT("ELEVATION_TILE_STITCH_FAILED");
        OutFailure.Stage = SkiPreparation::FailureStage::Decoding;
        OutFailure.Product = Product;
        OutFailure.Summary = StitchError;
        OutFailure.TileCount = Plan.Tiles.Num();
        OutFailure.RequestedWidth = Plan.Width;
        OutFailure.RequestedHeight = Plan.Height;
        return false;
    }
    return true;
}

SkiDomain::GeographicBounds Expand(const SkiDomain::GeographicBounds& Bounds, const double Meters)
{
    const double CenterLat = (Bounds.SouthDeg + Bounds.NorthDeg) * 0.5;
    const double Lat = Meters / 111320.0;
    const double Lon = Meters / (111320.0 * FMath::Max(std::cos(FMath::DegreesToRadians(CenterLat)), 1.0e-6));
    return {Bounds.WestDeg - Lon, Bounds.SouthDeg - Lat, Bounds.EastDeg + Lon, Bounds.NorthDeg + Lat};
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

bool AcquireWorldCover(SkiPreparation::IAcquisitionTransport& Transport,
    const SkiDomain::GeographicBounds& Bounds, const SkiPreparation::Request& PreparationRequest,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation, const double OperationBegan,
    const SkiPreparation::RetryPolicy& Policy,
    const TFunction<void(const SkiPreparation::HttpAcquisitionRequest&, int32, int32)>& BeforeAttempt,
    TArray<uint8>& OutCover,
    uint32& OutWidth, uint32& OutHeight, FString& OutError,
    SkiPreparation::ProviderFailure& OutFailure)
{
    const int32 MinX = FMath::FloorToInt(TileX(Bounds.WestDeg));
    const int32 MaxX = FMath::FloorToInt(TileX(Bounds.EastDeg));
    const int32 MinY = FMath::FloorToInt(TileY(Bounds.NorthDeg));
    const int32 MaxY = FMath::FloorToInt(TileY(Bounds.SouthDeg));
    const int32 TileCount = (MaxX - MinX + 1) * (MaxY - MinY + 1);
    int32 TileIndex = 0;
    TArray<TileImage> Tiles;
    IImageWrapperModule& Images = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    for (int32 X = MinX; X <= MaxX; ++X)
    {
        for (int32 Y = MinY; Y <= MaxY; ++Y)
        {
            ++TileIndex;
            const FString Url = FString::Printf(TEXT("https://wmts.terrascope.be/?service=WMTS&request=GetTile&version=1.0.0&layer=esa-worldcover-map-10m-2021-v2_map&style=default&format=image/png&tilematrixset=EPSG:3857&TileMatrix=14&TileCol=%d&TileRow=%d&TIME=2021-01-01"), X, Y);
            SkiPreparation::HttpAcquisitionRequest HttpRequest;
            HttpRequest.Url = Url;
            HttpRequest.Product = SkiPreparation::ProviderProduct::WorldCover;
            HttpRequest.Tile = {X - MinX, Y - MinY, MaxX - MinX + 1, MaxY - MinY + 1, 0, 0, 512, 512};
            HttpRequest.ActivityTimeoutSeconds = Policy.ActivityTimeoutSeconds;
            HttpRequest.TotalTimeoutSeconds = Policy.TotalTimeoutSeconds;
            HttpRequest.MaximumResponseBytes = Policy.MaximumResponseBytes;
            SkiPreparation::HttpAcquisitionResult Downloaded;
            if (!DownloadWithRetry(Transport, HttpRequest, Policy, PreparationRequest, Cancellation,
                    OperationBegan, TileIndex, TileCount, BeforeAttempt, Downloaded, OutFailure))
            {
                OutError = OutFailure.Summary;
                return false;
            }
            TArray<uint8>& Bytes = Downloaded.Bytes;
            const TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(EImageFormat::PNG);
            TileImage Tile;
            Tile.X = X; Tile.Y = Y;
            bool bDecoded = false;
            const bool bDecodeJobRan = SkiPreparation::ExecuteBoundedDecodeJob(Cancellation,
                [&]() { return OperationCurrent(PreparationRequest, Cancellation); },
                [&]()
                {
                    bDecoded = Wrapper.IsValid() && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
                        && Wrapper->GetRaw(ERGBFormat::RGBA, 8, Tile.Rgba);
                    return true;
                });
            if (!bDecodeJobRan)
            {
                OutError = TEXT("WorldCover decode cancelled.");
                OutFailure = {};
                OutFailure.Code = TEXT("PREPARATION_CANCELLED");
                OutFailure.Stage = SkiPreparation::FailureStage::Decoding;
                OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
                OutFailure.Retry = SkiPreparation::RetryClassification::Retryable;
                OutFailure.Summary = OutError;
                return false;
            }
            if (!bDecoded)
            {
                OutError = TEXT("WorldCover PNG decode failed.");
                OutFailure = {};
                OutFailure.Code = TEXT("WORLDCOVER_PNG_DECODE_FAILED");
                OutFailure.Stage = SkiPreparation::FailureStage::Decoding;
                OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
                OutFailure.Retry = SkiPreparation::RetryClassification::Retryable;
                OutFailure.Summary = OutError;
                OutFailure.HttpStatus = Downloaded.HttpStatus;
                OutFailure.ContentType = Downloaded.ContentType;
                OutFailure.ResponseBytes = Bytes.Num();
                OutFailure.ResponseSha256 = SkiPreparation::Sha256(Bytes);
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
        if (!OperationCurrent(PreparationRequest, Cancellation))
        {
            OutError = TEXT("WorldCover derivation cancelled.");
            OutFailure = {};
            OutFailure.Code = TEXT("PREPARATION_CANCELLED");
            OutFailure.Stage = SkiPreparation::FailureStage::Derivation;
            OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
            OutFailure.Retry = SkiPreparation::RetryClassification::Retryable;
            OutFailure.Summary = OutError;
            return false;
        }
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
    if (OutCover.Contains(255))
    {
        OutError = TEXT("WorldCover contains missing cells.");
        OutFailure = {};
        OutFailure.Code = TEXT("WORLDCOVER_INCOMPLETE");
        OutFailure.Stage = SkiPreparation::FailureStage::Derivation;
        OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
        OutFailure.Retry = SkiPreparation::RetryClassification::Retryable;
        OutFailure.Summary = OutError;
        return false;
    }
    return true;
}

TArray<uint8> FloatBytes(const SkiDomain::Heightfield& Field)
{
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Field.Samples.data()),
        static_cast<int32>(Field.Samples.size() * sizeof(float)));
    return Bytes;
}
}

void SkiPreparation::InitializePreparationDiagnostics(const FString& DataRoot)
{
    const FString NormalizedRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    {
        FScopeLock Lock(&DiagnosticsMutex);
        if (InitializedDiagnosticsRoots.Contains(NormalizedRoot)) return;
        InitializedDiagnosticsRoots.Add(NormalizedRoot);
    }
    InitializeDiagnosticsInternal(NormalizedRoot);
}

SkiPreparation::NativeTerrainProvider::NativeTerrainProvider(FString InDataRoot,
    TSharedPtr<IAcquisitionTransport, ESPMode::ThreadSafe> InTransport)
    : DataRoot(std::move(InDataRoot)), Transport(std::move(InTransport))
{
    if (!Transport) Transport = MakeShared<UnrealHttpAcquisitionTransport, ESPMode::ThreadSafe>();
    InitializePreparationDiagnostics(DataRoot);
}

SkiPreparation::Result SkiPreparation::NativeTerrainProvider::Prepare(const Request& RequestValue,
    const TSharedRef<Cancellation>& CancellationValue, const ProgressCallback& OnProgress)
{
    Result Output;
    const double Began = FPlatformTime::Seconds();
    RetryPolicy Policy;
    Policy.OperationDeadlineSeconds = RequestValue.Profile == SourceProfile::High ? 480.0 : 300.0;
    ProviderProduct ActiveProduct = ProviderProduct::None;
    uint64 ActiveCompleted = 0;
    auto Report = [&](const State Phase, const uint64 Completed, const TCHAR* Detail)
    {
        ActiveCompleted = Completed;
        WriteOperationJournal(DataRoot, RequestValue, Phase, ActiveProduct);
        AppendPreparationEvent(DataRoot, &RequestValue, TEXT("STATE"), Phase, ActiveProduct);
        if (OnProgress) OnProgress({Phase, Completed, 8, FPlatformTime::Seconds() - Began, Detail});
    };
    auto FinishFailure = [&](ProviderFailure Failure)
    {
        if (!OperationCurrent(RequestValue, CancellationValue)) Output.FinalState = State::Cancelled;
        Output.Error = Failure.Summary;
        WriteFailureDiagnostic(DataRoot, RequestValue, Failure);
        AppendPreparationEvent(DataRoot, &RequestValue, TEXT("FAILURE"), Output.FinalState,
            Failure.Product, &Failure);
        Output.Failure = std::move(Failure);
    };
    auto BeforeAttempt = [&](const HttpAcquisitionRequest& Attempt, const int32 TileIndex, const int32 TileCount)
    {
        if (!OnProgress) return;
        Progress Update{State::Acquiring, ActiveCompleted, 8, FPlatformTime::Seconds() - Began,
            FString::Printf(TEXT("%s tile %d/%d, attempt %d/%d, %ux%u"),
                ProviderProductName(Attempt.Product), TileIndex, TileCount, Attempt.Attempt,
                Policy.MaximumAttempts, Attempt.Tile.Width, Attempt.Tile.Height)};
        Update.Attempt = Attempt.Attempt; Update.MaximumAttempts = Policy.MaximumAttempts;
        Update.TileIndex = TileIndex; Update.TileCount = TileCount;
        OnProgress(Update);
    };
    Report(State::Validating, 0, TEXT("Validating native provider request"));
    ON_SCOPE_EXIT { ClearOperationJournal(DataRoot, RequestValue); };
    if (!ValidateRequest(RequestValue, Output.Error))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("REQUEST_INVALID");
        Failure.Stage = FailureStage::Validation;
        Failure.Summary = Output.Error;
        Failure.Retry = RetryClassification::ChangeSelection;
        FinishFailure(std::move(Failure));
        return Output;
    }
    if (!OperationCurrent(RequestValue, CancellationValue)) return Output;
    ActiveProduct = ProviderProduct::CoreElevation;
    Report(State::Acquiring, 1, TEXT("Downloading USGS 3DEP core elevation"));
    DecodedElevationRaster CoreRaster;
    ProviderFailure DecodeFailure;
    if (!AcquireElevation(*Transport, RequestValue.Bounds, RequestValue.Profile,
            ProviderProduct::CoreElevation, RequestValue, CancellationValue, Began, Policy,
            BeforeAttempt, CoreRaster, DecodeFailure))
    {
        FinishFailure(std::move(DecodeFailure));
        return Output;
    }
    const SkiDomain::GeographicBounds SurroundRequested = Expand(RequestValue.Bounds, 3000.0);
    ActiveProduct = ProviderProduct::SurroundingElevation;
    Report(State::Acquiring, 2, TEXT("Downloading required USGS surrounding elevation"));
    DecodedElevationRaster SurroundRaster;
    if (!AcquireElevation(*Transport, SurroundRequested, SourceProfile::Standard,
            ProviderProduct::SurroundingElevation, RequestValue, CancellationValue, Began, Policy,
            BeforeAttempt, SurroundRaster, DecodeFailure))
    {
        FinishFailure(std::move(DecodeFailure));
        return Output;
    }
    ActiveProduct = ProviderProduct::CoreElevation;
    Report(State::Decoding, 3, TEXT("Decoded and stitched elevation grids"));
    SkiDomain::Heightfield Core = std::move(CoreRaster.Heightfield);
    SkiDomain::Heightfield Surround = std::move(SurroundRaster.Heightfield);
    const SkiDomain::GeographicBounds ActualBounds = CoreRaster.ActualOuterBounds;
    ActiveProduct = ProviderProduct::WorldCover;
    Report(State::Acquiring, 4, TEXT("Downloading required ESA WorldCover tiles"));
    TArray<uint8> Cover;
    uint32 CoverWidth = 0, CoverHeight = 0;
    ProviderFailure CoverFailure;
    if (!AcquireWorldCover(*Transport, ActualBounds, RequestValue, CancellationValue, Began,
            Policy, BeforeAttempt, Cover, CoverWidth, CoverHeight, Output.Error, CoverFailure))
    {
        if (Output.Error.IsEmpty()) Output.Error = TEXT("WorldCover contains missing cells.");
        if (CoverFailure.Summary.IsEmpty()) CoverFailure.Summary = Output.Error;
        FinishFailure(std::move(CoverFailure));
        return Output;
    }
    ActiveProduct = ProviderProduct::None;
    if (!OperationCurrent(RequestValue, CancellationValue)) return Output;
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
    if (!OperationCurrent(RequestValue, CancellationValue)) return Output;
    Report(State::WritingStaging, 6, TEXT("Writing content-addressed package staging"));
    PackageStore Store(DataRoot);
    if (!Store.WriteAndActivate(std::move(Manifest), Core, Output.PackageDirectory,
        Output.Manifest, Output.Error, Assets, RequestValue.Lease,
        RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("PACKAGE_WRITE_FAILED");
        Failure.Stage = FailureStage::Writing;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }
    if (!OperationCurrent(RequestValue, CancellationValue)) return Output;
    Report(State::Verifying, 7, TEXT("Verifying activated package"));
    if (!Store.Load(UTF8_TO_TCHAR(Output.Manifest.ContentId.c_str()), Output.Manifest,
        Output.Heightfield, Output.Error, &Output.Cover))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("PACKAGE_VERIFY_FAILED");
        Failure.Stage = FailureStage::Verification;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }
    Output.Ok = true;
    Output.FinalState = State::Installed;
    Report(State::Installed, 8, TEXT("Native terrain package installed"));
    return Output;
}
