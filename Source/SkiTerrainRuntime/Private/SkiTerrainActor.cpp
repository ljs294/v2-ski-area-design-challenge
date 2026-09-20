#include "SkiTerrainRuntime/SkiTerrainActor.h"

#include "Async/Async.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LineBatchComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "SkiDomain/TerrainTile.h"
#include "UObject/ConstructorHelpers.h"

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
}

ASkiTerrainActor::ASkiTerrainActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("TerrainRoot"));
    RootComponent = SceneRoot;
    GuestDots = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GuestDots"));
    GuestDots->SetupAttachment(SceneRoot);
    GuestDots->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TerrainCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("TerrainCamera"));
    TerrainCamera->SetupAttachment(SceneRoot);
    SunLight = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("P1Sun"));
    SunLight->SetupAttachment(SceneRoot);
    SunLight->SetMobility(EComponentMobility::Movable);
    SunLight->SetCastShadows(true);
    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("P1Sky"));
    SkyLight->SetupAttachment(SceneRoot);
    SkyLight->SetMobility(EComponentMobility::Movable);
    OverlayLines = CreateDefaultSubobject<ULineBatchComponent>(TEXT("TerrainOverlays"));
    OverlayLines->SetupAttachment(SceneRoot);
    OverlayLines->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (Sphere.Succeeded()) GuestDots->SetStaticMesh(Sphere.Object);
}

void ASkiTerrainActor::SetTerrainSession(TSharedPtr<SkiApplication::TerrainSession> InSession)
{
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
    for (const SkiDomain::TerrainVertex& Vertex : SourceMesh.Vertices)
    {
        const int32 VertexId = Mesh.AppendVertex(FVector3d(
            static_cast<double>(Vertex.NorthM) * 100.0,
            static_cast<double>(Vertex.EastM) * 100.0,
            (static_cast<double>(Vertex.UpM) - OriginHeightM) * 100.0));
        Mesh.SetVertexNormal(VertexId, FVector3f(Vertex.NormalNorth, Vertex.NormalEast, Vertex.NormalUp));
        Mesh.SetVertexUV(VertexId, FVector2f(Vertex.U, Vertex.V));
        if (PresentedCover && PresentedCoverWidth > 0 && PresentedCoverHeight > 0)
        {
            const uint32 Column = FMath::Min(PresentedCoverWidth - 1,
                static_cast<uint32>(FMath::RoundToInt(Vertex.U * (PresentedCoverWidth - 1))));
            const uint32 Row = FMath::Min(PresentedCoverHeight - 1,
                static_cast<uint32>(FMath::RoundToInt(Vertex.V * (PresentedCoverHeight - 1))));
            Mesh.SetVertexColor(VertexId, CoverColor((*PresentedCover)[static_cast<size_t>(Row)
                * PresentedCoverWidth + Column]));
        }
    }
    for (int32 Index = 0; Index + 2 < static_cast<int32>(SourceMesh.Indices.size()); Index += 3)
    {
        const int32 Triangle = Mesh.AppendTriangle(static_cast<int32>(SourceMesh.Indices[Index]),
            static_cast<int32>(SourceMesh.Indices[Index + 1]),
            static_cast<int32>(SourceMesh.Indices[Index + 2]));
        if (Triangle < 0) return false;
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
    ClearTiles();
    PresentedOriginHeightM = Snapshot.Manifest ? Snapshot.Manifest->LocalOrigin.HeightM : 0.0;
    PresentedCover = Snapshot.Cover;
    PresentedCoverWidth = Snapshot.CoverWidth;
    PresentedCoverHeight = Snapshot.CoverHeight;
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
    const double EastExtentCm = (Snapshot.Heightfield->Width - 1) * Snapshot.Heightfield->EastSpacingM * 50.0;
    const double NorthExtentCm = (Snapshot.Heightfield->Height - 1) * Snapshot.Heightfield->NorthSpacingM * 50.0;
    const double Extent = FMath::Max(EastExtentCm, NorthExtentCm);
    const FVector CameraLocation(-NorthExtentCm * 0.75, -EastExtentCm * 1.05, Extent * 1.15);
    TerrainCamera->SetRelativeLocation(CameraLocation);
    TerrainCamera->SetRelativeRotation((-CameraLocation).Rotation());
    return !Session || Session->AcknowledgeRender(PresentedRevision);
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
    for (int32 Index = 0; Index < 3000; ++Index)
    {
        const uint32 Column = Random.RandRange(0, static_cast<int32>(Field.Width - 1));
        const uint32 Row = Random.RandRange(0, static_cast<int32>(Field.Height - 1));
        const float Height = Field.Samples[static_cast<size_t>(Row) * Field.Width + Column];
        if (!FMath::IsFinite(Height) || static_cast<double>(Height) == Field.NoDataValue) continue;
        const FVector Location(Field.SampleNorthM(Row) * 100.0, Field.EastM(Column) * 100.0,
            (static_cast<double>(Height) - OriginHeightM) * 100.0 + 100.0);
        GuestDots->AddInstance(FTransform(FQuat::Identity, Location, FVector(0.04)));
    }
}

bool ASkiTerrainActor::ApplyScratchMutation(const FVector2D& CenterEastNorthM,
    const double RadiusM, const double DeltaM)
{
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
    const TSharedPtr<SkiApplication::TerrainSession> CapturedSession = Session;
    const uint8 Lod = PresentedLod;
    const double OriginHeightM = PresentedOriginHeightM;
    const TWeakObjectPtr<ASkiTerrainActor> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, CapturedSession, Expected = Before.Readiness.Canonical,
        CenterEastNorthM, RadiusM, DeltaM, Lod, OriginHeightM, Completion = std::move(Completion)]() mutable
    {
        SkiDomain::MutationBounds Bounds;
        if (!CapturedSession->ApplyScratchMutation(Expected, CenterEastNorthM.X, CenterEastNorthM.Y,
                RadiusM, DeltaM, Bounds))
        {
            AsyncTask(ENamedThreads::GameThread, [Completion = std::move(Completion)]() mutable
            { if (Completion) Completion(false); });
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
                    AsyncTask(ENamedThreads::GameThread, [Completion = std::move(Completion)]() mutable
                    { if (Completion) Completion(false); });
                    return;
                }
                Meshes.Add(std::move(Mesh));
            }
        }
        AsyncTask(ENamedThreads::GameThread, [WeakThis, CapturedSession, Revision = Edited.Readiness.Canonical,
            Meshes = std::move(Meshes), OriginHeightM, Completion = std::move(Completion)]() mutable
        {
            if (!WeakThis.IsValid() || CapturedSession->Snapshot().Readiness.Canonical != Revision)
            {
                if (Completion) Completion(false);
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
                    if (Completion) Completion(false);
                    return;
                }
            }
            WeakThis->PresentedRevision = Revision;
            const SkiApplication::TerrainSnapshot Current = CapturedSession->Snapshot();
            if (Current.Heightfield) WeakThis->RebuildContourOverlay(*Current.Heightfield, OriginHeightM);
            WeakThis->SetLightingPreset(WeakThis->CurrentLightingPreset);
            const bool Ready = CapturedSession->AcknowledgeRender(Revision)
                && CapturedSession->Snapshot().Readiness.IsReady();
            if (Completion) Completion(Ready);
        });
    });
}

SkiDomain::RayHit ASkiTerrainActor::QueryCanonical(const FVector& WorldOriginCm,
    const FVector& WorldDirection) const
{
    if (!Session) return {};
    const SkiApplication::TerrainSnapshot Snapshot = Session->Snapshot();
    if (!Snapshot.Heightfield) return {};
    const FVector LocalOrigin = GetActorTransform().InverseTransformPosition(WorldOriginCm);
    const FVector LocalDirection = GetActorTransform().InverseTransformVectorNoScale(WorldDirection);
    const SkiDomain::Ray Query{{LocalOrigin.Y / 100.0, LocalOrigin.X / 100.0,
        LocalOrigin.Z / 100.0 + PresentedOriginHeightM},
        {LocalDirection.Y, LocalDirection.X, LocalDirection.Z}};
    return SkiDomain::QueryHeightfield(*Snapshot.Heightfield, Query);
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
    const TCHAR* AssetPath = Preset == TEXT("LowAngle")
        ? TEXT("/Game/P1Generated/M_Terrain_LowAngle.M_Terrain_LowAngle")
        : Preset == TEXT("Overcast")
            ? TEXT("/Game/P1Generated/M_Terrain_Overcast.M_Terrain_Overcast")
            : TEXT("/Game/P1Generated/M_Terrain_ClearMidday.M_Terrain_ClearMidday");
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
