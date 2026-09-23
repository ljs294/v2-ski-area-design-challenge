#include "SkiSelectorBrowser.h"

#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserWindow.h"
#include "SWebBrowser.h"
#include "WebBrowserModule.h"
#include "SkiPreparation/SelectorProtocol.h"

namespace
{
constexpr double MaximumReleaseWaitSeconds = 2.0;
}

void USkiSelectorBrowser::Configure(FString InInitialUrl, FString InToken, const uint64 InGeneration,
    TFunction<void(const SkiPreparation::Request&)> InAccepted,
    TFunction<void(const FString&)> InRejected)
{
    if (Browser.IsValid() || Window.IsValid() || PendingRequest.IsSet() || BridgeBound) Close();
    SetVisibility(ESlateVisibility::Visible);
    InitialUrl = std::move(InInitialUrl);
    ExpectedToken = std::move(InToken);
    ExpectedGeneration = InGeneration;
    Accepted = std::move(InAccepted);
    Rejected = std::move(InRejected);
    BlockedNavigationCount = 0;
    BlockedPopupCount = 0;
    bBridgeUnbound = false;
    bCefBrowserClosed = false;
    bWindowReleased = false;
    CloseMilliseconds = 0.0;
    bCloseStarted = false;
    bPopupDelegateProbeDenied = false;
}

TSharedRef<SWidget> USkiSelectorBrowser::RebuildWidget()
{
    if (!Window.IsValid() && IWebBrowserModule::IsAvailable()
        && IWebBrowserModule::Get().IsWebModuleAvailable())
    {
        IWebBrowserSingleton* Singleton = IWebBrowserModule::Get().GetSingleton();
        FCreateBrowserWindowSettings Settings;
        Settings.InitialURL = TEXT("about:blank");
        Settings.bShowErrorMessage = true;
        // An empty cookie location keeps this context in memory for the short-lived selector.
        Settings.Context = FBrowserContextSettings(TEXT("ski-p1-selector"));
        Window = Singleton->CreateBrowserWindow(Settings);
        WeakWindow = Window;
    }
    SAssignNew(Browser, SWebBrowser, Window)
        .InitialURL(TEXT("about:blank"))
        .ShowControls(false)
        .ShowAddressBar(false)
        .ShowErrorMessage(true)
        .SupportsThumbMouseButtonNavigation(false)
        .OnLoadCompleted(FSimpleDelegate::CreateUObject(this, &USkiSelectorBrowser::HandleLoadCompleted))
        .OnBeforeNavigation(SWebBrowser::FOnBeforeBrowse::CreateUObject(this, &USkiSelectorBrowser::BeforeNavigation))
        .OnBeforePopup(FOnBeforePopupDelegate::CreateUObject(this, &USkiSelectorBrowser::BeforePopup))
        .OnSuppressContextMenu(FOnSuppressContextMenu::CreateLambda([] { return true; }));
    return Browser.ToSharedRef();
}

void USkiSelectorBrowser::HandleLoadCompleted()
{
    if (!Browser || BridgeBound || Browser->GetUrl() != TEXT("about:blank")) return;
    FString DiagnosticPhase;
    if (FParse::Value(FCommandLine::Get(), TEXT("SkiP1SelectorDiagnostic="), DiagnosticPhase)
        && DiagnosticPhase == TEXT("blank")) return;
    // The initial blank document proves the asynchronous browser window exists.
    // Bind permanently for this short-lived, allow-listed browser before the
    // selector document is allowed to execute any JavaScript.
    BridgeBound = true;
    Browser->BindUObject(TEXT("skiSelector"), this, true);
    Browser->LoadURL(InitialUrl);
}

bool USkiSelectorBrowser::BeforeNavigation(const FString& Url, const FWebNavigationRequest&) const
{
    const bool Blocked = Url != TEXT("about:blank") && Url != InitialUrl;
    if (Blocked) ++BlockedNavigationCount;
    return Blocked;
}

bool USkiSelectorBrowser::BeforePopup(FString, FString) const
{
    ++BlockedPopupCount;
    return true;
}

void USkiSelectorBrowser::Submit(const FString& Json)
{
    if (PendingRequest.IsSet() || bCloseStarted || ExpectedToken.IsEmpty()) return;
    SkiPreparation::Request Request;
    FString Error;
    if (!SkiPreparation::ValidateSelectorMessage(Json, ExpectedToken, ExpectedGeneration, Request, Error))
    {
        if (Rejected) Rejected(Error);
        return;
    }
    // This runs inside a CEF JavaScript callback, where closing the browser is unsafe, so
    // teardown and the accepted callback are deferred to the next game-thread tick.
    PendingRequest = MoveTemp(Request);
    RemoveTicker();
    DeferredTicker = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &USkiSelectorBrowser::FinishDeferredAcceptance));
}

bool USkiSelectorBrowser::FinishDeferredAcceptance(float)
{
    if (!PendingRequest.IsSet()) return false;
    if (!bCloseStarted && FParse::Param(FCommandLine::Get(), TEXT("SkiP1SelectorSmoke")))
    {
        // A timer-driven JavaScript click has no trusted user activation and may never reach
        // CEF's popup callback. Exercise the actual bound Slate delegate in this smoke lane.
        bPopupDelegateProbeDenied = Window.IsValid() && Window->OnBeforePopup().IsBound()
            && Window->OnBeforePopup().Execute(TEXT("https://blocked.invalid/popup"), TEXT("_blank"));
    }
    if (!bCloseStarted) CloseBrowserResources();
    // The singleton holds only weak references; the window is released once every Slate
    // owner drops it. Accept only after release is observed or the bounded wait expires.
    bWindowReleased = !WeakWindow.IsValid();
    const double Waited = FPlatformTime::Seconds() - CloseStartedSeconds;
    if (!bWindowReleased && Waited < MaximumReleaseWaitSeconds) return true;
    CloseMilliseconds = Waited * 1000.0;
    DeferredTicker.Reset();
    if (!bWindowReleased)
    {
        PendingRequest.Reset();
        Accepted = {};
        ExpectedToken.Empty();
        ExpectedGeneration = 0;
        TFunction<void(const FString&)> RejectedCallback = MoveTemp(Rejected);
        if (RejectedCallback) RejectedCallback(TEXT("Selector browser did not release; preparation was not started."));
        return false;
    }
    bCefBrowserClosed = true;
    SkiPreparation::Request Request = MoveTemp(PendingRequest.GetValue());
    PendingRequest.Reset();
    TFunction<void(const SkiPreparation::Request&)> AcceptedCallback = MoveTemp(Accepted);
    if (AcceptedCallback) AcceptedCallback(Request);
    return false;
}

void USkiSelectorBrowser::Close()
{
    RemoveTicker();
    PendingRequest.Reset();
    Accepted = {};
    Rejected = {};
    ExpectedToken.Empty();
    ExpectedGeneration = 0;
    CloseBrowserResources();
}

void USkiSelectorBrowser::CloseBrowserResources()
{
    if (!bCloseStarted)
    {
        CloseStartedSeconds = FPlatformTime::Seconds();
        bCloseStarted = true;
    }
    if (Browser && BridgeBound) Browser->UnbindUObject(TEXT("skiSelector"), this, true);
    if (BridgeBound) bBridgeUnbound = true;
    BridgeBound = false;
    if (Window.IsValid())
    {
        Window->CloseBrowser(true, true);
        bCefBrowserClosed = !Window->IsValid();
    }
    Browser.Reset();
    Window.Reset();
    SetVisibility(ESlateVisibility::Collapsed);
    RemoveFromParent();
    bWindowReleased = !WeakWindow.IsValid();
    if (bWindowReleased) bCefBrowserClosed = true;
}

void USkiSelectorBrowser::RemoveTicker()
{
    if (DeferredTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(DeferredTicker);
    DeferredTicker.Reset();
}

void USkiSelectorBrowser::ReleaseSlateResources(const bool bReleaseChildren)
{
    if (Browser && BridgeBound) Browser->UnbindUObject(TEXT("skiSelector"), this, true);
    BridgeBound = false;
    if (Window.IsValid()) Window->CloseBrowser(true, false);
    Browser.Reset();
    Window.Reset();
    Super::ReleaseSlateResources(bReleaseChildren);
}

void USkiSelectorBrowser::BeginDestroy()
{
    Close();
    Super::BeginDestroy();
}
