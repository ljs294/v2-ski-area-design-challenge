#include "Misc/AutomationTest.h"
#include "Materials/MaterialInterface.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCorePhotoTileSamplingTest,
    "Ski.TerrainRuntime.Photo.TileSamplingAndFences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCorePhotoTileSamplingTest::RunTest(const FString& Parameters)
{
    constexpr uint64 Generation = 17;
    constexpr uint32 Side = SkiTerrainRuntime::TerrainCorePhotoTileSide;
    const SkiApplication::TerrainCoreTileKey Key{2, 3, 4};
    SkiTerrainRuntime::FSkiTerrainPhotoTile Photo;
    Photo.Generation = Generation;
    Photo.Key = Key;
    Photo.BgraPixels.SetNumUninitialized(static_cast<int32>(Side * Side));

    // Encode scanline position into sRGB channels so UV origin, row direction,
    // and bilinear interpolation remain observable without an image asset.
    for (uint32 Y = 0; Y < Side; ++Y)
    {
        for (uint32 X = 0; X < Side; ++X)
        {
            Photo.BgraPixels[Y * Side + X] = FColor(
                static_cast<uint8>(X), static_cast<uint8>(Y),
                static_cast<uint8>((X + Y) / 2U), 255);
        }
    }

    FVector3f Color(-1.0F, -1.0F, -1.0F);
    TestTrue(TEXT("tile-local north-west UV samples the first decoded pixel"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            &Photo, Generation, Key, 0.0, 0.0, Color));
    const FLinearColor NorthWest = FLinearColor::FromSRGBColor(FColor(0, 0, 0, 255));
    TestTrue(TEXT("north-west color is converted from sRGB to linear"),
        Color.Equals(FVector3f(NorthWest.R, NorthWest.G, NorthWest.B), 1.0e-6F));

    TestTrue(TEXT("tile-local south-east UV samples the last decoded pixel"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            &Photo, Generation, Key, 1.0, 1.0, Color));
    const FLinearColor SouthEast = FLinearColor::FromSRGBColor(FColor(255, 255, 255, 255));
    TestTrue(TEXT("south-east pixel is mapped without flipping the scanline axis"),
        Color.Equals(FVector3f(SouthEast.R, SouthEast.G, SouthEast.B), 1.0e-6F));

    const double MidPixelUv = 127.5 / 255.0;
    TestTrue(TEXT("interior UV is sampled"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            &Photo, Generation, Key, MidPixelUv, MidPixelUv, Color));
    const FLinearColor C00 = FLinearColor::FromSRGBColor(FColor(127, 127, 127, 255));
    const FLinearColor C10 = FLinearColor::FromSRGBColor(FColor(128, 127, 127, 255));
    const FLinearColor C01 = FLinearColor::FromSRGBColor(FColor(127, 128, 127, 255));
    const FLinearColor C11 = FLinearColor::FromSRGBColor(FColor(128, 128, 128, 255));
    const FVector3f Midpoint(
        0.25F * (C00.R + C10.R + C01.R + C11.R),
        0.25F * (C00.G + C10.G + C01.G + C11.G),
        0.25F * (C00.B + C10.B + C01.B + C11.B));
    TestTrue(TEXT("bilinear sampling interpolates decoded sRGB texels in linear color"),
        Color.Equals(Midpoint, 1.0e-6F));

    const FVector3f Fallback(0.42F, 0.40F, 0.34F);
    Color = Fallback;
    TestFalse(TEXT("a stale photo generation is rejected"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            &Photo, Generation + 1U, Key, 0.5, 0.5, Color));
    TestTrue(TEXT("stale generation leaves the fallback color untouched"), Color == Fallback);

    TestFalse(TEXT("a mismatched exact tile key is rejected"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            &Photo, Generation, {2, 3, 5}, 0.5, 0.5, Color));
    TestTrue(TEXT("mismatched tile leaves the fallback color untouched"), Color == Fallback);

    TestFalse(TEXT("missing imagery is rejected"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            nullptr, Generation, Key, 0.5, 0.5, Color));
    TestTrue(TEXT("missing imagery leaves the fallback color untouched"), Color == Fallback);

    Photo.BgraPixels.SetNum(static_cast<int32>(Side * Side - 1U));
    TestFalse(TEXT("incomplete 256x256 imagery is rejected"),
        SkiTerrainRuntime::TrySampleTerrainCorePhotoVertexColor(
            &Photo, Generation, Key, 0.5, 0.5, Color));
    TestTrue(TEXT("malformed imagery leaves the fallback color untouched"), Color == Fallback);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCorePhotoMaterialPassthroughTest,
    "Ski.TerrainRuntime.Photo.MaterialPassthrough",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCorePhotoMaterialPassthroughTest::RunTest(const FString& Parameters)
{
    const TCHAR* PhotoMaterial = TEXT("/Game/P1Generated/M_Photo.M_Photo");
    TestNotNull(TEXT("the generated Photo material is loadable"),
        LoadObject<UMaterialInterface>(nullptr, PhotoMaterial));
    for (const FName Preset : {FName(TEXT("Midday")), FName(TEXT("LowAngle")),
            FName(TEXT("Overcast"))})
    {
        TestEqual(FString::Printf(TEXT("Photo retains its untinted material under %s lighting"),
                *Preset.ToString()),
            FString(SkiTerrainRuntime::TerrainMaterialAssetPathForMode(
                ESkiTerrainViewMode::Photo, Preset)), FString(PhotoMaterial));
    }

    TestEqual(TEXT("Presentation retains the existing midday material"),
        FString(SkiTerrainRuntime::TerrainMaterialAssetPathForMode(
            ESkiTerrainViewMode::Presentation, TEXT("Midday"))),
        FString(TEXT("/Game/P1Generated/M_Terrain_ClearMidday.M_Terrain_ClearMidday")));
    TestEqual(TEXT("Presentation retains the existing low-angle material"),
        FString(SkiTerrainRuntime::TerrainMaterialAssetPathForMode(
            ESkiTerrainViewMode::Presentation, TEXT("LowAngle"))),
        FString(TEXT("/Game/P1Generated/M_Terrain_LowAngle.M_Terrain_LowAngle")));
    TestEqual(TEXT("Cover remains on the existing overlay material"),
        FString(SkiTerrainRuntime::TerrainMaterialAssetPathForMode(
            ESkiTerrainViewMode::Cover, TEXT("Overcast"))),
        FString(TEXT("/Game/P1Generated/M_Overlay.M_Overlay")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCorePhotoRequestSnapshotRequiresSessionTest,
    "Ski.TerrainRuntime.Photo.RequestSnapshotRequiresSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCorePhotoRequestSnapshotRequiresSessionTest::RunTest(const FString& Parameters)
{
    ASkiTerrainActor* Actor = NewObject<ASkiTerrainActor>();
    TestNotNull(TEXT("A terrain actor exists for the snapshot probe"), Actor);
    if (!Actor) return false;

    uint64 Generation = 123U;
    TArray<SkiApplication::TerrainCoreTileKey> Keys;
    Keys.Add({1, 2, 3});
    TestFalse(TEXT("A missing TerrainCore session cannot produce photo requests"),
        Actor->GetTerrainCorePhotoRequestSnapshot(Generation, Keys));
    TestEqual(TEXT("A rejected snapshot clears its generation"), Generation,
        static_cast<uint64>(0));
    TestTrue(TEXT("A rejected snapshot clears its key list"), Keys.IsEmpty());
    return true;
}

#endif
