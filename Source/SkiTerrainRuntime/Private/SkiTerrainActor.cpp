#include "SkiTerrainRuntime/SkiTerrainActor.h"

#include "Async/Async.h"
#include "SkiApplication/TerrainCoreEditedRepository.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/PostProcessComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Materials/MaterialInterface.h"
#include "SkiDomain/TerrainTile.h"
#include "SkiTerrainRuntime/TerrainCoreMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"

#include <limits>

namespace
{
FVector3f CoverColor(const uint8 Code)
{
    switch (Code)
    {
    case 1: case 10: return {0.08F, 0.25F, 0.10F}; // fixture/tree cover
    case 2: case 20: case 30: return {0.30F, 0.48F, 0.17F}; // shrub/grass
    case 3: case 70: return {0.85F, 0.88F, 0.90F}; // fixture/high snow
    case 40: return {0.52F, 0.56F, 0.20F}; // cropland
    case 50: return {0.56F, 0.31F, 0.24F}; // built
    case 60: return {0.55F, 0.48F, 0.37F}; // bare
    case 80: return {0.08F, 0.25F, 0.48F}; // water
    case 90: case 95: return {0.10F, 0.38F, 0.32F}; // wetland/mangrove
    case 100: return {0.42F, 0.46F, 0.25F};
    default: return {0.42F, 0.40F, 0.34F};
    }
}

uint64 TerrainCoreRenderKey(const SkiApplication::TerrainCoreTileKey& Key)
{
    return (static_cast<uint64>(Key.Lod) << 56U)
        | (static_cast<uint64>(Key.Y) << 28U) | Key.X;
}

bool BuildCircularTerrainCoreEdit(const SkiDomain::TerrainCoreManifest& Metadata,
    const SkiDomain::Revision BaseRevision, const FVector2D& CenterEastNorthM,
    const double RadiusM, const double DeltaM, SkiDomain::TerrainEditSet& OutEdits)
{
    OutEdits = {};
    constexpr uint64 MaximumScratchSamples = 262144;
    constexpr double MaximumScratchRadiusM = 2000.0;
    if (BaseRevision == 0 || BaseRevision == TNumericLimits<SkiDomain::Revision>::Max()
        || !FMath::IsFinite(RadiusM) || !FMath::IsFinite(DeltaM) || RadiusM <= 0.0
        || RadiusM > MaximumScratchRadiusM || DeltaM == 0.0)
    {
        return false;
    }
    const int64 MinimumColumn = FMath::Max<int64>(0, FMath::FloorToInt64(
        (CenterEastNorthM.X - RadiusM - Metadata.SampleCenterBounds.WestM)
            / Metadata.DeliveredEastSpacingM));
    const int64 MaximumColumn = FMath::Min<int64>(Metadata.Width - 1U, FMath::CeilToInt64(
        (CenterEastNorthM.X + RadiusM - Metadata.SampleCenterBounds.WestM)
            / Metadata.DeliveredEastSpacingM));
    const int64 MinimumRow = FMath::Max<int64>(0, FMath::FloorToInt64(
        (Metadata.SampleCenterBounds.NorthM - CenterEastNorthM.Y - RadiusM)
            / Metadata.DeliveredNorthSpacingM));
    const int64 MaximumRow = FMath::Min<int64>(Metadata.Height - 1U, FMath::CeilToInt64(
        (Metadata.SampleCenterBounds.NorthM - CenterEastNorthM.Y + RadiusM)
            / Metadata.DeliveredNorthSpacingM));
    if (MinimumColumn > MaximumColumn || MinimumRow > MaximumRow) return false;
    const uint64 Columns = static_cast<uint64>(MaximumColumn - MinimumColumn + 1);
    const uint64 Rows = static_cast<uint64>(MaximumRow - MinimumRow + 1);
    if (Rows == 0 || Columns > MaximumScratchSamples
        || Rows > MaximumScratchSamples / Columns) return false;

    OutEdits.TerrainCoreId = Metadata.ContentId;
    OutEdits.BaseRevision = BaseRevision;
    OutEdits.EditRevision = BaseRevision + 1U;
    OutEdits.Deltas.reserve(static_cast<std::size_t>(Columns * Rows));
    const double RadiusSquared = RadiusM * RadiusM;
    for (int64 Row = MinimumRow; Row <= MaximumRow; ++Row)
    {
        const double North = Metadata.SampleCenterBounds.NorthM
            - static_cast<double>(Row) * Metadata.DeliveredNorthSpacingM;
        for (int64 Column = MinimumColumn; Column <= MaximumColumn; ++Column)
        {
            const double East = Metadata.SampleCenterBounds.WestM
                + static_cast<double>(Column) * Metadata.DeliveredEastSpacingM;
            const double EastDelta = East - CenterEastNorthM.X;
            const double NorthDelta = North - CenterEastNorthM.Y;
            if (EastDelta * EastDelta + NorthDelta * NorthDelta <= RadiusSquared)
            {
                OutEdits.Deltas.push_back({static_cast<uint32>(Column),
                    static_cast<uint32>(Row), static_cast<float>(DeltaM)});
            }
        }
    }
    return !OutEdits.Deltas.empty()
        && SkiDomain::ValidateTerrainEditSet(OutEdits, Metadata.Width, Metadata.Height).Ok();
}
}

bool SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
    const FSkiTerrainPhotoTile* PhotoTile, const uint64 ExpectedGeneration,
    const SkiApplication::TerrainCoreTileKey& ExpectedKey,
    const double TileLocalU, const double TileLocalV,
    FVector3f& OutLinearColor) noexcept
{
    constexpr uint64 ExpectedPixelCount =
        static_cast<uint64>(TerrainCorePhotoTileSide) * TerrainCorePhotoTileSide;
    if (!PhotoTile || PhotoTile->Generation != ExpectedGeneration
        || PhotoTile->Key.Lod != ExpectedKey.Lod || PhotoTile->Key.X != ExpectedKey.X
        || PhotoTile->Key.Y != ExpectedKey.Y
        || PhotoTile->BgraPixels.Num() != static_cast<int32>(ExpectedPixelCount)
        || !FMath::IsFinite(TileLocalU) || !FMath::IsFinite(TileLocalV)
        || TileLocalU < 0.0 || TileLocalU > 1.0
        || TileLocalV < 0.0 || TileLocalV > 1.0)
    {
        return false;
    }

    const double PixelX = TileLocalU * (TerrainCorePhotoTileSide - 1U);
    const double PixelY = TileLocalV * (TerrainCorePhotoTileSide - 1U);
    const uint32 X0 = static_cast<uint32>(FMath::FloorToInt(PixelX));
    const uint32 Y0 = static_cast<uint32>(FMath::FloorToInt(PixelY));
    const uint32 X1 = FMath::Min(X0 + 1U, TerrainCorePhotoTileSide - 1U);
    const uint32 Y1 = FMath::Min(Y0 + 1U, TerrainCorePhotoTileSide - 1U);
    const float BlendX = static_cast<float>(PixelX - X0);
    const float BlendY = static_cast<float>(PixelY - Y0);
    const auto DecodeLinear = [PhotoTile](const uint32 X, const uint32 Y)
    {
        const uint32 Index = Y * TerrainCorePhotoTileSide + X;
        return FLinearColor::FromSRGBColor(PhotoTile->BgraPixels[Index]);
    };
    const FLinearColor C00 = DecodeLinear(X0, Y0);
    const FLinearColor C10 = DecodeLinear(X1, Y0);
    const FLinearColor C01 = DecodeLinear(X0, Y1);
    const FLinearColor C11 = DecodeLinear(X1, Y1);
    const auto Bilinear = [BlendX, BlendY](const float V00, const float V10,
        const float V01, const float V11)
    {
        const float North = FMath::Lerp(V00, V10, BlendX);
        const float South = FMath::Lerp(V01, V11, BlendX);
        return FMath::Lerp(North, South, BlendY);
    };
    const FVector3f Sample(Bilinear(C00.R, C10.R, C01.R, C11.R),
        Bilinear(C00.G, C10.G, C01.G, C11.G),
        Bilinear(C00.B, C10.B, C01.B, C11.B));
    if (!FMath::IsFinite(Sample.X) || !FMath::IsFinite(Sample.Y)
        || !FMath::IsFinite(Sample.Z))
    {
        return false;
    }
    OutLinearColor = Sample;
    return true;
}

const TCHAR* SkiTerrainRuntime::TerrainMaterialAssetPathForMode(
    const ESkiTerrainViewMode Mode, const FName LightingPreset) noexcept
{
    if (Mode == ESkiTerrainViewMode::Photo)
    {
        return TEXT("/Game/P1Generated/M_Photo.M_Photo");
    }
    if (Mode != ESkiTerrainViewMode::Presentation)
    {
        return TEXT("/Game/P1Generated/M_Overlay.M_Overlay");
    }
    if (LightingPreset == TEXT("LowAngle"))
    {
        return TEXT("/Game/P1Generated/M_Terrain_LowAngle.M_Terrain_LowAngle");
    }
    if (LightingPreset == TEXT("Overcast"))
    {
        return TEXT("/Game/P1Generated/M_Terrain_Overcast.M_Terrain_Overcast");
    }
    return TEXT("/Game/P1Generated/M_Terrain_ClearMidday.M_Terrain_ClearMidday");
}

ASkiTerrainActor::ASkiTerrainActor()
{
    PrimaryActorTick.bCanEverTick = true;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("TerrainRoot"));
    RootComponent = SceneRoot;
    GuestDots = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GuestDots"));
    GuestDots->SetupAttachment(SceneRoot);
    GuestDots->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SunLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("P1Sun"));
    SunLight->SetupAttachment(SceneRoot);
    SunLight->SetMobility(EComponentMobility::Movable);
    SunLight->SetCastShadows(true);
    SunLight->SetAtmosphereSunLight(true);
    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("P1Sky"));
    SkyLight->SetupAttachment(SceneRoot);
    SkyLight->SetMobility(EComponentMobility::Movable);
    SkyAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("VerificationSky"));
    SkyAtmosphere->SetupAttachment(SceneRoot);
    VerificationPostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("VerificationExposure"));
    VerificationPostProcess->SetupAttachment(SceneRoot);
    VerificationPostProcess->bUnbound = true;
    VerificationPostProcess->Settings.bOverride_AutoExposureMethod = true;
    VerificationPostProcess->Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    VerificationPostProcess->Settings.bOverride_AutoExposureBias = true;
    // Keep verification captures deterministic without the black surround driving
    // eye adaptation. A modest positive compensation matches the outdoor light
    // intensities used by the three bounded lighting presets.
    VerificationPostProcess->Settings.AutoExposureBias = 8.0F;
    OverlayLines = CreateDefaultSubobject<ULineBatchComponent>(TEXT("TerrainOverlays"));
    OverlayLines->SetupAttachment(SceneRoot);
    OverlayLines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (Sphere.Succeeded()) GuestDots->SetStaticMesh(Sphere.Object);
}

void ASkiTerrainActor::Tick(const float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UpdateTerrainCoreAutoLod(DeltaSeconds);
    PumpTerrainCoreStreaming();
}

void ASkiTerrainActor::ReleaseTerrainCorePins()
{
    if (!CoreCache)
    {
        CoreRequestedKeys.Reset();
        return;
    }
    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        const uint64 EncodedKey = TerrainCoreRenderKey(Key);
        if (CoreRequestedKeys.Contains(EncodedKey)) CoreCache->ReleasePin(Key);
    }
    CoreRequestedKeys.Reset();
}

void ASkiTerrainActor::CompleteScratchMutation(const bool bSucceeded)
{
    bScratchMutationInFlight = false;
    TFunction<void(bool)> Completion = MoveTemp(ScratchMutationCompletion);
    if (Completion) Completion(bSucceeded);
}

void ASkiTerrainActor::CancelScratchMutation()
{
    if (!bScratchMutationInFlight && !ScratchMutationCompletion) return;
    ++ScratchMutationSerial;
    CompleteScratchMutation(false);
}

void ASkiTerrainActor::FailTerrainCorePresentation()
{
    // A failed selection is never left partially visible. Keep the desired keys so an
    // explicit same-selection retry can restart them, but invalidate every outstanding
    // mesh publication and make the component/key state empty and coherent.
    ++CorePresentationSerial;
    ReleaseTerrainCorePins();
    CoreMeshBuildsInFlight.Reset();
    CoreRequestedKeys.Reset();
    ClearTiles();
    bCoreBoundsHaveSamples = false;
    ValidLocalBounds = FBox(ForceInit);
    SteepestLocalBounds = FBox(ForceInit);
    if (!bCoreReadyNotified)
    {
        bCoreReadyNotified = true;
        if (CoreReadyHandler) CoreReadyHandler(false);
    }
    if (bScratchMutationInFlight) CompleteScratchMutation(false);
}

void ASkiTerrainActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ++CorePresentationSerial;
    bTerrainCoreAutoLod = false;
    ReleaseTerrainCorePins();
    CancelScratchMutation();
    CoreMeshBuildsInFlight.Reset();
    CoreFailedMeshKeys.Reset();
    CoreDesiredKeys.Reset();
    ClearTerrainCorePhotoTiles();
    CoreCache.Reset();
    CoreSession.Reset();
    CoreBaseRepository.reset();
    CoreLodController.Reset();
    bCoreBoundsHaveSamples = false;
    ValidLocalBounds = FBox(ForceInit);
    SteepestLocalBounds = FBox(ForceInit);
    ClearTiles();
    Super::EndPlay(EndPlayReason);
}

void ASkiTerrainActor::SetTerrainSession(TSharedPtr<SkiApplication::TerrainSession> InSession)
{
    ++CorePresentationSerial;
    ReleaseTerrainCorePins();
    CancelScratchMutation();
    CoreCache.Reset();
    CoreSession.Reset();
    CoreCacheGeneration = 0;
    ClearTerrainCorePhotoTiles();
    CoreBaseRepository.reset();
    CoreEdits = {};
    CoreLodController.Reset();
    CoreDesiredKeys.Reset();
    CoreMeshBuildsInFlight.Reset();
    CoreFailedMeshKeys.Reset();
    bTerrainCoreAutoLod = false;
    bCoreBoundsHaveSamples = false;
    ValidLocalBounds = FBox(ForceInit);
    SteepestLocalBounds = FBox(ForceInit);
    Session = std::move(InSession);
}

void ASkiTerrainActor::ClearTiles()
{
    for (UDynamicMeshComponent* Tile : Tiles)
    {
        if (Tile) Tile->DestroyComponent();
    }
    Tiles.Reset();
    TileKeys.Reset();
    CoreRenderedKeys.Reset();
}

void ASkiTerrainActor::ClearTerrainCorePhotoTiles(const uint64 Generation)
{
    PresentedPhotoTiles.Reset();
    PhotoTilesGeneration = Generation;
}

void ASkiTerrainActor::PruneTerrainCorePhotoTiles(const uint64 Generation,
    const TSet<uint64>& DesiredKeys)
{
    if (PhotoTilesGeneration != Generation)
    {
        ClearTerrainCorePhotoTiles(Generation);
        return;
    }
    for (auto It = PresentedPhotoTiles.CreateIterator(); It; ++It)
    {
        if (!DesiredKeys.Contains(It.Key())) It.RemoveCurrent();
    }
}

bool ASkiTerrainActor::PresentTerrainCore(
    TSharedPtr<SkiApplication::TerrainCoreSession> InSession,
    const uint8 Lod, const double TimeoutSeconds)
{
    if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0
        || !BeginTerrainCoreStreaming(MoveTemp(InSession), Lod))
    {
        return false;
    }
    return PresentTerrainCoreLod(Lod, TimeoutSeconds);
}

bool ASkiTerrainActor::BeginTerrainCoreStreaming(
    TSharedPtr<SkiApplication::TerrainCoreSession> InSession, const uint8 Lod)
{
    if (!InSession || Lod >= SkiDomain::TerrainCoreLodFactors.size()) return false;
    const SkiApplication::TerrainCoreSnapshot Snapshot = InSession->Snapshot();
    if (!Snapshot.CanonicalReady()) return false;
    TArray<SkiApplication::TerrainCoreTileKey> Desired;
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Snapshot.Metadata->Tiles)
        if (Tile.LodIndex == Lod) Desired.Add({Tile.LodIndex, Tile.TileX, Tile.TileY});
    constexpr int32 MaximumFullViewTiles = 256;
    if (Desired.IsEmpty() || Desired.Num() > MaximumFullViewTiles) return false;

    ++CorePresentationSerial;
    ReleaseTerrainCorePins();
    CancelScratchMutation();
    Session.Reset();
    CoreSession = MoveTemp(InSession);
    CoreBaseRepository = Snapshot.Repository;
    CoreEdits = {};
    CoreCache = MakeUnique<SkiTerrainRuntime::TerrainCoreTileCache>(
        Snapshot.Repository, Snapshot.Generation);
    CoreLodController = MakeUnique<SkiTerrainRuntime::TerrainCoreLodController>();
    CoreLodController->Reset(Lod);
    bTerrainCoreAutoLod = true;
    CoreAutoLodElapsedSeconds = 0.0F;
    CoreCacheGeneration = Snapshot.Generation;
    ClearTerrainCorePhotoTiles(Snapshot.Generation);
    CoreDesiredKeys = MoveTemp(Desired);
    CoreMeshBuildsInFlight.Reset();
    CoreFailedMeshKeys.Reset();
    CoreRequestedKeys.Reset();
    ClearTiles();
    PresentedRevision = Snapshot.Revisions.Canonical;
    PresentedLod = Lod;
    PresentedOriginHeightM = Snapshot.Metadata->LocalOrigin.HeightM;
    MinimumHeightM = TNumericLimits<double>::Max();
    MaximumHeightM = TNumericLimits<double>::Lowest();
    ValidLocalBounds = FBox(ForceInit);
    SteepestLocalBounds = FBox(ForceInit);
    bCoreBoundsHaveSamples = false;
    bCoreReadyNotified = false;
    return true;
}

bool ASkiTerrainActor::SetTerrainCorePhotoTile(const uint64 ExpectedGeneration,
    const SkiApplication::TerrainCoreTileKey& Key, TArray<FColor> BgraPixels)
{
    if (!IsInGameThread() || !CoreSession || !CoreCache
        || BgraPixels.Num() != static_cast<int32>(
            SkiTerrainRuntime::TerrainCorePhotoTileSide
                * SkiTerrainRuntime::TerrainCorePhotoTileSide))
    {
        return false;
    }
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady() || Snapshot.Generation != ExpectedGeneration
        || ExpectedGeneration != CoreCacheGeneration
        || PhotoTilesGeneration != ExpectedGeneration)
    {
        return false;
    }

    const bool bDesired = Algo::AnyOf(CoreDesiredKeys,
        [&Key](const SkiApplication::TerrainCoreTileKey& Desired)
        {
            return Desired.Lod == Key.Lod && Desired.X == Key.X && Desired.Y == Key.Y;
        });
    const bool bExists = Algo::AnyOf(Snapshot.Metadata->Tiles,
        [&Key](const SkiDomain::TerrainCoreTileDescriptor& Tile)
        {
            return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X && Tile.TileY == Key.Y;
        });
    if (!bDesired || !bExists) return false;

    SkiTerrainRuntime::FSkiTerrainPhotoTile PhotoTile;
    PhotoTile.Generation = ExpectedGeneration;
    PhotoTile.Key = Key;
    PhotoTile.BgraPixels = MoveTemp(BgraPixels);
    const uint64 EncodedKey = TerrainCoreRenderKey(Key);
    PresentedPhotoTiles.Add(EncodedKey, MoveTemp(PhotoTile));
    // The cache is pruned whenever the desired streamed selection changes, so this
    // map retains only current-view tiles (at most the existing 256-tile view cap).
    if (CurrentViewMode == ESkiTerrainViewMode::Photo
        && CoreRenderedKeys.Contains(EncodedKey))
    {
        RebuildPresentedTerrainCorePhotoTile(Key);
    }
    return true;
}

bool ASkiTerrainActor::GetTerrainCorePhotoRequestSnapshot(uint64& OutGeneration,
    TArray<SkiApplication::TerrainCoreTileKey>& OutDesiredKeys) const
{
    OutGeneration = 0;
    OutDesiredKeys.Reset();
    constexpr int32 MaximumPhotoRequestTiles = 256;
    if (!IsInGameThread() || CurrentViewMode != ESkiTerrainViewMode::Photo
        || !CoreSession || !CoreCache || CoreDesiredKeys.IsEmpty()
        || CoreDesiredKeys.Num() > MaximumPhotoRequestTiles)
    {
        return false;
    }

    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady() || Snapshot.Generation != CoreCacheGeneration
        || Snapshot.Generation != PhotoTilesGeneration)
    {
        return false;
    }

    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        const bool bExists = Algo::AnyOf(Snapshot.Metadata->Tiles,
            [&Key](const SkiDomain::TerrainCoreTileDescriptor& Tile)
            {
                return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X && Tile.TileY == Key.Y;
            });
        if (!bExists) return false;
    }

    OutGeneration = Snapshot.Generation;
    OutDesiredKeys = CoreDesiredKeys;
    return true;
}

bool ASkiTerrainActor::RebuildPresentedTerrainCorePhotoTile(
    const SkiApplication::TerrainCoreTileKey& Key)
{
    if (!IsInGameThread() || CurrentViewMode != ESkiTerrainViewMode::Photo
        || !CoreSession || !CoreCache)
    {
        return false;
    }
    const uint64 EncodedKey = TerrainCoreRenderKey(Key);
    if (!CoreRenderedKeys.Contains(EncodedKey)) return true;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady() || Snapshot.Generation != CoreCacheGeneration
        || Snapshot.Generation != PhotoTilesGeneration
        || Snapshot.Revisions.Canonical != PresentedRevision)
    {
        return false;
    }
    const std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> Payload =
        CoreCache->FindResident(Key);
    if (!Payload) return false;
    SkiDomain::TerrainTileMesh Mesh;
    if (!SkiTerrainRuntime::BuildTerrainCoreTileMesh(*Payload, *Snapshot.Metadata,
            Snapshot.Revisions.Canonical, true, 25.0, Mesh))
    {
        return false;
    }
    if (Mesh.Indices.empty()) return true;
    if (!CreateTileComponent(Mesh, PresentedOriginHeightM)) return false;

    // Install the new component before dropping its prior version. The exact tile
    // key preserves neighboring and mixed-LOD meshes during a streamed update.
    UDynamicMeshComponent* Replacement = Tiles.Last();
    for (int32 Index = Tiles.Num() - 2; Index >= 0; --Index)
    {
        const SkiDomain::TileKey& ExistingKey = TileKeys[Index];
        if (Tiles[Index] == Replacement || ExistingKey.X != Key.X
            || ExistingKey.Y != Key.Y || ExistingKey.Lod != Key.Lod)
        {
            continue;
        }
        if (Tiles[Index]) Tiles[Index]->DestroyComponent();
        Tiles.RemoveAt(Index);
        TileKeys.RemoveAt(Index);
    }
    SetLightingPreset(CurrentLightingPreset);
    return true;
}

void ASkiTerrainActor::SetTerrainCoreCover(
    std::shared_ptr<const std::vector<std::uint8_t>> Cover,
    std::shared_ptr<const std::vector<std::uint8_t>> PackedValidity,
    const SkiDomain::CoverEcologyGridTransform& Transform)
{
    const std::uint64_t Count = static_cast<std::uint64_t>(Transform.Width) * Transform.Height;
    SkiDomain::GeographicBounds ExpectedOuter;
    const bool bValidTransform = Transform.HorizontalCrs == "EPSG:4326"
        && Transform.PixelRegistration == "sample-center"
        && Transform.RowOrientation == "north-to-south"
        && SkiDomain::ComputeCoverEcologyOuterBounds(Transform.Width, Transform.Height,
            Transform.LongitudeStepDeg, Transform.LatitudeStepDeg,
            Transform.SampleCenterBounds, ExpectedOuter)
        && FMath::IsNearlyEqual(ExpectedOuter.WestDeg, Transform.OuterBounds.WestDeg, 1.0e-9)
        && FMath::IsNearlyEqual(ExpectedOuter.SouthDeg, Transform.OuterBounds.SouthDeg, 1.0e-9)
        && FMath::IsNearlyEqual(ExpectedOuter.EastDeg, Transform.OuterBounds.EastDeg, 1.0e-9)
        && FMath::IsNearlyEqual(ExpectedOuter.NorthDeg, Transform.OuterBounds.NorthDeg, 1.0e-9);
    if (!Cover || !PackedValidity || !bValidTransform
        || Count > SkiDomain::CoverEcologyMaxCells || Count != Cover->size()
        || PackedValidity->size() != (Count + 7U) / 8U)
    {
        PresentedCover.reset();
        PresentedCoverValidity.reset();
        PresentedCoverTransform = {};
        bPresentedCoverGeographic = false;
        PresentedCoverWidth = PresentedCoverHeight = 0;
        return;
    }
    PresentedCover = MoveTemp(Cover);
    PresentedCoverValidity = MoveTemp(PackedValidity);
    PresentedCoverTransform = Transform;
    bPresentedCoverGeographic = true;
    PresentedCoverWidth = Transform.Width;
    PresentedCoverHeight = Transform.Height;
}

bool ASkiTerrainActor::PresentTerrainCoreLod(const uint8 Lod,
    const double TimeoutSeconds)
{
    if (!CoreSession || !CoreCache || Lod >= SkiDomain::TerrainCoreLodFactors.size())
    {
        return false;
    }
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady()) return false;
    if (CoreCacheGeneration != Snapshot.Generation)
    {
        CoreCache->ResetGeneration(Snapshot.Generation, Snapshot.Repository);
        CoreCacheGeneration = Snapshot.Generation;
        ClearTerrainCorePhotoTiles(Snapshot.Generation);
    }

    TArray<SkiApplication::TerrainCoreTileKey> Requested;
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Snapshot.Metadata->Tiles)
    {
        if (Tile.LodIndex == Lod)
        {
            Requested.Add({Tile.LodIndex, Tile.TileX, Tile.TileY});
        }
    }
    // A full-view publication is deliberately bounded. The 2-10 km product envelope fits
    // comfortably at the overview LOD; a finer fixed diagnostic must be view-streamed rather
    // than constructing an unbounded whole-mountain mesh.
    constexpr int32 MaximumFullViewTiles = 256;
    if (Requested.IsEmpty() || Requested.Num() > MaximumFullViewTiles) return false;
    TSet<uint64> RequestedPhotoKeys;
    for (const SkiApplication::TerrainCoreTileKey& Key : Requested)
    {
        RequestedPhotoKeys.Add(TerrainCoreRenderKey(Key));
    }
    PruneTerrainCorePhotoTiles(Snapshot.Generation, RequestedPhotoKeys);
    // This blocking path is reserved for deterministic regression/support calls. Build
    // the complete replacement transactionally; product UI selections stream via Tick.
    ++CorePresentationSerial;
    ReleaseTerrainCorePins();
    CoreMeshBuildsInFlight.Reset();
    CoreFailedMeshKeys.Reset();
    CoreRequestedKeys.Reset();
    bCoreReadyNotified = false;
    bCoreBoundsHaveSamples = false;
    ValidLocalBounds = FBox(ForceInit);
    SteepestLocalBounds = FBox(ForceInit);

    TArray<SkiDomain::TerrainTileMesh> Meshes;
    Meshes.Reserve(Requested.Num());
    double Minimum = TNumericLimits<double>::Max();
    double Maximum = TNumericLimits<double>::Lowest();
    FBox Bounds(ForceInit);
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    for (const SkiApplication::TerrainCoreTileKey& Key : Requested)
    {
        std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> Payload =
            CoreCache->FindResident(Key);
        while (!Payload && FPlatformTime::Seconds() < Deadline)
        {
            if (CoreCache->TileStatus(Key) ==
                SkiTerrainRuntime::TerrainCoreRequestStatus::Failed)
            {
                if (!CoreCache->RetryTile(Key))
                {
                    CoreFailedMeshKeys.Add(TerrainCoreRenderKey(Key));
                    FailTerrainCorePresentation();
                    return false;
                }
            }
            else if (!CoreCache->RequestTile(Key))
            {
                CoreCache->PumpPublications();
            }
            if (!CoreCache->WaitForWorkers(FMath::Min(1.0,
                    FMath::Max(0.001, Deadline - FPlatformTime::Seconds()))))
            {
                continue;
            }
            CoreCache->PumpPublications();
            Payload = CoreCache->FindResident(Key);
        }
        if (!Payload)
        {
            CoreFailedMeshKeys.Add(TerrainCoreRenderKey(Key));
            FailTerrainCorePresentation();
            return false;
        }
        SkiDomain::TerrainTileMesh Mesh;
        if (!SkiTerrainRuntime::BuildTerrainCoreTileMesh(*Payload, *Snapshot.Metadata,
                Snapshot.Revisions.Canonical, true, 25.0, Mesh))
        {
            CoreFailedMeshKeys.Add(TerrainCoreRenderKey(Key));
            FailTerrainCorePresentation();
            return false;
        }
        for (uint32 Row = 0; Row < Payload->Descriptor.CoreHeight; ++Row)
        {
            for (uint32 Column = 0; Column < Payload->Descriptor.CoreWidth; ++Column)
            {
                const uint32 StoredWidth = Payload->Descriptor.CoreWidth
                    + Payload->Descriptor.HaloWest + Payload->Descriptor.HaloEast;
                const uint64 Index = static_cast<uint64>(Row + Payload->Descriptor.HaloNorth)
                        * StoredWidth
                    + Column + Payload->Descriptor.HaloWest;
                if (Index >= Payload->Heights.size() || Index >= Payload->Validity.size()
                    || Payload->Validity[Index] == 0
                    || !FMath::IsFinite(Payload->Heights[Index])) continue;
                const double Height = Payload->Heights[Index];
                Minimum = FMath::Min(Minimum, Height);
                Maximum = FMath::Max(Maximum, Height);
                const uint64 FineColumn = static_cast<uint64>(
                    Payload->Descriptor.StartColumn + Column) * Payload->Descriptor.LodFactor;
                const uint64 FineRow = static_cast<uint64>(
                    Payload->Descriptor.StartRow + Row) * Payload->Descriptor.LodFactor;
                const double East = Snapshot.Metadata->SampleCenterBounds.WestM
                    + FineColumn * Snapshot.Metadata->DeliveredEastSpacingM;
                const double North = Snapshot.Metadata->SampleCenterBounds.NorthM
                    - FineRow * Snapshot.Metadata->DeliveredNorthSpacingM;
                Bounds += FVector(North * 100.0, East * 100.0,
                    (Height - Snapshot.Metadata->LocalOrigin.HeightM) * 100.0);
            }
        }
        Meshes.Add(MoveTemp(Mesh));
    }
    if (!Bounds.IsValid || !FMath::IsFinite(Minimum) || !FMath::IsFinite(Maximum))
    {
        if (!Requested.IsEmpty())
            CoreFailedMeshKeys.Add(TerrainCoreRenderKey(Requested[0]));
        FailTerrainCorePresentation();
        return false;
    }

    CoreDesiredKeys = Requested;
    ClearTiles();
    PresentedOriginHeightM = Snapshot.Metadata->LocalOrigin.HeightM;
    MinimumHeightM = Minimum;
    MaximumHeightM = Maximum;
    ValidLocalBounds = Bounds;
    bCoreBoundsHaveSamples = true;
    SteepestLocalBounds = Bounds;
    for (const SkiDomain::TerrainTileMesh& Mesh : Meshes)
    {
        if (!Mesh.Indices.empty() && !CreateTileComponent(Mesh, PresentedOriginHeightM))
        {
            ClearTiles();
            CoreFailedMeshKeys.Add(TerrainCoreRenderKey(
                {Mesh.Key.Lod, Mesh.Key.X, Mesh.Key.Y}));
            FailTerrainCorePresentation();
            return false;
        }
        CoreRenderedKeys.Add(TerrainCoreRenderKey(
            {Mesh.Key.Lod, Mesh.Key.X, Mesh.Key.Y}));
    }
    PresentedRevision = Snapshot.Revisions.Canonical;
    PresentedLod = Lod;
    const SkiTerrainRuntime::TerrainCoreCacheStats Stats = CoreCache->Stats();
    CoreSession->ReportResidency(Snapshot.Generation,
        {Stats.ResidentBytes, Stats.ResidentTiles, Stats.PendingTiles});
    if (!CoreSession->AcknowledgeRender(Snapshot.Generation, Snapshot.Revisions.Canonical)
        || !CoreSession->AcknowledgeQuery(Snapshot.Generation, Snapshot.Revisions.Canonical))
    {
        if (!Requested.IsEmpty())
            CoreFailedMeshKeys.Add(TerrainCoreRenderKey(Requested[0]));
        FailTerrainCorePresentation();
        return false;
    }
    RebuildTerrainCoreOverviewDiagnostics();
    SetLightingPreset(CurrentLightingPreset);
    const bool bReady = IsTerrainCoreRevisionAligned();
    bCoreReadyNotified = bReady;
    if (CoreReadyHandler) CoreReadyHandler(bReady);
    return bReady;
}

bool ASkiTerrainActor::IsTerrainCoreRevisionAligned() const
{
    if (!CoreSession) return false;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    return Snapshot.RenderReady() && Snapshot.QueryReady()
        && Snapshot.Revisions.Canonical == PresentedRevision
        && bCoreBoundsHaveSamples && CoreFailedMeshKeys.IsEmpty()
        && CoreMeshBuildsInFlight.IsEmpty()
        && CoreRenderedKeys.Num() == CoreDesiredKeys.Num()
        && Algo::AllOf(CoreDesiredKeys, [this](const SkiApplication::TerrainCoreTileKey& Key)
        { return CoreRenderedKeys.Contains(TerrainCoreRenderKey(Key)); });
}

bool ASkiTerrainActor::ApplyTerrainCoreSelection(
    TArray<SkiApplication::TerrainCoreTileKey> Desired, const bool bClearVisibleTiles,
    const bool bForceRebuild)
{
    if (!CoreSession || !CoreCache || Desired.IsEmpty() || Desired.Num() > 256) return false;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady() || Snapshot.Generation != CoreCacheGeneration) return false;
    TSet<uint64> DesiredSet;
    for (const SkiApplication::TerrainCoreTileKey& Key : Desired)
    {
        const bool bExists = Algo::AnyOf(Snapshot.Metadata->Tiles,
            [&Key](const SkiDomain::TerrainCoreTileDescriptor& Tile)
            {
                return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X && Tile.TileY == Key.Y;
            });
        if (!bExists || DesiredSet.Contains(TerrainCoreRenderKey(Key))) return false;
        DesiredSet.Add(TerrainCoreRenderKey(Key));
    }
    PruneTerrainCorePhotoTiles(Snapshot.Generation, DesiredSet);
    const bool bUnchanged = Desired.Num() == CoreDesiredKeys.Num()
        && Algo::AllOf(Desired, [this](const SkiApplication::TerrainCoreTileKey& Key)
        {
            return Algo::AnyOf(CoreDesiredKeys,
                [&Key](const SkiApplication::TerrainCoreTileKey& Existing)
                { return Existing.Lod == Key.Lod && Existing.X == Key.X && Existing.Y == Key.Y; });
        });
    if (bUnchanged && !bForceRebuild && CoreFailedMeshKeys.IsEmpty()) return true;

    ++CorePresentationSerial;
    ReleaseTerrainCorePins();
    CoreDesiredKeys = MoveTemp(Desired);
    CoreMeshBuildsInFlight.Reset();
    CoreFailedMeshKeys.Reset();
    CoreRequestedKeys.Reset();
    bCoreReadyNotified = false;
    bCoreBoundsHaveSamples = false;
    ValidLocalBounds = FBox(ForceInit);
    SteepestLocalBounds = FBox(ForceInit);
    if (bClearVisibleTiles) ClearTiles();
    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        if (CoreCache->TileStatus(Key) !=
            SkiTerrainRuntime::TerrainCoreRequestStatus::Failed) continue;
        const uint64 EncodedKey = TerrainCoreRenderKey(Key);
        if (!CoreCache->RetryTile(Key, true))
        {
            CoreFailedMeshKeys.Add(EncodedKey);
            FailTerrainCorePresentation();
            return false;
        }
        CoreRequestedKeys.Add(EncodedKey);
    }
    return true;
}

void ASkiTerrainActor::UpdateTerrainCoreAutoLod(const float DeltaSeconds)
{
    if (!bTerrainCoreAutoLod || !CoreSession || !CoreLodController || !GetWorld()) return;
    CoreAutoLodElapsedSeconds += FMath::Max(0.0F, DeltaSeconds);
    if (CoreAutoLodElapsedSeconds < 0.25F) return;
    CoreAutoLodElapsedSeconds = 0.0F;

    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    APlayerCameraManager* Camera = Controller ? Controller->PlayerCameraManager : nullptr;
    if (!Snapshot.CanonicalReady() || !Camera || !Controller) return;
    int32 ViewWidth = 0;
    int32 ViewHeight = 0;
    Controller->GetViewportSize(ViewWidth, ViewHeight);
    if (ViewWidth <= 0 || ViewHeight <= 0) return;

    const uint32 FinestTilesX = FMath::DivideAndRoundUp(
        Snapshot.Metadata->Width - 1U, SkiDomain::TerrainCoreTileCells);
    const uint32 FinestTilesY = FMath::DivideAndRoundUp(
        Snapshot.Metadata->Height - 1U, SkiDomain::TerrainCoreTileCells);
    const uint64 Count = static_cast<uint64>(FinestTilesX) * FinestTilesY;
    if (Count == 0 || Count > 256) return;
    std::vector<double> SamplePixels(static_cast<size_t>(Count), 0.0);
    const double TanHalfFov = FMath::Tan(FMath::DegreesToRadians(
        FMath::Clamp(static_cast<double>(Camera->GetFOVAngle()), 20.0, 150.0) * 0.5));
    const double SampleWorldCm = FMath::Max(Snapshot.Metadata->DeliveredEastSpacingM,
        Snapshot.Metadata->DeliveredNorthSpacingM) * 100.0;
    for (uint32 Y = 0; Y < FinestTilesY; ++Y)
    {
        for (uint32 X = 0; X < FinestTilesX; ++X)
        {
            const uint32 CenterColumn = FMath::Min(Snapshot.Metadata->Width - 1U,
                X * SkiDomain::TerrainCoreTileCells + SkiDomain::TerrainCoreTileCells / 2U);
            const uint32 CenterRow = FMath::Min(Snapshot.Metadata->Height - 1U,
                Y * SkiDomain::TerrainCoreTileCells + SkiDomain::TerrainCoreTileCells / 2U);
            const double East = Snapshot.Metadata->SampleCenterBounds.WestM
                + CenterColumn * Snapshot.Metadata->DeliveredEastSpacingM;
            const double North = Snapshot.Metadata->SampleCenterBounds.NorthM
                - CenterRow * Snapshot.Metadata->DeliveredNorthSpacingM;
            const FVector Center(North * 100.0, East * 100.0, 0.0);
            const double DistanceCm = FMath::Max(100.0,
                FVector::Distance(Camera->GetCameraLocation(), Center));
            SamplePixels[static_cast<size_t>(Y) * FinestTilesX + X] =
                SampleWorldCm * ViewHeight / (2.0 * TanHalfFov * DistanceCm);
        }
    }
    std::vector<SkiApplication::TerrainCoreTileKey> Selected;
    if (!CoreLodController->SelectTileKeys(FinestTilesX, FinestTilesY,
            SamplePixels, Selected)) return;
    TArray<SkiApplication::TerrainCoreTileKey> Desired;
    Desired.Reserve(static_cast<int32>(Selected.size()));
    for (const SkiApplication::TerrainCoreTileKey& Key : Selected) Desired.Add(Key);
    ApplyTerrainCoreSelection(MoveTemp(Desired), true);
}

bool ASkiTerrainActor::PublishTerrainCoreMesh(const uint64 ExpectedSerial,
    const uint64 ExpectedGeneration, const SkiDomain::Revision ExpectedRevision,
    const uint64 EncodedKey, const SkiApplication::TerrainCoreTilePayload& Payload,
    const SkiDomain::TerrainTileMesh& Mesh, const bool bBuilt)
{
    const uint64* InFlightSerial = CoreMeshBuildsInFlight.Find(EncodedKey);
    if (InFlightSerial && *InFlightSerial == ExpectedSerial)
    {
        CoreMeshBuildsInFlight.Remove(EncodedKey);
    }
    if (ExpectedSerial != CorePresentationSerial)
    {
        ++RejectedCoreMeshBuilds;
        return false;
    }
    const SkiApplication::TerrainCoreSnapshot Current = CoreSession
        ? CoreSession->Snapshot() : SkiApplication::TerrainCoreSnapshot{};
    if (!bBuilt || Current.Generation != ExpectedGeneration
        || Current.Revisions.Canonical != ExpectedRevision
        || (!Mesh.Indices.empty() && !CreateTileComponent(Mesh, PresentedOriginHeightM)))
    {
        ++RejectedCoreMeshBuilds;
        CoreFailedMeshKeys.Add(EncodedKey);
        FailTerrainCorePresentation();
        return false;
    }
    if (!bCoreBoundsHaveSamples)
    {
        ValidLocalBounds = FBox(ForceInit);
        MinimumHeightM = TNumericLimits<double>::Max();
        MaximumHeightM = TNumericLimits<double>::Lowest();
    }
    bool bPublishedFiniteSample = false;
    const uint32 StoredWidth = Payload.Descriptor.CoreWidth
        + Payload.Descriptor.HaloWest + Payload.Descriptor.HaloEast;
    for (uint32 Row = 0; Row < Payload.Descriptor.CoreHeight; ++Row)
    {
        for (uint32 Column = 0; Column < Payload.Descriptor.CoreWidth; ++Column)
        {
            const uint64 Index = static_cast<uint64>(Row + Payload.Descriptor.HaloNorth)
                    * StoredWidth + Column + Payload.Descriptor.HaloWest;
            if (Index >= Payload.Heights.size() || Index >= Payload.Validity.size()
                || Payload.Validity[Index] == 0 || !FMath::IsFinite(Payload.Heights[Index])) continue;
            const double HeightM = Payload.Heights[Index];
            const uint64 FineColumn = static_cast<uint64>(Payload.Descriptor.StartColumn + Column)
                * Payload.Descriptor.LodFactor;
            const uint64 FineRow = static_cast<uint64>(Payload.Descriptor.StartRow + Row)
                * Payload.Descriptor.LodFactor;
            const double East = Current.Metadata->SampleCenterBounds.WestM
                + FineColumn * Current.Metadata->DeliveredEastSpacingM;
            const double North = Current.Metadata->SampleCenterBounds.NorthM
                - FineRow * Current.Metadata->DeliveredNorthSpacingM;
            MinimumHeightM = FMath::Min(MinimumHeightM, HeightM);
            MaximumHeightM = FMath::Max(MaximumHeightM, HeightM);
            ValidLocalBounds += FVector(North * 100.0, East * 100.0,
                (HeightM - PresentedOriginHeightM) * 100.0);
            bPublishedFiniteSample = true;
        }
    }
    if (bPublishedFiniteSample) bCoreBoundsHaveSamples = true;
    CoreRenderedKeys.Add(EncodedKey);
    if (CoreRenderedKeys.Num() != CoreDesiredKeys.Num()) return true;
    if (!bCoreBoundsHaveSamples)
    {
        FailTerrainCorePresentation();
        return false;
    }

    const SkiTerrainRuntime::TerrainCoreCacheStats Stats = CoreCache->Stats();
    CoreSession->ReportResidency(ExpectedGeneration,
        {Stats.ResidentBytes, Stats.ResidentTiles, Stats.PendingTiles});
    CoreSession->AcknowledgeRender(ExpectedGeneration, ExpectedRevision);
    CoreSession->AcknowledgeQuery(ExpectedGeneration, ExpectedRevision);
    RebuildTerrainCoreOverviewDiagnostics();
    SetLightingPreset(CurrentLightingPreset);
    const bool bReady = IsTerrainCoreRevisionAligned();
    if (!bCoreReadyNotified)
    {
        bCoreReadyNotified = true;
        if (CoreReadyHandler) CoreReadyHandler(bReady);
    }
    if (bScratchMutationInFlight) CompleteScratchMutation(bReady);
    return bReady;
}

bool ASkiTerrainActor::RunStaleTerrainCoreMeshPublicationProbe()
{
    if (!CoreSession || !CoreCache || CoreDesiredKeys.IsEmpty()) return false;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.Metadata) return false;
    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        const std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> Payload =
            CoreCache->FindResident(Key);
        if (!Payload) continue;
        SkiDomain::TerrainTileMesh Mesh;
        if (!SkiTerrainRuntime::BuildTerrainCoreTileMesh(*Payload, *Snapshot.Metadata,
                Snapshot.Revisions.Canonical, true, 25.0, Mesh)
            || Mesh.Indices.empty())
        {
            continue;
        }
        const uint64 Before = RejectedCoreMeshBuilds;
        return !PublishTerrainCoreMesh(CorePresentationSerial - 1U, Snapshot.Generation,
            Snapshot.Revisions.Canonical, TerrainCoreRenderKey(Key),
            *Payload, Mesh, true) && RejectedCoreMeshBuilds == Before + 1U;
    }
    return false;
}

void ASkiTerrainActor::PumpTerrainCoreStreaming()
{
    if (!CoreSession || !CoreCache || CoreDesiredKeys.IsEmpty()) return;
    if (!CoreFailedMeshKeys.IsEmpty()) return;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady() || Snapshot.Generation != CoreCacheGeneration
        || Snapshot.Revisions.Canonical != PresentedRevision)
    {
        return;
    }
    CoreCache->PumpPublications();
    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        const uint64 EncodedKey = TerrainCoreRenderKey(Key);
        if (CoreCache->TileStatus(Key) == SkiTerrainRuntime::TerrainCoreRequestStatus::Failed)
        {
            CoreRequestedKeys.Remove(EncodedKey);
            CoreFailedMeshKeys.Add(EncodedKey);
            FailTerrainCorePresentation();
            return;
        }
        if (CoreRenderedKeys.Contains(EncodedKey)
            || CoreRequestedKeys.Contains(EncodedKey)) continue;
        if (CoreCache->RequestTile(Key, true)) CoreRequestedKeys.Add(EncodedKey);
    }

    constexpr int32 MaximumMeshBuildJobs = 2;
    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        if (CoreMeshBuildsInFlight.Num() >= MaximumMeshBuildJobs) break;
        const uint64 EncodedKey = TerrainCoreRenderKey(Key);
        if (CoreRenderedKeys.Contains(EncodedKey)
            || CoreMeshBuildsInFlight.Contains(EncodedKey)) continue;
        std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> Payload =
            CoreCache->FindResident(Key);
        if (!Payload) continue;
        const uint64 ExpectedSerial = CorePresentationSerial;
        CoreMeshBuildsInFlight.Add(EncodedKey, ExpectedSerial);
        const uint64 ExpectedGeneration = Snapshot.Generation;
        const SkiDomain::Revision ExpectedRevision = Snapshot.Revisions.Canonical;
        const std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata = Snapshot.Metadata;
        const TWeakObjectPtr<ASkiTerrainActor> WeakThis(this);
        Async(EAsyncExecution::ThreadPool,
            [WeakThis, Payload = MoveTemp(Payload), Metadata, ExpectedSerial,
                ExpectedGeneration, ExpectedRevision, EncodedKey]() mutable
            {
                SkiDomain::TerrainTileMesh Mesh;
                const bool Built = Metadata && SkiTerrainRuntime::BuildTerrainCoreTileMesh(
                    *Payload, *Metadata, ExpectedRevision, true, 25.0, Mesh);
                AsyncTask(ENamedThreads::GameThread,
                    [WeakThis, Payload = MoveTemp(Payload), Mesh = MoveTemp(Mesh), Built,
                        ExpectedSerial, ExpectedGeneration, ExpectedRevision, EncodedKey]() mutable
                    {
                        if (!WeakThis.IsValid()) return;
                        WeakThis->PublishTerrainCoreMesh(ExpectedSerial, ExpectedGeneration,
                            ExpectedRevision, EncodedKey, *Payload, Mesh, Built);
                    });
            });
    }
}

bool ASkiTerrainActor::BuildTileComponent(const SkiDomain::Heightfield& Field,
    const SkiDomain::TileKey& Key, const double OriginHeightM)
{
    SkiDomain::TerrainTileMesh SourceMesh;
    if (!SkiDomain::BuildTerrainTile(Field, Key, true, 25.0, SourceMesh)) return false;
    return CreateTileComponent(SourceMesh, OriginHeightM);
}

bool ASkiTerrainActor::CreateTileComponent(const SkiDomain::TerrainTileMesh& SourceMesh,
    const double OriginHeightM)
{
    UE::Geometry::FDynamicMesh3 Mesh;
    Mesh.EnableVertexNormals(FVector3f::UpVector);
    Mesh.EnableVertexUVs(FVector2f::ZeroVector);
    Mesh.EnableVertexColors(FVector3f(0.42F, 0.40F, 0.34F));
    TArray<FVector4f> RenderColors;
    RenderColors.Reserve(static_cast<int32>(SourceMesh.Vertices.size()));
    SkiDomain::LocalFrame CoverFrame;
    bool bHaveCoverFrame = false;
    const SkiTerrainRuntime::FSkiTerrainPhotoTile* PhotoTile = nullptr;
    double PhotoMinU = TNumericLimits<double>::Max();
    double PhotoMaxU = TNumericLimits<double>::Lowest();
    double PhotoMinV = TNumericLimits<double>::Max();
    double PhotoMaxV = TNumericLimits<double>::Lowest();
    bool bHavePhotoUvBounds = false;
    if (CurrentViewMode == ESkiTerrainViewMode::Photo && CoreSession
        && PhotoTilesGeneration == CoreCacheGeneration)
    {
        const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
        if (Snapshot.CanonicalReady() && Snapshot.Generation == PhotoTilesGeneration)
        {
            const SkiApplication::TerrainCoreTileKey Key{
                SourceMesh.Key.Lod, SourceMesh.Key.X, SourceMesh.Key.Y};
            PhotoTile = PresentedPhotoTiles.Find(TerrainCoreRenderKey(Key));
            if (PhotoTile)
            {
                for (const SkiDomain::TerrainVertex& Vertex : SourceMesh.Vertices)
                {
                    if (!FMath::IsFinite(Vertex.U) || !FMath::IsFinite(Vertex.V)) continue;
                    PhotoMinU = FMath::Min(PhotoMinU, static_cast<double>(Vertex.U));
                    PhotoMaxU = FMath::Max(PhotoMaxU, static_cast<double>(Vertex.U));
                    PhotoMinV = FMath::Min(PhotoMinV, static_cast<double>(Vertex.V));
                    PhotoMaxV = FMath::Max(PhotoMaxV, static_cast<double>(Vertex.V));
                    bHavePhotoUvBounds = true;
                }
                bHavePhotoUvBounds = bHavePhotoUvBounds
                    && PhotoMaxU > PhotoMinU && PhotoMaxV > PhotoMinV;
            }
        }
    }
    if (bPresentedCoverGeographic && CoreSession)
    {
        const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
        if (Snapshot.Metadata)
        {
            const SkiDomain::GeodeticPoint& Origin = Snapshot.Metadata->LocalOrigin;
            bHaveCoverFrame = SkiDomain::TryMakeLocalFrame(
                {Origin.LatitudeDeg, Origin.LongitudeDeg, 0.0}, CoverFrame);
        }
    }
    for (const SkiDomain::TerrainVertex& Vertex : SourceMesh.Vertices)
    {
        const int32 VertexId = Mesh.AppendVertex(FVector3d(
            static_cast<double>(Vertex.NorthM) * 100.0,
            static_cast<double>(Vertex.EastM) * 100.0,
            (static_cast<double>(Vertex.UpM) - OriginHeightM) * 100.0));
        Mesh.SetVertexNormal(VertexId, FVector3f(Vertex.NormalNorth, Vertex.NormalEast, Vertex.NormalUp));
        Mesh.SetVertexUV(VertexId, FVector2f(Vertex.U, Vertex.V));
        FVector3f Color(0.42F, 0.40F, 0.34F);
        if (CurrentViewMode == ESkiTerrainViewMode::Elevation)
        {
            const float Alpha = static_cast<float>(FMath::Clamp((Vertex.UpM - MinimumHeightM)
                / FMath::Max(1.0, MaximumHeightM - MinimumHeightM), 0.0, 1.0));
            Color = FMath::Lerp(FVector3f(0.05F, 0.16F, 0.42F), FVector3f(0.96F, 0.88F, 0.44F), Alpha);
        }
        else if (CurrentViewMode == ESkiTerrainViewMode::Slope)
        {
            const double Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp<double>(Vertex.NormalUp, 0.0, 1.0)));
            Color = Degrees < 15.0 ? FVector3f(0.15F, 0.55F, 0.18F)
                : Degrees < 30.0 ? FVector3f(0.92F, 0.78F, 0.12F)
                : Degrees < 45.0 ? FVector3f(0.95F, 0.35F, 0.08F)
                : FVector3f(0.65F, 0.05F, 0.18F);
        }
        else if (CurrentViewMode == ESkiTerrainViewMode::TileLod)
        {
            const uint32 Hue = (SourceMesh.Key.X * 37U + SourceMesh.Key.Y * 67U + SourceMesh.Key.Lod * 101U) % 255U;
            const FLinearColor Hsv = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hue), 210, 235);
            Color = FVector3f(Hsv.R, Hsv.G, Hsv.B);
        }
        else if (CurrentViewMode == ESkiTerrainViewMode::Photo)
        {
            if (PhotoTile && bHavePhotoUvBounds)
            {
                const double TileLocalU = (static_cast<double>(Vertex.U) - PhotoMinU)
                    / (PhotoMaxU - PhotoMinU);
                const double TileLocalV = (static_cast<double>(Vertex.V) - PhotoMinV)
                    / (PhotoMaxV - PhotoMinV);
                const SkiApplication::TerrainCoreTileKey Key{
                    SourceMesh.Key.Lod, SourceMesh.Key.X, SourceMesh.Key.Y};
                SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(PhotoTile,
                    PhotoTilesGeneration, Key, TileLocalU, TileLocalV, Color);
            }
        }
        else if (PresentedCover && bPresentedCoverGeographic && PresentedCoverValidity
            && bHaveCoverFrame)
        {
            SkiDomain::GeodeticPoint Geographic;
            uint8 CoverClass = 0;
            if (SkiDomain::TrySeaLevelGeodeticFromEnu(CoverFrame,
                    Vertex.EastM, Vertex.NorthM, Geographic)
                && SkiDomain::SampleCoverEcologyClass(PresentedCoverTransform,
                    *PresentedCover, *PresentedCoverValidity,
                    Geographic.LatitudeDeg, Geographic.LongitudeDeg, CoverClass))
            {
                Color = CoverColor(CoverClass);
            }
        }
        else if (PresentedCover && !bPresentedCoverGeographic
            && PresentedCoverWidth > 0 && PresentedCoverHeight > 0)
        {
            const uint32 Column = FMath::Min(PresentedCoverWidth - 1,
                static_cast<uint32>(FMath::RoundToInt(Vertex.U * (PresentedCoverWidth - 1))));
            const uint32 Row = FMath::Min(PresentedCoverHeight - 1,
                static_cast<uint32>(FMath::RoundToInt(Vertex.V * (PresentedCoverHeight - 1))));
            Color = CoverColor((*PresentedCover)[static_cast<size_t>(Row) * PresentedCoverWidth + Column]);
        }
        Mesh.SetVertexColor(VertexId, Color);
        RenderColors.Add(FVector4f(Color.X, Color.Y, Color.Z, 1.0F));
    }
    for (int32 Index = 0; Index + 2 < static_cast<int32>(SourceMesh.Indices.size()); Index += 3)
    {
        const int32 Triangle = Mesh.AppendTriangle(static_cast<int32>(SourceMesh.Indices[Index]),
            static_cast<int32>(SourceMesh.Indices[Index + 1]),
            static_cast<int32>(SourceMesh.Indices[Index + 2]));
        if (Triangle < 0) return false;
    }
    // Dynamic Mesh rendering consumes the primary color overlay. The legacy
    // per-vertex color channel above is retained for mesh-level inspection,
    // while this one-element-per-vertex overlay drives the packaged diagnostic
    // views without altering canonical geometry.
    Mesh.EnableAttributes();
    Mesh.Attributes()->EnablePrimaryColors();
    UE::Geometry::FDynamicMeshColorOverlay* Colors = Mesh.Attributes()->PrimaryColors();
    TArray<int32> ColorElements;
    ColorElements.SetNum(Mesh.MaxVertexID());
    for (int32 VertexId : Mesh.VertexIndicesItr())
    {
        ColorElements[VertexId] = Colors->AppendElement(RenderColors[VertexId]);
    }
    for (int32 TriangleId : Mesh.TriangleIndicesItr())
    {
        const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriangleId);
        Colors->SetTriangle(TriangleId, UE::Geometry::FIndex3i(
            ColorElements[Triangle.A], ColorElements[Triangle.B], ColorElements[Triangle.C]));
    }
    UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(this);
    Component->SetupAttachment(SceneRoot);
    Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Component->SetCastShadow(true);
    AddInstanceComponent(Component);
    Component->RegisterComponent();
    Component->SetMesh(MoveTemp(Mesh));
    Tiles.Add(Component);
    TileKeys.Add(SourceMesh.Key);
    return true;
}

bool ASkiTerrainActor::Present(const SkiApplication::TerrainSnapshot& Snapshot, const uint8 Lod)
{
    if (!Snapshot.Heightfield || !SkiDomain::IsValidHeightfield(*Snapshot.Heightfield) || Lod > 2)
    {
        return false;
    }
    if (CoreSession)
    {
        ++CorePresentationSerial;
        ReleaseTerrainCorePins();
        CancelScratchMutation();
    }
    CoreCache.Reset();
    CoreSession.Reset();
    CoreCacheGeneration = 0;
    ClearTerrainCorePhotoTiles();
    CoreDesiredKeys.Reset();
    CoreMeshBuildsInFlight.Reset();
    CoreFailedMeshKeys.Reset();
    bCoreBoundsHaveSamples = false;
    ClearTiles();
    PresentedOriginHeightM = Snapshot.Manifest ? Snapshot.Manifest->LocalOrigin.HeightM : 0.0;
    PresentedCover = Snapshot.Cover;
    PresentedCoverValidity.reset();
    PresentedCoverTransform = {};
    bPresentedCoverGeographic = false;
    PresentedCoverWidth = Snapshot.CoverWidth;
    PresentedCoverHeight = Snapshot.CoverHeight;
    ValidLocalBounds = FBox(ForceInit);
    MinimumHeightM = TNumericLimits<double>::Max();
    MaximumHeightM = TNumericLimits<double>::Lowest();
    for (uint32 Row = 0; Row < Snapshot.Heightfield->Height; ++Row)
    {
        for (uint32 Column = 0; Column < Snapshot.Heightfield->Width; ++Column)
        {
            const float Height = Snapshot.Heightfield->Samples[static_cast<size_t>(Row) * Snapshot.Heightfield->Width + Column];
            if (!FMath::IsFinite(Height) || static_cast<double>(Height) == Snapshot.Heightfield->NoDataValue) continue;
            MinimumHeightM = FMath::Min(MinimumHeightM, static_cast<double>(Height));
            MaximumHeightM = FMath::Max(MaximumHeightM, static_cast<double>(Height));
            ValidLocalBounds += FVector(Snapshot.Heightfield->SampleNorthM(Row) * 100.0,
                Snapshot.Heightfield->EastM(Column) * 100.0,
                (static_cast<double>(Height) - PresentedOriginHeightM) * 100.0);
        }
    }
    if (!ValidLocalBounds.IsValid) return false;
    TArray<double> QuadrantSlopes[4];
    for (uint32 Row = 0; Row + 1 < Snapshot.Heightfield->Height; ++Row)
        for (uint32 Column = 0; Column + 1 < Snapshot.Heightfield->Width; ++Column)
        {
            const float H = Snapshot.Heightfield->Samples[static_cast<size_t>(Row) * Snapshot.Heightfield->Width + Column];
            const float East = Snapshot.Heightfield->Samples[static_cast<size_t>(Row) * Snapshot.Heightfield->Width + Column + 1];
            const float South = Snapshot.Heightfield->Samples[static_cast<size_t>(Row + 1) * Snapshot.Heightfield->Width + Column];
            if (!FMath::IsFinite(H) || !FMath::IsFinite(East) || !FMath::IsFinite(South)) continue;
            const double Gradient = FMath::Sqrt(FMath::Square((East - H) / Snapshot.Heightfield->EastSpacingM)
                + FMath::Square((South - H) / Snapshot.Heightfield->NorthSpacingM));
            const int32 Quadrant = (Column >= Snapshot.Heightfield->Width / 2 ? 1 : 0)
                + (Row >= Snapshot.Heightfield->Height / 2 ? 2 : 0);
            QuadrantSlopes[Quadrant].Add(FMath::RadiansToDegrees(FMath::Atan(Gradient)));
        }
    int32 Steepest = 0;
    double SteepestP95 = -1.0;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        QuadrantSlopes[Index].Sort();
        if (QuadrantSlopes[Index].IsEmpty()) continue;
        const double P95 = QuadrantSlopes[Index][FMath::Min(QuadrantSlopes[Index].Num() - 1,
            FMath::FloorToInt(QuadrantSlopes[Index].Num() * 0.95))];
        if (P95 > SteepestP95) { SteepestP95 = P95; Steepest = Index; }
    }
    const FVector Center = ValidLocalBounds.GetCenter();
    const FVector Extent = ValidLocalBounds.GetExtent();
    const bool SouthHalf = (Steepest & 2) != 0;
    const bool EastHalf = (Steepest & 1) != 0;
    const FVector QuadrantCenter(Center.X + (SouthHalf ? -0.5 : 0.5) * Extent.X,
        Center.Y + (EastHalf ? 0.5 : -0.5) * Extent.Y, Center.Z);
    SteepestLocalBounds = FBox(QuadrantCenter - FVector(Extent.X * 0.55, Extent.Y * 0.55, Extent.Z),
        QuadrantCenter + FVector(Extent.X * 0.55, Extent.Y * 0.55, Extent.Z));
    const uint32 TilesX = (Snapshot.Heightfield->Width - 2) / SkiDomain::TerrainTileCells + 1;
    const uint32 TilesY = (Snapshot.Heightfield->Height - 2) / SkiDomain::TerrainTileCells + 1;
    for (uint32 Y = 0; Y < TilesY; ++Y)
    {
        for (uint32 X = 0; X < TilesX; ++X)
        {
            if (!BuildTileComponent(*Snapshot.Heightfield, {X, Y, Lod}, PresentedOriginHeightM))
            {
                ClearTiles();
                return false;
            }
        }
    }
    PresentedRevision = Snapshot.Heightfield->CurrentRevision;
    PresentedLod = Lod;
    RebuildDots(*Snapshot.Heightfield, PresentedOriginHeightM);
    RebuildContourOverlay(*Snapshot.Heightfield, PresentedOriginHeightM);
    SetLightingPreset(CurrentLightingPreset);
    return !Session || Session->AcknowledgeRender(PresentedRevision);
}

bool ASkiTerrainActor::SetLod(const uint8 Lod)
{
    if (CoreSession)
    {
        if (!CoreCache || !CoreLodController
            || Lod >= SkiDomain::TerrainCoreLodFactors.size()) return false;
        const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
        if (!Snapshot.CanonicalReady()) return false;
        TArray<SkiApplication::TerrainCoreTileKey> Desired;
        for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Snapshot.Metadata->Tiles)
        {
            if (Tile.LodIndex == Lod)
                Desired.Add({Tile.LodIndex, Tile.TileX, Tile.TileY});
        }
        constexpr int32 MaximumFullViewTiles = 256;
        if (Desired.IsEmpty() || Desired.Num() > MaximumFullViewTiles) return false;
        bTerrainCoreAutoLod = false;
        CoreLodController->Reset(Lod);
        PresentedLod = Lod;
        return ApplyTerrainCoreSelection(MoveTemp(Desired), true);
    }
    return Session && Lod <= 2 && Present(Session->Snapshot(), Lod);
}

bool ASkiTerrainActor::SetLodAuto()
{
    if (!CoreSession || !CoreLodController) return false;
    CoreLodController->Reset(PresentedLod);
    bTerrainCoreAutoLod = true;
    CoreAutoLodElapsedSeconds = 0.25F;
    return true;
}

void ASkiTerrainActor::SetViewMode(const ESkiTerrainViewMode Mode)
{
    if (CurrentViewMode == Mode) return;
    CurrentViewMode = Mode;
    if (CoreSession && !CoreDesiredKeys.IsEmpty())
    {
        TArray<SkiApplication::TerrainCoreTileKey> Desired = CoreDesiredKeys;
        ApplyTerrainCoreSelection(MoveTemp(Desired), true, true);
    }
    else if (Session) Present(Session->Snapshot(), PresentedLod);
}

void ASkiTerrainActor::SetVerticalExaggeration(const float Scale)
{
    SetActorScale3D(FVector(1.0F, 1.0F, FMath::Clamp(Scale, 1.0F, 4.0F)));
}

bool ASkiTerrainActor::GetValidWorldBounds(FBox& OutBounds) const
{
    if ((CoreSession && !bCoreBoundsHaveSamples) || !ValidLocalBounds.IsValid) return false;
    OutBounds = ValidLocalBounds.TransformBy(GetActorTransform());
    return true;
}

bool ASkiTerrainActor::GetSteepestQuadrantWorldBounds(FBox& OutBounds) const
{
    if ((CoreSession && !bCoreBoundsHaveSamples) || !SteepestLocalBounds.IsValid) return false;
    OutBounds = SteepestLocalBounds.TransformBy(GetActorTransform());
    return true;
}

void ASkiTerrainActor::ShowTopologyPatch(const SkiDomain::RayHit& Hit)
{
    if (CoreSession && CoreCache)
    {
        const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
        if (!Snapshot.Metadata || !Snapshot.CanonicalReady()) return;
        OverlayLines->Flush();
        const auto& Metadata = *Snapshot.Metadata;
        const uint32 MinRow = Hit.Row > 4 ? Hit.Row - 4 : 0;
        const uint32 MinColumn = Hit.Column > 4 ? Hit.Column - 4 : 0;
        const uint32 MaxRow = FMath::Min(Metadata.Height - 1, Hit.Row + 5);
        const uint32 MaxColumn = FMath::Min(Metadata.Width - 1, Hit.Column + 5);
        TArray<FBatchedLine> Lines;
        auto Point = [&](const uint32 Row, const uint32 Column, FVector& Out)
        {
            double Height = 0.0;
            bool Valid = false;
            const SkiTerrainRuntime::TerrainCoreQueryStatus Status = CoreCache->QueryCanonicalCell(
                FMath::Min(Column, Metadata.Width - 2), FMath::Min(Row, Metadata.Height - 2),
                Column == Metadata.Width - 1 ? 1.0 : 0.0,
                Row == Metadata.Height - 1 ? 1.0 : 0.0, Height, Valid);
            if (Status != SkiTerrainRuntime::TerrainCoreQueryStatus::Ready || !Valid) return false;
            const double East = Metadata.SampleCenterBounds.WestM
                + Column * Metadata.DeliveredEastSpacingM;
            const double North = Metadata.SampleCenterBounds.NorthM
                - Row * Metadata.DeliveredNorthSpacingM;
            Out = FVector(North * 100.0, East * 100.0,
                (Height - PresentedOriginHeightM) * 100.0 + 30.0);
            return true;
        };
        // Prime all cells in the bounded patch; the cache retains only the finest
        // tiles needed by the canonical query path.
        for (uint32 Row = MinRow; Row <= MaxRow; ++Row)
            for (uint32 Column = MinColumn; Column <= MaxColumn; ++Column)
            {
                double Height = 0.0; bool Valid = false;
                CoreCache->QueryCanonicalCell(FMath::Min(Column, Metadata.Width - 2),
                    FMath::Min(Row, Metadata.Height - 2), 0.0, 0.0, Height, Valid);
            }
        CoreCache->WaitForWorkers(2.0);
        CoreCache->PumpPublications();
        for (uint32 Row = MinRow; Row < MaxRow; ++Row)
            for (uint32 Column = MinColumn; Column < MaxColumn; ++Column)
            {
                FVector A, B, C, D;
                if (!Point(Row, Column, A) || !Point(Row, Column + 1, B)
                    || !Point(Row + 1, Column, C) || !Point(Row + 1, Column + 1, D)) continue;
                Lines.Emplace(A, B, FLinearColor(0, 1, 1, 1), 0, 2.0F, 1);
                Lines.Emplace(A, C, FLinearColor(0, 1, 1, 1), 0, 2.0F, 1);
                Lines.Emplace(A, D, FLinearColor::Yellow, 0, 2.5F, 1);
            }
        OverlayLines->DrawLines(Lines);
        return;
    }
    if (!Session) return;
    const SkiApplication::TerrainSnapshot Snapshot = Session->Snapshot();
    if (!Snapshot.Heightfield) return;
    RebuildContourOverlay(*Snapshot.Heightfield, PresentedOriginHeightM);
    const SkiDomain::Heightfield& Field = *Snapshot.Heightfield;
    const uint32 MinRow = Hit.Row > 4 ? Hit.Row - 4 : 0;
    const uint32 MinColumn = Hit.Column > 4 ? Hit.Column - 4 : 0;
    const uint32 MaxRow = FMath::Min(Field.Height - 1, Hit.Row + 5);
    const uint32 MaxColumn = FMath::Min(Field.Width - 1, Hit.Column + 5);
    TArray<FBatchedLine> Lines;
    auto Point = [&](uint32 Row, uint32 Column)
    {
        const float H = Field.Samples[static_cast<size_t>(Row) * Field.Width + Column];
        return FVector(Field.SampleNorthM(Row) * 100.0, Field.EastM(Column) * 100.0,
            (static_cast<double>(H) - PresentedOriginHeightM) * 100.0 + 30.0);
    };
    for (uint32 Row = MinRow; Row < MaxRow; ++Row)
        for (uint32 Column = MinColumn; Column < MaxColumn; ++Column)
        {
            Lines.Emplace(Point(Row, Column), Point(Row, Column + 1), FLinearColor(0, 1, 1, 1), 0, 2.0F, 1);
            Lines.Emplace(Point(Row, Column), Point(Row + 1, Column), FLinearColor(0, 1, 1, 1), 0, 2.0F, 1);
            Lines.Emplace(Point(Row, Column), Point(Row + 1, Column + 1), FLinearColor::Yellow, 0, 2.5F, 1);
        }
    OverlayLines->DrawLines(Lines);
}

void ASkiTerrainActor::RebuildContourOverlay(const SkiDomain::Heightfield& Field,
    const double OriginHeightM)
{
    OverlayLines->Flush();
    const uint32 Step = FMath::Max(1U, FMath::Max(Field.Width, Field.Height) / 512U);
    TArray<FBatchedLine> Lines;
    Lines.Reserve(12000);
    constexpr int32 MaxSegments = 20000;
    const auto Point = [&Field, OriginHeightM](const uint32 RowA, const uint32 ColumnA,
        const float HeightA, const uint32 RowB, const uint32 ColumnB, const float HeightB,
        const double Level)
    {
        const double Denominator = static_cast<double>(HeightB) - HeightA;
        const double Alpha = FMath::IsNearlyZero(Denominator) ? 0.5
            : FMath::Clamp((Level - HeightA) / Denominator, 0.0, 1.0);
        const double East = FMath::Lerp(Field.EastM(ColumnA), Field.EastM(ColumnB), Alpha);
        const double North = FMath::Lerp(Field.SampleNorthM(RowA), Field.SampleNorthM(RowB), Alpha);
        return FVector(North * 100.0, East * 100.0, (Level - OriginHeightM) * 100.0 + 12.0);
    };
    for (uint32 Row = 0; Row + Step < Field.Height && Lines.Num() < MaxSegments; Row += Step)
    {
        for (uint32 Column = 0; Column + Step < Field.Width && Lines.Num() < MaxSegments; Column += Step)
        {
            const float H00 = Field.Samples[static_cast<size_t>(Row) * Field.Width + Column];
            const float H10 = Field.Samples[static_cast<size_t>(Row) * Field.Width + Column + Step];
            const float H11 = Field.Samples[static_cast<size_t>(Row + Step) * Field.Width + Column + Step];
            const float H01 = Field.Samples[static_cast<size_t>(Row + Step) * Field.Width + Column];
            if (!FMath::IsFinite(H00) || !FMath::IsFinite(H10) || !FMath::IsFinite(H11)
                || !FMath::IsFinite(H01)) continue;
            const double Minimum = FMath::Min(FMath::Min(H00, H10), FMath::Min(H11, H01));
            const double Maximum = FMath::Max(FMath::Max(H00, H10), FMath::Max(H11, H01));
            for (double Level = FMath::CeilToDouble(Minimum / 50.0) * 50.0;
                Level <= Maximum && Lines.Num() < MaxSegments; Level += 50.0)
            {
                TArray<FVector, TInlineAllocator<4>> Crossings;
                const auto Cross = [Level](const float A, const float B)
                { return (A < Level && B >= Level) || (B < Level && A >= Level); };
                if (Cross(H00, H10)) Crossings.Add(Point(Row, Column, H00, Row, Column + Step, H10, Level));
                if (Cross(H10, H11)) Crossings.Add(Point(Row, Column + Step, H10, Row + Step, Column + Step, H11, Level));
                if (Cross(H11, H01)) Crossings.Add(Point(Row + Step, Column + Step, H11, Row + Step, Column, H01, Level));
                if (Cross(H01, H00)) Crossings.Add(Point(Row + Step, Column, H01, Row, Column, H00, Level));
                for (int32 Index = 0; Index + 1 < Crossings.Num(); Index += 2)
                {
                    Lines.Emplace(Crossings[Index], Crossings[Index + 1],
                        FLinearColor(1.0F, 0.54F, 0.10F, 0.72F), 0.0F, 1.25F, 0);
                }
            }
        }
    }
    OverlayLines->DrawLines(Lines);
}

void ASkiTerrainActor::RebuildDots(const SkiDomain::Heightfield& Field, const double OriginHeightM)
{
    GuestDots->ClearInstances();
    if (!GuestDots->GetStaticMesh()) return;
    FRandomStream Random(0x51A1);
    TArray<FVector> ValidLocations;
    ValidLocations.Reserve(FMath::Min<uint64>(8192, static_cast<uint64>(Field.Width) * Field.Height));
    for (uint32 Row = 0; Row < Field.Height && ValidLocations.Num() < 8192; ++Row)
    {
        for (uint32 Column = 0; Column < Field.Width && ValidLocations.Num() < 8192; ++Column)
        {
            const float Height = Field.Samples[static_cast<size_t>(Row) * Field.Width + Column];
            if (!FMath::IsFinite(Height) || static_cast<double>(Height) == Field.NoDataValue) continue;
            ValidLocations.Add(FVector(Field.SampleNorthM(Row) * 100.0, Field.EastM(Column) * 100.0,
                (static_cast<double>(Height) - OriginHeightM) * 100.0 + 100.0));
        }
    }
    for (int32 Index = 0; Index < 3000 && !ValidLocations.IsEmpty(); ++Index)
        GuestDots->AddInstance(FTransform(FQuat::Identity,
            ValidLocations[Random.RandRange(0, ValidLocations.Num() - 1)], FVector(0.04)));
}

void ASkiTerrainActor::RebuildTerrainCoreOverviewDiagnostics()
{
    if (!CoreSession || !CoreCache) return;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.Metadata) return;
    const SkiDomain::TerrainCoreManifest& Metadata = *Snapshot.Metadata;
    OverlayLines->Flush();
    GuestDots->ClearInstances();
    TArray<FVector> ValidLocations;
    ValidLocations.Reserve(8192);
    TArray<double> QuadrantSlopes[4];
    TArray<FBatchedLine> Lines;
    Lines.Reserve(20000);
    for (const SkiApplication::TerrainCoreTileKey& Key : CoreDesiredKeys)
    {
        const std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> Payload =
            CoreCache->FindResident(Key);
        if (!Payload) continue;
        const uint32 StoredWidth = Payload->Descriptor.CoreWidth
            + Payload->Descriptor.HaloWest + Payload->Descriptor.HaloEast;
        const auto Sample = [&](const uint32 Row, const uint32 Column, float& Out)
        {
            const uint64 Index = static_cast<uint64>(Row + Payload->Descriptor.HaloNorth)
                * StoredWidth + Column + Payload->Descriptor.HaloWest;
            if (Index >= Payload->Heights.size() || Index >= Payload->Validity.size()
                || Payload->Validity[Index] == 0 || !FMath::IsFinite(Payload->Heights[Index])) return false;
            Out = Payload->Heights[Index];
            return true;
        };
        for (uint32 Row = 0; Row < Payload->Descriptor.CoreHeight; ++Row)
        {
            for (uint32 Column = 0; Column < Payload->Descriptor.CoreWidth; ++Column)
            {
                float Height = 0.0F;
                if (!Sample(Row, Column, Height)) continue;
                const uint64 FineColumn = static_cast<uint64>(Payload->Descriptor.StartColumn + Column)
                    * Payload->Descriptor.LodFactor;
                const uint64 FineRow = static_cast<uint64>(Payload->Descriptor.StartRow + Row)
                    * Payload->Descriptor.LodFactor;
                const double East = Metadata.SampleCenterBounds.WestM
                    + FineColumn * Metadata.DeliveredEastSpacingM;
                const double North = Metadata.SampleCenterBounds.NorthM
                    - FineRow * Metadata.DeliveredNorthSpacingM;
                if (ValidLocations.Num() < 8192) ValidLocations.Add(FVector(North * 100.0, East * 100.0,
                    (Height - PresentedOriginHeightM) * 100.0 + 100.0));
                if (Row + 1 >= Payload->Descriptor.CoreHeight
                    || Column + 1 >= Payload->Descriptor.CoreWidth) continue;
                float EastHeight = 0.0F, SouthHeight = 0.0F;
                if (!Sample(Row, Column + 1, EastHeight) || !Sample(Row + 1, Column, SouthHeight)) continue;
                const double EastRun = Metadata.DeliveredEastSpacingM * Payload->Descriptor.LodFactor;
                const double NorthRun = Metadata.DeliveredNorthSpacingM * Payload->Descriptor.LodFactor;
                const double Gradient = FMath::Sqrt(FMath::Square((EastHeight - Height) / EastRun)
                    + FMath::Square((SouthHeight - Height) / NorthRun));
                const int32 Quadrant = (FineColumn >= Metadata.Width / 2 ? 1 : 0)
                    + (FineRow >= Metadata.Height / 2 ? 2 : 0);
                QuadrantSlopes[Quadrant].Add(FMath::RadiansToDegrees(FMath::Atan(Gradient)));
                if (Lines.Num() < 20000)
                {
                    const FVector A(North * 100.0, East * 100.0,
                        (Height - PresentedOriginHeightM) * 100.0 + 12.0);
                    const FVector B(North * 100.0, (East + EastRun) * 100.0,
                        (EastHeight - PresentedOriginHeightM) * 100.0 + 12.0);
                    if (FMath::FloorToInt(Height / 50.0) != FMath::FloorToInt(EastHeight / 50.0))
                        Lines.Emplace(A, B, FLinearColor(1.0F, 0.54F, 0.10F, 0.72F), 0.0F, 1.25F, 0);
                }
            }
        }
    }
    FRandomStream Random(0x51A1);
    if (GuestDots->GetStaticMesh())
        for (int32 Index = 0; Index < 3000 && !ValidLocations.IsEmpty(); ++Index)
            GuestDots->AddInstance(FTransform(FQuat::Identity,
                ValidLocations[Random.RandRange(0, ValidLocations.Num() - 1)], FVector(0.04)));
    OverlayLines->DrawLines(Lines);
    int32 Steepest = 0;
    double P95Maximum = -1.0;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        QuadrantSlopes[Index].Sort();
        if (QuadrantSlopes[Index].IsEmpty()) continue;
        const double P95 = QuadrantSlopes[Index][FMath::Min(QuadrantSlopes[Index].Num() - 1,
            FMath::FloorToInt(QuadrantSlopes[Index].Num() * 0.95))];
        if (P95 > P95Maximum) { P95Maximum = P95; Steepest = Index; }
    }
    const FVector Center = ValidLocalBounds.GetCenter();
    const FVector Extent = ValidLocalBounds.GetExtent();
    const FVector QuadrantCenter(Center.X + ((Steepest & 2) ? -0.5 : 0.5) * Extent.X,
        Center.Y + ((Steepest & 1) ? 0.5 : -0.5) * Extent.Y, Center.Z);
    SteepestLocalBounds = FBox(QuadrantCenter - FVector(Extent.X * 0.55, Extent.Y * 0.55, Extent.Z),
        QuadrantCenter + FVector(Extent.X * 0.55, Extent.Y * 0.55, Extent.Z));
}

int32 ASkiTerrainActor::GetSyntheticGuestMarkerCount() const noexcept
{
    return GuestDots ? GuestDots->GetInstanceCount() : 0;
}

int32 ASkiTerrainActor::GetOverlaySegmentCount() const noexcept
{
    return OverlayLines ? OverlayLines->BatchedLines.Num() : 0;
}

bool ASkiTerrainActor::ApplyScratchMutation(const FVector2D& CenterEastNorthM,
    const double RadiusM, const double DeltaM)
{
    if (bScratchMutationInFlight) return false;
    if (CoreSession)
    {
        const SkiApplication::TerrainCoreSnapshot Before = CoreSession->Snapshot();
        if (!Before.CanonicalReady() || !CoreCache) return false;
        SkiDomain::TerrainEditSet Candidate;
        if (!BuildCircularTerrainCoreEdit(*Before.Metadata, Before.Revisions.Canonical,
                CenterEastNorthM, RadiusM, DeltaM, Candidate)) return false;
        std::string EditError;
        auto EditedRepository = SkiApplication::TerrainCoreEditedRepository::Create(
            Before.Repository, Candidate, Before.Revisions.Canonical, EditError);
        if (!EditedRepository) return false;
        SkiApplication::TerrainCoreSnapshot Published;
        if (!CoreSession->PublishEdit(Before.Generation, Before.Revisions.Canonical,
                EditedRepository, Candidate.EditRevision, Published))
        {
            return false;
        }
        std::shared_ptr<const SkiDomain::TerrainEditSet> CumulativeEdits;
        EditedRepository->FlattenEditOverlay(CoreBaseRepository, CumulativeEdits);
        CoreEdits = *CumulativeEdits;
        CoreCache->ResetGeneration(Published.Generation, Published.Repository);
        CoreCacheGeneration = Published.Generation;
        ClearTerrainCorePhotoTiles(Published.Generation);
        PresentedRevision = Published.Revisions.Canonical;
        return PresentTerrainCoreLod(PresentedLod, 30.0);
    }
    if (!Session) return false;
    const SkiApplication::TerrainSnapshot Before = Session->Snapshot();
    if (!Before.Heightfield) return false;
    SkiDomain::MutationBounds Bounds;
    if (!Session->ApplyScratchMutation(Before.Readiness.Canonical, CenterEastNorthM.X,
            CenterEastNorthM.Y, RadiusM, DeltaM, Bounds))
    {
        return false;
    }
    return Present(Session->Snapshot());
}

void ASkiTerrainActor::ApplyScratchMutationAsync(const FVector2D& CenterEastNorthM,
    const double RadiusM, const double DeltaM, TFunction<void(bool)> Completion)
{
    if (bScratchMutationInFlight)
    {
        if (Completion) Completion(false);
        return;
    }
    if (CoreSession)
    {
        const SkiApplication::TerrainCoreSnapshot Before = CoreSession->Snapshot();
        if (!Before.CanonicalReady() || !CoreCache)
        {
            if (Completion) Completion(false);
            return;
        }
        bScratchMutationInFlight = true;
        ScratchMutationCompletion = MoveTemp(Completion);
        const uint64 MutationSerial = ++ScratchMutationSerial;
        const TSharedPtr<SkiApplication::TerrainCoreSession> CapturedSession = CoreSession;
        const TWeakObjectPtr<ASkiTerrainActor> WeakThis(this);
        Async(EAsyncExecution::ThreadPool,
            [WeakThis, CapturedSession, Before, CenterEastNorthM, RadiusM, DeltaM,
                MutationSerial]() mutable
            {
                SkiDomain::TerrainEditSet Candidate;
                std::string EditError;
                const bool bBuilt = BuildCircularTerrainCoreEdit(*Before.Metadata,
                    Before.Revisions.Canonical, CenterEastNorthM, RadiusM, DeltaM, Candidate);
                std::shared_ptr<SkiApplication::TerrainCoreEditedRepository> EditedRepository;
                if (bBuilt)
                {
                    EditedRepository = SkiApplication::TerrainCoreEditedRepository::Create(
                        Before.Repository, Candidate, Before.Revisions.Canonical, EditError);
                }
                AsyncTask(ENamedThreads::GameThread,
                    [WeakThis, CapturedSession, ExpectedGeneration = Before.Generation,
                        ExpectedRevision = Before.Revisions.Canonical,
                        Candidate = MoveTemp(Candidate), EditedRepository = MoveTemp(EditedRepository),
                        MutationSerial]() mutable
                    {
                        if (!WeakThis.IsValid()) return;
                        if (!WeakThis->bScratchMutationInFlight
                            || WeakThis->ScratchMutationSerial != MutationSerial)
                        {
                            return;
                        }
                        if (WeakThis->CoreSession != CapturedSession || !WeakThis->CoreCache
                            || !EditedRepository)
                        {
                            WeakThis->CompleteScratchMutation(false);
                            return;
                        }
                        SkiApplication::TerrainCoreSnapshot Published;
                        if (!CapturedSession->PublishEdit(ExpectedGeneration, ExpectedRevision,
                                EditedRepository, Candidate.EditRevision, Published))
                        {
                            WeakThis->CompleteScratchMutation(false);
                            return;
                        }
                        std::shared_ptr<const SkiDomain::TerrainEditSet> CumulativeEdits;
                        EditedRepository->FlattenEditOverlay(
                            WeakThis->CoreBaseRepository, CumulativeEdits);
                        WeakThis->CoreEdits = *CumulativeEdits;
                        WeakThis->ReleaseTerrainCorePins();
                        WeakThis->CoreCache->ResetGeneration(Published.Generation,
                            Published.Repository);
                        WeakThis->CoreCacheGeneration = Published.Generation;
                        WeakThis->ClearTerrainCorePhotoTiles(Published.Generation);
                        WeakThis->PresentedRevision = Published.Revisions.Canonical;
                        ++WeakThis->CorePresentationSerial;
                        WeakThis->CoreMeshBuildsInFlight.Reset();
                        WeakThis->CoreFailedMeshKeys.Reset();
                        WeakThis->CoreRequestedKeys.Reset();
                        WeakThis->ClearTiles();
                        WeakThis->bCoreBoundsHaveSamples = false;
                        WeakThis->bCoreReadyNotified = false;
                        WeakThis->ValidLocalBounds = FBox(ForceInit);
                        WeakThis->SteepestLocalBounds = FBox(ForceInit);
                    });
            });
        return;
    }
    if (!Session)
    {
        if (Completion) Completion(false);
        return;
    }
    const SkiApplication::TerrainSnapshot Before = Session->Snapshot();
    if (!Before.Heightfield)
    {
        if (Completion) Completion(false);
        return;
    }
    bScratchMutationInFlight = true;
    ScratchMutationCompletion = MoveTemp(Completion);
    const uint64 MutationSerial = ++ScratchMutationSerial;
    const TSharedPtr<SkiApplication::TerrainSession> CapturedSession = Session;
    const uint8 Lod = PresentedLod;
    const double OriginHeightM = PresentedOriginHeightM;
    const TWeakObjectPtr<ASkiTerrainActor> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, CapturedSession, Expected = Before.Readiness.Canonical,
        CenterEastNorthM, RadiusM, DeltaM, Lod, OriginHeightM, MutationSerial]() mutable
    {
        SkiDomain::MutationBounds Bounds;
        if (!CapturedSession->ApplyScratchMutation(Expected, CenterEastNorthM.X, CenterEastNorthM.Y,
                RadiusM, DeltaM, Bounds))
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, MutationSerial]()
            {
                if (WeakThis.IsValid() && WeakThis->bScratchMutationInFlight
                    && WeakThis->ScratchMutationSerial == MutationSerial)
                {
                    WeakThis->CompleteScratchMutation(false);
                }
            });
            return;
        }
        const SkiApplication::TerrainSnapshot Edited = CapturedSession->Snapshot();
        CapturedSession->AcknowledgeQuery(Edited.Readiness.Canonical);
        const uint32 MinColumn = Bounds.MinColumn == 0 ? 0 : Bounds.MinColumn - 1;
        const uint32 MinRow = Bounds.MinRow == 0 ? 0 : Bounds.MinRow - 1;
        const uint32 MaxColumn = FMath::Min(Edited.Heightfield->Width - 1, Bounds.MaxColumn + 1);
        const uint32 MaxRow = FMath::Min(Edited.Heightfield->Height - 1, Bounds.MaxRow + 1);
        const uint32 MinTileX = MinColumn / SkiDomain::TerrainTileCells;
        const uint32 MaxTileX = FMath::Min((Edited.Heightfield->Width - 2) / SkiDomain::TerrainTileCells,
            MaxColumn / SkiDomain::TerrainTileCells);
        const uint32 MinTileY = MinRow / SkiDomain::TerrainTileCells;
        const uint32 MaxTileY = FMath::Min((Edited.Heightfield->Height - 2) / SkiDomain::TerrainTileCells,
            MaxRow / SkiDomain::TerrainTileCells);
        TArray<SkiDomain::TerrainTileMesh> Meshes;
        for (uint32 Y = MinTileY; Y <= MaxTileY; ++Y)
        {
            for (uint32 X = MinTileX; X <= MaxTileX; ++X)
            {
                SkiDomain::TerrainTileMesh Mesh;
                if (!SkiDomain::BuildTerrainTile(*Edited.Heightfield, {X, Y, Lod}, true, 25.0, Mesh))
                {
                    AsyncTask(ENamedThreads::GameThread, [WeakThis, MutationSerial]()
                    {
                        if (WeakThis.IsValid() && WeakThis->bScratchMutationInFlight
                            && WeakThis->ScratchMutationSerial == MutationSerial)
                        {
                            WeakThis->CompleteScratchMutation(false);
                        }
                    });
                    return;
                }
                Meshes.Add(std::move(Mesh));
            }
        }
        AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSession, Revision = Edited.Readiness.Canonical,
            Meshes = std::move(Meshes), OriginHeightM, MutationSerial]() mutable
        {
            if (!WeakThis.IsValid()) return;
            if (!WeakThis->bScratchMutationInFlight
                || WeakThis->ScratchMutationSerial != MutationSerial)
            {
                return;
            }
            if (WeakThis->Session != CapturedSession
                || CapturedSession->Snapshot().Readiness.Canonical != Revision)
            {
                WeakThis->CompleteScratchMutation(false);
                return;
            }
            for (const SkiDomain::TerrainTileMesh& Mesh : Meshes)
            {
                for (int32 Index = WeakThis->TileKeys.Num() - 1; Index >= 0; --Index)
                {
                    const SkiDomain::TileKey& Key = WeakThis->TileKeys[Index];
                    if (Key.X == Mesh.Key.X && Key.Y == Mesh.Key.Y && Key.Lod == Mesh.Key.Lod)
                    {
                        if (WeakThis->Tiles[Index]) WeakThis->Tiles[Index]->DestroyComponent();
                        WeakThis->Tiles.RemoveAt(Index);
                        WeakThis->TileKeys.RemoveAt(Index);
                    }
                }
                if (!WeakThis->CreateTileComponent(Mesh, OriginHeightM))
                {
                    WeakThis->CompleteScratchMutation(false);
                    return;
                }
            }
            WeakThis->PresentedRevision = Revision;
            const SkiApplication::TerrainSnapshot Current = CapturedSession->Snapshot();
            if (Current.Heightfield) WeakThis->RebuildContourOverlay(*Current.Heightfield, OriginHeightM);
            WeakThis->SetLightingPreset(WeakThis->CurrentLightingPreset);
            const bool Ready = CapturedSession->AcknowledgeRender(Revision)
                && CapturedSession->Snapshot().Readiness.IsReady();
            WeakThis->CompleteScratchMutation(Ready);
        });
    });
}

SkiDomain::RayHit ASkiTerrainActor::QueryCanonical(const FVector& WorldOriginCm,
    const FVector& WorldDirection) const
{
    if (CoreSession) return QueryTerrainCore(WorldOriginCm, WorldDirection);
    if (!Session) return {};
    const SkiApplication::TerrainSnapshot Snapshot = Session->Snapshot();
    if (!Snapshot.Heightfield) return {};
    const FVector LocalOrigin = GetActorTransform().InverseTransformPosition(WorldOriginCm);
    const FVector LocalDirection = GetActorTransform().InverseTransformVector(WorldDirection);
    const SkiDomain::Ray Query{{LocalOrigin.Y / 100.0, LocalOrigin.X / 100.0,
        LocalOrigin.Z / 100.0 + PresentedOriginHeightM},
        {LocalDirection.Y, LocalDirection.X, LocalDirection.Z}};
    return SkiDomain::QueryHeightfield(*Snapshot.Heightfield, Query);
}

SkiDomain::RayHit ASkiTerrainActor::QueryTerrainCore(const FVector& WorldOriginCm,
    const FVector& WorldDirection) const
{
    SkiDomain::RayHit Miss;
    if (!CoreSession || !CoreCache) return Miss;
    const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
    if (!Snapshot.CanonicalReady() || Snapshot.Generation != CoreCacheGeneration) return Miss;
    const FVector LocalOriginCm = GetActorTransform().InverseTransformPosition(WorldOriginCm);
    const FVector LocalDirection = GetActorTransform().InverseTransformVector(WorldDirection);
    const SkiDomain::EnuVector Origin{LocalOriginCm.Y / 100.0,
        LocalOriginCm.X / 100.0, LocalOriginCm.Z / 100.0 + PresentedOriginHeightM};
    const double Length = LocalDirection.Length();
    if (!FMath::IsFinite(Length) || Length <= UE_SMALL_NUMBER) return Miss;
    const SkiDomain::EnuVector Direction{LocalDirection.Y / Length,
        LocalDirection.X / Length, LocalDirection.Z / Length};
    const SkiDomain::TerrainCoreManifest& Metadata = *Snapshot.Metadata;

    double Enter = 0.0;
    double Exit = TNumericLimits<double>::Max();
    const auto Clip = [&Enter, &Exit](const double Coordinate, const double Delta,
        const double Minimum, const double Maximum)
    {
        if (FMath::Abs(Delta) <= UE_SMALL_NUMBER)
            return Coordinate >= Minimum && Coordinate <= Maximum;
        double First = (Minimum - Coordinate) / Delta;
        double Second = (Maximum - Coordinate) / Delta;
        if (First > Second) Swap(First, Second);
        Enter = FMath::Max(Enter, First);
        Exit = FMath::Min(Exit, Second);
        return Exit >= Enter;
    };
    if (!Clip(Origin.East, Direction.East, Metadata.SampleCenterBounds.WestM,
            Metadata.SampleCenterBounds.EastM)
        || !Clip(Origin.North, Direction.North, Metadata.SampleCenterBounds.SouthM,
            Metadata.SampleCenterBounds.NorthM)
        || Exit < 0.0)
    {
        return Miss;
    }
    Enter = FMath::Max(0.0, Enter);
    constexpr double MaximumRayDistanceM = 100000.0;
    Exit = FMath::Min(Exit, MaximumRayDistanceM);

    const auto HeightAt = [this, &Metadata](const double East, const double North,
        double& OutHeight, uint32& OutColumn, uint32& OutRow)
    {
        const double ColumnValue = (East - Metadata.SampleCenterBounds.WestM)
            / Metadata.DeliveredEastSpacingM;
        const double RowValue = (Metadata.SampleCenterBounds.NorthM - North)
            / Metadata.DeliveredNorthSpacingM;
        if (!FMath::IsFinite(ColumnValue) || !FMath::IsFinite(RowValue)
            || ColumnValue < 0.0 || RowValue < 0.0
            || ColumnValue > Metadata.Width - 1.0 || RowValue > Metadata.Height - 1.0)
        {
            return false;
        }
        OutColumn = FMath::Min(Metadata.Width - 2U,
            static_cast<uint32>(std::floor(ColumnValue)));
        OutRow = FMath::Min(Metadata.Height - 2U,
            static_cast<uint32>(std::floor(RowValue)));
        const double EastFraction = FMath::Clamp(ColumnValue - OutColumn, 0.0, 1.0);
        const double SouthFraction = FMath::Clamp(RowValue - OutRow, 0.0, 1.0);
        bool Valid = false;
        SkiTerrainRuntime::TerrainCoreQueryStatus Status = CoreCache->QueryCanonicalCell(
            OutColumn, OutRow, EastFraction, SouthFraction, OutHeight, Valid);
        if (Status == SkiTerrainRuntime::TerrainCoreQueryStatus::Pending)
        {
            if (!CoreCache->WaitForWorkers(2.0)) return false;
            CoreCache->PumpPublications();
            Status = CoreCache->QueryCanonicalCell(OutColumn, OutRow,
                EastFraction, SouthFraction, OutHeight, Valid);
        }
        return Status == SkiTerrainRuntime::TerrainCoreQueryStatus::Ready && Valid;
    };
    const double HorizontalSpeed = FMath::Sqrt(Direction.East * Direction.East
        + Direction.North * Direction.North);
    if (HorizontalSpeed <= UE_SMALL_NUMBER)
    {
        double Height = 0.0;
        uint32 Column = 0;
        uint32 Row = 0;
        if (!HeightAt(Origin.East, Origin.North, Height, Column, Row)
            || FMath::Abs(Direction.Up) <= UE_SMALL_NUMBER) return Miss;
        const double Distance = (Height - Origin.Up) / Direction.Up;
        if (Distance < 0.0 || Distance > MaximumRayDistanceM) return Miss;
        return {true, Distance, {Origin.East, Origin.North, Height}, Row, Column,
            Snapshot.Revisions.Canonical};
    }

    const double HorizontalStep = FMath::Max(0.25,
        FMath::Min(Metadata.DeliveredEastSpacingM, Metadata.DeliveredNorthSpacingM) * 0.5);
    const double Step = HorizontalStep / HorizontalSpeed;
    const uint64 MaximumSteps = 200000;
    double PreviousDistance = Enter;
    double PreviousDifference = 0.0;
    bool HavePrevious = false;
    for (uint64 Index = 0; Index < MaximumSteps && PreviousDistance <= Exit; ++Index)
    {
        const double Distance = FMath::Min(Exit, Enter + static_cast<double>(Index) * Step);
        const double East = Origin.East + Direction.East * Distance;
        const double North = Origin.North + Direction.North * Distance;
        const double Up = Origin.Up + Direction.Up * Distance;
        double Height = 0.0;
        uint32 Column = 0;
        uint32 Row = 0;
        if (HeightAt(East, North, Height, Column, Row))
        {
            const double Difference = Up - Height;
            if (HavePrevious && PreviousDifference >= 0.0 && Difference <= 0.0)
            {
                double Low = PreviousDistance;
                double High = Distance;
                uint32 HitColumn = Column;
                uint32 HitRow = Row;
                for (int32 Iteration = 0; Iteration < 16; ++Iteration)
                {
                    const double Middle = (Low + High) * 0.5;
                    const double MidEast = Origin.East + Direction.East * Middle;
                    const double MidNorth = Origin.North + Direction.North * Middle;
                    const double MidUp = Origin.Up + Direction.Up * Middle;
                    double MidHeight = 0.0;
                    uint32 MidColumn = 0;
                    uint32 MidRow = 0;
                    if (!HeightAt(MidEast, MidNorth, MidHeight, MidColumn, MidRow)) break;
                    if (MidUp - MidHeight > 0.0) Low = Middle;
                    else
                    {
                        High = Middle;
                        HitColumn = MidColumn;
                        HitRow = MidRow;
                    }
                }
                const double HitDistance = High;
                const double HitEast = Origin.East + Direction.East * HitDistance;
                const double HitNorth = Origin.North + Direction.North * HitDistance;
                double HitHeight = 0.0;
                if (!HeightAt(HitEast, HitNorth, HitHeight, HitColumn, HitRow)) return Miss;
                return {true, HitDistance, {HitEast, HitNorth, HitHeight}, HitRow,
                    HitColumn, Snapshot.Revisions.Canonical};
            }
            HavePrevious = true;
            PreviousDifference = Difference;
        }
        else
        {
            HavePrevious = false;
        }
        PreviousDistance = Distance;
        if (Distance >= Exit) break;
    }
    return Miss;
}

FString ASkiTerrainActor::DescribeProbe(const SkiDomain::RayHit& Hit) const
{
    if (!Hit.Hit) return TEXT("No canonical terrain hit.");
    double SlopeDegrees = std::numeric_limits<double>::quiet_NaN();
    uint8 CoverClass = 0;
    bool bHaveCoverClass = false;
    if (CoreSession && CoreCache)
    {
        const SkiApplication::TerrainCoreSnapshot Snapshot = CoreSession->Snapshot();
        if (Snapshot.Metadata && Hit.Column + 1 < Snapshot.Metadata->Width
            && Hit.Row + 1 < Snapshot.Metadata->Height)
        {
            double H00 = 0.0, H10 = 0.0, H01 = 0.0;
            bool V00 = false, V10 = false, V01 = false;
            const auto Query = [&](const double EastFraction, const double SouthFraction,
                double& Height, bool& Valid)
            {
                return CoreCache->QueryCanonicalCell(Hit.Column, Hit.Row, EastFraction,
                    SouthFraction, Height, Valid)
                    == SkiTerrainRuntime::TerrainCoreQueryStatus::Ready && Valid;
            };
            if (Query(0.0, 0.0, H00, V00) && Query(1.0, 0.0, H10, V10)
                && Query(0.0, 1.0, H01, V01))
            {
                const double Gradient = FMath::Sqrt(FMath::Square((H10 - H00)
                    / Snapshot.Metadata->DeliveredEastSpacingM) + FMath::Square((H01 - H00)
                    / Snapshot.Metadata->DeliveredNorthSpacingM));
                SlopeDegrees = FMath::RadiansToDegrees(FMath::Atan(Gradient));
            }
            if (PresentedCover && PresentedCoverValidity && bPresentedCoverGeographic)
            {
                const SkiDomain::GeodeticPoint& Origin = Snapshot.Metadata->LocalOrigin;
                SkiDomain::LocalFrame Frame;
                SkiDomain::GeodeticPoint Geographic;
                if (SkiDomain::TryMakeLocalFrame(
                        {Origin.LatitudeDeg, Origin.LongitudeDeg, 0.0}, Frame)
                    && SkiDomain::TrySeaLevelGeodeticFromEnu(Frame,
                        Hit.Position.East, Hit.Position.North, Geographic))
                {
                    bHaveCoverClass = SkiDomain::SampleCoverEcologyClass(PresentedCoverTransform,
                        *PresentedCover, *PresentedCoverValidity,
                        Geographic.LatitudeDeg, Geographic.LongitudeDeg, CoverClass);
                }
            }
        }
    }
    else if (Session)
    {
        const SkiApplication::TerrainSnapshot Snapshot = Session->Snapshot();
        if (Snapshot.Heightfield && Hit.Column + 1 < Snapshot.Heightfield->Width
            && Hit.Row + 1 < Snapshot.Heightfield->Height)
        {
            const auto& Field = *Snapshot.Heightfield;
            const float H00 = Field.Samples[static_cast<size_t>(Hit.Row) * Field.Width + Hit.Column];
            const float H10 = Field.Samples[static_cast<size_t>(Hit.Row) * Field.Width + Hit.Column + 1];
            const float H01 = Field.Samples[static_cast<size_t>(Hit.Row + 1) * Field.Width + Hit.Column];
            if (FMath::IsFinite(H00) && FMath::IsFinite(H10) && FMath::IsFinite(H01))
            {
                const double Gradient = FMath::Sqrt(FMath::Square((H10 - H00) / Field.EastSpacingM)
                    + FMath::Square((H01 - H00) / Field.NorthSpacingM));
                SlopeDegrees = FMath::RadiansToDegrees(FMath::Atan(Gradient));
            }
        }
    }
    const FString Slope = FMath::IsFinite(SlopeDegrees)
        ? FString::Printf(TEXT("%.1f°"), SlopeDegrees) : TEXT("unavailable");
    const FString Cover = bHaveCoverClass
        ? FString::Printf(TEXT("%u"), CoverClass) : TEXT("unavailable");
    return FString::Printf(TEXT("Probe r%u c%u | %.2f m | slope %s | cover %s | revision %llu"),
        Hit.Row, Hit.Column, Hit.Position.Up, *Slope, *Cover,
        static_cast<uint64>(Hit.SourceRevision));
}

void ASkiTerrainActor::SetLightingPreset(const FName Preset)
{
    // Lighting Actors and material instances are presentation-owned. This name is
    // intentionally a bounded view command, never weather or simulation state.
    if (Preset != TEXT("Midday") && Preset != TEXT("LowAngle") && Preset != TEXT("Overcast"))
    {
        return;
    }
    CurrentLightingPreset = Preset;
    Tags.RemoveAll([](const FName Tag) { return Tag.ToString().StartsWith(TEXT("Lighting:")); });
    Tags.Add(FName(*FString::Printf(TEXT("Lighting:%s"), *Preset.ToString())));
    const TCHAR* AssetPath = SkiTerrainRuntime::TerrainMaterialAssetPathForMode(
        CurrentViewMode, Preset);
    if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, AssetPath))
    {
        for (UDynamicMeshComponent* Tile : Tiles) if (Tile) Tile->SetMaterial(0, Material);
    }
    if (UMaterialInterface* Overlay = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/P1Generated/M_Overlay.M_Overlay"))) GuestDots->SetMaterial(0, Overlay);
    if (Preset == TEXT("LowAngle"))
    {
        SunLight->SetRelativeRotation(FRotator(-14.0, -62.0, 0.0));
        SunLight->SetIntensity(4.5F);
        SunLight->SetLightColor(FLinearColor(1.0F, 0.54F, 0.28F));
        SkyLight->SetIntensity(0.55F);
        SkyLight->SetLightColor(FLinearColor(0.42F, 0.50F, 0.70F));
    }
    else if (Preset == TEXT("Overcast"))
    {
        SunLight->SetRelativeRotation(FRotator(-62.0, -20.0, 0.0));
        SunLight->SetIntensity(1.3F);
        SunLight->SetLightColor(FLinearColor(0.72F, 0.78F, 0.86F));
        SkyLight->SetIntensity(1.65F);
        SkyLight->SetLightColor(FLinearColor(0.66F, 0.72F, 0.80F));
    }
    else
    {
        SunLight->SetRelativeRotation(FRotator(-52.0, -28.0, 0.0));
        SunLight->SetIntensity(8.0F);
        SunLight->SetLightColor(FLinearColor(1.0F, 0.93F, 0.78F));
        SkyLight->SetIntensity(1.05F);
        SkyLight->SetLightColor(FLinearColor(0.56F, 0.68F, 0.92F));
    }
}
