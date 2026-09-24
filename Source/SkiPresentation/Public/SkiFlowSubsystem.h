#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "SkiFlowSubsystem.generated.h"

/** Carries one verified installed-resort choice across FrontEnd-to-Mountain level travel. */
UCLASS()
class SKIPRESENTATION_API USkiFlowSubsystem final : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    void QueueInstalledResort(const FString& ContentId, const FString& DataRoot = FString())
    {
        PendingContentId = ContentId;
        PendingDataRoot = DataRoot;
    }
    FString ConsumeInstalledResort()
    {
        FString Value = MoveTemp(PendingContentId);
        PendingContentId.Reset();
        return Value;
    }
    FString ConsumeInstalledDataRoot()
    {
        FString Value = MoveTemp(PendingDataRoot);
        PendingDataRoot.Reset();
        return Value;
    }
    void SetReturnError(const FString& Error) { ReturnError = Error; }
    FString ConsumeReturnError()
    {
        FString Value = MoveTemp(ReturnError);
        ReturnError.Reset();
        return Value;
    }

private:
    FString PendingContentId;
    FString PendingDataRoot;
    FString ReturnError;
};
