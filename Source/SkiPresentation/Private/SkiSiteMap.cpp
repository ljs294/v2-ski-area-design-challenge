#include "SkiSiteMap.h"
#include "SSkiSiteMap.h"

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SlateRenderer.h"
#include "Fonts/FontMeasure.h"
#include "SkiDomain/TerrainPackage.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "Engine/Texture2D.h"
#include "Styling/CoreStyle.h"

#include <cfloat>
#include <cmath>

double SkiSiteMapMath::WrapWorldPixelX(const double X, const double WorldPixels)
{
    if (!FMath::IsFinite(X) || !FMath::IsFinite(WorldPixels) || WorldPixels <= 0.0)
        return X;
    const double Wrapped = FMath::Fmod(X, WorldPixels);
    return Wrapped < 0.0 ? Wrapped + WorldPixels : Wrapped;
}

double SkiSiteMapMath::UnwrapWorldPixelXNear(const double X, const double ReferenceX,
    const double WorldPixels)
{
    if (!FMath::IsFinite(X) || !FMath::IsFinite(ReferenceX)
        || !FMath::IsFinite(WorldPixels) || WorldPixels <= 0.0) return X;
    const double HalfWorld = WorldPixels * 0.5;
    const double Delta = WrapWorldPixelX(X - ReferenceX + HalfWorld, WorldPixels)
        - HalfWorld;
    return ReferenceX + Delta;
}

int32 SkiSiteMapMath::WrapWorldTileX(const int32 X, const int32 WorldTileCount)
{
    if (WorldTileCount <= 0) return X;
    const int32 Wrapped = X % WorldTileCount;
    return Wrapped < 0 ? Wrapped + WorldTileCount : Wrapped;
}

namespace
{
constexpr int32 TilePixels = 256;
constexpr int32 MaxCachedTiles = 128;
constexpr int32 MaxPendingRequests = 8;
constexpr int32 MaxRequestsPerTick = 3;
constexpr int32 MaxCompletedTilesPerTick = 4;
constexpr int32 MaxContourSegmentsPerTile = 12000;
constexpr int32 MinMapZoom = 0;
constexpr int32 MaxMapZoom = 16;
constexpr double Pi = 3.14159265358979323846;
constexpr double ContourIntervalM = 12.192; // 40 ft, with every fifth contour emphasized.
constexpr double MaxMercatorLatitudeDeg = 85.0511287798066;
constexpr double MetersPerFoot = 0.3048;
constexpr double DefaultFitPaddingPx = 48.0;
std::atomic<IImageWrapperModule*> GImageWrapperModule{nullptr};

struct FDecodedTileResult
{
    int32 X = 0;
    int32 Y = 0;
    uint8 Zoom = 0;
    uint8 Layer = 0;
    uint64 Epoch = 0;
    bool bSucceeded = false;
    FString Failure;
    TArray<FColor> Pixels;
    TArray<FSkiMapContourSegment> Contours;
};

struct FCompletedPreview
{
    uint64 Generation = 0;
    FString Summary;
};

bool IsValidFitRequest(const SkiDomain::GeographicBounds& Bounds, const double PaddingPx)
{
    return FMath::IsFinite(Bounds.WestDeg) && FMath::IsFinite(Bounds.SouthDeg)
        && FMath::IsFinite(Bounds.EastDeg) && FMath::IsFinite(Bounds.NorthDeg)
        && FMath::IsFinite(PaddingPx) && PaddingPx >= 0.0
        && Bounds.WestDeg >= -180.0 && Bounds.WestDeg <= 180.0
        && Bounds.EastDeg >= -180.0 && Bounds.EastDeg <= 180.0
        && Bounds.SouthDeg >= -90.0 && Bounds.SouthDeg <= Bounds.NorthDeg
        && Bounds.NorthDeg <= 90.0;
}

bool DecodeTileImage(const TArray<uint8>& Compressed, const EImageFormat Format,
    TArray<FColor>& OutPixels, FString& OutError)
{
    OutPixels.Reset();
    if (Compressed.IsEmpty())
    {
        OutError = TEXT("The map service returned an empty tile.");
        return false;
    }

    IImageWrapperModule* ImageWrapperModule = GImageWrapperModule.load(std::memory_order_acquire);
    if (!ImageWrapperModule)
    {
        OutError = TEXT("The image decoder is not available.");
        return false;
    }
    const TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule->CreateImageWrapper(Format);
    if (!ImageWrapper.IsValid()
        || !ImageWrapper->SetCompressed(Compressed.GetData(), Compressed.Num())
        || ImageWrapper->GetWidth() != TilePixels || ImageWrapper->GetHeight() != TilePixels)
    {
        OutError = TEXT("The map service returned a tile with an unsupported image layout.");
        return false;
    }

    TArray<uint8> RawBytes;
    if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawBytes)
        || RawBytes.Num() != TilePixels * TilePixels * sizeof(FColor))
    {
        OutError = TEXT("The map tile could not be decoded to a 256 by 256 image.");
        return false;
    }
    OutPixels.SetNumUninitialized(TilePixels * TilePixels);
    FMemory::Memcpy(OutPixels.GetData(), RawBytes.GetData(), RawBytes.Num());
    return true;
}

float TerrariumElevationM(const FColor& Pixel)
{
    // FColor stores BGRA on the byte path requested from ImageWrapper.
    return static_cast<float>(Pixel.R) * 256.0f + static_cast<float>(Pixel.G)
        + static_cast<float>(Pixel.B) / 256.0f - 32768.0f;
}

template<typename AllocatorType>
void AddCrossing(const float A, const float B, const FVector2f PointA,
    const FVector2f PointB, const float Level, TArray<FVector2f, AllocatorType>& OutPoints)
{
    if (!((A < Level && B >= Level) || (B < Level && A >= Level))) return;
    const float Denominator = B - A;
    if (FMath::IsNearlyZero(Denominator)) return;
    const float T = FMath::Clamp((Level - A) / Denominator, 0.0f, 1.0f);
    OutPoints.Add(PointA + (PointB - PointA) * T);
}

void BuildTileContours(const TArray<FColor>& Pixels,
    TArray<FSkiMapContourSegment>& OutSegments)
{
    OutSegments.Reset();
    if (Pixels.Num() != TilePixels * TilePixels) return;

    constexpr int32 SampleStep = 2;
    for (int32 Y = 0; Y < TilePixels - SampleStep; Y += SampleStep)
    {
        for (int32 X = 0; X < TilePixels - SampleStep; X += SampleStep)
        {
            const float H00 = TerrariumElevationM(Pixels[Y * TilePixels + X]);
            const float H10 = TerrariumElevationM(Pixels[Y * TilePixels + X + SampleStep]);
            const float H11 = TerrariumElevationM(Pixels[(Y + SampleStep) * TilePixels + X + SampleStep]);
            const float H01 = TerrariumElevationM(Pixels[(Y + SampleStep) * TilePixels + X]);
            const float Minimum = FMath::Min(FMath::Min(H00, H10), FMath::Min(H11, H01));
            const float Maximum = FMath::Max(FMath::Max(H00, H10), FMath::Max(H11, H01));
            const int32 FirstLevel = FMath::CeilToInt(Minimum / ContourIntervalM);
            const int32 LastLevel = FMath::FloorToInt(Maximum / ContourIntervalM);
            for (int32 LevelIndex = FirstLevel; LevelIndex <= LastLevel; ++LevelIndex)
            {
                if (OutSegments.Num() >= MaxContourSegmentsPerTile) return;
                const float LevelM = static_cast<float>(LevelIndex * ContourIntervalM);
                TArray<FVector2f, TInlineAllocator<4>> Crossings;
                const FVector2f P00(static_cast<float>(X), static_cast<float>(Y));
                const FVector2f P10(static_cast<float>(X + SampleStep), static_cast<float>(Y));
                const FVector2f P11(static_cast<float>(X + SampleStep), static_cast<float>(Y + SampleStep));
                const FVector2f P01(static_cast<float>(X), static_cast<float>(Y + SampleStep));
                AddCrossing(H00, H10, P00, P10, LevelM, Crossings);
                AddCrossing(H10, H11, P10, P11, LevelM, Crossings);
                AddCrossing(H11, H01, P11, P01, LevelM, Crossings);
                AddCrossing(H01, H00, P01, P00, LevelM, Crossings);
                for (int32 Index = 0; Index + 1 < Crossings.Num(); Index += 2)
                {
                    FSkiMapContourSegment& Segment = OutSegments.AddDefaulted_GetRef();
                    Segment.Start = Crossings[Index];
                    Segment.End = Crossings[Index + 1];
                    Segment.ElevationMeters = LevelM;
                    Segment.bIndex = FMath::Abs(LevelIndex % 5) == 0;
                }
            }
        }
    }
}

bool DecodeCompressedTile(const FDecodedTileResult& RequestResult,
    const TArray<uint8>& Compressed, FDecodedTileResult& OutResult)
{
    OutResult = RequestResult;
    const EImageFormat Format = static_cast<SSkiSiteMap::ETileLayer>(RequestResult.Layer)
        == SSkiSiteMap::ETileLayer::Imagery ? EImageFormat::JPEG : EImageFormat::PNG;
    if (!DecodeTileImage(Compressed, Format, OutResult.Pixels, OutResult.Failure)) return false;
    if (Format == EImageFormat::PNG) BuildTileContours(OutResult.Pixels, OutResult.Contours);
    OutResult.X = RequestResult.X;
    OutResult.Y = RequestResult.Y;
    OutResult.Zoom = RequestResult.Zoom;
    OutResult.Layer = RequestResult.Layer;
    OutResult.Epoch = RequestResult.Epoch;
    OutResult.bSucceeded = true;
    return true;
}

FVector2f ToVector2f(const FVector2D& Value)
{
    return FVector2f(static_cast<float>(Value.X), static_cast<float>(Value.Y));
}
}

struct FSkiSiteMapAsyncState
{
    std::atomic<bool> bActive{false};
    std::atomic<uint64> Epoch{1};
    std::atomic<uint64> LatestPreviewGeneration{0};
    FCriticalSection Mutex;
    TArray<FDecodedTileResult> CompletedTiles;
    TOptional<FCompletedPreview> CompletedPreview;
};

void SSkiSiteMap::Construct(const FArguments& Arguments)
{
    GImageWrapperModule.store(&FModuleManager::Get().LoadModuleChecked<IImageWrapperModule>(
        TEXT("ImageWrapper")), std::memory_order_release);
    PreviewStatus = Arguments._OnPreviewStatus;
    SelectionStatus = Arguments._OnSelectionStatus;
    AsyncState = MakeShared<FSkiSiteMapAsyncState, ESPMode::ThreadSafe>();
    SkiDomain::GeodeticPoint Origin{SiteOriginLatitudeDeg, SiteOriginLongitudeDeg, 0.0};
    SkiDomain::TryMakeLocalFrame(Origin, SiteFrame);
    SetCanTick(true);
    SetVisibility(EVisibility::Visible);
}

SSkiSiteMap::~SSkiSiteMap()
{
    CancelRequestsAndPreview();
    TileCache.Reset();
}

void SSkiSiteMap::SetPickerActive(const bool bActive)
{
    if (bPickerActive == bActive) return;
    bPickerActive = bActive;
    if (!bActive)
    {
        CancelRequestsAndPreview();
        TileCache.Reset();
        FailedRequests.Reset();
        VisibleTiles.Reset();
        DragMode = EDragMode::None;
        bBoundaryEditingEnabled = false;
        bPreviewDebouncePending = false;
        NotifyPreviewStatus(TEXT("Site preview paused."));
    }
    else
    {
        AsyncState->bActive.store(true, std::memory_order_release);
        AsyncState->Epoch.fetch_add(1, std::memory_order_acq_rel);
        Invalidate(EInvalidateWidgetReason::Paint);
    }
}

void SSkiSiteMap::SetBoundaryEditingEnabled(const bool bEnabled)
{
    bBoundaryEditingEnabled = bPickerActive && bEnabled;
    if (!bBoundaryEditingEnabled) DragMode = EDragMode::None;
}

void SSkiSiteMap::SetNetworkEnabled(const bool bEnabled)
{
    if (bNetworkEnabled == bEnabled) return;
    bNetworkEnabled = bEnabled;
    if (!bEnabled)
    {
        CancelRequestsAndPreview();
        PendingRequests.Reset();
        VisibleTiles.Reset();
    }
    else if (bPickerActive)
    {
        AsyncState->bActive.store(true, std::memory_order_release);
        AsyncState->Epoch.fetch_add(1, std::memory_order_acq_rel);
    }
}

void SSkiSiteMap::CancelRequestsAndPreview()
{
    if (!AsyncState.IsValid()) return;
    AsyncState->bActive.store(false, std::memory_order_release);
    AsyncState->Epoch.fetch_add(1, std::memory_order_acq_rel);
    for (TPair<FTileKey, TSharedPtr<SkiPreparation::Cancellation>>& Pair
        : PendingRequests)
    {
        if (Pair.Value.IsValid()) Pair.Value->Cancel();
    }
    PendingRequests.Reset();
    if (ActivePreview.bCancelled.IsValid()) ActivePreview.bCancelled->store(true, std::memory_order_release);
    ActivePreview = {};
    ++PreviewGeneration;
    AsyncState->LatestPreviewGeneration.store(PreviewGeneration, std::memory_order_release);
    {
        FScopeLock Lock(&AsyncState->Mutex);
        AsyncState->CompletedTiles.Reset();
        AsyncState->CompletedPreview.Reset();
    }
}

void SSkiSiteMap::SetCenter(const double LatitudeDeg, const double LongitudeDeg)
{
    SkiDomain::WebMercatorPixel Pixel;
    if (!SkiDomain::TryWebMercatorPixel(LatitudeDeg, LongitudeDeg,
            static_cast<uint8>(FMath::FloorToInt(ZoomLevel)), Pixel)) return;
    bFitPending = false;
    CenterLatitudeDeg = LatitudeDeg;
    CenterLongitudeDeg = LongitudeDeg;
    SiteOriginLatitudeDeg = LatitudeDeg;
    SiteOriginLongitudeDeg = LongitudeDeg;
    SkiDomain::TryMakeLocalFrame({LatitudeDeg, LongitudeDeg, 0.0}, SiteFrame);
    ClearSelection();
    Invalidate(EInvalidateWidgetReason::Paint);
}

bool SSkiSiteMap::FitToBounds(const SkiDomain::GeographicBounds& Bounds,
    const double PaddingPx)
{
    if (!IsValidFitRequest(Bounds, PaddingPx)) return false;
    PendingFitBounds = Bounds;
    PendingFitPaddingPx = PaddingPx;
    bFitPending = true;
    Invalidate(EInvalidateWidgetReason::Paint);
    return true;
}

bool SSkiSiteMap::ApplyPendingFit(const FVector2D& ViewportSize)
{
    if (!bFitPending) return false;
    if (!FMath::IsFinite(ViewportSize.X) || !FMath::IsFinite(ViewportSize.Y)
        || ViewportSize.X <= 1.0 || ViewportSize.Y <= 1.0) return false;

    SkiDomain::WebMercatorPixel SouthWest, NorthEast;
    const double SouthLatitude = FMath::Clamp(PendingFitBounds.SouthDeg,
        -MaxMercatorLatitudeDeg, MaxMercatorLatitudeDeg);
    const double NorthLatitude = FMath::Clamp(PendingFitBounds.NorthDeg,
        -MaxMercatorLatitudeDeg, MaxMercatorLatitudeDeg);
    if (!SkiDomain::TryWebMercatorPixel(SouthLatitude, PendingFitBounds.WestDeg,
            static_cast<uint8>(MaxMapZoom), SouthWest)
        || !SkiDomain::TryWebMercatorPixel(NorthLatitude, PendingFitBounds.EastDeg,
            static_cast<uint8>(MaxMapZoom), NorthEast))
    {
        bFitPending = false;
        return false;
    }

    const double WorldPixels = std::ldexp(static_cast<double>(TilePixels), MaxMapZoom);
    double EastX = NorthEast.X;
    if (EastX < SouthWest.X) EastX += WorldPixels; // Geographic bounds may cross the dateline.
    const double BoundsWidthPx = FMath::Max(1.0, EastX - SouthWest.X);
    const double BoundsHeightPx = FMath::Max(1.0, FMath::Abs(SouthWest.Y - NorthEast.Y));
    const double MaxPadding = FMath::Max(0.0,
        FMath::Min(ViewportSize.X, ViewportSize.Y) * 0.5 - 0.5);
    const double Padding = FMath::Clamp(PendingFitPaddingPx, 0.0, MaxPadding);
    const double AvailableWidth = FMath::Max(1.0, ViewportSize.X - Padding * 2.0);
    const double AvailableHeight = FMath::Max(1.0, ViewportSize.Y - Padding * 2.0);
    const double ScaleToFit = FMath::Min(AvailableWidth / BoundsWidthPx,
        AvailableHeight / BoundsHeightPx);
    if (!FMath::IsFinite(ScaleToFit) || ScaleToFit <= 0.0)
    {
        bFitPending = false;
        return false;
    }

    const double CenterXUnwrapped = (SouthWest.X + EastX) * 0.5;
    const double CenterX = SkiSiteMapMath::WrapWorldPixelX(CenterXUnwrapped, WorldPixels);
    const double CenterY = (SouthWest.Y + NorthEast.Y) * 0.5;
    double NewCenterLatitude = 0.0, NewCenterLongitude = 0.0;
    if (!SkiDomain::TryGeodeticFromWebMercatorPixel({CenterX, CenterY},
            static_cast<uint8>(MaxMapZoom), NewCenterLatitude, NewCenterLongitude))
    {
        bFitPending = false;
        return false;
    }

    const double NewZoomLevel = FMath::Clamp(
        MaxMapZoom + std::log2(ScaleToFit), static_cast<double>(MinMapZoom),
        static_cast<double>(MaxMapZoom));
    SkiDomain::LocalFrame NewSiteFrame;
    if (!SkiDomain::TryMakeLocalFrame(
            {NewCenterLatitude, NewCenterLongitude, 0.0}, NewSiteFrame))
    {
        bFitPending = false;
        return false;
    }

    bFitPending = false;
    CenterLatitudeDeg = NewCenterLatitude;
    CenterLongitudeDeg = NewCenterLongitude;
    SiteOriginLatitudeDeg = NewCenterLatitude;
    SiteOriginLongitudeDeg = NewCenterLongitude;
    SiteFrame = NewSiteFrame;
    ZoomLevel = NewZoomLevel;
    ClearSelection();
    Invalidate(EInvalidateWidgetReason::Paint);
    return true;
}

void SSkiSiteMap::SetContoursEnabled(const bool bEnabled)
{
    if (bContoursEnabled == bEnabled) return;
    bContoursEnabled = bEnabled;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SSkiSiteMap::SetContourUnits(const bool bMetric)
{
    if (bMetricContourLabels == bMetric) return;
    bMetricContourLabels = bMetric;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SSkiSiteMap::ClearSelection()
{
    bHasSelection = false;
    Selection = {};
    UpdateSelection({}, false);
}

bool SSkiSiteMap::CreateViewportSmokeBoundary(const FGuid& SmokeToken)
{
#if UE_BUILD_DEVELOPMENT
    FString LaunchTokenText;
    FGuid LaunchToken;
    if (!bPickerActive || !bBoundaryEditingEnabled || HasValidSelection() || !SmokeToken.IsValid()
        || !FParse::Param(FCommandLine::Get(), TEXT("SkiP1PickerViewportSmoke"))
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), LaunchTokenText)
        || !FGuid::Parse(LaunchTokenText, LaunchToken) || LaunchToken != SmokeToken)
        return false;

    SkiDomain::SiteRectangleM SmokeRectangle;
    if (!SkiDomain::TryDrawSiteRectangle(0.0, 0.0, 2000.0, 2000.0, SmokeRectangle)
        || !SkiDomain::IsValidSiteRectangle(SmokeRectangle, 4000.0))
        return false;

    // Use the same commit path as a map drag so status and preview listeners see the result.
    UpdateSelection(SmokeRectangle, true);
    return HasValidSelection();
#else
    (void)SmokeToken;
    return false;
#endif
}

bool SSkiSiteMap::HasValidSelection() const
{
    return bHasSelection && SkiDomain::IsValidSiteRectangle(Selection);
}

bool SSkiSiteMap::TryGetSelectedBounds(SkiDomain::GeographicBounds& OutBounds) const
{
    OutBounds = {};
    return HasValidSelection() && SiteRectangleToBounds(Selection, OutBounds);
}

bool SSkiSiteMap::SiteRectangleToBounds(const SkiDomain::SiteRectangleM& Rectangle,
    SkiDomain::GeographicBounds& OutBounds) const
{
    OutBounds = {};
    if (!SkiDomain::IsValidSiteRectangle(Rectangle)) return false;
    const double Easts[] = {Rectangle.WestM, Rectangle.WestM, Rectangle.EastM, Rectangle.EastM};
    const double Norths[] = {Rectangle.SouthM, Rectangle.NorthM, Rectangle.SouthM, Rectangle.NorthM};
    double West = DBL_MAX, South = DBL_MAX, East = -DBL_MAX, North = -DBL_MAX;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Easts); ++Index)
    {
        SkiDomain::GeodeticPoint Point;
        if (!SkiDomain::TrySeaLevelGeodeticFromEnu(SiteFrame, Easts[Index], Norths[Index], Point))
            return false;
        West = FMath::Min(West, Point.LongitudeDeg);
        East = FMath::Max(East, Point.LongitudeDeg);
        South = FMath::Min(South, Point.LatitudeDeg);
        North = FMath::Max(North, Point.LatitudeDeg);
    }
    OutBounds = {West, South, East, North};
    return true;
}

void SSkiSiteMap::UpdateSelection(const SkiDomain::SiteRectangleM& NewSelection,
    const bool bValid)
{
    bHasSelection = bValid && SkiDomain::IsValidSiteRectangle(NewSelection);
    Selection = bHasSelection ? NewSelection : SkiDomain::SiteRectangleM{};
    SchedulePreview(true);
    if (bHasSelection)
    {
        const double WidthKm = (Selection.EastM - Selection.WestM) / 1000.0;
        const double HeightKm = (Selection.NorthM - Selection.SouthM) / 1000.0;
        NotifySelectionStatus(FString::Printf(
            TEXT("Boundary %.2f km × %.2f km · %.2f km² · each side 2–4 km"),
            WidthKm, HeightKm, WidthKm * HeightKm));
    }
    else NotifySelectionStatus(TEXT("Drag on the map to draw a 2–4 km boundary."));
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SSkiSiteMap::SchedulePreview(const bool bBoundaryChanged)
{
    if (ActivePreview.bCancelled.IsValid()) ActivePreview.bCancelled->store(true, std::memory_order_release);
    ActivePreview = {};
    ++PreviewGeneration;
    AsyncState->LatestPreviewGeneration.store(PreviewGeneration, std::memory_order_release);
    {
        FScopeLock Lock(&AsyncState->Mutex);
        AsyncState->CompletedPreview.Reset();
    }
    bPreviewDebouncePending = bPickerActive && bNetworkEnabled && bHasSelection;
    PreviewDueAt = FPlatformTime::Seconds() + 0.300;
    if (bBoundaryChanged)
    {
        NotifyPreviewStatus(bHasSelection
            ? TEXT("Boundary changed. Preview updates after 300 ms idle.")
            : TEXT("Preview will appear after a 2–4 km boundary is selected."));
    }
}

void SSkiSiteMap::StartPreview()
{
    bPreviewDebouncePending = false;
    SkiDomain::GeographicBounds Bounds;
    if (!SiteRectangleToBounds(Selection, Bounds)) return;

    ActivePreview.Generation = PreviewGeneration;
    ActivePreview.bCancelled = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Cancellation = ActivePreview.bCancelled;
    const TSharedPtr<FSkiSiteMapAsyncState, ESPMode::ThreadSafe> State = AsyncState;
    const uint64 Generation = ActivePreview.Generation;
    const double WidthKm = (Selection.EastM - Selection.WestM) / 1000.0;
    const double HeightKm = (Selection.NorthM - Selection.SouthM) / 1000.0;
    Async(EAsyncExecution::ThreadPool, [State, Cancellation, Generation, Bounds, WidthKm, HeightKm]
    {
        if (Cancellation->load(std::memory_order_acquire)
            || !State->bActive.load(std::memory_order_acquire)
            || State->LatestPreviewGeneration.load(std::memory_order_acquire) != Generation) return;

        // M2 deliberately stops at a local, generation-fenced catalog stub. M3 supplies coverage,
        // source quality, outlines, and size estimates through this same result path.
        const FString Summary = FString::Printf(
            TEXT("Preview ready · %.2f km × %.2f km · %.2f°–%.2f° N, %.2f°–%.2f° W\n"
                 "USGS coverage, source quality, and size estimates arrive with the M3 catalog."),
            WidthKm, HeightKm, Bounds.SouthDeg, Bounds.NorthDeg,
            FMath::Abs(Bounds.WestDeg), FMath::Abs(Bounds.EastDeg));
        if (Cancellation->load(std::memory_order_acquire)
            || !State->bActive.load(std::memory_order_acquire)
            || State->LatestPreviewGeneration.load(std::memory_order_acquire) != Generation) return;
        FScopeLock Lock(&State->Mutex);
        if (State->LatestPreviewGeneration.load(std::memory_order_relaxed) == Generation)
            State->CompletedPreview = FCompletedPreview{Generation, Summary};
    });
}

void SSkiSiteMap::DrainAsyncResults()
{
    if (!AsyncState.IsValid()) return;
    TArray<FDecodedTileResult> Completed;
    TOptional<FCompletedPreview> CompletedPreview;
    {
        FScopeLock Lock(&AsyncState->Mutex);
        const int32 Count = FMath::Min(MaxCompletedTilesPerTick, AsyncState->CompletedTiles.Num());
        if (Count > 0)
        {
            Completed.Append(AsyncState->CompletedTiles.GetData(), Count);
            AsyncState->CompletedTiles.RemoveAt(0, Count, EAllowShrinking::No);
        }
        if (AsyncState->CompletedPreview.IsSet())
        {
            CompletedPreview = AsyncState->CompletedPreview;
            AsyncState->CompletedPreview.Reset();
        }
    }

    const double Now = FPlatformTime::Seconds();
    for (FDecodedTileResult& Result : Completed)
    {
        const FTileKey Key{Result.X, Result.Y, Result.Zoom, static_cast<ETileLayer>(Result.Layer)};
        PendingRequests.Remove(Key);
        if (!bPickerActive || !bNetworkEnabled || !Result.bSucceeded
            || Result.Epoch != AsyncState->Epoch.load(std::memory_order_acquire))
        {
            if (!Result.bSucceeded && bPickerActive && bNetworkEnabled
                && Result.Epoch == AsyncState->Epoch.load(std::memory_order_acquire))
                FailedRequests.Add(Key, Now + 5.0);
            continue;
        }

        if (Key.Layer == ETileLayer::Imagery)
        {
            if (!UploadImagery(Key, Result.Pixels)) FailedRequests.Add(Key, Now + 5.0);
        }
        else
        {
            FTile Tile;
            Tile.Contours = MoveTemp(Result.Contours);
            Tile.LastFrame = Frame;
            TileCache.Add(Key, MoveTemp(Tile));
        }
    }

    if (CompletedPreview.IsSet() && CompletedPreview->Generation == PreviewGeneration
        && bPickerActive && bNetworkEnabled)
    {
        ActivePreview = {};
        NotifyPreviewStatus(CompletedPreview->Summary);
    }
    TrimTileCache();
}

void SSkiSiteMap::Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime)
{
    SLeafWidget::Tick(Geometry, CurrentTime, DeltaTime);
    ++Frame;
    RequestsStartedThisFrame = 0;
    UploadsStartedThisFrame = 0;
    DrainAsyncResults();
    if (bFitPending && ApplyPendingFit(Geometry.GetLocalSize()))
        Invalidate(EInvalidateWidgetReason::Paint);
    if (!bPickerActive) return;
    if (bPreviewDebouncePending && FPlatformTime::Seconds() >= PreviewDueAt) StartPreview();
    if (bNetworkEnabled) RequestVisibleTiles(Geometry.GetLocalSize());
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SSkiSiteMap::RequestVisibleTiles(const FVector2D& Size)
{
    if (Size.X <= 0.0 || Size.Y <= 0.0 || !bPickerActive || !bNetworkEnabled) return;
    const int32 TileZoom = FMath::Clamp(FMath::FloorToInt(ZoomLevel), MinMapZoom, MaxMapZoom);
    const double FractionalScale = FMath::Pow(2.0, ZoomLevel - TileZoom);
    const int32 WorldTileCount = 1 << TileZoom;
    SkiDomain::WebMercatorPixel Center;
    if (!SkiDomain::TryWebMercatorPixel(CenterLatitudeDeg, CenterLongitudeDeg,
            static_cast<uint8>(TileZoom), Center)) return;

    const double Left = Center.X - Size.X * 0.5 / FractionalScale;
    const double Right = Center.X + Size.X * 0.5 / FractionalScale;
    const double Top = Center.Y - Size.Y * 0.5 / FractionalScale;
    const double Bottom = Center.Y + Size.Y * 0.5 / FractionalScale;
    const int32 MinTileX = FMath::FloorToInt(Left / TilePixels) - 1;
    const int32 MaxTileX = FMath::FloorToInt(Right / TilePixels) + 1;
    const int32 MinTileY = FMath::Max(0, FMath::FloorToInt(Top / TilePixels) - 1);
    const int32 MaxTileY = FMath::Min(static_cast<int32>(WorldTileCount) - 1,
        FMath::FloorToInt(Bottom / TilePixels) + 1);
    VisibleTiles.Reset();
    for (int32 RawY = MinTileY; RawY <= MaxTileY; ++RawY)
    {
        for (int32 RawX = MinTileX; RawX <= MaxTileX; ++RawX)
        {
            const int32 WrappedX = SkiSiteMapMath::WrapWorldTileX(RawX, WorldTileCount);
            const FTileKey ImageryKey{WrappedX, RawY, static_cast<uint8>(TileZoom), ETileLayer::Imagery};
            AddVisibleTile(ImageryKey, VisibleTiles);
            if (bContoursEnabled)
            {
                const FTileKey ElevationKey{WrappedX, RawY, static_cast<uint8>(TileZoom), ETileLayer::Elevation};
                AddVisibleTile(ElevationKey, VisibleTiles);
            }
        }
    }

    for (const FTileKey& Key : VisibleTiles)
    {
        if (RequestsStartedThisFrame >= MaxRequestsPerTick
            || PendingRequests.Num() >= MaxPendingRequests) break;
        RequestTile(Key);
    }
}

void SSkiSiteMap::AddVisibleTile(const FTileKey& Key, TArray<FTileKey>& OutVisible)
{
    if (!OutVisible.Contains(Key)) OutVisible.Add(Key);
    if (FTile* Tile = TileCache.Find(Key)) Tile->LastFrame = Frame;
}

void SSkiSiteMap::RequestTile(const FTileKey& Key)
{
    if (TileCache.Contains(Key) || PendingRequests.Contains(Key)) return;
    if (const double* RetryAt = FailedRequests.Find(Key))
        if (*RetryAt > FPlatformTime::Seconds()) return;
    FailedRequests.Remove(Key);

    const FString Url = Key.Layer == ETileLayer::Imagery
        ? FString::Printf(TEXT("https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/%d/%d/%d"),
            Key.Zoom, Key.Y, Key.X)
        : FString::Printf(TEXT("https://elevation-tiles-prod.s3.amazonaws.com/terrarium/%d/%d/%d.png"),
            Key.Zoom, Key.X, Key.Y);
    auto Cancellation = MakeShared<SkiPreparation::Cancellation>();
    PendingRequests.Add(Key, Cancellation);
    ++RequestsStartedThisFrame;

    const TSharedPtr<FSkiSiteMapAsyncState, ESPMode::ThreadSafe> State = AsyncState;
    const uint64 RequestEpoch = State->Epoch.load(std::memory_order_acquire);
    const FDecodedTileResult TileRequest{Key.X, Key.Y, Key.Zoom,
        static_cast<uint8>(Key.Layer), RequestEpoch, false, FString(), {}, {}};
    Async(EAsyncExecution::ThreadPool, [State, Cancellation, TileRequest, Url]
    {
        SkiPreparation::HttpAcquisitionRequest Request;
        Request.Url = Url;
        Request.Product = SkiPreparation::ProviderProduct::Imagery;
        Request.TotalTimeoutSeconds = 20.0F;
        Request.ActivityTimeoutSeconds = 10.0F;
        Request.MaximumResponseBytes = 4ULL * 1024ULL * 1024ULL;
        SkiPreparation::SkiNetGateway Gateway;
        const SkiPreparation::HttpAcquisitionResult HttpResult = Gateway.Get(
            Request, Cancellation);
        if (!State->bActive.load(std::memory_order_acquire)
            || State->Epoch.load(std::memory_order_acquire) != TileRequest.Epoch
            || Cancellation->IsCancelled()) return;

        FDecodedTileResult Decoded;
        if (!HttpResult.Ok())
        {
            Decoded = TileRequest;
            Decoded.Failure = FString::Printf(TEXT("USGS map tile request failed (%d, %s)."),
                HttpResult.HttpStatus, *HttpResult.RequestStatus);
        }
        else
        {
            const bool bExpectedContentType = static_cast<ETileLayer>(TileRequest.Layer)
                == ETileLayer::Imagery
                ? HttpResult.ContentType.StartsWith(TEXT("image/jpeg"), ESearchCase::IgnoreCase)
                    || HttpResult.ContentType.StartsWith(TEXT("image/jpg"), ESearchCase::IgnoreCase)
                : HttpResult.ContentType.StartsWith(TEXT("image/png"), ESearchCase::IgnoreCase)
                    || HttpResult.ContentType.StartsWith(TEXT("application/octet-stream"), ESearchCase::IgnoreCase);
            if (!bExpectedContentType)
            {
                Decoded = TileRequest;
                Decoded.Failure = FString::Printf(TEXT("Map tile had unexpected content type: %s."),
                    *HttpResult.ContentType);
            }
            else if (!DecodeCompressedTile(TileRequest, HttpResult.Bytes, Decoded))
            {
                if (Decoded.Failure.IsEmpty()) Decoded.Failure = TEXT("Map tile decode failed.");
            }
        }
        if (!State->bActive.load(std::memory_order_acquire)
            || State->Epoch.load(std::memory_order_acquire) != TileRequest.Epoch
            || Cancellation->IsCancelled()) return;
        FScopeLock Lock(&State->Mutex);
        if (State->bActive.load(std::memory_order_relaxed)
            && State->Epoch.load(std::memory_order_relaxed) == TileRequest.Epoch)
            State->CompletedTiles.Add(MoveTemp(Decoded));
    });
}

bool SSkiSiteMap::UploadImagery(const FTileKey& Key, const TArray<FColor>& Pixels)
{
    if (Pixels.Num() != TilePixels * TilePixels) return false;
    if (UploadsStartedThisFrame >= MaxCompletedTilesPerTick) return false;
    UTexture2D* Texture = UTexture2D::CreateTransient(TilePixels, TilePixels, PF_B8G8R8A8);
    if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.IsEmpty()) return false;
    Texture->NeverStream = true;
    FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
    void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
    if (!Data)
    {
        Mip.BulkData.Unlock();
        return false;
    }
    FMemory::Memcpy(Data, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
    Mip.BulkData.Unlock();
    Texture->UpdateResource();

    FTile Tile;
    Tile.Texture.Reset(Texture);
    Tile.Brush.SetResourceObject(Texture);
    Tile.Brush.ImageSize = FVector2D(TilePixels, TilePixels);
    Tile.Brush.DrawAs = ESlateBrushDrawType::Image;
    Tile.LastFrame = Frame;
    TileCache.Add(Key, MoveTemp(Tile));
    ++UploadsStartedThisFrame;
    return true;
}

void SSkiSiteMap::TrimTileCache()
{
    while (TileCache.Num() > MaxCachedTiles)
    {
        FTileKey OldestKey;
        uint64 OldestFrame = MAX_uint64;
        bool bFound = false;
        for (const TPair<FTileKey, FTile>& Pair : TileCache)
        {
            if (VisibleTiles.Contains(Pair.Key)) continue;
            if (Pair.Value.LastFrame < OldestFrame)
            {
                OldestFrame = Pair.Value.LastFrame;
                OldestKey = Pair.Key;
                bFound = true;
            }
        }
        if (!bFound) break;
        TileCache.Remove(OldestKey);
    }
}

bool SSkiSiteMap::ScreenToSiteMeters(const FGeometry& Geometry, const FVector2D Position,
    double& OutEastM, double& OutNorthM) const
{
    OutEastM = OutNorthM = 0.0;
    const int32 TileZoom = FMath::Clamp(FMath::FloorToInt(ZoomLevel), MinMapZoom, MaxMapZoom);
    const double Scale = FMath::Pow(2.0, ZoomLevel - TileZoom);
    const double WorldPixels = std::ldexp(static_cast<double>(TilePixels), TileZoom);
    SkiDomain::WebMercatorPixel Center;
    if (!SkiDomain::TryWebMercatorPixel(CenterLatitudeDeg, CenterLongitudeDeg,
            static_cast<uint8>(TileZoom), Center)) return false;
    const FVector2D Size = Geometry.GetLocalSize();
    SkiDomain::WebMercatorPixel Pixel;
    Pixel.X = Center.X + (Position.X - Size.X * 0.5) / Scale;
    Pixel.X = SkiSiteMapMath::WrapWorldPixelX(Pixel.X, WorldPixels);
    Pixel.Y = Center.Y + (Position.Y - Size.Y * 0.5) / Scale;
    double Latitude = 0.0, Longitude = 0.0;
    if (!SkiDomain::TryGeodeticFromWebMercatorPixel(Pixel, static_cast<uint8>(TileZoom),
            Latitude, Longitude)) return false;
    const SkiDomain::EnuPoint Enu = SkiDomain::ToEnu(SiteFrame, {Latitude, Longitude, 0.0});
    OutEastM = Enu.EastM;
    OutNorthM = Enu.NorthM;
    return FMath::IsFinite(OutEastM) && FMath::IsFinite(OutNorthM);
}

bool SSkiSiteMap::SiteMetersToScreen(const FGeometry& Geometry, const double EastM,
    const double NorthM, FVector2D& OutPosition) const
{
    OutPosition = FVector2D::ZeroVector;
    SkiDomain::GeodeticPoint Point;
    if (!SkiDomain::TrySeaLevelGeodeticFromEnu(SiteFrame, EastM, NorthM, Point)) return false;
    const int32 TileZoom = FMath::Clamp(FMath::FloorToInt(ZoomLevel), MinMapZoom, MaxMapZoom);
    const double Scale = FMath::Pow(2.0, ZoomLevel - TileZoom);
    SkiDomain::WebMercatorPixel Center, Pixel;
    if (!SkiDomain::TryWebMercatorPixel(CenterLatitudeDeg, CenterLongitudeDeg,
            static_cast<uint8>(TileZoom), Center)
        || !SkiDomain::TryWebMercatorPixel(Point.LatitudeDeg, Point.LongitudeDeg,
            static_cast<uint8>(TileZoom), Pixel)) return false;
    Pixel.X = SkiSiteMapMath::UnwrapWorldPixelXNear(Pixel.X, Center.X,
        std::ldexp(static_cast<double>(TilePixels), TileZoom));
    const FVector2D Size = Geometry.GetLocalSize();
    OutPosition = FVector2D(Size.X * 0.5 + (Pixel.X - Center.X) * Scale,
        Size.Y * 0.5 + (Pixel.Y - Center.Y) * Scale);
    return true;
}

FReply SSkiSiteMap::OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!bPickerActive) return FReply::Unhandled();
    const FVector2D Position = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    const FKey Button = Event.GetEffectingButton();
    if (Button == EKeys::RightMouseButton || Button == EKeys::MiddleMouseButton)
    {
        DragMode = EDragMode::Pan;
        DragStartPosition = Position;
        return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
    }
    if (Button != EKeys::LeftMouseButton) return FReply::Unhandled();
    if (!bBoundaryEditingEnabled) return FReply::Handled();

    if (!ScreenToSiteMeters(Geometry, Position, DragStartEastM, DragStartNorthM))
        return FReply::Unhandled();
    DragStartPosition = Position;
    DragStartSelection = Selection;
    SkiDomain::SiteResizeHandle HitHandle;
    if (HitSelectionHandle(Geometry, Position, HitHandle))
    {
        ResizeHandle = static_cast<uint8>(HitHandle);
        DragMode = EDragMode::Resize;
        return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
    }
    if (IsInsideSelection(Geometry, Position))
    {
        DragMode = EDragMode::Move;
        return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
    }
    DragMode = EDragMode::Draw;
    UpdateSelection({}, false);
    return FReply::Handled().CaptureMouse(AsShared()).SetUserFocus(AsShared(), EFocusCause::Mouse);
}

FReply SSkiSiteMap::OnMouseButtonUp(const FGeometry&, const FPointerEvent& Event)
{
    const FKey Button = Event.GetEffectingButton();
    if ((Button == EKeys::LeftMouseButton && DragMode != EDragMode::Pan)
        || ((Button == EKeys::RightMouseButton || Button == EKeys::MiddleMouseButton)
            && DragMode == EDragMode::Pan))
    {
        DragMode = EDragMode::None;
        return FReply::Handled().ReleaseMouseCapture();
    }
    return FReply::Unhandled();
}

FReply SSkiSiteMap::OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!bPickerActive || DragMode == EDragMode::None) return FReply::Unhandled();
    const FVector2D Position = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    if (DragMode == EDragMode::Pan)
    {
        const FVector2D Delta = Position - DragStartPosition;
        const int32 TileZoom = FMath::Clamp(FMath::FloorToInt(ZoomLevel), MinMapZoom, MaxMapZoom);
        const double Scale = FMath::Pow(2.0, ZoomLevel - TileZoom);
        SkiDomain::WebMercatorPixel Center;
        if (SkiDomain::TryWebMercatorPixel(CenterLatitudeDeg, CenterLongitudeDeg,
                static_cast<uint8>(TileZoom), Center))
        {
            Center.X -= Delta.X / Scale;
            Center.Y -= Delta.Y / Scale;
            const double World = static_cast<double>(1U << TileZoom) * TilePixels;
            Center.X = SkiSiteMapMath::WrapWorldPixelX(Center.X, World);
            Center.Y = FMath::Clamp(Center.Y, 0.0, World);
            SkiDomain::TryGeodeticFromWebMercatorPixel(Center, static_cast<uint8>(TileZoom),
                CenterLatitudeDeg, CenterLongitudeDeg);
        }
        DragStartPosition = Position;
        Invalidate(EInvalidateWidgetReason::Paint);
        return FReply::Handled();
    }

    double East = 0.0, North = 0.0;
    if (!ScreenToSiteMeters(Geometry, Position, East, North)) return FReply::Handled();
    SkiDomain::SiteRectangleM Updated;
    bool bValid = false;
    if (DragMode == EDragMode::Draw)
        bValid = SkiDomain::TryDrawSiteRectangle(DragStartEastM, DragStartNorthM,
            East, North, Updated);
    else if (DragMode == EDragMode::Move)
        bValid = SkiDomain::TryMoveSiteRectangle(DragStartSelection,
            East - DragStartEastM, North - DragStartNorthM, Updated);
    else if (DragMode == EDragMode::Resize)
        bValid = SkiDomain::TryResizeSiteRectangle(DragStartSelection,
            static_cast<SkiDomain::SiteResizeHandle>(ResizeHandle), East, North, Updated);
    if (bValid) UpdateSelection(Updated, true);
    return FReply::Handled();
}

FReply SSkiSiteMap::OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!bPickerActive) return FReply::Unhandled();
    const FVector2D Cursor = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
    double AnchorEast = 0.0, AnchorNorth = 0.0;
    const bool bPreserveCursor = ScreenToSiteMeters(Geometry, Cursor, AnchorEast, AnchorNorth);
    ZoomLevel = FMath::Clamp(ZoomLevel + Event.GetWheelDelta() * 0.5,
        static_cast<double>(MinMapZoom), static_cast<double>(MaxMapZoom));
    if (bPreserveCursor)
    {
        const int32 TileZoom = FMath::Clamp(FMath::FloorToInt(ZoomLevel), MinMapZoom, MaxMapZoom);
        const double Scale = FMath::Pow(2.0, ZoomLevel - TileZoom);
        SkiDomain::GeodeticPoint AnchorPoint;
        SkiDomain::TrySeaLevelGeodeticFromEnu(SiteFrame, AnchorEast, AnchorNorth, AnchorPoint);
        SkiDomain::WebMercatorPixel AnchorPixel;
        if (SkiDomain::TryWebMercatorPixel(AnchorPoint.LatitudeDeg, AnchorPoint.LongitudeDeg,
                static_cast<uint8>(TileZoom), AnchorPixel))
        {
            const FVector2D Size = Geometry.GetLocalSize();
            const double WorldPixels = std::ldexp(static_cast<double>(TilePixels), TileZoom);
            const double CenterX = SkiSiteMapMath::WrapWorldPixelX(
                AnchorPixel.X - (Cursor.X - Size.X * 0.5) / Scale, WorldPixels);
            const double CenterY = AnchorPixel.Y - (Cursor.Y - Size.Y * 0.5) / Scale;
            SkiDomain::TryGeodeticFromWebMercatorPixel({CenterX, CenterY},
                static_cast<uint8>(TileZoom), CenterLatitudeDeg, CenterLongitudeDeg);
        }
    }
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
}

void SSkiSiteMap::OnFocusLost(const FFocusEvent& Event)
{
    SLeafWidget::OnFocusLost(Event);
    DragMode = EDragMode::None;
}

bool SSkiSiteMap::HitSelectionHandle(const FGeometry& Geometry, const FVector2D Position,
    SkiDomain::SiteResizeHandle& OutHandle) const
{
    if (!HasValidSelection()) return false;
    struct FHandleCandidate { double East; double North; SkiDomain::SiteResizeHandle Handle; };
    const FHandleCandidate Candidates[] = {
        {Selection.WestM, (Selection.SouthM + Selection.NorthM) * 0.5, SkiDomain::SiteResizeHandle::West},
        {Selection.EastM, (Selection.SouthM + Selection.NorthM) * 0.5, SkiDomain::SiteResizeHandle::East},
        {(Selection.WestM + Selection.EastM) * 0.5, Selection.SouthM, SkiDomain::SiteResizeHandle::South},
        {(Selection.WestM + Selection.EastM) * 0.5, Selection.NorthM, SkiDomain::SiteResizeHandle::North},
        {Selection.WestM, Selection.SouthM, SkiDomain::SiteResizeHandle::SouthWest},
        {Selection.EastM, Selection.SouthM, SkiDomain::SiteResizeHandle::SouthEast},
        {Selection.WestM, Selection.NorthM, SkiDomain::SiteResizeHandle::NorthWest},
        {Selection.EastM, Selection.NorthM, SkiDomain::SiteResizeHandle::NorthEast},
    };
    double BestDistanceSquared = 18.0 * 18.0;
    bool bFound = false;
    for (const FHandleCandidate& Candidate : Candidates)
    {
        FVector2D CandidatePosition;
        if (!SiteMetersToScreen(Geometry, Candidate.East, Candidate.North, CandidatePosition)) continue;
        const double DistanceSquared = FVector2D::DistSquared(Position, CandidatePosition);
        if (DistanceSquared < BestDistanceSquared)
        {
            BestDistanceSquared = DistanceSquared;
            OutHandle = Candidate.Handle;
            bFound = true;
        }
    }
    return bFound;
}

bool SSkiSiteMap::IsInsideSelection(const FGeometry& Geometry, const FVector2D Position) const
{
    if (!HasValidSelection()) return false;
    double East = 0.0, North = 0.0;
    return ScreenToSiteMeters(Geometry, Position, East, North)
        && East >= Selection.WestM && East <= Selection.EastM
        && North >= Selection.SouthM && North <= Selection.NorthM;
}

void SSkiSiteMap::DrawSolidQuad(const FGeometry& Geometry,
    FSlateWindowElementList& OutDrawElements, const int32 LayerId,
    const FVector2f A, const FVector2f B, const FVector2f C, const FVector2f D,
    const FColor Color) const
{
    TArray<FSlateVertex> Vertices;
    TArray<SlateIndex> Indices;
    const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, A, FVector2f::ZeroVector, Color));
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, B, FVector2f::ZeroVector, Color));
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, C, FVector2f::ZeroVector, Color));
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, D, FVector2f::ZeroVector, Color));
    Indices.Append({0, 1, 2, 0, 2, 3});
    const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(FSlateBrush());
    FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, Handle, Vertices, Indices,
        nullptr, 0, 0);
}

void SSkiSiteMap::DrawLine(const FGeometry& Geometry, FSlateWindowElementList& OutDrawElements,
    const int32 LayerId, const FVector2f Start, const FVector2f End,
    const float Width, const FColor Color) const
{
    const FVector2f Direction = (End - Start).GetSafeNormal();
    const FVector2f Normal(-Direction.Y * Width * 0.5f, Direction.X * Width * 0.5f);
    DrawSolidQuad(Geometry, OutDrawElements, LayerId,
        Start - Normal, Start + Normal, End + Normal, End - Normal, Color);
}

int32 SSkiSiteMap::OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
    const FSlateRect& CullingRect, FSlateWindowElementList& OutDrawElements,
    const int32 LayerId, const FWidgetStyle& WidgetStyle, const bool bParentEnabled) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
    FSlateDrawElement::MakeBox(OutDrawElements, LayerId, Geometry.ToPaintGeometry(), WhiteBrush,
        ESlateDrawEffect::None, FLinearColor(0.055f, 0.075f, 0.085f, 1.0f));

    const int32 TileZoom = FMath::Clamp(FMath::FloorToInt(ZoomLevel), MinMapZoom, MaxMapZoom);
    const double Scale = FMath::Pow(2.0, ZoomLevel - TileZoom);
    const double WorldPixels = std::ldexp(static_cast<double>(TilePixels), TileZoom);
    SkiDomain::WebMercatorPixel Center;
    const bool bCenterValid = SkiDomain::TryWebMercatorPixel(CenterLatitudeDeg,
        CenterLongitudeDeg, static_cast<uint8>(TileZoom), Center);
    const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
    const FSlateResourceHandle VertHandle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(FSlateBrush());
    TSet<FIntPoint> LabelCollisionCells;

    if (bCenterValid)
    {
        for (const FTileKey& Key : VisibleTiles)
        {
            if (Key.Layer != ETileLayer::Imagery) continue;
            const FTile* Tile = TileCache.Find(Key);
            if (!Tile || !Tile->Texture.IsValid()) continue;
            const double DisplayTileX = SkiSiteMapMath::UnwrapWorldPixelXNear(
                Key.X * TilePixels, Center.X, WorldPixels);
            const FVector2D Position(Size.X * 0.5 + (DisplayTileX - Center.X) * Scale,
                Size.Y * 0.5 + (Key.Y * TilePixels + TilePixels * 0.5 - Center.Y) * Scale - TilePixels * Scale * 0.5);
            const FVector2D Span(TilePixels * Scale, TilePixels * Scale);
            FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 1,
                Geometry.ToPaintGeometry(ToVector2f(Position), ToVector2f(Span)), &Tile->Brush);
        }

        if (bContoursEnabled)
        {
            for (const FTileKey& Key : VisibleTiles)
            {
                if (Key.Layer != ETileLayer::Elevation) continue;
                const FTile* Tile = TileCache.Find(Key);
                if (!Tile || Tile->Contours.IsEmpty()) continue;
                const double DisplayTileX = SkiSiteMapMath::UnwrapWorldPixelXNear(
                    Key.X * TilePixels, Center.X, WorldPixels);
                const FVector2f Origin(static_cast<float>(Size.X * 0.5
                        + (DisplayTileX - Center.X) * Scale),
                    static_cast<float>(Size.Y * 0.5 + (Key.Y * TilePixels - Center.Y) * Scale));
                TArray<FSlateVertex> Vertices;
                TArray<SlateIndex> Indices;
                Vertices.Reserve(Tile->Contours.Num() * 4);
                Indices.Reserve(Tile->Contours.Num() * 6);
                for (const FSkiMapContourSegment& Segment : Tile->Contours)
                {
                    const FVector2f A = Origin + Segment.Start * static_cast<float>(Scale);
                    const FVector2f B = Origin + Segment.End * static_cast<float>(Scale);
                    const FVector2f Direction = (B - A).GetSafeNormal();
                    const FVector2f Normal(-Direction.Y * (Segment.bIndex ? 1.8f : 1.0f) * 0.5f,
                        Direction.X * (Segment.bIndex ? 1.8f : 1.0f) * 0.5f);
                    const int32 Base = Vertices.Num();
                    const FColor Color = Segment.bIndex ? FColor(255, 225, 178, 225)
                        : FColor(255, 255, 255, 150);
                    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, A - Normal, FVector2f::ZeroVector, Color));
                    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, A + Normal, FVector2f::ZeroVector, Color));
                    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, B - Normal, FVector2f::ZeroVector, Color));
                    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, B + Normal, FVector2f::ZeroVector, Color));
                    Indices.Append({static_cast<SlateIndex>(Base), static_cast<SlateIndex>(Base + 1), static_cast<SlateIndex>(Base + 2),
                        static_cast<SlateIndex>(Base + 1), static_cast<SlateIndex>(Base + 3), static_cast<SlateIndex>(Base + 2)});

                    if (Segment.bIndex)
                    {
                        const FVector2f Mid = (A + B) * 0.5f;
                        double LabelAngle = FMath::Atan2(B.Y - A.Y, B.X - A.X);
                        if (LabelAngle > Pi * 0.5) LabelAngle -= Pi;
                        else if (LabelAngle < -Pi * 0.5) LabelAngle += Pi;

                        const FVector2f LabelSize(82.0f, 18.0f);
                        const FVector2f HalfExtents(
                            (FMath::Abs(FMath::Cos(LabelAngle)) * LabelSize.X
                                + FMath::Abs(FMath::Sin(LabelAngle)) * LabelSize.Y) * 0.5f,
                            (FMath::Abs(FMath::Sin(LabelAngle)) * LabelSize.X
                                + FMath::Abs(FMath::Cos(LabelAngle)) * LabelSize.Y) * 0.5f);
                        constexpr float LabelCellSize = 88.0f;
                        const int32 MinCellX = FMath::FloorToInt((Mid.X - HalfExtents.X) / LabelCellSize);
                        const int32 MaxCellX = FMath::FloorToInt((Mid.X + HalfExtents.X) / LabelCellSize);
                        const int32 MinCellY = FMath::FloorToInt((Mid.Y - HalfExtents.Y) / LabelCellSize);
                        const int32 MaxCellY = FMath::FloorToInt((Mid.Y + HalfExtents.Y) / LabelCellSize);
                        TArray<FIntPoint, TInlineAllocator<4>> CandidateCells;
                        bool bLabelOverlaps = false;
                        for (int32 CellY = MinCellY; CellY <= MaxCellY; ++CellY)
                        {
                            for (int32 CellX = MinCellX; CellX <= MaxCellX; ++CellX)
                            {
                                const FIntPoint Cell(CellX, CellY);
                                CandidateCells.Add(Cell);
                                bLabelOverlaps |= LabelCollisionCells.Contains(Cell);
                            }
                        }
                        if (!bLabelOverlaps && Mid.X + HalfExtents.X >= 0.0f
                            && Mid.Y + HalfExtents.Y >= 0.0f
                            && Mid.X - HalfExtents.X <= Size.X
                            && Mid.Y - HalfExtents.Y <= Size.Y)
                        {
                            for (const FIntPoint& Cell : CandidateCells)
                                LabelCollisionCells.Add(Cell);

                            const int32 LabelElevation = bMetricContourLabels
                                ? FMath::RoundToInt(Segment.ElevationMeters)
                                : FMath::RoundToInt(Segment.ElevationMeters / MetersPerFoot);
                            // Metric mode converts the established 40 ft contour levels to metres.
                            const FString Label = FString::FormatAsNumber(LabelElevation)
                                + (bMetricContourLabels ? TEXT(" m") : TEXT("'"));
                            const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
                            const FVector2f LabelPosition = Mid - LabelSize * 0.5f;
                            const FPaintGeometry LabelGeometry = Geometry.ToPaintGeometry(LabelSize,
                                FSlateLayoutTransform(LabelPosition),
                                FSlateRenderTransform(FQuat2D(LabelAngle), FVector2f::ZeroVector),
                                LabelSize * 0.5f);
                            FSlateDrawElement::MakeText(OutDrawElements, LayerId + 3,
                                LabelGeometry, FText::FromString(Label), Font,
                                ESlateDrawEffect::None, FLinearColor(1.0f, 0.93f, 0.78f, 0.94f));
                        }
                    }
                }
                if (!Vertices.IsEmpty())
                    FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId + 2,
                        VertHandle, Vertices, Indices, nullptr, 0, 0);
            }
        }
    }

    if (HasValidSelection())
    {
        FVector2D SW, NW, NE, SE;
        if (SiteMetersToScreen(Geometry, Selection.WestM, Selection.SouthM, SW)
            && SiteMetersToScreen(Geometry, Selection.WestM, Selection.NorthM, NW)
            && SiteMetersToScreen(Geometry, Selection.EastM, Selection.NorthM, NE)
            && SiteMetersToScreen(Geometry, Selection.EastM, Selection.SouthM, SE))
        {
            const FColor OutsideShade(5, 12, 16, 104);
            const double SelectionLeft = FMath::Min(FMath::Min(SW.X, NW.X), FMath::Min(NE.X, SE.X));
            const double SelectionRight = FMath::Max(FMath::Max(SW.X, NW.X), FMath::Max(NE.X, SE.X));
            const double SelectionTop = FMath::Min(FMath::Min(SW.Y, NW.Y), FMath::Min(NE.Y, SE.Y));
            const double SelectionBottom = FMath::Max(FMath::Max(SW.Y, NW.Y), FMath::Max(NE.Y, SE.Y));
            const double ShadeLeft = FMath::Clamp(SelectionLeft, 0.0, Size.X);
            const double ShadeRight = FMath::Clamp(SelectionRight, 0.0, Size.X);
            const double ShadeTop = FMath::Clamp(SelectionTop, 0.0, Size.Y);
            const double ShadeBottom = FMath::Clamp(SelectionBottom, 0.0, Size.Y);
            if (ShadeRight <= ShadeLeft || ShadeBottom <= ShadeTop)
            {
                DrawSolidQuad(Geometry, OutDrawElements, LayerId + 4,
                    FVector2f(0.0f, 0.0f), FVector2f(static_cast<float>(Size.X), 0.0f),
                    FVector2f(static_cast<float>(Size.X), static_cast<float>(Size.Y)),
                    FVector2f(0.0f, static_cast<float>(Size.Y)), OutsideShade);
            }
            else
            {
                DrawSolidQuad(Geometry, OutDrawElements, LayerId + 4,
                    FVector2f(0.0f, 0.0f), FVector2f(static_cast<float>(Size.X), 0.0f),
                    FVector2f(static_cast<float>(ShadeRight), static_cast<float>(ShadeTop)),
                    FVector2f(static_cast<float>(ShadeLeft), static_cast<float>(ShadeTop)), OutsideShade);
                DrawSolidQuad(Geometry, OutDrawElements, LayerId + 4,
                    FVector2f(static_cast<float>(Size.X), 0.0f),
                    FVector2f(static_cast<float>(Size.X), static_cast<float>(Size.Y)),
                    FVector2f(static_cast<float>(ShadeRight), static_cast<float>(ShadeBottom)),
                    FVector2f(static_cast<float>(ShadeRight), static_cast<float>(ShadeTop)), OutsideShade);
                DrawSolidQuad(Geometry, OutDrawElements, LayerId + 4,
                    FVector2f(static_cast<float>(Size.X), static_cast<float>(Size.Y)),
                    FVector2f(0.0f, static_cast<float>(Size.Y)),
                    FVector2f(static_cast<float>(ShadeLeft), static_cast<float>(ShadeBottom)),
                    FVector2f(static_cast<float>(ShadeRight), static_cast<float>(ShadeBottom)), OutsideShade);
                DrawSolidQuad(Geometry, OutDrawElements, LayerId + 4,
                    FVector2f(0.0f, static_cast<float>(Size.Y)), FVector2f(0.0f, 0.0f),
                    FVector2f(static_cast<float>(ShadeLeft), static_cast<float>(ShadeTop)),
                    FVector2f(static_cast<float>(ShadeLeft), static_cast<float>(ShadeBottom)), OutsideShade);
            }
            DrawSolidQuad(Geometry, OutDrawElements, LayerId + 5, ToVector2f(SW), ToVector2f(NW),
                ToVector2f(NE), ToVector2f(SE), FColor(28, 145, 190, 46));
            DrawLine(Geometry, OutDrawElements, LayerId + 6, ToVector2f(SW), ToVector2f(NW), 3.0f,
                FColor(45, 205, 245, 255));
            DrawLine(Geometry, OutDrawElements, LayerId + 6, ToVector2f(NW), ToVector2f(NE), 3.0f,
                FColor(45, 205, 245, 255));
            DrawLine(Geometry, OutDrawElements, LayerId + 6, ToVector2f(NE), ToVector2f(SE), 3.0f,
                FColor(45, 205, 245, 255));
            DrawLine(Geometry, OutDrawElements, LayerId + 6, ToVector2f(SE), ToVector2f(SW), 3.0f,
                FColor(45, 205, 245, 255));
            const FVector2D Handles[] = {
                SW, NW, NE, SE,
                FVector2D(SW.X, (SW.Y + NW.Y) * 0.5),
                FVector2D(SE.X, (SE.Y + NE.Y) * 0.5),
                FVector2D((SW.X + SE.X) * 0.5, SW.Y),
                FVector2D((NW.X + NE.X) * 0.5, NW.Y),
            };
            for (const FVector2D& Point : Handles)
                DrawSolidQuad(Geometry, OutDrawElements, LayerId + 7,
                    ToVector2f(Point + FVector2D(-5, -5)), ToVector2f(Point + FVector2D(5, -5)),
                    ToVector2f(Point + FVector2D(5, 5)), ToVector2f(Point + FVector2D(-5, 5)),
                    FColor(235, 248, 250, 255));
        }
    }

    const FSlateFontInfo AttributionFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 13);
    const FText Attribution = FText::FromString(
        TEXT("USGS · © OpenStreetMap contributors · Mapzen/AWS"));
    const TSharedRef<FSlateFontMeasure> FontMeasure =
        FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    const FVector2f Measured = FontMeasure->Measure(Attribution, AttributionFont);
    const float AvailableAttributionWidth = FMath::Max(1.0f, static_cast<float>(Size.X - 24.0));
    const float AttributionWidth = FMath::Min(static_cast<float>(Measured.X), AvailableAttributionWidth);
    const float AttributionHeight = FMath::Max(18.0f, static_cast<float>(Measured.Y));
    const float AttributionX = FMath::Max(0.0f, static_cast<float>(Size.X) - AttributionWidth - 20.0f);
    const float AttributionY = FMath::Max(0.0f, static_cast<float>(Size.Y) - AttributionHeight - 20.0f);
    const FVector2f AttributionBoxPosition(AttributionX - 8.0f, AttributionY - 4.0f);
    const FVector2f AttributionBoxSize(AttributionWidth + 16.0f, AttributionHeight + 8.0f);
    FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 8,
        Geometry.ToPaintGeometry(AttributionBoxPosition, AttributionBoxSize), WhiteBrush,
        ESlateDrawEffect::None, FLinearColor(0.035f, 0.07f, 0.09f, 0.82f));
    FSlateDrawElement::MakeText(OutDrawElements, LayerId + 9,
        Geometry.ToPaintGeometry(FVector2f(AttributionX, AttributionY),
            FVector2f(AttributionWidth, AttributionHeight)),
        Attribution, AttributionFont, ESlateDrawEffect::None,
        FLinearColor(1.0f, 1.0f, 1.0f, 0.98f));
    return LayerId + 9;
}

void SSkiSiteMap::NotifySelectionStatus(const FString& Text) const
{
    if (SelectionStatus) SelectionStatus(Text);
}

void SSkiSiteMap::NotifyPreviewStatus(const FString& Text) const
{
    if (PreviewStatus) PreviewStatus(Text);
}

void SSkiSiteMap::SetPreviewStatusHandler(FSkiSiteMapStatusCallback Handler)
{
    PreviewStatus = MoveTemp(Handler);
}

void SSkiSiteMap::SetSelectionStatusHandler(FSkiSiteMapStatusCallback Handler)
{
    SelectionStatus = MoveTemp(Handler);
}

TSharedRef<SWidget> USkiSiteMapWidget::RebuildWidget()
{
    const TWeakObjectPtr<USkiSiteMapWidget> WeakThis(this);
    SiteMap = SNew(SSkiSiteMap)
        .OnPreviewStatus([WeakThis](const FString& Text)
        {
            if (WeakThis.IsValid() && WeakThis->PreviewStatusHandler)
                WeakThis->PreviewStatusHandler(Text);
        })
        .OnSelectionStatus([WeakThis](const FString& Text)
        {
            if (WeakThis.IsValid() && WeakThis->SelectionStatusHandler)
                WeakThis->SelectionStatusHandler(Text);
        });
    SiteMap->SetCenter(CenterLatitudeDeg, CenterLongitudeDeg);
    SiteMap->SetContoursEnabled(bContoursEnabled);
    SiteMap->SetContourUnits(bMetricContourLabels);
    SiteMap->SetNetworkEnabled(bNetworkEnabled);
    if (bHasPendingFit)
        SiteMap->FitToBounds(PendingFitBounds, PendingFitPaddingPx);
    SiteMap->SetPickerActive(bPickerActive);
    SiteMap->SetBoundaryEditingEnabled(bBoundaryEditingEnabled);
    return SiteMap.ToSharedRef();
}

void USkiSiteMapWidget::ReleaseSlateResources(const bool bReleaseChildren)
{
    if (SiteMap.IsValid()) SiteMap->SetPickerActive(false);
    SiteMap.Reset();
    Super::ReleaseSlateResources(bReleaseChildren);
}

void USkiSiteMapWidget::SetPickerActive(const bool bActive)
{
    bPickerActive = bActive;
    if (!bActive) bBoundaryEditingEnabled = false;
    if (SiteMap.IsValid()) SiteMap->SetPickerActive(bActive);
}

void USkiSiteMapWidget::SetBoundaryEditingEnabled(const bool bEnabled)
{
    bBoundaryEditingEnabled = bPickerActive && bEnabled;
    if (SiteMap.IsValid()) SiteMap->SetBoundaryEditingEnabled(bEnabled);
}

void USkiSiteMapWidget::SetCenter(const double LatitudeDeg, const double LongitudeDeg)
{
    CenterLatitudeDeg = LatitudeDeg;
    CenterLongitudeDeg = LongitudeDeg;
    bHasPendingFit = false;
    if (SiteMap.IsValid()) SiteMap->SetCenter(LatitudeDeg, LongitudeDeg);
}

bool USkiSiteMapWidget::FitToBounds(const SkiDomain::GeographicBounds& Bounds,
    const double PaddingPx)
{
    if (!IsValidFitRequest(Bounds, PaddingPx)) return false;
    PendingFitBounds = Bounds;
    PendingFitPaddingPx = PaddingPx;
    bHasPendingFit = true;
    return !SiteMap.IsValid() || SiteMap->FitToBounds(Bounds, PaddingPx);
}

void USkiSiteMapWidget::SetContoursEnabled(const bool bEnabled)
{
    bContoursEnabled = bEnabled;
    if (SiteMap.IsValid()) SiteMap->SetContoursEnabled(bEnabled);
}

void USkiSiteMapWidget::SetContourUnits(const bool bMetric)
{
    bMetricContourLabels = bMetric;
    if (SiteMap.IsValid()) SiteMap->SetContourUnits(bMetric);
}

void USkiSiteMapWidget::SetNetworkEnabled(const bool bEnabled)
{
    bNetworkEnabled = bEnabled;
    if (SiteMap.IsValid()) SiteMap->SetNetworkEnabled(bEnabled);
}

void USkiSiteMapWidget::ClearSelection()
{
    if (SiteMap.IsValid()) SiteMap->ClearSelection();
}

bool USkiSiteMapWidget::CreateViewportSmokeBoundary(const FGuid& SmokeToken)
{
#if UE_BUILD_DEVELOPMENT
    return SiteMap.IsValid() && SiteMap->CreateViewportSmokeBoundary(SmokeToken);
#else
    (void)SmokeToken;
    return false;
#endif
}

bool USkiSiteMapWidget::HasValidSelection() const
{
    return SiteMap.IsValid() && SiteMap->HasValidSelection();
}

bool USkiSiteMapWidget::TryGetSelectedBounds(SkiDomain::GeographicBounds& OutBounds) const
{
    OutBounds = {};
    return SiteMap.IsValid() && SiteMap->TryGetSelectedBounds(OutBounds);
}

SkiDomain::SiteRectangleM USkiSiteMapWidget::GetSelectionMeters() const
{
    return SiteMap.IsValid() ? SiteMap->GetSelectionMeters() : SkiDomain::SiteRectangleM{};
}

void USkiSiteMapWidget::SetPreviewStatusHandler(TFunction<void(const FString&)> Handler)
{
    PreviewStatusHandler = MoveTemp(Handler);
    if (SiteMap.IsValid())
    {
        const TWeakObjectPtr<USkiSiteMapWidget> WeakThis(this);
        SiteMap->SetPreviewStatusHandler([WeakThis](const FString& Text)
        {
            if (WeakThis.IsValid() && WeakThis->PreviewStatusHandler)
                WeakThis->PreviewStatusHandler(Text);
        });
    }
}

void USkiSiteMapWidget::SetSelectionStatusHandler(TFunction<void(const FString&)> Handler)
{
    SelectionStatusHandler = MoveTemp(Handler);
    if (SiteMap.IsValid())
    {
        const TWeakObjectPtr<USkiSiteMapWidget> WeakThis(this);
        SiteMap->SetSelectionStatusHandler([WeakThis](const FString& Text)
        {
            if (WeakThis.IsValid() && WeakThis->SelectionStatusHandler)
                WeakThis->SelectionStatusHandler(Text);
        });
    }
}
