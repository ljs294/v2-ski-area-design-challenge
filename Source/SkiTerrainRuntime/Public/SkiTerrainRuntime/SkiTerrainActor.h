#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiDomain/TerrainTile.h"
#include "SkiTerrainActor.generated.h"

class UDynamicMeshComponent;
class UInstancedStaticMeshComponent;
class UCameraComponent;
class UDirectionalLightComponent;
class USkyLightComponent;
class ULineBatchComponent;

UCLASS()
class SKITERRAINRUNTIME_API ASkiTerrainActor : public AActor
{
    GENERATED_BODY()

public:
    ASkiTerrainActor();

    bool Present(const SkiApplication::TerrainSnapshot& Snapshot, uint8 Lod = 0);
    bool ApplyScratchMutation(const FVector2D& CenterEastNorthM, double RadiusM, double DeltaM);
    void ApplyScratchMutationAsync(const FVector2D& CenterEastNorthM, double RadiusM, double DeltaM,
        TFunction<void(bool)> Completion);
    SkiDomain::RayHit QueryCanonical(const FVector& WorldOriginCm, const FVector& WorldDirection) const;
    void SetTerrainSession(TSharedPtr<SkiApplication::TerrainSession> InSession);
    void SetLightingPreset(FName Preset);

private:
    void ClearTiles();
    bool BuildTileComponent(const SkiDomain::Heightfield& Field, const SkiDomain::TileKey& Key, double OriginHeightM);
    bool CreateTileComponent(const SkiDomain::TerrainTileMesh& SourceMesh, double OriginHeightM);
    void RebuildDots(const SkiDomain::Heightfield& Field, double OriginHeightM);
    void RebuildContourOverlay(const SkiDomain::Heightfield& Field, double OriginHeightM);

    UPROPERTY()
    TObjectPtr<USceneComponent> SceneRoot;

    UPROPERTY()
    TArray<TObjectPtr<UDynamicMeshComponent>> Tiles;
    TArray<SkiDomain::TileKey> TileKeys;

    UPROPERTY()
    TObjectPtr<UInstancedStaticMeshComponent> GuestDots;

    UPROPERTY()
    TObjectPtr<UCameraComponent> TerrainCamera;

    UPROPERTY()
    TObjectPtr<UDirectionalLightComponent> SunLight;

    UPROPERTY()
    TObjectPtr<USkyLightComponent> SkyLight;

    UPROPERTY()
    TObjectPtr<ULineBatchComponent> OverlayLines;

    TSharedPtr<SkiApplication::TerrainSession> Session;
    std::shared_ptr<const std::vector<std::uint8_t>> PresentedCover;
    std::uint32_t PresentedCoverWidth = 0;
    std::uint32_t PresentedCoverHeight = 0;
    SkiDomain::Revision PresentedRevision = 0;
    double PresentedOriginHeightM = 0.0;
    uint8 PresentedLod = 0;
    FName CurrentLightingPreset = TEXT("Midday");
};
