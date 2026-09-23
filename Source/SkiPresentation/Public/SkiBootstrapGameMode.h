#pragma once

#include "GameFramework/GameModeBase.h"
#include "SkiApplication/TerrainCoreSession.h"
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
    bool BeginP1UiLayoutSmoke();
    void FinishP1UiLayoutSmoke();
    bool BeginP1VisualCapture();
    void RequestP1VisualScreenshot();
    void FinishP1VisualCapture();
    bool BeginP1PerformanceSmoke();
    void SampleP1PerformanceFrame();
    void FinishP1PerformanceSmoke();
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
    TSharedPtr<SkiApplication::TerrainCoreSession> TerrainCoreSession;
    TSharedPtr<SkiPreparation::Cancellation> PreparationCancellation;
    TSharedPtr<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe> PreparationLease;
    TOptional<SkiPreparation::Request> LastRequest;
    uint64 ActiveSessionGeneration = 0;
    uint64 ActiveOperationGeneration = 0;
    FString VisualReceiptPath;
    FString VisualScreenshotPath;
    FString VisualToken;
    FString VisualMode;
    FString VisualLighting;
    FString VisualView;
    FString VisualTerrainCoreId;
    FString VisualCoverEcologyId;
    FString VisualInstallationId;
    FString VisualPackageHash;
    SkiDomain::GeographicBounds VisualRequestedBounds;
    SkiDomain::GeographicBounds VisualActualBounds;
    FString VisualDatum;
    uint32 VisualTerrainWidth = 0;
    uint32 VisualTerrainHeight = 0;
    double VisualEastSpacingM = 0.0;
    double VisualNorthSpacingM = 0.0;
    int32 VisualLod = 0;
    int32 VisualCaptureWidth = 2560;
    int32 VisualCaptureHeight = 1440;
    FString UiLayoutReceiptPath;
    FString UiLayoutToken;
    FString UiLayoutState;
    bool bUiInputIsolationValid = false;
    bool bUiLayoutRunInputIsolation = false;
    bool bTerrainCoreInitialFramePending = false;
    FString UiInputIsolationError;
    FString PerformanceReceiptPath;
    FString PerformanceFramesPath;
    FString PerformanceToken;
    TArray<double> PerformanceLowFrameMs;
    TArray<double> PerformanceReferenceFrameMs;
    int32 PerformancePhaseFrame = 0;
    int32 PerformancePhase = 0;
    double PerformanceReopenSeconds = 0.0;
    double PerformanceFirstRenderSeconds = 0.0;
};
