#include "SkiPreparation/TerrainAcquisition.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ScopeLock.h"
#include "SkiDomain/Coordinates.h"

#include <atomic>
#include <cmath>

namespace
{
struct FRequestState
{
    FRequestState() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
    ~FRequestState() { FPlatformProcess::ReturnSynchEventToPool(Event); }
    FEvent* Event = nullptr;
    FCriticalSection ResultMutex;
    SkiPreparation::HttpAcquisitionResult Result;
    TSharedPtr<SkiPreparation::AcquisitionResourceLease, ESPMode::ThreadSafe> BackendLifetime;
    std::atomic_bool Done = false;
    std::atomic_bool TooLarge = false;
    double BeganSeconds = 0.0;
};

SkiPreparation::TransportFailureReason MapFailure(const EHttpFailureReason Reason)
{
    using SkiPreparation::TransportFailureReason;
    switch (Reason)
    {
    case EHttpFailureReason::ConnectionError: return TransportFailureReason::ConnectionError;
    case EHttpFailureReason::TimedOut: return TransportFailureReason::TimedOut;
    case EHttpFailureReason::Cancelled: return TransportFailureReason::Cancelled;
    case EHttpFailureReason::ResponseTooLarge: return TransportFailureReason::ResponseTooLarge;
    case EHttpFailureReason::None: return TransportFailureReason::Other;
    default: return TransportFailureReason::Other;
    }
}

bool NearlyEqual(const double A, const double B, const double AbsoluteTolerance, const double RelativeTolerance = 1.0e-5)
{
    return FMath::Abs(A - B) <= FMath::Max(AbsoluteTolerance,
        RelativeTolerance * FMath::Max(FMath::Abs(A), FMath::Abs(B)));
}

FCriticalSection ResourceGateMutex;
int32 ActiveNetworkRequests = 0;
int32 ActiveElevationRequests = 0;
int32 ActiveDecodeJobs = 0;

bool IsElevationProduct(const SkiPreparation::ProviderProduct Product)
{
    return Product == SkiPreparation::ProviderProduct::CoreElevation
        || Product == SkiPreparation::ProviderProduct::SurroundingElevation;
}

class FNetworkPermit final : public SkiPreparation::AcquisitionResourceLease
{
public:
    bool Acquire(const SkiPreparation::ProviderProduct Product,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation, const double Deadline,
        const TFunction<bool()>& IsCurrent)
    {
        bElevation = IsElevationProduct(Product);
        while (!Cancellation->IsCancelled() && (!IsCurrent || IsCurrent())
            && (Deadline <= 0.0 || FPlatformTime::Seconds() < Deadline))
        {
            {
                FScopeLock Lock(&ResourceGateMutex);
                if (ActiveNetworkRequests < 4 && (!bElevation || ActiveElevationRequests < 2))
                {
                    ++ActiveNetworkRequests;
                    if (bElevation) ++ActiveElevationRequests;
                    bAcquired = true;
                    return true;
                }
            }
            FPlatformProcess::SleepNoStats(0.005F);
        }
        return false;
    }

    ~FNetworkPermit()
    {
        if (!bAcquired) return;
        FScopeLock Lock(&ResourceGateMutex);
        --ActiveNetworkRequests;
        if (bElevation) --ActiveElevationRequests;
    }

private:
    bool bAcquired = false;
    bool bElevation = false;
};

class FDecodePermit
{
public:
    bool Acquire(const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
        const TFunction<bool()>& IsCurrent)
    {
        while (!Cancellation->IsCancelled() && (!IsCurrent || IsCurrent()))
        {
            {
                FScopeLock Lock(&ResourceGateMutex);
                if (ActiveDecodeJobs < 2)
                {
                    ++ActiveDecodeJobs;
                    bAcquired = true;
                    return true;
                }
            }
            FPlatformProcess::SleepNoStats(0.005F);
        }
        return false;
    }

    ~FDecodePermit()
    {
        if (!bAcquired) return;
        FScopeLock Lock(&ResourceGateMutex);
        --ActiveDecodeJobs;
    }

private:
    bool bAcquired = false;
};
}

SkiPreparation::HttpAcquisitionResult SkiPreparation::UnrealHttpAcquisitionTransport::Get(
    const HttpAcquisitionRequest& Request, const TSharedRef<Cancellation>& Cancellation)
{
    const TSharedRef<FRequestState> State = MakeShared<FRequestState>();
    State->BackendLifetime = Request.BackendLifetime;
    State->BeganSeconds = FPlatformTime::Seconds();
    const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
    Http->SetURL(Request.Url);
    Http->SetVerb(TEXT("GET"));
    Http->SetHeader(TEXT("User-Agent"), TEXT("MountainPlanner-Unreal-P1/1"));
    Http->SetActivityTimeout(Request.ActivityTimeoutSeconds);
    Http->SetTimeout(Request.TotalTimeoutSeconds);
    Http->OnHeaderReceived().BindLambda([State, Maximum = Request.MaximumResponseBytes](FHttpRequestPtr Active,
        const FString& HeaderName, const FString& HeaderValue)
    {
        if (!HeaderName.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase)) return;
        const FString Trimmed = HeaderValue.TrimStartAndEnd();
        uint64 Length = 0;
        bool bDigitsOnly = !Trimmed.IsEmpty();
        for (const TCHAR Character : Trimmed) bDigitsOnly &= FChar::IsDigit(Character);
        const bool bInvalidOrLarge = !bDigitsOnly || !LexTryParseString(Length, *Trimmed) || Length > Maximum;
        if (bInvalidOrLarge && !State->TooLarge.exchange(true)) Active->CancelRequest();
    });
    Http->OnRequestProgress64().BindLambda([State, Maximum = Request.MaximumResponseBytes](FHttpRequestPtr Active, uint64, const uint64 Received)
    {
        bool bCancel = false;
        {
            FScopeLock Lock(&State->ResultMutex);
            State->Result.BytesReceived = Received;
            if (Received > 0 && State->Result.TimeToFirstByteSeconds < 0.0)
                State->Result.TimeToFirstByteSeconds = FPlatformTime::Seconds() - State->BeganSeconds;
            bCancel = Received > Maximum && !State->TooLarge.exchange(true);
        }
        if (bCancel) Active->CancelRequest();
    });
    Http->OnProcessRequestComplete().BindLambda([State, Maximum = Request.MaximumResponseBytes](FHttpRequestPtr Completed,
        FHttpResponsePtr Response, const bool Connected)
    {
        FScopeLock Lock(&State->ResultMutex);
        State->Result.ElapsedSeconds = FPlatformTime::Seconds() - State->BeganSeconds;
        State->Result.RequestStatus = EHttpRequestStatus::ToString(Completed->GetStatus());
        State->Result.HttpStatus = Response ? Response->GetResponseCode() : 0;
        State->Result.ContentType = Response ? Response->GetContentType().Left(128) : FString();
        State->Result.RetryAfter = Response ? Response->GetHeader(TEXT("Retry-After")).Left(64) : FString();
        const int64 ContentLength = Response ? Response->GetContentLength() : 0;
        if (State->TooLarge.load() || ContentLength < 0 || static_cast<uint64>(ContentLength) > Maximum)
            State->Result.FailureReason = SkiPreparation::TransportFailureReason::ResponseTooLarge;
        else if (!Connected || !Response)
            State->Result.FailureReason = MapFailure(Completed->GetFailureReason());
        else if (!EHttpResponseCodes::IsOk(State->Result.HttpStatus))
            State->Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
        else
        {
            State->Result.Bytes = Response->GetContent();
            State->Result.BytesReceived = State->Result.Bytes.Num();
            if (State->Result.Bytes.IsEmpty()) State->Result.FailureReason = SkiPreparation::TransportFailureReason::EmptyResponse;
        }
        State->Done.store(true, std::memory_order_release);
        State->BackendLifetime.Reset();
        State->Event->Trigger();
    });
    if (!Http->ProcessRequest())
    {
        State->Result.FailureReason = TransportFailureReason::QueueFailure;
        State->Result.RequestStatus = TEXT("NotStarted");
        State->BackendLifetime.Reset();
        return State->Result;
    }
    while (!State->Done.load(std::memory_order_acquire))
    {
        if (Cancellation->IsCancelled())
        {
            Http->CancelRequest();
            HttpAcquisitionResult Cancelled;
            Cancelled.FailureReason = TransportFailureReason::Cancelled;
            Cancelled.RequestStatus = TEXT("Cancelled");
            Cancelled.ElapsedSeconds = FPlatformTime::Seconds() - State->BeganSeconds;
            return Cancelled;
        }
        if (Request.AbsoluteOperationDeadlineSeconds > 0.0
            && FPlatformTime::Seconds() >= Request.AbsoluteOperationDeadlineSeconds)
        {
            Http->CancelRequest();
            HttpAcquisitionResult TimedOut;
            TimedOut.FailureReason = TransportFailureReason::TimedOut;
            TimedOut.RequestStatus = TEXT("OperationDeadline");
            TimedOut.ElapsedSeconds = FPlatformTime::Seconds() - State->BeganSeconds;
            return TimedOut;
        }
        State->Event->Wait(25);
    }
    FScopeLock Lock(&State->ResultMutex);
    return State->Result;
}

SkiPreparation::AcquisitionPlan SkiPreparation::BuildElevationAcquisitionPlan(
    const SkiDomain::GeographicBounds& Bounds, const SourceProfile Profile, const uint32 MaximumTileAxis)
{
    AcquisitionPlan Plan;
    const SkiDomain::GeodeticPoint Origin{(Bounds.SouthDeg + Bounds.NorthDeg) * 0.5,
        (Bounds.WestDeg + Bounds.EastDeg) * 0.5, 0.0};
    SkiDomain::LocalFrame Frame;
    if (!SkiDomain::TryMakeLocalFrame(Origin, Frame) || MaximumTileAxis < 2) return Plan;
    const SkiDomain::EnuPoint West = SkiDomain::ToEnu(Frame, {Origin.LatitudeDeg, Bounds.WestDeg, 0.0});
    const SkiDomain::EnuPoint East = SkiDomain::ToEnu(Frame, {Origin.LatitudeDeg, Bounds.EastDeg, 0.0});
    const SkiDomain::EnuPoint South = SkiDomain::ToEnu(Frame, {Bounds.SouthDeg, Origin.LongitudeDeg, 0.0});
    const SkiDomain::EnuPoint North = SkiDomain::ToEnu(Frame, {Bounds.NorthDeg, Origin.LongitudeDeg, 0.0});
    Plan.WidthM = FMath::Abs(East.EastM - West.EastM);
    Plan.HeightM = FMath::Abs(North.NorthM - South.NorthM);
    if (!FMath::IsFinite(Plan.WidthM) || !FMath::IsFinite(Plan.HeightM) || Plan.WidthM <= 0.0 || Plan.HeightM <= 0.0) return {};
    const uint32 MaximumAxis = Profile == SourceProfile::High ? 2000U : 1000U;
    if (Plan.WidthM >= Plan.HeightM)
    {
        Plan.Width = MaximumAxis;
        Plan.Height = FMath::Clamp<uint32>(FMath::RoundToInt(MaximumAxis * Plan.HeightM / Plan.WidthM), 2U, MaximumAxis);
    }
    else
    {
        Plan.Height = MaximumAxis;
        Plan.Width = FMath::Clamp<uint32>(FMath::RoundToInt(MaximumAxis * Plan.WidthM / Plan.HeightM), 2U, MaximumAxis);
    }
    const int32 ColumnCount = FMath::DivideAndRoundUp(static_cast<int32>(Plan.Width), static_cast<int32>(MaximumTileAxis));
    const int32 RowCount = FMath::DivideAndRoundUp(static_cast<int32>(Plan.Height), static_cast<int32>(MaximumTileAxis));
    for (int32 Row = 0; Row < RowCount; ++Row)
    {
        for (int32 Column = 0; Column < ColumnCount; ++Column)
        {
            RasterTileKey Tile;
            Tile.Column = Column; Tile.Row = Row; Tile.ColumnCount = ColumnCount; Tile.RowCount = RowCount;
            Tile.StartColumn = Column * MaximumTileAxis; Tile.StartRow = Row * MaximumTileAxis;
            Tile.Width = FMath::Min(MaximumTileAxis, Plan.Width - Tile.StartColumn);
            Tile.Height = FMath::Min(MaximumTileAxis, Plan.Height - Tile.StartRow);
            Plan.Tiles.Add(Tile);
        }
    }
    return Plan;
}

bool SkiPreparation::IsRetryableTransportFailure(const HttpAcquisitionResult& Result) noexcept
{
    if (Result.FailureReason == TransportFailureReason::ConnectionError
        || Result.FailureReason == TransportFailureReason::TimedOut) return true;
    if (Result.FailureReason != TransportFailureReason::HttpStatus) return false;
    return Result.HttpStatus == 408 || Result.HttpStatus == 425 || Result.HttpStatus == 429
        || Result.HttpStatus >= 500;
}

double SkiPreparation::RetryDelaySeconds(const HttpAcquisitionResult& Result, const int32 Attempt,
    const uint32 DeterministicJitterSeed) noexcept
{
    if (!Result.RetryAfter.IsEmpty())
    {
        const FString Trimmed = Result.RetryAfter.TrimStartAndEnd();
        if (Trimmed.IsNumeric())
        {
            const double Seconds = FCString::Atod(*Trimmed);
            if (FMath::IsFinite(Seconds) && Seconds >= 0.0) return FMath::Min(30.0, Seconds);
        }
    }
    const double Base = Attempt <= 1 ? 1.0 : 2.0;
    return Base + static_cast<double>(DeterministicJitterSeed % 251U) / 1000.0;
}

bool SkiPreparation::ExecuteAcquisitionWithRetry(IAcquisitionTransport& Transport,
    HttpAcquisitionRequest Request, const RetryPolicy& Policy,
    const TSharedRef<Cancellation>& Cancellation, const double OperationBeganSeconds,
    const TFunction<bool()>& IsCurrent,
    const TFunction<void(const HttpAcquisitionRequest&)>& BeforeAttempt,
    HttpAcquisitionResult& OutResult)
{
    OutResult = {};
    for (int32 Attempt = 1; Attempt <= Policy.MaximumAttempts; ++Attempt)
    {
        if (Cancellation->IsCancelled() || (IsCurrent && !IsCurrent())
            || FPlatformTime::Seconds() - OperationBeganSeconds >= Policy.OperationDeadlineSeconds)
        {
            OutResult = {};
            OutResult.Attempt = Attempt;
            OutResult.FailureReason = TransportFailureReason::Cancelled;
            OutResult.RequestStatus = TEXT("Cancelled");
            return false;
        }
        Request.Attempt = Attempt;
        Request.AbsoluteOperationDeadlineSeconds = OperationBeganSeconds + Policy.OperationDeadlineSeconds;
        if (BeforeAttempt) BeforeAttempt(Request);
        const TSharedRef<FNetworkPermit, ESPMode::ThreadSafe> Permit =
            MakeShared<FNetworkPermit, ESPMode::ThreadSafe>();
        if (!Permit->Acquire(Request.Product, Cancellation, Request.AbsoluteOperationDeadlineSeconds, IsCurrent))
        {
            OutResult = {};
            OutResult.Attempt = Attempt;
            OutResult.FailureReason = Cancellation->IsCancelled() || (IsCurrent && !IsCurrent())
                ? TransportFailureReason::Cancelled : TransportFailureReason::TimedOut;
            OutResult.RequestStatus = OutResult.FailureReason == TransportFailureReason::Cancelled
                ? TEXT("Cancelled") : TEXT("OperationDeadline");
            return false;
        }
        Request.BackendLifetime = Permit;
        OutResult = Transport.Get(Request, Cancellation);
        Request.BackendLifetime.Reset();
        OutResult.Attempt = Attempt;
        if (OutResult.Ok()) return true;
        if (!IsRetryableTransportFailure(OutResult) || Attempt == Policy.MaximumAttempts) return false;
        const double Delay = RetryDelaySeconds(OutResult, Attempt,
            static_cast<uint32>(Request.Tile.Column * 131 + Request.Tile.Row * 67 + Attempt * 17));
        const double WaitBegan = FPlatformTime::Seconds();
        while (FPlatformTime::Seconds() - WaitBegan < Delay)
        {
            if (Cancellation->IsCancelled() || (IsCurrent && !IsCurrent()))
            {
                OutResult.FailureReason = TransportFailureReason::Cancelled;
                OutResult.RequestStatus = TEXT("Cancelled");
                return false;
            }
            FPlatformProcess::SleepNoStats(0.025F);
        }
    }
    return false;
}

bool SkiPreparation::ExecuteBoundedDecodeJob(
    const TSharedRef<Cancellation>& Cancellation,
    const TFunction<bool()>& IsCurrent,
    TFunctionRef<bool()> Job)
{
    FDecodePermit Permit;
    return Permit.Acquire(Cancellation, IsCurrent) && !Cancellation->IsCancelled()
        && (!IsCurrent || IsCurrent()) && Job();
}

const TCHAR* SkiPreparation::TransportFailureReasonName(const TransportFailureReason Value) noexcept
{
    switch (Value)
    {
    case TransportFailureReason::None: return TEXT("None");
    case TransportFailureReason::QueueFailure: return TEXT("QueueFailure");
    case TransportFailureReason::ConnectionError: return TEXT("ConnectionError");
    case TransportFailureReason::TimedOut: return TEXT("TimedOut");
    case TransportFailureReason::Cancelled: return TEXT("Cancelled");
    case TransportFailureReason::ResponseTooLarge: return TEXT("ResponseTooLarge");
    case TransportFailureReason::HttpStatus: return TEXT("HttpStatus");
    case TransportFailureReason::EmptyResponse: return TEXT("EmptyResponse");
    default: return TEXT("Other");
    }
}

bool SkiPreparation::StitchElevationTiles(const AcquisitionPlan& Plan,
    const TArray<DecodedElevationRaster>& Tiles, DecodedElevationRaster& OutRaster, FString& OutError)
{
    OutRaster = {};
    OutError.Reset();
    if (Plan.Width < 2 || Plan.Height < 2 || Plan.Tiles.Num() != Tiles.Num()
        || static_cast<uint64>(Plan.Width) * Plan.Height > SkiDomain::MaxHeightSamples)
    {
        OutError = TEXT("Elevation tile plan is invalid.");
        return false;
    }
    const DecodedElevationRaster& First = Tiles[0];
    const double EastSpacing = First.Heightfield.EastSpacingM;
    const double NorthSpacing = First.Heightfield.NorthSpacingM;
    const double NoData = First.NoDataValue;
    SkiDomain::Heightfield Field;
    Field.Width = Plan.Width; Field.Height = Plan.Height;
    Field.WestM = -static_cast<double>(Plan.Width - 1) * EastSpacing * 0.5;
    Field.NorthM = static_cast<double>(Plan.Height - 1) * NorthSpacing * 0.5;
    Field.EastSpacingM = EastSpacing; Field.NorthSpacingM = NorthSpacing;
    Field.NoDataValue = NoData; Field.CurrentRevision = 1;
    Field.Samples.assign(static_cast<size_t>(Plan.Width) * Plan.Height, static_cast<float>(NoData));
    TBitArray<> Filled(false, static_cast<int32>(static_cast<uint64>(Plan.Width) * Plan.Height));
    TMap<FIntPoint, int32> TileIndices;
    SkiDomain::GeographicBounds Outer{DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int32 Index = 0; Index < Plan.Tiles.Num(); ++Index)
    {
        const RasterTileKey& Key = Plan.Tiles[Index];
        const DecodedElevationRaster& Tile = Tiles[Index];
        const SkiDomain::GeographicBounds& Bounds = Tile.ActualOuterBounds;
        const bool bBoundsValid = FMath::IsFinite(Bounds.WestDeg) && FMath::IsFinite(Bounds.SouthDeg)
            && FMath::IsFinite(Bounds.EastDeg) && FMath::IsFinite(Bounds.NorthDeg)
            && Bounds.WestDeg < Bounds.EastDeg && Bounds.SouthDeg < Bounds.NorthDeg;
        if (Tile.Heightfield.Width != Key.Width || Tile.Heightfield.Height != Key.Height
            || Key.Width == 0 || Key.Height == 0
            || Key.StartColumn > Plan.Width || Key.Width > Plan.Width - Key.StartColumn
            || Key.StartRow > Plan.Height || Key.Height > Plan.Height - Key.StartRow
            || !NearlyEqual(Tile.Heightfield.EastSpacingM, EastSpacing, 0.02)
            || !NearlyEqual(Tile.Heightfield.NorthSpacingM, NorthSpacing, 0.02)
            || !NearlyEqual(Tile.NoDataValue, NoData, 1.0e-6)
            || Tile.Orientation != First.Orientation || !bBoundsValid
            || (Tile.SourceWidth != 0 && Tile.SourceWidth != Key.Width)
            || (Tile.SourceHeight != 0 && Tile.SourceHeight != Key.Height)
            || TileIndices.Contains(FIntPoint(Key.Column, Key.Row)))
        {
            OutError = TEXT("Elevation tile dimensions, metadata, or placement are inconsistent.");
            return false;
        }
        TileIndices.Add(FIntPoint(Key.Column, Key.Row), Index);
        Outer.WestDeg = FMath::Min(Outer.WestDeg, Tile.ActualOuterBounds.WestDeg);
        Outer.SouthDeg = FMath::Min(Outer.SouthDeg, Tile.ActualOuterBounds.SouthDeg);
        Outer.EastDeg = FMath::Max(Outer.EastDeg, Tile.ActualOuterBounds.EastDeg);
        Outer.NorthDeg = FMath::Max(Outer.NorthDeg, Tile.ActualOuterBounds.NorthDeg);
        for (uint32 Row = 0; Row < Key.Height; ++Row)
        {
            const size_t Source = static_cast<size_t>(Row) * Key.Width;
            const size_t Destination = static_cast<size_t>(Key.StartRow + Row) * Plan.Width + Key.StartColumn;
            for (uint32 Column = 0; Column < Key.Width; ++Column)
            {
                const int32 DestinationIndex = static_cast<int32>(Destination + Column);
                if (Filled[DestinationIndex])
                {
                    OutError = TEXT("Elevation tile output ranges overlap.");
                    return false;
                }
                Filled[DestinationIndex] = true;
            }
            FMemory::Memcpy(Field.Samples.data() + Destination, Tile.Heightfield.Samples.data() + Source,
                static_cast<size_t>(Key.Width) * sizeof(float));
        }
    }
    if (Filled.Find(false) != INDEX_NONE)
    {
        OutError = TEXT("Elevation tile output ranges contain a gap.");
        return false;
    }
    for (int32 Index = 0; Index < Plan.Tiles.Num(); ++Index)
    {
        const RasterTileKey& Key = Plan.Tiles[Index];
        const SkiDomain::GeographicBounds& Bounds = Tiles[Index].ActualOuterBounds;
        const double LongitudeStep = (Bounds.EastDeg - Bounds.WestDeg) / Key.Width;
        const double LatitudeStep = (Bounds.NorthDeg - Bounds.SouthDeg) / Key.Height;
        const double LongitudeTolerance = FMath::Max(1.0e-9, LongitudeStep * 1.0e-4);
        const double LatitudeTolerance = FMath::Max(1.0e-9, LatitudeStep * 1.0e-4);
        if (const int32* RightIndex = TileIndices.Find(FIntPoint(Key.Column + 1, Key.Row)))
        {
            const SkiDomain::GeographicBounds& Right = Tiles[*RightIndex].ActualOuterBounds;
            if (!NearlyEqual(Bounds.EastDeg, Right.WestDeg, LongitudeTolerance, 0.0)
                || !NearlyEqual(Bounds.SouthDeg, Right.SouthDeg, LatitudeTolerance, 0.0)
                || !NearlyEqual(Bounds.NorthDeg, Right.NorthDeg, LatitudeTolerance, 0.0))
            {
                OutError = TEXT("Elevation tile georeferencing contains a horizontal gap, overlap, or grid mismatch.");
                return false;
            }
        }
        if (const int32* BottomIndex = TileIndices.Find(FIntPoint(Key.Column, Key.Row + 1)))
        {
            const SkiDomain::GeographicBounds& Bottom = Tiles[*BottomIndex].ActualOuterBounds;
            if (!NearlyEqual(Bounds.SouthDeg, Bottom.NorthDeg, LatitudeTolerance, 0.0)
                || !NearlyEqual(Bounds.WestDeg, Bottom.WestDeg, LongitudeTolerance, 0.0)
                || !NearlyEqual(Bounds.EastDeg, Bottom.EastDeg, LongitudeTolerance, 0.0))
            {
                OutError = TEXT("Elevation tile georeferencing contains a vertical gap, overlap, or grid mismatch.");
                return false;
            }
        }
    }
    if (!SkiDomain::IsValidHeightfield(Field))
    {
        OutError = TEXT("Stitched elevation heightfield is invalid.");
        return false;
    }
    OutRaster.Heightfield = std::move(Field);
    OutRaster.ActualOuterBounds = Outer;
    OutRaster.NoDataValue = NoData;
    OutRaster.SourceWidth = Plan.Width; OutRaster.SourceHeight = Plan.Height;
    OutRaster.Storage = First.Storage; OutRaster.Compression = First.Compression; OutRaster.Orientation = First.Orientation;
    return true;
}
