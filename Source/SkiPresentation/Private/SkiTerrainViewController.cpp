#include "SkiTerrainViewController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputKeyEventArgs.h"
#include "InputCoreTypes.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"

ASkiTerrainViewController::ASkiTerrainViewController()
{
    PrimaryActorTick.bCanEverTick = true;
    bShowMouseCursor = true;
    DefaultMouseCursor = EMouseCursor::Crosshairs;
}

void ASkiTerrainViewController::SetupInputComponent()
{
    Super::SetupInputComponent();
    InputComponent->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ASkiTerrainViewController::BeginOrbit);
    InputComponent->BindKey(EKeys::RightMouseButton, IE_Released, this, &ASkiTerrainViewController::EndOrbit);
    InputComponent->BindKey(EKeys::MiddleMouseButton, IE_Pressed, this, &ASkiTerrainViewController::BeginPan);
    InputComponent->BindKey(EKeys::MiddleMouseButton, IE_Released, this, &ASkiTerrainViewController::EndPan);
    InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ASkiTerrainViewController::Probe);
    InputComponent->BindKey(EKeys::LeftMouseButton, IE_DoubleClick, this, &ASkiTerrainViewController::FocusProbe);
    InputComponent->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ASkiTerrainViewController::ZoomIn);
    InputComponent->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ASkiTerrainViewController::ZoomOut);
    InputComponent->BindKey(EKeys::F, IE_Pressed, this, &ASkiTerrainViewController::InputFrameAll);
    InputComponent->BindKey(EKeys::Home, IE_Pressed, this, &ASkiTerrainViewController::InputFrameAll);
    InputComponent->BindKey(EKeys::One, IE_Pressed, this, &ASkiTerrainViewController::InputFrameAll);
    InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ASkiTerrainViewController::InputFrameSteepest);
    InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ASkiTerrainViewController::InputFrameLastProbe);
    InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &ASkiTerrainViewController::CancelGesture);
}

void ASkiTerrainViewController::AttachTerrain(ASkiTerrainActor* InTerrain)
{
    Terrain = InTerrain;
    if (!CameraActor) CameraActor = GetWorld()->SpawnActor<ACameraActor>();
    if (CameraActor)
    {
        CameraActor->GetCameraComponent()->SetFieldOfView(50.0F);
        SetViewTarget(CameraActor);
    }
    FrameAll();
}

void ASkiTerrainViewController::SetStatusHandler(TFunction<void(const FString&)> Handler)
{
    StatusHandler = std::move(Handler);
}

void ASkiTerrainViewController::SetUiGeometryHandlers(TFunction<double()> RightInsetProvider,
    TFunction<bool()> PointerBlockedProvider, TFunction<bool()> KeyboardBlockedProvider)
{
    UiRightInsetProvider = std::move(RightInsetProvider);
    UiPointerBlockedProvider = std::move(PointerBlockedProvider);
    UiKeyboardBlockedProvider = std::move(KeyboardBlockedProvider);
}

bool ASkiTerrainViewController::RunInputIsolationRegression(const FVector2D PanelPoint,
    const FVector2D TerrainPoint, TFunction<void()> FocusUi, FString& OutError)
{
    OutError.Reset();
    if (!Terrain || !CameraActor || !GEngine || !GEngine->GameViewport
        || !GEngine->GameViewport->Viewport || !FSlateApplication::IsInitialized())
    {
        OutError = TEXT("Input regression requires packaged terrain, camera, Slate, and game viewport.");
        return false;
    }
    const auto SendInput = [&](const FKey& Key, const EInputEvent Event)
    {
        const FInputDeviceId InputDevice = IPlatformInputDeviceMapper::Get()
            .GetPrimaryInputDeviceForUser(GetPlatformUserId());
        const FInputKeyEventArgs Arguments = FInputKeyEventArgs::CreateSimulated(Key, Event, 1.0F,
            -1, InputDevice, false, GEngine->GameViewport->Viewport);
        return GEngine->GameViewport->InputKey(Arguments);
    };
    const auto FlushInput = [&]()
    {
        // Viewport input is queued in UPlayerInput. Exercise the real component
        // stack now rather than inspecting action state before its input tick.
        TickPlayerInput(1.0F / 60.0F, false);
    };
    Pivot += FVector(321.0, -123.0, 77.0);
    Distance = FMath::Clamp(Distance * 0.71, MinimumDistance, MaximumDistance);
    Yaw += 11.0F;
    UpdateCamera();
    const FVector BlockedPivot = Pivot;
    const double BlockedDistance = Distance;
    const float BlockedYaw = Yaw;
    SkiDomain::RayHit PriorProbe;
    PriorProbe.Hit = true;
    PriorProbe.Position = {0.0, 0.0, Terrain->GetOriginHeightM()};
    LastProbe = PriorProbe;

    FSlateApplication::Get().SetCursorPos(PanelPoint);
    if (FocusUi) FocusUi();
    const bool bActualProvidersBlock = UiPointerBlockedProvider && UiPointerBlockedProvider()
        && UiKeyboardBlockedProvider && UiKeyboardBlockedProvider();
    SendInput(EKeys::RightMouseButton, IE_Pressed);
    SendInput(EKeys::MouseScrollUp, IE_Pressed);
    SendInput(EKeys::LeftMouseButton, IE_DoubleClick);
    SendInput(EKeys::F, IE_Pressed);
    SendInput(EKeys::Two, IE_Pressed);
    SendInput(EKeys::Three, IE_Pressed);
    FlushInput();
    SendInput(EKeys::RightMouseButton, IE_Released);
    SendInput(EKeys::MouseScrollUp, IE_Released);
    SendInput(EKeys::LeftMouseButton, IE_Released);
    SendInput(EKeys::F, IE_Released);
    SendInput(EKeys::Two, IE_Released);
    SendInput(EKeys::Three, IE_Released);
    FlushInput();
    if (!bActualProvidersBlock || bOrbiting || bPanning || !Pivot.Equals(BlockedPivot, 0.01)
        || !FMath::IsNearlyEqual(Distance, BlockedDistance, 0.01)
        || !FMath::IsNearlyEqual(Yaw, BlockedYaw, 0.01F))
    {
        OutError = TEXT("Real panel focus/pointer providers or input bindings allowed terrain camera input.");
        return false;
    }

    FBox TerrainBounds;
    FVector2D ProjectedTerrainPoint;
    const bool bProbePointProjected = Terrain->GetValidWorldBounds(TerrainBounds)
        && ProjectWorldLocationToScreen(TerrainBounds.GetCenter(), ProjectedTerrainPoint, true);
    if (bProbePointProjected)
    {
        SetMouseLocation(FMath::RoundToInt(ProjectedTerrainPoint.X),
            FMath::RoundToInt(ProjectedTerrainPoint.Y));
    }
    else
    {
        FSlateApplication::Get().SetCursorPos(TerrainPoint);
    }
    if (FocusUi) FocusUi();
    const bool bTerrainPointerUnblocked = UiPointerBlockedProvider && !UiPointerBlockedProvider();
    const bool bUiFocusedBeforeClaim = UiKeyboardBlockedProvider && UiKeyboardBlockedProvider();
    SendInput(EKeys::RightMouseButton, IE_Pressed);
    FlushInput();
    const bool bOrbitClaimed = bOrbiting && !bPanning
        && UiKeyboardBlockedProvider && !UiKeyboardBlockedProvider();
    SendInput(EKeys::Escape, IE_Pressed);
    FlushInput();
    const bool bOrbitReleased = !bOrbiting && !bPanning;
    SendInput(EKeys::RightMouseButton, IE_Released);
    SendInput(EKeys::Escape, IE_Released);
    FlushInput();
    SendInput(EKeys::MiddleMouseButton, IE_Pressed);
    FlushInput();
    const bool bPanClaimed = bPanning && !bOrbiting;
    SendInput(EKeys::MiddleMouseButton, IE_Released);
    FlushInput();
    const bool bPanReleased = !bOrbiting && !bPanning;

    const double DistanceBeforeZoom = Distance;
    SendInput(EKeys::MouseScrollUp, IE_Pressed);
    FlushInput();
    const bool bZoomWorked = Distance < DistanceBeforeZoom;
    SendInput(EKeys::MouseScrollUp, IE_Released);
    FlushInput();

    LastProbe.Reset();
    SendInput(EKeys::LeftMouseButton, IE_DoubleClick);
    FlushInput();
    const bool bFocusProbeWorked = bProbePointProjected && bTerrainPointerUnblocked && LastProbe.IsSet();
    SendInput(EKeys::LeftMouseButton, IE_Released);
    FlushInput();

    Pivot += FVector(927.0, -613.0, 211.0);
    Distance = FMath::Clamp(Distance * 0.63, MinimumDistance, MaximumDistance);
    Yaw = 17.0F;
    UpdateCamera();
    const FVector BeforeFrameAll = Pivot;
    SendInput(EKeys::F, IE_Pressed);
    FlushInput();
    const bool bFrameAllWorked = !Pivot.Equals(BeforeFrameAll, 0.01) && FMath::IsNearlyEqual(Yaw, 225.0F);
    SendInput(EKeys::F, IE_Released);
    FlushInput();

    Pivot += FVector(-811.0, 447.0, -139.0);
    Yaw = 33.0F;
    UpdateCamera();
    const FVector BeforeFrameSteepest = Pivot;
    SendInput(EKeys::Two, IE_Pressed);
    FlushInput();
    const bool bFrameSteepestWorked = !Pivot.Equals(BeforeFrameSteepest, 0.01)
        && FMath::IsNearlyEqual(Yaw, 225.0F);
    SendInput(EKeys::Two, IE_Released);
    FlushInput();

    const FVector BeforeFrameProbe = Pivot;
    const double DistanceBeforeFrameProbe = Distance;
    SendInput(EKeys::Three, IE_Pressed);
    FlushInput();
    const bool bFrameProbeWorked = LastProbe.IsSet()
        && (!Pivot.Equals(BeforeFrameProbe, 0.01) || Distance < DistanceBeforeFrameProbe);
    SendInput(EKeys::Three, IE_Released);
    FlushInput();

    if (!bTerrainPointerUnblocked || !bUiFocusedBeforeClaim || !bOrbitClaimed || !bOrbitReleased
        || !bPanClaimed || !bPanReleased
        || !bZoomWorked || !bFocusProbeWorked || !bFrameAllWorked || !bFrameSteepestWorked
        || !bFrameProbeWorked)
    {
        OutError = FString::Printf(TEXT("Input ownership failed: projected=%d terrain-unblocked=%d "
            "ui-focused=%d orbit-claimed=%d "
            "orbit-released=%d pan-claimed=%d pan-released=%d zoom=%d focus-probe=%d "
            "frame-all=%d frame-steepest=%d frame-probe=%d."),
            bProbePointProjected, bTerrainPointerUnblocked, bUiFocusedBeforeClaim, bOrbitClaimed,
            bOrbitReleased, bPanClaimed, bPanReleased,
            bZoomWorked, bFocusProbeWorked, bFrameAllWorked, bFrameSteepestWorked, bFrameProbeWorked);
        return false;
    }
    return true;
}

void ASkiTerrainViewController::FrameBounds(const FBox& Bounds)
{
    if (!Bounds.IsValid || !CameraActor) return;
    Pivot = Bounds.GetCenter();
    const double Radius = FMath::Max(100.0, Bounds.GetExtent().Size());
    int32 ViewWidth = 1920, ViewHeight = 1080;
    GetViewportSize(ViewWidth, ViewHeight);
    const double RightInset = UiRightInsetProvider ? FMath::Clamp(UiRightInsetProvider(), 0.0, ViewWidth * 0.72) : 0.0;
    const double UnobstructedWidth = FMath::Max(1.0, static_cast<double>(ViewWidth) - RightInset);
    const double Aspect = UnobstructedWidth / FMath::Max(1.0, static_cast<double>(ViewHeight));
    const double VerticalHalf = FMath::DegreesToRadians(25.0);
    const double HorizontalHalf = FMath::Atan(FMath::Tan(VerticalHalf) * Aspect);
    Distance = Radius / FMath::Max(0.05, FMath::Sin(FMath::Min(VerticalHalf, HorizontalHalf))) / 0.92;
    MinimumDistance = Radius * 0.08;
    MaximumDistance = Radius * 12.0;
    Yaw = 225.0F; Pitch = -55.0F;
    LastViewportWidth = ViewWidth; LastViewportHeight = ViewHeight; LastRightInset = RightInset;
    UpdateCamera();
}

void ASkiTerrainViewController::FrameAll() { FBox B; if (Terrain && Terrain->GetValidWorldBounds(B)) FrameBounds(B); }
void ASkiTerrainViewController::FrameSteepest() { FBox B; if (Terrain && Terrain->GetSteepestQuadrantWorldBounds(B)) FrameBounds(B); }
void ASkiTerrainViewController::FrameLastProbe()
{
    if (!Terrain || !LastProbe.IsSet()) return;
    const SkiDomain::RayHit& Hit = LastProbe.GetValue();
    const FVector Local(Hit.Position.North * 100.0, Hit.Position.East * 100.0,
        (Hit.Position.Up - Terrain->GetOriginHeightM()) * 100.0);
    Pivot = Terrain->GetActorTransform().TransformPosition(Local);
    Distance = FMath::Clamp(Distance * 0.35, MinimumDistance, MaximumDistance);
    UpdateCamera();
}

void ASkiTerrainViewController::PlayerTick(const float DeltaSeconds)
{
    Super::PlayerTick(DeltaSeconds);
    int32 ViewWidth = 0, ViewHeight = 0; GetViewportSize(ViewWidth, ViewHeight);
    const double RightInset = UiRightInsetProvider ? UiRightInsetProvider() : 0.0;
    if (Terrain && ViewWidth > 0 && ViewHeight > 0 && !bOrbiting && !bPanning
        && (ViewWidth != LastViewportWidth || ViewHeight != LastViewportHeight
            || FMath::Abs(RightInset - LastRightInset) > 1.0))
    {
        LastViewportWidth = ViewWidth;
        LastViewportHeight = ViewHeight;
        LastRightInset = RightInset;
        UpdateCamera();
    }
    if (bOrbiting && !IsInputKeyDown(EKeys::RightMouseButton)) bOrbiting = false;
    if (bPanning && !IsInputKeyDown(EKeys::MiddleMouseButton)) bPanning = false;
    if (!CameraActor || (!bOrbiting && !bPanning)) return;
    float DX = 0.0F, DY = 0.0F; GetInputMouseDelta(DX, DY);
    if (bOrbiting) { Yaw = FMath::UnwindDegrees(Yaw + DX * 0.25F); Pitch = FMath::Clamp(Pitch - DY * 0.20F, -85.0F, -12.0F); }
    if (bPanning)
    {
        const FRotationMatrix Matrix(FRotator(Pitch, Yaw, 0));
        const double Scale = Distance * 0.0012;
        Pivot += Matrix.GetScaledAxis(EAxis::Y) * (-DX * Scale);
        Pivot += Matrix.GetScaledAxis(EAxis::Z) * (DY * Scale);
    }
    UpdateCamera();
}

void ASkiTerrainViewController::UpdateCamera()
{
    if (!CameraActor) return;
    const FRotator Rotation(Pitch, Yaw, 0);
    int32 ViewWidth = 0, ViewHeight = 0; GetViewportSize(ViewWidth, ViewHeight);
    const double RightInset = UiRightInsetProvider ? FMath::Clamp(UiRightInsetProvider(), 0.0,
        static_cast<double>(ViewWidth) * 0.72) : 0.0;
    const double Aspect = static_cast<double>(ViewWidth) / FMath::Max(1, ViewHeight);
    const double HorizontalHalf = FMath::Atan(FMath::Tan(FMath::DegreesToRadians(25.0)) * Aspect);
    const double Shift = ViewWidth > 0 ? RightInset / ViewWidth * Distance * FMath::Tan(HorizontalHalf) : 0.0;
    const FVector Right = FRotationMatrix(Rotation).GetScaledAxis(EAxis::Y);
    CameraActor->SetActorLocationAndRotation(Pivot - Rotation.Vector() * Distance + Right * Shift, Rotation);
}
void ASkiTerrainViewController::ClaimTerrainInput(){if(FSlateApplication::IsInitialized())FSlateApplication::Get().SetAllUserFocusToGameViewport();}
bool ASkiTerrainViewController::IsKeyboardInputBlocked()const{return UiKeyboardBlockedProvider&&UiKeyboardBlockedProvider();}
void ASkiTerrainViewController::BeginOrbit() { if(UiPointerBlockedProvider&&UiPointerBlockedProvider())return;ClaimTerrainInput();bOrbiting = true; bPanning = false; }
void ASkiTerrainViewController::EndOrbit() { bOrbiting = false; }
void ASkiTerrainViewController::BeginPan() { if(UiPointerBlockedProvider&&UiPointerBlockedProvider())return;ClaimTerrainInput();bPanning = true; bOrbiting = false; }
void ASkiTerrainViewController::EndPan() { bPanning = false; }
void ASkiTerrainViewController::CancelGesture() { bOrbiting = false; bPanning = false; }
void ASkiTerrainViewController::ZoomIn() { if(UiPointerBlockedProvider&&UiPointerBlockedProvider())return;Distance = FMath::Clamp(Distance * 0.84, MinimumDistance, MaximumDistance); UpdateCamera(); }
void ASkiTerrainViewController::ZoomOut() { if(UiPointerBlockedProvider&&UiPointerBlockedProvider())return;Distance = FMath::Clamp(Distance * 1.19, MinimumDistance, MaximumDistance); UpdateCamera(); }
void ASkiTerrainViewController::Probe(){TryProbe();}
bool ASkiTerrainViewController::TryProbe()
{
    if(UiPointerBlockedProvider&&UiPointerBlockedProvider())return false;
    ClaimTerrainInput();
    if (!Terrain) return false;
    FVector Origin, Direction;
    if (!DeprojectMousePositionToWorld(Origin, Direction)) return false;
    const SkiDomain::RayHit Hit = Terrain->QueryCanonical(Origin, Direction);
    if (!Hit.Hit) return false;
    LastProbe = Hit; Terrain->ShowTopologyPatch(Hit);
    if (StatusHandler) StatusHandler(Terrain->DescribeProbe(Hit));
    return true;
}
void ASkiTerrainViewController::FocusProbe() { if(TryProbe())FrameLastProbe(); }
void ASkiTerrainViewController::InputFrameAll(){if(!IsKeyboardInputBlocked())FrameAll();}
void ASkiTerrainViewController::InputFrameSteepest(){if(!IsKeyboardInputBlocked())FrameSteepest();}
void ASkiTerrainViewController::InputFrameLastProbe(){if(!IsKeyboardInputBlocked())FrameLastProbe();}
FString ASkiTerrainViewController::DescribeCamera() const
{
    return FString::Printf(TEXT("Camera yaw %.0f pitch %.0f distance %.0f m"), Yaw, Pitch, Distance / 100.0);
}
