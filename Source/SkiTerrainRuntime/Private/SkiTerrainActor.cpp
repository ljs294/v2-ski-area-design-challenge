#include "SkiTerrainRuntime/SkiTerrainActor.h"

#include "Async/Async.h"
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
    TArray<FVector4f> RenderColors;
    RenderColors.Reserve(static_cast<int32>(SourceMesh.Vertices.size()));
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
        else if (PresentedCover && PresentedCoverWidth > 0 && PresentedCoverHeight > 0)
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
    ClearTiles();
    PresentedOriginHeightM = Snapshot.Manifest ? Snapshot.Manifest->LocalOrigin.HeightM : 0.0;
    PresentedCover = Snapshot.Cover;
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
    return Session && Lod <= 2 && Present(Session->Snapshot(), Lod);
}

void ASkiTerrainActor::SetViewMode(const ESkiTerrainViewMode Mode)
{
    if (CurrentViewMode == Mode) return;
    CurrentViewMode = Mode;
    if (Session) Present(Session->Snapshot(), PresentedLod);
}

void ASkiTerrainActor::SetVerticalExaggeration(const float Scale)
{
    SetActorScale3D(FVector(1.0F, 1.0F, FMath::Clamp(Scale, 1.0F, 4.0F)));
}

bool ASkiTerrainActor::GetValidWorldBounds(FBox& OutBounds) const
{
    if (!ValidLocalBounds.IsValid) return false;
    OutBounds = ValidLocalBounds.TransformBy(GetActorTransform());
    return true;
}

bool ASkiTerrainActor::GetSteepestQuadrantWorldBounds(FBox& OutBounds) const
{
    if (!SteepestLocalBounds.IsValid) return false;
    OutBounds = SteepestLocalBounds.TransformBy(GetActorTransform());
    return true;
}

void ASkiTerrainActor::ShowTopologyPatch(const SkiDomain::RayHit& Hit)
{
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
    const TCHAR* AssetPath = CurrentViewMode != ESkiTerrainViewMode::Presentation
        ? TEXT("/Game/P1Generated/M_Overlay.M_Overlay")
        : Preset == TEXT("LowAngle")
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
