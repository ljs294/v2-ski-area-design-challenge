#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiPreparation/GeoTiffDecoder.h"

namespace SkiPreparation
{
enum class TransportFailureReason : uint8
{
    None,
    QueueFailure,
    ConnectionError,
    TimedOut,
    Cancelled,
    ResponseTooLarge,
    HttpStatus,
    EmptyResponse,
    Other,
};

struct SKIPREPARATION_API RasterTileKey
{
    int32 Column = 0;
    int32 Row = 0;
    int32 ColumnCount = 1;
    int32 RowCount = 1;
    uint32 StartColumn = 0;
    uint32 StartRow = 0;
    uint32 Width = 0;
    uint32 Height = 0;
};

struct SKIPREPARATION_API AcquisitionPlan
{
    uint32 Width = 0;
    uint32 Height = 0;
    double WidthM = 0.0;
    double HeightM = 0.0;
    TArray<RasterTileKey> Tiles;
};

struct SKIPREPARATION_API RetryPolicy
{
    int32 MaximumAttempts = 3;
    float ActivityTimeoutSeconds = 90.0F;
    float TotalTimeoutSeconds = 180.0F;
    double OperationDeadlineSeconds = 300.0;
    uint64 MaximumResponseBytes = 16ULL * 1024ULL * 1024ULL;
};

struct SKIPREPARATION_API HttpByteRange
{
    uint64 Offset = 0;
    uint64 Length = 0;
};

class SKIPREPARATION_API AcquisitionResourceLease
{
public:
    virtual ~AcquisitionResourceLease() = default;
};

struct SKIPREPARATION_API HttpAcquisitionRequest
{
    FString Url;
    ProviderProduct Product = ProviderProduct::None;
    RasterTileKey Tile;
    int32 Attempt = 1;
    float ActivityTimeoutSeconds = 90.0F;
    float TotalTimeoutSeconds = 180.0F;
    double AbsoluteOperationDeadlineSeconds = 0.0;
    uint64 MaximumResponseBytes = 16ULL * 1024ULL * 1024ULL;
    /** Typed single byte range. Arbitrary caller-supplied headers are intentionally unsupported. */
    TOptional<HttpByteRange> ByteRange;
    TSharedPtr<AcquisitionResourceLease, ESPMode::ThreadSafe> BackendLifetime;
};

struct SKIPREPARATION_API HttpAcquisitionResult
{
    TArray<uint8> Bytes;
    TransportFailureReason FailureReason = TransportFailureReason::None;
    FString RequestStatus;
    FString ContentType;
    FString RetryAfter;
    FString ContentRange;
    int32 HttpStatus = 0;
    uint64 BytesReceived = 0;
    double TimeToFirstByteSeconds = -1.0;
    double ElapsedSeconds = 0.0;
    int32 Attempt = 0;
    bool Ok() const noexcept { return FailureReason == TransportFailureReason::None && HttpStatus >= 200 && HttpStatus < 300 && !Bytes.IsEmpty(); }
};

class SKIPREPARATION_API IAcquisitionTransport
{
public:
    virtual ~IAcquisitionTransport() = default;
    virtual HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& Cancellation) = 0;
};

class SKIPREPARATION_API UnrealHttpAcquisitionTransport final : public IAcquisitionTransport
{
public:
    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& Cancellation) override;
};

/**
 * Process-wide, nestable fail-closed guard for packaged workflows that must not use the
 * application HTTP acquisition port. UnrealHttpAcquisitionTransport records and rejects an
 * attempt before constructing an IHttpRequest while any guard is active.
 */
class SKIPREPARATION_API ScopedAcquisitionPortDeny final
{
public:
    ScopedAcquisitionPortDeny();
    ~ScopedAcquisitionPortDeny();
    ScopedAcquisitionPortDeny(const ScopedAcquisitionPortDeny&) = delete;
    ScopedAcquisitionPortDeny& operator=(const ScopedAcquisitionPortDeny&) = delete;
    bool IsActive() const noexcept;
    uint64 ObservedTransportCalls() const noexcept;

private:
    uint64 StartingTransportCalls = 0;
    bool bInstalled = false;
};

SKIPREPARATION_API AcquisitionPlan BuildElevationAcquisitionPlan(
    const SkiDomain::GeographicBounds& Bounds, SourceProfile Profile, uint32 MaximumTileAxis = 1000);
SKIPREPARATION_API bool IsRetryableTransportFailure(const HttpAcquisitionResult& Result) noexcept;
SKIPREPARATION_API double RetryDelaySeconds(const HttpAcquisitionResult& Result, int32 Attempt,
    uint32 DeterministicJitterSeed) noexcept;
SKIPREPARATION_API bool ExecuteAcquisitionWithRetry(IAcquisitionTransport& Transport,
    HttpAcquisitionRequest Request, const RetryPolicy& Policy,
    const TSharedRef<Cancellation>& Cancellation, double OperationBeganSeconds,
    const TFunction<bool()>& IsCurrent,
    const TFunction<void(const HttpAcquisitionRequest&)>& BeforeAttempt,
    HttpAcquisitionResult& OutResult);
SKIPREPARATION_API bool ExecuteBoundedDecodeJob(
    const TSharedRef<Cancellation>& Cancellation,
    const TFunction<bool()>& IsCurrent,
    TFunctionRef<bool()> Job);
SKIPREPARATION_API const TCHAR* TransportFailureReasonName(TransportFailureReason Value) noexcept;
/** Strictly validates bytes START-END/TOTAL for an exact typed request range. */
SKIPREPARATION_API bool ValidateContentRange(const FString& Header, const HttpByteRange& Requested,
    uint64 ReceivedBytes, uint64& OutTotalBytes) noexcept;
SKIPREPARATION_API bool StitchElevationTiles(const AcquisitionPlan& Plan,
    const TArray<DecodedElevationRaster>& Tiles, DecodedElevationRaster& OutRaster, FString& OutError);
}
