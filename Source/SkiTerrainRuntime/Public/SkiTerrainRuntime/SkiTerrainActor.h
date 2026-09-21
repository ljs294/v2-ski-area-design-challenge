#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiDomain/TerrainTile.h"
#include "SkiTerrainActor.generated.h"

class UDynamicMeshComponent;
class UInstancedStaticMeshComponent;
class UDirectionalLightComponent;
class USkyLightComponent;
class USkyAtmosphereComponent;
class UPostProcessComponent;
class ULineBatchComponent;

UENUM(BlueprintType)
enum class ESkiTerrainViewMode : uint8
{
    Presentation,
    Elevation,
    Slope,
    Cover,
    TileLod,
};

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
    bool SetLod(uint8 Lod);
    void SetViewMode(ESkiTerrainViewMode Mode);
    void SetVerticalExaggeration(float Scale);
    bool GetValidWorldBounds(FBox& OutBounds) const;
    bool GetSteepestQuadrantWorldBounds(FBox& OutBounds) const;
    void ShowTopologyPatch(const SkiDomain::RayHit& Hit);
    uint8 GetPresentedLod() const noexcept { return PresentedLod; }
    ESkiTerrainViewMode GetViewMode() const noexcept { return CurrentViewMode; }
    double GetOriginHeightM() const noexcept { return PresentedOriginHeightM; }
    TSharedPtr<SkiApplication::TerrainSession> GetTerrainSession() const { return Session; }

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
    TObjectPtr<UDirectionalLightComponent> SunLight;

    UPROPERTY()
    TObjectPtr<USkyLightComponent> SkyLight;

    UPROPERTY()
    TObjectPtr<USkyAtmosphereComponent> SkyAtmosphere;

    UPROPERTY()
    TObjectPtr<UPostProcessComponent> VerificationPostProcess;

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
    ESkiTerrainViewMode CurrentViewMode = ESkiTerrainViewMode::Presentation;
    FBox ValidLocalBounds = FBox(ForceInit);
    FBox SteepestLocalBounds = FBox(ForceInit);
    double MinimumHeightM = 0.0;
    double MaximumHeightM = 1.0;
};
