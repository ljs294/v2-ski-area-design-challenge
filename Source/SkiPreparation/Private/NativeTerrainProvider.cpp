#include "SkiPreparation/NativeTerrainProvider.h"

#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiPreparation/WorldCoverCogDecoder.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include <cmath>

namespace
{
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
    Root->SetStringField(TEXT("profile"), Request.Profile == SkiPreparation::SourceProfile::Medium
        ? TEXT("medium") : TEXT("legacy-standard"));
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

/** What the 3DEP mosaic actually used under the delivered grid, as reported by the service. */
struct ElevationSourceMetadata
{
    bool bQueried = false;
    bool bNativeSpacingUniform = false;
    double NativeEastSpacingM = 0.0;
    double NativeNorthSpacingM = 0.0;
    FString VerticalDatum = TEXT("not reported by provider");
    FString AcquisitionEpoch = TEXT("not reported by provider");
    FString Products = TEXT("not reported by provider");
    int32 DistinctSources = 0;
};

FString ElevationSamplesUrl(const SkiDomain::GeographicBounds& Bounds, const double PixelDegrees)
{
    FString Points;
    for (int32 Row = 0; Row < 5; ++Row)
    {
        for (int32 Column = 0; Column < 5; ++Column)
        {
            const double Longitude = Bounds.WestDeg + (Column + 0.5) / 5.0 * (Bounds.EastDeg - Bounds.WestDeg);
            const double Latitude = Bounds.SouthDeg + (Row + 0.5) / 5.0 * (Bounds.NorthDeg - Bounds.SouthDeg);
            Points += FString::Printf(TEXT("%s[%.8f,%.8f]"), Points.IsEmpty() ? TEXT("") : TEXT(","), Longitude, Latitude);
        }
    }
    const FString Geometry = FString::Printf(TEXT("{\"points\":[%s],\"spatialReference\":{\"wkid\":4326}}"), *Points);
    const FString PixelSize = FString::Printf(TEXT("{\"x\":%.10f,\"y\":%.10f,\"spatialReference\":{\"wkid\":4326}}"),
        PixelDegrees, PixelDegrees);
    return FString::Printf(TEXT("https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/getSamples?geometry=%s&geometryType=esriGeometryMultipoint&returnFirstValueOnly=true&pixelSize=%s&outFields=%s&f=json"),
        *FGenericPlatformHttp::UrlEncode(Geometry), *FGenericPlatformHttp::UrlEncode(PixelSize),
        *FGenericPlatformHttp::UrlEncode(TEXT("Name,ProductName,VerticalDatum,LowPS,Resolution_X,Resolution_Y,AcquisitionDate")));
}

double JsonNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    double Number = 0.0;
    FString Text;
    if (Object->TryGetNumberField(Field, Number)) return Number;
    if (Object->TryGetStringField(Field, Text)) return FCString::Atod(*Text);
    return 0.0;
}

/** Parses a getSamples response. Honest by construction: mixed or missing fields stay labelled. */
bool ParseElevationSourceMetadata(const TArray<uint8>& Bytes, const double CenterLatitude,
    ElevationSourceMetadata& Out)
{
    FString Json;
    FFileHelper::BufferToString(Json, Bytes.GetData(), Bytes.Num());
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root) return false;
    const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
    if (!Root->TryGetArrayField(TEXT("samples"), Samples) || !Samples || Samples->IsEmpty()) return false;
    TSet<FString> Products, Datums;
    TSet<int64> ResolutionKeys;
    double ResolutionX = 0.0, ResolutionY = 0.0;
    double MinDate = TNumericLimits<double>::Max(), MaxDate = 0.0;
    for (const TSharedPtr<FJsonValue>& Value : *Samples)
    {
        const TSharedPtr<FJsonObject>* Sample = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Sample) || !Sample) continue;
        const TSharedPtr<FJsonObject>* Attributes = nullptr;
        if (!(*Sample)->TryGetObjectField(TEXT("attributes"), Attributes) || !Attributes) continue;
        FString Product, Datum;
        if ((*Attributes)->TryGetStringField(TEXT("ProductName"), Product) && !Product.IsEmpty())
            Products.Add(Product.Left(96));
        if ((*Attributes)->TryGetStringField(TEXT("VerticalDatum"), Datum) && !Datum.IsEmpty())
            Datums.Add(Datum.Left(96));
        const double X = JsonNumber(*Attributes, TEXT("Resolution_X"));
        const double Y = JsonNumber(*Attributes, TEXT("Resolution_Y"));
        if (X > 0.0)
        {
            ResolutionKeys.Add(FMath::RoundToInt64(X * 1.0e9));
            ResolutionX = X;
            ResolutionY = Y > 0.0 ? Y : X;
        }
        const double Date = JsonNumber(*Attributes, TEXT("AcquisitionDate"));
        if (Date > 0.0) { MinDate = FMath::Min(MinDate, Date); MaxDate = FMath::Max(MaxDate, Date); }
    }
    Out.bQueried = true;
    Out.DistinctSources = Products.Num();
    if (!Products.IsEmpty()) Out.Products = FString::Join(Products.Array(), TEXT("; "));
    if (Datums.Num() == 1) Out.VerticalDatum = *Datums.CreateConstIterator();
    else if (Datums.Num() > 1) Out.VerticalDatum = TEXT("mixed: ") + FString::Join(Datums.Array(), TEXT("; "));
    if (MaxDate > 0.0)
    {
        const FString First = FDateTime::FromUnixTimestamp(static_cast<int64>(MinDate / 1000.0)).ToString(TEXT("%Y-%m-%d"));
        const FString Last = FDateTime::FromUnixTimestamp(static_cast<int64>(MaxDate / 1000.0)).ToString(TEXT("%Y-%m-%d"));
        Out.AcquisitionEpoch = First == Last ? First : First + TEXT("/") + Last;
    }
    if (ResolutionKeys.Num() == 1)
    {
        // Geographic sources report resolution in degrees; projected sources in metres.
        const bool bDegrees = ResolutionX < 0.01;
        const double Cosine = std::cos(FMath::DegreesToRadians(CenterLatitude));
        Out.NativeEastSpacingM = bDegrees ? ResolutionX * 111320.0 * Cosine : ResolutionX;
        Out.NativeNorthSpacingM = bDegrees ? ResolutionY * 111320.0 : ResolutionY;
        Out.bNativeSpacingUniform = Out.NativeEastSpacingM > 0.0 && Out.NativeNorthSpacingM > 0.0;
    }
    return true;
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

FString WorldCoverTileName(const int32 SouthDegrees, const int32 WestDegrees)
{
    return FString::Printf(TEXT("%c%02d%c%03d"), SouthDegrees >= 0 ? 'N' : 'S',
        FMath::Abs(SouthDegrees), WestDegrees >= 0 ? 'E' : 'W', FMath::Abs(WestDegrees));
}

class FHttpCogSource final : public SkiPreparation::ICogByteSource
{
public:
    FHttpCogSource(SkiPreparation::IAcquisitionTransport& InTransport, FString InUrl,
        const SkiPreparation::Request& InPreparationRequest,
        const TSharedRef<SkiPreparation::Cancellation>& InCancellation,
        const double InOperationBegan, const SkiPreparation::RetryPolicy& InPolicy,
        const int32 InTileIndex, const int32 InTileCount,
        TFunction<void(const SkiPreparation::HttpAcquisitionRequest&, int32, int32)> InBeforeAttempt)
        : Transport(InTransport), Url(std::move(InUrl)), PreparationRequest(InPreparationRequest)
        , Cancellation(InCancellation), OperationBegan(InOperationBegan), Policy(InPolicy)
        , TileIndex(InTileIndex), TileCount(InTileCount), BeforeAttempt(std::move(InBeforeAttempt)) {}

    bool Initialize(SkiPreparation::ProviderFailure& OutFailure)
    {
        constexpr uint64 HeaderBytes = 1024ULL * 1024ULL;
        SkiPreparation::HttpAcquisitionResult Result;
        if (!Fetch(0, HeaderBytes, Result, OutFailure)) return false;
        uint64 ParsedSize = 0;
        const bool bFullHeader = SkiPreparation::ValidateContentRange(Result.ContentRange,
            {0, HeaderBytes}, Result.Bytes.Num(), ParsedSize) && ParsedSize >= HeaderBytes;
        // An object smaller than one header block is valid only when the single response is
        // exactly the whole object.
        const bool bWholeSmallObject = !bFullHeader && !Result.Bytes.IsEmpty()
            && SkiPreparation::ValidateContentRange(Result.ContentRange,
                {0, static_cast<uint64>(Result.Bytes.Num())}, Result.Bytes.Num(), ParsedSize)
            && ParsedSize == static_cast<uint64>(Result.Bytes.Num());
        if ((!bFullHeader && !bWholeSmallObject) || ParsedSize > 512ULL * 1024ULL * 1024ULL)
        {
            OutFailure = {};
            OutFailure.Code = TEXT("WORLDCOVER_COG_RANGE_INVALID");
            OutFailure.Stage = SkiPreparation::FailureStage::Acquisition;
            OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
            OutFailure.Summary = TEXT("WorldCover COG returned an invalid or unsafe Content-Range.");
            return false;
        }
        TotalSize = ParsedSize;
        CachedBytes = static_cast<uint64>(Result.Bytes.Num());
        Cache.Add(0, MoveTemp(Result.Bytes));
        CacheOrder.Add(0);
        return true;
    }

    uint64 Size() const noexcept override { return TotalSize; }

    bool Read(const uint64 Offset, const uint64 Length, TArray<uint8>& OutBytes,
        FString& OutError) override
    {
        OutBytes.Reset();
        if (Length == 0 || Offset > TotalSize || Length > TotalSize - Offset)
        {
            OutError = TEXT("COG read range is outside the immutable object.");
            return false;
        }
        OutBytes.Reserve(static_cast<int32>(Length));
        uint64 Position = Offset;
        uint64 Remaining = Length;
        while (Remaining > 0)
        {
            constexpr uint64 BlockBytes = 1024ULL * 1024ULL;
            const uint64 BlockOffset = (Position / BlockBytes) * BlockBytes;
            const uint64 BlockLength = FMath::Min(BlockBytes, TotalSize - BlockOffset);
            TArray<uint8>* Block = Cache.Find(BlockOffset);
            if (!Block)
            {
                SkiPreparation::HttpAcquisitionResult Result;
                SkiPreparation::ProviderFailure Failure;
                if (!Fetch(BlockOffset, BlockLength, Result, Failure))
                {
                    OutError = Failure.Summary.IsEmpty() ? TEXT("COG range acquisition failed.") : Failure.Summary;
                    return false;
                }
                Cache.Add(BlockOffset, MoveTemp(Result.Bytes));
                CacheOrder.Add(BlockOffset);
                CachedBytes += BlockLength;
                while (CachedBytes > 16ULL * 1024ULL * 1024ULL && CacheOrder.Num() > 1)
                {
                    const uint64 Evict = CacheOrder[0];
                    CacheOrder.RemoveAt(0);
                    if (TArray<uint8>* Bytes = Cache.Find(Evict)) CachedBytes -= Bytes->Num();
                    Cache.Remove(Evict);
                }
                Block = Cache.Find(BlockOffset);
            }
            const uint64 Within = Position - BlockOffset;
            const uint64 Count = FMath::Min(Remaining, BlockLength - Within);
            OutBytes.Append(Block->GetData() + Within, static_cast<int32>(Count));
            Position += Count;
            Remaining -= Count;
        }
        return static_cast<uint64>(OutBytes.Num()) == Length;
    }

private:
    bool Fetch(const uint64 Offset, const uint64 Length,
        SkiPreparation::HttpAcquisitionResult& OutResult,
        SkiPreparation::ProviderFailure& OutFailure)
    {
        SkiPreparation::HttpAcquisitionRequest Request;
        Request.Url = Url;
        Request.Product = SkiPreparation::ProviderProduct::WorldCover;
        Request.Tile = {TileIndex - 1, 0, TileCount, 1, 0, 0, 0, 0};
        Request.ActivityTimeoutSeconds = Policy.ActivityTimeoutSeconds;
        Request.TotalTimeoutSeconds = Policy.TotalTimeoutSeconds;
        Request.MaximumResponseBytes = FMath::Min<uint64>(Length, 1024ULL * 1024ULL);
        Request.ByteRange = SkiPreparation::HttpByteRange{Offset, Length};
        return DownloadWithRetry(Transport, Request, Policy, PreparationRequest, Cancellation,
            OperationBegan, TileIndex, TileCount, BeforeAttempt, OutResult, OutFailure);
    }

    SkiPreparation::IAcquisitionTransport& Transport;
    FString Url;
    const SkiPreparation::Request& PreparationRequest;
    TSharedRef<SkiPreparation::Cancellation> Cancellation;
    double OperationBegan = 0.0;
    SkiPreparation::RetryPolicy Policy;
    int32 TileIndex = 0;
    int32 TileCount = 0;
    TFunction<void(const SkiPreparation::HttpAcquisitionRequest&, int32, int32)> BeforeAttempt;
    uint64 TotalSize = 0;
    uint64 CachedBytes = 1024ULL * 1024ULL;
    TMap<uint64, TArray<uint8>> Cache;
    TArray<uint64> CacheOrder;
};

/** Native WorldCover grid window: whole source pixels on the global lattice. */
struct CoverGridWindow
{
    SkiDomain::GeographicBounds OuterBounds;
    double StepDeg = 0.0;
    uint32 Width = 0;
    uint32 Height = 0;
    FString SourceTiles;
};

bool AcquireWorldCover(SkiPreparation::IAcquisitionTransport& Transport,
    const SkiDomain::GeographicBounds& Bounds, const SkiPreparation::Request& PreparationRequest,
    const TSharedRef<SkiPreparation::Cancellation>& Cancellation, const double OperationBegan,
    const SkiPreparation::RetryPolicy& Policy,
    const TFunction<void(const SkiPreparation::HttpAcquisitionRequest&, int32, int32)>& BeforeAttempt,
    TArray<uint8>& OutCover, TArray<uint8>& OutValidity, CoverGridWindow& OutGrid,
    FString& OutError, SkiPreparation::ProviderFailure& OutFailure)
{
    const auto Fail = [&](const TCHAR* Code, const FString& Summary,
        const SkiPreparation::RetryClassification Retry)
    {
        OutError = Summary;
        OutFailure = {};
        OutFailure.Code = Code;
        OutFailure.Stage = SkiPreparation::FailureStage::Derivation;
        OutFailure.Product = SkiPreparation::ProviderProduct::WorldCover;
        OutFailure.Retry = Retry;
        OutFailure.Summary = Summary;
        return false;
    };
    const int32 MinWest = FMath::FloorToInt(Bounds.WestDeg / 3.0) * 3;
    const int32 MaxWest = FMath::FloorToInt((Bounds.EastDeg - 1.0e-10) / 3.0) * 3;
    const int32 MinSouth = FMath::FloorToInt(Bounds.SouthDeg / 3.0) * 3;
    const int32 MaxSouth = FMath::FloorToInt((Bounds.NorthDeg - 1.0e-10) / 3.0) * 3;
    const int32 TileCount = ((MaxWest - MinWest) / 3 + 1) * ((MaxSouth - MinSouth) / 3 + 1);
    int32 TileIndex = 0;
    TArray<SkiPreparation::DecodedCoverWindow> Tiles;
    TArray<FString> Names;
    for (int32 South = MinSouth; South <= MaxSouth; South += 3)
    {
        for (int32 West = MinWest; West <= MaxWest; West += 3)
        {
            ++TileIndex;
            const FString Name = WorldCoverTileName(South, West);
            Names.Add(Name);
            const FString Url = FString::Printf(TEXT("https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/ESA_WorldCover_10m_2021_v200_%s_Map.tif"), *Name);
            FHttpCogSource Source(Transport, Url, PreparationRequest, Cancellation, OperationBegan,
                Policy, TileIndex, TileCount, BeforeAttempt);
            if (!Source.Initialize(OutFailure)) { OutError = OutFailure.Summary; return false; }
            // Request the footprint expanded by one source pixel so every lattice cell that
            // touches the terrain is decoded, not only those whose centres fall inside it.
            constexpr double Margin = 2.0 / 12000.0;
            SkiDomain::GeographicBounds Intersection{
                FMath::Max(Bounds.WestDeg - Margin, static_cast<double>(West)),
                FMath::Max(Bounds.SouthDeg - Margin, static_cast<double>(South)),
                FMath::Min(Bounds.EastDeg + Margin, static_cast<double>(West + 3)),
                FMath::Min(Bounds.NorthDeg + Margin, static_cast<double>(South + 3))};
            SkiPreparation::DecodedCoverWindow Window;
            bool bDecoded = false;
            const bool bDecodeJobRan = SkiPreparation::ExecuteBoundedDecodeJob(Cancellation,
                [&]() { return OperationCurrent(PreparationRequest, Cancellation); }, [&]()
                {
                    bDecoded = SkiPreparation::DecodeWorldCoverCogWindow(Source, Intersection,
                        Window, OutFailure, [&]() { return !OperationCurrent(PreparationRequest, Cancellation); });
                    return true;
                });
            if (!bDecodeJobRan || !bDecoded)
            {
                if (OutFailure.Summary.IsEmpty()) OutFailure.Summary = TEXT("WorldCover COG decode was cancelled.");
                OutError = OutFailure.Summary;
                return false;
            }
            Tiles.Add(MoveTemp(Window));
        }
    }
    // Every tile must share one global lattice; mixed scales cannot be mosaicked honestly.
    const double Step = Tiles[0].LongitudeSpacingDeg;
    for (const SkiPreparation::DecodedCoverWindow& Tile : Tiles)
    {
        if (Step <= 0.0 || FMath::Abs(Tile.LongitudeSpacingDeg - Step) > Step * 1.0e-9
            || FMath::Abs(Tile.LatitudeSpacingDeg - Step) > Step * 1.0e-9)
            return Fail(TEXT("WORLDCOVER_GRID_MISMATCH"),
                TEXT("WorldCover tiles do not share one native lattice."),
                SkiPreparation::RetryClassification::NotRetryable);
    }
    // Round outward so the window always contains the footprint; an edge already on the
    // lattice may gain one extra (still decoded) cell, never lose coverage.
    const int64 FirstColumn = static_cast<int64>(FMath::FloorToDouble(Bounds.WestDeg / Step - 1.0e-9));
    const int64 EndColumn = static_cast<int64>(FMath::CeilToDouble(Bounds.EastDeg / Step + 1.0e-9));
    const int64 FirstRow = static_cast<int64>(FMath::FloorToDouble(-Bounds.NorthDeg / Step - 1.0e-9));
    const int64 EndRow = static_cast<int64>(FMath::CeilToDouble(-Bounds.SouthDeg / Step + 1.0e-9));
    const int64 Width = EndColumn - FirstColumn;
    const int64 Height = EndRow - FirstRow;
    if (Width < 2 || Height < 2 || Width * Height > 16000000)
        return Fail(TEXT("WORLDCOVER_WINDOW_INVALID"), TEXT("WorldCover native window is empty or exceeds the cover cell limit."),
            SkiPreparation::RetryClassification::ChangeSelection);
    OutGrid.StepDeg = Step;
    OutGrid.Width = static_cast<uint32>(Width);
    OutGrid.Height = static_cast<uint32>(Height);
    OutGrid.OuterBounds = {FirstColumn * Step, -EndRow * Step, EndColumn * Step, -FirstRow * Step};
    OutGrid.SourceTiles = FString::Join(Names, TEXT(","));
    OutCover.Init(0, static_cast<int32>(Width * Height));
    OutValidity.Init(0, OutCover.Num());
    int64 MissingInFootprint = 0;
    for (int64 Row = 0; Row < Height; ++Row)
    {
        if (!OperationCurrent(PreparationRequest, Cancellation))
            return Fail(TEXT("PREPARATION_CANCELLED"), TEXT("WorldCover derivation cancelled."),
                SkiPreparation::RetryClassification::Retryable);
        const double Latitude = -(FirstRow + Row + 0.5) * Step;
        for (int64 Column = 0; Column < Width; ++Column)
        {
            const double Longitude = (FirstColumn + Column + 0.5) * Step;
            const int32 OutputIndex = static_cast<int32>(Row * Width + Column);
            for (const SkiPreparation::DecodedCoverWindow& Tile : Tiles)
            {
                // Exact lattice mapping: the output cell centre is a source pixel centre.
                const int64 PX = FMath::RoundToInt64((Longitude - Tile.SampleCenterBounds.WestDeg) / Step);
                const int64 PY = FMath::RoundToInt64((Tile.SampleCenterBounds.NorthDeg - Latitude) / Step);
                if (PX < 0 || PY < 0 || PX >= Tile.Width || PY >= Tile.Height) continue;
                const int32 SourceIndex = static_cast<int32>(PY * Tile.Width + PX);
                if (Tile.Validity[SourceIndex])
                {
                    OutCover[OutputIndex] = Tile.Classes[SourceIndex];
                    OutValidity[OutputIndex] = 1;
                }
                break;
            }
            if (!OutValidity[OutputIndex]) ++MissingInFootprint;
        }
    }
    if (MissingInFootprint > 0)
        return Fail(TEXT("WORLDCOVER_NODATA_IN_FOOTPRINT"), FString::Printf(
            TEXT("WorldCover has %lld nodata or missing cells inside the selected terrain."), MissingInFootprint),
            SkiPreparation::RetryClassification::ChangeSelection);
    return true;
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
    if (!Transport) Transport = MakeShared<SkiNetGateway, ESPMode::ThreadSafe>();
    InitializePreparationDiagnostics(DataRoot);
}

SkiPreparation::Result SkiPreparation::NativeTerrainProvider::Prepare(const Request& RequestValue,
    const TSharedRef<Cancellation>& CancellationValue, const ProgressCallback& OnProgress)
{
    Result Output;
    const double Began = FPlatformTime::Seconds();
    RetryPolicy Policy;
    Policy.OperationDeadlineSeconds = 300.0;
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
        if (!OperationCurrent(RequestValue, CancellationValue))
        {
            // Whatever stage observed it, a failure caused by cancellation reports as one.
            Output.FinalState = State::Cancelled;
            Failure.Code = TEXT("PREPARATION_CANCELLED");
            Failure.Retry = RetryClassification::Retryable;
            if (Failure.Summary.IsEmpty())
                Failure.Summary = TEXT("Preparation was cancelled or superseded before activation.");
        }
        Output.Error = Failure.Summary;
        WriteFailureDiagnostic(DataRoot, RequestValue, Failure);
        AppendPreparationEvent(DataRoot, &RequestValue, TEXT("FAILURE"), Output.FinalState,
            Failure.Product, &Failure);
        Output.Failure = std::move(Failure);
    };
    auto FinishCancelled = [&]()
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("PREPARATION_CANCELLED");
        Failure.Stage = FailureStage::Validation;
        Failure.Product = ActiveProduct;
        Failure.Retry = RetryClassification::Retryable;
        Failure.Summary = TEXT("Preparation was cancelled or superseded before activation.");
        Output.FinalState = State::Cancelled;
        Output.Error = Failure.Summary;
        AppendPreparationEvent(DataRoot, &RequestValue, TEXT("CANCELLED"), State::Cancelled,
            ActiveProduct, &Failure);
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
    if (!OperationCurrent(RequestValue, CancellationValue)) { FinishCancelled(); return Output; }
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
    ElevationSourceMetadata SourceMetadata;
    {
        const double CenterLatitude = (CoreRaster.ActualOuterBounds.SouthDeg + CoreRaster.ActualOuterBounds.NorthDeg) * 0.5;
        const double PixelDegrees = FMath::Max(1.0e-6,
            (CoreRaster.ActualOuterBounds.NorthDeg - CoreRaster.ActualOuterBounds.SouthDeg)
            / FMath::Max<uint32>(1U, CoreRaster.Heightfield.Height));
        HttpAcquisitionRequest MetadataRequest;
        MetadataRequest.Url = ElevationSamplesUrl(CoreRaster.ActualOuterBounds, PixelDegrees);
        MetadataRequest.Product = ProviderProduct::CoreElevation;
        MetadataRequest.ActivityTimeoutSeconds = Policy.ActivityTimeoutSeconds;
        MetadataRequest.TotalTimeoutSeconds = Policy.TotalTimeoutSeconds;
        MetadataRequest.MaximumResponseBytes = 256ULL * 1024ULL;
        HttpAcquisitionResult MetadataResult;
        ProviderFailure MetadataFailure;
        if (DownloadWithRetry(*Transport, MetadataRequest, Policy, RequestValue, CancellationValue,
                Began, 1, 1, BeforeAttempt, MetadataResult, MetadataFailure))
            ParseElevationSourceMetadata(MetadataResult.Bytes, CenterLatitude, SourceMetadata);
        if (!SourceMetadata.bQueried)
            Output.Warnings.Add(TEXT("USGS did not report source products for this area; native spacing and datum are labelled not reported."));
    }
    if (!OperationCurrent(RequestValue, CancellationValue)) { FinishCancelled(); return Output; }
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
    TArray<uint8> CoverValidity;
    CoverGridWindow CoverGrid;
    ProviderFailure CoverFailure;
    if (!AcquireWorldCover(*Transport, ActualBounds, RequestValue, CancellationValue, Began,
            Policy, BeforeAttempt, Cover, CoverValidity, CoverGrid, Output.Error, CoverFailure))
    {
        if (Output.Error.IsEmpty()) Output.Error = TEXT("WorldCover contains missing cells.");
        if (CoverFailure.Summary.IsEmpty()) CoverFailure.Summary = Output.Error;
        FinishFailure(std::move(CoverFailure));
        return Output;
    }
    const uint32 CoverWidth = CoverGrid.Width;
    const uint32 CoverHeight = CoverGrid.Height;
    ActiveProduct = ProviderProduct::None;
    if (!OperationCurrent(RequestValue, CancellationValue)) { FinishCancelled(); return Output; }
    Report(State::Deriving, 5, TEXT("Preparing TerrainCore and native-grid analytical cover"));
    Output.Warnings.Add(TEXT("Optional NAIP imagery was not requested for Medium."));
    Output.Warnings.Add(TEXT("Optional Overpass vector context was not requested for Medium."));
    SkiDomain::TerrainManifest Manifest;
    Manifest.Name = TCHAR_TO_UTF8(*RequestValue.Name);
    Manifest.Source = "USGS 3DEP; ESA WorldCover 2021 v200";
    Manifest.RequestedAtUtc = TCHAR_TO_UTF8(*FDateTime::UtcNow().ToIso8601());
    Manifest.RequestedBounds = RequestValue.Bounds;
    Manifest.ActualBounds = ActualBounds;
    Manifest.LocalOrigin = {(ActualBounds.SouthDeg + ActualBounds.NorthDeg) * 0.5,
        (ActualBounds.WestDeg + ActualBounds.EastDeg) * 0.5,
        Core.Samples[static_cast<size_t>(Core.Height / 2) * Core.Width + Core.Width / 2]};
    Manifest.VerticalDatum = TCHAR_TO_UTF8(*SourceMetadata.VerticalDatum);
    Manifest.HeightWidth = Core.Width;
    Manifest.HeightHeight = Core.Height;
    Manifest.EastSpacingM = Core.EastSpacingM;
    Manifest.NorthSpacingM = Core.NorthSpacingM;
    Manifest.CoverWidth = CoverWidth;
    Manifest.CoverHeight = CoverHeight;
    if (!OperationCurrent(RequestValue, CancellationValue)) { FinishCancelled(); return Output; }
    Report(State::WritingStaging, 6, TEXT("Writing TerrainCore and analytical CoverEcology staging"));

    SkiDomain::TerrainCoreManifest TerrainCore;
    TerrainCore.GeneratorVersion = "mountain-planner-terraincore-v2";
    TerrainCore.ProcessingVersions = {"usgs-3dep-medium-export-v1", "terraincore-derivation-v1"};
    TerrainCore.LocalOrigin = Manifest.LocalOrigin;
    TerrainCore.Width = Core.Width;
    TerrainCore.Height = Core.Height;
    TerrainCore.DeliveredEastSpacingM = Core.EastSpacingM;
    TerrainCore.DeliveredNorthSpacingM = Core.NorthSpacingM;
    TerrainCore.Registration = SkiDomain::PixelRegistration::SampleCenter;
    TerrainCore.SampleCenterBounds = {Core.WestM, Core.SampleNorthM(Core.Height - 1U),
        Core.EastM(Core.Width - 1U), Core.NorthM};
    SkiDomain::ComputeTerrainCoreBounds(TerrainCore.Width, TerrainCore.Height,
        TerrainCore.DeliveredEastSpacingM, TerrainCore.DeliveredNorthSpacingM,
        TerrainCore.SampleCenterBounds, TerrainCore.OuterBounds);
    TerrainCore.Source.SourceId = "usgs-3dep-export";
    TerrainCore.Source.Product = TCHAR_TO_UTF8(*FString::Printf(
        TEXT("USGS 3DEP dynamic export (Medium), bilinear from: %s"), *SourceMetadata.Products).Left(250));
    TerrainCore.Source.AcquisitionEpoch = TCHAR_TO_UTF8(*SourceMetadata.AcquisitionEpoch);
    TerrainCore.Source.HorizontalCrs = "EPSG:4326/WGS84 to local ENU";
    TerrainCore.Source.HorizontalDatum = "WGS84";
    TerrainCore.Source.VerticalDatum = Manifest.VerticalDatum;
    TerrainCore.Source.License = "USGS public domain";
    TerrainCore.Source.Attribution = "USGS 3D Elevation Program";
    // Native spacing is claimed only when every sampled location reports one source resolution.
    TerrainCore.Source.NativeSpacingReported = SourceMetadata.bNativeSpacingUniform;
    TerrainCore.Source.NativeEastSpacingM = SourceMetadata.bNativeSpacingUniform ? SourceMetadata.NativeEastSpacingM : 0.0;
    TerrainCore.Source.NativeNorthSpacingM = SourceMetadata.bNativeSpacingUniform ? SourceMetadata.NativeNorthSpacingM : 0.0;

    // Required surround: its own TerrainCore, placed in the core's local frame.
    SkiDomain::TerrainCoreManifest SurroundCore = TerrainCore;
    {
        const double OriginLatitude = Manifest.LocalOrigin.LatitudeDeg;
        const double Cosine = std::cos(FMath::DegreesToRadians(OriginLatitude));
        const double SurroundCenterLon = (SurroundRaster.ActualOuterBounds.WestDeg + SurroundRaster.ActualOuterBounds.EastDeg) * 0.5;
        const double SurroundCenterLat = (SurroundRaster.ActualOuterBounds.SouthDeg + SurroundRaster.ActualOuterBounds.NorthDeg) * 0.5;
        Surround.WestM += (SurroundCenterLon - Manifest.LocalOrigin.LongitudeDeg) * 111320.0 * Cosine;
        Surround.NorthM += (SurroundCenterLat - OriginLatitude) * 111320.0;
    }
    SurroundCore.Width = Surround.Width;
    SurroundCore.Height = Surround.Height;
    SurroundCore.DeliveredEastSpacingM = Surround.EastSpacingM;
    SurroundCore.DeliveredNorthSpacingM = Surround.NorthSpacingM;
    SurroundCore.SampleCenterBounds = {Surround.WestM, Surround.SampleNorthM(Surround.Height - 1U),
        Surround.EastM(Surround.Width - 1U), Surround.NorthM};
    SkiDomain::ComputeTerrainCoreBounds(SurroundCore.Width, SurroundCore.Height,
        SurroundCore.DeliveredEastSpacingM, SurroundCore.DeliveredNorthSpacingM,
        SurroundCore.SampleCenterBounds, SurroundCore.OuterBounds);
    SurroundCore.Source.SourceId = "usgs-3dep-surround";
    SurroundCore.Source.Product = "USGS 3DEP required surrounding elevation (Standard export)";

    TerrainCorePackageStore CoreStore(DataRoot);
    FString CoreDirectory;
    if (!CoreStore.WriteAndActivate(TerrainCore, Core, CoreDirectory,
            Output.TerrainCoreManifest, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("TERRAINCORE_WRITE_FAILED");
        Failure.Stage = FailureStage::Writing;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }

    TerrainCorePackageStore SurroundStore(DataRoot);
    FString SurroundDirectory;
    if (!SurroundStore.WriteAndActivate(SurroundCore, Surround, SurroundDirectory,
            Output.SurroundTerrainCoreManifest, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        if (!OperationCurrent(RequestValue, CancellationValue)) { FinishCancelled(); return Output; }
        ProviderFailure Failure;
        Failure.Code = TEXT("SURROUND_WRITE_FAILED");
        Failure.Stage = FailureStage::Writing;
        Failure.Product = ProviderProduct::SurroundingElevation;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }

    SkiDomain::CoverEcologyManifest Ecology;
    Ecology.GeneratorVersion = "mountain-planner-cover-ecology-v1";
    Ecology.CoverRevision = 1;
    // The cover is an unresampled window of whole source pixels on WorldCover's global lattice.
    Ecology.Source = {"esa-worldcover-2021-v200", "ESA WorldCover analytical class COG",
        "2021", TCHAR_TO_UTF8(*FString::Printf(TEXT("official-class-value-cog-native-grid-window:%s"),
            *CoverGrid.SourceTiles).Left(250)), "CC BY 4.0",
        "ESA WorldCover project / Contains modified Copernicus Sentinel data (2021)"};
    Ecology.Transform.Width = CoverWidth;
    Ecology.Transform.Height = CoverHeight;
    Ecology.Transform.LongitudeStepDeg = CoverGrid.StepDeg;
    Ecology.Transform.LatitudeStepDeg = CoverGrid.StepDeg;
    Ecology.Transform.SampleCenterBounds = {
        CoverGrid.OuterBounds.WestDeg + CoverGrid.StepDeg * 0.5,
        CoverGrid.OuterBounds.SouthDeg + CoverGrid.StepDeg * 0.5,
        CoverGrid.OuterBounds.EastDeg - CoverGrid.StepDeg * 0.5,
        CoverGrid.OuterBounds.NorthDeg - CoverGrid.StepDeg * 0.5};
    SkiDomain::ComputeCoverEcologyOuterBounds(CoverWidth, CoverHeight,
        Ecology.Transform.LongitudeStepDeg, Ecology.Transform.LatitudeStepDeg,
        Ecology.Transform.SampleCenterBounds, Ecology.Transform.OuterBounds);
    TArray<uint8> PackedValidity;
    PackedValidity.Init(0, FMath::DivideAndRoundUp(CoverValidity.Num(), 8));
    for (int32 Index = 0; Index < CoverValidity.Num(); ++Index)
        if (CoverValidity[Index]) PackedValidity[Index / 8] |= static_cast<uint8>(1U << (Index % 8));
    CoverEcologyStore EcologyStore(DataRoot);
    FString EcologyDirectory;
    if (!EcologyStore.WriteAndActivate(Ecology, Cover, PackedValidity, EcologyDirectory,
            Output.CoverEcologyManifest, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("COVER_ECOLOGY_WRITE_FAILED");
        Failure.Stage = FailureStage::Verification;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }

    SkiDomain::InstalledTerrainReceipt Installation;
    Installation.GeneratorVersion = "mountain-planner-installed-terrain-v1";
    Installation.TerrainCoreId = Output.TerrainCoreManifest.ContentId;
    Installation.SurroundTerrainCoreId = Output.SurroundTerrainCoreManifest.ContentId;
    Installation.CoverEcologyId = Output.CoverEcologyManifest.ContentId;
    // NAIP is not requested by the Medium tier; recording it as unavailable would be a false
    // acquisition claim.
    Installation.OptionalSources = {
        {"naip", "USDA NAIP RGB+NIR", SkiDomain::OptionalSourceStatus::NotRequested, {},
            "NOT_REQUESTED_IN_P1_MEDIUM", "USGS public domain", "USDA/USGS"},
        {"overpass", "OpenStreetMap vector context", SkiDomain::OptionalSourceStatus::NotRequested, {},
            "NOT_REQUESTED_IN_P1_MEDIUM", "ODbL 1.0", "OpenStreetMap contributors"}};
    if (!OperationCurrent(RequestValue, CancellationValue)) { FinishCancelled(); return Output; }
    Report(State::Activating, 7, TEXT("Activating the composite installation"));
    InstalledTerrainStore InstallationStore(DataRoot);
    if (!InstallationStore.WriteAndActivate(Installation, Output.PackageDirectory,
            Output.InstallationReceipt, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("COMPOSITE_ACTIVATION_FAILED");
        Failure.Stage = FailureStage::Activation;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }
    if (!OperationCurrent(RequestValue, CancellationValue))
        Output.Warnings.Add(TEXT("Cancellation arrived after the composite installation was committed; the installation stands."));
    Report(State::Verifying, 7, TEXT("Reopening TerrainCore, CoverEcology, and composite installation"));
    TerrainCorePackageIndex CoreIndex;
    CoverEcologyPackageIndex EcologyIndex;
    InstalledTerrainIndex InstallationIndex;
    if (!CoreStore.Open(UTF8_TO_TCHAR(Output.TerrainCoreManifest.ContentId.c_str()), CoreIndex, Output.Error)
        || !EcologyStore.Open(UTF8_TO_TCHAR(Output.CoverEcologyManifest.ContentId.c_str()), EcologyIndex, Output.Error)
        || !InstallationStore.Open(UTF8_TO_TCHAR(Output.InstallationReceipt.ContentId.c_str()), InstallationIndex, Output.Error))
    {
        ProviderFailure Failure;
        Failure.Code = TEXT("COMPOSITE_VERIFY_FAILED");
        Failure.Stage = FailureStage::Verification;
        Failure.Summary = Output.Error;
        FinishFailure(std::move(Failure));
        return Output;
    }
    Output.Manifest = MoveTemp(Manifest);
    Output.Heightfield = MoveTemp(Core);
    Output.SurroundHeightfield = MoveTemp(Surround);
    Output.Cover = MoveTemp(Cover);
    Output.CoverValidity = MoveTemp(CoverValidity);
    Output.HasNativeV2Installation = true;
    Output.Ok = true;
    Output.FinalState = State::Installed;
    Report(State::Installed, 8, TEXT("Native terrain package installed"));
    return Output;
}
