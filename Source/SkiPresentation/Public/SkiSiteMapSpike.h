#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateBrush.h"
#include "Rendering/DrawElements.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/ThreadSafeCounter.h"
#include "Widgets/SLeafWidget.h"

class UTexture2D;

// Synthetic, offline M0-A workload. The caller owns the shared pointer and must
// release it before measuring the post-OpenLevel baseline.
class SKIPRESENTATION_API SSkiSiteMapSpike final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SSkiSiteMapSpike) {}
    SLATE_END_ARGS()

    void Construct(const FArguments&);
    virtual ~SSkiSiteMapSpike() override;
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1920, 1080); }
    virtual void Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) override;
    virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
        const FSlateRect& CullingRect, FSlateWindowElementList& OutDrawElements,
        int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const override;

    struct FSample
    {
        double TimeSeconds = 0;
        double FrameMilliseconds = 0;
        uint64 UsedPhysicalBytes = 0;
        uint64 TextureMemoryBytes = 0;
        double GameThreadMilliseconds = 0;
        double RenderThreadMilliseconds = 0;
        double GpuMilliseconds = 0;
        int32 ViewportWidth = 0;
        int32 ViewportHeight = 0;
        int32 ResidentTiles = 0;
        int32 UploadedTiles = 0;
        int32 Hits = 0;
        int32 Misses = 0;
        int32 Evictions = 0;
    };
    const TArray<FSample>& GetSamples() const { return Samples; }
    int32 GetLiveTextureCount() const { return Textures.Num(); }
    void ReleaseTiles();

private:
    struct FTile
    {
        FIntPoint Key;
        TStrongObjectPtr<UTexture2D> Texture;
        FSlateBrush Brush;
        uint64 LastFrame = 0;
    };
    static constexpr int32 TilePixels = 256;
    static constexpr int32 MaxResidentTiles = 96;
    static constexpr int32 MaxUploadsPerTick = 4;
    bool UploadTile(const FIntPoint& Key);
    void AddContourLine(TArray<FSlateVertex>& Vertices, TArray<SlateIndex>& Indices,
        const FGeometry& Geometry, FVector2f Start, FVector2f End, float Width, FColor Color) const;
    TMap<FIntPoint, FTile> Textures;
    TArray<FIntPoint> VisibleKeys;
    TArray<FSample> Samples;
    double StartedAt = 0;
    double LastTickAt = 0;
    FVector2D Pan = FVector2D::ZeroVector;
    float Zoom = 1.0f;
    uint64 Frame = 0;
    int32 Hits = 0;
    int32 Misses = 0;
    int32 Evictions = 0;
    int32 Uploads = 0;
    uint64 LastTextureMemoryBytes = 0;
    double LastTextureMemorySampleAt = -1.0;
    TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> InFlightUploadBytes;
};
