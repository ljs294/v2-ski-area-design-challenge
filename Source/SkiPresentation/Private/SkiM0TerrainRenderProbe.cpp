#include "SkiM0TerrainRenderProbe.h"

#include "SkiTerrainViewController.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"
#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiApplication/TerrainCoreSession.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "HighResScreenshot.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#include <memory>

namespace
{
struct FM0RenderState
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ASkiTerrainActor> Actor;
    TSharedPtr<SkiApplication::TerrainCoreSession> Session;
    std::shared_ptr<SkiPreparation::TerrainCorePackageStore> Store;
    FString Root;
    FString Receipt;
    FString Screenshot;
    FString Token;
    double Started = 0.0;
    int32 ScreenshotPolls = 0;
};

bool IsSafeM0Root(const FString& Candidate)
{
    FString Root = FPaths::ConvertRelativePathToFull(Candidate);
    FPaths::CollapseRelativeDirectories(Root);
    Root.ReplaceInline(TEXT("\\"), TEXT("/"));
    FString Base = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("TerrainCoreM0Scale")));
    FPaths::CollapseRelativeDirectories(Base);
    Base.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!Root.StartsWith(Base + TEXT("/"), ESearchCase::IgnoreCase)
        || Root.Find(TEXT("/../")) != INDEX_NONE
        || !IFileManager::Get().DirectoryExists(*Root)) return false;
#if PLATFORM_WINDOWS
    FString Current = Base;
    const DWORD BaseAttributes = GetFileAttributesW(*Base);
    if (BaseAttributes == INVALID_FILE_ATTRIBUTES
        || (BaseAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    const FString Relative = Root.RightChop(Base.Len() + 1);
    TArray<FString> Parts;
    Relative.ParseIntoArray(Parts, TEXT("/"), true);
    for (const FString& Part : Parts)
    {
        Current = FPaths::Combine(Current, Part);
        const DWORD Attributes = GetFileAttributesW(*Current);
        if (Attributes == INVALID_FILE_ATTRIBUTES
            || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    }
#endif
    return true;
}

void Finish(const TSharedRef<FM0RenderState>& State, bool Passed,
    const FString& Error = {}, double FirstVisibleSeconds = -1.0,
    const FString& ScreenshotHash = {})
{
    const int32 Rendered = State->Actor.IsValid()
        ? State->Actor->GetRenderedTerrainCoreTileCount() : 0;
    FString SafeError = Error;
    SafeError.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    SafeError.ReplaceInline(TEXT("\""), TEXT("\\\""));
    FString SafeScreenshot = State->Screenshot;
    SafeScreenshot.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    SafeScreenshot.ReplaceInline(TEXT("\""), TEXT("\\\""));
    const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"scenario\":\"m0-terraincore-render\",\"passed\":%s,\"firstVisibleTerrainSeconds\":%.6f,\"screenshotSha256\":\"%s\",\"screenshotPath\":\"%s\",\"renderedTiles\":%d,\"error\":\"%s\"}"),
        *State->Token, Passed ? TEXT("true") : TEXT("false"),
        FirstVisibleSeconds, *ScreenshotHash, *SafeScreenshot, Rendered, *SafeError);
    const bool Written = FFileHelper::SaveStringToFile(Receipt, *State->Receipt,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    if (State->Actor.IsValid()) State->Actor->Destroy();
    State->Session.Reset();
    State->Store.reset();
    const bool Cleaned = IsSafeM0Root(State->Root)
        && IFileManager::Get().DeleteDirectory(*State->Root, false, true);
    FPlatformMisc::RequestExitWithStatus(false, Passed && Written && Cleaned ? 0 : 1);
}

void PollScreenshot(const TSharedRef<FM0RenderState>& State)
{
    if (!State->World.IsValid()) { Finish(State, false, TEXT("World ended before screenshot.")); return; }
    TArray<uint8> Bytes;
    if (FFileHelper::LoadFileToArray(Bytes, *State->Screenshot) && Bytes.Num() > 1024)
    {
        const FString Hash = SkiPreparation::Sha256(MakeArrayView(Bytes));
        Finish(State, true, {}, FPlatformTime::Seconds() - State->Started, Hash);
        return;
    }
    if (++State->ScreenshotPolls > 100)
    {
        Finish(State, false, TEXT("Terrain screenshot timed out."));
        return;
    }
    FTimerHandle Timer;
    State->World->GetTimerManager().SetTimer(Timer,
        FTimerDelegate::CreateLambda([State] { PollScreenshot(State); }), 0.1F, false);
}

void RequestScreenshot(const TSharedRef<FM0RenderState>& State)
{
    if (!State->World.IsValid() || !GEngine || !GEngine->GameViewport
        || !GEngine->GameViewport->Viewport)
    {
        Finish(State, false, TEXT("Offscreen game viewport is unavailable."));
        return;
    }
    FScreenshotRequest::RequestScreenshot(State->Screenshot, false, false, false);
    PollScreenshot(State);
}

void PollRender(const TSharedRef<FM0RenderState>& State)
{
    if (!State->World.IsValid() || !State->Actor.IsValid())
    {
        Finish(State, false, TEXT("Terrain actor ended before render."));
        return;
    }
    if (FPlatformTime::Seconds() - State->Started > 60.0)
    {
        Finish(State, false, TEXT("Terrain mesh publication timed out."));
        return;
    }
    if (State->Actor->GetRenderedTerrainCoreTileCount() > 0
        && State->Actor->IsTerrainCoreRevisionAligned())
    {
        ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(
            State->World->GetFirstPlayerController());
        if (!Controller) { Finish(State, false, TEXT("Terrain view controller is unavailable.")); return; }
        Controller->AttachTerrain(State->Actor.Get());
        Controller->DisableInput(Controller);
        // Let the camera and mesh components reach the render thread before capture.
        FTimerHandle Timer;
        State->World->GetTimerManager().SetTimer(Timer,
            FTimerDelegate::CreateLambda([State] { RequestScreenshot(State); }),
            0.1F, false);
        return;
    }
    FTimerHandle Timer;
    State->World->GetTimerManager().SetTimer(Timer,
        FTimerDelegate::CreateLambda([State] { PollRender(State); }), 0.05F, false);
}
}

bool StartM0TerrainRenderProbe(UWorld* World, const FString& PackageRoot,
    const FString& ContentId, const FString& ReceiptPath, const FString& Token)
{
    if (!World || !FParse::Param(FCommandLine::Get(), TEXT("RenderOffScreen"))) return false;
    const FString Root = FPaths::ConvertRelativePathToFull(PackageRoot);
    auto State = MakeShared<FM0RenderState>();
    State->World = World;
    State->Root = Root;
    State->Receipt = FPaths::ConvertRelativePathToFull(ReceiptPath);
    State->Token = Token;
    State->Screenshot = FPaths::Combine(FPaths::GetPath(State->Receipt), Token + TEXT(".png"));
    State->Started = FPlatformTime::Seconds();
    if (!IsSafeM0Root(Root))
    { Finish(State, false, TEXT("Retained package root is unsafe.")); return true; }
    if (IFileManager::Get().FileExists(*State->Screenshot))
    { Finish(State, false, TEXT("Screenshot path already exists.")); return true; }
    State->Store = std::make_shared<SkiPreparation::TerrainCorePackageStore>(Root);
    SkiPreparation::TerrainCorePackageIndex Index;
    FString Error;
    if (!State->Store->Open(ContentId, Index, Error))
    { Finish(State, false, Error); return true; }
    std::string RepositoryError;
    auto Repository = SkiApplication::TerrainCoreRepository::Create(Index.Manifest,
        [Store = State->Store, Index](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& Out, std::string& ReadError)
        {
            SkiPreparation::TerrainCoreDecodedTile Tile;
            FString Error;
            if (!Store->ReadTile(Index, Descriptor.LodIndex, Descriptor.TileX,
                    Descriptor.TileY, Tile, Error))
            { ReadError = TCHAR_TO_UTF8(*Error); return false; }
            Out.Key = {Descriptor.LodIndex, Descriptor.TileX, Descriptor.TileY};
            Out.Descriptor = Tile.Descriptor;
            Out.Heights.assign(Tile.Heights.GetData(), Tile.Heights.GetData() + Tile.Heights.Num());
            Out.Validity.assign(Tile.Validity.GetData(), Tile.Validity.GetData() + Tile.Validity.Num());
            return true;
        }, RepositoryError);
    State->Session = MakeShared<SkiApplication::TerrainCoreSession>();
    if (!Repository || !State->Session->Install(Repository, 1))
    { Finish(State, false, UTF8_TO_TCHAR(RepositoryError.c_str())); return true; }
    State->Actor = World->SpawnActor<ASkiTerrainActor>();
    if (!State->Actor.IsValid() || !State->Actor->BeginTerrainCoreStreaming(State->Session, 4))
    { Finish(State, false, TEXT("LOD4 terrain streaming could not start.")); return true; }
    PollRender(State);
    return true;
}
