#pragma once

#include "GameFramework/GameModeBase.h"
#include "TimerManager.h"
#include "SkiApplication/TerrainCoreSession.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include <memory>
#include "SkiBootstrapGameMode.generated.h"

class ASkiTerrainActor;
class USkiP1Widget;
struct FSkiPreparedInstalledTerrain;
struct FSkiInstalledPhotoStreamState;
namespace SkiPreparation
{
struct InstalledTerrainIndex;
struct SiteContextPackageIndex;
}

UCLASS()
class SKIPRESENTATION_API ASkiBootstrapGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ASkiBootstrapGameMode();
    void OpenLatestInstalledTerrain();
    /** Resolves Mountain component IDs only after InstalledTerrainStore::Open succeeded. */
    static bool ResolveVerifiedInstalledTerrainComponents(const FString& ContentId,
        const SkiPreparation::InstalledTerrainIndex& Index, bool bStoreOpenVerified,
        FString& OutTerrainCoreId, FString& OutCoverEcologyId);
    static bool IsPickerViewportScrollAtEnd(float ScrollOffset, float ScrollMaximum,
        float Tolerance = 1.0F) noexcept;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    bool RunP1Smoke();
    bool BeginP1UiLayoutSmoke();
    void FinishP1UiLayoutSmoke();
#if !UE_BUILD_SHIPPING
    bool BeginP1PickerViewportSmoke();
    void InspectP1PickerViewportTop();
    void InspectP1PickerViewportBoundary();
    void InspectP1PickerViewportName();
    void InspectP1PickerViewportBottom();
    void RequestP1PickerViewportScreenshot();
    void FinishP1PickerViewportSmoke();
#endif
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
    bool OpenInstalledTerrain(const FString& ContentId,
        std::shared_ptr<FSkiPreparedInstalledTerrain> Prepared = {});
    void BeginInstalledPhotoPresentation();
    void RefreshInstalledPhotoSelection();
    void PumpInstalledPhotoCompletions();
    void IssueInstalledPhotoTileReads();
    void CompleteInstalledPhotoTileRead(uint64 RequestSerial, uint64 Generation,
        SkiApplication::TerrainCoreTileKey Key, bool bSucceeded, TArray<FColor> Pixels,
        const FString& Error);
    void CancelInstalledPhotoPresentation();
    void ClearInstalledPhotoContext();
    void TransitionToInstalledTerrain(const FString& ContentId);
    void RefreshInstalledLibrary();

    UPROPERTY()
    TObjectPtr<USkiP1Widget> P1Widget;

    UPROPERTY()
    TObjectPtr<ASkiTerrainActor> TerrainActor;

    TSharedPtr<SkiApplication::TerrainSession> TerrainSession;
    TSharedPtr<SkiApplication::TerrainCoreSession> TerrainCoreSession;
    TSharedPtr<SkiPreparation::Cancellation> PreparationCancellation;
    TSharedPtr<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe> PreparationLease;
    // Kept for the entire installed Mountain session, not just package opening.
    TUniquePtr<SkiPreparation::ScopedAcquisitionPortDeny> MountainAcquisitionDeny;
    std::shared_ptr<const SkiPreparation::SiteContextPackageIndex> InstalledPhotoSiteContext;
    std::shared_ptr<FSkiInstalledPhotoStreamState> InstalledPhotoStream;
    FString InstalledPhotoDataRoot;
    FTimerHandle InstalledPhotoSelectionPollTimer;
    uint64 InstalledPhotoRequestSerial = 0;
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
#if !UE_BUILD_SHIPPING
    FString PickerViewportReceiptPath;
    FString PickerViewportScreenshotPath;
    FString PickerViewportToken;
    FString PickerViewportTopRectsJson;
    FString PickerViewportTextJson;
    FString PickerViewportStepsJson;
    FString PickerViewportStep1EvidenceJson;
    FString PickerViewportStep2EvidenceJson;
    FString PickerViewportStep3EvidenceJson;
    FVector4 PickerViewportScrollRect = FVector4(0, 0, 0, 0);
    float PickerViewportScrollAtStart = 0.0F;
    float PickerViewportScrollAtEnd = 0.0F;
    float PickerViewportScrollMaximum = 0.0F;
    int32 PickerViewportWidth = 0;
    int32 PickerViewportHeight = 0;
    bool bPickerViewportTopValid = false;
    bool bPickerViewportBottomValid = false;
#endif
    bool bTerrainCoreInitialFramePending = false;
    uint64 LibraryRefreshGeneration = 0;
    uint64 InstalledOpenGeneration = 0;
    uint64 MountainPrepareGeneration = 0;
    FString PendingInstalledOpenId;
    FString DeferredInstalledOpenId;
    FString InstalledOpenDataRootOverride;
    bool bInstalledVerifierBusy = false;
    FString ReturnErrorNotice;
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
    bool bPerformanceCameraFramed = false;
    int32 PerformanceLowRenderedTiles = 0;
    int32 PerformanceReferenceRenderedTiles = 0;
};
