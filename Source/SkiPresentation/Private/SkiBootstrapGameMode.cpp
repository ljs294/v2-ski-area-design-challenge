#include "SkiBootstrapGameMode.h"
#include "SkiBootstrapWidget.h"
#include "SkiP1Widget.h"
#include "SkiTerrainViewController.h"
#include "SkiTerrainCoreRegression.h"
#include "SkiApplication/Bootstrap.h"
#include "SkiApplication/TerrainCoreEditedRepository.h"
#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiApplication/TerrainCoreSession.h"
#include "SkiPreparation/FixtureTerrainProvider.h"
#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/GeoTiffDecoder.h"
#include "SkiPreparation/NativeTerrainProvider.h"
#include "SkiPreparation/TerrainAcquisition.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"
#include "Async/Async.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "HighResScreenshot.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
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
bool IsContentId(const FString& Candidate)
{
    if (Candidate.Len() != 64) return false;
    for (const TCHAR Character : Candidate)
        if (!FChar::IsHexDigit(Character)) return false;
    return true;
}

std::shared_ptr<const std::vector<std::uint8_t>> PackCoverValidity(
    const TArray<uint8>& Validity, const uint32 Width, const uint32 Height)
{
    const uint64 Count = static_cast<uint64>(Width) * Height;
    if (Count == 0 || Count > static_cast<uint64>(MAX_int32)
        || Validity.Num() != static_cast<int32>(Count)) return {};
    auto Packed = std::make_shared<std::vector<std::uint8_t>>((Count + 7ULL) / 8ULL, 0);
    for (uint64 Index = 0; Index < Count; ++Index)
    {
        if (Validity[static_cast<int32>(Index)] != 0)
            (*Packed)[Index >> 3U] |= static_cast<std::uint8_t>(1U << (Index & 7U));
    }
    return Packed;
}

std::shared_ptr<const std::vector<std::uint8_t>> CopyCoverChannel(const TArray<uint8>& Channel)
{
    return std::make_shared<const std::vector<std::uint8_t>>(
        Channel.GetData(), Channel.GetData() + Channel.Num());
}

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

std::shared_ptr<SkiApplication::TerrainCoreRepository> OpenRuntimeTerrainCoreRepository(
    const std::shared_ptr<SkiPreparation::TerrainCorePackageStore>& Store,
    const SkiPreparation::TerrainCorePackageIndex& Index, FString& OutError)
{
    std::string RepositoryError;
    auto Repository = SkiApplication::TerrainCoreRepository::Create(Index.Manifest,
        [Store, Index](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& OutPayload, std::string& Error)
        {
            SkiPreparation::TerrainCoreDecodedTile Decoded;
            FString DecodeError;
            if (!Store->ReadTile(Index, Descriptor.LodIndex, Descriptor.TileX,
                    Descriptor.TileY, Decoded, DecodeError))
            {
                Error = TCHAR_TO_UTF8(*DecodeError);
                return false;
            }
            OutPayload.Key = {Descriptor.LodIndex, Descriptor.TileX, Descriptor.TileY};
            OutPayload.Descriptor = Decoded.Descriptor;
            OutPayload.Heights.assign(Decoded.Heights.GetData(),
                Decoded.Heights.GetData() + Decoded.Heights.Num());
            OutPayload.Validity.assign(Decoded.Validity.GetData(),
                Decoded.Validity.GetData() + Decoded.Validity.Num());
            return true;
        }, RepositoryError);
    if (!Repository) OutError = UTF8_TO_TCHAR(RepositoryError.c_str());
    return Repository;
}

SkiDomain::TerrainCoreManifest MakeTerrainCoreManifest(
    const SkiDomain::TerrainManifest& Legacy,
    const SkiDomain::Heightfield& Heightfield)
{
    SkiDomain::TerrainCoreManifest Core;
    Core.GeneratorVersion = "mountain-planner-terraincore-v2";
    Core.ProcessingVersions = {"schema1-ground-normalization-v1", "terraincore-derivation-v1"};
    Core.LocalOrigin = Legacy.LocalOrigin;
    Core.Width = Heightfield.Width;
    Core.Height = Heightfield.Height;
    Core.DeliveredEastSpacingM = Heightfield.EastSpacingM;
    Core.DeliveredNorthSpacingM = Heightfield.NorthSpacingM;
    Core.Registration = SkiDomain::PixelRegistration::SampleCenter;
    Core.SampleCenterBounds = {Heightfield.WestM,
        Heightfield.SampleNorthM(Heightfield.Height - 1U),
        Heightfield.EastM(Heightfield.Width - 1U), Heightfield.NorthM};
    SkiDomain::ComputeTerrainCoreBounds(Core.Width, Core.Height,
        Core.DeliveredEastSpacingM, Core.DeliveredNorthSpacingM,
        Core.SampleCenterBounds, Core.OuterBounds);
    Core.Source.SourceId = Legacy.Source.empty() ? "schema1-elevation" : Legacy.Source;
    Core.Source.Product = Legacy.Source.empty() ? "prepared elevation" : Legacy.Source;
    Core.Source.AcquisitionEpoch = Legacy.RequestedAtUtc.empty()
        ? "unknown" : Legacy.RequestedAtUtc;
    Core.Source.HorizontalCrs = Legacy.HorizontalFrame.empty()
        ? "WGS84/local-ENU" : Legacy.HorizontalFrame;
    Core.Source.HorizontalDatum = "WGS84";
    Core.Source.VerticalDatum = Legacy.VerticalDatum.empty()
        ? "unknown" : Legacy.VerticalDatum;
    Core.Source.License = "see schema-1 source receipt";
    Core.Source.Attribution = Legacy.Source.empty() ? "unknown provider" : Legacy.Source;
    Core.Source.NativeEastSpacingM = Heightfield.EastSpacingM;
    Core.Source.NativeNorthSpacingM = Heightfield.NorthSpacingM;
    for (const SkiDomain::TerrainAsset& Asset : Legacy.Assets)
    {
        if (Asset.Type == "height-f32le" && Asset.Required)
        {
            if (!Asset.Source.empty()) Core.Source.Product = Asset.Source;
            if (!Asset.License.empty()) Core.Source.License = Asset.License;
            break;
        }
    }
    return Core;
}
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
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1PerformanceSmoke")))
    {
        if (!BeginP1PerformanceSmoke()) FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }

    P1Widget = Controller ? CreateWidget<USkiP1Widget>(Controller, USkiP1Widget::StaticClass()) : nullptr;
    if (!ExpectedMap || !SkiApplication::CheckDomainBoundary() || !P1Widget || !P1Widget->IsP1Ready()) return;
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1SelectorSmoke")))
    {
        P1Widget->SetSelectionHandler([this](const SkiPreparation::Request& Request)
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
            const bool ClosedBeforeAcceptance = P1Widget && P1Widget->IsSelectorClosed();
            bool BridgeUnbound = false, CefClosed = false, WindowReleased = false;
            double CloseMs = 0.0;
            const bool ProofAvailable = P1Widget
                && P1Widget->GetSelectorTeardownProof(BridgeUnbound, CefClosed, WindowReleased, CloseMs);
            const int32 BlockedNavigation = P1Widget ? P1Widget->GetBlockedSelectorNavigationCount() : 0;
            const int32 BlockedPopup = P1Widget ? P1Widget->GetBlockedSelectorPopupCount() : 0;
            const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"selector\":true,\"profile\":\"%s\",\"closedBeforeAcceptance\":%s,\"blockedNavigation\":%d,\"blockedPopup\":%d,\"popupDelegateProbeDenied\":%s,\"bridgeUnbound\":%s,\"cefBrowserClosed\":%s,\"windowReleased\":%s,\"closeMs\":%.1f}"),
                *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower),
                Request.Profile == SkiPreparation::SourceProfile::Medium ? TEXT("medium") : TEXT("legacy-standard"),
                ClosedBeforeAcceptance ? TEXT("true") : TEXT("false"), BlockedNavigation, BlockedPopup,
                P1Widget && P1Widget->WasSelectorPopupDelegateProbeDenied() ? TEXT("true") : TEXT("false"),
                ProofAvailable && BridgeUnbound ? TEXT("true") : TEXT("false"),
                ProofAvailable && CefClosed ? TEXT("true") : TEXT("false"),
                ProofAvailable && WindowReleased ? TEXT("true") : TEXT("false"), CloseMs);
            const bool Written = ArgumentsValid && PathValid && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            FPlatformMisc::RequestExitWithStatus(false, Written ? 0 : 1);
        });
    }
    else P1Widget->SetSelectionHandler([this](const SkiPreparation::Request& Request) { BeginP1Preparation(Request); });
    P1Widget->SetOpenInstalledHandler([this] { OpenLatestInstalledTerrain(); });
    P1Widget->AddToViewport();
    Controller->bShowMouseCursor = true;
    Controller->SetInputMode(FInputModeUIOnly());
    FString SelectorDiagnostic;
    if (FParse::Value(FCommandLine::Get(), TEXT("SkiP1SelectorDiagnostic="), SelectorDiagnostic))
    {
        if (SelectorDiagnostic != TEXT("blank") && SelectorDiagnostic != TEXT("no-tiles")
            && SelectorDiagnostic != TEXT("full"))
        {
            FPlatformMisc::RequestExitWithStatus(false, 1);
            return;
        }
        P1Widget->SetSelectionHandler({});
        P1Widget->OpenSelector();
        FTimerHandle DiagnosticTimer;
        GetWorldTimerManager().SetTimer(DiagnosticTimer,
            FTimerDelegate::CreateLambda([] { FPlatformMisc::RequestExitWithStatus(false, 0); }),
            5.0F, false);
        return;
    }
    FString InstalledChoice;
    if (FParse::Value(FCommandLine::Get(), TEXT("SkiP1OpenInstalled="), InstalledChoice))
    {
        if (InstalledChoice.Equals(TEXT("latest"), ESearchCase::IgnoreCase))
            OpenLatestInstalledTerrain();
        else if (IsContentId(InstalledChoice)) OpenInstalledTerrain(InstalledChoice);
        else
        {
            P1Widget->OpenSelector();
            P1Widget->SetSelectorStatus(TEXT("Invalid installed terrain ID. Choose bounds or reopen an installed terrain."));
        }
    }
    else P1Widget->OpenSelector();
}

bool ASkiBootstrapGameMode::BeginP1UiLayoutSmoke()
{
    FString DataRoot;
    FGuid ParsedToken;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), UiLayoutReceiptPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), UiLayoutToken)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1UiState="), UiLayoutState)
        || !FGuid::Parse(UiLayoutToken, ParsedToken)) return false;
    UiLayoutState = UiLayoutState.ToLower();
    if (UiLayoutState != TEXT("selecting") && UiLayoutState != TEXT("preparing")
        && UiLayoutState != TEXT("failed") && UiLayoutState != TEXT("ready")) return false;
    bUiLayoutRunInputIsolation = FParse::Param(FCommandLine::Get(), TEXT("SkiP1UiInputIsolation"));
    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    UiLayoutReceiptPath = FPaths::ConvertRelativePathToFull(UiLayoutReceiptPath);
    FString Prefix = DataRoot; if (!Prefix.EndsWith(TEXT("/")) && !Prefix.EndsWith(TEXT("\\"))) Prefix += TEXT("/");
    Prefix.ReplaceInline(TEXT("\\"), TEXT("/")); FString Receipt = UiLayoutReceiptPath; Receipt.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!Receipt.StartsWith(Prefix) || FPaths::GetCleanFilename(UiLayoutReceiptPath) != UiLayoutToken + TEXT(".receipt.json")) return false;
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    P1Widget = Controller ? CreateWidget<USkiP1Widget>(Controller, USkiP1Widget::StaticClass()) : nullptr;
    if (!P1Widget || !P1Widget->IsP1Ready()) return false;
    if (UiLayoutState == TEXT("selecting")) P1Widget->OpenSelector();
    if (UiLayoutState == TEXT("preparing"))
    {
        P1Widget->BeginPreparationUI([]{});
        P1Widget->SetPreparationProgress({SkiPreparation::State::Acquiring, 2, 8, 30.0,
            TEXT("Downloading analytical cover tile 2/8"), 2, 3, 2, 8});
    }
    else if (UiLayoutState == TEXT("failed"))
    {
        SkiPreparation::ProviderFailure Failure;
        Failure.Code = TEXT("HTTP_ACQUISITION_FAILED"); Failure.Stage = SkiPreparation::FailureStage::Acquisition;
        Failure.Product = SkiPreparation::ProviderProduct::CoreElevation; Failure.Retry = SkiPreparation::RetryClassification::Retryable;
        Failure.TransportFailure = TEXT("TimedOut"); Failure.RequestStatus = TEXT("Failed"); Failure.RequestedWidth = 1000;
        Failure.RequestedHeight = 1000; Failure.TileIndex = 1; Failure.TileCount = 4; Failure.Attempt = 3; Failure.MaximumAttempts = 3;
        Failure.ElapsedSeconds = 90.0; Failure.DiagnosticReceipt = TEXT("TerrainDiagnostics/maximal-safe-receipt-name.json");
        P1Widget->ShowPreparationFailure(Failure, TEXT("failure"), []{}, []{});
    }
    else if (UiLayoutState == TEXT("ready"))
    {
        P1Widget->SetTerrainDetails(TEXT("Medium terrain\n513 x 513 samples\nTerrainCore + CoverEcology verified"), true);
        P1Widget->SetNodeStatus(TEXT("Runtime node view\nSelection -> Medium\nGround + cover -> verified\nRender/query -> aligned"));
    }
    P1Widget->AddToViewport();
    if (bUiLayoutRunInputIsolation)
    {
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
    }
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
    bUiInputIsolationValid = !bUiLayoutRunInputIsolation;
    if (TerrainController && bUiLayoutRunInputIsolation)
    {
        bUiInputIsolationValid = TerrainController->RunInputIsolationRegression(
            P1Widget->GetStatusPanelCenterAbsolute(), P1Widget->GetUnobstructedCenterAbsolute(),
            [WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)]()
            { if (WeakWidget.IsValid()) WeakWidget->FocusRecoveryAction(); }, UiInputIsolationError);
    }
    int32 Width = 0, Height = 0; GetWorld()->GetFirstPlayerController()->GetViewportSize(Width, Height);
    FString Error;
    const EP1ShellState ExpectedState = UiLayoutState == TEXT("selecting")
        ? EP1ShellState::Selecting : UiLayoutState == TEXT("preparing")
        ? EP1ShellState::Preparing : UiLayoutState == TEXT("failed")
        ? EP1ShellState::Failed : EP1ShellState::Ready;
    const bool LayoutValid = P1Widget->ValidateShellLayout({Width, Height}, ExpectedState, Error);
    const bool RecoveryValid = UiLayoutState != TEXT("failed")
        || P1Widget->ValidateRecoveryLayout({Width, Height}, Error);
    const bool Valid = LayoutValid && RecoveryValid && bUiInputIsolationValid;
    if (!bUiInputIsolationValid) Error += TEXT(" Input isolation: ") + UiInputIsolationError;
    const FVector4 Panel = P1Widget->GetStatusPanelRectAbsolute();
    const FVector4 Selector = P1Widget->GetSelectorPanelRectAbsolute();
    const FVector4 Scroll = P1Widget->GetStatusScrollRectAbsolute();
    const FVector4 Retry = P1Widget->GetRetryRectAbsolute();
    const FVector4 Change = P1Widget->GetChangeSelectionRectAbsolute();
    const FString Receipt = FString::Printf(
        TEXT("{\"token\":\"%s\",\"scenario\":\"ui-layout\",\"uiState\":\"%s\",\"resolution\":[%d,%d],\"rightInset\":%.1f,\"layoutValid\":%s,\"recoveryActionsReachable\":%s,\"inputIsolation\":%s,\"panelRect\":[%.1f,%.1f,%.1f,%.1f],\"selectorRect\":[%.1f,%.1f,%.1f,%.1f],\"scrollRect\":[%.1f,%.1f,%.1f,%.1f],\"retryRect\":[%.1f,%.1f,%.1f,%.1f],\"changeRect\":[%.1f,%.1f,%.1f,%.1f],\"error\":\"%s\"}"),
        *UiLayoutToken, *UiLayoutState, Width, Height, P1Widget->GetRightPanelInsetPixels(),
        LayoutValid ? TEXT("true") : TEXT("false"), RecoveryValid ? TEXT("true") : TEXT("false"),
        bUiInputIsolationValid ? TEXT("true") : TEXT("false"),
        Panel.X,Panel.Y,Panel.Z,Panel.W,Selector.X,Selector.Y,Selector.Z,Selector.W,
        Scroll.X,Scroll.Y,Scroll.Z,Scroll.W,Retry.X,Retry.Y,Retry.Z,Retry.W,
        Change.X,Change.Y,Change.Z,Change.W,*Error.ReplaceCharWithEscapedChar());
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

    SkiPreparation::Request Request;
    Request.Name = TEXT("Crystal Mountain synthetic Medium visual fixture");
    Request.Bounds = {-121.489, 46.925, -121.462, 46.947};
    Request.Profile = SkiPreparation::SourceProfile::Medium;
    Request.SessionGeneration = 1; Request.OperationGeneration = 1;
    Request.Lease = MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(1, 1);
    SkiPreparation::FixtureTerrainProvider Provider(DataRoot);
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::Result Result = Provider.Prepare(Request, Cancellation, {});
    if (!Result.Ok || !Result.HasNativeV2Installation) return false;

    VisualTerrainCoreId = UTF8_TO_TCHAR(Result.TerrainCoreManifest.ContentId.c_str());
    VisualCoverEcologyId = UTF8_TO_TCHAR(Result.CoverEcologyManifest.ContentId.c_str());
    VisualInstallationId = UTF8_TO_TCHAR(Result.InstallationReceipt.ContentId.c_str());
    VisualRequestedBounds = Result.Manifest.RequestedBounds;
    VisualActualBounds = Result.Manifest.ActualBounds;
    VisualDatum = UTF8_TO_TCHAR(Result.Manifest.VerticalDatum.c_str());
    VisualTerrainWidth = Result.TerrainCoreManifest.Width;
    VisualTerrainHeight = Result.TerrainCoreManifest.Height;
    VisualEastSpacingM = Result.TerrainCoreManifest.DeliveredEastSpacingM;
    VisualNorthSpacingM = Result.TerrainCoreManifest.DeliveredNorthSpacingM;

    auto CoreStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(DataRoot);
    SkiPreparation::TerrainCorePackageIndex CoreIndex;
    FString Error;
    if (!CoreStore->Open(VisualTerrainCoreId, CoreIndex, Error)) return false;
    auto Repository = OpenRuntimeTerrainCoreRepository(CoreStore, CoreIndex, Error);
    if (!Repository) return false;
    TerrainCoreSession = MakeShared<SkiApplication::TerrainCoreSession>();
    if (!TerrainCoreSession->Install(Repository, 1)) return false;
    auto RuntimeCover = CopyCoverChannel(Result.Cover);
    auto RuntimeCoverValidity = PackCoverValidity(Result.CoverValidity,
        Result.CoverEcologyManifest.Transform.Width,
        Result.CoverEcologyManifest.Transform.Height);
    if (!RuntimeCoverValidity) return false;
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    if (!TerrainActor) return false;
    TerrainActor->SetTerrainCoreCover(RuntimeCover, RuntimeCoverValidity,
        Result.CoverEcologyManifest.Transform);
    if (!TerrainActor->PresentTerrainCore(TerrainCoreSession,
            static_cast<uint8>(VisualLod))) return false;
    TArray<uint8> ManifestBytes;
    const FString ManifestPath = FPaths::Combine(CoreIndex.PackageDirectory, TEXT("manifest.json"));
    if (!FFileHelper::LoadFileToArray(ManifestBytes, *ManifestPath)) return false;
    VisualPackageHash = SkiPreparation::Sha256(ManifestBytes);
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
    if (!TerrainCoreSession || !TerrainActor || !TerrainActor->IsUsingTerrainCore())
    { FPlatformMisc::RequestExitWithStatus(false, 1); return; }
    const SkiApplication::TerrainCoreSnapshot Snapshot = TerrainCoreSession->Snapshot();
    int32 Width = 0, Height = 0; GetWorld()->GetFirstPlayerController()->GetViewportSize(Width, Height);
    const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"representation\":\"terraincore-v2\",\"contentId\":\"%s\",\"terrainCoreId\":\"%s\",\"coverEcologyId\":\"%s\",\"installationId\":\"%s\",\"packageHash\":\"%s\",\"requestedBounds\":[%.9f,%.9f,%.9f,%.9f],\"bounds\":[%.9f,%.9f,%.9f,%.9f],\"datum\":\"%s\",\"dimensions\":[%u,%u],\"spacing\":[%.6f,%.6f],\"revisions\":[%llu,%llu,%llu],\"revisionAligned\":%s,\"camera\":\"%s\",\"fov\":50,\"lod\":%d,\"lighting\":\"%s\",\"diagnosticMode\":\"%s\",\"view\":\"%s\",\"verticalScale\":1,\"rhi\":\"%s\",\"resolution\":[%d,%d],\"viewport\":[%d,%d],\"internalResolutionPercent\":100,\"syntheticGuestMarkers\":%d,\"overlaySegments\":%d,\"screenshotSha256\":\"%s\"}"),
        *VisualToken, *VisualTerrainCoreId, *VisualTerrainCoreId,
        *VisualCoverEcologyId, *VisualInstallationId, *VisualPackageHash,
        VisualRequestedBounds.WestDeg, VisualRequestedBounds.SouthDeg,
        VisualRequestedBounds.EastDeg, VisualRequestedBounds.NorthDeg,
        VisualActualBounds.WestDeg, VisualActualBounds.SouthDeg,
        VisualActualBounds.EastDeg, VisualActualBounds.NorthDeg, *VisualDatum,
        VisualTerrainWidth, VisualTerrainHeight, VisualEastSpacingM,
        VisualNorthSpacingM, static_cast<uint64>(Snapshot.Revisions.Canonical),
        static_cast<uint64>(Snapshot.Revisions.Render), static_cast<uint64>(Snapshot.Revisions.Query),
        TerrainActor->IsTerrainCoreRevisionAligned() ? TEXT("true") : TEXT("false"),
        *Cast<ASkiTerrainViewController>(GetWorld()->GetFirstPlayerController())->DescribeCamera(), VisualLod,
        *VisualLighting, *VisualMode, *VisualView, *FApp::GetGraphicsRHI(), VisualCaptureWidth,
        VisualCaptureHeight, Width, Height, TerrainActor->GetSyntheticGuestMarkerCount(),
        TerrainActor->GetOverlaySegmentCount(), *SkiPreparation::Sha256(Bytes));
    const bool Written = FFileHelper::SaveStringToFile(Receipt, *VisualReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FPlatformMisc::RequestExitWithStatus(false, Written ? 0 : 1);
}

bool ASkiBootstrapGameMode::BeginP1PerformanceSmoke()
{
    FString DataRoot;
    FGuid ParsedToken;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), PerformanceReceiptPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), PerformanceToken)
        || !FGuid::Parse(PerformanceToken, ParsedToken)) return false;
    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    PerformanceReceiptPath = FPaths::ConvertRelativePathToFull(PerformanceReceiptPath);
    PerformanceFramesPath = FPaths::Combine(DataRoot, PerformanceToken + TEXT(".frames.json"));
    FString Prefix = DataRoot.Replace(TEXT("\\"), TEXT("/"));
    if (!Prefix.EndsWith(TEXT("/"))) Prefix += TEXT("/");
    if (!PerformanceReceiptPath.Replace(TEXT("\\"), TEXT("/")).StartsWith(Prefix)
        || FPaths::GetCleanFilename(PerformanceReceiptPath)
            != PerformanceToken + TEXT(".receipt.json")) return false;

    SkiPreparation::Request Request;
    Request.Name = TEXT("Crystal Mountain Medium performance fixture");
    Request.Bounds = {-121.489, 46.925, -121.462, 46.947};
    Request.Profile = SkiPreparation::SourceProfile::Medium;
    Request.SessionGeneration = 701; Request.OperationGeneration = 1;
    Request.Lease = MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(701, 1);
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::Result Prepared = SkiPreparation::FixtureTerrainProvider(DataRoot).Prepare(
        Request, Cancellation, {});
    if (!Prepared.Ok || !Prepared.HasNativeV2Installation) return false;

    const double ReopenBegan = FPlatformTime::Seconds();
    SkiPreparation::InstalledTerrainIndex Installation;
    SkiPreparation::TerrainCorePackageIndex Core;
    SkiPreparation::CoverEcologyPackageIndex Ecology;
    TArray<uint8> Cover;
    TArray<uint8> Validity;
    FString Error;
    SkiPreparation::InstalledTerrainStore InstallationStore(DataRoot);
    SkiPreparation::TerrainCorePackageStore CoreStore(DataRoot);
    SkiPreparation::CoverEcologyStore CoverStore(DataRoot);
    if (!InstallationStore.Open(UTF8_TO_TCHAR(Prepared.InstallationReceipt.ContentId.c_str()),
            Installation, Error)
        || !CoreStore.Open(UTF8_TO_TCHAR(Installation.Receipt.TerrainCoreId.c_str()), Core, Error)
        || !CoverStore.Open(UTF8_TO_TCHAR(Installation.Receipt.CoverEcologyId.c_str()), Ecology, Error)
        || !CoverStore.ReadChannels(Ecology, Cover, Validity, Error)) return false;
    auto RuntimeStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(DataRoot);
    auto Repository = OpenRuntimeTerrainCoreRepository(RuntimeStore, Core, Error);
    if (!Repository) return false;
    TerrainCoreSession = MakeShared<SkiApplication::TerrainCoreSession>();
    if (!TerrainCoreSession->Install(Repository, 1)) return false;
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    if (!TerrainActor) return false;
    TerrainActor->SetTerrainCoreCover(CopyCoverChannel(Cover), CopyCoverChannel(Validity),
        Ecology.Manifest.Transform);
    const double RenderBegan = FPlatformTime::Seconds();
    if (!TerrainActor->PresentTerrainCore(TerrainCoreSession, 2)) return false;
    ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(
        GetWorld()->GetFirstPlayerController());
    FBox TerrainBounds;
    if (!Controller || !TerrainActor->GetValidWorldBounds(TerrainBounds)) return false;
    Controller->AttachTerrain(TerrainActor);
    Controller->FrameAll();
    bPerformanceCameraFramed = Cast<ACameraActor>(Controller->GetViewTarget()) != nullptr;
    if (!bPerformanceCameraFramed) return false;
    PerformanceFirstRenderSeconds = FPlatformTime::Seconds() - RenderBegan;
    PerformanceReopenSeconds = FPlatformTime::Seconds() - ReopenBegan;
    PerformanceLowFrameMs.Reset(); PerformanceReferenceFrameMs.Reset();
    PerformanceLowRenderedTiles = 0; PerformanceReferenceRenderedTiles = 0;
    PerformancePhase = 0; PerformancePhaseFrame = 0;
    GetWorldTimerManager().SetTimerForNextTick(this,
        &ASkiBootstrapGameMode::SampleP1PerformanceFrame);
    return true;
}

void ASkiBootstrapGameMode::SampleP1PerformanceFrame()
{
    constexpr int32 WarmFrames = 60;
    constexpr int32 MeasuredFrames = 240;
    if (PerformancePhaseFrame >= WarmFrames)
    {
        const double FrameMs = FMath::Max(0.0, FApp::GetDeltaTime() * 1000.0);
        (PerformancePhase == 0 ? PerformanceLowFrameMs : PerformanceReferenceFrameMs).Add(FrameMs);
    }
    ++PerformancePhaseFrame;
    if (PerformancePhaseFrame >= WarmFrames + MeasuredFrames)
    {
        if (PerformancePhase == 0)
        {
            PerformanceLowRenderedTiles = TerrainActor
                ? TerrainActor->GetRenderedTerrainCoreTileCount() : 0;
            if (!TerrainActor || !TerrainActor->SetLod(0))
            { FPlatformMisc::RequestExitWithStatus(false, 1); return; }
            PerformancePhase = 1;
            PerformancePhaseFrame = 0;
        }
        else
        {
            PerformanceReferenceRenderedTiles = TerrainActor
                ? TerrainActor->GetRenderedTerrainCoreTileCount() : 0;
            FinishP1PerformanceSmoke();
            return;
        }
    }
    GetWorldTimerManager().SetTimerForNextTick(this,
        &ASkiBootstrapGameMode::SampleP1PerformanceFrame);
}

void ASkiBootstrapGameMode::FinishP1PerformanceSmoke()
{
    const auto Percentile = [](TArray<double> Values, const double Fraction)
    {
        Values.Sort();
        if (Values.IsEmpty()) return 0.0;
        return Values[FMath::Clamp(FMath::CeilToInt(Fraction * Values.Num()) - 1,
            0, Values.Num() - 1)];
    };
    const auto Maximum = [](const TArray<double>& Values)
    {
        double Result = 0.0; for (const double Value : Values) Result = FMath::Max(Result, Value);
        return Result;
    };
    const auto CountAbove = [](const TArray<double>& Values, const double Threshold)
    {
        int32 Count = 0; for (const double Value : Values) if (Value > Threshold) ++Count;
        return Count;
    };
    const double LowP95 = Percentile(PerformanceLowFrameMs, 0.95);
    const double LowP99 = Percentile(PerformanceLowFrameMs, 0.99);
    const double LowMax = Maximum(PerformanceLowFrameMs);
    const double RefP95 = Percentile(PerformanceReferenceFrameMs, 0.95);
    const double RefP99 = Percentile(PerformanceReferenceFrameMs, 0.99);
    const double RefMax = Maximum(PerformanceReferenceFrameMs);
    const bool Passed = PerformanceLowFrameMs.Num() == 240
        && PerformanceReferenceFrameMs.Num() == 240
        && LowP95 <= 33.3 && LowP99 <= 50.0 && LowMax <= 250.0
        && RefP95 <= 20.0 && RefP99 <= 33.3 && RefMax <= 250.0
        && PerformanceReopenSeconds <= 30.0 && TerrainActor
        && TerrainActor->IsTerrainCoreRevisionAligned()
        && bPerformanceCameraFramed && PerformanceLowRenderedTiles > 0
        && PerformanceReferenceRenderedTiles > 0;
    const auto ArrayJson = [](const TArray<double>& Values)
    {
        FString Json = TEXT("[");
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        { if (Index) Json += TEXT(","); Json += FString::Printf(TEXT("%.6f"), Values[Index]); }
        return Json + TEXT("]");
    };
    const FString Frames = FString::Printf(TEXT("{\"lowMs\":%s,\"referenceMs\":%s}"),
        *ArrayJson(PerformanceLowFrameMs), *ArrayJson(PerformanceReferenceFrameMs));
    const bool FramesWritten = FFileHelper::SaveStringToFile(Frames, *PerformanceFramesPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    TArray<uint8> FrameBytes;
    const bool FramesRead = FramesWritten && FFileHelper::LoadFileToArray(FrameBytes,
        *PerformanceFramesPath);
    const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
    const FString Cpu = FPlatformMisc::GetCPUBrand().ReplaceCharWithEscapedChar();
    const FString Gpu = FPlatformMisc::GetPrimaryGPUBrand().ReplaceCharWithEscapedChar();
    const FString Receipt = FString::Printf(
        TEXT("{\"token\":\"%s\",\"scenario\":\"performance-regression\",\"passed\":%s,\"cameraFramed\":%s,\"lowRenderedTiles\":%d,\"referenceRenderedTiles\":%d,\"low\":{\"frames\":%d,\"p95Ms\":%.6f,\"p99Ms\":%.6f,\"maxMs\":%.6f,\"over50\":%d,\"over100\":%d,\"over250\":%d},\"reference\":{\"frames\":%d,\"p95Ms\":%.6f,\"p99Ms\":%.6f,\"maxMs\":%.6f,\"over50\":%d,\"over100\":%d,\"over250\":%d},\"preparedReopenSeconds\":%.6f,\"firstRenderSeconds\":%.6f,\"coldWarm\":\"single-process cold reopen; warmed per lane\",\"rhi\":\"%s\",\"resolution\":[%d,%d],\"internalResolutionPercent\":100,\"cpu\":\"%s\",\"gpu\":\"%s\",\"availablePhysicalBytes\":%llu,\"cacheBudgetBytes\":%llu,\"frameSamples\":\"%s\",\"frameSamplesSha256\":\"%s\"}"),
        *PerformanceToken, Passed ? TEXT("true") : TEXT("false"),
        bPerformanceCameraFramed ? TEXT("true") : TEXT("false"),
        PerformanceLowRenderedTiles, PerformanceReferenceRenderedTiles,
        PerformanceLowFrameMs.Num(), LowP95, LowP99, LowMax,
        CountAbove(PerformanceLowFrameMs,50),CountAbove(PerformanceLowFrameMs,100),CountAbove(PerformanceLowFrameMs,250),
        PerformanceReferenceFrameMs.Num(), RefP95, RefP99, RefMax,
        CountAbove(PerformanceReferenceFrameMs,50),CountAbove(PerformanceReferenceFrameMs,100),CountAbove(PerformanceReferenceFrameMs,250),
        PerformanceReopenSeconds, PerformanceFirstRenderSeconds, *FApp::GetGraphicsRHI(),
        GEngine&&GEngine->GameViewport&&GEngine->GameViewport->Viewport?GEngine->GameViewport->Viewport->GetSizeXY().X:0,
        GEngine&&GEngine->GameViewport&&GEngine->GameViewport->Viewport?GEngine->GameViewport->Viewport->GetSizeXY().Y:0,
        *Cpu, *Gpu, Memory.AvailablePhysical,
        TerrainActor?TerrainActor->GetTerrainCoreCacheStats().ConfiguredBudgetBytes:0ULL,
        *FPaths::GetCleanFilename(PerformanceFramesPath),
        FramesRead?*SkiPreparation::Sha256(FrameBytes):TEXT(""));
    const bool Written = FramesRead && FFileHelper::SaveStringToFile(Receipt,
        *PerformanceReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
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

    if (Scenario == TEXT("terraincore-import-edit")
        || Scenario == TEXT("terraincore-offline-reopen"))
    {
        FString EditSetId;
        const bool bOffline = Scenario == TEXT("terraincore-offline-reopen");
        if (bOffline
            && (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1ContentId="), ContentId)
                || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1EditSetId="), EditSetId)))
        {
            return false;
        }
        const uint64 Session = 401;
        const uint64 Operation = bOffline ? 2 : 1;
        TUniquePtr<SkiPreparation::ScopedAcquisitionPortDeny> OfflineGuard;
        if (bOffline)
        {
            OfflineGuard = MakeUnique<SkiPreparation::ScopedAcquisitionPortDeny>();
            if (!OfflineGuard->IsActive()) return false;
        }
        const TSharedPtr<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe> Lease =
            MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(Session, Operation);
        SkiPresentation::TerrainCoreRegressionProof Proof;
        const bool Passed = SkiPresentation::RunTerrainCoreRegression(
            bOffline ? SkiPresentation::TerrainCoreRegressionPhase::OfflineReopen
                     : SkiPresentation::TerrainCoreRegressionPhase::ImportEdit,
            DataRoot, Lease, Session, Operation, ContentId, EditSetId, Proof);
        if (!Passed) return false;

        // Exercise the same actor/session/cache/mesh path used by an installed terrain, not a
        // regression-only adapter. Both phases reopen the immutable package and persisted edits.
        auto RuntimeStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(DataRoot);
        SkiPreparation::TerrainCorePackageIndex RuntimeIndex;
        FString RuntimeError;
        if (!RuntimeStore->Open(Proof.ContentId, RuntimeIndex, RuntimeError)) return false;
        auto BaseRepository = OpenRuntimeTerrainCoreRepository(RuntimeStore, RuntimeIndex, RuntimeError);
        if (!BaseRepository) return false;
        SkiDomain::TerrainEditSet RuntimeEdits;
        if (!RuntimeStore->LoadEditSet(Proof.ContentId, Proof.EditSetId,
                RuntimeIndex.Manifest.Width, RuntimeIndex.Manifest.Height,
                RuntimeEdits, RuntimeError))
        {
            return false;
        }
        std::string RuntimeEditError;
        auto RuntimeRepository = SkiApplication::TerrainCoreEditedRepository::Create(
            BaseRepository, RuntimeEdits, RuntimeEdits.BaseRevision, RuntimeEditError);
        if (!RuntimeRepository) return false;
        TSharedPtr<SkiApplication::TerrainCoreSession> RuntimeSession =
            MakeShared<SkiApplication::TerrainCoreSession>();
        if (!RuntimeSession->Install(RuntimeRepository, RuntimeEdits.EditRevision)) return false;
        ASkiTerrainActor* RuntimeActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
        if (!RuntimeActor || !RuntimeActor->PresentTerrainCore(RuntimeSession, 0)
            || !RuntimeActor->PresentTerrainCore(RuntimeSession, 1)
            || !RuntimeActor->PresentTerrainCore(RuntimeSession, 2)
            || !RuntimeActor->PresentTerrainCore(RuntimeSession, 3)
            || !RuntimeActor->PresentTerrainCore(RuntimeSession, 4)
            || !RuntimeActor->PresentTerrainCore(RuntimeSession, 0))
        {
            if (RuntimeActor) RuntimeActor->Destroy();
            return false;
        }
        const uint32 ProbeColumn = RuntimeIndex.Manifest.Width / 2U;
        const uint32 ProbeRow = RuntimeIndex.Manifest.Height / 2U;
        const double ProbeEast = RuntimeIndex.Manifest.SampleCenterBounds.WestM
            + ProbeColumn * RuntimeIndex.Manifest.DeliveredEastSpacingM;
        const double ProbeNorth = RuntimeIndex.Manifest.SampleCenterBounds.NorthM
            - ProbeRow * RuntimeIndex.Manifest.DeliveredNorthSpacingM;
        const SkiDomain::RayHit RuntimeHit = RuntimeActor->QueryCanonical(
            FVector(ProbeNorth * 100.0, ProbeEast * 100.0, 1000000.0),
            FVector(0.0, 0.0, -1.0));
        const double MutationRadiusM = FMath::Max(
            RuntimeIndex.Manifest.DeliveredEastSpacingM,
            RuntimeIndex.Manifest.DeliveredNorthSpacingM) * 2.5;
        const bool bActorMutation = RuntimeHit.Hit
            && RuntimeActor->ApplyScratchMutation(
                FVector2D(RuntimeHit.Position.East, RuntimeHit.Position.North),
                MutationRadiusM, 0.5);
        const SkiDomain::RayHit RuntimeHitAfterMutation = RuntimeActor->QueryCanonical(
            FVector(ProbeNorth * 100.0, ProbeEast * 100.0, 1000000.0),
            FVector(0.0, 0.0, -1.0));
        const bool bActorMutationObserved = bActorMutation && RuntimeHitAfterMutation.Hit
            && RuntimeHitAfterMutation.SourceRevision > RuntimeHit.SourceRevision
            && RuntimeHitAfterMutation.Position.Up > RuntimeHit.Position.Up;
        const bool bRendererPath = RuntimeActor->IsUsingTerrainCore()
            && RuntimeActor->GetRenderedTerrainCoreTileCount() > 0;
        const bool bRevisionAligned = RuntimeActor->IsTerrainCoreRevisionAligned();
        const bool bActorStaleMeshRejected =
            RuntimeActor->RunStaleTerrainCoreMeshPublicationProbe();
        const int32 RenderedTiles = RuntimeActor->GetRenderedTerrainCoreTileCount();
        const int32 SyntheticGuestMarkers = RuntimeActor->GetSyntheticGuestMarkerCount();
        const int32 OverlaySegments = RuntimeActor->GetOverlaySegmentCount();
        const uint64 DefaultCacheBudgetBytes =
            RuntimeActor->GetTerrainCoreCacheStats().ConfiguredBudgetBytes;
        Proof.bAcquisitionPortGuardInstalled = !bOffline
            || (OfflineGuard && OfflineGuard->IsActive());
        Proof.AcquisitionTransportCalls = OfflineGuard
            ? static_cast<int32>(FMath::Min<uint64>(OfflineGuard->ObservedTransportCalls(), MAX_int32))
            : 0;
        RuntimeActor->Destroy();
        if (!bRendererPath || !bRevisionAligned || !bActorStaleMeshRejected || !RuntimeHit.Hit
            || !bActorMutationObserved
            || (bOffline && (!Proof.bAcquisitionPortGuardInstalled
                || Proof.AcquisitionTransportCalls != 0))
            || SyntheticGuestMarkers != 3000 || OverlaySegments <= 0
            || DefaultCacheBudgetBytes != 512ULL * 1024ULL * 1024ULL) return false;
        FString Factors;
        for (int32 Index = 0; Index < Proof.LodFactors.Num(); ++Index)
        {
            if (Index > 0) Factors += TEXT(",");
            Factors += LexToString(Proof.LodFactors[Index]);
        }
        const FString Receipt = bOffline
            ? FString::Printf(
                TEXT("{\"token\":\"%s\",\"scenario\":\"terraincore-offline-reopen\",\"contentId\":\"%s\",\"editSetId\":\"%s\",\"offlineReopen\":%s,\"finestQuery\":%s,\"editDeltaReconstructed\":%s,\"baseImmutable\":%s,\"networkAttempts\":%d,\"acquisitionPortGuardInstalled\":%s,\"acquisitionTransportCalls\":%d,\"reopenSeconds\":%.6f,\"editedQueryHeightM\":%.6f,\"rendererPath\":%s,\"renderedTileCount\":%d,\"revisionAligned\":%s,\"actorPicked\":%s,\"actorMutationObserved\":%s,\"actorStaleMeshRejected\":%s,\"syntheticGuestMarkers\":%d,\"overlaySegments\":%d}"),
                *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), *Proof.ContentId,
                *Proof.EditSetId, Proof.bOfflineReopened ? TEXT("true") : TEXT("false"),
                Proof.bFinestQuery ? TEXT("true") : TEXT("false"),
                Proof.bEditDeltaReconstructed ? TEXT("true") : TEXT("false"),
                Proof.bBaseImmutable ? TEXT("true") : TEXT("false"), Proof.NetworkAttempts,
                Proof.bAcquisitionPortGuardInstalled ? TEXT("true") : TEXT("false"),
                Proof.AcquisitionTransportCalls,
                Proof.OfflineReopenMilliseconds / 1000.0, Proof.EditedQueryHeightM,
                bRendererPath ? TEXT("true") : TEXT("false"), RenderedTiles,
                bRevisionAligned ? TEXT("true") : TEXT("false"),
                RuntimeHit.Hit ? TEXT("true") : TEXT("false"),
                bActorMutationObserved ? TEXT("true") : TEXT("false"),
                bActorStaleMeshRejected ? TEXT("true") : TEXT("false"),
                SyntheticGuestMarkers, OverlaySegments)
            : FString::Printf(
                TEXT("{\"token\":\"%s\",\"scenario\":\"terraincore-import-edit\",\"schemaVersion\":2,\"contentId\":\"%s\",\"editSetId\":\"%s\",\"lodFactors\":[%s],\"partialEdge\":%s,\"sharedBorder\":%s,\"normalHalo\":%s,\"baseImmutable\":%s,\"finestQuery\":%s,\"editPersisted\":%s,\"editDeltaReconstructed\":%s,\"staleBuildRejected\":%s,\"networkAttempts\":%d,\"residentBudgetBytes\":%llu,\"peakResidentBytes\":%llu,\"evictionCount\":%u,\"defaultCacheBudgetBytes\":%llu,\"rendererPath\":%s,\"renderedTileCount\":%d,\"revisionAligned\":%s,\"actorPicked\":%s,\"actorMutationObserved\":%s,\"actorStaleMeshRejected\":%s,\"syntheticGuestMarkers\":%d,\"overlaySegments\":%d}"),
                *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), *Proof.ContentId,
                *Proof.EditSetId, *Factors, Proof.bPartialEdgeTile ? TEXT("true") : TEXT("false"),
                Proof.bSharedBorder ? TEXT("true") : TEXT("false"),
                Proof.bNormalHalo ? TEXT("true") : TEXT("false"),
                Proof.bBaseImmutable ? TEXT("true") : TEXT("false"),
                Proof.bFinestQuery ? TEXT("true") : TEXT("false"),
                Proof.bEditPersisted ? TEXT("true") : TEXT("false"),
                Proof.bEditDeltaReconstructed ? TEXT("true") : TEXT("false"),
                Proof.bStalePublicationRejected ? TEXT("true") : TEXT("false"),
                Proof.NetworkAttempts, Proof.CacheBudgetBytes, Proof.PeakResidentBytes,
                Proof.ObservedEvictions, DefaultCacheBudgetBytes,
                bRendererPath ? TEXT("true") : TEXT("false"),
                RenderedTiles, bRevisionAligned ? TEXT("true") : TEXT("false"),
                RuntimeHit.Hit ? TEXT("true") : TEXT("false"),
                bActorMutationObserved ? TEXT("true") : TEXT("false"),
                bActorStaleMeshRejected ? TEXT("true") : TEXT("false"),
                SyntheticGuestMarkers, OverlaySegments);
        return FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    if (Scenario == TEXT("acquisition-regression"))
    {
        const SkiDomain::GeographicBounds MountWashington{-71.365, 44.225, -71.241, 44.315};
        const SkiPreparation::AcquisitionPlan Plan = SkiPreparation::BuildElevationAcquisitionPlan(
            MountWashington, SkiPreparation::SourceProfile::Medium);
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

    FString EditSetId;
    SkiPreparation::InstalledTerrainStore InstallationStore(DataRoot);
    SkiPreparation::TerrainCorePackageStore CoreStore(DataRoot);
    SkiPreparation::CoverEcologyStore CoverStore(DataRoot);
    SkiPreparation::InstalledTerrainIndex InstallationIndex;
    SkiPreparation::TerrainCorePackageIndex CoreIndex;
    SkiPreparation::CoverEcologyPackageIndex CoverIndex;
    TArray<uint8> Cover;
    TArray<uint8> CoverValidity;
    FString Error;
    if (Scenario == TEXT("import"))
    {
        SkiPreparation::Request Request;
        Request.Name = TEXT("Crystal Mountain P1 Medium qualification");
        Request.Bounds = {-121.489, 46.925, -121.462, 46.947};
        Request.Profile = SkiPreparation::SourceProfile::Medium;
        Request.SessionGeneration = 1;
        Request.OperationGeneration = 1;
        Request.Lease = MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(1, 1);
        SkiPreparation::FixtureTerrainProvider Provider(DataRoot);
        const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
        SkiPreparation::Result Result = Provider.Prepare(Request, Cancellation, {});
        if (!Result.Ok || !Result.HasNativeV2Installation) return false;
        ContentId = UTF8_TO_TCHAR(Result.InstallationReceipt.ContentId.c_str());
    }
    else if (Scenario == TEXT("offline-reopen"))
    {
        if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1ContentId="), ContentId)
            || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1EditSetId="), EditSetId)
            || !IsContentId(EditSetId)) return false;
    }
    else return false;

    TUniquePtr<SkiPreparation::ScopedAcquisitionPortDeny> OfflineGuard;
    if (Scenario == TEXT("offline-reopen"))
    {
        OfflineGuard = MakeUnique<SkiPreparation::ScopedAcquisitionPortDeny>();
        if (!OfflineGuard->IsActive()) return false;
    }
    if (!InstallationStore.Open(ContentId, InstallationIndex, Error)
        || !CoreStore.Open(UTF8_TO_TCHAR(InstallationIndex.Receipt.TerrainCoreId.c_str()),
            CoreIndex, Error)
        || !CoreStore.Verify(CoreIndex, Error)
        || !CoverStore.Open(UTF8_TO_TCHAR(InstallationIndex.Receipt.CoverEcologyId.c_str()),
            CoverIndex, Error)
        || !CoverStore.Verify(CoverIndex, Error)
        || !CoverStore.ReadChannels(CoverIndex, Cover, CoverValidity, Error)) return false;
    auto RuntimeStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(DataRoot);
    auto Repository = OpenRuntimeTerrainCoreRepository(RuntimeStore, CoreIndex, Error);
    if (!Repository) return false;
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> PresentedRepository = Repository;
    SkiDomain::Revision PresentedRevision = 1;
    bool EditDeltaReconstructed = false;
    if (Scenario == TEXT("offline-reopen"))
    {
        SkiDomain::TerrainEditSet ReopenedEdits;
        if (!CoreStore.LoadEditSet(UTF8_TO_TCHAR(InstallationIndex.Receipt.TerrainCoreId.c_str()),
                EditSetId, CoreIndex.Manifest.Width, CoreIndex.Manifest.Height,
                ReopenedEdits, Error)) return false;
        std::string EditError;
        auto Edited = SkiApplication::TerrainCoreEditedRepository::Create(
            Repository, ReopenedEdits, ReopenedEdits.BaseRevision, EditError);
        if (!Edited || ReopenedEdits.Deltas.empty()) return false;
        PresentedRepository = Edited;
        PresentedRevision = ReopenedEdits.EditRevision;
        EditDeltaReconstructed = true;
    }
    TerrainCoreSession = MakeShared<SkiApplication::TerrainCoreSession>();
    if (!TerrainCoreSession->Install(PresentedRepository, PresentedRevision)) return false;
    auto RuntimeCover = CopyCoverChannel(Cover);
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    if (!TerrainActor) return false;
    TerrainActor->SetTerrainCoreCover(RuntimeCover, CopyCoverChannel(CoverValidity),
        CoverIndex.Manifest.Transform);
    if (!TerrainActor->PresentTerrainCore(TerrainCoreSession, 0)
        || !TerrainActor->PresentTerrainCore(TerrainCoreSession, 1)
        || !TerrainActor->PresentTerrainCore(TerrainCoreSession, 2)
        || !TerrainActor->PresentTerrainCore(TerrainCoreSession, 0)) return false;
    const double ProbeEast = (CoreIndex.Manifest.SampleCenterBounds.WestM
        + CoreIndex.Manifest.SampleCenterBounds.EastM) * 0.5;
    const double ProbeNorth = (CoreIndex.Manifest.SampleCenterBounds.SouthM
        + CoreIndex.Manifest.SampleCenterBounds.NorthM) * 0.5;
    const SkiDomain::RayHit Hit = TerrainActor->QueryCanonical(
        FVector(ProbeNorth * 100.0, ProbeEast * 100.0, 1000000.0), FVector(0, 0, -1));
    if (!Hit.Hit) return false;
    bool MutationObserved = true;
    double EditDeltaM = 0.0;
    double EditedQueryHeightM = Hit.Position.Up;
    if (Scenario == TEXT("import"))
    {
        const double RadiusM = FMath::Max(CoreIndex.Manifest.DeliveredEastSpacingM,
            CoreIndex.Manifest.DeliveredNorthSpacingM) * 3.0;
        if (!TerrainActor->ApplyScratchMutation(
                FVector2D(Hit.Position.East, Hit.Position.North), RadiusM, 3.0)) return false;
        const SkiDomain::RayHit Mutated = TerrainActor->QueryCanonical(
            FVector(ProbeNorth * 100.0, ProbeEast * 100.0, 1000000.0), FVector(0, 0, -1));
        MutationObserved = Mutated.Hit && Mutated.Position.Up > Hit.Position.Up + 0.01;
        if (!MutationObserved) return false;
        EditDeltaM = Mutated.Position.Up - Hit.Position.Up;
        EditedQueryHeightM = Mutated.Position.Up;
        const SkiApplication::TerrainCoreSnapshot EditedSnapshot = TerrainCoreSession->Snapshot();
        std::shared_ptr<const SkiApplication::ITerrainCoreRepository> BaseRepository;
        std::shared_ptr<const SkiDomain::TerrainEditSet> Edits;
        if (!EditedSnapshot.Repository || !EditedSnapshot.Repository->FlattenEditOverlay(
                BaseRepository, Edits) || !Edits || !BaseRepository
            || !CoreStore.WriteEditSetAndActivate(*Edits, CoreIndex.Manifest.Width,
                CoreIndex.Manifest.Height, EditSetId, Error)) return false;
        SkiDomain::TerrainEditSet SavedEdits;
        EditDeltaReconstructed = CoreStore.LoadEditSet(
            UTF8_TO_TCHAR(InstallationIndex.Receipt.TerrainCoreId.c_str()), EditSetId,
            CoreIndex.Manifest.Width, CoreIndex.Manifest.Height, SavedEdits, Error)
            && SavedEdits.EditRevision == Edits->EditRevision
            && SavedEdits.Deltas.size() == Edits->Deltas.size();
        if (!EditDeltaReconstructed) return false;
    }
    const SkiApplication::TerrainCoreSnapshot Snapshot = TerrainCoreSession->Snapshot();
    const bool Ready = Snapshot.RenderReady() && Snapshot.QueryReady()
        && TerrainActor->IsTerrainCoreRevisionAligned();
    const int32 MarkerCount = TerrainActor->GetSyntheticGuestMarkerCount();
    const int32 OverlaySegments = TerrainActor->GetOverlaySegmentCount();
    TerrainActor->Destroy();
    TerrainActor = nullptr;
    TerrainCoreSession.Reset();
    SkiPreparation::InstalledTerrainIndex ReopenedInstallation;
    SkiPreparation::TerrainCorePackageIndex ReopenedCore;
    SkiPreparation::CoverEcologyPackageIndex ReopenedCover;
    const bool Reopened = InstallationStore.Open(ContentId, ReopenedInstallation, Error)
        && CoreStore.Open(UTF8_TO_TCHAR(ReopenedInstallation.Receipt.TerrainCoreId.c_str()),
            ReopenedCore, Error)
        && CoverStore.Open(UTF8_TO_TCHAR(ReopenedInstallation.Receipt.CoverEcologyId.c_str()),
            ReopenedCover, Error);
    const bool OfflineNoAcquisition = !OfflineGuard
        || (OfflineGuard->IsActive() && OfflineGuard->ObservedTransportCalls() == 0);
    const FString Receipt = FString::Printf(
        TEXT("{\"token\":\"%s\",\"scenario\":\"%s\",\"qualityTier\":\"medium\",\"schemaVersion\":2,\"contentId\":\"%s\",\"terrainCoreId\":\"%s\",\"coverEcologyId\":\"%s\",\"editSetId\":\"%s\",\"baseQueryHeightM\":%.6f,\"editedQueryHeightM\":%.6f,\"editDeltaM\":%.6f,\"editDeltaReconstructed\":%s,\"ready\":%s,\"picked\":true,\"mutationObserved\":%s,\"reopened\":%s,\"offlineReopen\":%s,\"acquisitionPortGuardInstalled\":%s,\"acquisitionTransportCalls\":%llu,\"nativeV2\":true,\"optionalOutcomes\":%d,\"syntheticGuestMarkers\":%d,\"overlaySegments\":%d}"),
        *ParsedToken.ToString(EGuidFormats::DigitsWithHyphensLower), *Scenario, *ContentId,
        UTF8_TO_TCHAR(InstallationIndex.Receipt.TerrainCoreId.c_str()),
        UTF8_TO_TCHAR(InstallationIndex.Receipt.CoverEcologyId.c_str()),
        *EditSetId, Hit.Position.Up, EditedQueryHeightM, EditDeltaM,
        EditDeltaReconstructed ? TEXT("true") : TEXT("false"),
        Ready ? TEXT("true") : TEXT("false"), MutationObserved ? TEXT("true") : TEXT("false"),
        Reopened ? TEXT("true") : TEXT("false"),
        Scenario == TEXT("offline-reopen") ? TEXT("true") : TEXT("false"),
        OfflineGuard && OfflineGuard->IsActive() ? TEXT("true") : TEXT("false"),
        OfflineGuard ? OfflineGuard->ObservedTransportCalls() : 0ULL,
        static_cast<int32>(InstallationIndex.Receipt.OptionalSources.size()),
        MarkerCount, OverlaySegments);
    return Ready && Reopened && MutationObserved && EditDeltaReconstructed
        && IsContentId(EditSetId) && OfflineNoAcquisition
        && MarkerCount == 3000 && OverlaySegments > 0
        && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
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
    FString Details;
    // Schema-1 remains installed/readable for compatibility, but every new successful
    // preparation is normalized into the disk-backed TerrainCore v2 path before gameplay.
    auto CoreStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(
        FPaths::ProjectSavedDir());
    SkiDomain::TerrainCoreManifest CoreInstalled;
    FString CoreDirectory;
    FString CoreError;
    if (Result.HasNativeV2Installation)
    {
        CoreInstalled = Result.TerrainCoreManifest;
        CoreDirectory = Result.PackageDirectory;
    }
    else if (!CoreStore->WriteAndActivate(
        MakeTerrainCoreManifest(Result.Manifest, Result.Heightfield), Result.Heightfield,
        CoreDirectory, CoreInstalled, CoreError, PreparationLease,
        SessionGeneration, OperationGeneration))
    {
        if (P1Widget) P1Widget->SetTransientStatus(
            TEXT("TerrainCore verification/activation failed: ") + CoreError);
        return;
    }
    SkiPreparation::TerrainCorePackageIndex CoreIndex;
    if (!CoreStore->Open(UTF8_TO_TCHAR(CoreInstalled.ContentId.c_str()), CoreIndex, CoreError))
    {
        if (P1Widget) P1Widget->SetTransientStatus(
            TEXT("Installed TerrainCore could not be reopened: ") + CoreError);
        return;
    }
    Details = FString::Printf(
        TEXT("Medium terrain | %s\nActual %u x %u samples | delivered %.2f x %.2f m\nNative source spacing: %s\nGround grid processing: %s\nRequested %.6f, %.6f - %.6f, %.6f\nReturned %.6f, %.6f - %.6f, %.6f\nDatum %s\nTerrainCore %s\nCoverEcology %s\nInstallation %s"),
        UTF8_TO_TCHAR(Result.Manifest.Source.c_str()), Result.Manifest.HeightWidth,
        Result.Manifest.HeightHeight, Result.Manifest.EastSpacingM, Result.Manifest.NorthSpacingM,
        CoreInstalled.Source.NativeSpacingReported
            ? *FString::Printf(TEXT("%.2f x %.2f m"), CoreInstalled.Source.NativeEastSpacingM,
                CoreInstalled.Source.NativeNorthSpacingM) : TEXT("not uniformly reported by source export"),
        CoreInstalled.Source.SourceId == "usgs-3dep-export"
            ? TEXT("bilinear sampled from source export") : TEXT("see source provenance"),
        Result.Manifest.RequestedBounds.WestDeg, Result.Manifest.RequestedBounds.SouthDeg,
        Result.Manifest.RequestedBounds.EastDeg, Result.Manifest.RequestedBounds.NorthDeg,
        Result.Manifest.ActualBounds.WestDeg, Result.Manifest.ActualBounds.SouthDeg,
        Result.Manifest.ActualBounds.EastDeg, Result.Manifest.ActualBounds.NorthDeg,
        UTF8_TO_TCHAR(Result.Manifest.VerticalDatum.c_str()),
        UTF8_TO_TCHAR(CoreInstalled.ContentId.c_str()),
        Result.HasNativeV2Installation
            ? UTF8_TO_TCHAR(Result.CoverEcologyManifest.ContentId.c_str()) : TEXT("legacy in-package cover"),
        Result.HasNativeV2Installation
            ? UTF8_TO_TCHAR(Result.InstallationReceipt.ContentId.c_str()) : TEXT("legacy normalized session"));
    auto CoreRepository = OpenRuntimeTerrainCoreRepository(CoreStore, CoreIndex, CoreError);
    if (!CoreRepository)
    {
        if (P1Widget) P1Widget->SetTransientStatus(
            TEXT("Installed TerrainCore repository could not open: ") + CoreError);
        return;
    }
    TerrainSession.Reset();
    TerrainCoreSession = MakeShared<SkiApplication::TerrainCoreSession>();
    if (!TerrainCoreSession->Install(CoreRepository, Result.Heightfield.CurrentRevision))
    {
        if (P1Widget) P1Widget->SetTransientStatus(
            TEXT("Installed TerrainCore could not enter the terrain session."));
        return;
    }
    auto RuntimeCover = CopyCoverChannel(Result.Cover);
    auto RuntimeCoverValidity = PackCoverValidity(Result.CoverValidity,
        Result.CoverEcologyManifest.Transform.Width,
        Result.CoverEcologyManifest.Transform.Height);
    if (!RuntimeCoverValidity)
    {
        if (P1Widget) P1Widget->SetTransientStatus(TEXT("Prepared cover validity is malformed."));
        return;
    }
    if (TerrainActor)
    {
        TerrainActor->SetTerrainCoreReadyHandler({});
        TerrainActor->Destroy();
        TerrainActor = nullptr;
    }
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    if (!TerrainActor)
    {
        if (P1Widget) P1Widget->SetTransientStatus(TEXT("TerrainCore actor creation failed."));
        return;
    }
    TerrainActor->SetTerrainCoreCover(RuntimeCover, RuntimeCoverValidity,
        Result.CoverEcologyManifest.Transform);
    bTerrainCoreInitialFramePending = true;
    TerrainActor->SetTerrainCoreReadyHandler(
        [WeakThis = TWeakObjectPtr<ASkiBootstrapGameMode>(this),
            WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget),
            CoreContentId = FString(UTF8_TO_TCHAR(CoreInstalled.ContentId.c_str()))](const bool bReady)
        {
            if (WeakWidget.IsValid())
            {
                WeakWidget->SetTransientStatus(bReady
                    ? TEXT("Installed TerrainCore is render/query ready. Scratch edits are separate from the immutable package.")
                    : TEXT("TerrainCore streaming failed to align render and query revisions."));
                if (WeakThis.IsValid() && WeakThis->TerrainCoreSession)
                {
                    const auto Snapshot = WeakThis->TerrainCoreSession->Snapshot();
                    WeakWidget->SetNodeStatus(FString::Printf(
                        TEXT("Runtime node view\n• Selection → Medium terrain\n• Required ground: verified\n• Required analytical WorldCover: verified\n• Installed TerrainCore: %s\n• Canonical/render/query: %llu/%llu/%llu"),
                        *CoreContentId, static_cast<uint64>(Snapshot.Revisions.Canonical),
                        static_cast<uint64>(Snapshot.Revisions.Render),
                        static_cast<uint64>(Snapshot.Revisions.Query)));
                }
            }
            if (WeakThis.IsValid() && bReady && WeakThis->bTerrainCoreInitialFramePending)
            {
                WeakThis->bTerrainCoreInitialFramePending = false;
                if (ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(
                        WeakThis->GetWorld()->GetFirstPlayerController()))
                {
                    Controller->FrameAll();
                }
            }
        });
    if (!TerrainActor->BeginTerrainCoreStreaming(TerrainCoreSession, 4))
    {
        TerrainActor->SetTerrainCoreReadyHandler({});
        TerrainActor->Destroy();
        TerrainActor = nullptr;
        if (P1Widget) P1Widget->SetTransientStatus(
            TEXT("TerrainCore renderer rejected the installed overview."));
        return;
    }
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
        P1Widget->SetNodeStatus(FString::Printf(
            TEXT("Runtime node view\n• Selection → Medium terrain\n• Required ground: verified\n• Required analytical WorldCover: verified\n• Installed TerrainCore: %s\n• Canonical/render/query: %llu/%llu/%llu"),
            UTF8_TO_TCHAR(CoreInstalled.ContentId.c_str()),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Canonical),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Render),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Query)));
        P1Widget->SetViewCommandHandler([WeakTerrain = TWeakObjectPtr<ASkiTerrainActor>(TerrainActor)](const FName Command)
        {
            if (!WeakTerrain.IsValid()) return;
            if (Command == TEXT("Elevation")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Elevation);
            else if (Command == TEXT("Slope")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Slope);
            else if (Command == TEXT("Cover")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Cover);
            else if (Command == TEXT("Lod")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::TileLod);
            else if (Command == TEXT("LodAuto")) WeakTerrain->SetLodAuto();
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
        P1Widget->SetTransientStatus(TEXT("Installed TerrainCore verified; streaming the overview.") + WarningText);
    }
}

void ASkiBootstrapGameMode::OpenLatestInstalledTerrain()
{
    const FString Root = FPaths::ProjectSavedDir();
    const FString Installations = FPaths::Combine(Root, TEXT("InstalledTerrain"));
    TArray<FString> Names;
    IFileManager::Get().FindFiles(Names, *FPaths::Combine(Installations, TEXT("*")),
        false, true);
    TArray<TPair<FDateTime, FString>> Candidates;
    for (const FString& Name : Names)
    {
        if (IsContentId(Name))
            Candidates.Emplace(IFileManager::Get().GetTimeStamp(
                *FPaths::Combine(Installations, Name)), Name);
    }
    // Directory time is only a chooser. Open validates the receipt and all required stores.
    Candidates.Sort([](const TPair<FDateTime, FString>& A,
        const TPair<FDateTime, FString>& B)
    {
        return A.Key == B.Key ? A.Value < B.Value : A.Key > B.Key;
    });
    SkiPreparation::InstalledTerrainStore Store(Root);
    for (const auto& Candidate : Candidates)
    {
        SkiPreparation::InstalledTerrainIndex Index;
        FString Error;
        if (Store.Open(Candidate.Value, Index, Error))
        {
            OpenInstalledTerrain(Candidate.Value);
            return;
        }
    }
    if (P1Widget)
    {
        P1Widget->OpenSelector();
        P1Widget->SetSelectorStatus(TEXT("No verified installed terrain was found. Choose bounds to prepare one."));
    }
}

bool ASkiBootstrapGameMode::OpenInstalledTerrain(const FString& ContentId)
{
    const auto Fail = [this](const FString& Message)
    {
        if (P1Widget)
        {
            P1Widget->OpenSelector();
            P1Widget->SetSelectorStatus(Message);
        }
        return false;
    };
    if (!IsContentId(ContentId)) return Fail(TEXT("Installed terrain ID is invalid."));
    SkiPreparation::ScopedAcquisitionPortDeny OfflineGuard;
    if (!OfflineGuard.IsActive())
        return Fail(TEXT("Offline terrain reopen could not disable acquisition."));
    const FString Root = FPaths::ProjectSavedDir();
    SkiPreparation::InstalledTerrainStore InstallationStore(Root);
    auto CoreStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(Root);
    SkiPreparation::CoverEcologyStore CoverStore(Root);
    SkiPreparation::InstalledTerrainIndex Installation;
    SkiPreparation::TerrainCorePackageIndex Core;
    SkiPreparation::CoverEcologyPackageIndex Ecology;
    TArray<uint8> Cover;
    TArray<uint8> Validity;
    FString Error;
    if (!InstallationStore.Open(ContentId, Installation, Error)
        || !CoreStore->Open(UTF8_TO_TCHAR(Installation.Receipt.TerrainCoreId.c_str()),
            Core, Error)
        || !CoverStore.Open(UTF8_TO_TCHAR(Installation.Receipt.CoverEcologyId.c_str()),
            Ecology, Error)
        || !CoverStore.ReadChannels(Ecology, Cover, Validity, Error))
    {
        return Fail(TEXT("Installed terrain could not be verified: ") + Error);
    }
    auto Repository = OpenRuntimeTerrainCoreRepository(CoreStore, Core, Error);
    if (!Repository) return Fail(TEXT("Installed terrain repository could not open: ") + Error);
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> PresentedRepository = Repository;
    SkiDomain::Revision Revision = 1;
    FString EditSetId;
    if (FParse::Value(FCommandLine::Get(), TEXT("SkiP1EditSetId="), EditSetId))
    {
        SkiDomain::TerrainEditSet Edits;
        if (!IsContentId(EditSetId)
            || !CoreStore->LoadEditSet(UTF8_TO_TCHAR(Installation.Receipt.TerrainCoreId.c_str()),
                EditSetId, Core.Manifest.Width, Core.Manifest.Height, Edits, Error))
            return Fail(TEXT("Installed terrain edit sidecar could not open: ") + Error);
        std::string EditError;
        auto Edited = SkiApplication::TerrainCoreEditedRepository::Create(
            Repository, Edits, Edits.BaseRevision, EditError);
        if (!Edited)
            return Fail(FString(TEXT("Installed terrain edit sidecar is invalid: "))
                + UTF8_TO_TCHAR(EditError.c_str()));
        PresentedRepository = Edited;
        Revision = Edits.EditRevision;
    }
    if (OfflineGuard.ObservedTransportCalls() != 0)
        return Fail(TEXT("Offline reopen attempted network acquisition."));
    if (PreparationCancellation) PreparationCancellation->Cancel();
    if (PreparationLease) PreparationLease->Invalidate();
    ++ActiveOperationGeneration;
    LastRequest.Reset();
    if (P1Widget) P1Widget->BeginPreparationUI([this] { ChangeSelection(); });
    if (TerrainActor)
    {
        TerrainActor->SetTerrainCoreReadyHandler({});
        TerrainActor->Destroy();
        TerrainActor = nullptr;
    }
    TerrainSession.Reset();
    TerrainCoreSession = MakeShared<SkiApplication::TerrainCoreSession>();
    if (!TerrainCoreSession->Install(PresentedRepository, Revision))
        return Fail(TEXT("Installed terrain session could not open."));
    TerrainActor = GetWorld()->SpawnActor<ASkiTerrainActor>();
    if (!TerrainActor) return Fail(TEXT("Installed terrain actor creation failed."));
    TerrainActor->SetTerrainCoreCover(CopyCoverChannel(Cover), CopyCoverChannel(Validity),
        Ecology.Manifest.Transform);
    bTerrainCoreInitialFramePending = true;
    TerrainActor->SetTerrainCoreReadyHandler(
        [WeakThis = TWeakObjectPtr<ASkiBootstrapGameMode>(this),
            WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget), ContentId](const bool bReady)
        {
            if (WeakWidget.IsValid())
                WeakWidget->SetTransientStatus(bReady
                    ? TEXT("Installed terrain reopened offline; render and query are ready.")
                    : TEXT("Installed terrain streaming failed to align render and query revisions."));
            if (WeakThis.IsValid() && bReady && WeakThis->bTerrainCoreInitialFramePending)
            {
                WeakThis->bTerrainCoreInitialFramePending = false;
                if (ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(
                        WeakThis->GetWorld()->GetFirstPlayerController())) Controller->FrameAll();
            }
        });
    if (!TerrainActor->BeginTerrainCoreStreaming(TerrainCoreSession, 4))
    {
        TerrainActor->SetTerrainCoreReadyHandler({});
        TerrainActor->Destroy();
        TerrainActor = nullptr;
        return Fail(TEXT("Installed terrain renderer rejected the overview."));
    }
    TerrainActor->SetLightingPreset(TEXT("Midday"));
    if (ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(
            GetWorld()->GetFirstPlayerController()))
    {
        Controller->AttachTerrain(TerrainActor);
        Controller->SetStatusHandler([WeakWidget = TWeakObjectPtr<USkiP1Widget>(P1Widget)](
            const FString& Status) { if (WeakWidget.IsValid()) WeakWidget->SetProbeStatus(Status); });
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
        P1Widget->SetTerrainDetails(FString::Printf(
            TEXT("Medium terrain | reopened offline\n%s\nActual %u x %u samples | delivered %.2f x %.2f m\nNative source spacing: %s\nGround grid processing: %s\nDatum %s\nTerrainCore %s\nCoverEcology %s\nInstallation %s%s"),
            UTF8_TO_TCHAR(Core.Manifest.Source.Product.c_str()), Core.Manifest.Width,
            Core.Manifest.Height, Core.Manifest.DeliveredEastSpacingM,
            Core.Manifest.DeliveredNorthSpacingM,
            Core.Manifest.Source.NativeSpacingReported
                ? *FString::Printf(TEXT("%.2f x %.2f m"),
                    Core.Manifest.Source.NativeEastSpacingM,
                    Core.Manifest.Source.NativeNorthSpacingM)
                : TEXT("not uniformly reported by source export"),
            Core.Manifest.Source.SourceId == "usgs-3dep-export"
                ? TEXT("bilinear sampled from source export") : TEXT("see source provenance"),
            UTF8_TO_TCHAR(Core.Manifest.Source.VerticalDatum.c_str()),
            UTF8_TO_TCHAR(Core.Manifest.ContentId.c_str()),
            UTF8_TO_TCHAR(Ecology.Manifest.ContentId.c_str()), *ContentId,
            EditSetId.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("\nEdit sidecar %s"), *EditSetId)),
            Core.Manifest.Source.SourceId.find("fixture") != std::string::npos);
        P1Widget->SetNodeStatus(FString::Printf(
            TEXT("Runtime node view\nInstalled TerrainCore: %s\nCanonical/render/query: %llu/%llu/%llu"),
            UTF8_TO_TCHAR(Core.Manifest.ContentId.c_str()),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Canonical),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Render),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Query)));
        P1Widget->SetViewCommandHandler([WeakTerrain = TWeakObjectPtr<ASkiTerrainActor>(TerrainActor)](
            const FName Command)
        {
            if (!WeakTerrain.IsValid()) return;
            if (Command == TEXT("Elevation")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Elevation);
            else if (Command == TEXT("Slope")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Slope);
            else if (Command == TEXT("Cover")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::Cover);
            else if (Command == TEXT("Lod")) WeakTerrain->SetViewMode(ESkiTerrainViewMode::TileLod);
            else if (Command == TEXT("LodAuto")) WeakTerrain->SetLodAuto();
            else if (Command == TEXT("Lod0")) WeakTerrain->SetLod(0);
            else if (Command == TEXT("Lod1")) WeakTerrain->SetLod(1);
            else if (Command == TEXT("Lod2")) WeakTerrain->SetLod(2);
            else if (Command == TEXT("Vertical1")) WeakTerrain->SetVerticalExaggeration(1.0F);
            else if (Command == TEXT("Vertical2")) WeakTerrain->SetVerticalExaggeration(2.0F);
            else if (Command == TEXT("Vertical4")) WeakTerrain->SetVerticalExaggeration(4.0F);
            else if (Command == TEXT("Midday") || Command == TEXT("LowAngle")
                || Command == TEXT("Overcast")) WeakTerrain->SetLightingPreset(Command);
            else WeakTerrain->SetViewMode(ESkiTerrainViewMode::Presentation);
        });
        P1Widget->SetTransientStatus(TEXT("Verified installed terrain; streaming offline overview."));
    }
    return true;
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
    if (ASkiTerrainViewController* Controller = Cast<ASkiTerrainViewController>(
            GetWorld()->GetFirstPlayerController()))
    {
        Controller->AttachTerrain(nullptr);
    }
    if (TerrainActor)
    {
        TerrainActor->SetTerrainCoreReadyHandler({});
        TerrainActor->Destroy();
        TerrainActor = nullptr;
    }
    TerrainCoreSession.Reset();
    TerrainSession.Reset();
    bTerrainCoreInitialFramePending = false;
    if (P1Widget) P1Widget->ResetSelector();
}
