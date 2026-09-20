#include "SkiSelectorBrowser.h"

#include "SWebBrowser.h"
#include "SkiPreparation/SelectorProtocol.h"

void USkiSelectorBrowser::Configure(FString InInitialUrl, FString InToken, const uint64 InGeneration,
    TFunction<void(const SkiPreparation::Request&)> InAccepted,
    TFunction<void(const FString&)> InRejected)
{
    InitialUrl = std::move(InInitialUrl);
    ExpectedToken = std::move(InToken);
    ExpectedGeneration = InGeneration;
    Accepted = std::move(InAccepted);
    Rejected = std::move(InRejected);
}

TSharedRef<SWidget> USkiSelectorBrowser::RebuildWidget()
{
    SAssignNew(Browser, SWebBrowser)
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
    // The initial blank document proves the asynchronous browser window exists.
    // Bind permanently for this short-lived, allow-listed browser before the
    // selector document is allowed to execute any JavaScript.
    BridgeBound = true;
    Browser->BindUObject(TEXT("skiSelector"), this, true);
    Browser->LoadURL(InitialUrl);
}

bool USkiSelectorBrowser::BeforeNavigation(const FString& Url, const FWebNavigationRequest&) const
{
    return Url != TEXT("about:blank") && Url != InitialUrl;
}

bool USkiSelectorBrowser::BeforePopup(FString, FString) const
{
    return true;
}

void USkiSelectorBrowser::Submit(const FString& Json)
{
    SkiPreparation::Request Request;
    FString Error;
    if (!SkiPreparation::ValidateSelectorMessage(Json, ExpectedToken, ExpectedGeneration, Request, Error))
    {
        if (Rejected) Rejected(Error);
        return;
    }
    if (Accepted) Accepted(Request);
    Close();
}

void USkiSelectorBrowser::Close()
{
    if (Browser)
    {
        Browser->UnbindUObject(TEXT("skiSelector"), this, true);
        Browser.Reset();
    }
    BridgeBound = false;
    SetVisibility(ESlateVisibility::Collapsed);
}

void USkiSelectorBrowser::ReleaseSlateResources(const bool bReleaseChildren)
{
    if (Browser) Browser->UnbindUObject(TEXT("skiSelector"), this, true);
    Browser.Reset();
    BridgeBound = false;
    Super::ReleaseSlateResources(bReleaseChildren);
}
