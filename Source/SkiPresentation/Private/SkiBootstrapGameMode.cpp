#include "SkiBootstrapGameMode.h"
#include "SkiBootstrapWidget.h"
#include "SkiP1Widget.h"
#include "SkiTerrainViewController.h"
#include "SkiApplication/Bootstrap.h"
#include "SkiPreparation/FixtureTerrainProvider.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/NativeTerrainProvider.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "HighResScreenshot.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Base64.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

#include <atomic>

namespace
{
class FPackagedScriptedTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    int32 Attempts = 0;
    bool bForwardedTimeouts = true;

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>&) override
    {
        ++Attempts;
        bForwardedTimeouts &= FMath::IsNearlyEqual(Request.ActivityTimeoutSeconds, 90.0F)
            && FMath::IsNearlyEqual(Request.TotalTimeoutSeconds, 180.0F);
        SkiPreparation::HttpAcquisitionResult Result;
        Result.RetryAfter = TEXT("0");
        if (Attempts == 1) Result.FailureReason = SkiPreparation::TransportFailureReason::TimedOut;
        else if (Attempts == 2)
        {
            Result.FailureReason = SkiPreparation::TransportFailureReason::HttpStatus;
            Result.HttpStatus = 503;
        }
        else
        {
            Result.HttpStatus = 200;
            Result.Bytes = {1};
            Result.BytesReceived = 1;
        }
        return Result;
    }
};

class FPackagedConcurrentTransport final : public SkiPreparation::IAcquisitionTransport
{
public:
    std::atomic<int32> Active{0};
    std::atomic<int32> Maximum{0};
    bool bDetachBackend = true;

    void WaitForBackends()
    {
        TArray<TFuture<void>> Pending;
        {
            FScopeLock Lock(&FutureMutex);
            Pending = std::move(BackendFutures);
        }
        for (TFuture<void>& Future : Pending) Future.Get();
    }

    SkiPreparation::HttpAcquisitionResult Get(const SkiPreparation::HttpAcquisitionRequest& Request,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation) override
    {
        const int32 Current = Active.fetch_add(1) + 1;
        int32 Observed = Maximum.load();
        while (Current > Observed && !Maximum.compare_exchange_weak(Observed, Current)) {}
        if(bDetachBackend)
        {
            TSharedPtr<SkiPreparation::AcquisitionResourceLease,ESPMode::ThreadSafe> BackendLifetime=Request.BackendLifetime;
            TFuture<void> Backend=Async(EAsyncExecution::Thread,
                [this,BackendLifetime=std::move(BackendLifetime)]()
                {FPlatformProcess::SleepNoStats(0.075F);Active.fetch_sub(1);});
            FScopeLock Lock(&FutureMutex);BackendFutures.Add(std::move(Backend));
            SkiPreparation::HttpAcquisitionResult Result;Result.HttpStatus=200;Result.Bytes={1};Result.BytesReceived=1;
            return Result;
        }
        const double Began = FPlatformTime::Seconds();
        while (!Cancellation->IsCancelled() && FPlatformTime::Seconds() - Began < 0.075)
            FPlatformProcess::SleepNoStats(0.005F);
        Active.fetch_sub(1);
        SkiPreparation::HttpAcquisitionResult Result;
        if (Cancellation->IsCancelled())
        {
            Result.FailureReason = SkiPreparation::TransportFailureReason::Cancelled;
            Result.RequestStatus = TEXT("Cancelled");
        }
        else
        {
            Result.HttpStatus = 200;
            Result.Bytes = {1};
            Result.BytesReceived = 1;
        }
        return Result;
    }

private:
    FCriticalSection FutureMutex;
    TArray<TFuture<void>> BackendFutures;
};
}

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
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1UiLayoutSmoke")))
    {
        if (!BeginP1UiLayoutSmoke()) FPlatformMisc::RequestExitWithStatus(false, 1);
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

bool ASkiBootstrapGameMode::BeginP1UiLayoutSmoke()
{
    FString DataRoot;
    FGuid ParsedToken;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), UiLayoutReceiptPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), UiLayoutToken)
        || !FGuid::Parse(UiLayoutToken, ParsedToken)) return false;
    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    UiLayoutReceiptPath = FPaths::ConvertRelativePathToFull(UiLayoutReceiptPath);
    FString Prefix = DataRoot; if (!Prefix.EndsWith(TEXT("/")) && !Prefix.EndsWith(TEXT("\\"))) Prefix += TEXT("/");
    Prefix.ReplaceInline(TEXT("\\"), TEXT("/")); FString Receipt = UiLayoutReceiptPath; Receipt.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!Receipt.StartsWith(Prefix) || FPaths::GetCleanFilename(UiLayoutReceiptPath) != UiLayoutToken + TEXT(".receipt.json")) return false;
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    P1Widget = Controller ? CreateWidget<USkiP1Widget>(Controller, USkiP1Widget::StaticClass()) : nullptr;
    if (!P1Widget || !P1Widget->IsP1Ready()) return false;
    SkiPreparation::ProviderFailure Failure;
    Failure.Code = TEXT("HTTP_ACQUISITION_FAILED"); Failure.Stage = SkiPreparation::FailureStage::Acquisition;
    Failure.Product = SkiPreparation::ProviderProduct::CoreElevation; Failure.Retry = SkiPreparation::RetryClassification::Retryable;
    Failure.TransportFailure = TEXT("TimedOut"); Failure.RequestStatus = TEXT("Failed"); Failure.RequestedWidth = 1000;
    Failure.RequestedHeight = 1000; Failure.TileIndex = 1; Failure.TileCount = 4; Failure.Attempt = 3; Failure.MaximumAttempts = 3;
    Failure.ElapsedSeconds = 90.0; Failure.DiagnosticReceipt = TEXT("TerrainDiagnostics/maximal-safe-receipt-name.json");
    P1Widget->ShowPreparationFailure(Failure, TEXT("failure"), []{}, []{});
    P1Widget->AddToViewport();
    SkiPreparation::Request FixtureRequest;
    FixtureRequest.Name = TEXT("UI input isolation fixture");
    FixtureRequest.Bounds = {-121.56, 46.95, -121.53, 46.97};
    FixtureRequest.SessionGeneration = 91; FixtureRequest.OperationGeneration = 37;
    FixtureRequest.Lease = MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(91, 37);
    const TSharedRef<SkiPreparation::Cancellation> FixtureCancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::FixtureTerrainProvider FixtureProvider(DataRoot);
    SkiPreparation::Result FixtureResult = FixtureProvider.Prepare(FixtureRequest, FixtureCancellation, {});
    TerrainSession = MakeShared<SkiApplication::TerrainSession>();
    std::vector<std::uint8_t> Cover(FixtureResult.Cover.GetData(),
        FixtureResult.Cover.GetData() + FixtureResult.Cover.Num());
    if (!FixtureResult.Ok || !TerrainSession->Install(std::move(FixtureResult.Heightfield),
            std::move(FixtureResult.Manifest), std::move(Cover))) return false;
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    if (!TerrainActor) return false;
    TerrainActor->SetTerrainSession(TerrainSession);
    if (!TerrainActor->Present(TerrainSession->Snapshot())) return false;
    ASkiTerrainViewController* TerrainController = Cast<ASkiTerrainViewController>(Controller);
    if (!TerrainController) return false;
    TerrainController->AttachTerrain(TerrainActor);
    TerrainController->SetUiGeometryHandlers(
        [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
        { return WeakWidget.IsValid() ? WeakWidget->GetRightPanelInsetPixels() : 0.0; },
        [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
        { return WeakWidget.IsValid() && WeakWidget->IsPointerOverStatusPanel(); },
        [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
        { return WeakWidget.IsValid() && WeakWidget->DoesUiOwnKeyboardInput(); });
    TerrainController->bShowMouseCursor = true;
    FInputModeGameAndUI InputMode;
    InputMode.SetHideCursorDuringCapture(false);
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    TerrainController->SetInputMode(InputMode);
    // A next-tick check can run before the first Slate paint in Shipping, leaving
    // cached widget geometry at zero. Give the packaged viewport one real frame
    // budget, then force a layout prepass before measuring the recovery UI.
    FTimerHandle LayoutTimer;
    GetWorldTimerManager().SetTimer(LayoutTimer, this,
        &ASkiBootstrapGameMode::FinishP1UiLayoutSmoke, 0.20F, false);
    return true;
}

void ASkiBootstrapGameMode::FinishP1UiLayoutSmoke()
{
    if (!P1Widget) { FPlatformMisc::RequestExitWithStatus(false, 1); return; }
    P1Widget->ForceLayoutPrepass();
    ASkiTerrainViewController* TerrainController = Cast<ASkiTerrainViewController>(GetWorld()->GetFirstPlayerController());
    if (TerrainController)
    {
        bUiInputIsolationValid = TerrainController->RunInputIsolationRegression(
            P1Widget->GetStatusPanelCenterAbsolute(), P1Widget->GetUnobstructedCenterAbsolute(),
            [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
            { if (WeakWidget.IsValid()) WeakWidget->FocusRecoveryAction(); }, UiInputIsolationError);
    }
    int32 Width = 0, Height = 0; GetWorld()->GetFirstPlayerController()->GetViewportSize(Width, Height);
    FString Error;
    const bool Valid = P1Widget->ValidateRecoveryLayout({Width, Height}, Error)
        && bUiInputIsolationValid;
    if (!bUiInputIsolationValid) Error += TEXT(" Input isolation: ") + UiInputIsolationError;
    const FString Receipt = FString::Printf(
        TEXT("{\"token\":\"%s\",\"scenario\":\"ui-layout\",\"resolution\":[%d,%d],\"rightInset\":%.1f,\"recoveryActionsReachable\":%s,\"inputIsolation\":%s,\"error\":\"%s\"}"),
        *UiLayoutToken, Width, Height, P1Widget->GetRightPanelInsetPixels(), Valid ? TEXT("true") : TEXT("false"),
        bUiInputIsolationValid ? TEXT("true") : TEXT("false"), *Error.ReplaceCharWithEscapedChar());
    const bool Written = FFileHelper::SaveStringToFile(Receipt, *UiLayoutReceiptPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FPlatformMisc::RequestExitWithStatus(false, Valid && Written ? 0 : 1);
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

    if (Scenario == TEXT("acquisition-regression"))
    {
        const SkiDomain::GeographicBounds MountWashington{-71.365, 44.225, -71.241, 44.315};
        const SkiPreparation::AcquisitionPlan Plan = SkiPreparation::BuildElevationAcquisitionPlan(
            MountWashington, SkiPreparation::SourceProfile::High);
        FPackagedScriptedTransport Transport;
        SkiPreparation::RetryPolicy Policy;
        Policy.OperationDeadlineSeconds = 5.0;
        SkiPreparation::HttpAcquisitionRequest Request;
        Request.ActivityTimeoutSeconds = Policy.ActivityTimeoutSeconds;
        Request.TotalTimeoutSeconds = Policy.TotalTimeoutSeconds;
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::HttpAcquisitionResult Downloaded;
        const bool Retried = SkiPreparation::ExecuteAcquisitionWithRetry(Transport, Request, Policy,
            Cancellation, FPlatformTime::Seconds(), [](){return true;}, {}, Downloaded);

        SkiPreparation::AcquisitionPlan StitchPlan;
        StitchPlan.Width = 4; StitchPlan.Height = 4; StitchPlan.WidthM = 40.0; StitchPlan.HeightM = 40.0;
        TArray<SkiPreparation::DecodedElevationRaster> Tiles;
        for (int32 Row = 0; Row < 2; ++Row) for (int32 Column = 0; Column < 2; ++Column)
        {
            StitchPlan.Tiles.Add({Column,Row,2,2,static_cast<uint32>(Column*2),static_cast<uint32>(Row*2),2,2});
            SkiPreparation::DecodedElevationRaster Tile;
            Tile.Heightfield.Width=2;Tile.Heightfield.Height=2;Tile.Heightfield.EastSpacingM=10.0;
            Tile.Heightfield.NorthSpacingM=10.0;Tile.Heightfield.NoDataValue=-9999.0;Tile.Heightfield.CurrentRevision=1;
            Tile.Heightfield.Samples={1,2,3,4};Tile.NoDataValue=-9999.0;
            Tile.ActualOuterBounds={static_cast<double>(Column*2),static_cast<double>(2-Row*2),
                static_cast<double>(Column*2+2),static_cast<double>(4-Row*2)};
            Tiles.Add(std::move(Tile));
        }
        SkiPreparation::DecodedElevationRaster Stitched;
        FString StitchError;
        const bool StitchedOk = SkiPreparation::StitchElevationTiles(StitchPlan,Tiles,Stitched,StitchError)
            && FMath::IsNearlyEqual(Stitched.Heightfield.WestM,-15.0)
            && FMath::IsNearlyEqual(Stitched.Heightfield.NorthM,15.0);

        SkiPreparation::PackageStore Store(DataRoot);
        SkiDomain::TerrainManifest Manifest;
        Manifest.Name="shipping fence";Manifest.Source="fixture";Manifest.RequestedAtUtc="2026-09-21T00:00:00Z";
        Manifest.RequestedBounds=MountWashington;Manifest.ActualBounds=MountWashington;
        Manifest.LocalOrigin={44.27,-71.303,0.0};
        const TSharedPtr<SkiPreparation::PreparationOperationLease,ESPMode::ThreadSafe> Lease=
            MakeShared<SkiPreparation::PreparationOperationLease,ESPMode::ThreadSafe>(4,7);
        Lease->Invalidate();
        FString Directory,ActivationError;SkiDomain::TerrainManifest Installed;
        const bool ActivationBlocked=!Store.WriteAndActivate(Manifest,Stitched.Heightfield,Directory,
            Installed,ActivationError,{},Lease,4,7);
        auto MeasureNetworkCap = [](const SkiPreparation::ProviderProduct Product,
            const int32 Count, int32& OutMaximum)
        {
            FPackagedConcurrentTransport Concurrent;
            TArray<TFuture<bool>> Futures;
            SkiPreparation::RetryPolicy ConcurrentPolicy;
            ConcurrentPolicy.MaximumAttempts=1;ConcurrentPolicy.OperationDeadlineSeconds=3.0;
            for(int32 Index=0;Index<Count;++Index)Futures.Add(Async(EAsyncExecution::ThreadPool,[&,Product]()
            {
                SkiPreparation::HttpAcquisitionRequest ConcurrentRequest;ConcurrentRequest.Product=Product;
                SkiPreparation::HttpAcquisitionResult ConcurrentResult;
                const TSharedRef<SkiPreparation::Cancellation> ConcurrentCancellation=MakeShared<SkiPreparation::Cancellation>();
                return SkiPreparation::ExecuteAcquisitionWithRetry(Concurrent,ConcurrentRequest,
                    ConcurrentPolicy,ConcurrentCancellation,FPlatformTime::Seconds(),[]{return true;},{},ConcurrentResult);
            }));
            bool bSucceeded=true;for(TFuture<bool>& Future:Futures)bSucceeded&=Future.Get();Concurrent.WaitForBackends();
            OutMaximum=Concurrent.Maximum.load();return bSucceeded;
        };
        int32 ElevationMaximum=0,NetworkMaximum=0;
        const bool NetworkCaps=MeasureNetworkCap(SkiPreparation::ProviderProduct::CoreElevation,6,ElevationMaximum)
            && MeasureNetworkCap(SkiPreparation::ProviderProduct::WorldCover,8,NetworkMaximum)
            && ElevationMaximum>0&&ElevationMaximum<=2&&NetworkMaximum>0&&NetworkMaximum<=4;
        std::atomic<int32> ActiveDecodes{0},MaximumDecodes{0};TArray<TFuture<bool>> DecodeFutures;
        for(int32 Index=0;Index<6;++Index)DecodeFutures.Add(Async(EAsyncExecution::ThreadPool,[&]()
        {
            const TSharedRef<SkiPreparation::Cancellation> DecodeCancellation=MakeShared<SkiPreparation::Cancellation>();
            return SkiPreparation::ExecuteBoundedDecodeJob(DecodeCancellation,[]{return true;},[&]()
            {
                const int32 Current=ActiveDecodes.fetch_add(1)+1;int32 Observed=MaximumDecodes.load();
                while(Current>Observed&&!MaximumDecodes.compare_exchange_weak(Observed,Current)){}
                FPlatformProcess::SleepNoStats(0.075F);ActiveDecodes.fetch_sub(1);return true;
            });
        }));
        bool DecodeCaps=true;for(TFuture<bool>& Future:DecodeFutures)DecodeCaps&=Future.Get();
        DecodeCaps=DecodeCaps&&MaximumDecodes.load()>0&&MaximumDecodes.load()<=2;
        FPackagedConcurrentTransport InFlightTransport;
        InFlightTransport.bDetachBackend=false;
        const TSharedRef<SkiPreparation::Cancellation> InFlightCancellation=MakeShared<SkiPreparation::Cancellation>();
        TFuture<bool> InFlight=Async(EAsyncExecution::ThreadPool,[&]()
        {
            SkiPreparation::HttpAcquisitionRequest InFlightRequest;InFlightRequest.Product=SkiPreparation::ProviderProduct::CoreElevation;
            SkiPreparation::RetryPolicy InFlightPolicy;InFlightPolicy.MaximumAttempts=1;InFlightPolicy.OperationDeadlineSeconds=3.0;
            SkiPreparation::HttpAcquisitionResult InFlightResult;
            return SkiPreparation::ExecuteAcquisitionWithRetry(InFlightTransport,InFlightRequest,
                InFlightPolicy,InFlightCancellation,FPlatformTime::Seconds(),[]{return true;},{},InFlightResult);
        });
        const double StartDeadline=FPlatformTime::Seconds()+1.0;
        while(InFlightTransport.Active.load()==0&&FPlatformTime::Seconds()<StartDeadline)FPlatformProcess::SleepNoStats(0.001F);
        const bool InFlightStarted=InFlightTransport.Active.load()>0;const double CancelBegan=FPlatformTime::Seconds();
        InFlightCancellation->Cancel();const bool InFlightFailed=!InFlight.Get();
        const double CancellationMilliseconds=(FPlatformTime::Seconds()-CancelBegan)*1000.0;
        const bool CancellationBounded=InFlightStarted&&InFlightFailed&&CancellationMilliseconds<=250.0;
        const bool Passed = FMath::Max(Plan.Width, Plan.Height) == 2000U && Plan.Tiles.Num() == 4
            && Retried && Downloaded.Attempt == 3 && Transport.Attempts == 3
            && Transport.bForwardedTimeouts && StitchedOk && ActivationBlocked
            && NetworkCaps && DecodeCaps && CancellationBounded;
        const FString Receipt = FString::Printf(
            TEXT("{\"token\":\"%s\",\"scenario\":\"acquisition-regression\",\"dimensions\":[%u,%u],\"tileCount\":%d,\"activityTimeoutSeconds\":90,\"totalTimeoutSeconds\":180,\"attempts\":%d,\"stitchedCentered\":%s,\"activationBlocked\":%s,\"networkCaps\":%s,\"decodeCaps\":%s,\"cancellationMilliseconds\":%.3f,\"passed\":%s}"),
            *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), Plan.Width, Plan.Height,
            Plan.Tiles.Num(), Transport.Attempts, StitchedOk?TEXT("true"):TEXT("false"),
            ActivationBlocked?TEXT("true"):TEXT("false"),NetworkCaps?TEXT("true"):TEXT("false"),
            DecodeCaps?TEXT("true"):TEXT("false"),CancellationMilliseconds,Passed?TEXT("true"):TEXT("false"));
        return Passed && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
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
    if (PreparationLease) PreparationLease->Invalidate();
    PreparationCancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::Request PreparedRequest = Request;
    PreparationLease = MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(
        PreparedRequest.SessionGeneration, PreparedRequest.OperationGeneration);
    PreparedRequest.Lease = PreparationLease;
    if (P1Widget) P1Widget->BeginPreparationUI([this] { ChangeSelection(); });
    LastRequest = PreparedRequest;
    ActiveSessionGeneration = PreparedRequest.SessionGeneration;
    ActiveOperationGeneration = PreparedRequest.OperationGeneration;
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = PreparationCancellation.ToSharedRef();
    const FString DataRoot = FPaths::ProjectSavedDir();
    const bool UseFixture = FParse::Param(FCommandLine::Get(), TEXT("SkiUseFixtureProvider"));
    const TWeakObjectPtr<ASkiBootstrapGameMode> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, PreparedRequest, Cancellation, DataRoot, UseFixture]
    {
        TUniquePtr<SkiPreparation::Provider> Provider;
        if (UseFixture) Provider = MakeUnique<SkiPreparation::FixtureTerrainProvider>(DataRoot);
        else Provider = MakeUnique<SkiPreparation::NativeTerrainProvider>(DataRoot);
        SkiPreparation::Result Result = Provider->Prepare(PreparedRequest, Cancellation,
            [WeakThis, PreparedRequest, Cancellation](const SkiPreparation::Progress& Progress)
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis, PreparedRequest, Cancellation, Progress]
                {
                    if (WeakThis.IsValid() && WeakThis->P1Widget && !Cancellation->IsCancelled()
                        && WeakThis->ActiveSessionGeneration == PreparedRequest.SessionGeneration
                        && WeakThis->ActiveOperationGeneration == PreparedRequest.OperationGeneration)
                        WeakThis->P1Widget->SetPreparationProgress(Progress);
                });
            });
        AsyncTask(ENamedThreads::GameThread, [WeakThis, PreparedRequest, Cancellation, Result = std::move(Result)]() mutable
        {
            if (WeakThis.IsValid()) WeakThis->FinishP1Preparation(std::move(Result),
                PreparedRequest.SessionGeneration, PreparedRequest.OperationGeneration, Cancellation);
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
        Controller->SetUiGeometryHandlers(
            [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
            { return WeakWidget.IsValid() ? WeakWidget->GetRightPanelInsetPixels() : 0.0; },
            [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
            { return WeakWidget.IsValid() && WeakWidget->IsPointerOverStatusPanel(); },
            [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
            { return WeakWidget.IsValid() && WeakWidget->DoesUiOwnKeyboardInput(); });
        Controller->bShowMouseCursor = true;
        FInputModeGameAndUI InputMode;
        InputMode.SetHideCursorDuringCapture(false);
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        Controller->SetInputMode(InputMode);
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
    if (PreparationLease) PreparationLease->Invalidate();
    ++ActiveOperationGeneration;
    LastRequest.Reset();
    if (P1Widget) P1Widget->ResetSelector();
}
