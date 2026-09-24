#if WITH_DEV_AUTOMATION_TESTS

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "SkiPreparation/SiteContextPhotoDecoder.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
using namespace SkiPreparation;

TArray<uint8> EncodeTestJpeg(const int32 Width, const int32 Height,
    const TFunction<FColor(int32, int32)>& MakePixel)
{
    if (Width <= 0 || Height <= 0 || !MakePixel) return {};
    TArray<FColor> Pixels;
    Pixels.SetNumUninitialized(Width * Height);
    for (int32 Y = 0; Y < Height; ++Y)
        for (int32 X = 0; X < Width; ++X)
            Pixels[Y * Width + X] = MakePixel(X, Y);

    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::JPEG);
    if (!Wrapper.IsValid()
        || !Wrapper->SetRaw(reinterpret_cast<const uint8*>(Pixels.GetData()),
            Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
    {
        return {};
    }
    const TArray64<uint8>& Compressed = Wrapper->GetCompressed(100);
    TArray<uint8> Result;
    if (Compressed.Num() > 0 && Compressed.Num() <= MAX_int32)
        Result.Append(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
    return Result;
}

uint8 ExpectedBilinearChannel(const FSiteContextPhotoTile& Tile,
    const double U, const double V, const uint8 FColor::* Channel)
{
    const double X = U * static_cast<double>(SiteContextPhotoTilePixels - 1U);
    const double Y = V * static_cast<double>(SiteContextPhotoTilePixels - 1U);
    const uint32 X0 = static_cast<uint32>(std::floor(X));
    const uint32 Y0 = static_cast<uint32>(std::floor(Y));
    const uint32 X1 = FMath::Min(X0 + 1U, SiteContextPhotoTilePixels - 1U);
    const uint32 Y1 = FMath::Min(Y0 + 1U, SiteContextPhotoTilePixels - 1U);
    const double Tx = X - X0;
    const double Ty = Y - Y0;
    const double Top = Tile.Pixels[Y0 * SiteContextPhotoTilePixels + X0].*Channel * (1.0 - Tx)
        + Tile.Pixels[Y0 * SiteContextPhotoTilePixels + X1].*Channel * Tx;
    const double Bottom = Tile.Pixels[Y1 * SiteContextPhotoTilePixels + X0].*Channel * (1.0 - Tx)
        + Tile.Pixels[Y1 * SiteContextPhotoTilePixels + X1].*Channel * Tx;
    return static_cast<uint8>(std::floor(Top * (1.0 - Ty) + Bottom * Ty + 0.5));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextPhotoDecoderValidDecodeAndSamplingTest,
    "MountainPlanner.M5.SiteContextPhotoDecoder.ValidDecodeAndSampling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextPhotoDecoderValidDecodeAndSamplingTest::RunTest(const FString& Parameters)
{
    const TArray<uint8> Jpeg = EncodeTestJpeg(SiteContextPhotoTilePixels,
        SiteContextPhotoTilePixels, [](const int32 X, const int32 Y)
        {
            if (X < 128 && Y < 128) return FColor(220, 35, 25);
            if (X >= 128 && Y < 128) return FColor(25, 210, 45);
            if (X < 128) return FColor(35, 55, 220);
            return FColor(230, 220, 35);
        });
    TestTrue(TEXT("fixture JPEG is present"), !Jpeg.IsEmpty());
    if (Jpeg.IsEmpty()) return false;

    FSiteContextPhotoTile Tile;
    FString Error;
    TestTrue(TEXT("the bounded decoder accepts a hash-verified JPEG tile"),
        DecodeSiteContextPhotoTile(Jpeg, Tile, Error));
    TestTrue(TEXT("decoded tile has exactly 256 by 256 BGRA8 pixels"), Tile.IsValid());
    if (!Tile.IsValid()) return false;

    FColor Color;
    TestTrue(TEXT("north-west UV is sampleable"),
        SampleSiteContextPhotoTileBilinear(Tile, 0.1, 0.1, Color));
    TestTrue(TEXT("BGRA decode preserves logical red, green and blue channels"),
        Color.R > 190 && Color.G < 70 && Color.B < 65 && Color.A == 255);
    TestTrue(TEXT("north-east UV selects the north-east image quadrant"),
        SampleSiteContextPhotoTileBilinear(Tile, 0.9, 0.1, Color));
    TestTrue(TEXT("north-east sample is green, not a byte-swapped channel"),
        Color.G > 175 && Color.R < 65 && Color.B < 75);
    TestTrue(TEXT("south-west UV selects the south-west image quadrant"),
        SampleSiteContextPhotoTileBilinear(Tile, 0.1, 0.9, Color));
    TestTrue(TEXT("south-west sample is blue"), Color.B > 185 && Color.R < 70 && Color.G < 85);

    TestTrue(TEXT("UV endpoints address the corner pixel centers"),
        SampleSiteContextPhotoTileBilinear(Tile, 0.0, 0.0, Color));
    TestEqual(TEXT("north-west endpoint red matches the decoded pixel"),
        static_cast<int32>(Color.R), static_cast<int32>(Tile.Pixels[0].R));
    TestTrue(TEXT("UV (1,1) addresses the south-east pixel center"),
        SampleSiteContextPhotoTileBilinear(Tile, 1.0, 1.0, Color));
    TestEqual(TEXT("south-east endpoint blue matches the decoded pixel"),
        static_cast<int32>(Color.B),
        static_cast<int32>(Tile.Pixels.Last().B));

    constexpr double SampleU = 0.503;
    constexpr double SampleV = 0.417;
    TestTrue(TEXT("interior UV is bilinearly sampleable"),
        SampleSiteContextPhotoTileBilinear(Tile, SampleU, SampleV, Color));
    TestEqual(TEXT("bilinear red channel uses deterministic rounded interpolation"),
        static_cast<int32>(Color.R), static_cast<int32>(ExpectedBilinearChannel(
            Tile, SampleU, SampleV, &FColor::R)));
    TestEqual(TEXT("bilinear green channel uses deterministic rounded interpolation"),
        static_cast<int32>(Color.G), static_cast<int32>(ExpectedBilinearChannel(
            Tile, SampleU, SampleV, &FColor::G)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSiteContextPhotoDecoderFailsClosedTest,
    "MountainPlanner.M5.SiteContextPhotoDecoder.FailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSiteContextPhotoDecoderFailsClosedTest::RunTest(const FString& Parameters)
{
    const auto NeverCancelled = [] { return false; };
    FSiteContextPhotoTile Tile;
    FString Error;

    TestFalse(TEXT("missing image bytes are rejected"),
        DecodeSiteContextPhotoTile({}, Tile, Error, NeverCancelled));
    FColor EmptyColor;
    TestFalse(TEXT("an empty decoded tile cannot be sampled"),
        SampleSiteContextPhotoTileBilinear(Tile, 0.5, 0.5, EmptyColor));

    TArray<uint8> Oversize;
    Oversize.SetNumZeroed(static_cast<int32>(SiteContextPhotoTileMaxCompressedBytes + 1U));
    TestFalse(TEXT("compressed data above the strict tile cap is rejected before decode"),
        DecodeSiteContextPhotoTile(Oversize, Tile, Error, NeverCancelled));
    TestFalse(TEXT("decoder clears previous output when an oversize tile fails"), Tile.IsValid());

    const TArray<uint8> Malformed{0xff, 0xd8, 0x00, 0x01, 0xff, 0xd9};
    TestFalse(TEXT("malformed JPEG bytes are rejected"),
        DecodeSiteContextPhotoTile(Malformed, Tile, Error, NeverCancelled));
    TestFalse(TEXT("wrong-dimension JPEG tiles are rejected"),
        DecodeSiteContextPhotoTile(EncodeTestJpeg(255, 256,
            [](int32, int32) { return FColor(80, 90, 100); }), Tile, Error, NeverCancelled));

    const TArray<uint8> GoodJpeg = EncodeTestJpeg(SiteContextPhotoTilePixels,
        SiteContextPhotoTilePixels, [](int32, int32) { return FColor(90, 100, 110); });
    TestFalse(TEXT("pre-cancelled decode performs no decode"),
        DecodeSiteContextPhotoTile(GoodJpeg, Tile, Error, [] { return true; }));
    int32 CancellationChecks = 0;
    TestFalse(TEXT("cancellation after JPEG metadata parsing prevents raw pixel publication"),
        DecodeSiteContextPhotoTile(GoodJpeg, Tile, Error,
            [&CancellationChecks] { return ++CancellationChecks >= 2; }));
    TestFalse(TEXT("cancelled decode leaves no partial pixels"), Tile.IsValid());

    TestTrue(TEXT("a valid tile decodes for invalid-coordinate tests"),
        DecodeSiteContextPhotoTile(GoodJpeg, Tile, Error, NeverCancelled));
    FColor Color(1, 2, 3, 4);
    TestFalse(TEXT("non-finite UV is rejected"), SampleSiteContextPhotoTileBilinear(
        Tile, std::numeric_limits<double>::quiet_NaN(), 0.5, Color));
    TestEqual(TEXT("failed sample clears its output color"), static_cast<int32>(Color.A), 0);
    TestFalse(TEXT("out-of-tile UV does not clamp across imagery tile boundaries"),
        SampleSiteContextPhotoTileBilinear(Tile, -0.001, 0.5, Color));
    return true;
}

#endif
