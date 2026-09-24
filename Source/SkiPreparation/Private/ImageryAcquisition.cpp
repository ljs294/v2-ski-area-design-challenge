#include "SkiPreparation/ImageryAcquisition.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "SkiDomain/Coordinates.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
using namespace SkiPreparation;

constexpr std::uint32_t SourceZoom = 17;
constexpr std::uint32_t SourceTilePixels = 256;
constexpr std::uint64_t SourceTileDecodedBytes =
    static_cast<std::uint64_t>(SourceTilePixels) * SourceTilePixels * sizeof(FColor);
constexpr double Pi = 3.141592653589793238462643383279502884;
constexpr double WebMercatorLatitudeLimitDeg = 85.0511287798066;
constexpr std::uint32_t MaximumPyramidTilesHard = 512;
constexpr std::uint32_t MaximumSourceRequestsHard = 512;
constexpr std::uint32_t MaximumSourceTilesPerOutputHard = 16;
constexpr std::uint64_t MaximumSourceTileBytesHard = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaximumSourceTransferBytesHard = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaximumDecodedSourceBytesHard = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaximumWorkingMemoryBytesHard = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaximumOutputTileBytesHard = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t MaximumOutputBytesHard = 256ULL * 1024ULL * 1024ULL;
constexpr double MaximumElapsedSecondsHard = 900.0;
constexpr uint64 MaximumOutputPixels =
    static_cast<uint64>(ImageryPyramidTilePixels) * ImageryPyramidTilePixels;
constexpr uint64 MaximumMappingScratchBytes = MaximumOutputPixels * sizeof(double) * 2ULL;
constexpr uint64 MaximumOutputRawBytes = SourceTileDecodedBytes;
constexpr uint64 MaximumCodecScratchBytes = 1ULL * 1024ULL * 1024ULL;
constexpr uint64 MaximumManifestScratchBytes = 1ULL * 1024ULL * 1024ULL;

struct FSourcePixelPosition
{
    double X = 0.0;
    double Y = 0.0;
};

struct FDecodedSourceTile
{
    /** BGRA8 bytes; this is the bounded resident image cache payload. */
    TArray<uint8> Pixels;
    uint64 LastUse = 0;
};

class FCountingAcquisitionTransport final : public IAcquisitionTransport
{
public:
    FCountingAcquisitionTransport(IAcquisitionTransport& InTransport,
        ImageryAcquisitionReport& InReport)
        : Transport(InTransport), Report(InReport) {}

    HttpAcquisitionResult Get(const HttpAcquisitionRequest& Request,
        const TSharedRef<Cancellation>& CancellationValue) override
    {
        ++Report.Requests;
        return Transport.Get(Request, CancellationValue);
    }

private:
    IAcquisitionTransport& Transport;
    ImageryAcquisitionReport& Report;
};

uint64 SourceTileKey(const uint32 X, const uint32 Y) noexcept
{
    return (static_cast<uint64>(Y) << 32U) | X;
}

uint32 SourceTileX(const uint64 Key) noexcept
{
    return static_cast<uint32>(Key & 0xffffffffULL);
}

uint32 SourceTileY(const uint64 Key) noexcept
{
    return static_cast<uint32>(Key >> 32U);
}

uint32 LodDimension(const uint32 BaseDimension, const uint32 Factor) noexcept
{
    return static_cast<uint32>((static_cast<uint64>(BaseDimension) - 1ULL
        + Factor - 1ULL) / Factor + 1ULL);
}

bool CountRequiredTiles(const SkiDomain::TerrainCoreManifest& TerrainCore,
    uint64& OutCount) noexcept
{
    OutCount = 0;
    if (TerrainCore.Width < 2 || TerrainCore.Height < 2
        || static_cast<uint64>(TerrainCore.Width) * TerrainCore.Height
            > SkiDomain::TerrainCoreMaxSamples)
    {
        return false;
    }

    for (const uint32 Factor : SkiDomain::TerrainCoreLodFactors)
    {
        const uint32 Width = LodDimension(TerrainCore.Width, Factor);
        const uint32 Height = LodDimension(TerrainCore.Height, Factor);
        const uint64 TilesX = (static_cast<uint64>(Width) - 1ULL
            + SkiDomain::TerrainCoreTileCells - 1ULL) / SkiDomain::TerrainCoreTileCells;
        const uint64 TilesY = (static_cast<uint64>(Height) - 1ULL
            + SkiDomain::TerrainCoreTileCells - 1ULL) / SkiDomain::TerrainCoreTileCells;
        if (TilesX != 0 && TilesY > (std::numeric_limits<uint64>::max() - OutCount) / TilesX)
            return false;
        OutCount += TilesX * TilesY;
    }
    return OutCount != 0;
}

bool ValidBudget(const ImageryAcquisitionBudget& Budget) noexcept
{
    const uint64 WorkingBound = Budget.MaximumDecodedSourceBytes
        + Budget.MaximumSourceTileBytes + SourceTileDecodedBytes
        + MaximumMappingScratchBytes + MaximumOutputRawBytes
        + 2ULL * Budget.MaximumOutputTileBytes + MaximumCodecScratchBytes
        + MaximumManifestScratchBytes;
    return Budget.MaximumPyramidTiles > 0
        && Budget.MaximumPyramidTiles <= MaximumPyramidTilesHard
        && Budget.MaximumSourceRequests > 0
        && Budget.MaximumSourceRequests <= MaximumSourceRequestsHard
        && Budget.MaximumSourceTilesPerOutputTile > 0
        && Budget.MaximumSourceTilesPerOutputTile <= MaximumSourceTilesPerOutputHard
        && Budget.MaximumSourceTileBytes > 0
        && Budget.MaximumSourceTileBytes <= MaximumSourceTileBytesHard
        && Budget.MaximumSourceTransferBytes > 0
        && Budget.MaximumSourceTransferBytes <= MaximumSourceTransferBytesHard
        && Budget.MaximumDecodedSourceBytes >= SourceTileDecodedBytes
        && Budget.MaximumDecodedSourceBytes <= MaximumDecodedSourceBytesHard
        && Budget.MaximumWorkingMemoryBytes >= WorkingBound
        && Budget.MaximumWorkingMemoryBytes <= MaximumWorkingMemoryBytesHard
        && Budget.MaximumOutputTileBytes > 0
        && Budget.MaximumOutputTileBytes <= MaximumOutputTileBytesHard
        && Budget.MaximumOutputBytes > 0
        && Budget.MaximumOutputBytes <= MaximumOutputBytesHard
        && std::isfinite(Budget.MaximumElapsedSeconds)
        && Budget.MaximumElapsedSeconds > 0.0
        && Budget.MaximumElapsedSeconds <= MaximumElapsedSecondsHard;
}

bool IsActive(const TSharedRef<Cancellation>& CancellationValue,
    const double BeganSeconds, const ImageryAcquisitionBudget& Budget) noexcept
{
    return !CancellationValue->IsCancelled()
        && FPlatformTime::Seconds() - BeganSeconds < Budget.MaximumElapsedSeconds;
}

bool ProjectTerrainCoreToWebMercator(const SkiDomain::LocalFrame& Frame,
    const double EastM, const double NorthM, FSourcePixelPosition& OutPosition) noexcept
{
    OutPosition = {};
    SkiDomain::GeodeticPoint Geodetic;
    if (!SkiDomain::TrySeaLevelGeodeticFromEnu(Frame, EastM, NorthM, Geodetic)
        || !std::isfinite(Geodetic.LatitudeDeg)
        || !std::isfinite(Geodetic.LongitudeDeg)
        || std::abs(Geodetic.LatitudeDeg) > WebMercatorLatitudeLimitDeg)
    {
        return false;
    }

    const double WorldPixels = static_cast<double>(SourceTilePixels)
        * static_cast<double>(1U << SourceZoom);
    const double LatitudeRadians = Geodetic.LatitudeDeg * Pi / 180.0;
    const double WorldX = (Geodetic.LongitudeDeg + 180.0) / 360.0 * WorldPixels;
    const double WorldY = (0.5 - std::asinh(std::tan(LatitudeRadians))
        / (2.0 * Pi)) * WorldPixels;
    const double LastPixel = WorldPixels - 1.0;
    if (!std::isfinite(WorldX) || !std::isfinite(WorldY)
        || WorldX < 0.0 || WorldX > WorldPixels
        || WorldY < 0.0 || WorldY > WorldPixels)
    {
        return false;
    }

    // Web-Mercator coordinates denote pixel edges. Shift once to the source
    // pixel-center lattice before the deterministic bilinear reconstruction.
    OutPosition.X = std::clamp(WorldX - 0.5, 0.0, LastPixel);
    OutPosition.Y = std::clamp(WorldY - 0.5, 0.0, LastPixel);
    return true;
}

bool IsMimeType(const FString& ContentType, const TCHAR* Expected)
{
    FString Mime = ContentType;
    int32 Semicolon = INDEX_NONE;
    if (Mime.FindChar(TEXT(';'), Semicolon)) Mime = Mime.Left(Semicolon);
    return Mime.TrimStartAndEnd().Equals(Expected, ESearchCase::IgnoreCase);
}

bool DecodeSourceTile(const HttpAcquisitionResult& Response,
    const TSharedRef<Cancellation>& CancellationValue, TArray<uint8>& OutBgra,
    FString& OutError)
{
    OutBgra.Reset();
    OutError.Reset();
    if (!IsMimeType(Response.ContentType, TEXT("image/jpeg"))
        && !IsMimeType(Response.ContentType, TEXT("image/jpg"))
        && !IsMimeType(Response.ContentType, TEXT("image/png")))
    {
        OutError = TEXT("USGS imagery returned an unsupported media type.");
        return false;
    }
    if (Response.Bytes.IsEmpty()
        || Response.Bytes.Num() > static_cast<int32>(MaximumSourceTileBytesHard)
        || CancellationValue->IsCancelled())
    {
        OutError = CancellationValue->IsCancelled()
            ? TEXT("Cancelled while decoding a source image tile.")
            : TEXT("USGS imagery response is empty or exceeds the compressed tile limit.");
        return false;
    }

    const EImageFormat Format = IsMimeType(Response.ContentType, TEXT("image/png"))
        ? EImageFormat::PNG : EImageFormat::JPEG;
    IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(Format);
    if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Response.Bytes.GetData(), Response.Bytes.Num())
        || Wrapper->GetWidth() != static_cast<int32>(SourceTilePixels)
        || Wrapper->GetHeight() != static_cast<int32>(SourceTilePixels))
    {
        OutError = TEXT("USGS imagery tile is not a valid 256 by 256 image.");
        return false;
    }

    if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutBgra)
        || OutBgra.Num() != static_cast<int32>(SourceTileDecodedBytes))
    {
        OutBgra.Reset();
        OutError = TEXT("USGS imagery tile could not be decoded to bounded BGRA8 pixels.");
        return false;
    }
    return !CancellationValue->IsCancelled();
}

FString SourceTileUrl(const uint32 X, const uint32 Y)
{
    return FString::Printf(TEXT("https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/%u/%u/%u"),
        SourceZoom, Y, X);
}

struct FSampledColor
{
    uint8 Red = 255;
    uint8 Green = 255;
    uint8 Blue = 255;
    double Alpha = 0.0;
};

bool SampleBilinear(const FSourcePixelPosition& Position,
    const TMap<uint64, FDecodedSourceTile>& DecodedTiles,
    FSampledColor& OutColor) noexcept
{
    OutColor = {};
    const int32 X0 = FMath::FloorToInt(Position.X);
    const int32 Y0 = FMath::FloorToInt(Position.Y);
    const int32 X1 = FMath::Min(X0 + 1, (1 << (SourceZoom + 8U)) - 1);
    const int32 Y1 = FMath::Min(Y0 + 1, (1 << (SourceZoom + 8U)) - 1);
    const double Tx = Position.X - static_cast<double>(X0);
    const double Ty = Position.Y - static_cast<double>(Y0);
    const std::array<double, 4> Weights{
        (1.0 - Tx) * (1.0 - Ty), Tx * (1.0 - Ty),
        (1.0 - Tx) * Ty, Tx * Ty};
    const std::array<int32, 4> Xs{X0, X1, X0, X1};
    const std::array<int32, 4> Ys{Y0, Y0, Y1, Y1};

    double WeightedAlpha = 0.0;
    double PremultipliedRed = 0.0;
    double PremultipliedGreen = 0.0;
    double PremultipliedBlue = 0.0;
    for (std::size_t Tap = 0; Tap < Weights.size(); ++Tap)
    {
        const uint32 TileX = static_cast<uint32>(Xs[Tap]) / SourceTilePixels;
        const uint32 TileY = static_cast<uint32>(Ys[Tap]) / SourceTilePixels;
        const FDecodedSourceTile* Tile = DecodedTiles.Find(SourceTileKey(TileX, TileY));
        if (Tile == nullptr || Tile->Pixels.Num() != static_cast<int32>(SourceTileDecodedBytes))
            return false;
        const uint32 LocalX = static_cast<uint32>(Xs[Tap]) % SourceTilePixels;
        const uint32 LocalY = static_cast<uint32>(Ys[Tap]) % SourceTilePixels;
        const int32 Offset = static_cast<int32>((LocalY * SourceTilePixels + LocalX) * 4U);
        const double Alpha = static_cast<double>(Tile->Pixels[Offset + 3]) / 255.0;
        const double WeightedTapAlpha = Alpha * Weights[Tap];
        WeightedAlpha += WeightedTapAlpha;
        PremultipliedBlue += static_cast<double>(Tile->Pixels[Offset]) * WeightedTapAlpha;
        PremultipliedGreen += static_cast<double>(Tile->Pixels[Offset + 1]) * WeightedTapAlpha;
        PremultipliedRed += static_cast<double>(Tile->Pixels[Offset + 2]) * WeightedTapAlpha;
    }
    if (WeightedAlpha <= 1.0e-12) return true;

    // Transparent source pixels carry no image data. Composite the supported
    // portion against white because the installed format is opaque JPEG.
    const auto Composite = [WeightedAlpha](const double Premultiplied)
    {
        const double Straight = Premultiplied / WeightedAlpha;
        return static_cast<uint8>(std::clamp(std::lround(
            Straight * WeightedAlpha + 255.0 * (1.0 - WeightedAlpha)), 0L, 255L));
    };
    OutColor.Red = Composite(PremultipliedRed);
    OutColor.Green = Composite(PremultipliedGreen);
    OutColor.Blue = Composite(PremultipliedBlue);
    OutColor.Alpha = WeightedAlpha;
    return true;
}

bool EncodeOutputJpeg(const TArray<uint8>& Bgra, const uint64 MaximumBytes,
    TArray<uint8>& OutBytes, FString& OutError)
{
    OutBytes.Reset();
    OutError.Reset();
    if (Bgra.Num() != static_cast<int32>(SourceTileDecodedBytes))
    {
        OutError = TEXT("Reprojected imagery buffer has the wrong dimensions.");
        return false;
    }

    IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper"));
    const TSharedPtr<IImageWrapper> Wrapper = ImageModule.CreateImageWrapper(EImageFormat::JPEG);
    if (!Wrapper.IsValid()
        || !Wrapper->SetRaw(Bgra.GetData(), Bgra.Num(), SourceTilePixels, SourceTilePixels,
            ERGBFormat::BGRA, 8))
    {
        OutError = TEXT("Unable to initialize the TerrainCore-aligned JPEG encoder.");
        return false;
    }
    const TArray64<uint8>& Encoded = Wrapper->GetCompressed(90);
    if (Encoded.IsEmpty() || static_cast<uint64>(Encoded.Num()) > MaximumBytes
        || Encoded.Num() > MAX_int32)
    {
        OutError = TEXT("Encoded imagery tile is empty or exceeds the output tile limit.");
        return false;
    }
    OutBytes.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
    return true;
}

bool FetchSourceTile(IAcquisitionTransport& Transport,
    const uint32 X, const uint32 Y,
    const TSharedRef<Cancellation>& CancellationValue,
    const ImageryAcquisitionBudget& Budget, const double BeganSeconds,
    ImageryAcquisitionReport& Report,
    TMap<uint64, FDecodedSourceTile>& DecodedTiles, uint64& DecodedBytes,
    TSet<uint64>& PinnedKeys)
{
    const uint64 Key = SourceTileKey(X, Y);
    if (FDecodedSourceTile* Cached = DecodedTiles.Find(Key))
    {
        Cached->LastUse = Report.Requests + static_cast<uint64>(DecodedTiles.Num()) + 1ULL;
        PinnedKeys.Add(Key);
        return true;
    }
    if (Report.Requests >= Budget.MaximumSourceRequests)
    {
        Report.Error = ImageryAcquisitionError::RequestBudgetExceeded;
        Report.FailureDetail = TEXT("The imagery source tile request budget was exhausted.");
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        return false;
    }
    if (Report.SourceBytes >= Budget.MaximumSourceTransferBytes)
    {
        Report.Error = ImageryAcquisitionError::TransferBudgetExceeded;
        Report.FailureDetail = TEXT("The imagery source transfer budget was exhausted.");
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        return false;
    }
    if (CancellationValue->IsCancelled())
    {
        Report.Error = ImageryAcquisitionError::Cancelled;
        Report.FailureDetail = TEXT("Imagery acquisition was cancelled before a source request.");
        return false;
    }
    if (FPlatformTime::Seconds() - BeganSeconds >= Budget.MaximumElapsedSeconds)
    {
        Report.Error = ImageryAcquisitionError::DeadlineExceeded;
        Report.FailureDetail = TEXT("The imagery acquisition deadline expired.");
        return false;
    }

    const FString Url = SourceTileUrl(X, Y);
    FString UrlReason;
    if (!SkiNetGateway::ValidateUrl(Url, UrlReason))
    {
        Report.Error = ImageryAcquisitionError::InvalidGatewayUrl;
        Report.FailureDetail = FString::Printf(TEXT("Generated USGS imagery URL failed gateway validation: %s"),
            *UrlReason);
        return false;
    }

    HttpAcquisitionRequest Request;
    Request.Url = Url;
    Request.Product = ProviderProduct::Imagery;
    Request.Tile.Column = static_cast<int32>(X);
    Request.Tile.Row = static_cast<int32>(Y);
    Request.Tile.Width = SourceTilePixels;
    Request.Tile.Height = SourceTilePixels;
    Request.MaximumResponseBytes = FMath::Min<uint64>(Budget.MaximumSourceTileBytes,
        Budget.MaximumSourceTransferBytes - Report.SourceBytes);
    Request.TotalTimeoutSeconds = static_cast<float>(Budget.MaximumElapsedSeconds);
    Request.ActivityTimeoutSeconds = 60.0F;

    RetryPolicy Policy;
    // This small path makes one wire attempt per source tile so the request
    // counter is an exact bound; orchestration can retry the whole staged run.
    Policy.MaximumAttempts = 1;
    Policy.ActivityTimeoutSeconds = Request.ActivityTimeoutSeconds;
    Policy.TotalTimeoutSeconds = Request.TotalTimeoutSeconds;
    Policy.OperationDeadlineSeconds = Budget.MaximumElapsedSeconds;
    Policy.MaximumResponseBytes = Request.MaximumResponseBytes;
    HttpAcquisitionResult Response;
    const auto IsCurrent = [&]()
    {
        return IsActive(CancellationValue, BeganSeconds, Budget);
    };
    FCountingAcquisitionTransport CountedTransport(Transport, Report);
    const bool bRequestOk = ExecuteAcquisitionWithRetry(CountedTransport, Request, Policy,
        CancellationValue, BeganSeconds, IsCurrent, {}, Response);
    const uint64 Received = FMath::Max<uint64>(Response.BytesReceived,
        static_cast<uint64>(Response.Bytes.Num()));
    if (Received > Request.MaximumResponseBytes
        || Received > Budget.MaximumSourceTransferBytes - Report.SourceBytes)
    {
        Report.SourceBytes = Budget.MaximumSourceTransferBytes;
        Report.Error = ImageryAcquisitionError::TransferBudgetExceeded;
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        Report.HttpStatus = Response.HttpStatus;
        Report.FailureDetail = TEXT("USGS imagery response exceeded the bounded transfer allowance.");
        return false;
    }
    Report.SourceBytes += Received;
    if (CancellationValue->IsCancelled())
    {
        Report.Error = ImageryAcquisitionError::Cancelled;
        Report.FailureDetail = TEXT("Imagery acquisition was cancelled during a source request.");
        return false;
    }
    if (!IsCurrent())
    {
        Report.Error = FPlatformTime::Seconds() - BeganSeconds >= Budget.MaximumElapsedSeconds
            ? ImageryAcquisitionError::DeadlineExceeded
            : ImageryAcquisitionError::Cancelled;
        Report.FailureDetail = TEXT("Imagery acquisition stopped before source-tile decoding.");
        return false;
    }
    if (Response.HttpStatus == 204 || Response.HttpStatus == 404)
    {
        Report.Error = ImageryAcquisitionError::MissingSourceTile;
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        Report.HttpStatus = Response.HttpStatus;
        Report.FailureDetail = TEXT("USGS returned no imagery for a required source tile.");
        return false;
    }
    if (!bRequestOk || !Response.Ok())
    {
        if (Response.FailureReason == TransportFailureReason::ResponseTooLarge)
        {
            Report.Error = ImageryAcquisitionError::TransferBudgetExceeded;
            Report.SourceTileX = X;
            Report.SourceTileY = Y;
            Report.HttpStatus = Response.HttpStatus;
            Report.FailureDetail = TEXT("USGS imagery exceeded the bounded source response allowance.");
            return false;
        }
        Report.Error = ImageryAcquisitionError::SourceRequestFailed;
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        Report.HttpStatus = Response.HttpStatus;
        Report.FailureDetail = FString::Printf(TEXT("USGS imagery request failed (%s, HTTP %d)."),
            TransportFailureReasonName(Response.FailureReason), Response.HttpStatus);
        return false;
    }
    if (Response.Bytes.Num() <= 0
        || static_cast<uint64>(Response.Bytes.Num()) > Budget.MaximumSourceTileBytes)
    {
        Report.Error = ImageryAcquisitionError::TransferBudgetExceeded;
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        Report.HttpStatus = Response.HttpStatus;
        Report.FailureDetail = TEXT("USGS source tile exceeded its per-response byte limit.");
        return false;
    }
    if (DecodedBytes + SourceTileDecodedBytes > Budget.MaximumDecodedSourceBytes)
    {
        // Evict least-recently-used decoded tiles that this output tile will not sample.
        while (DecodedBytes + SourceTileDecodedBytes > Budget.MaximumDecodedSourceBytes)
        {
            uint64 OldestKey = 0;
            uint64 OldestUse = std::numeric_limits<uint64>::max();
            bool bFoundVictim = false;
            for (const TPair<uint64, FDecodedSourceTile>& Pair : DecodedTiles)
            {
                if (PinnedKeys.Contains(Pair.Key) || Pair.Key == Key) continue;
                if (!bFoundVictim || Pair.Value.LastUse < OldestUse)
                {
                    bFoundVictim = true;
                    OldestKey = Pair.Key;
                    OldestUse = Pair.Value.LastUse;
                }
            }
            if (!bFoundVictim)
            {
                Report.Error = ImageryAcquisitionError::DecodedMemoryBudgetExceeded;
                Report.SourceTileX = X;
                Report.SourceTileY = Y;
                Report.FailureDetail = TEXT("Required source tiles do not fit the decoded image memory budget.");
                return false;
            }
            if (FDecodedSourceTile* Victim = DecodedTiles.Find(OldestKey))
                DecodedBytes -= static_cast<uint64>(Victim->Pixels.Num());
            DecodedTiles.Remove(OldestKey);
        }
    }

    TArray<uint8> Decoded;
    FString DecodeError;
    if (!DecodeSourceTile(Response, CancellationValue, Decoded, DecodeError))
    {
        Report.Error = CancellationValue->IsCancelled()
            ? ImageryAcquisitionError::Cancelled
            : (DecodeError.Contains(TEXT("media type"))
                ? ImageryAcquisitionError::UnsupportedSourceEncoding
                : ImageryAcquisitionError::InvalidSourceImage);
        Report.SourceTileX = X;
        Report.SourceTileY = Y;
        Report.HttpStatus = Response.HttpStatus;
        Report.FailureDetail = MoveTemp(DecodeError);
        return false;
    }
    FDecodedSourceTile Tile;
    Tile.Pixels = MoveTemp(Decoded);
    Tile.LastUse = Report.Requests + static_cast<uint64>(DecodedTiles.Num()) + 1ULL;
    DecodedBytes += static_cast<uint64>(Tile.Pixels.Num());
    DecodedTiles.Add(Key, MoveTemp(Tile));
    PinnedKeys.Add(Key);
    return true;
}

bool RunImageryAcquisition(const SkiDomain::TerrainCoreManifest& TerrainCore,
    IAcquisitionTransport& Transport, IImageryPyramidAssetWriter& AssetWriter,
    const TSharedRef<Cancellation>& CancellationValue,
    ImageryAcquisitionReport& OutReport, const ImageryAcquisitionBudget& Budget)
{
    OutReport = {};
    const double BeganSeconds = FPlatformTime::Seconds();
    ImageryAcquisitionReport Report;
    const auto Fail = [&](const ImageryAcquisitionError Error, const TCHAR* Detail)
    {
        Report.Error = Error;
        if (Report.FailureDetail.IsEmpty()) Report.FailureDetail = Detail;
        Report.ElapsedSeconds = FMath::Max(0.0, FPlatformTime::Seconds() - BeganSeconds);
        OutReport = MoveTemp(Report);
        return false;
    };

    if (!ValidBudget(Budget))
        return Fail(ImageryAcquisitionError::InvalidBudget,
            TEXT("Imagery acquisition budget is outside the supported hard limits."));
    Report.BoundedWorkingMemoryBytes = Budget.MaximumDecodedSourceBytes
        + Budget.MaximumSourceTileBytes + SourceTileDecodedBytes
        + MaximumMappingScratchBytes + MaximumOutputRawBytes
        + 2ULL * Budget.MaximumOutputTileBytes + MaximumCodecScratchBytes
        + MaximumManifestScratchBytes;
    if (CancellationValue->IsCancelled())
        return Fail(ImageryAcquisitionError::Cancelled, TEXT("Imagery acquisition was already cancelled."));

    uint64 RequiredTileCount = 0;
    if (!CountRequiredTiles(TerrainCore, RequiredTileCount))
        return Fail(ImageryAcquisitionError::InvalidTerrainCore,
            TEXT("TerrainCore dimensions cannot produce a bounded imagery tile plan."));
    if (RequiredTileCount > Budget.MaximumPyramidTiles)
        return Fail(ImageryAcquisitionError::TooManyPyramidTiles,
            TEXT("TerrainCore imagery pyramid exceeds the bounded acquisition tile limit."));

    const ImagerySourceMetadata Source = MakeUsgsImageryOnlyMetadata();
    ImageryPyramidManifest Candidate;
    const ImageryPyramidValidation Layout = BuildRequiredImageryPyramid(TerrainCore,
        Source, Candidate, &CancellationValue.Get());
    if (!Layout.Ok())
    {
        return Fail(Layout.Error == ImageryPyramidError::Cancelled
                ? ImageryAcquisitionError::Cancelled : ImageryAcquisitionError::InvalidTerrainCore,
            TEXT("TerrainCore could not produce the required imagery pyramid layout."));
    }

    SkiDomain::LocalFrame LocalFrame;
    if (!SkiDomain::TryMakeLocalFrame(TerrainCore.LocalOrigin, LocalFrame))
        return Fail(ImageryAcquisitionError::InvalidTerrainCore,
            TEXT("TerrainCore local origin cannot be used for imagery projection."));

    TMap<uint64, FDecodedSourceTile> DecodedTiles;
    DecodedTiles.Reserve(static_cast<int32>(Budget.MaximumDecodedSourceBytes
        / SourceTileDecodedBytes));
    uint64 DecodedBytes = 0;
    uint64 LruClock = 0;

    for (int32 TileIndex = 0; TileIndex < Candidate.Tiles.Num(); ++TileIndex)
    {
        if (CancellationValue->IsCancelled())
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::Cancelled,
                TEXT("Imagery acquisition was cancelled between output tiles."));
        }
        if (FPlatformTime::Seconds() - BeganSeconds >= Budget.MaximumElapsedSeconds)
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::DeadlineExceeded,
                TEXT("Imagery acquisition exceeded its elapsed-time budget."));
        }

        const ImageryPyramidTile& OutputTile = Candidate.Tiles[TileIndex];
        TArray<FSourcePixelPosition> Mapping;
        Mapping.SetNumUninitialized(static_cast<int32>(MaximumOutputPixels));
        TSet<uint64> NeededSet;
        for (uint32 Row = 0; Row < ImageryPyramidTilePixels; ++Row)
        {
            if (CancellationValue->IsCancelled())
            {
                Report.TileIndex = TileIndex;
                return Fail(ImageryAcquisitionError::Cancelled,
                    TEXT("Imagery acquisition was cancelled while projecting an output tile."));
            }
            if (FPlatformTime::Seconds() - BeganSeconds >= Budget.MaximumElapsedSeconds)
            {
                Report.TileIndex = TileIndex;
                return Fail(ImageryAcquisitionError::DeadlineExceeded,
                    TEXT("Imagery acquisition exceeded its elapsed-time budget while projecting."));
            }
            const double NorthM = OutputTile.RasterSampleCenterBounds.NorthM
                - static_cast<double>(Row) * OutputTile.MetersPerPixelNorth;
            for (uint32 Column = 0; Column < ImageryPyramidTilePixels; ++Column)
            {
                const double EastM = OutputTile.RasterSampleCenterBounds.WestM
                    + static_cast<double>(Column) * OutputTile.MetersPerPixelEast;
                // The TerrainCore layout pads the last image tile to 256 pixels.
                // Repeat the last in-footprint sample for that padding so this
                // fixed raster extent never causes an off-site imagery prefetch.
                const double InFootprintEastM = std::clamp(EastM,
                    TerrainCore.SampleCenterBounds.WestM,
                    TerrainCore.SampleCenterBounds.EastM);
                const double InFootprintNorthM = std::clamp(NorthM,
                    TerrainCore.SampleCenterBounds.SouthM,
                    TerrainCore.SampleCenterBounds.NorthM);
                FSourcePixelPosition& Position = Mapping[static_cast<int32>(
                    Row * ImageryPyramidTilePixels + Column)];
                if (!ProjectTerrainCoreToWebMercator(LocalFrame,
                        InFootprintEastM, InFootprintNorthM, Position))
                {
                    Report.TileIndex = TileIndex;
                    return Fail(ImageryAcquisitionError::UnsupportedCoordinate,
                        TEXT("TerrainCore output tile includes a coordinate outside USGS Web Mercator imagery coverage."));
                }
                const int32 X0 = FMath::FloorToInt(Position.X);
                const int32 Y0 = FMath::FloorToInt(Position.Y);
                const int32 X1 = FMath::Min(X0 + 1, (1 << (SourceZoom + 8U)) - 1);
                const int32 Y1 = FMath::Min(Y0 + 1, (1 << (SourceZoom + 8U)) - 1);
                NeededSet.Add(SourceTileKey(static_cast<uint32>(X0) / SourceTilePixels,
                    static_cast<uint32>(Y0) / SourceTilePixels));
                NeededSet.Add(SourceTileKey(static_cast<uint32>(X1) / SourceTilePixels,
                    static_cast<uint32>(Y0) / SourceTilePixels));
                NeededSet.Add(SourceTileKey(static_cast<uint32>(X0) / SourceTilePixels,
                    static_cast<uint32>(Y1) / SourceTilePixels));
                NeededSet.Add(SourceTileKey(static_cast<uint32>(X1) / SourceTilePixels,
                    static_cast<uint32>(Y1) / SourceTilePixels));
            }
        }
        if (static_cast<uint32>(NeededSet.Num()) > Budget.MaximumSourceTilesPerOutputTile)
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::RequestBudgetExceeded,
                TEXT("One output image tile intersects too many USGS source tiles."));
        }
        const uint64 RequiredDecodedBytes = static_cast<uint64>(NeededSet.Num())
            * SourceTileDecodedBytes;
        if (RequiredDecodedBytes > Budget.MaximumDecodedSourceBytes)
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::DecodedMemoryBudgetExceeded,
                TEXT("Source tiles needed for one output image exceed the decoded memory budget."));
        }

        TArray<uint64> NeededKeys = NeededSet.Array();
        NeededKeys.Sort();
        TSet<uint64> PinnedKeys;
        for (const uint64 Key : NeededKeys)
        {
            ++LruClock;
            if (FDecodedSourceTile* Cached = DecodedTiles.Find(Key))
            {
                Cached->LastUse = LruClock;
                PinnedKeys.Add(Key);
                continue;
            }
            const uint32 X = SourceTileX(Key);
            const uint32 Y = SourceTileY(Key);
            if (!FetchSourceTile(Transport, X, Y, CancellationValue, Budget,
                    BeganSeconds, Report, DecodedTiles, DecodedBytes, PinnedKeys))
            {
                Report.TileIndex = TileIndex;
                return Fail(Report.Error, TEXT("A required USGS source tile could not be acquired."));
            }
            if (FDecodedSourceTile* Added = DecodedTiles.Find(Key)) Added->LastUse = LruClock;
        }

        TArray<uint8> OutputBgra;
        OutputBgra.SetNumUninitialized(static_cast<int32>(SourceTileDecodedBytes));
        uint64 TileNoDataSamples = 0;
        for (uint32 Row = 0; Row < ImageryPyramidTilePixels; ++Row)
        {
            if (CancellationValue->IsCancelled())
            {
                Report.TileIndex = TileIndex;
                return Fail(ImageryAcquisitionError::Cancelled,
                    TEXT("Imagery acquisition was cancelled while resampling an output tile."));
            }
            if (FPlatformTime::Seconds() - BeganSeconds >= Budget.MaximumElapsedSeconds)
            {
                Report.TileIndex = TileIndex;
                return Fail(ImageryAcquisitionError::DeadlineExceeded,
                    TEXT("Imagery acquisition exceeded its elapsed-time budget while resampling."));
            }
            for (uint32 Column = 0; Column < ImageryPyramidTilePixels; ++Column)
            {
                const int32 PixelIndex = static_cast<int32>(Row * ImageryPyramidTilePixels + Column);
                FSampledColor Color;
                if (!SampleBilinear(Mapping[PixelIndex], DecodedTiles, Color))
                {
                    Report.TileIndex = TileIndex;
                    return Fail(ImageryAcquisitionError::InvalidSourceImage,
                        TEXT("A projected sample could not be found in the acquired source tiles."));
                }
                if (Color.Alpha <= 1.0e-12)
                {
                    ++TileNoDataSamples;
                    Color = {};
                }
                const int32 Offset = PixelIndex * 4;
                OutputBgra[Offset] = Color.Blue;
                OutputBgra[Offset + 1] = Color.Green;
                OutputBgra[Offset + 2] = Color.Red;
                OutputBgra[Offset + 3] = 255;
            }
        }
        if (TileNoDataSamples != 0)
        {
            Report.TileIndex = TileIndex;
            Report.NoDataSamples += TileNoDataSamples;
            return Fail(ImageryAcquisitionError::NoDataInOutputTile,
                TEXT("USGS source imagery contains uncovered pixels inside a required TerrainCore imagery tile."));
        }

        TArray<uint8> Encoded;
        FString EncodingError;
        if (!EncodeOutputJpeg(OutputBgra, Budget.MaximumOutputTileBytes, Encoded,
                EncodingError))
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::OutputEncodingFailed,
                *EncodingError);
        }
        if (static_cast<uint64>(Encoded.Num()) > Budget.MaximumOutputBytes - Report.OutputBytes)
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::OutputBudgetExceeded,
                TEXT("Encoded imagery pyramid exceeds its output byte budget."));
        }
        if (CancellationValue->IsCancelled())
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::Cancelled,
                TEXT("Imagery acquisition was cancelled before writing an output tile."));
        }
        const FString Hash = Sha256(TArrayView<const uint8>(Encoded.GetData(), Encoded.Num()));
        if (Hash.IsEmpty())
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::InvalidOutputManifest,
                TEXT("SHA-256 could not be computed for a generated imagery tile."));
        }

        FString WriteError;
        if (!AssetWriter.Write(OutputTile.Path,
                TArrayView<const uint8>(Encoded.GetData(), Encoded.Num()),
                CancellationValue.Get(), WriteError))
        {
            Report.TileIndex = TileIndex;
            return Fail(ImageryAcquisitionError::AssetWriteFailed,
                WriteError.IsEmpty() ? TEXT("Imagery tile staging write failed.") : *WriteError);
        }

        ImageryPyramidTile& CompletedTile = Candidate.Tiles[TileIndex];
        CompletedTile.Sha256 = TCHAR_TO_UTF8(*Hash);
        CompletedTile.Bytes = static_cast<uint64>(Encoded.Num());
        Report.OutputBytes += CompletedTile.Bytes;
        for (const uint64 Key : NeededKeys)
        {
            if (FDecodedSourceTile* Used = DecodedTiles.Find(Key)) Used->LastUse = ++LruClock;
        }
    }

    if (CancellationValue->IsCancelled())
        return Fail(ImageryAcquisitionError::Cancelled,
            TEXT("Imagery acquisition was cancelled before manifest verification."));
    if (!ValidateImageryPyramidManifest(TerrainCore, Candidate).Ok())
        return Fail(ImageryAcquisitionError::InvalidOutputManifest,
            TEXT("Generated imagery assets do not satisfy the TerrainCore pyramid contract."));

    Report.Manifest = MoveTemp(Candidate);
    Report.ElapsedSeconds = FMath::Max(0.0, FPlatformTime::Seconds() - BeganSeconds);
    OutReport = MoveTemp(Report);
    return true;
}
}

SkiPreparation::ImagerySourceMetadata SkiPreparation::MakeUsgsImageryOnlyMetadata()
{
    ImagerySourceMetadata Source;
    Source.SourceId = "usgs-imagery-only";
    Source.Product = "USGSImageryOnly";
    Source.ServiceUrlTemplate = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{y}/{x}";
    Source.License = "Public domain, U.S. Government work";
    Source.Attribution = "USGS The National Map";
    Source.TermsUrl = "https://www.usgs.gov/information-policies-and-instructions/copyrights-and-credits";
    return Source;
}

bool SkiPreparation::AcquireUsgsImageryPyramid(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    IImageryPyramidAssetWriter& AssetWriter,
    const TSharedRef<Cancellation>& CancellationValue,
    ImageryAcquisitionReport& OutReport,
    const ImageryAcquisitionBudget& Budget)
{
    SkiNetGateway Gateway;
    return RunImageryAcquisition(TerrainCore, Gateway, AssetWriter,
        CancellationValue, OutReport, Budget);
}

#if !UE_BUILD_SHIPPING && WITH_DEV_AUTOMATION_TESTS
bool SkiPreparation::AcquireUsgsImageryPyramidForTest(
    const SkiDomain::TerrainCoreManifest& TerrainCore,
    IAcquisitionTransport& ScriptedTransport,
    IImageryPyramidAssetWriter& AssetWriter,
    const TSharedRef<Cancellation>& CancellationValue,
    ImageryAcquisitionReport& OutReport,
    const ImageryAcquisitionBudget& Budget)
{
    return RunImageryAcquisition(TerrainCore, ScriptedTransport, AssetWriter,
        CancellationValue, OutReport, Budget);
}
#endif
