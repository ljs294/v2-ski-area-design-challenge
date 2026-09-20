#pragma once

#include "Components/Widget.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiSelectorBrowser.generated.h"

class SWebBrowser;
struct FWebNavigationRequest;

UCLASS()
class SKIPRESENTATION_API USkiSelectorBrowser : public UWidget
{
    GENERATED_BODY()

public:
    void Configure(FString InInitialUrl, FString InToken, uint64 InGeneration,
        TFunction<void(const SkiPreparation::Request&)> InAccepted,
        TFunction<void(const FString&)> InRejected);
    void Close();

    UFUNCTION()
    void Submit(const FString& Json);

    virtual void ReleaseSlateResources(bool bReleaseChildren) override;

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;

private:
    void HandleLoadCompleted();
    bool BeforeNavigation(const FString& Url, const FWebNavigationRequest& Request) const;
    bool BeforePopup(FString Url, FString Frame) const;

    FString InitialUrl;
    FString ExpectedToken;
    uint64 ExpectedGeneration = 0;
    TFunction<void(const SkiPreparation::Request&)> Accepted;
    TFunction<void(const FString&)> Rejected;
    TSharedPtr<SWebBrowser> Browser;
    bool BridgeBound = false;
};
