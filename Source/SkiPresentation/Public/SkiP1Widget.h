#pragma once
#include "Blueprint/UserWidget.h"
#include "Components/EditableTextBox.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiP1Widget.generated.h"
class UButton; class UProgressBar; class UTextBlock; class UVerticalBox; class UScrollBox;
class UUniformGridPanel; class UEditableTextBox; class UBorder; class USkiSiteMapWidget; class USkiP1Widget;
UCLASS()
class SKIPRESENTATION_API USkiInstalledResortAction : public UObject
{
    GENERATED_BODY()
public:
    TWeakObjectPtr<USkiP1Widget> Owner;
    FString ContentId;
    UFUNCTION() void Clicked();
};

struct FSkiInstalledResortItem
{
    FString ContentId;
    FString DisplayName;
    FString Detail;
};
struct FSkiPlaceSearchResult
{
    FString Name;
    FString Region;
    double LatitudeDeg = 0.0;
    double LongitudeDeg = 0.0;
    SkiDomain::GeographicBounds BoundingBox;
};
using FSkiPlaceSearchCompletion = TFunction<void(TArray<FSkiPlaceSearchResult>, FString)>;
using FSkiPlaceSearchHandler = TFunction<void(const FString&,
    const TSharedRef<SkiPreparation::Cancellation>&, FSkiPlaceSearchCompletion)>;
UCLASS()
class SKIPRESENTATION_API USkiPlaceSearchAction : public UObject
{
    GENERATED_BODY()
public:
    TWeakObjectPtr<USkiP1Widget> Owner;
    int32 SearchGeneration = 0;
    int32 ResultIndex = INDEX_NONE;
    UFUNCTION() void Clicked();
};
enum class EP1ShellState : uint8 { Selecting, Preparing, Failed, Ready };
UCLASS()
class SKIPRESENTATION_API USkiP1Widget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> Handler);
    void SetOpenInstalledHandler(TFunction<void()> Handler);
    void SetOpenInstalledByIdHandler(TFunction<void(const FString&)> Handler);
    void SetNavigationHandler(TFunction<void()> Handler);
    void SetResumeDownloadHandler(TFunction<void()> Handler);
    /** Integration seam for a worker-owned IPlaceSearchProvider; coordinates are handled locally. */
    void SetPlaceSearchHandler(FSkiPlaceSearchHandler Handler);
    void SetInstalledResorts(const TArray<FSkiInstalledResortItem>& Items);
    void OpenInstalledById(const FString& ContentId);
    bool IsNativeTitleReady() const;
    bool IsNativePickerPlaceholder() const;
    bool IsNativeSitePickerReady() const;
    // Packaged M1 smoke only: exercises the native actions without OS input.
    bool RunNativeFrontEndSmoke(const FString& ExpectedContentId, FString& OutError,
        bool bInvokeProductionOpen = false);
    void SetSelectorStatus(const FString& Status);
    void SetPreparationProgress(const SkiPreparation::Progress& Progress);
    void SetTransientStatus(const FString& Status);
    void SetProbeStatus(const FString& Status);
    void SetTerrainDetails(const FString& Details, bool bSynthetic);
    void SetPhotoCommandAvailable(bool bAvailable);
    static bool ShouldShowPhotoCommandForInstallation(uint32 InstallationSchema,
        bool bHasVerifiedSiteContext) noexcept;
    void SetNodeStatus(const FString& Status);
    void BeginPreparationUI(TFunction<void()> ChangeSelection);
    void ShowPreparationFailure(const TOptional<SkiPreparation::ProviderFailure>& Failure, const FString& Fallback,
        TFunction<void()> Retry, TFunction<void()> ChangeSelection);
    void ResetSelector();
    // Opens the native title shell. New Resort opens the native site picker.
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
    static FVector2D CalculateSitePickerPanelSize(FIntPoint ViewportSize) noexcept;
protected:
    virtual void NativeOnInitialized() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
private:
    friend class USkiPlaceSearchAction;
    void AcceptSelection(const SkiPreparation::Request& Request);
    void ConfigureSelector();
    void RebuildInstalledResorts();
    void RunLocationSearch();
    void CompletePlaceSearch(int32 Generation, TArray<FSkiPlaceSearchResult> Results,
        const FString& Error);
    void SelectPlaceSearchResult(int32 Generation, int32 ResultIndex);
    void RebuildPlaceSearchResults();
    void UpdatePickerSteps();
    UButton* AddCommandButton(UVerticalBox* Parent, const TCHAR* Label, FName Name);
    UButton* AddGridCommandButton(UUniformGridPanel* Parent, const TCHAR* Label, FName Name,
        int32 Row, int32 Column);
    void BindCommandButton(UButton* Button, FName Name);
    void SetShellState(EP1ShellState State);
    UFUNCTION() void RetryClicked(); UFUNCTION() void ChangeSelectionClicked();
    UFUNCTION() void OpenInstalledClicked();
    UFUNCTION() void NewResortClicked();
    UFUNCTION() void BackToTitleClicked();
    UFUNCTION() void ResumeDownloadClicked();
    UFUNCTION() void SearchLocationClicked();
    UFUNCTION() void SelectSiteClicked();
    UFUNCTION() void ChangeLocationClicked();
    UFUNCTION() void CoordinateCommitted(const FText& Text, ETextCommit::Type CommitMethod);
    UFUNCTION() void ToggleContoursClicked();
    UFUNCTION() void ToggleContourUnitsClicked();
    UFUNCTION() void ClearBoundaryClicked();
    UFUNCTION() void PresentationClicked(); UFUNCTION() void ElevationClicked();
    UFUNCTION() void PhotoClicked();
    UFUNCTION() void SlopeClicked(); UFUNCTION() void CoverClicked(); UFUNCTION() void LodClicked();
    UFUNCTION() void LodAutoClicked();
    UFUNCTION() void Lod0Clicked(); UFUNCTION() void Lod1Clicked(); UFUNCTION() void Lod2Clicked();
    UFUNCTION() void Vertical1Clicked(); UFUNCTION() void Vertical2Clicked(); UFUNCTION() void Vertical4Clicked();
    UFUNCTION() void MiddayClicked(); UFUNCTION() void LowAngleClicked(); UFUNCTION() void OvercastClicked();
    UPROPERTY() TObjectPtr<UVerticalBox> SelectorPanel;
    UPROPERTY() TObjectPtr<UVerticalBox> TitleContents;
    UPROPERTY() TObjectPtr<UVerticalBox> PickerContents;
    UPROPERTY() TObjectPtr<UVerticalBox> PickerSteps;
    UPROPERTY() TObjectPtr<UVerticalBox> LocationControls;
    UPROPERTY() TObjectPtr<UVerticalBox> BoundaryControls;
    UPROPERTY() TObjectPtr<UVerticalBox> ResortNameControls;
    UPROPERTY() TArray<TObjectPtr<UTextBlock>> PickerStepLabels;
    UPROPERTY() TObjectPtr<UBorder> PickerCardBackdrop;
    UPROPERTY() TObjectPtr<UVerticalBox> InstalledResortList;
    UPROPERTY() TObjectPtr<UButton> ResumeDownloadButton;
    UPROPERTY() TArray<TObjectPtr<USkiInstalledResortAction>> InstalledActions;
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
    UPROPERTY() TObjectPtr<USkiSiteMapWidget> SiteMapWidget;
    UPROPERTY() TObjectPtr<UEditableTextBox> LocationSearchBox;
    UPROPERTY() TObjectPtr<UVerticalBox> PlaceSearchResults;
    UPROPERTY() TObjectPtr<UTextBlock> PickerSearchStatusText;
    UPROPERTY() TObjectPtr<UTextBlock> PickerBoundaryStatusText;
    UPROPERTY() TObjectPtr<UTextBlock> PickerPreviewStatusText;
    UPROPERTY() TObjectPtr<UButton> SelectSiteButton;
    UPROPERTY() TObjectPtr<UButton> ChangeLocationButton;
    UPROPERTY() TObjectPtr<UButton> ToggleContoursButton;
    UPROPERTY() TObjectPtr<UButton> ContourUnitsButton;
    UPROPERTY() TObjectPtr<UButton> ClearBoundaryButton;
    UPROPERTY() TObjectPtr<UButton> PhotoButton;
    UPROPERTY() TObjectPtr<UButton> PickerDownloadButton;
    UPROPERTY() TObjectPtr<UEditableTextBox> ResortNameBox;
    UPROPERTY() TArray<TObjectPtr<USkiPlaceSearchAction>> PlaceSearchActions;
    TFunction<void(const SkiPreparation::Request&)> SelectionHandler;
    TFunction<void()> OpenInstalledHandler;
    TFunction<void(const FString&)> OpenInstalledByIdHandler;
    TFunction<void()> NavigationHandler;
    TFunction<void()> ResumeDownloadHandler;
    FSkiPlaceSearchHandler PlaceSearchHandler;
    TArray<FSkiPlaceSearchResult> PlaceSearchResultsData;
    TSharedPtr<SkiPreparation::Cancellation> PlaceSearchCancellation;
    int32 PlaceSearchGeneration = 0;
    bool bHasChosenLocation = false;
    bool bMetricContourUnits = false;
    bool bContoursVisible = true;
    bool bPhotoCommandAvailable = false;
    bool bBoundaryStepActive = false;
    TArray<FSkiInstalledResortItem> InstalledResorts;
    TFunction<void()> RetryHandler; TFunction<void()> ChangeSelectionHandler;
    TFunction<void(FName)> ViewCommandHandler;
    bool bSitePickerOpen = false;
    EP1ShellState ShellState = EP1ShellState::Selecting;
};
