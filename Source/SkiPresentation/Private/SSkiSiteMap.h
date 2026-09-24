#pragma once

#include "CoreMinimal.h"
#include "Rendering/DrawElements.h"
#include "Styling/SlateBrush.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SLeafWidget.h"
#include "SkiDomain/Coordinates.h"
#include "SkiDomain/SiteSelection.h"
#include "SkiDomain/TerrainPackage.h"

#include <atomic>

namespace SkiSiteMapMath
{
double WrapWorldPixelX(double X, double WorldPixels);
double UnwrapWorldPixelXNear(double X, double ReferenceX, double WorldPixels);
int32 WrapWorldTileX(int32 X, int32 WorldTileCount);
}

namespace SkiPreparation { class Cancellation; }

struct FSkiMapContourSegment
{
    FVector2f Start;
    FVector2f End;
    float ElevationMeters = 0.0f;
    bool bIndex = false;
};

class UTexture2D;
struct FSkiSiteMapAsyncState;
using FSkiSiteMapStatusCallback = TFunction<void(const FString&)>;

/**
 * Native, north-up picker map. Imagery/elevation work is bounded and owned by this widget;
 * a generation fence prevents late tile and preview results from changing a newer view.
 */
class SSkiSiteMap final : public SLeafWidget
{
public:
    enum class ETileLayer : uint8 { Imagery, Elevation };
    SLATE_BEGIN_ARGS(SSkiSiteMap) {}
        SLATE_ARGUMENT(FSkiSiteMapStatusCallback, OnPreviewStatus)
        SLATE_ARGUMENT(FSkiSiteMapStatusCallback, OnSelectionStatus)
    SLATE_END_ARGS()

    void Construct(const FArguments& Arguments);
    virtual ~SSkiSiteMap() override;
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920.0, 1080.0); }
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
        const FSlateRect& CullingRect, FSlateWindowElementList& OutDrawElements,
        int32 LayerId, const FWidgetStyle& WidgetStyle, bool bParentEnabled) const override;
    virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseButtonUp(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseMove(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual FReply OnMouseWheel(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual void OnFocusLost(const FFocusEvent& Event) override;
    virtual bool SupportsKeyboardFocus() const override { return true; }

    void SetPickerActive(bool bActive);
    void SetBoundaryEditingEnabled(bool bEnabled);
    void SetCenter(double LatitudeDeg, double LongitudeDeg);
    bool FitToBounds(const SkiDomain::GeographicBounds& Bounds, double PaddingPx = 48.0);
    void SetContoursEnabled(bool bEnabled);
    void SetContourUnits(bool bMetric);
    void SetNetworkEnabled(bool bEnabled);
    void ClearSelection();
    bool CreateViewportSmokeBoundary(const FGuid& SmokeToken);
    bool HasValidSelection() const;
    bool TryGetSelectedBounds(SkiDomain::GeographicBounds& OutBounds) const;
    SkiDomain::SiteRectangleM GetSelectionMeters() const { return Selection; }
    void SetPreviewStatusHandler(FSkiSiteMapStatusCallback Handler);
    void SetSelectionStatusHandler(FSkiSiteMapStatusCallback Handler);

private:
    enum class EDragMode : uint8 { None, Pan, Draw, Move, Resize };
    struct FTileKey
    {
        int32 X = 0;
        int32 Y = 0;
        uint8 Zoom = 0;
        ETileLayer Layer = ETileLayer::Imagery;
        bool operator==(const FTileKey& Other) const
        { return X == Other.X && Y == Other.Y && Zoom == Other.Zoom && Layer == Other.Layer; }
        friend uint32 GetTypeHash(const FTileKey& Key)
        {
            return HashCombine(HashCombine(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)),
                HashCombine(::GetTypeHash(Key.Zoom),
                    ::GetTypeHash(static_cast<uint8>(Key.Layer))));
        }
    };
    struct FTile
    {
        TStrongObjectPtr<UTexture2D> Texture;
        FSlateBrush Brush;
        TArray<FSkiMapContourSegment> Contours;
        uint64 LastFrame = 0;
    };
    struct FPreviewOperation
    {
        uint64 Generation = 0;
        TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> bCancelled;
    };

    bool ScreenToSiteMeters(const FGeometry& Geometry, FVector2D Position,
        double& OutEastM, double& OutNorthM) const;
    bool SiteMetersToScreen(const FGeometry& Geometry, double EastM, double NorthM,
        FVector2D& OutPosition) const;
    bool ApplyPendingFit(const FVector2D& ViewportSize);
    bool SiteRectangleToBounds(const SkiDomain::SiteRectangleM& Rectangle,
        SkiDomain::GeographicBounds& OutBounds) const;
    void UpdateSelection(const SkiDomain::SiteRectangleM& NewSelection, bool bValid);
    void SchedulePreview(bool bBoundaryChanged);
    void StartPreview();
    void DrainAsyncResults();
    void RequestVisibleTiles(const FVector2D& Size);
    void RequestTile(const FTileKey& Key);
    void CancelRequestsAndPreview();
    void AddVisibleTile(const FTileKey& Key, TArray<FTileKey>& OutVisible);
    void TrimTileCache();
    bool UploadImagery(const FTileKey& Key, const TArray<FColor>& Pixels);
    void DrawSolidQuad(const FGeometry& Geometry, FSlateWindowElementList& OutDrawElements,
        int32 LayerId, FVector2f A, FVector2f B, FVector2f C, FVector2f D,
        FColor Color) const;
    void DrawLine(const FGeometry& Geometry, FSlateWindowElementList& OutDrawElements,
        int32 LayerId, FVector2f Start, FVector2f End, float Width, FColor Color) const;
    bool HitSelectionHandle(const FGeometry& Geometry, FVector2D Position,
        SkiDomain::SiteResizeHandle& OutHandle) const;
    bool IsInsideSelection(const FGeometry& Geometry, FVector2D Position) const;
    void NotifySelectionStatus(const FString& Text) const;
    void NotifyPreviewStatus(const FString& Text) const;

    TSharedPtr<FSkiSiteMapAsyncState, ESPMode::ThreadSafe> AsyncState;
    TMap<FTileKey, FTile> TileCache;
    TMap<FTileKey, TSharedPtr<SkiPreparation::Cancellation>> PendingRequests;
    TMap<FTileKey, double> FailedRequests;
    TArray<FTileKey> VisibleTiles;
    FPreviewOperation ActivePreview;
    TFunction<void(const FString&)> PreviewStatus;
    TFunction<void(const FString&)> SelectionStatus;
    SkiDomain::LocalFrame SiteFrame;
    SkiDomain::SiteRectangleM Selection;
    SkiDomain::SiteRectangleM DragStartSelection;
    FVector2D DragStartPosition = FVector2D::ZeroVector;
    double DragStartEastM = 0.0;
    double DragStartNorthM = 0.0;
    double CenterLatitudeDeg = 47.25;
    double CenterLongitudeDeg = -121.55;
    double SiteOriginLatitudeDeg = 47.25;
    double SiteOriginLongitudeDeg = -121.55;
    double ZoomLevel = 13.0;
    SkiDomain::GeographicBounds PendingFitBounds;
    double PendingFitPaddingPx = 48.0;
    double PreviewDueAt = 0.0;
    uint64 Frame = 0;
    uint64 PreviewGeneration = 0;
    int32 RequestsStartedThisFrame = 0;
    int32 UploadsStartedThisFrame = 0;
    uint8 ResizeHandle = 0;
    EDragMode DragMode = EDragMode::None;
    bool bPickerActive = false;
    bool bBoundaryEditingEnabled = false;
    bool bNetworkEnabled = true;
    bool bContoursEnabled = true;
    bool bHasSelection = false;
    bool bPreviewDebouncePending = false;
    bool bFitPending = false;
    bool bMetricContourLabels = false;
};
