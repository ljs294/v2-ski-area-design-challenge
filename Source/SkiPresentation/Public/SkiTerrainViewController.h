#pragma once

#include "GameFramework/PlayerController.h"
#include "SkiDomain/Heightfield.h"
#include "SkiTerrainViewController.generated.h"

class ACameraActor;
class ASkiTerrainActor;

UCLASS()
class SKIPRESENTATION_API ASkiTerrainViewController : public APlayerController
{
    GENERATED_BODY()

public:
    ASkiTerrainViewController();
    void AttachTerrain(ASkiTerrainActor* InTerrain);
    void FrameAll();
    void FrameSteepest();
    void FrameLastProbe();
    void SetStatusHandler(TFunction<void(const FString&)> Handler);
    void SetUiGeometryHandlers(TFunction<double()> RightInsetProvider,
        TFunction<bool()> PointerBlockedProvider, TFunction<bool()> KeyboardBlockedProvider);
    bool RunInputIsolationRegression(FVector2D PanelPoint, FVector2D TerrainPoint,
        TFunction<void()> FocusUi, FString& OutError);
    FString DescribeCamera() const;

protected:
    virtual void SetupInputComponent() override;
    virtual void PlayerTick(float DeltaSeconds) override;

private:
    void BeginOrbit(); void EndOrbit(); void BeginPan(); void EndPan(); void CancelGesture();
    void ZoomIn(); void ZoomOut(); void Probe(); bool TryProbe(); void FocusProbe();
    void InputFrameAll(); void InputFrameSteepest(); void InputFrameLastProbe();
    bool IsKeyboardInputBlocked() const; void ClaimTerrainInput();
    void FrameBounds(const FBox& Bounds); void UpdateCamera();

    UPROPERTY() TObjectPtr<ASkiTerrainActor> Terrain;
    UPROPERTY() TObjectPtr<ACameraActor> CameraActor;
    FVector Pivot = FVector::ZeroVector;
    double Distance = 100000.0;
    double MinimumDistance = 1000.0;
    double MaximumDistance = 10000000.0;
    float Yaw = 225.0F;
    float Pitch = -55.0F;
    bool bOrbiting = false;
    bool bPanning = false;
    TOptional<SkiDomain::RayHit> LastProbe;
    TFunction<void(const FString&)> StatusHandler;
    TFunction<double()> UiRightInsetProvider;
    TFunction<bool()> UiPointerBlockedProvider;
    TFunction<bool()> UiKeyboardBlockedProvider;
    int32 LastViewportWidth = 0;
    int32 LastViewportHeight = 0;
    double LastRightInset = -1.0;
};
