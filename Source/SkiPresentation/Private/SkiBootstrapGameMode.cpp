#include "SkiBootstrapGameMode.h"
#include "SkiBootstrapWidget.h"
#include "SkiP1Widget.h"
#include "SkiTerrainViewController.h"
#include "SkiApplication/Bootstrap.h"
#include "SkiPreparation/FixtureTerrainProvider.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/NativeTerrainProvider.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "HighResScreenshot.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Base64.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

ASkiBootstrapGameMode::ASkiBootstrapGameMode()
{
    PlayerControllerClass = ASkiTerrainViewController::StaticClass();
}

void ASkiBootstrapGameMode::BeginPlay()
{
    Super::BeginPlay();
    SkiPreparation::InitializePreparationDiagnostics(FPaths::ProjectSavedDir());
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    const bool ExpectedMap = GetWorld()->GetOutermost()->GetName() == TEXT("/Game/P0Generated/Bootstrap");

    // Explicit local startup probe, available in Shipping without enabling logging,
    // an automation listener, or editor modules. This does not qualify GPU visuals.
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP0Smoke")))
    {
        UClass* WidgetClass = LoadClass<USkiBootstrapWidget>(nullptr, TEXT("/Game/P0Generated/WBP_Bootstrap.WBP_Bootstrap_C"));
        USkiBootstrapWidget* Widget = Controller && WidgetClass ? CreateWidget<USkiBootstrapWidget>(Controller, WidgetClass) : nullptr;
        const bool Ready = ExpectedMap && SkiApplication::CheckDomainBoundary() && Widget && Widget->IsBootstrapReady();
        if (Widget) Widget->AddToViewport();
        FString ReceiptPath;
        FString Token;
        FGuid ParsedToken;
        const bool ArgumentsValid = FParse::Value(FCommandLine::Get(), TEXT("SkiP0Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP0Token="), Token) && FGuid::Parse(Token, ParsedToken);
        const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"ready\":%s}"), *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), Ready ? TEXT("true") : TEXT("false"));
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath);
        FPlatformMisc::RequestExitWithStatus(false, Ready && Written ? 0 : 1);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1Smoke")))
    {
        FPlatformMisc::RequestExitWithStatus(false, RunP1Smoke() ? 0 : 1);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1VisualCapture")))
    {
        if (!BeginP1VisualCapture()) FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }

    P1Widget = Controller ? CreateWidget<USkiP1Widget>(Controller, USkiP1Widget::StaticClass()) : nullptr;
    if (!ExpectedMap || !SkiApplication::CheckDomainBoundary() || !P1Widget || !P1Widget->IsP1Ready()) return;
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1SelectorSmoke")))
    {
        P1Widget->SetSelectionHandler([](const SkiPreparation::Request& Request)
        {
            FString DataRoot, ReceiptPath, Token;
            FGuid ParsedToken;
            const bool ArgumentsValid = FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
                && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
                && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token)
                && FGuid::Parse(Token, ParsedToken);
            DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
            ReceiptPath = FPaths::ConvertRelativePathToFull(ReceiptPath);
            FString RootPrefix = DataRoot;
            if (!RootPrefix.EndsWith(TEXT("/")) && !RootPrefix.EndsWith(TEXT("\\"))) RootPrefix += TEXT("/");
            RootPrefix.ReplaceInline(TEXT("\\"), TEXT("/"));
            FString NormalReceipt = ReceiptPath;
            NormalReceipt.ReplaceInline(TEXT("\\"), TEXT("/"));
            const bool PathValid = NormalReceipt.StartsWith(RootPrefix)
                && FPaths::GetCleanFilename(ReceiptPath) == Token + TEXT(".receipt.json");
            const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"selector\":true,\"profile\":\"%s\"}"),
                *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower),
                Request.Profile == SkiPreparation::SourceProfile::Standard ? TEXT("standard") : TEXT("high"));
            const bool Written = ArgumentsValid && PathValid && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            FPlatformMisc::RequestExitWithStatus(false, Written ? 0 : 1);
        });
    }
    else P1Widget->SetSelectionHandler([this](const SkiPreparation::Request& Request) { BeginP1Preparation(Request); });
    P1Widget->AddToViewport();
    Controller->bShowMouseCursor = true;
    Controller->SetInputMode(FInputModeUIOnly());
}

bool ASkiBootstrapGameMode::BeginP1VisualCapture()
{
    FString DataRoot;
    FGuid ParsedToken;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), VisualReceiptPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Screenshot="), VisualScreenshotPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), VisualToken)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1CaptureMode="), VisualMode)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1CaptureLighting="), VisualLighting)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1CaptureView="), VisualView)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1CaptureLod="), VisualLod)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1CaptureWidth="), VisualCaptureWidth)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1CaptureHeight="), VisualCaptureHeight)
        || !FGuid::Parse(VisualToken, ParsedToken) || VisualLod < 0 || VisualLod > 2) return false;
    if (VisualCaptureWidth < 1280 || VisualCaptureWidth > 4096
        || VisualCaptureHeight < 720 || VisualCaptureHeight > 4096) return false;
    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    VisualReceiptPath = FPaths::ConvertRelativePathToFull(VisualReceiptPath);
    VisualScreenshotPath = FPaths::ConvertRelativePathToFull(VisualScreenshotPath);
    FString Prefix = DataRoot.Replace(TEXT("\\"), TEXT("/")); if (!Prefix.EndsWith(TEXT("/"))) Prefix += TEXT("/");
    const FString ReceiptNormal = VisualReceiptPath.Replace(TEXT("\\"), TEXT("/"));
    const FString ScreenshotNormal = VisualScreenshotPath.Replace(TEXT("\\"), TEXT("/"));
    if (!ReceiptNormal.StartsWith(Prefix) || !ScreenshotNormal.StartsWith(Prefix)
        || FPaths::GetCleanFilename(VisualReceiptPath) != VisualToken + TEXT(".receipt.json")
        || FPaths::GetCleanFilename(VisualScreenshotPath) != VisualToken + TEXT(".png")) return false;

    SkiDomain::TerrainManifest VisualManifest;
    SkiDomain::Heightfield VisualField;
    TArray<uint8> VisualCover;
    FString ExistingContentId;
    if (FParse::Value(FCommandLine::Get(), TEXT("SkiP1ContentId="), ExistingContentId))
    {
        FString Error;
        SkiPreparation::PackageStore Store(DataRoot);
        if (!Store.Load(ExistingContentId, VisualManifest, VisualField, Error, &VisualCover)) return false;
    }
    else
    {
        SkiPreparation::Request Request;
        Request.Name = TEXT("Crystal Mountain synthetic visual fixture");
        Request.Bounds = {-121.489, 46.925, -121.462, 46.947};
        Request.Profile = SkiPreparation::SourceProfile::Standard;
        Request.SessionGeneration = 1; Request.OperationGeneration = 1;
        SkiPreparation::FixtureTerrainProvider Provider(DataRoot);
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::Result Result = Provider.Prepare(Request, Cancellation, {});
        if (!Result.Ok) return false;
        VisualManifest = std::move(Result.Manifest);
        VisualField = std::move(Result.Heightfield);
        VisualCover = std::move(Result.Cover);
    }
    TerrainSession = MakeShared<SkiApplication::TerrainSession>();
    std::vector<std::uint8_t> RuntimeCover(VisualCover.GetData(), VisualCover.GetData() + VisualCover.Num());
    if (!TerrainSession->Install(std::move(VisualField), std::move(VisualManifest), std::move(RuntimeCover))) return false;
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>(); TerrainActor->SetTerrainSession(TerrainSession);
    if (!TerrainActor->Present(TerrainSession->Snapshot(), static_cast<uint8>(VisualLod))) return false;
    TerrainSession->AcknowledgeQuery(TerrainSession->Snapshot().Readiness.Canonical);
    TerrainActor->SetLightingPreset(FName(*VisualLighting));
    if (VisualMode == TEXT("elevation")) TerrainActor->SetViewMode(ESkiTerrainViewMode::Elevation);
    else if (VisualMode == TEXT("slope")) TerrainActor->SetViewMode(ESkiTerrainViewMode::Slope);
    else if (VisualMode == TEXT("cover")) TerrainActor->SetViewMode(ESkiTerrainViewMode::Cover);
    else if (VisualMode == TEXT("lod")) TerrainActor->SetViewMode(ESkiTerrainViewMode::TileLod);
    ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(GetWorld()->GetFirstPlayerController());
    if (!Controller) return false;
    Controller->AttachTerrain(TerrainActor); if (VisualView == TEXT("close")) Controller->FrameSteepest();
    // Qualification captures are driven entirely by tokened command-line
    // arguments. Ignore incidental physical input while the temporary window is
    // open so camera receipts and framing remain repeatable.
    Controller->DisableInput(Controller);
    if (VisualMode == TEXT("topology"))
    {
        FBox Bounds;
        if (TerrainActor->GetSteepestQuadrantWorldBounds(Bounds))
        {
            const SkiDomain::RayHit Hit = TerrainActor->QueryCanonical(
                FVector(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z + 1000000.0), FVector(0,0,-1));
            if (Hit.Hit) TerrainActor->ShowTopologyPatch(Hit);
        }
    }
    P1Widget = CreateWidget<USkiP1Widget>(Controller, USkiP1Widget::StaticClass());
    if (P1Widget)
    {
        P1Widget->AddToViewport();
        P1Widget->SetTerrainDetails(TEXT("Crystal Mountain synthetic fixture\nVerification capture at 1x vertical scale"), true);
        P1Widget->CloseSelector();
    }
    GetWorldTimerManager().SetTimerForNextTick(this, &ASkiBootstrapGameMode::RequestP1VisualScreenshot);
    return true;
}

void ASkiBootstrapGameMode::RequestP1VisualScreenshot()
{
    FTimerHandle Handle;
    GetWorldTimerManager().SetTimer(Handle, [this]
    {
        if (VisualCaptureWidth == 2560 && VisualCaptureHeight == 1080)
        {
            FScreenshotRequest::RequestScreenshot(VisualScreenshotPath, true, false, false);
        }
        else
        {
            FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
            Config.SetFilename(VisualScreenshotPath);
            if (!Config.SetResolution(VisualCaptureWidth, VisualCaptureHeight, 1.0F)
                || !GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport
                || !GEngine->GameViewport->Viewport->TakeHighResScreenShot())
            {
                FPlatformMisc::RequestExitWithStatus(false, 1);
                return;
            }
        }
        FTimerHandle FinishHandle;
        GetWorldTimerManager().SetTimer(FinishHandle, this, &ASkiBootstrapGameMode::FinishP1VisualCapture, 2.0F, false);
    }, 2.0F, false);
}

void ASkiBootstrapGameMode::FinishP1VisualCapture()
{
    if (!FPaths::FileExists(VisualScreenshotPath)) { FPlatformMisc::RequestExitWithStatus(false, 1); return; }
    TArray<uint8> Bytes; if (!FFileHelper::LoadFileToArray(Bytes, *VisualScreenshotPath)) { FPlatformMisc::RequestExitWithStatus(false, 1); return; }
    const SkiApplication::TerrainSnapshot Snapshot = TerrainSession->Snapshot();
    int32 Width = 0, Height = 0; GetWorld()->GetFirstPlayerController()->GetViewportSize(Width, Height);
    const SkiDomain::GeographicBounds Bounds = Snapshot.Manifest ? Snapshot.Manifest->ActualBounds : SkiDomain::GeographicBounds{};
    const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"contentId\":\"%s\",\"packageHash\":\"%s\",\"bounds\":[%.9f,%.9f,%.9f,%.9f],\"datum\":\"%s\",\"dimensions\":[%u,%u],\"spacing\":[%.6f,%.6f],\"revisions\":[%llu,%llu,%llu],\"camera\":\"%s\",\"fov\":50,\"lod\":%d,\"lighting\":\"%s\",\"diagnosticMode\":\"%s\",\"view\":\"%s\",\"rhi\":\"%s\",\"resolution\":[%d,%d],\"viewport\":[%d,%d],\"internalResolutionPercent\":100,\"screenshotSha256\":\"%s\"}"),
        *VisualToken, Snapshot.Manifest ? UTF8_TO_TCHAR(Snapshot.Manifest->ContentId.c_str()) : TEXT(""),
        Snapshot.Manifest ? UTF8_TO_TCHAR(Snapshot.Manifest->ContentId.c_str()) : TEXT(""),
        Bounds.WestDeg, Bounds.SouthDeg, Bounds.EastDeg, Bounds.NorthDeg,
        Snapshot.Manifest ? UTF8_TO_TCHAR(Snapshot.Manifest->VerticalDatum.c_str()) : TEXT("unknown"),
        Snapshot.Heightfield->Width, Snapshot.Heightfield->Height, Snapshot.Heightfield->EastSpacingM,
        Snapshot.Heightfield->NorthSpacingM, static_cast<uint64>(Snapshot.Readiness.Canonical),
        static_cast<uint64>(Snapshot.Readiness.Render), static_cast<uint64>(Snapshot.Readiness.Query),
        *Cast<ASkiTerrainViewController>(GetWorld()->GetFirstPlayerController())->DescribeCamera(), VisualLod,
        *VisualLighting, *VisualMode, *VisualView, *FApp::GetGraphicsRHI(), VisualCaptureWidth,
        VisualCaptureHeight, Width, Height, *SkiPreparation::Sha256(Bytes));
    const bool Written = FFileHelper::SaveStringToFile(Receipt, *VisualReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FPlatformMisc::RequestExitWithStatus(false, Written ? 0 : 1);
}

bool ASkiBootstrapGameMode::RunP1Smoke()
{
    FString DataRoot;
    FString ReceiptPath;
    FString Token;
    FString Scenario;
    FString ContentId;
    FGuid ParsedToken;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Scenario="), Scenario)
        || !FGuid::Parse(Token, ParsedToken)) return false;
    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    ReceiptPath = FPaths::ConvertRelativePathToFull(ReceiptPath);
    FString RootPrefix = DataRoot;
    if (!RootPrefix.EndsWith(TEXT("/")) && !RootPrefix.EndsWith(TEXT("\\"))) RootPrefix += TEXT("/");
    RootPrefix.ReplaceInline(TEXT("\\"), TEXT("/"));
    FString NormalReceipt = ReceiptPath;
    NormalReceipt.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!NormalReceipt.StartsWith(RootPrefix) || FPaths::GetCleanFilename(ReceiptPath) != Token + TEXT(".receipt.json"))
        return false;

    if (Scenario == TEXT("geotiff-regression"))
    {
        FString Encoded;
        TArray<uint8> Bytes;
        const FString Fixture = FPaths::Combine(FPaths::ProjectContentDir(),
            TEXT("P1Fixtures/usgs-tiled-nodata-synthetic.tif.base64"));
        if (!FFileHelper::LoadFileToString(Encoded, *Fixture)
            || !FBase64::Decode(Encoded.TrimStartAndEnd(), Bytes)
            || SkiPreparation::Sha256(Bytes) != TEXT("4614b0cec77c843a6b00ec7d0a4b3b90511031ee9eb73f185c5656088fe599bf"))
            return false;
        SkiPreparation::DecodedElevationRaster Raster;
        SkiPreparation::ProviderFailure Failure;
        if (!SkiPreparation::DecodeElevationGeoTiff(Bytes, {-121.5, 46.982, -121.481, 47.0},
                SkiPreparation::ProviderProduct::CoreElevation, Raster, Failure)) return false;
        const FString Receipt = FString::Printf(
            TEXT("{\"token\":\"%s\",\"scenario\":\"geotiff-regression\",\"fixtureSha256\":\"%s\",\"dimensions\":[%u,%u],\"bounds\":[%.6f,%.6f,%.6f,%.6f],\"spacing\":[%.6f,%.6f],\"nodata\":%.1f,\"organization\":\"tiled\",\"decoded\":true}"),
            *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), *SkiPreparation::Sha256(Bytes),
            Raster.SourceWidth, Raster.SourceHeight, Raster.ActualOuterBounds.WestDeg,
            Raster.ActualOuterBounds.SouthDeg, Raster.ActualOuterBounds.EastDeg,
            Raster.ActualOuterBounds.NorthDeg, Raster.Heightfield.EastSpacingM,
            Raster.Heightfield.NorthSpacingM, Raster.NoDataValue);
        return FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    SkiPreparation::PackageStore Store(DataRoot);
    SkiDomain::TerrainManifest Manifest;
    SkiDomain::Heightfield Field;
    TArray<uint8> Cover;
    FString Error;
    if (Scenario == TEXT("import"))
    {
        SkiPreparation::Request Request;
        Request.Name = TEXT("Crystal Mountain P1 qualification");
        Request.Bounds = {-121.489, 46.925, -121.462, 46.947};
        Request.Profile = SkiPreparation::SourceProfile::Standard;
        Request.SessionGeneration = 1;
        Request.OperationGeneration = 1;
        SkiPreparation::FixtureTerrainProvider Provider(DataRoot);
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::Result Result = Provider.Prepare(Request, Cancellation, {});
        if (!Result.Ok) return false;
        Manifest = std::move(Result.Manifest);
        Field = std::move(Result.Heightfield);
        Cover = std::move(Result.Cover);
        ContentId = UTF8_TO_TCHAR(Manifest.ContentId.c_str());
    }
    else if (Scenario == TEXT("offline-reopen"))
    {
        if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1ContentId="), ContentId)
            || !Store.Load(ContentId, Manifest, Field, Error, &Cover)) return false;
    }
    else return false;

    TerrainSession = MakeShared<SkiApplication::TerrainSession>();
    std::vector<std::uint8_t> RuntimeCover(Cover.GetData(), Cover.GetData() + Cover.Num());
    if (!TerrainSession->Install(std::move(Field), Manifest, std::move(RuntimeCover))) return false;
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    TerrainActor->SetTerrainSession(TerrainSession);
    if (!TerrainActor->Present(TerrainSession->Snapshot(), 0)
        || !TerrainActor->Present(TerrainSession->Snapshot(), 1)
        || !TerrainActor->Present(TerrainSession->Snapshot(), 2)) return false;
    TerrainSession->AcknowledgeQuery(TerrainSession->Snapshot().Readiness.Canonical);
    const SkiDomain::RayHit Hit = TerrainActor->QueryCanonical(FVector(0, 0, 400000), FVector(0, 0, -1));
    if (!Hit.Hit) return false;
    if (Scenario == TEXT("import"))
    {
        if (!TerrainActor->ApplyScratchMutation(FVector2D(Hit.Position.East, Hit.Position.North), 50.0, 3.0)) return false;
        TerrainSession->AcknowledgeQuery(TerrainSession->Snapshot().Readiness.Canonical);
    }
    const SkiApplication::TerrainSnapshot Snapshot = TerrainSession->Snapshot();
    const bool Ready = Snapshot.Readiness.IsReady();
    TerrainActor->Destroy();
    TerrainActor = nullptr;
    TerrainSession.Reset();
    SkiDomain::TerrainManifest ReopenedManifest;
    SkiDomain::Heightfield ReopenedField;
    const bool Reopened = Store.Load(ContentId, ReopenedManifest, ReopenedField, Error);
    const FString Receipt = FString::Printf(
        TEXT("{\"token\":\"%s\",\"scenario\":\"%s\",\"contentId\":\"%s\",\"ready\":%s,\"picked\":true,\"reopened\":%s,\"assetCount\":%d}"),
        *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), *Scenario, *ContentId,
        Ready ? TEXT("true") : TEXT("false"), Reopened ? TEXT("true") : TEXT("false"),
        static_cast<int32>(ReopenedManifest.Assets.size()));
    return Ready && Reopened && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void ASkiBootstrapGameMode::BeginP1Preparation(const SkiPreparation::Request& Request)
{
    if (PreparationCancellation) PreparationCancellation->Cancel();
    PreparationCancellation = MakeShared<SkiPreparation::Cancellation>();
    LastRequest = Request;
    ActiveSessionGeneration = Request.SessionGeneration;
    ActiveOperationGeneration = Request.OperationGeneration;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = PreparationCancellation.ToSharedRef();
    const FString DataRoot = FPaths::ProjectSavedDir();
    const bool UseFixture = FParse::Param(FCommandLine::Get(), TEXT("SkiUseFixtureProvider"));
    const TWeakObjectPtr<ASkiBootstrapGameMode> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, Request, Cancellation, DataRoot, UseFixture]
    {
        TUniquePtr<SkiPreparation::Provider> Provider;
        if (UseFixture) Provider = MakeUnique<SkiPreparation::FixtureTerrainProvider>(DataRoot);
        else Provider = MakeUnique<SkiPreparation::NativeTerrainProvider>(DataRoot);
        SkiPreparation::Result Result = Provider->Prepare(Request, Cancellation,
            [WeakThis, Request, Cancellation](const SkiPreparation::Progress& Progress)
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis, Request, Cancellation, Progress]
                {
                    if (WeakThis.IsValid() && WeakThis->P1Widget && !Cancellation->IsCancelled()
                        && WeakThis->ActiveSessionGeneration == Request.SessionGeneration
                        && WeakThis->ActiveOperationGeneration == Request.OperationGeneration)
                        WeakThis->P1Widget->SetPreparationProgress(Progress);
                });
            });
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Request, Cancellation, Result = std::move(Result)]() mutable
        {
            if (WeakThis.IsValid()) WeakThis->FinishP1Preparation(std::move(Result),
                Request.SessionGeneration, Request.OperationGeneration, Cancellation);
        });
    });
}

void ASkiBootstrapGameMode::FinishP1Preparation(SkiPreparation::Result Result,
    const uint64 SessionGeneration, const uint64 OperationGeneration,
    TSharedRef<SkiPreparation::Cancellation> Cancellation)
{
    if (Cancellation->IsCancelled() || ActiveSessionGeneration != SessionGeneration
        || ActiveOperationGeneration != OperationGeneration) return;
    if (!Result.Ok)
    {
        if (P1Widget) P1Widget->ShowPreparationFailure(Result.Failure, Result.Error,
            [this] { RetryPreparation(); }, [this] { ChangeSelection(); });
        return;
    }
    const TArray<FString> Warnings = Result.Warnings;
    const bool bSynthetic = Result.Manifest.Source.find("fixture") != std::string::npos;
    const FString Details = FString::Printf(
        TEXT("%s\n%u × %u samples | %.2f × %.2f m spacing\nBounds %.6f, %.6f — %.6f, %.6f\nDatum %s | LOD 0 | triangle step 1"),
        UTF8_TO_TCHAR(Result.Manifest.Source.c_str()), Result.Manifest.HeightWidth, Result.Manifest.HeightHeight,
        Result.Manifest.EastSpacingM, Result.Manifest.NorthSpacingM,
        Result.Manifest.ActualBounds.WestDeg, Result.Manifest.ActualBounds.SouthDeg,
        Result.Manifest.ActualBounds.EastDeg, Result.Manifest.ActualBounds.NorthDeg,
        UTF8_TO_TCHAR(Result.Manifest.VerticalDatum.c_str()));
    TerrainSession = MakeShared<SkiApplication::TerrainSession>();
    std::vector<std::uint8_t> RuntimeCover(Result.Cover.GetData(),
        Result.Cover.GetData() + Result.Cover.Num());
    if (!TerrainSession->Install(std::move(Result.Heightfield), std::move(Result.Manifest),
            std::move(RuntimeCover)))
    {
        if (P1Widget) P1Widget->SetTransientStatus(TEXT("Installed package could not enter the terrain session."));
        return;
    }
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    TerrainActor->SetTerrainSession(TerrainSession);
    if (!TerrainActor->Present(TerrainSession->Snapshot()))
    {
        if (P1Widget) P1Widget->SetTransientStatus(TEXT("Terrain renderer rejected the canonical snapshot."));
        return;
    }
    TerrainSession->AcknowledgeQuery(TerrainSession->Snapshot().Readiness.Canonical);
    TerrainActor->SetLightingPreset(TEXT("Midday"));
    if (ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(GetWorld()->GetFirstPlayerController()))
    {
        Controller->AttachTerrain(TerrainActor);
        Controller->SetStatusHandler([WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)](const FString& Status)
        { if (WeakWidget.IsValid()) WeakWidget->SetProbeStatus(Status); });
        Controller->bShowMouseCursor = true;
        Controller->SetInputMode(FInputModeGameAndUI());
    }
    if (P1Widget)
    {
        P1Widget->SetTerrainDetails(Details, bSynthetic);
        P1Widget->SetViewCommandHandler([WeakTerrain = TWeakObjectPtr<ASkiTerrainActor>(TerrainActor)](const FName Command)
        {
            if (!WeakTerrain.IsValid()) return;
            if (Command == TEXT("Elevation")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Elevation);
            else if (Command == TEXT("Slope")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Slope);
            else if (Command == TEXT("Cover")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Cover);
            else if (Command == TEXT("Lod")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::TileLod);
            else if (Command == TEXT("Lod0")) WeakTerrain->SetLod(0);
            else if (Command == TEXT("Lod1")) WeakTerrain->SetLod(1);
            else if (Command == TEXT("Lod2")) WeakTerrain->SetLod(2);
            else if (Command == TEXT("Vertical1")) WeakTerrain->SetVerticalExaggeration(1.0F);
            else if (Command == TEXT("Vertical2")) WeakTerrain->SetVerticalExaggeration(2.0F);
            else if (Command == TEXT("Vertical4")) WeakTerrain->SetVerticalExaggeration(4.0F);
            else if (Command == TEXT("Midday") || Command == TEXT("LowAngle") || Command == TEXT("Overcast")) WeakTerrain->SetLightingPreset(Command);
            else WeakTerrain->SetViewMode(ESkiTerrainViewMode::Presentation);
        });
        const FString WarningText = Warnings.IsEmpty() ? FString() : TEXT(" Warnings: ") + FString::Join(Warnings, TEXT(" "));
        P1Widget->SetTransientStatus(TEXT("Installed terrain is render/query ready. Scratch edits are separate from the package.") + WarningText);
    }
}

void ASkiBootstrapGameMode::RetryPreparation()
{
    if (!LastRequest.IsSet()) return;
    SkiPreparation::Request Request = LastRequest.GetValue();
    ++Request.OperationGeneration;
    BeginP1Preparation(Request);
}

void ASkiBootstrapGameMode::ChangeSelection()
{
    if (PreparationCancellation) PreparationCancellation->Cancel();
    ++ActiveOperationGeneration;
    LastRequest.Reset();
    if (P1Widget) P1Widget->ResetSelector();
}
