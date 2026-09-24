#include "SkiPreparation/SiteContextPhotoDecoder.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"

#include <cmath>

namespace
{
bool IsCancelled(const TFunction<bool()>& Callback)
{
    return Callback && Callback();
}

bool FailDecode(const TCHAR* Message, FString& OutError)
{
    OutError = Message;
    return false;
}

uint8 RoundChannel(const double Value) noexcept
{
    // Channels are convex combinations in [0,255]. floor(x+0.5) defines ties
    // consistently and does not depend on the current floating-point rounding mode.
    return static_cast<uint8>(std::floor(Value + 0.5));
}

uint8 InterpolateChannel(const FColor& P00, const FColor& P10,
    const FColor& P01, const FColor& P11, const uint8 FColor::* Channel,
    const double Tx, const double Ty) noexcept
{
    const double Top = static_cast<double>(P00.*Channel) * (1.0 - Tx)
        + static_cast<double>(P10.*Channel) * Tx;
    const double Bottom = static_cast<double>(P01.*Channel) * (1.0 - Tx)
        + static_cast<double>(P11.*Channel) * Tx;
    return RoundChannel(Top * (1.0 - Ty) + Bottom * Ty);
}
}

bool SkiPreparation::DecodeSiteContextPhotoTile(const TArray<uint8>& CompressedJpeg,
    FSiteContextPhotoTile& OutTile, FString& OutError,
    const TFunction<bool()>& IsCancelledCallback)
{
    OutTile = {};
    OutError.Reset();
    if (IsCancelled(IsCancelledCallback))
        return FailDecode(TEXT("SiteContext photo decode was cancelled."), OutError);
    if (CompressedJpeg.IsEmpty())
        return FailDecode(TEXT("SiteContext photo asset is missing or empty."), OutError);
    if (static_cast<uint64>(CompressedJpeg.Num()) > SiteContextPhotoTileMaxCompressedBytes)
        return FailDecode(TEXT("SiteContext photo asset exceeds the compressed tile limit."), OutError);
    if (CompressedJpeg.Num() < 4 || CompressedJpeg[0] != 0xff || CompressedJpeg[1] != 0xd8)
        return FailDecode(TEXT("SiteContext photo asset is not a JPEG image."), OutError);

    static_assert(sizeof(FColor) == 4, "SiteContext photo BGRA8 pixels require four-byte FColor.");
    constexpr int32 ExpectedPixelCount = static_cast<int32>(SiteContextPhotoTilePixels
        * SiteContextPhotoTilePixels);
    constexpr int32 ExpectedDecodedBytes = static_cast<int32>(SiteContextPhotoTileDecodedBytes);

    IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(EImageFormat::JPEG);
    if (!Wrapper.IsValid()
        || !Wrapper->SetCompressed(CompressedJpeg.GetData(), CompressedJpeg.Num())
        || Wrapper->GetWidth() != static_cast<int32>(SiteContextPhotoTilePixels)
        || Wrapper->GetHeight() != static_cast<int32>(SiteContextPhotoTilePixels))
    {
        return FailDecode(TEXT("SiteContext imagery is not a valid 256 by 256 JPEG tile."), OutError);
    }
    if (IsCancelled(IsCancelledCallback))
        return FailDecode(TEXT("SiteContext photo decode was cancelled."), OutError);

    TArray<uint8> Decoded;
    if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Decoded)
        || Decoded.Num() != ExpectedDecodedBytes)
    {
        return FailDecode(TEXT("SiteContext JPEG did not decode to bounded BGRA8 pixels."), OutError);
    }
    if (IsCancelled(IsCancelledCallback))
        return FailDecode(TEXT("SiteContext photo decode was cancelled."), OutError);

    FSiteContextPhotoTile Candidate;
    Candidate.Pixels.SetNumUninitialized(ExpectedPixelCount);
    FMemory::Memcpy(Candidate.Pixels.GetData(), Decoded.GetData(), ExpectedDecodedBytes);
    OutTile = MoveTemp(Candidate);
    return true;
}

bool SkiPreparation::SampleSiteContextPhotoTileBilinear(
    const FSiteContextPhotoTile& Tile, const double U, const double V,
    FColor& OutColor) noexcept
{
    OutColor = FColor(0, 0, 0, 0);
    if (!Tile.IsValid() || !std::isfinite(U) || !std::isfinite(V)
        || U < 0.0 || U > 1.0 || V < 0.0 || V > 1.0)
    {
        return false;
    }

    const double X = U * static_cast<double>(SiteContextPhotoTilePixels - 1U);
    const double Y = V * static_cast<double>(SiteContextPhotoTilePixels - 1U);
    const uint32 X0 = static_cast<uint32>(std::floor(X));
    const uint32 Y0 = static_cast<uint32>(std::floor(Y));
    const uint32 X1 = FMath::Min(X0 + 1U, SiteContextPhotoTilePixels - 1U);
    const uint32 Y1 = FMath::Min(Y0 + 1U, SiteContextPhotoTilePixels - 1U);
    const double Tx = X - static_cast<double>(X0);
    const double Ty = Y - static_cast<double>(Y0);
    const FColor& P00 = Tile.Pixels[Y0 * SiteContextPhotoTilePixels + X0];
    const FColor& P10 = Tile.Pixels[Y0 * SiteContextPhotoTilePixels + X1];
    const FColor& P01 = Tile.Pixels[Y1 * SiteContextPhotoTilePixels + X0];
    const FColor& P11 = Tile.Pixels[Y1 * SiteContextPhotoTilePixels + X1];

    OutColor = FColor(
        InterpolateChannel(P00, P10, P01, P11, &FColor::R, Tx, Ty),
        InterpolateChannel(P00, P10, P01, P11, &FColor::G, Tx, Ty),
        InterpolateChannel(P00, P10, P01, P11, &FColor::B, Tx, Ty),
        InterpolateChannel(P00, P10, P01, P11, &FColor::A, Tx, Ty));
    return true;
}
