#include "SkiBootstrapGameMode.h"
#include "SkiBootstrapWidget.h"
#include "SkiP1Widget.h"
#include "SkiApplication/Bootstrap.h"
#include "SkiPreparation/FixtureTerrainProvider.h"
#include "SkiPreparation/NativeTerrainProvider.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

void ASkiBootstrapGameMode::BeginPlay()
{
    Super::BeginPlay();
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
            [WeakThis](const SkiPreparation::Progress& Progress)
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis, Progress]
                {
                    if (WeakThis.IsValid() && WeakThis->P1Widget) WeakThis->P1Widget->SetPreparationProgress(Progress);
                });
            });
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Result = std::move(Result)]() mutable
        {
            if (WeakThis.IsValid()) WeakThis->FinishP1Preparation(std::move(Result));
        });
    });
}

void ASkiBootstrapGameMode::FinishP1Preparation(SkiPreparation::Result Result)
{
    if (!Result.Ok)
    {
        if (P1Widget) P1Widget->SetTransientStatus(Result.Error);
        return;
    }
    const TArray<FString> Warnings = Result.Warnings;
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
    if (APlayerController* Controller = GetWorld()->GetFirstPlayerController())
    {
        Controller->SetViewTarget(TerrainActor);
        Controller->bShowMouseCursor = true;
        Controller->SetInputMode(FInputModeGameAndUI());
    }
    if (P1Widget)
    {
        const FString WarningText = Warnings.IsEmpty() ? FString() : TEXT(" Warnings: ") + FString::Join(Warnings, TEXT(" "));
        P1Widget->SetTransientStatus(TEXT("Installed terrain is render/query ready. Scratch edits are separate from the package.") + WarningText);
    }
}
