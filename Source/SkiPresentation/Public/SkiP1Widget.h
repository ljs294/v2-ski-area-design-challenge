#pragma once
#include "Blueprint/UserWidget.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiP1Widget.generated.h"
class UButton; class UProgressBar; class UTextBlock; class UVerticalBox; class UScrollBox;
class UUniformGridPanel; class USkiSelectorBrowser;
enum class EP1ShellState : uint8 { Selecting, Preparing, Failed, Ready };
UCLASS()
class SKIPRESENTATION_API USkiP1Widget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> Handler);
    void SetOpenInstalledHandler(TFunction<void()> Handler);
    void SetSelectorStatus(const FString& Status);
    void SetPreparationProgress(const SkiPreparation::Progress& Progress);
    void SetTransientStatus(const FString& Status);
    void SetProbeStatus(const FString& Status);
    void SetTerrainDetails(const FString& Details, bool bSynthetic);
    void SetNodeStatus(const FString& Status);
    void BeginPreparationUI(TFunction<void()> ChangeSelection);
    void ShowPreparationFailure(const TOptional<SkiPreparation::ProviderFailure>& Failure, const FString& Fallback,
        TFunction<void()> Retry, TFunction<void()> ChangeSelection);
    void ResetSelector();
    // Creates the CEF selector for the Selecting state; no other state owns a browser.
    void OpenSelector();
    bool HasLiveSelector() const;
    bool GetSelectorTeardownProof(bool& OutBridgeUnbound, bool& OutCefClosed, bool& OutReleased,
        double& OutCloseMs) const;
    void SetViewCommandHandler(TFunction<void(FName)> Handler);
    void CloseSelector();
    bool IsSelectorClosed() const;
    int32 GetBlockedSelectorNavigationCount() const;
    int32 GetBlockedSelectorPopupCount() const;
    bool WasSelectorPopupDelegateProbeDenied() const;
    bool IsP1Ready() const;
    bool IsPointerOverStatusPanel() const;
    bool DoesUiOwnKeyboardInput() const;
    double GetRightPanelInsetPixels() const;
    FVector2D GetStatusPanelCenterAbsolute() const;
    FVector2D GetUnobstructedCenterAbsolute() const;
    void FocusRecoveryAction();
    bool ValidateRecoveryLayout(FIntPoint ViewportSize, FString& OutError) const;
    bool ValidateShellLayout(FIntPoint ViewportSize, EP1ShellState ExpectedState,
        FString& OutError) const;
    FVector4 GetStatusPanelRectAbsolute() const;
    FVector4 GetSelectorPanelRectAbsolute() const;
    FVector4 GetStatusScrollRectAbsolute() const;
    FVector4 GetRetryRectAbsolute() const;
    FVector4 GetChangeSelectionRectAbsolute() const;
    static double CalculateStatusPanelWidth(double ViewportWidth) noexcept;
    static FVector2D CalculateSelectorPanelSize(FIntPoint ViewportSize) noexcept;
protected:
    virtual void NativeOnInitialized() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
private:
    void AcceptSelection(const SkiPreparation::Request& Request);
    void ConfigureSelector();
    UButton* AddCommandButton(UVerticalBox* Parent, const TCHAR* Label, FName Name);
    UButton* AddGridCommandButton(UUniformGridPanel* Parent, const TCHAR* Label, FName Name,
        int32 Row, int32 Column);
    void BindCommandButton(UButton* Button, FName Name);
    void SetShellState(EP1ShellState State);
    UFUNCTION() void RetryClicked(); UFUNCTION() void ChangeSelectionClicked();
    UFUNCTION() void OpenInstalledClicked();
    UFUNCTION() void PresentationClicked(); UFUNCTION() void ElevationClicked();
    UFUNCTION() void SlopeClicked(); UFUNCTION() void CoverClicked(); UFUNCTION() void LodClicked();
    UFUNCTION() void LodAutoClicked();
    UFUNCTION() void Lod0Clicked(); UFUNCTION() void Lod1Clicked(); UFUNCTION() void Lod2Clicked();
    UFUNCTION() void Vertical1Clicked(); UFUNCTION() void Vertical2Clicked(); UFUNCTION() void Vertical4Clicked();
    UFUNCTION() void MiddayClicked(); UFUNCTION() void LowAngleClicked(); UFUNCTION() void OvercastClicked();
    UPROPERTY() TObjectPtr<USkiSelectorBrowser> Selector;
    UPROPERTY() TObjectPtr<UVerticalBox> SelectorPanel;
    UPROPERTY() TObjectPtr<UVerticalBox> StatusPanel;
    UPROPERTY() TObjectPtr<UScrollBox> StatusScroll;
    UPROPERTY() TObjectPtr<UVerticalBox> ReadyControls;
    UPROPERTY() TObjectPtr<UTextBlock> ProgressText;
    UPROPERTY() TObjectPtr<UProgressBar> ProgressBar;
    UPROPERTY() TObjectPtr<UTextBlock> StatusText;
    UPROPERTY() TObjectPtr<UTextBlock> DetailsText;
    UPROPERTY() TObjectPtr<UTextBlock> ProbeText;
    UPROPERTY() TObjectPtr<UTextBlock> NodeText;
    UPROPERTY() TObjectPtr<UButton> RetryButton;
    UPROPERTY() TObjectPtr<UButton> ChangeSelectionButton;
    UPROPERTY() TObjectPtr<UButton> OpenInstalledButton;
    UPROPERTY() TObjectPtr<UTextBlock> SelectorStatusText;
    TFunction<void(const SkiPreparation::Request&)> SelectionHandler;
    TFunction<void()> OpenInstalledHandler;
    TFunction<void()> RetryHandler; TFunction<void()> ChangeSelectionHandler;
    TFunction<void(FName)> ViewCommandHandler;
    uint64 SelectorGeneration = 0;
    double OpenInstalledCloseStartedSeconds = 0.0;
    bool bOpenInstalledPending = false;
    EP1ShellState ShellState = EP1ShellState::Selecting;
};
