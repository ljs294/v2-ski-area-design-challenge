#pragma once

#include "Components/Widget.h"
#include "Containers/Ticker.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiSelectorBrowser.generated.h"

class IWebBrowserWindow;
class SWebBrowser;
struct FWebNavigationRequest;

// Short-lived, allow-listed selector browser. The CEF window is created explicitly so its
// closure and release can be proven before any preparation or gameplay work begins.
UCLASS()
class SKIPRESENTATION_API USkiSelectorBrowser : public UWidget
{
    GENERATED_BODY()

public:
    void Configure(FString InInitialUrl, FString InToken, uint64 InGeneration,
        TFunction<void(const SkiPreparation::Request&)> InAccepted,
        TFunction<void(const FString&)> InRejected);
    // Cancels a pending selection and releases the browser. Safe to call repeatedly;
    // never call from inside a CEF callback.
    void Close();
    bool IsClosed() const noexcept { return !Browser.IsValid() && !Window.IsValid() && !BridgeBound && !WeakWindow.IsValid(); }
    bool WasBridgeUnbound() const noexcept { return bBridgeUnbound; }
    bool WasCefBrowserClosed() const noexcept { return bCefBrowserClosed; }
    bool WasWindowReleased() const noexcept { return bWindowReleased; }
    double GetCloseMilliseconds() const noexcept { return CloseMilliseconds; }
    int32 GetBlockedNavigationCount() const noexcept { return BlockedNavigationCount; }
    int32 GetBlockedPopupCount() const noexcept { return BlockedPopupCount; }
    bool WasPopupDelegateProbeDenied() const noexcept { return bPopupDelegateProbeDenied; }

    UFUNCTION()
    void Submit(const FString& Json);

    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
    virtual void BeginDestroy() override;

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;

private:
    void HandleLoadCompleted();
    bool BeforeNavigation(const FString& Url, const FWebNavigationRequest& Request) const;
    bool BeforePopup(FString Url, FString Frame) const;
    bool FinishDeferredAcceptance(float DeltaSeconds);
    void CloseBrowserResources();
    void RemoveTicker();

    FString InitialUrl;
    FString ExpectedToken;
    uint64 ExpectedGeneration = 0;
    TFunction<void(const SkiPreparation::Request&)> Accepted;
    TFunction<void(const FString&)> Rejected;
    TSharedPtr<SWebBrowser> Browser;
    TSharedPtr<IWebBrowserWindow> Window;
    TWeakPtr<IWebBrowserWindow> WeakWindow;
    TOptional<SkiPreparation::Request> PendingRequest;
    FTSTicker::FDelegateHandle DeferredTicker;
    double CloseStartedSeconds = 0.0;
    double CloseMilliseconds = 0.0;
    bool BridgeBound = false;
    bool bBridgeUnbound = false;
    bool bCefBrowserClosed = false;
    bool bWindowReleased = false;
    bool bCloseStarted = false;
    bool bPopupDelegateProbeDenied = false;
    mutable int32 BlockedNavigationCount = 0;
    mutable int32 BlockedPopupCount = 0;
};
