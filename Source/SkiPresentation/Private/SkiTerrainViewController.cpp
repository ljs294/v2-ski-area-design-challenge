#include "SkiTerrainViewController.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
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
    InputComponent->BindKey(EKeys::F, IE_Pressed, this, &ASkiTerrainViewController::FrameAll);
    InputComponent->BindKey(EKeys::Home, IE_Pressed, this, &ASkiTerrainViewController::FrameAll);
    InputComponent->BindKey(EKeys::One, IE_Pressed, this, &ASkiTerrainViewController::FrameAll);
    InputComponent->BindKey(EKeys::Two, IE_Pressed, this, &ASkiTerrainViewController::FrameSteepest);
    InputComponent->BindKey(EKeys::Three, IE_Pressed, this, &ASkiTerrainViewController::FrameLastProbe);
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

void ASkiTerrainViewController::FrameBounds(const FBox& Bounds)
{
    if (!Bounds.IsValid || !CameraActor) return;
    Pivot = Bounds.GetCenter();
    const double Radius = FMath::Max(100.0, Bounds.GetExtent().Size());
    int32 ViewWidth = 1920, ViewHeight = 1080;
    GetViewportSize(ViewWidth, ViewHeight);
    const double UnobstructedWidth = FMath::Max(1.0, static_cast<double>(ViewWidth) - FMath::Min(560.0, ViewWidth * 0.38));
    const double Aspect = UnobstructedWidth / FMath::Max(1.0, static_cast<double>(ViewHeight));
    const double VerticalHalf = FMath::DegreesToRadians(25.0);
    const double HorizontalHalf = FMath::Atan(FMath::Tan(VerticalHalf) * Aspect);
    Distance = Radius / FMath::Max(0.05, FMath::Sin(FMath::Min(VerticalHalf, HorizontalHalf))) * 1.08;
    MinimumDistance = Radius * 0.08;
    MaximumDistance = Radius * 12.0;
    Yaw = 225.0F; Pitch = -55.0F;
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
    CameraActor->SetActorLocationAndRotation(Pivot - Rotation.Vector() * Distance, Rotation);
}
void ASkiTerrainViewController::BeginOrbit() { bOrbiting = true; bPanning = false; }
void ASkiTerrainViewController::EndOrbit() { bOrbiting = false; }
void ASkiTerrainViewController::BeginPan() { bPanning = true; bOrbiting = false; }
void ASkiTerrainViewController::EndPan() { bPanning = false; }
void ASkiTerrainViewController::CancelGesture() { bOrbiting = false; bPanning = false; }
void ASkiTerrainViewController::ZoomIn() { Distance = FMath::Clamp(Distance * 0.84, MinimumDistance, MaximumDistance); UpdateCamera(); }
void ASkiTerrainViewController::ZoomOut() { Distance = FMath::Clamp(Distance * 1.19, MinimumDistance, MaximumDistance); UpdateCamera(); }
void ASkiTerrainViewController::Probe()
{
    if (!Terrain) return;
    FVector Origin, Direction;
    if (!DeprojectMousePositionToWorld(Origin, Direction)) return;
    const SkiDomain::RayHit Hit = Terrain->QueryCanonical(Origin, Direction);
    if (!Hit.Hit) return;
    LastProbe = Hit; Terrain->ShowTopologyPatch(Hit);
    if (StatusHandler) StatusHandler(FString::Printf(TEXT("Probe r%u c%u | %.2f m | revision %llu"),
        Hit.Row, Hit.Column, Hit.Position.Up, static_cast<uint64>(Hit.SourceRevision)));
}
void ASkiTerrainViewController::FocusProbe() { Probe(); FrameLastProbe(); }
FString ASkiTerrainViewController::DescribeCamera() const
{
    return FString::Printf(TEXT("Camera yaw %.0f pitch %.0f distance %.0f m"), Yaw, Pitch, Distance / 100.0);
}
