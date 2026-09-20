#pragma once

#include "Blueprint/UserWidget.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiP1Widget.generated.h"

class UProgressBar;
class UTextBlock;
class USkiSelectorBrowser;

UCLASS()
class SKIPRESENTATION_API USkiP1Widget : public UUserWidget
{
    GENERATED_BODY()

public:
    void SetSelectionHandler(TFunction<void(const SkiPreparation::Request&)> Handler);
    void SetPreparationProgress(const SkiPreparation::Progress& Progress);
    void SetTransientStatus(const FString& Status);
    void CloseSelector();
    bool IsP1Ready() const;

protected:
    virtual void NativeOnInitialized() override;

private:
    void AcceptSelection(const SkiPreparation::Request& Request);

    UPROPERTY()
    TObjectPtr<USkiSelectorBrowser> Selector;

    UPROPERTY()
    TObjectPtr<UTextBlock> ProgressText;

    UPROPERTY()
    TObjectPtr<UProgressBar> ProgressBar;

    UPROPERTY()
    TObjectPtr<UTextBlock> StatusText;

    TFunction<void(const SkiPreparation::Request&)> SelectionHandler;
};
