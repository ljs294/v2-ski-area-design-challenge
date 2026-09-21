#pragma once

#include "CoreMinimal.h"
#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainPackage.h"

#include <atomic>

namespace SkiPreparation
{
enum class State : uint8
{
    Selected,
    Validating,
    Acquiring,
    Decoding,
    Deriving,
    WritingStaging,
    Verifying,
    Activating,
    Installed,
    Failed,
    Cancelled,
};

enum class SourceProfile : uint8
{
    Standard,
    High,
};

enum class FailureStage : uint8
{
    Validation,
    Acquisition,
    Decoding,
    Derivation,
    Writing,
    Verification,
    Activation,
};

enum class ProviderProduct : uint8
{
    None,
    CoreElevation,
    SurroundingElevation,
    WorldCover,
    Imagery,
    VectorContext,
};

enum class RetryClassification : uint8
{
    Retryable,
    ChangeSelection,
    NotRetryable,
};

struct SKIPREPARATION_API ProviderFailure
{
    FString Code;
    FailureStage Stage = FailureStage::Validation;
    ProviderProduct Product = ProviderProduct::None;
    RetryClassification Retry = RetryClassification::NotRetryable;
    FString Summary;
    int32 HttpStatus = 0;
    FString ContentType;
    int64 ResponseBytes = 0;
    FString ResponseSha256;
    uint32 Width = 0;
    uint32 Height = 0;
    FString Organization;
    uint16 Compression = 0;
    uint16 Orientation = 0;
    uint16 SampleFormat = 0;
    FString NoData;
    uint32 MetadataTag = 0;
    int32 MetadataType = 0;
    int32 MetadataReadCount = 0;
    bool MetadataPassCount = false;
    FString GeoreferenceStatus;
    FString DiagnosticReceipt;
};

struct SKIPREPARATION_API Request
{
    FString Name;
    SkiDomain::GeographicBounds Bounds;
    SourceProfile Profile = SourceProfile::Standard;
    uint64 SessionGeneration = 0;
    uint64 OperationGeneration = 0;
};

struct SKIPREPARATION_API Progress
{
    State Phase = State::Selected;
    uint64 Completed = 0;
    TOptional<uint64> Total;
    double ElapsedSeconds = 0.0;
    FString Detail;
};

struct SKIPREPARATION_API Result
{
    bool Ok = false;
    State FinalState = State::Failed;
    FString Error;
    TOptional<ProviderFailure> Failure;
    FString PackageDirectory;
    TArray<FString> Warnings;
    SkiDomain::TerrainManifest Manifest;
    SkiDomain::Heightfield Heightfield;
    TArray<uint8> Cover;
};

class SKIPREPARATION_API Cancellation
{
public:
    void Cancel() noexcept { Cancelled.store(true, std::memory_order_release); }
    bool IsCancelled() const noexcept { return Cancelled.load(std::memory_order_acquire); }

private:
    std::atomic_bool Cancelled = false;
};

using ProgressCallback = TFunction<void(const Progress&)>;

class SKIPREPARATION_API Provider
{
public:
    virtual ~Provider() = default;
    virtual Result Prepare(const Request& RequestValue, const TSharedRef<Cancellation>& CancellationValue,
        const ProgressCallback& OnProgress) = 0;
};

SKIPREPARATION_API bool ValidateRequest(const Request& RequestValue, FString& OutError);
SKIPREPARATION_API const TCHAR* StateName(State Value) noexcept;
SKIPREPARATION_API const TCHAR* FailureStageName(FailureStage Value) noexcept;
SKIPREPARATION_API const TCHAR* ProviderProductName(ProviderProduct Value) noexcept;
SKIPREPARATION_API void InitializePreparationDiagnostics(const FString& DataRoot);
}
