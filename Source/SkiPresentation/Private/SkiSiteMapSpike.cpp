#include "SkiSiteMapSpike.h"

#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Rendering/SlateRenderer.h"
#include "RenderTimer.h"
#include "DynamicRHI.h"
#include "RHIStats.h"

void SSkiSiteMapSpike::Construct(const FArguments&)
{
    StartedAt = LastTickAt = FPlatformTime::Seconds();
    InFlightUploadBytes = MakeShared<FThreadSafeCounter, ESPMode::ThreadSafe>();
}

SSkiSiteMapSpike::~SSkiSiteMapSpike()
{
    ReleaseTiles();
}

void SSkiSiteMapSpike::ReleaseTiles()
{
    Textures.Empty();
    VisibleKeys.Empty();
}

bool SSkiSiteMapSpike::UploadTile(const FIntPoint& Key)
{
    static constexpr int32 UploadBytes = TilePixels * TilePixels * sizeof(FColor);
    static constexpr int32 MaxInFlightUploadBytes = 16 * 1024 * 1024;
    if (InFlightUploadBytes->GetValue() + UploadBytes > MaxInFlightUploadBytes) return false;
    FTile Tile;
    if (Textures.Num() >= MaxResidentTiles)
    {
        FIntPoint OldestKey = FIntPoint::ZeroValue;
        uint64 OldestFrame = MAX_uint64;
        for (const TPair<FIntPoint, FTile>& Pair : Textures)
        {
            if (Pair.Value.LastFrame < OldestFrame)
            { OldestFrame = Pair.Value.LastFrame; OldestKey = Pair.Key; }
        }
        if (FTile* Reusable = Textures.Find(OldestKey))
        {
            Tile = MoveTemp(*Reusable);
            Textures.Remove(OldestKey);
            ++Evictions;
        }
    }
    UTexture2D* Texture = Tile.Texture.Get();
    const bool bNewTexture = Texture == nullptr;
    if (!Texture)
    {
        Texture = UTexture2D::CreateTransient(TilePixels, TilePixels, PF_B8G8R8A8);
        Tile.Texture.Reset(Texture);
    }
    if (!Texture) return false;
    Texture->NeverStream = true;
    if (!bNewTexture && !Texture->GetResource()) return false;
    FColor* Pixels = nullptr;
    if (bNewTexture)
    {
        FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
        Pixels = static_cast<FColor*>(Mip.BulkData.Lock(LOCK_READ_WRITE));
    }
    else Pixels = new FColor[TilePixels * TilePixels];
    if (!Pixels) return false;
    for (int32 Y = 0; Y < TilePixels; ++Y)
    {
        for (int32 X = 0; X < TilePixels; ++X)
        {
            const float Ridge = FMath::Sin((X + Key.X * TilePixels) * 0.018f)
                * FMath::Cos((Y + Key.Y * TilePixels) * 0.013f);
            const uint8 Shade = static_cast<uint8>(FMath::Clamp(112.0f + Ridge * 34.0f, 0.0f, 255.0f));
            Pixels[Y * TilePixels + X] = FColor(Shade, Shade + 12, Shade + 22, 255);
        }
    }
    if (bNewTexture)
    {
        Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
        Texture->UpdateResource();
    }
    else
    {
        FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, TilePixels, TilePixels);
        const TSharedPtr<FThreadSafeCounter, ESPMode::ThreadSafe> Pending = InFlightUploadBytes;
        Pending->Add(UploadBytes);
        Texture->UpdateTextureRegions(0, 1, Region, TilePixels * sizeof(FColor), sizeof(FColor),
        reinterpret_cast<uint8*>(Pixels), [Pending](uint8* Data, const FUpdateTextureRegion2D* DoneRegion)
        {
            delete[] reinterpret_cast<FColor*>(Data);
            delete DoneRegion;
            Pending->Subtract(UploadBytes);
        });
    }
    Tile.Key = Key;
    Tile.Brush.SetResourceObject(Texture);
    Tile.Brush.ImageSize = FVector2D(TilePixels, TilePixels);
    Tile.Brush.DrawAs = ESlateBrushDrawType::Image;
    Tile.LastFrame = Frame;
    Textures.Add(Key, MoveTemp(Tile));
    ++Uploads;
    return true;
}

void SSkiSiteMapSpike::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime)
{
    ++Frame;
    const double Elapsed = CurrentTime - StartedAt;
    // Repeatable pan and zoom path, requiring no network or input automation.
    Pan = FVector2D(800.0 * FMath::Sin(Elapsed * 0.20), 600.0 * FMath::Sin(Elapsed * 0.13));
    Zoom = 0.75f + 0.65f * (0.5f + 0.5f * FMath::Sin(Elapsed * 0.31));
    const FVector2D Size = Geometry.GetLocalSize();
    const float Span = TilePixels * Zoom;
    const int32 MinX = FMath::FloorToInt(Pan.X / Span) - 1;
    const int32 MinY = FMath::FloorToInt(Pan.Y / Span) - 1;
    const int32 MaxX = FMath::CeilToInt((Pan.X + Size.X) / Span) + 1;
    const int32 MaxY = FMath::CeilToInt((Pan.Y + Size.Y) / Span) + 1;
    VisibleKeys.Reset();
    int32 Budget = MaxUploadsPerTick;
    for (int32 Y = MinY; Y <= MaxY; ++Y)
    for (int32 X = MinX; X <= MaxX; ++X)
    {
        const FIntPoint Key(X, Y);
        VisibleKeys.Add(Key);
        if (FTile* Existing = Textures.Find(Key)) { Existing->LastFrame = Frame; ++Hits; }
        else
        {
            ++Misses;
            if (Budget > 0 && UploadTile(Key)) --Budget;
        }
    }
    while (Textures.Num() > MaxResidentTiles)
    {
        FIntPoint OldestKey = FIntPoint::ZeroValue;
        uint64 OldestFrame = MAX_uint64;
        for (const TPair<FIntPoint, FTile>& Pair : Textures)
            if (Pair.Value.LastFrame < OldestFrame) { OldestFrame = Pair.Value.LastFrame; OldestKey = Pair.Key; }
        Textures.Remove(OldestKey);
        ++Evictions;
    }
    FSample Sample;
    Sample.TimeSeconds = Elapsed;
    Sample.FrameMilliseconds = (CurrentTime - LastTickAt) * 1000.0;
    Sample.UsedPhysicalBytes = FPlatformMemory::GetStats().UsedPhysical;
    if (LastTextureMemorySampleAt < 0.0 || CurrentTime - LastTextureMemorySampleAt >= 1.0)
    {
        FTextureMemoryStats TextureStats;
        RHIGetTextureMemoryStats(TextureStats);
        LastTextureMemoryBytes = TextureStats.StreamingMemorySize + TextureStats.NonStreamingMemorySize;
        LastTextureMemorySampleAt = CurrentTime;
    }
    Sample.TextureMemoryBytes = LastTextureMemoryBytes;
    Sample.GameThreadMilliseconds = FPlatformTime::ToMilliseconds(GGameThreadTime);
    Sample.RenderThreadMilliseconds = FPlatformTime::ToMilliseconds(GRenderThreadTime);
    const uint32 GpuCycles = RHIGetGPUFrameCycles();
    Sample.GpuMilliseconds = GpuCycles ? FPlatformTime::ToMilliseconds(GpuCycles) : 0.0;
    Sample.ViewportWidth = FMath::RoundToInt(Size.X);
    Sample.ViewportHeight = FMath::RoundToInt(Size.Y);
    Sample.ResidentTiles = Textures.Num();
    Sample.UploadedTiles = Uploads;
    Sample.Hits = Hits;
    Sample.Misses = Misses;
    Sample.Evictions = Evictions;
    Samples.Add(Sample);
    LastTickAt = CurrentTime;
    Uploads = Hits = Misses = Evictions = 0;
    Invalidate(EInvalidateWidgetReason::Paint);
}

void SSkiSiteMapSpike::AddContourLine(TArray<FSlateVertex>& Vertices, TArray<SlateIndex>& Indices,
    const FGeometry& Geometry, FVector2f Start, FVector2f End, float Width, FColor Color) const
{
    const FVector2f Direction = (End - Start).GetSafeNormal();
    const FVector2f Normal(-Direction.Y * Width * 0.5f, Direction.X * Width * 0.5f);
    const int32 Base = Vertices.Num();
    const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Start - Normal, FVector2f::ZeroVector, Color));
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Start + Normal, FVector2f::ZeroVector, Color));
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, End - Normal, FVector2f::ZeroVector, Color));
    Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, End + Normal, FVector2f::ZeroVector, Color));
    Indices.Append({static_cast<SlateIndex>(Base), static_cast<SlateIndex>(Base + 1), static_cast<SlateIndex>(Base + 2),
        static_cast<SlateIndex>(Base + 1), static_cast<SlateIndex>(Base + 3), static_cast<SlateIndex>(Base + 2)});
}

int32 SSkiSiteMapSpike::OnPaint(const FPaintArgs& Args, const FGeometry& Geometry,
    const FSlateRect& CullingRect, FSlateWindowElementList& OutDrawElements,
    int32 LayerId, const FWidgetStyle& Style, bool bParentEnabled) const
{
    const float Span = TilePixels * Zoom;
    for (const FIntPoint& Key : VisibleKeys)
    {
        const FTile* Tile = Textures.Find(Key);
        // A resident parent is allowed as an overzoom fallback while uploads are queued.
        if (!Tile) Tile = Textures.Find(FIntPoint(FMath::FloorToInt(Key.X / 2.0f), FMath::FloorToInt(Key.Y / 2.0f)));
        if (!Tile) continue;
        const FVector2D Position = FVector2D(Key.X * Span, Key.Y * Span) - Pan;
        FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
            Geometry.ToPaintGeometry(FVector2f(Position), FVector2f(Span, Span)), &Tile->Brush);
    }
    const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(FSlateBrush());
    for (const FIntPoint& Key : VisibleKeys)
    {
        if (!Textures.Contains(Key)) continue;
        const FVector2f Origin = FVector2f(Key.X * Span - Pan.X, Key.Y * Span - Pan.Y);
        TArray<FSlateVertex> Vertices;
        TArray<SlateIndex> Indices;
        Vertices.Reserve(128 * 4);
        Indices.Reserve(128 * 6);
        for (int32 Line = 0; Line < 128; ++Line)
        {
            const float Y = (Line + 0.5f) * Span / 128.0f;
            const float Bend = FMath::Sin((Key.X * 128 + Line) * 0.17f) * 12.0f;
            AddContourLine(Vertices, Indices, Geometry,
                Origin + FVector2f(0, Y + Bend), Origin + FVector2f(Span, Y - Bend),
                Line % 5 == 0 ? 2.0f : 1.0f, FColor::White);
        }
        FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId + 1, Handle,
            Vertices, Indices, nullptr, 0, 0);
    }
    return LayerId + 1;
}
