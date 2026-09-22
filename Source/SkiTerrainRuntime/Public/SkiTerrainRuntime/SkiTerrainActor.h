#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SkiApplication/TerrainCoreSession.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiDomain/TerrainTile.h"
#include "SkiTerrainRuntime/TerrainCoreTileCache.h"
#include "SkiTerrainRuntime/TerrainCoreLodController.h"

#include <memory>
#include <vector>

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
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    bool Present(const SkiApplication::TerrainSnapshot& Snapshot, uint8 Lod = 0);
    /** Present the disk-backed TerrainCore path used by packaged Medium/High terrain. */
    bool PresentTerrainCore(TSharedPtr<SkiApplication::TerrainCoreSession> InSession,
        uint8 Lod = 4, double TimeoutSeconds = 30.0);
    bool BeginTerrainCoreStreaming(TSharedPtr<SkiApplication::TerrainCoreSession> InSession,
        uint8 Lod = 4);
    void SetTerrainCoreCover(std::shared_ptr<const std::vector<std::uint8_t>> Cover,
        std::uint32_t Width, std::uint32_t Height);
    void SetTerrainCoreReadyHandler(TFunction<void(bool)> Handler)
    { CoreReadyHandler = MoveTemp(Handler); }
    bool ApplyScratchMutation(const FVector2D& CenterEastNorthM, double RadiusM, double DeltaM);
    void ApplyScratchMutationAsync(const FVector2D& CenterEastNorthM, double RadiusM, double DeltaM,
        TFunction<void(bool)> Completion);
    SkiDomain::RayHit QueryCanonical(const FVector& WorldOriginCm, const FVector& WorldDirection) const;
    void SetTerrainSession(TSharedPtr<SkiApplication::TerrainSession> InSession);
    void SetLightingPreset(FName Preset);
    bool SetLod(uint8 Lod);
    bool SetLodAuto();
    void SetViewMode(ESkiTerrainViewMode Mode);
    void SetVerticalExaggeration(float Scale);
    bool GetValidWorldBounds(FBox& OutBounds) const;
    bool GetSteepestQuadrantWorldBounds(FBox& OutBounds) const;
    void ShowTopologyPatch(const SkiDomain::RayHit& Hit);
    uint8 GetPresentedLod() const noexcept { return PresentedLod; }
    ESkiTerrainViewMode GetViewMode() const noexcept { return CurrentViewMode; }
    double GetOriginHeightM() const noexcept { return PresentedOriginHeightM; }
    TSharedPtr<SkiApplication::TerrainSession> GetTerrainSession() const { return Session; }
    TSharedPtr<SkiApplication::TerrainCoreSession> GetTerrainCoreSession() const { return CoreSession; }
    bool IsUsingTerrainCore() const noexcept { return CoreSession.IsValid(); }
    int32 GetRenderedTerrainCoreTileCount() const noexcept { return CoreRenderedKeys.Num(); }
    SkiTerrainRuntime::TerrainCoreCacheStats GetTerrainCoreCacheStats() const
    { return CoreCache ? CoreCache->Stats() : SkiTerrainRuntime::TerrainCoreCacheStats{}; }
    bool IsTerrainCoreRevisionAligned() const;
    uint64 GetRejectedTerrainCoreMeshBuilds() const noexcept { return RejectedCoreMeshBuilds; }
    /** Uses the same publication fence as asynchronous mesh builds. Packaged regression only. */
    bool RunStaleTerrainCoreMeshPublicationProbe();

private:
    void ClearTiles();
    bool BuildTileComponent(const SkiDomain::Heightfield& Field, const SkiDomain::TileKey& Key, double OriginHeightM);
    bool CreateTileComponent(const SkiDomain::TerrainTileMesh& SourceMesh, double OriginHeightM);
    bool PresentTerrainCoreLod(uint8 Lod, double TimeoutSeconds);
    SkiDomain::RayHit QueryTerrainCore(const FVector& WorldOriginCm,
        const FVector& WorldDirection) const;
    void PumpTerrainCoreStreaming();
    void UpdateTerrainCoreAutoLod(float DeltaSeconds);
    void ReleaseTerrainCorePins();
    void FailTerrainCorePresentation();
    void CompleteScratchMutation(bool bSucceeded);
    void CancelScratchMutation();
    bool ApplyTerrainCoreSelection(
        TArray<SkiApplication::TerrainCoreTileKey> Desired, bool bClearVisibleTiles,
        bool bForceRebuild = false);
    bool PublishTerrainCoreMesh(uint64 ExpectedSerial, uint64 ExpectedGeneration,
        SkiDomain::Revision ExpectedRevision, uint64 EncodedKey,
        const SkiApplication::TerrainCoreTilePayload& Payload,
        const SkiDomain::TerrainTileMesh& Mesh, bool bBuilt);
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
    TSharedPtr<SkiApplication::TerrainCoreSession> CoreSession;
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> CoreBaseRepository;
    SkiDomain::TerrainEditSet CoreEdits;
    mutable TUniquePtr<SkiTerrainRuntime::TerrainCoreTileCache> CoreCache;
    TUniquePtr<SkiTerrainRuntime::TerrainCoreLodController> CoreLodController;
    TSet<uint64> CoreRenderedKeys;
    TSet<uint64> CoreRequestedKeys;
    TMap<uint64, uint64> CoreMeshBuildsInFlight;
    TSet<uint64> CoreFailedMeshKeys;
    TArray<SkiApplication::TerrainCoreTileKey> CoreDesiredKeys;
    uint64 CoreCacheGeneration = 0;
    uint64 CorePresentationSerial = 0;
    uint64 RejectedCoreMeshBuilds = 0;
    bool bCoreBoundsHaveSamples = false;
    bool bCoreReadyNotified = false;
    TFunction<void(bool)> CoreReadyHandler;
    TFunction<void(bool)> ScratchMutationCompletion;
    uint64 ScratchMutationSerial = 0;
    bool bScratchMutationInFlight = false;
    float CoreAutoLodElapsedSeconds = 0.0F;
    bool bTerrainCoreAutoLod = false;
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
