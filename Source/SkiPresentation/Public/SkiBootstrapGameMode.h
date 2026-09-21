#pragma once

#include "GameFramework/GameModeBase.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiBootstrapGameMode.generated.h"

class ASkiTerrainActor;
class USkiP1Widget;

UCLASS()
class SKIPRESENTATION_API ASkiBootstrapGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ASkiBootstrapGameMode();

protected:
    virtual void BeginPlay() override;

private:
    bool RunP1Smoke();
    bool BeginP1VisualCapture();
    void RequestP1VisualScreenshot();
    void FinishP1VisualCapture();
    void BeginP1Preparation(const SkiPreparation::Request& Request);
    void FinishP1Preparation(SkiPreparation::Result Result, uint64 SessionGeneration,
        uint64 OperationGeneration, TSharedRef<SkiPreparation::Cancellation> Cancellation);
    void RetryPreparation();
    void ChangeSelection();

    UPROPERTY()
    TObjectPtr<USkiP1Widget> P1Widget;

    UPROPERTY()
    TObjectPtr<ASkiTerrainActor> TerrainActor;

    TSharedPtr<SkiApplication::TerrainSession> TerrainSession;
    TSharedPtr<SkiPreparation::Cancellation> PreparationCancellation;
    TOptional<SkiPreparation::Request> LastRequest;
    uint64 ActiveSessionGeneration = 0;
    uint64 ActiveOperationGeneration = 0;
    FString VisualReceiptPath;
    FString VisualScreenshotPath;
    FString VisualToken;
    FString VisualMode;
    FString VisualLighting;
    FString VisualView;
    int32 VisualLod = 0;
    int32 VisualCaptureWidth = 2560;
    int32 VisualCaptureHeight = 1440;
};
