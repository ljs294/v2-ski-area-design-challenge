#pragma once
#include "Blueprint/UserWidget.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiP1Widget.generated.h"
class UButton; class UProgressBar; class UTextBlock; class UVerticalBox; class USkiSelectorBrowser;
UCLASS()
class SKIPRESENTATION_API USkiP1Widget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> Handler);
    void SetPreparationProgress(const SkiPreparation::Progress& Progress);
    void SetTransientStatus(const FString& Status);
    void SetProbeStatus(const FString& Status);
    void SetTerrainDetails(const FString& Details, bool bSynthetic);
    void ShowPreparationFailure(const TOptional<SkiPreparation::ProviderFailure>& Failure, const FString& Fallback,
        TFunction<void()> Retry, TFunction<void()> ChangeSelection);
    void ResetSelector();
    void SetViewCommandHandler(TFunction<void(FName)> Handler);
    void CloseSelector();
    bool IsP1Ready() const;
protected:
    virtual void NativeOnInitialized() override;
private:
    void AcceptSelection(const SkiPreparation::Request& Request);
    void ConfigureSelector();
    UButton* AddCommandButton(UVerticalBox* Parent, const TCHAR* Label, FName Name);
    UFUNCTION() void RetryClicked(); UFUNCTION() void ChangeSelectionClicked();
    UFUNCTION() void PresentationClicked(); UFUNCTION() void ElevationClicked();
    UFUNCTION() void SlopeClicked(); UFUNCTION() void CoverClicked(); UFUNCTION() void LodClicked();
    UFUNCTION() void Lod0Clicked(); UFUNCTION() void Lod1Clicked(); UFUNCTION() void Lod2Clicked();
    UFUNCTION() void Vertical1Clicked(); UFUNCTION() void Vertical2Clicked(); UFUNCTION() void Vertical4Clicked();
    UFUNCTION() void MiddayClicked(); UFUNCTION() void LowAngleClicked(); UFUNCTION() void OvercastClicked();
    UPROPERTY() TObjectPtr<USkiSelectorBrowser> Selector;
    UPROPERTY() TObjectPtr<UVerticalBox> SelectorPanel;
    UPROPERTY() TObjectPtr<UTextBlock> ProgressText;
    UPROPERTY() TObjectPtr<UProgressBar> ProgressBar;
    UPROPERTY() TObjectPtr<UTextBlock> StatusText;
    UPROPERTY() TObjectPtr<UTextBlock> DetailsText;
    UPROPERTY() TObjectPtr<UTextBlock> ProbeText;
    UPROPERTY() TObjectPtr<UButton> RetryButton;
    UPROPERTY() TObjectPtr<UButton> ChangeSelectionButton;
    TFunction<void(const SkiPreparation::Request&)> SelectionHandler;
    TFunction<void()> RetryHandler; TFunction<void()> ChangeSelectionHandler;
    TFunction<void(FName)> ViewCommandHandler;
    uint64 SelectorGeneration = 0;
};
