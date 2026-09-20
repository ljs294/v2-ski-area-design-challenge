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
}
