#include "SkiSiteMapSpikeRunner.h"

#include "SkiSiteMapSpike.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectIterator.h"

namespace
{
struct FCounts
{
    int32 Objects = 0;
    int32 Textures = 0;
    uint64 UsedPhysical = 0;
};

FCounts CountResources()
{
    FCounts Result;
    for (TObjectIterator<UObject> It; It; ++It)
    {
        ++Result.Objects;
        if (It->IsA<UTexture2D>()) ++Result.Textures;
    }
    Result.UsedPhysical = FPlatformMemory::GetStats().UsedPhysical;
    return Result;
}

double Percentile(TArray<double> Values, double Fraction)
{
    if (Values.IsEmpty()) return 0.0;
    Values.Sort();
    return Values[FMath::Clamp(FMath::CeilToInt(Fraction * Values.Num()) - 1, 0, Values.Num() - 1)];
}

struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TSharedPtr<SSkiSiteMapSpike> Widget;
    FString ReceiptPath;
    FString Token;
    FName LevelName;
    FCounts Baseline;
    FCounts After;
    TArray<SSkiSiteMapSpike::FSample> Samples;
    double Began = 0;
    double TeardownAt = 0;
    int32 Phase = 0;
    FTSTicker::FDelegateHandle Ticker;
};

TSharedPtr<FRun> ActiveRun;

void WriteReceipt(const FRun& Run)
{
    TArray<double> Frames;
    Frames.Reserve(Run.Samples.Num());
    uint64 PeakPhysical = 0;
    uint64 PeakTexture = 0;
    int32 PeakResident = 0;
    int32 Width = 0, Height = 0;
    TArray<double> GameTimes, RenderTimes, GpuTimes;
    int64 Hits = 0, Misses = 0, Evictions = 0, Uploads = 0;
    for (const auto& Sample : Run.Samples)
    {
        Frames.Add(Sample.FrameMilliseconds);
        PeakPhysical = FMath::Max(PeakPhysical, Sample.UsedPhysicalBytes);
        PeakTexture = FMath::Max(PeakTexture, Sample.TextureMemoryBytes);
        if (Sample.GameThreadMilliseconds > 0) GameTimes.Add(Sample.GameThreadMilliseconds);
        if (Sample.RenderThreadMilliseconds > 0) RenderTimes.Add(Sample.RenderThreadMilliseconds);
        if (Sample.GpuMilliseconds > 0) GpuTimes.Add(Sample.GpuMilliseconds);
        if (Sample.ViewportWidth > 0 && Sample.ViewportHeight > 0)
        { Width = Sample.ViewportWidth; Height = Sample.ViewportHeight; }
        PeakResident = FMath::Max(PeakResident, Sample.ResidentTiles);
        Hits += Sample.Hits;
        Misses += Sample.Misses;
        Evictions += Sample.Evictions;
        Uploads += Sample.UploadedTiles;
    }
    const bool bReturned = Run.After.Textures <= Run.Baseline.Textures
        && Run.After.Objects <= Run.Baseline.Objects;
    const auto Metric = [](const TArray<double>& Values)
    {
        return Values.IsEmpty() ? FString(TEXT("null"))
            : FString::Printf(TEXT("%.3f"), Percentile(Values, 0.95));
    };
    const FString GameTime = Metric(GameTimes);
    const FString RenderTime = Metric(RenderTimes);
    const FString GpuTime = Metric(GpuTimes);
    const FString Json = FString::Printf(
        TEXT("{\"scenario\":\"M0-A-native-map\",\"token\":\"%s\",\"resolution\":\"%dx%d\",\"durationSeconds\":300,\"frames\":%d,")
        TEXT("\"frameMs\":{\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f},")
        TEXT("\"memory\":{\"baselinePhysicalBytes\":%llu,\"peakPhysicalBytes\":%llu,\"afterPhysicalBytes\":%llu,")
        TEXT("\"baselineTextures\":%d,\"afterTextures\":%d,\"baselineUObjects\":%d,\"afterUObjects\":%d},")
        TEXT("\"tiles\":{\"peakResident\":%d,\"hits\":%lld,\"misses\":%lld,\"evictions\":%lld,\"uploads\":%lld},")
        TEXT("\"teardownToBaseline\":%s,\"gameThreadMs\":%s,\"renderThreadMs\":%s,\"gpuMs\":%s,")
        TEXT("\"peakCpuBytes\":%llu,\"peakTextureBytes\":%llu}"),
        *Run.Token, Width, Height, Run.Samples.Num(), Percentile(Frames, 0.50), Percentile(Frames, 0.95), Percentile(Frames, 0.99),
        Run.Baseline.UsedPhysical, PeakPhysical, Run.After.UsedPhysical,
        Run.Baseline.Textures, Run.After.Textures, Run.Baseline.Objects, Run.After.Objects,
        PeakResident, Hits, Misses, Evictions, Uploads, bReturned ? TEXT("true") : TEXT("false"),
        *GameTime, *RenderTime, *GpuTime, PeakPhysical, PeakTexture);
    FFileHelper::SaveStringToFile(Json, *Run.ReceiptPath);
}
}

bool StartSkiSiteMapSpike(UWorld* World, const FString& ReceiptPath, const FString& Token)
{
    // The scripted OpenLevel re-enters the bootstrap while the ticker is still
    // recording teardown. Accept that dispatch without creating another map.
    if (ActiveRun.IsValid()) return true;
    FGuid ParsedToken;
    if (!World || !World->GetGameViewport() || ReceiptPath.IsEmpty() || !FGuid::Parse(Token, ParsedToken)) return false;
    ActiveRun = MakeShared<FRun>();
    TSharedPtr<FRun> Run = ActiveRun;
    Run->World = World;
    Run->ReceiptPath = ReceiptPath;
    Run->Token = Token;
    Run->LevelName = FName(*World->GetMapName());
    Run->Baseline = CountResources();
    Run->Began = FPlatformTime::Seconds();
    Run->Widget = SNew(SSkiSiteMapSpike);
    World->GetGameViewport()->AddViewportWidgetContent(Run->Widget.ToSharedRef());
    Run->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float)
    {
        const double Now = FPlatformTime::Seconds();
        if (Run->Phase == 0 && Now - Run->Began >= 300.0)
        {
            Run->Samples = Run->Widget->GetSamples();
            if (UWorld* CurrentWorld = Run->World.Get())
            {
                if (UGameViewportClient* Viewport = CurrentWorld->GetGameViewport())
                    Viewport->RemoveViewportWidgetContent(Run->Widget.ToSharedRef());
                Run->Widget.Reset();
                UGameplayStatics::OpenLevel(CurrentWorld, Run->LevelName);
            }
            else Run->Widget.Reset();
            Run->TeardownAt = Now;
            Run->Phase = 1;
        }
        else if (Run->Phase == 1 && Now - Run->TeardownAt >= 3.0)
        {
            CollectGarbage(RF_NoFlags);
            Run->After = CountResources();
            WriteReceipt(*Run);
            ActiveRun.Reset();
            FPlatformMisc::RequestExitWithStatus(false, 0);
            return false;
        }
        return true;
    }));
    return true;
}
