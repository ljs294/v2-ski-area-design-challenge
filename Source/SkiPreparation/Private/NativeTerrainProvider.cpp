#include "SkiPreparation/NativeTerrainProvider.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
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
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <cmath>

namespace
{
constexpr int32 MaxNetworkConcurrency = 4;
constexpr int32 MaxDecodeConcurrency = 2;
constexpr int32 WorldCoverZoom = 14;
constexpr int64 MaxPreparationLogBytes = 1024 * 1024;

FCriticalSection DiagnosticsMutex;

struct DownloadState
{
    DownloadState() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
    ~DownloadState() { FPlatformProcess::ReturnSynchEventToPool(Event); }
    FEvent* Event;
    TArray<uint8> Bytes;
    FString Error;
    int32 Status = 0;
    FString ContentType;
    std::atomic_bool Done = false;
};

struct DownloadResult
{
    TArray<uint8> Bytes;
    int32 Status = 0;
    FString ContentType;
    FString Error;
};

FString DiagnosticsDirectory(const FString& DataRoot)
{
    return FPaths::Combine(DataRoot, TEXT("TerrainDiagnostics"));
}

FString SerializeJson(const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json));
    return Json;
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
    const FString Journal = FPaths::Combine(Directory, TEXT("current-operation.json"));
    const FString Temporary = Journal + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(SerializeJson(Root), *Temporary,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Files.Move(*Journal, *Temporary, true, true);
    }
}

void ClearOperationJournal(const FString& DataRoot)
{
    FScopeLock Lock(&DiagnosticsMutex);
    IFileManager::Get().Delete(*FPaths::Combine(DiagnosticsDirectory(DataRoot), TEXT("current-operation.json")));
}

void InitializeDiagnosticsInternal(const FString& DataRoot)
{
    const FString Journal = FPaths::Combine(DiagnosticsDirectory(DataRoot), TEXT("current-operation.json"));
    if (FPaths::FileExists(Journal))
    {
        AppendPreparationEvent(DataRoot, nullptr, TEXT("PREPARATION_INTERRUPTED"),
            SkiPreparation::State::Failed, SkiPreparation::ProviderProduct::None);
        ClearOperationJournal(DataRoot);
    }
    const FString WorkRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(DataRoot, TEXT(".work")));
    TArray<FString> Directories;
    IFileManager& Files = IFileManager::Get();
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

bool Download(const FString& Url, const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
    DownloadResult& Out)
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
        State->ContentType = Response ? Response->GetContentType() : FString();
        if (Response) State->Bytes = Response->GetContent();
        if (!Connected || !Response || !EHttpResponseCodes::IsOk(State->Status))
            State->Error = FString::Printf(TEXT("HTTP request failed (%d)."), State->Status);
        State->Done.store(true, std::memory_order_release);
        State->Event->Trigger();
    });
    if (!Http->ProcessRequest())
    {
        Out.Error = TEXT("HTTP request could not be queued.");
        return false;
    }
    while (!State->Done.load(std::memory_order_acquire))
    {
        if (Cancellation->IsCancelled())
        {
            Http->CancelRequest();
            Out.Error = TEXT("Terrain acquisition cancelled.");
            return false;
        }
        State->Event->Wait(25);
    }
    Out.Status = State->Status;
    Out.ContentType = State->ContentType.Left(128);
    Out.Error = State->Error;
    Out.Bytes = std::move(State->Bytes);
    return Out.Error.IsEmpty() && !Out.Bytes.IsEmpty();
}

void PopulateDownloadFailure(const DownloadResult& Downloaded,
    const SkiPreparation::ProviderProduct Product, SkiPreparation::ProviderFailure& Failure)
{
    Failure = {};
    Failure.Code = TEXT("HTTP_ACQUISITION_FAILED");
    Failure.Stage = SkiPreparation::FailureStage::Acquisition;
    Failure.Product = Product;
    Failure.Retry = Downloaded.Status >= 500 || Downloaded.Status == 0
        ? SkiPreparation::RetryClassification::Retryable
        : SkiPreparation::RetryClassification::ChangeSelection;
    Failure.Summary = Downloaded.Error.IsEmpty() ? TEXT("Provider returned no usable data.") : Downloaded.Error;
    Failure.HttpStatus = Downloaded.Status;
    Failure.ContentType = Downloaded.ContentType.Left(128);
    Failure.ResponseBytes = Downloaded.Bytes.Num();
    Failure.ResponseSha256 = SkiPreparation::Sha256(Downloaded.Bytes);
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
    uint32& OutWidth, uint32& OutHeight, FString& OutError,
    SkiPreparation::ProviderFailure& OutFailure)
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
            DownloadResult Downloaded;
            if (!Download(Url, Cancellation, Downloaded))
            {
                PopulateDownloadFailure(Downloaded, SkiPreparation::ProviderProduct::WorldCover, OutFailure);
                OutError = OutFailure.Summary;
                return false;
            }
            TArray<uint8>& Bytes = Downloaded.Bytes;
            const TSharedPtr<IImageWrapper> Wrapper = Images.CreateImageWrapper(EImageFormat::PNG);
            TileImage Tile;
            Tile.X = X; Tile.Y = Y;
            if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())
                || !Wrapper->GetRaw(ERGBFormat::RGBA, 8, Tile.Rgba))
            {
                OutError = TEXT("WorldCover PNG decode failed.");
                OutFailure = {};
                OutFailure.Code = TEXT("WORLDCOVER_PNG_DECODE_FAILED");
                OutFailure.Stage = SkiPreparation::FailureStage::Decoding;
                OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
                OutFailure.Retry = SkiPreparation::RetryClassification::Retryable;
                OutFailure.Summary = OutError;
                OutFailure.HttpStatus = Downloaded.Status;
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
        if (Cancellation->IsCancelled())
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
    InitializeDiagnosticsInternal(DataRoot);
}

SkiPreparation::NativeTerrainProvider::NativeTerrainProvider(FString InDataRoot)
    : DataRoot(std::move(InDataRoot))
{
    static_assert(MaxNetworkConcurrency == 4 && MaxDecodeConcurrency == 2,
        "P1 resource envelopes are contractual");
    InitializePreparationDiagnostics(DataRoot);
}

SkiPreparation::Result SkiPreparation::NativeTerrainProvider::Prepare(const Request& RequestValue,
    const TSharedRef<Cancellation>& CancellationValue, const ProgressCallback& OnProgress)
{
    Result Output;
    const double Began = FPlatformTime::Seconds();
    ProviderProduct ActiveProduct = ProviderProduct::None;
    auto Report = [&](const State Phase, const uint64 Completed, const TCHAR* Detail)
    {
        WriteOperationJournal(DataRoot, RequestValue, Phase, ActiveProduct);
        AppendPreparationEvent(DataRoot, &RequestValue, TEXT("STATE"), Phase, ActiveProduct);
        if (OnProgress) OnProgress({Phase, Completed, 8, FPlatformTime::Seconds() - Began, Detail});
    };
    auto FinishFailure = [&](ProviderFailure Failure)
    {
        if (CancellationValue->IsCancelled()) Output.FinalState = State::Cancelled;
        Output.Error = Failure.Summary;
        WriteFailureDiagnostic(DataRoot, RequestValue, Failure);
        AppendPreparationEvent(DataRoot, &RequestValue, TEXT("FAILURE"), Output.FinalState,
            Failure.Product, &Failure);
        Output.Failure = std::move(Failure);
    };
    Report(State::Validating, 0, TEXT("Validating native provider request"));
    ON_SCOPE_EXIT { ClearOperationJournal(DataRoot); };
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
    const int32 RequestedDimension = RequestValue.Profile == SourceProfile::High ? 2000 : 1000;
    DownloadResult CoreDownload;
    DownloadResult SurroundDownload;
    ActiveProduct = ProviderProduct::CoreElevation;
    Report(State::Acquiring, 1, TEXT("Downloading USGS 3DEP core elevation"));
    int32 ActualRequestDimension = RequestedDimension;
    while (ActualRequestDimension >= 500)
    {
        CoreDownload = {};
        if (Download(ElevationUrl(RequestValue.Bounds, ActualRequestDimension), CancellationValue,
            CoreDownload)) break;
        if (CoreDownload.Status < 500 || ActualRequestDimension == 500)
        {
            ProviderFailure Failure;
            PopulateDownloadFailure(CoreDownload, ProviderProduct::CoreElevation, Failure);
            FinishFailure(std::move(Failure));
            return Output;
        }
        ActualRequestDimension = FMath::Max(500, ActualRequestDimension / 2);
    }
    const SkiDomain::GeographicBounds SurroundRequested = Expand(RequestValue.Bounds, 3000.0);
    ActiveProduct = ProviderProduct::SurroundingElevation;
    Report(State::Acquiring, 2, TEXT("Downloading required USGS surrounding elevation"));
    if (!Download(ElevationUrl(SurroundRequested, 1024), CancellationValue, SurroundDownload))
    {
        ProviderFailure Failure;
        PopulateDownloadFailure(SurroundDownload, ProviderProduct::SurroundingElevation, Failure);
        FinishFailure(std::move(Failure));
        return Output;
    }
    ActiveProduct = ProviderProduct::CoreElevation;
    Report(State::Decoding, 3, TEXT("Decoding GeoTIFF elevation grids"));
    DecodedElevationRaster CoreRaster;
    DecodedElevationRaster SurroundRaster;
    ProviderFailure DecodeFailure;
    if (!DecodeElevationGeoTiff(CoreDownload.Bytes, RequestValue.Bounds,
            ProviderProduct::CoreElevation, CoreRaster, DecodeFailure))
    {
        DecodeFailure.HttpStatus = CoreDownload.Status;
        DecodeFailure.ContentType = CoreDownload.ContentType;
        DecodeFailure.ResponseBytes = CoreDownload.Bytes.Num();
        DecodeFailure.ResponseSha256 = Sha256(CoreDownload.Bytes);
        FinishFailure(std::move(DecodeFailure));
        return Output;
    }
    ActiveProduct = ProviderProduct::SurroundingElevation;
    WriteOperationJournal(DataRoot, RequestValue, State::Decoding, ActiveProduct);
    if (!DecodeElevationGeoTiff(SurroundDownload.Bytes, SurroundRequested,
            ProviderProduct::SurroundingElevation, SurroundRaster, DecodeFailure))
    {
        DecodeFailure.HttpStatus = SurroundDownload.Status;
        DecodeFailure.ContentType = SurroundDownload.ContentType;
        DecodeFailure.ResponseBytes = SurroundDownload.Bytes.Num();
        DecodeFailure.ResponseSha256 = Sha256(SurroundDownload.Bytes);
        FinishFailure(std::move(DecodeFailure));
        return Output;
    }
    SkiDomain::Heightfield Core = std::move(CoreRaster.Heightfield);
    SkiDomain::Heightfield Surround = std::move(SurroundRaster.Heightfield);
    const SkiDomain::GeographicBounds ActualBounds = CoreRaster.ActualOuterBounds;
    ActiveProduct = ProviderProduct::WorldCover;
    Report(State::Acquiring, 4, TEXT("Downloading required ESA WorldCover tiles"));
    TArray<uint8> Cover;
    uint32 CoverWidth = 0, CoverHeight = 0;
    ProviderFailure CoverFailure;
    if (!AcquireWorldCover(ActualBounds, CancellationValue, Cover, CoverWidth, CoverHeight,
            Output.Error, CoverFailure))
    {
        if (Output.Error.IsEmpty()) Output.Error = TEXT("WorldCover contains missing cells.");
        if (CoverFailure.Summary.IsEmpty()) CoverFailure.Summary = Output.Error;
        FinishFailure(std::move(CoverFailure));
        return Output;
    }
    ActiveProduct = ProviderProduct::None;
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
        Output.Manifest, Output.Error, Assets))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("PACKAGE_WRITE_FAILED");
        Failure.Stage = FailureStage::Writing;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }
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
