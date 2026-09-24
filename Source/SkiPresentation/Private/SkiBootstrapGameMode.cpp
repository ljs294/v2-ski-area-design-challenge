#include "SkiBootstrapGameMode.h"
#include "SkiBootstrapWidget.h"
#include "SkiFlowSubsystem.h"
#include "SkiP1Widget.h"
#include "SkiTerrainViewController.h"
#include "SkiTerrainCoreRegression.h"
#include "SkiSiteMapSpikeRunner.h"
#include "SkiM0ShippingProjectionProbe.h"
#include "SkiM0TerrainRenderProbe.h"
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
#include "SkiPreparation/M0TerrainCoreScale.h"
#include "SkiPreparation/M0RasterProjectionProbe.h"
#include "SkiPreparation/PlaceSearch.h"
#include "SkiPreparation/SiteContext.h"
#include "SkiPreparation/SiteContextPhotoDecoder.h"
#include "SkiPreparation/SkiNetGateway.h"
#include "SkiTerrainRuntime/SkiTerrainActor.h"
#include "SkiSiteMap.h"
#include "Async/Async.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "HighResScreenshot.h"
#include "GameFramework/PlayerController.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Components/VerticalBox.h"
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
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

#include <algorithm>
#include <atomic>

struct FSkiInstalledPhotoCompletion
{
    uint64 RequestSerial = 0;
    uint64 Generation = 0;
    SkiApplication::TerrainCoreTileKey Key;
    bool bSucceeded = false;
    TArray<FColor> Pixels;
    FString Error;
};

struct FSkiInstalledPhotoMailbox
{
    std::atomic_bool Cancelled = false;
    FCriticalSection Mutex;
    TArray<FSkiInstalledPhotoCompletion> Completions;
};

struct FSkiInstalledPhotoStreamState
{
    uint64 RequestSerial = 0;
    uint64 Generation = 0;
    TArray<SkiApplication::TerrainCoreTileKey> DesiredKeys;
    int32 NextKeyIndex = 0;
    int32 InFlightReads = 0;
    std::shared_ptr<FSkiInstalledPhotoMailbox> Mailbox;
    std::shared_ptr<const SkiPreparation::SiteContextPackageIndex> SiteContext;
    FString DataRoot;
    bool bReportedReadFailure = false;
    int32 SubmittedTiles = 0;
};

struct FSkiPreparedInstalledTerrain
{
    std::shared_ptr<SkiPreparation::TerrainCorePackageStore> CoreStore;
    SkiPreparation::InstalledTerrainIndex Installation;
    SkiPreparation::TerrainCorePackageIndex Core;
    SkiPreparation::CoverEcologyPackageIndex Ecology;
    SkiPreparation::SiteContextPackageIndex SiteContext;
    TArray<uint8> Cover;
    TArray<uint8> Validity;
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> PresentedRepository;
    SkiDomain::Revision Revision = 1;
    FString EditSetId;
    FString DataRoot;
    FString Error;
    bool bHasVerifiedSiteContext = false;
    bool bReady = false;
};

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

std::shared_ptr<FSkiPreparedInstalledTerrain> PrepareInstalledTerrain(
    const FString& Root, const FString& ContentId, const FString& EditSetId)
{
    auto Prepared = std::make_shared<FSkiPreparedInstalledTerrain>();
    Prepared->EditSetId = EditSetId;
    Prepared->DataRoot = Root;
    SkiPreparation::InstalledTerrainStore InstallationStore(Root);
    Prepared->CoreStore = std::make_shared<SkiPreparation::TerrainCorePackageStore>(Root);
    SkiPreparation::CoverEcologyStore CoverStore(Root);
    FString TerrainCoreId;
    FString CoverEcologyId;
    if (!InstallationStore.Open(ContentId, Prepared->Installation, Prepared->Error))
        return Prepared;
    if (!ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(ContentId,
            Prepared->Installation, true, TerrainCoreId, CoverEcologyId))
    {
        Prepared->Error = TEXT("Installed receipt is not a verified playable schema-2 or schema-3 package.");
        return Prepared;
    }
    if (!Prepared->CoreStore->Open(TerrainCoreId, Prepared->Core, Prepared->Error)
        || !CoverStore.Open(CoverEcologyId, Prepared->Ecology, Prepared->Error)
        || !CoverStore.ReadChannels(Prepared->Ecology, Prepared->Cover,
            Prepared->Validity, Prepared->Error)) return Prepared;

    if (Prepared->Installation.SchemaVersion == SkiPreparation::CompositeInstallReceiptSchema)
    {
        const SkiPreparation::CompositeInstallComponent* SiteContextComponent = nullptr;
        for (const SkiPreparation::CompositeInstallComponent& Component
                : Prepared->Installation.CompositeReceipt.Components)
        {
            if (Component.Kind == SkiPreparation::CompositeInstallComponentKind::SiteContext)
            {
                SiteContextComponent = &Component;
                break;
            }
        }
        if (!SiteContextComponent)
        {
            Prepared->Error = TEXT("Verified schema-3 installation is missing its SiteContext component.");
            return Prepared;
        }

        const FString SiteContextId = UTF8_TO_TCHAR(SiteContextComponent->ContentId.c_str());
        SkiPreparation::SiteContextStore SiteContextStore(Root);
        // InstalledTerrainStore::Open already verified the SiteContext bytes and the
        // manifest hash from the composite receipt. Reopen only to retain its metadata.
        if (!SiteContextStore.Open(SiteContextId, Prepared->Core.Manifest,
                Prepared->SiteContext, Prepared->Error)) return Prepared;
        if (UTF8_TO_TCHAR(Prepared->SiteContext.Manifest.ContentId.c_str()) != SiteContextId)
        {
            Prepared->Error = TEXT("Verified schema-3 SiteContext identity changed before presentation.");
            return Prepared;
        }
        Prepared->bHasVerifiedSiteContext = true;
    }

    auto Repository = OpenRuntimeTerrainCoreRepository(Prepared->CoreStore,
        Prepared->Core, Prepared->Error);
    if (!Repository) return Prepared;
    Prepared->PresentedRepository = Repository;
    if (!EditSetId.IsEmpty())
    {
        SkiDomain::TerrainEditSet Edits;
        if (!IsContentId(EditSetId)
            || !Prepared->CoreStore->LoadEditSet(
                TerrainCoreId,
                EditSetId, Prepared->Core.Manifest.Width, Prepared->Core.Manifest.Height,
                Edits, Prepared->Error)) return Prepared;
        std::string EditError;
        auto Edited = SkiApplication::TerrainCoreEditedRepository::Create(
            Repository, Edits, Edits.BaseRevision, EditError);
        if (!Edited)
        {
            Prepared->Error = UTF8_TO_TCHAR(EditError.c_str());
            return Prepared;
        }
        Prepared->PresentedRepository = Edited;
        Prepared->Revision = Edits.EditRevision;
    }
    Prepared->bReady = true;
    return Prepared;
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

FString ComposeInstalledTerrainDetails(const FString& ExistingDetails,
    const std::uint32_t InstallationSchema,
    const SkiPreparation::SiteContextManifest& SiteContext,
    const SkiDomain::TerrainQualityReport& Quality)
{
    if (InstallationSchema != SkiPreparation::CompositeInstallReceiptSchema)
        return ExistingDetails;

    const auto FormatFraction = [](const double Fraction)
    {
        if (!FMath::IsFinite(Fraction) || Fraction < 0.0 || Fraction > 1.0)
            return FString(TEXT("unknown"));
        return FString::Printf(TEXT("%.1f%%"), Fraction * 100.0);
    };
    const auto GradeName = [](const SkiDomain::TerrainGrade Grade)
    {
        switch (Grade)
        {
        case SkiDomain::TerrainGrade::A: return TEXT("A");
        case SkiDomain::TerrainGrade::B: return TEXT("B");
        case SkiDomain::TerrainGrade::C: return TEXT("C");
        case SkiDomain::TerrainGrade::D: return TEXT("D");
        default: return TEXT("unknown");
        }
    };

    FString AttributionSummary;
    for (const SkiPreparation::SiteContextAttribution& Attribution : SiteContext.Attributions)
    {
        if (!AttributionSummary.IsEmpty()) AttributionSummary += TEXT("; ");
        AttributionSummary += FString::Printf(TEXT("%s — %s (%s)"),
            UTF8_TO_TCHAR(Attribution.Provider.c_str()),
            UTF8_TO_TCHAR(Attribution.Text.c_str()),
            UTF8_TO_TCHAR(Attribution.License.c_str()));
    }
    if (AttributionSummary.IsEmpty()) AttributionSummary = TEXT("not recorded");

    uint64 VectorAssetBytes = 0;
    bool bFoundVectorAsset = false;
    for (const SkiPreparation::SiteContextAsset& Asset : SiteContext.Assets)
    {
        if (Asset.Path == SiteContext.VectorAssetPath)
        {
            VectorAssetBytes = Asset.Length;
            bFoundVectorAsset = true;
            break;
        }
    }
    const FString VectorAssetSummary = bFoundVectorAsset
        ? FString::Printf(TEXT("%s · %llu bytes"),
            UTF8_TO_TCHAR(SiteContext.VectorAssetPath.c_str()),
            static_cast<unsigned long long>(VectorAssetBytes))
        : FString::Printf(TEXT("%s · size unavailable"),
            UTF8_TO_TCHAR(SiteContext.VectorAssetPath.c_str()));

    const SkiPreparation::SiteContextVectorSourceLineage& Lineage = SiteContext.VectorSource;
    const FString LineageSummary = SiteContext.SchemaVersion >= SkiPreparation::SiteContextSchema
        ? FString::Printf(TEXT("%s · %s · source %s · retrieved %s · %s"),
            UTF8_TO_TCHAR(Lineage.Provider.c_str()), UTF8_TO_TCHAR(Lineage.Endpoint.c_str()),
            UTF8_TO_TCHAR(Lineage.SourceTimestampUtc.c_str()),
            UTF8_TO_TCHAR(Lineage.RetrievedAtUtc.c_str()),
            UTF8_TO_TCHAR(Lineage.License.c_str()))
        : TEXT("not recorded in legacy SiteContext schema 1");

    const FString SourceMixSummary = Quality.SourceMix.Unknown
        ? TEXT("unknown")
        : FString::Printf(TEXT("S1M %s / Project 1 m %s / 1/3 arc-second %s%s"),
            *FormatFraction(Quality.SourceMix.S1M),
            *FormatFraction(Quality.SourceMix.Project1m),
            *FormatFraction(Quality.SourceMix.ArcSec13),
            Quality.SourceMix.Estimated ? TEXT(" (estimated)") : TEXT(""));

    const FString SiteContextSummary = FString::Printf(
        TEXT("Site context metadata\n")
        TEXT("Imagery: %llu verified tiles\n")
        TEXT("Attribution: %s\n")
        TEXT("OSM lineage: %s\n")
        TEXT("OSM feature asset: %s\n")
        TEXT("Terrain quality: Grade %s · source mix %s · unknown metadata %s · NoData %s\n")
        TEXT("Photo presentation: verified tiles load on demand for the current terrain view.\n")
        TEXT("Vector presentation gap: installed OSM features are not yet decoded or rendered."),
        static_cast<unsigned long long>(SiteContext.ImageryTiles.size()), *AttributionSummary,
        *LineageSummary, *VectorAssetSummary, GradeName(Quality.Grade), *SourceMixSummary,
        *FormatFraction(Quality.UnknownMetadataFraction), *FormatFraction(Quality.NoDataFraction));
    return ExistingDetails + TEXT("\n\n") + SiteContextSummary;
}

bool ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(
    const FString& ContentId, const SkiPreparation::InstalledTerrainIndex& Index,
    const bool bStoreOpenVerified, FString& OutTerrainCoreId,
    FString& OutCoverEcologyId)
{
    OutTerrainCoreId.Reset();
    OutCoverEcologyId.Reset();
    if (!bStoreOpenVerified || !IsContentId(ContentId)) return false;

    const std::string ExpectedId(TCHAR_TO_UTF8(*ContentId));
    if (Index.SchemaVersion == SkiDomain::InstalledTerrainSchema
        && Index.Receipt.SchemaVersion == SkiDomain::InstalledTerrainSchema
        && Index.Receipt.ContentId == ExpectedId
        && SkiDomain::ValidateInstalledTerrainReceipt(Index.Receipt).Ok())
    {
        const FString TerrainCoreId = UTF8_TO_TCHAR(Index.Receipt.TerrainCoreId.c_str());
        const FString CoverEcologyId = UTF8_TO_TCHAR(Index.Receipt.CoverEcologyId.c_str());
        if (!IsContentId(TerrainCoreId) || !IsContentId(CoverEcologyId)) return false;
        OutTerrainCoreId = TerrainCoreId;
        OutCoverEcologyId = CoverEcologyId;
        return true;
    }

    const SkiPreparation::CompositeInstallReceipt& Receipt = Index.CompositeReceipt;
    if (Index.SchemaVersion != SkiPreparation::CompositeInstallReceiptSchema
        || Receipt.SchemaVersion != SkiPreparation::CompositeInstallReceiptSchema
        || Receipt.ContentId != ExpectedId
        || !SkiPreparation::ValidateCompositeInstallReceipt(Receipt).Ok()) return false;

    const SkiPreparation::CompositeInstallComponent* TerrainCore = nullptr;
    const SkiPreparation::CompositeInstallComponent* CoverEcology = nullptr;
    for (const SkiPreparation::CompositeInstallComponent& Component : Receipt.Components)
    {
        if (Component.Kind == SkiPreparation::CompositeInstallComponentKind::TerrainCore)
            TerrainCore = &Component;
        else if (Component.Kind == SkiPreparation::CompositeInstallComponentKind::CoverEcology)
            CoverEcology = &Component;
    }
    if (!TerrainCore || !CoverEcology) return false;

    const FString TerrainCoreId = UTF8_TO_TCHAR(TerrainCore->ContentId.c_str());
    const FString CoverEcologyId = UTF8_TO_TCHAR(CoverEcology->ContentId.c_str());
    if (!IsContentId(TerrainCoreId) || !IsContentId(CoverEcologyId)) return false;
    OutTerrainCoreId = TerrainCoreId;
    OutCoverEcologyId = CoverEcologyId;
    return true;
}

ASkiBootstrapGameMode::ASkiBootstrapGameMode()
{
    PlayerControllerClass = ASkiTerrainViewController::StaticClass();
}

bool ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(const float ScrollOffset,
    const float ScrollMaximum, const float Tolerance) noexcept
{
    if (!FMath::IsFinite(ScrollOffset) || !FMath::IsFinite(ScrollMaximum)
        || !FMath::IsFinite(Tolerance) || ScrollMaximum < 0.0F || Tolerance < 0.0F
        || ScrollOffset < -Tolerance || ScrollOffset > ScrollMaximum + Tolerance)
        return false;
    return ScrollMaximum <= Tolerance || ScrollOffset >= ScrollMaximum - Tolerance;
}

void ASkiBootstrapGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ClearInstalledPhotoContext();
    Super::EndPlay(EndPlayReason);
}

void ASkiBootstrapGameMode::BeginPlay()
{
    Super::BeginPlay();
    SkiPreparation::InitializePreparationDiagnostics(FPaths::ProjectSavedDir());
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    const FString CurrentMap = GetWorld()->GetOutermost()->GetName();
    const bool bMountainMap = CurrentMap == TEXT("/Game/P1Generated/P1Terrain");
    const bool ExpectedMap = CurrentMap == TEXT("/Game/P0Generated/Bootstrap") || bMountainMap;

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
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiM0TerrainCoreScale")))
    {
        FString ReceiptPath, Token, Report, Error, PackageRoot, ContentId;
        int32 Side = 0;
        FGuid ParsedToken;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0Side="), Side)
            && FGuid::Parse(Token, ParsedToken);
        const bool bKeepPackage = FParse::Param(FCommandLine::Get(), TEXT("SkiM0KeepPackage"));
        const bool Passed = ArgumentsValid && (bKeepPackage
            ? SkiPreparation::RunM0TerrainCoreScale(static_cast<uint32>(Side), Report, Error,
                PackageRoot, ContentId)
            : SkiPreparation::RunM0TerrainCoreScale(static_cast<uint32>(Side), Report, Error));
        Report.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Report.ReplaceInline(TEXT("\""), TEXT("\\\""));
        Error.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        Error.ReplaceInline(TEXT("\""), TEXT("\\\""));
        PackageRoot.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
        PackageRoot.ReplaceInline(TEXT("\""), TEXT("\\\""));
        const FString Receipt = FString::Printf(
            TEXT("{\"token\":\"%s\",\"scenario\":\"m0-terraincore-scale\",\"side\":%d,\"passed\":%s,\"report\":\"%s\",\"error\":\"%s\",\"packageRoot\":\"%s\",\"contentId\":\"%s\"}"),
            *Token, Side, Passed ? TEXT("true") : TEXT("false"), *Report, *Error,
            *PackageRoot, *ContentId);
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt,
            *ReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiM0TerrainCoreRender")))
    {
        FString Root, ContentId, ReceiptPath, Token;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0RenderRoot="), Root)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0ContentId="), ContentId)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token);
        if (!ArgumentsValid || !StartM0TerrainRenderProbe(GetWorld(), Root, ContentId,
                ReceiptPath, Token))
            FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiM0MapSpike")))
    {
        FString ReceiptPath, Token;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0MapReceipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token);
        if (!ArgumentsValid || !StartSkiSiteMapSpike(GetWorld(), ReceiptPath, Token))
            FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiM0RasterProjection")))
    {
        FString ReceiptPath, Token, CogPath, GpkgPath;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0Cog="), CogPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0GeoPackage="), GpkgPath);
        SkiPreparation::FM0RasterProjectionReceipt Probe;
        if (ArgumentsValid)
        {
            Probe = SkiPreparation::RunM0RasterProjectionProbe(CogPath, GpkgPath);
            ProbeM0ShippingProjection(*GetWorld(), Probe);
        }
        const bool Passed = ArgumentsValid && Probe.bCogPassed
            && Probe.bGeoPackagePassed
            && (Probe.bShippingProjPassed || Probe.bFallbackProjectionPassed);
        const FString Receipt = FString::Printf(TEXT("{\"token\":\"%s\",\"scenario\":\"m0-raster-projection\",\"passed\":%s,\"probe\":%s}"),
            *Token, Passed ? TEXT("true") : TEXT("false"), *Probe.ToJson());
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt,
            *ReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
        return;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiM0RealS1M")))
    {
        FString ReceiptPath, Token, CogUrl;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0CogUrl="), CogUrl);
        const auto Probe = ArgumentsValid
            ? SkiPreparation::RunM0RealS1MCogProbe(CogUrl)
            : SkiPreparation::FM0RasterProjectionReceipt{};
        const bool Passed = ArgumentsValid && Probe.bCogPassed
            && Probe.bRealS1MProbe && Probe.bBaseTileDecoded && Probe.bOverviewTileDecoded;
        const FString Receipt = FString::Printf(
            TEXT("{\"token\":\"%s\",\"scenario\":\"m0-real-s1m\",\"passed\":%s,\"probe\":%s}"),
            *Token, Passed ? TEXT("true") : TEXT("false"), *Probe.ToJson());
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt,
            *ReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
        return;
    }
#if !UE_BUILD_SHIPPING
    FString M0Scenario;
    if (FParse::Value(FCommandLine::Get(), TEXT("SkiP1Scenario="), M0Scenario)
        && M0Scenario == TEXT("gateway-redirect"))
    {
        FString ReceiptPath, Url, CaPath;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestUrl="), Url)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestCA="), CaPath);
        const auto Result = ArgumentsValid
            ? SkiPreparation::SkiNetGateway::RunTestRedirectProbe(Url, CaPath)
            : SkiPreparation::HttpAcquisitionResult{};
        const bool Passed = ArgumentsValid && Result.RequestStatus == TEXT("REDIRECT_DENIED")
            && Result.HttpStatus >= 300 && Result.HttpStatus < 400;
        const FString Receipt = FString::Printf(
            TEXT("{\"scenario\":\"gateway-redirect\",\"requestStatus\":\"%s\",\"httpStatus\":%d,\"passed\":%s}"),
            *Result.RequestStatus, Result.HttpStatus, Passed ? TEXT("true") : TEXT("false"));
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt,
            *ReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
        return;
    }
    if (M0Scenario == TEXT("gateway-range"))
    {
        FString ReceiptPath, Url, CaPath;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestUrl="), Url)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestCA="), CaPath);
        const auto Result = ArgumentsValid
            ? SkiPreparation::SkiNetGateway::RunTestRangeProbe(Url, CaPath, 4, 4)
            : SkiPreparation::HttpAcquisitionResult{};
        FString Hex;
        for (uint8 Byte : Result.Bytes) Hex += FString::Printf(TEXT("%02x"), Byte);
        const bool bInvalidRangeCase = Url.EndsWith(TEXT("/range/bad-content-range"));
        const bool Passed = ArgumentsValid && (bInvalidRangeCase
            ? Result.RequestStatus == TEXT("InvalidContentRange")
            : Result.Ok() && Result.HttpStatus == 206 && Hex == TEXT("08000000"));
        const FString Receipt = FString::Printf(
            TEXT("{\"scenario\":\"gateway-range\",\"httpStatus\":%d,\"bytesHex\":\"%s\",\"requestStatus\":\"%s\",\"passed\":%s}"),
            Result.HttpStatus, *Hex, *Result.RequestStatus, Passed ? TEXT("true") : TEXT("false"));
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt,
            *ReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
        return;
    }
    if (M0Scenario == TEXT("gateway-cog"))
    {
        FString ReceiptPath, Url, CaPath, GpkgPath;
        const bool ArgumentsValid = ExpectedMap
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestUrl="), Url)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiGatewayTestCA="), CaPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiM0GeoPackage="), GpkgPath);
        SkiPreparation::FM0RasterProjectionReceipt Probe;
        if (ArgumentsValid)
        {
            Probe = SkiPreparation::RunM0GatewayRasterProbe(Url, CaPath, GpkgPath);
            ProbeM0ShippingProjection(*GetWorld(), Probe);
        }
        const bool Passed = ArgumentsValid && Probe.bCogPassed && Probe.bGeoPackagePassed
            && (Probe.bShippingProjPassed || Probe.bFallbackProjectionPassed)
            && Probe.GatewayRangeRequests > 1;
        const FString Receipt = FString::Printf(
            TEXT("{\"scenario\":\"gateway-cog\",\"passed\":%s,\"probe\":%s}"),
            Passed ? TEXT("true") : TEXT("false"), *Probe.ToJson());
        const bool Written = ArgumentsValid && FFileHelper::SaveStringToFile(Receipt,
            *ReceiptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FPlatformMisc::RequestExitWithStatus(false, Passed && Written ? 0 : 1);
        return;
    }
#endif
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1UiLayoutSmoke")))
    {
        if (!BeginP1UiLayoutSmoke()) FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("SkiP1PickerViewportSmoke")))
    {
        if (!BeginP1PickerViewportSmoke()) FPlatformMisc::RequestExitWithStatus(false, 1);
        return;
    }
#endif
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
    if (!bMountainMap && FParse::Param(FCommandLine::Get(), TEXT("SkiM1FrontEndSmoke")))
    {
        FString DataRoot, ReceiptPath, Token;
        FGuid ParsedToken;
        const bool ArgumentsValid = FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), ReceiptPath)
            && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token)
            && FGuid::Parse(Token, ParsedToken);
        DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
        ReceiptPath = FPaths::ConvertRelativePathToFull(ReceiptPath);
        FPaths::NormalizeFilename(DataRoot);
        FPaths::NormalizeFilename(ReceiptPath);
        const bool CanonicalPaths = FPaths::CollapseRelativeDirectories(DataRoot)
            && FPaths::CollapseRelativeDirectories(ReceiptPath);
        if (!DataRoot.EndsWith(TEXT("/"))) DataRoot += TEXT("/");
        const bool PathValid = ArgumentsValid && CanonicalPaths
            && ReceiptPath.StartsWith(DataRoot, ESearchCase::IgnoreCase)
            && FPaths::GetCleanFilename(ReceiptPath) == Token + TEXT(".receipt.json");
        if (!PathValid)
        {
            FPlatformMisc::RequestExitWithStatus(false, 1);
            return;
        }
        P1Widget->AddToViewport();
        P1Widget->OpenSelector();
        SkiPreparation::Request FixtureRequest;
        FixtureRequest.Name = TEXT("Verified fixture resort");
        FixtureRequest.Bounds = {-121.56, 46.95, -121.53, 46.97};
        FixtureRequest.SessionGeneration = 91;
        FixtureRequest.OperationGeneration = 1;
        FixtureRequest.Lease = MakeShared<SkiPreparation::PreparationOperationLease,
            ESPMode::ThreadSafe>(91, 1);
        const TSharedRef<SkiPreparation::Cancellation> FixtureCancellation =
            MakeShared<SkiPreparation::Cancellation>();
        const SkiPreparation::Result FixtureResult =
            SkiPreparation::FixtureTerrainProvider(DataRoot).Prepare(
                FixtureRequest, FixtureCancellation, {});
        SkiPreparation::InstalledTerrainStore Store(DataRoot);
        TArray<SkiPreparation::InstalledTerrainLibraryEntry> Verified;
        FString Error;
        const bool bListed = FixtureResult.Ok && Store.ListVerified(Verified, Error)
            && Verified.Num() == 1;
        FString InstalledId;
        if (bListed)
        {
            InstalledId = Verified[0].ContentId;
            FSkiInstalledResortItem Fixture;
            Fixture.ContentId = InstalledId;
            Fixture.DisplayName = FixtureRequest.Name;
            Fixture.Detail = Verified[0].SourceId;
            P1Widget->SetInstalledResorts({Fixture});
        }
        InstalledOpenDataRootOverride = DataRoot;
        P1Widget->SetOpenInstalledByIdHandler([this](const FString& ContentId)
        {
            TransitionToInstalledTerrain(ContentId);
        });
        const bool Passed = bListed && !P1Widget->HasLiveSelector()
            && P1Widget->RunNativeFrontEndSmoke(InstalledId, Error, true);
        if (!Passed || !P1Widget->IsNativeTitleReady())
        {
            FPlatformMisc::RequestExitWithStatus(false, 1);
            return;
        }
        return;
    }
    P1Widget->SetSelectionHandler([this](const SkiPreparation::Request& Request) { BeginP1Preparation(Request); });
    P1Widget->SetOpenInstalledHandler([this] { OpenLatestInstalledTerrain(); });
    P1Widget->SetOpenInstalledByIdHandler([this](const FString& ContentId)
    {
        TransitionToInstalledTerrain(ContentId);
    });
    P1Widget->SetNavigationHandler([this]
    {
        ++InstalledOpenGeneration;
        PendingInstalledOpenId.Empty();
        DeferredInstalledOpenId.Empty();
        if (P1Widget) P1Widget->SetSelectorStatus(TEXT("Choose an installed resort, or start a new resort."));
    });
    const TSharedRef<SkiPreparation::IAcquisitionTransport, ESPMode::ThreadSafe>
        PlaceSearchTransport = MakeShared<SkiPreparation::SkiNetGateway, ESPMode::ThreadSafe>();
    const TSharedRef<SkiPreparation::IPlaceSearchProvider, ESPMode::ThreadSafe>
        PlaceSearchProvider = MakeShared<SkiPreparation::NominatimSearchProvider,
            ESPMode::ThreadSafe>(PlaceSearchTransport);
    P1Widget->SetPlaceSearchHandler([PlaceSearchProvider](const FString& Query,
        const TSharedRef<SkiPreparation::Cancellation>& Cancellation,
        FSkiPlaceSearchCompletion Completion)
    {
        Async(EAsyncExecution::ThreadPool,
            [PlaceSearchProvider, Query, Cancellation, Completion = MoveTemp(Completion)]() mutable
        {
            TArray<SkiPreparation::PlaceSearchResult> ProviderResults;
            FString Error;
            const bool bSearchSucceeded = PlaceSearchProvider->Search(Query,
                Cancellation, ProviderResults, Error);
            if (Cancellation->IsCancelled()) return;
            if (!bSearchSucceeded && Error.IsEmpty()) Error = TEXT("PLACE_SEARCH_FAILED");

            TArray<FSkiPlaceSearchResult> PresentationResults;
            PresentationResults.Reserve(ProviderResults.Num());
            for (const SkiPreparation::PlaceSearchResult& ProviderResult : ProviderResults)
            {
                FSkiPlaceSearchResult& PresentationResult =
                    PresentationResults.AddDefaulted_GetRef();
                PresentationResult.Name = ProviderResult.Name;
                PresentationResult.Region = ProviderResult.Region.IsEmpty()
                    ? ProviderResult.Country : ProviderResult.Region;
                PresentationResult.LatitudeDeg = ProviderResult.Point.LatitudeDeg;
                PresentationResult.LongitudeDeg = ProviderResult.Point.LongitudeDeg;
                PresentationResult.BoundingBox = ProviderResult.BoundingBox;
            }
            Completion(MoveTemp(PresentationResults), MoveTemp(Error));
        });
    });
    P1Widget->AddToViewport();
    Controller->bShowMouseCursor = true;
    Controller->SetInputMode(FInputModeUIOnly());
    if (bMountainMap)
    {
        USkiFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<USkiFlowSubsystem>();
        const FString ContentId = Flow ? Flow->ConsumeInstalledResort() : FString();
        const FString FlowRoot = Flow ? Flow->ConsumeInstalledDataRoot() : FString();
        MountainAcquisitionDeny = MakeUnique<SkiPreparation::ScopedAcquisitionPortDeny>();
        if (!IsContentId(ContentId) || !MountainAcquisitionDeny->IsActive())
        {
            if (Flow) Flow->SetReturnError(TEXT("The selected resort could not be opened offline."));
            UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/P0Generated/Bootstrap")));
            return;
        }
        P1Widget->BeginPreparationUI([this] { ChangeSelection(); });
        P1Widget->SetTransientStatus(TEXT("Opening and verifying the installed resort offline…"));
        FString EditSetId;
        FParse::Value(FCommandLine::Get(), TEXT("SkiP1EditSetId="), EditSetId);
        const FString Root = FlowRoot.IsEmpty() ? FPaths::ProjectSavedDir() : FlowRoot;
        const TWeakObjectPtr<ASkiBootstrapGameMode> WeakThis(this);
        const uint64 PrepareGeneration = ++MountainPrepareGeneration;
        Async(EAsyncExecution::ThreadPool, [WeakThis, Root, ContentId, EditSetId,
            PrepareGeneration]()
        {
            auto Prepared = PrepareInstalledTerrain(Root, ContentId, EditSetId);
            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, ContentId, PrepareGeneration, Prepared = MoveTemp(Prepared)]()
            {
                if (!WeakThis.IsValid()
                    || WeakThis->MountainPrepareGeneration != PrepareGeneration
                    || !WeakThis->GetWorld()
                    || WeakThis->GetWorld()->GetOutermost()->GetName()
                        != TEXT("/Game/P1Generated/P1Terrain")) return;
                if (WeakThis->OpenInstalledTerrain(ContentId, Prepared)) return;
                if (USkiFlowSubsystem* ReturnFlow = WeakThis->GetGameInstance()
                    ->GetSubsystem<USkiFlowSubsystem>())
                    ReturnFlow->SetReturnError(TEXT("The selected resort could not be opened offline."));
                UGameplayStatics::OpenLevel(WeakThis.Get(), FName(TEXT("/Game/P0Generated/Bootstrap")));
            });
        });
        return;
    }
    if (USkiFlowSubsystem* Flow = GetGameInstance()->GetSubsystem<USkiFlowSubsystem>())
        ReturnErrorNotice = Flow->ConsumeReturnError();
    RefreshInstalledLibrary();
    FString InstalledChoice;
    if (ReturnErrorNotice.IsEmpty()
        && FParse::Value(FCommandLine::Get(), TEXT("SkiP1OpenInstalled="), InstalledChoice))
    {
        if (InstalledChoice.Equals(TEXT("latest"), ESearchCase::IgnoreCase))
            OpenLatestInstalledTerrain();
        else if (IsContentId(InstalledChoice)) TransitionToInstalledTerrain(InstalledChoice);
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

#if !UE_BUILD_SHIPPING
namespace
{
FVector4 PickerViewportRect(const UWidget* Widget, const FVector2D& RootPosition)
{
    if (!Widget) return FVector4(0, 0, 0, 0);
    const FGeometry Geometry = Widget->GetCachedGeometry();
    const FVector2D Position = Geometry.GetAbsolutePosition() - RootPosition;
    const FVector2D Size = Geometry.GetAbsoluteSize();
    return FVector4(Position.X, Position.Y, Size.X, Size.Y);
}

bool PickerViewportRectValid(const FVector4& Rect)
{
    return FMath::IsFinite(Rect.X) && FMath::IsFinite(Rect.Y)
        && FMath::IsFinite(Rect.Z) && FMath::IsFinite(Rect.W)
        && Rect.Z > 0.0 && Rect.W > 0.0;
}

bool PickerViewportRectInside(const FVector4& Inner, const FVector4& Outer,
    const double Tolerance = 1.0)
{
    return PickerViewportRectValid(Inner) && PickerViewportRectValid(Outer)
        && Inner.X >= Outer.X - Tolerance && Inner.Y >= Outer.Y - Tolerance
        && Inner.X + Inner.Z <= Outer.X + Outer.Z + Tolerance
        && Inner.Y + Inner.W <= Outer.Y + Outer.W + Tolerance;
}

FString PickerViewportRectJson(const FVector4& Rect)
{
    return FString::Printf(TEXT("[%.1f,%.1f,%.1f,%.1f]"), Rect.X, Rect.Y, Rect.Z, Rect.W);
}

FString EscapePickerViewportJsonString(const FString& Value)
{
    FString Escaped = Value;
    return Escaped.ReplaceCharWithEscapedChar();
}

void FailPickerViewportSmoke(const TCHAR* Message)
{
    UE_LOG(LogTemp, Error, TEXT("P1 picker viewport smoke failed: %s"), Message);
    FPlatformMisc::RequestExitWithStatus(false, 1);
}
}

bool ASkiBootstrapGameMode::BeginP1PickerViewportSmoke()
{
    FString DataRoot;
    FGuid ParsedToken;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Receipt="), PickerViewportReceiptPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Screenshot="), PickerViewportScreenshotPath)
        || !FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), PickerViewportToken)
        || !FGuid::Parse(PickerViewportToken, ParsedToken)) return false;

    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
    PickerViewportReceiptPath = FPaths::ConvertRelativePathToFull(PickerViewportReceiptPath);
    PickerViewportScreenshotPath = FPaths::ConvertRelativePathToFull(PickerViewportScreenshotPath);
    FPaths::NormalizeFilename(DataRoot);
    FPaths::NormalizeFilename(PickerViewportReceiptPath);
    FPaths::NormalizeFilename(PickerViewportScreenshotPath);
    if (!FPaths::CollapseRelativeDirectories(DataRoot)
        || !FPaths::CollapseRelativeDirectories(PickerViewportReceiptPath)
        || !FPaths::CollapseRelativeDirectories(PickerViewportScreenshotPath)) return false;
    FString Prefix = DataRoot;
    Prefix.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!Prefix.EndsWith(TEXT("/"))) Prefix += TEXT("/");
    FString ReceiptPath = PickerViewportReceiptPath;
    ReceiptPath.ReplaceInline(TEXT("\\"), TEXT("/"));
    FString ScreenshotPath = PickerViewportScreenshotPath;
    ScreenshotPath.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!ReceiptPath.StartsWith(Prefix, ESearchCase::IgnoreCase)
        || !ScreenshotPath.StartsWith(Prefix, ESearchCase::IgnoreCase)
        || FPaths::GetCleanFilename(PickerViewportReceiptPath)
            != PickerViewportToken + TEXT(".receipt.json")
        || FPaths::GetCleanFilename(PickerViewportScreenshotPath)
            != PickerViewportToken + TEXT(".png")) return false;

    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    P1Widget = Controller ? CreateWidget<USkiP1Widget>(Controller, USkiP1Widget::StaticClass()) : nullptr;
    if (!P1Widget || !P1Widget->IsP1Ready()) return false;
    P1Widget->OpenSelector();
    P1Widget->AddToViewport();

    USkiSiteMapWidget* MapWidget = Cast<USkiSiteMapWidget>(
        P1Widget->GetWidgetFromName(TEXT("SiteMap")));
    UButton* NewResortButton = Cast<UButton>(
        P1Widget->GetWidgetFromName(TEXT("NewResort")));
    if (!MapWidget || !NewResortButton) return false;

    // The screenshot contains the real map widget and native picker UI, but the
    // viewport run is hermetic: no tile or place-search request may escape.
    MapWidget->SetNetworkEnabled(false);
    P1Widget->ForceLayoutPrepass();
    NewResortButton->OnClicked.Broadcast();
    if (!P1Widget->IsNativeSitePickerReady()) return false;

    FTimerHandle LayoutTimer;
    GetWorldTimerManager().SetTimer(LayoutTimer, this,
        &ASkiBootstrapGameMode::InspectP1PickerViewportTop, 0.25F, false);
    return true;
}

void ASkiBootstrapGameMode::InspectP1PickerViewportTop()
{
    if (!P1Widget) { FailPickerViewportSmoke(TEXT("picker widget was destroyed")); return; }
    P1Widget->ForceLayoutPrepass();
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    if (!Controller) { FailPickerViewportSmoke(TEXT("player controller is missing")); return; }
    Controller->GetViewportSize(PickerViewportWidth, PickerViewportHeight);
    const bool ExpectedResolution =
        (PickerViewportWidth == 1280 && PickerViewportHeight == 720)
        || (PickerViewportWidth == 1920 && PickerViewportHeight == 1080)
        || (PickerViewportWidth == 2560 && PickerViewportHeight == 1080)
        || (PickerViewportWidth == 2560 && PickerViewportHeight == 1440)
        || (PickerViewportWidth == 576 && PickerViewportHeight == 1024);
    if (!ExpectedResolution)
    {
        FailPickerViewportSmoke(TEXT("viewport does not match the requested M2 test matrix"));
        return;
    }

    const FVector2D RootPosition = P1Widget->GetCachedGeometry().GetAbsolutePosition();
    const FVector2D RootSize = P1Widget->GetCachedGeometry().GetAbsoluteSize();
    const FVector4 ViewportRect(0, 0, PickerViewportWidth, PickerViewportHeight);
    UWidget* MapWidget = P1Widget->GetWidgetFromName(TEXT("SiteMap"));
    UWidget* PanelWidget = P1Widget->GetWidgetFromName(TEXT("SelectorPanel"));
    UWidget* ScrollWidget = P1Widget->GetWidgetFromName(TEXT("PickerScroll"));
    UWidget* HeadingWidget = P1Widget->GetWidgetFromName(TEXT("PickerHeading"));
    UWidget* SubtitleWidget = P1Widget->GetWidgetFromName(TEXT("PickerSubtitle"));
    UWidget* StepsWidget = P1Widget->GetWidgetFromName(TEXT("PickerSteps"));
    UWidget* LocationControlsWidget = P1Widget->GetWidgetFromName(TEXT("LocationControls"));
    UWidget* LocationHeadingWidget = P1Widget->GetWidgetFromName(TEXT("LocationHeading"));
    UWidget* LocationSearchWidget = P1Widget->GetWidgetFromName(TEXT("LocationSearchBox"));
    UWidget* SearchButtonWidget = P1Widget->GetWidgetFromName(TEXT("SearchLocation"));
    UWidget* SelectSiteButtonWidget = P1Widget->GetWidgetFromName(TEXT("SelectSite"));
    UWidget* SearchStatusWidget = P1Widget->GetWidgetFromName(TEXT("PickerSearchStatus"));
    UWidget* BoundaryControlsWidget = P1Widget->GetWidgetFromName(TEXT("BoundaryControls"));
    UWidget* NameControlsWidget = P1Widget->GetWidgetFromName(TEXT("ResortNameControls"));
    UScrollBox* PickerScroll = Cast<UScrollBox>(ScrollWidget);
    const FVector4 PanelRect = PickerViewportRect(PanelWidget, RootPosition);
    const FVector4 MapRect = PickerViewportRect(MapWidget, RootPosition);
    const FVector4 ScrollRect = PickerViewportRect(ScrollWidget, RootPosition);
    const FVector4 HeadingRect = PickerViewportRect(HeadingWidget, RootPosition);
    const FVector4 SubtitleRect = PickerViewportRect(SubtitleWidget, RootPosition);
    const FVector4 StepsRect = PickerViewportRect(StepsWidget, RootPosition);
    const FVector4 LocationControlsRect = PickerViewportRect(LocationControlsWidget, RootPosition);
    const FVector4 LocationHeadingRect = PickerViewportRect(LocationHeadingWidget, RootPosition);
    const FVector4 LocationSearchRect = PickerViewportRect(LocationSearchWidget, RootPosition);
    const FVector4 SearchButtonRect = PickerViewportRect(SearchButtonWidget, RootPosition);
    const FVector4 SelectSiteRect = PickerViewportRect(SelectSiteButtonWidget, RootPosition);
    UTextBlock* Heading = Cast<UTextBlock>(HeadingWidget);
    UTextBlock* Subtitle = Cast<UTextBlock>(SubtitleWidget);
    UTextBlock* LocationHeading = Cast<UTextBlock>(LocationHeadingWidget);
    UEditableTextBox* LocationSearch = Cast<UEditableTextBox>(LocationSearchWidget);
    UButton* SearchButton = Cast<UButton>(SearchButtonWidget);
    UButton* SelectSiteButton = Cast<UButton>(SelectSiteButtonWidget);
    UTextBlock* SearchStatus = Cast<UTextBlock>(SearchStatusWidget);
    UTextBlock* SearchButtonLabel = SearchButton
        ? Cast<UTextBlock>(SearchButton->GetChildAt(0)) : nullptr;
    UTextBlock* SelectSiteLabel = SelectSiteButton
        ? Cast<UTextBlock>(SelectSiteButton->GetChildAt(0)) : nullptr;
    UVerticalBox* Steps = Cast<UVerticalBox>(StepsWidget);
    if (!PickerScroll || !Heading || !Subtitle || !LocationHeading || !LocationSearch
        || !SearchButtonLabel || !SelectSiteButton || !SelectSiteLabel || !SearchStatus
        || !Steps || !LocationControlsWidget || !BoundaryControlsWidget || !NameControlsWidget)
    {
        FailPickerViewportSmoke(TEXT("required picker content widgets are missing"));
        return;
    }

    TArray<FVector4> StepRects;
    TArray<FString> StepTexts;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        UTextBlock* Step = Index < Steps->GetChildrenCount()
            ? Cast<UTextBlock>(Steps->GetChildAt(Index)) : nullptr;
        if (!Step)
        {
            FailPickerViewportSmoke(TEXT("one of the four picker steps is missing"));
            return;
        }
        StepRects.Add(PickerViewportRect(Step, RootPosition));
        StepTexts.Add(Step->GetText().ToString());
    }

    const FString HeadingText = Heading->GetText().ToString();
    const FString SubtitleText = Subtitle->GetText().ToString();
    const FString LocationHeadingText = LocationHeading->GetText().ToString();
    const FString SearchButtonText = SearchButtonLabel->GetText().ToString();
    const FString SelectSiteText = SelectSiteLabel->GetText().ToString();
    PickerViewportScrollRect = ScrollRect;
    PickerViewportScrollAtStart = PickerScroll->GetScrollOffset();

    const bool TopValid = P1Widget->IsNativeSitePickerReady()
        && MapWidget && MapWidget->GetVisibility() == ESlateVisibility::Visible
        && PanelWidget && ScrollWidget && HeadingWidget && SubtitleWidget && StepsWidget
        && LocationControlsWidget->GetVisibility() == ESlateVisibility::Visible
        && BoundaryControlsWidget->GetVisibility() == ESlateVisibility::Collapsed
        && NameControlsWidget->GetVisibility() == ESlateVisibility::Collapsed
        && !SelectSiteButton->GetIsEnabled()
        && LocationHeadingWidget && LocationSearchWidget && SearchButtonWidget
        && RootSize.X > 0.0 && RootSize.Y > 0.0
        && PickerViewportRectInside(PanelRect, ViewportRect)
        && PickerViewportRectInside(MapRect, ViewportRect)
        && FMath::Abs(MapRect.X) <= 1.0 && FMath::Abs(MapRect.Y) <= 1.0
        && FMath::Abs(MapRect.Z - PickerViewportWidth) <= 1.0
        && FMath::Abs(MapRect.W - PickerViewportHeight) <= 1.0
        && PickerViewportRectInside(HeadingRect, PanelRect)
        && PickerViewportRectInside(SubtitleRect, PanelRect)
        && PickerViewportRectInside(StepsRect, PanelRect)
        && PickerViewportRectInside(ScrollRect, PanelRect)
        && PickerViewportRectInside(LocationControlsRect, ScrollRect)
        && PickerViewportRectInside(LocationHeadingRect, ScrollRect)
        && PickerViewportRectInside(LocationSearchRect, ScrollRect)
        && PickerViewportRectInside(SearchButtonRect, ScrollRect)
        && PickerViewportRectInside(SelectSiteRect, ScrollRect)
        && PickerViewportScrollAtStart <= 1.0F
        && HeadingText.Contains(TEXT("New resort"))
        && SubtitleText == TEXT("Find the mountain you want to make your own.")
        && LocationHeadingText.Contains(TEXT("Search a place"))
        && SearchButtonText.Contains(TEXT("Search / go to coordinates"))
        && SelectSiteText.Contains(TEXT("Select site"))
        && StepTexts.Num() == 4
        && StepTexts[0].StartsWith(TEXT("●"))
        && StepTexts[1].StartsWith(TEXT("○"))
        && StepTexts[0].Contains(TEXT("Choose location"))
        && StepTexts[1].Contains(TEXT("Define boundary"))
        && StepTexts[2].Contains(TEXT("Name resort"))
        && StepTexts[3].Contains(TEXT("Download"));
    for (const FVector4& StepRect : StepRects)
    {
        if (!PickerViewportRectInside(StepRect, StepsRect))
        {
            FailPickerViewportSmoke(TEXT("picker step label leaves its visible stepper"));
            return;
        }
    }
    if (!TopValid)
    {
        FailPickerViewportSmoke(TEXT("picker Step 1 visibility, header, location controls, or scroll bounds are invalid"));
        return;
    }
    bPickerViewportTopValid = true;
    PickerViewportTopRectsJson = FString::Printf(
        TEXT("\"panel\":%s,\"map\":%s,\"scroll\":%s,\"heading\":%s,\"subtitle\":%s,\"steps\":%s"),
        *PickerViewportRectJson(PanelRect), *PickerViewportRectJson(MapRect),
        *PickerViewportRectJson(ScrollRect), *PickerViewportRectJson(HeadingRect),
        *PickerViewportRectJson(SubtitleRect), *PickerViewportRectJson(StepsRect));

    LocationSearch->SetText(FText::FromString(TEXT("47.25, -121.55")));
    SearchButton->OnClicked.Broadcast();
    const FString SearchStatusText = SearchStatus->GetText().ToString();
    if (!SelectSiteButton->GetIsEnabled() || !SearchStatusText.Contains(TEXT("Centered at"))
        || !SearchStatusText.Contains(TEXT("Select site")))
    {
        FailPickerViewportSmoke(TEXT("coordinate search did not enable the real Select site action"));
        return;
    }
    PickerViewportStep1EvidenceJson = FString::Printf(
        TEXT("\"step1\":{\"locationVisible\":true,\"boundaryVisible\":false,\"nameVisible\":false,\"selectSiteInitiallyDisabled\":true,\"selectSiteEnabledAfterSearch\":true,\"locationQuery\":\"47.25, -121.55\",\"activeLabel\":\"%s\",\"searchStatus\":\"%s\",\"texts\":{\"locationHeading\":\"%s\",\"searchButton\":\"%s\",\"selectSiteButton\":\"%s\"},\"rects\":{\"locationControls\":%s,\"locationHeading\":%s,\"locationSearch\":%s,\"searchButton\":%s,\"selectSiteButton\":%s}}"),
        *EscapePickerViewportJsonString(StepTexts[0]),
        *EscapePickerViewportJsonString(SearchStatusText),
        *EscapePickerViewportJsonString(LocationHeadingText),
        *EscapePickerViewportJsonString(SearchButtonText),
        *EscapePickerViewportJsonString(SelectSiteText),
        *PickerViewportRectJson(LocationControlsRect),
        *PickerViewportRectJson(LocationHeadingRect),
        *PickerViewportRectJson(LocationSearchRect), *PickerViewportRectJson(SearchButtonRect),
        *PickerViewportRectJson(SelectSiteRect));

    SelectSiteButton->OnClicked.Broadcast();
    P1Widget->ForceLayoutPrepass();
    FTimerHandle BoundaryTimer;
    GetWorldTimerManager().SetTimer(BoundaryTimer, this,
        &ASkiBootstrapGameMode::InspectP1PickerViewportBoundary, 0.12F, false);
}

void ASkiBootstrapGameMode::InspectP1PickerViewportBoundary()
{
    if (!P1Widget || !bPickerViewportTopValid || PickerViewportStep1EvidenceJson.IsEmpty())
    { FailPickerViewportSmoke(TEXT("picker did not preserve its Step 1 evidence")); return; }
    P1Widget->ForceLayoutPrepass();
    const FVector2D RootPosition = P1Widget->GetCachedGeometry().GetAbsolutePosition();
    UScrollBox* PickerScroll = Cast<UScrollBox>(P1Widget->GetWidgetFromName(TEXT("PickerScroll")));
    UWidget* LocationControls = P1Widget->GetWidgetFromName(TEXT("LocationControls"));
    UWidget* BoundaryControls = P1Widget->GetWidgetFromName(TEXT("BoundaryControls"));
    UWidget* NameControls = P1Widget->GetWidgetFromName(TEXT("ResortNameControls"));
    UWidget* BoundaryHeadingWidget = P1Widget->GetWidgetFromName(TEXT("BoundaryHeading"));
    UWidget* BoundaryInstructionsWidget = P1Widget->GetWidgetFromName(TEXT("BoundaryInstructions"));
    UWidget* ClearBoundaryWidget = P1Widget->GetWidgetFromName(TEXT("ClearBoundary"));
    UWidget* BoundaryStatusWidget = P1Widget->GetWidgetFromName(TEXT("PickerBoundaryStatus"));
    UWidget* PreviewStatusWidget = P1Widget->GetWidgetFromName(TEXT("PickerPreviewStatus"));
    UVerticalBox* Steps = Cast<UVerticalBox>(P1Widget->GetWidgetFromName(TEXT("PickerSteps")));
    UTextBlock* BoundaryHeading = Cast<UTextBlock>(BoundaryHeadingWidget);
    UTextBlock* BoundaryInstructions = Cast<UTextBlock>(BoundaryInstructionsWidget);
    UTextBlock* BoundaryStatus = Cast<UTextBlock>(BoundaryStatusWidget);
    UTextBlock* PreviewStatus = Cast<UTextBlock>(PreviewStatusWidget);
    USkiSiteMapWidget* MapWidget = Cast<USkiSiteMapWidget>(P1Widget->GetWidgetFromName(TEXT("SiteMap")));
    if (!PickerScroll || !LocationControls || !BoundaryControls || !NameControls
        || !BoundaryHeading || !BoundaryInstructions || !ClearBoundaryWidget
        || !BoundaryStatus || !PreviewStatus || !Steps || !MapWidget)
    {
        FailPickerViewportSmoke(TEXT("picker Step 2 content widgets are missing"));
        return;
    }

    TArray<FString> StepTexts;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        UTextBlock* Step = Index < Steps->GetChildrenCount()
            ? Cast<UTextBlock>(Steps->GetChildAt(Index)) : nullptr;
        if (!Step) { FailPickerViewportSmoke(TEXT("picker Step 2 label is missing")); return; }
        StepTexts.Add(Step->GetText().ToString());
    }
    const FVector4 BoundaryHeadingRect = PickerViewportRect(BoundaryHeadingWidget, RootPosition);
    const FVector4 BoundaryInstructionsRect = PickerViewportRect(BoundaryInstructionsWidget, RootPosition);
    const FVector4 ClearBoundaryRect = PickerViewportRect(ClearBoundaryWidget, RootPosition);
    const FVector4 BoundaryStatusRect = PickerViewportRect(BoundaryStatusWidget, RootPosition);
    const FVector4 PreviewStatusRect = PickerViewportRect(PreviewStatusWidget, RootPosition);
    const bool Step2Valid = PickerScroll->GetScrollOffset() <= 1.0F
        && LocationControls->GetVisibility() == ESlateVisibility::Collapsed
        && BoundaryControls->GetVisibility() == ESlateVisibility::Visible
        && NameControls->GetVisibility() == ESlateVisibility::Collapsed
        && StepTexts[0].StartsWith(TEXT("✓")) && StepTexts[1].StartsWith(TEXT("●"))
        && StepTexts[1].Contains(TEXT("Define boundary"))
        && BoundaryHeading->GetText().ToString().Contains(TEXT("Define your boundary"))
        && BoundaryInstructions->GetText().ToString().Contains(TEXT("Drag on the map"))
        && PickerViewportRectInside(BoundaryHeadingRect, PickerViewportScrollRect)
        && PickerViewportRectInside(BoundaryInstructionsRect, PickerViewportScrollRect)
        && PickerViewportRectInside(ClearBoundaryRect, PickerViewportScrollRect)
        && PickerViewportRectInside(BoundaryStatusRect, PickerViewportScrollRect)
        && PickerViewportRectInside(PreviewStatusRect, PickerViewportScrollRect);
    if (!Step2Valid)
    {
        FailPickerViewportSmoke(TEXT("picker did not expose valid Step 2 boundary controls"));
        return;
    }
    PickerViewportStep2EvidenceJson = FString::Printf(
        TEXT("\"step2\":{\"locationVisible\":false,\"boundaryVisible\":true,\"nameVisible\":false,\"activeLabel\":\"%s\",\"scrollAtStart\":%.2f,\"texts\":{\"boundaryHeading\":\"%s\",\"boundaryInstructions\":\"%s\",\"boundaryStatus\":\"%s\",\"previewStatus\":\"%s\"},\"rects\":{\"boundaryHeading\":%s,\"boundaryInstructions\":%s,\"clearBoundary\":%s,\"boundaryStatus\":%s,\"previewStatus\":%s}}"),
        *EscapePickerViewportJsonString(StepTexts[1]), PickerScroll->GetScrollOffset(),
        *EscapePickerViewportJsonString(BoundaryHeading->GetText().ToString()),
        *EscapePickerViewportJsonString(BoundaryInstructions->GetText().ToString()),
        *EscapePickerViewportJsonString(BoundaryStatus->GetText().ToString()),
        *EscapePickerViewportJsonString(PreviewStatus->GetText().ToString()),
        *PickerViewportRectJson(BoundaryHeadingRect),
        *PickerViewportRectJson(BoundaryInstructionsRect), *PickerViewportRectJson(ClearBoundaryRect),
        *PickerViewportRectJson(BoundaryStatusRect), *PickerViewportRectJson(PreviewStatusRect));

    FGuid SmokeToken;
    if (!FGuid::Parse(PickerViewportToken, SmokeToken)
        || !MapWidget->CreateViewportSmokeBoundary(SmokeToken)
        || !MapWidget->HasValidSelection())
    {
        FailPickerViewportSmoke(TEXT("tokened native boundary rectangle did not pass real selection validation"));
        return;
    }
    P1Widget->ForceLayoutPrepass();
    FTimerHandle NameTimer;
    GetWorldTimerManager().SetTimer(NameTimer, this,
        &ASkiBootstrapGameMode::InspectP1PickerViewportName, 0.12F, false);
}

void ASkiBootstrapGameMode::InspectP1PickerViewportName()
{
    if (!P1Widget || !bPickerViewportTopValid || PickerViewportStep2EvidenceJson.IsEmpty())
    { FailPickerViewportSmoke(TEXT("picker did not preserve its Step 2 evidence")); return; }
    P1Widget->ForceLayoutPrepass();
    const FVector2D RootPosition = P1Widget->GetCachedGeometry().GetAbsolutePosition();
    UScrollBox* PickerScroll = Cast<UScrollBox>(P1Widget->GetWidgetFromName(TEXT("PickerScroll")));
    UWidget* LocationControls = P1Widget->GetWidgetFromName(TEXT("LocationControls"));
    UWidget* BoundaryControls = P1Widget->GetWidgetFromName(TEXT("BoundaryControls"));
    UWidget* NameControls = P1Widget->GetWidgetFromName(TEXT("ResortNameControls"));
    UWidget* BoundaryHeadingWidget = P1Widget->GetWidgetFromName(TEXT("BoundaryHeading"));
    UWidget* BoundaryInstructionsWidget = P1Widget->GetWidgetFromName(TEXT("BoundaryInstructions"));
    UWidget* NameHeadingWidget = P1Widget->GetWidgetFromName(TEXT("ResortNameHeading"));
    UWidget* NameBoxWidget = P1Widget->GetWidgetFromName(TEXT("ResortNameBox"));
    UWidget* DownloadButtonWidget = P1Widget->GetWidgetFromName(TEXT("PickerDownload"));
    UWidget* DownloadLabelWidget = P1Widget->GetWidgetFromName(TEXT("PickerDownloadLabel"));
    UWidget* StepsWidget = P1Widget->GetWidgetFromName(TEXT("PickerSteps"));
    UButton* DownloadButton = Cast<UButton>(DownloadButtonWidget);
    UTextBlock* BoundaryHeading = Cast<UTextBlock>(BoundaryHeadingWidget);
    UTextBlock* BoundaryInstructions = Cast<UTextBlock>(BoundaryInstructionsWidget);
    UTextBlock* NameHeading = Cast<UTextBlock>(NameHeadingWidget);
    UEditableTextBox* NameBox = Cast<UEditableTextBox>(NameBoxWidget);
    UTextBlock* DownloadLabel = Cast<UTextBlock>(DownloadLabelWidget);
    UVerticalBox* Steps = Cast<UVerticalBox>(StepsWidget);
    USkiSiteMapWidget* MapWidget = Cast<USkiSiteMapWidget>(P1Widget->GetWidgetFromName(TEXT("SiteMap")));
    if (!PickerScroll || !LocationControls || !BoundaryControls || !NameControls
        || !BoundaryHeading || !BoundaryInstructions || !NameHeading || !NameBox
        || !DownloadButton || !DownloadLabel || !Steps || !MapWidget)
    {
        FailPickerViewportSmoke(TEXT("picker Step 3 content widgets are missing"));
        return;
    }

    TArray<FString> StepTexts;
    TArray<FString> SerializedStepTexts;
    TArray<FString> StepRectTexts;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        UTextBlock* Step = Index < Steps->GetChildrenCount()
            ? Cast<UTextBlock>(Steps->GetChildAt(Index)) : nullptr;
        if (!Step) { FailPickerViewportSmoke(TEXT("picker Step 3 label is missing")); return; }
        const FString StepText = Step->GetText().ToString();
        const FVector4 StepRect = PickerViewportRect(Step, RootPosition);
        if (!PickerViewportRectInside(StepRect, PickerViewportRect(StepsWidget, RootPosition)))
        { FailPickerViewportSmoke(TEXT("picker Step 3 label leaves its visible stepper")); return; }
        StepTexts.Add(StepText);
        SerializedStepTexts.Add(FString::Printf(TEXT("\"%s\""), *EscapePickerViewportJsonString(StepText)));
        StepRectTexts.Add(PickerViewportRectJson(StepRect));
    }
    const FVector4 BoundaryControlsRect = PickerViewportRect(BoundaryControls, RootPosition);
    const FVector4 BoundaryHeadingRect = PickerViewportRect(BoundaryHeadingWidget, RootPosition);
    const FVector4 BoundaryInstructionsRect = PickerViewportRect(BoundaryInstructionsWidget, RootPosition);
    const FVector4 NameControlsRect = PickerViewportRect(NameControls, RootPosition);
    const FVector4 NameHeadingRect = PickerViewportRect(NameHeadingWidget, RootPosition);
    const FVector4 NameBoxRect = PickerViewportRect(NameBoxWidget, RootPosition);
    const FVector4 DownloadButtonRect = PickerViewportRect(DownloadButtonWidget, RootPosition);
    const FVector4 DownloadLabelRect = PickerViewportRect(DownloadLabelWidget, RootPosition);
    const FString BoundaryHeadingText = BoundaryHeading->GetText().ToString();
    const FString BoundaryInstructionsText = BoundaryInstructions->GetText().ToString();
    const FString NameHeadingText = NameHeading->GetText().ToString();
    const FString DownloadLabelText = DownloadLabel->GetText().ToString();
    const bool Step3Valid = PickerScroll->GetScrollOffset() <= 1.0F
        && LocationControls->GetVisibility() == ESlateVisibility::Collapsed
        && BoundaryControls->GetVisibility() == ESlateVisibility::Visible
        && NameControls->GetVisibility() == ESlateVisibility::Visible
        && MapWidget->HasValidSelection() && !DownloadButton->GetIsEnabled()
        && StepTexts[0].StartsWith(TEXT("✓")) && StepTexts[1].StartsWith(TEXT("✓"))
        && StepTexts[2].StartsWith(TEXT("●")) && StepTexts[2].Contains(TEXT("Name resort"))
        && BoundaryHeadingText.Contains(TEXT("Define your boundary"))
        && BoundaryInstructionsText.Contains(TEXT("Drag on the map"))
        && NameHeadingText.Contains(TEXT("Name your resort"))
        && DownloadLabelText.Contains(TEXT("Download unavailable"))
        && PickerViewportRectValid(BoundaryControlsRect)
        && PickerViewportRectValid(BoundaryHeadingRect)
        && PickerViewportRectValid(BoundaryInstructionsRect)
        && PickerViewportRectValid(NameControlsRect)
        && PickerViewportRectValid(NameHeadingRect) && PickerViewportRectValid(NameBoxRect)
        && PickerViewportRectValid(DownloadButtonRect) && PickerViewportRectValid(DownloadLabelRect);
    if (!Step3Valid)
    {
        TArray<FString> FailedPredicates;
        auto RecordFailedPredicate = [&FailedPredicates](const bool bPassed, const TCHAR* Name)
        {
            if (!bPassed) FailedPredicates.Add(Name);
        };
        RecordFailedPredicate(PickerScroll->GetScrollOffset() <= 1.0F, TEXT("scroll-at-start"));
        RecordFailedPredicate(LocationControls->GetVisibility() == ESlateVisibility::Collapsed,
            TEXT("location-collapsed"));
        RecordFailedPredicate(BoundaryControls->GetVisibility() == ESlateVisibility::Visible,
            TEXT("boundary-visible"));
        RecordFailedPredicate(NameControls->GetVisibility() == ESlateVisibility::Visible,
            TEXT("name-visible"));
        RecordFailedPredicate(MapWidget->HasValidSelection(), TEXT("selection-valid"));
        RecordFailedPredicate(!DownloadButton->GetIsEnabled(), TEXT("download-disabled"));
        RecordFailedPredicate(StepTexts[0].StartsWith(TEXT("✓")), TEXT("step-1-complete"));
        RecordFailedPredicate(StepTexts[1].StartsWith(TEXT("✓")), TEXT("step-2-complete"));
        RecordFailedPredicate(StepTexts[2].StartsWith(TEXT("●")), TEXT("step-3-active"));
        RecordFailedPredicate(StepTexts[2].Contains(TEXT("Name resort")), TEXT("step-3-name-label"));
        RecordFailedPredicate(BoundaryHeadingText.Contains(TEXT("Define your boundary")),
            TEXT("boundary-heading-text"));
        RecordFailedPredicate(BoundaryInstructionsText.Contains(TEXT("Drag on the map")),
            TEXT("boundary-instructions-text"));
        RecordFailedPredicate(NameHeadingText.Contains(TEXT("Name your resort")),
            TEXT("name-heading-text"));
        RecordFailedPredicate(DownloadLabelText.Contains(TEXT("Download unavailable")),
            TEXT("download-label-text"));
        RecordFailedPredicate(PickerViewportRectValid(BoundaryControlsRect),
            TEXT("boundary-controls-rect"));
        RecordFailedPredicate(PickerViewportRectValid(BoundaryHeadingRect),
            TEXT("boundary-heading-rect"));
        RecordFailedPredicate(PickerViewportRectValid(BoundaryInstructionsRect),
            TEXT("boundary-instructions-rect"));
        RecordFailedPredicate(PickerViewportRectValid(NameControlsRect), TEXT("name-controls-rect"));
        RecordFailedPredicate(PickerViewportRectValid(NameHeadingRect), TEXT("name-heading-rect"));
        RecordFailedPredicate(PickerViewportRectValid(NameBoxRect), TEXT("name-box-rect"));
        RecordFailedPredicate(PickerViewportRectValid(DownloadButtonRect),
            TEXT("download-button-rect"));
        RecordFailedPredicate(PickerViewportRectValid(DownloadLabelRect),
            TEXT("download-label-rect"));

        const FString Diagnostic = FString::Printf(
            TEXT("valid site boundary did not reveal the frozen Step 3 name and disabled Download content; failed=[%s]; scroll=%.2f; visibility(location/boundary/name)=%d/%d/%d; selection=%d; downloadEnabled=%d; stepTexts=[%s | %s | %s | %s]; text(name/boundary/download)=[%s | %s | %s]; rects(boundary/name/download)=[%s | %s | %s]"),
            *FString::Join(FailedPredicates, TEXT(",")), PickerScroll->GetScrollOffset(),
            static_cast<int32>(LocationControls->GetVisibility()),
            static_cast<int32>(BoundaryControls->GetVisibility()),
            static_cast<int32>(NameControls->GetVisibility()),
            MapWidget->HasValidSelection() ? 1 : 0, DownloadButton->GetIsEnabled() ? 1 : 0,
            *EscapePickerViewportJsonString(StepTexts[0]),
            *EscapePickerViewportJsonString(StepTexts[1]),
            *EscapePickerViewportJsonString(StepTexts[2]),
            *EscapePickerViewportJsonString(StepTexts[3]),
            *EscapePickerViewportJsonString(NameHeadingText),
            *EscapePickerViewportJsonString(BoundaryHeadingText),
            *EscapePickerViewportJsonString(DownloadLabelText),
            *PickerViewportRectJson(BoundaryControlsRect),
            *PickerViewportRectJson(NameControlsRect),
            *PickerViewportRectJson(DownloadButtonRect));
        FailPickerViewportSmoke(*Diagnostic);
        return;
    }
    PickerViewportStepsJson = FString::Join(SerializedStepTexts, TEXT(","));
    PickerViewportTopRectsJson += FString::Printf(TEXT(",\"stepItems\":[%s]"),
        *FString::Join(StepRectTexts, TEXT(",")));
    UTextBlock* Heading = Cast<UTextBlock>(P1Widget->GetWidgetFromName(TEXT("PickerHeading")));
    UTextBlock* Subtitle = Cast<UTextBlock>(P1Widget->GetWidgetFromName(TEXT("PickerSubtitle")));
    PickerViewportTextJson = FString::Printf(
        TEXT("{\"heading\":\"%s\",\"subtitle\":\"%s\",\"steps\":[%s]}"),
        *EscapePickerViewportJsonString(Heading->GetText().ToString()),
        *EscapePickerViewportJsonString(Subtitle->GetText().ToString()),
        *PickerViewportStepsJson);
    PickerViewportStep3EvidenceJson = FString::Printf(
        TEXT("\"step3\":{\"locationVisible\":false,\"boundaryVisible\":true,\"nameVisible\":true,\"selectionValid\":true,\"downloadEnabled\":false,\"activeLabel\":\"%s\",\"texts\":{\"boundaryHeading\":\"%s\",\"boundaryInstructions\":\"%s\",\"resortNameHeading\":\"%s\",\"downloadLabel\":\"%s\"},\"rects\":{\"boundaryControls\":%s,\"boundaryHeading\":%s,\"boundaryInstructions\":%s,\"nameControls\":%s,\"resortNameHeading\":%s,\"nameBox\":%s,\"downloadButton\":%s,\"downloadLabel\":%s"),
        *EscapePickerViewportJsonString(StepTexts[2]),
        *EscapePickerViewportJsonString(BoundaryHeadingText),
        *EscapePickerViewportJsonString(BoundaryInstructionsText),
        *EscapePickerViewportJsonString(NameHeadingText), *EscapePickerViewportJsonString(DownloadLabelText),
        *PickerViewportRectJson(BoundaryControlsRect), *PickerViewportRectJson(BoundaryHeadingRect),
        *PickerViewportRectJson(BoundaryInstructionsRect), *PickerViewportRectJson(NameControlsRect),
        *PickerViewportRectJson(NameHeadingRect), *PickerViewportRectJson(NameBoxRect),
        *PickerViewportRectJson(DownloadButtonRect), *PickerViewportRectJson(DownloadLabelRect));

    PickerScroll->ScrollToEnd();
    P1Widget->ForceLayoutPrepass();
    FTimerHandle BottomTimer;
    GetWorldTimerManager().SetTimer(BottomTimer, this,
        &ASkiBootstrapGameMode::InspectP1PickerViewportBottom, 0.12F, false);
}

void ASkiBootstrapGameMode::InspectP1PickerViewportBottom()
{
    if (!P1Widget || !bPickerViewportTopValid || PickerViewportStep2EvidenceJson.IsEmpty()
        || PickerViewportStep3EvidenceJson.IsEmpty())
    { FailPickerViewportSmoke(TEXT("picker did not preserve the Step 3 state before scrolling")); return; }
    P1Widget->ForceLayoutPrepass();
    UScrollBox* PickerScroll = Cast<UScrollBox>(
        P1Widget->GetWidgetFromName(TEXT("PickerScroll")));
    UWidget* NameControlsWidget = P1Widget->GetWidgetFromName(TEXT("ResortNameControls"));
    UWidget* NameHeadingWidget = P1Widget->GetWidgetFromName(TEXT("ResortNameHeading"));
    UWidget* NameBoxWidget = P1Widget->GetWidgetFromName(TEXT("ResortNameBox"));
    UWidget* DownloadButtonWidget = P1Widget->GetWidgetFromName(TEXT("PickerDownload"));
    UWidget* DownloadLabelWidget = P1Widget->GetWidgetFromName(TEXT("PickerDownloadLabel"));
    UButton* DownloadButton = Cast<UButton>(DownloadButtonWidget);
    UTextBlock* NameHeading = Cast<UTextBlock>(NameHeadingWidget);
    UTextBlock* DownloadLabel = Cast<UTextBlock>(DownloadLabelWidget);
    if (!PickerScroll || !NameControlsWidget || !NameHeading || !NameBoxWidget
        || !DownloadButton || !DownloadLabel
        || NameControlsWidget->GetVisibility() != ESlateVisibility::Visible)
    {
        FailPickerViewportSmoke(TEXT("picker bottom content widgets are missing"));
        return;
    }

    PickerViewportScrollAtEnd = PickerScroll->GetScrollOffset();
    PickerViewportScrollMaximum = PickerScroll->GetScrollOffsetOfEnd();
    const FVector2D RootPosition = P1Widget->GetCachedGeometry().GetAbsolutePosition();
    const FVector4 NameControlsRect = PickerViewportRect(NameControlsWidget, RootPosition);
    const FVector4 NameHeadingRect = PickerViewportRect(NameHeadingWidget, RootPosition);
    const FVector4 NameBoxRect = PickerViewportRect(NameBoxWidget, RootPosition);
    const FVector4 DownloadButtonRect = PickerViewportRect(DownloadButtonWidget, RootPosition);
    const FVector4 DownloadLabelRect = PickerViewportRect(DownloadLabelWidget, RootPosition);
    const bool BottomValid = IsPickerViewportScrollAtEnd(PickerViewportScrollAtEnd,
            PickerViewportScrollMaximum)
        && PickerViewportRectInside(NameControlsRect, PickerViewportScrollRect)
        && PickerViewportRectInside(NameHeadingRect, PickerViewportScrollRect)
        && PickerViewportRectInside(NameBoxRect, PickerViewportScrollRect)
        && PickerViewportRectInside(DownloadButtonRect, PickerViewportScrollRect)
        && PickerViewportRectInside(DownloadLabelRect, DownloadButtonRect)
        && !DownloadButton->GetIsEnabled()
        && NameHeading->GetText().ToString().Contains(TEXT("Name your resort"))
        && DownloadLabel->GetText().ToString().Contains(TEXT("Download unavailable"));
    if (!BottomValid)
    {
        FailPickerViewportSmoke(TEXT("scrolling cannot reach visible Step 3 name and disabled Download controls"));
        return;
    }
    bPickerViewportBottomValid = true;
    PickerViewportStep3EvidenceJson += FString::Printf(
        TEXT("},\"scrollAtEnd\":%.2f,\"scrollMaximum\":%.2f,\"bottomRects\":{\"nameControls\":%s,\"resortNameHeading\":%s,\"nameBox\":%s,\"downloadButton\":%s,\"downloadLabel\":%s}}"),
        PickerViewportScrollAtEnd, PickerViewportScrollMaximum,
        *PickerViewportRectJson(NameControlsRect), *PickerViewportRectJson(NameHeadingRect),
        *PickerViewportRectJson(NameBoxRect), *PickerViewportRectJson(DownloadButtonRect),
        *PickerViewportRectJson(DownloadLabelRect));

    FTimerHandle ScreenshotTimer;
    GetWorldTimerManager().SetTimer(ScreenshotTimer, this,
        &ASkiBootstrapGameMode::RequestP1PickerViewportScreenshot, 0.12F, false);
}

void ASkiBootstrapGameMode::RequestP1PickerViewportScreenshot()
{
    if (!P1Widget || !bPickerViewportTopValid || !bPickerViewportBottomValid)
    {
        FailPickerViewportSmoke(TEXT("picker was not ready for screenshot capture"));
        return;
    }
    UScrollBox* PickerScroll = Cast<UScrollBox>(
        P1Widget->GetWidgetFromName(TEXT("PickerScroll")));
    if (!PickerScroll || !IsPickerViewportScrollAtEnd(PickerScroll->GetScrollOffset(),
            PickerScroll->GetScrollOffsetOfEnd()))
    {
        FailPickerViewportSmoke(TEXT("picker did not remain at the name-step scroll bottom before capture"));
        return;
    }
    FScreenshotRequest::RequestScreenshot(PickerViewportScreenshotPath, true, false, false);
    FTimerHandle FinishTimer;
    GetWorldTimerManager().SetTimer(FinishTimer, this,
        &ASkiBootstrapGameMode::FinishP1PickerViewportSmoke, 1.5F, false);
}

void ASkiBootstrapGameMode::FinishP1PickerViewportSmoke()
{
    if (!P1Widget || !bPickerViewportTopValid || !bPickerViewportBottomValid
        || PickerViewportStep1EvidenceJson.IsEmpty() || PickerViewportStep2EvidenceJson.IsEmpty()
        || PickerViewportStep3EvidenceJson.IsEmpty()
        || !FPaths::FileExists(PickerViewportScreenshotPath))
    {
        FailPickerViewportSmoke(TEXT("rendered picker screenshot was not written"));
        return;
    }
    TArray<uint8> ScreenshotBytes;
    if (!FFileHelper::LoadFileToArray(ScreenshotBytes, *PickerViewportScreenshotPath))
    {
        FailPickerViewportSmoke(TEXT("rendered picker screenshot could not be read back"));
        return;
    }
    APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    int32 Width = 0;
    int32 Height = 0;
    if (!Controller) { FailPickerViewportSmoke(TEXT("player controller disappeared")); return; }
    Controller->GetViewportSize(Width, Height);
    if (Width != PickerViewportWidth || Height != PickerViewportHeight)
    {
        FailPickerViewportSmoke(TEXT("viewport changed before screenshot receipt"));
        return;
    }
    const FString ScreenshotHash = SkiPreparation::Sha256(ScreenshotBytes);
    const FString Receipt = FString::Printf(
        TEXT("{\"token\":\"%s\",\"scenario\":\"picker-viewport\",\"resolution\":[%d,%d],\"viewport\":[%d,%d],\"captureKind\":\"rendered-viewport-png\",\"capturedStep\":\"name-resort\",\"capturedScrollPosition\":\"end\",\"nativePickerVisible\":true,\"mapWidgetPresent\":true,\"networkDisabled\":true,\"topContentVisible\":true,\"bottomContentReachable\":true,\"visualReviewRequired\":true,\"scrollAtStart\":%.2f,\"scrollAtEnd\":%.2f,\"scrollMaximum\":%.2f,\"rects\":{%s},\"texts\":%s,\"stepStates\":{%s,%s,%s},\"screenshotPath\":\"%s\",\"screenshotSha256\":\"%s\"}"),
        *PickerViewportToken, Width, Height, Width, Height, PickerViewportScrollAtStart,
        PickerViewportScrollAtEnd, PickerViewportScrollMaximum,
        *PickerViewportTopRectsJson, *PickerViewportTextJson,
        *PickerViewportStep1EvidenceJson, *PickerViewportStep2EvidenceJson,
        *PickerViewportStep3EvidenceJson,
        *EscapePickerViewportJsonString(PickerViewportScreenshotPath), *ScreenshotHash);
    const bool Written = FFileHelper::SaveStringToFile(Receipt, *PickerViewportReceiptPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    FPlatformMisc::RequestExitWithStatus(false, Written ? 0 : 1);
}
#endif

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
    if (GetWorld() && GetWorld()->GetOutermost()->GetName()
        == TEXT("/Game/P1Generated/P1Terrain"))
    {
        if (P1Widget)
            P1Widget->SetTransientStatus(TEXT("Resort acquisition is unavailable in the Mountain phase."));
        return;
    }
    MountainAcquisitionDeny.Reset();
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
    if (bInstalledVerifierBusy)
    {
        if (DeferredInstalledOpenId == TEXT("latest")) return;
        if (PendingInstalledOpenId == TEXT("latest")
            && DeferredInstalledOpenId.IsEmpty()) return;
        DeferredInstalledOpenId = TEXT("latest");
        ++InstalledOpenGeneration;
        if (P1Widget) P1Widget->SetSelectorStatus(TEXT("Waiting for the previous resort verification…"));
        return;
    }
    if (PendingInstalledOpenId == TEXT("latest")) return;
    PendingInstalledOpenId = TEXT("latest");
    bInstalledVerifierBusy = true;
    const FString Root = FPaths::ProjectSavedDir();
    const uint64 Generation = ++InstalledOpenGeneration;
    const TWeakObjectPtr<ASkiBootstrapGameMode> WeakThis(this);
    if (P1Widget) P1Widget->SetSelectorStatus(TEXT("Finding the latest verified resort…"));
    Async(EAsyncExecution::ThreadPool, [WeakThis, Root, Generation]()
    {
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
        Candidates.Sort([](const TPair<FDateTime, FString>& A,
            const TPair<FDateTime, FString>& B)
        {
            return A.Key == B.Key ? A.Value < B.Value : A.Key > B.Key;
        });
        SkiPreparation::InstalledTerrainStore Store(Root);
        FString SelectedId;
        for (const auto& Candidate : Candidates)
        {
            SkiPreparation::InstalledTerrainIndex Index;
            FString Error;
            FString TerrainCoreId;
            FString CoverEcologyId;
            const bool bOpened = Store.Open(Candidate.Value, Index, Error);
            if (ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(
                    Candidate.Value, Index, bOpened, TerrainCoreId, CoverEcologyId))
            {
                SelectedId = Candidate.Value;
                break;
            }
        }
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Generation, SelectedId = MoveTemp(SelectedId)]()
        {
            if (!WeakThis.IsValid()) return;
            WeakThis->bInstalledVerifierBusy = false;
            WeakThis->PendingInstalledOpenId.Empty();
            if (!WeakThis->DeferredInstalledOpenId.IsEmpty())
            {
                const FString Next = MoveTemp(WeakThis->DeferredInstalledOpenId);
                WeakThis->DeferredInstalledOpenId.Empty();
                if (Next == TEXT("latest")) WeakThis->OpenLatestInstalledTerrain();
                else WeakThis->TransitionToInstalledTerrain(Next);
                return;
            }
            if (WeakThis->InstalledOpenGeneration != Generation) return;
            if (!SelectedId.IsEmpty()) WeakThis->TransitionToInstalledTerrain(SelectedId);
            else if (WeakThis->P1Widget)
                WeakThis->P1Widget->SetSelectorStatus(TEXT("No verified installed resort was found."));
        });
    });
}

void ASkiBootstrapGameMode::TransitionToInstalledTerrain(const FString& ContentId)
{
    if (!IsContentId(ContentId))
    {
        if (P1Widget) P1Widget->SetSelectorStatus(TEXT("Installed terrain ID is invalid."));
        return;
    }
    if (bInstalledVerifierBusy)
    {
        if (DeferredInstalledOpenId == ContentId) return;
        if (PendingInstalledOpenId == ContentId
            && DeferredInstalledOpenId.IsEmpty()) return;
        DeferredInstalledOpenId = ContentId;
        ++InstalledOpenGeneration;
        if (P1Widget) P1Widget->SetSelectorStatus(TEXT("Waiting for the previous resort verification…"));
        return;
    }
    if (PendingInstalledOpenId == ContentId) return;
    PendingInstalledOpenId = ContentId;
    bInstalledVerifierBusy = true;
    const uint64 Generation = ++InstalledOpenGeneration;
    const FString Root = InstalledOpenDataRootOverride.IsEmpty()
        ? FPaths::ProjectSavedDir() : InstalledOpenDataRootOverride;
    const TWeakObjectPtr<ASkiBootstrapGameMode> WeakThis(this);
    if (P1Widget) P1Widget->SetSelectorStatus(TEXT("Verifying installed resort…"));
    Async(EAsyncExecution::ThreadPool, [WeakThis, Root, ContentId, Generation]()
    {
        SkiPreparation::InstalledTerrainIndex Index;
        FString Error;
        FString TerrainCoreId;
        FString CoverEcologyId;
        const bool bOpened = SkiPreparation::InstalledTerrainStore(Root).Open(
            ContentId, Index, Error);
        const bool bVerified = ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(
            ContentId, Index, bOpened, TerrainCoreId, CoverEcologyId);
        if (bOpened && !bVerified && Error.IsEmpty())
            Error = TEXT("Installed receipt is not a verified playable schema-2 or schema-3 package.");
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, ContentId, Generation, bVerified, Error = MoveTemp(Error)]()
        {
            if (!WeakThis.IsValid()) return;
            WeakThis->bInstalledVerifierBusy = false;
            WeakThis->PendingInstalledOpenId.Empty();
            if (!WeakThis->DeferredInstalledOpenId.IsEmpty())
            {
                const FString Next = MoveTemp(WeakThis->DeferredInstalledOpenId);
                WeakThis->DeferredInstalledOpenId.Empty();
                if (Next == TEXT("latest")) WeakThis->OpenLatestInstalledTerrain();
                else WeakThis->TransitionToInstalledTerrain(Next);
                return;
            }
            if (WeakThis->InstalledOpenGeneration != Generation) return;
            if (!bVerified)
            {
                if (WeakThis->P1Widget)
                    WeakThis->P1Widget->SetSelectorStatus(
                        TEXT("Installed resort could not be verified: ") + Error);
                return;
            }
            USkiFlowSubsystem* Flow = WeakThis->GetGameInstance()->GetSubsystem<USkiFlowSubsystem>();
            if (!Flow)
            {
                if (WeakThis->P1Widget)
                    WeakThis->P1Widget->SetSelectorStatus(TEXT("Resort navigation is unavailable."));
                return;
            }
            Flow->QueueInstalledResort(ContentId, WeakThis->InstalledOpenDataRootOverride);
            UGameplayStatics::OpenLevel(WeakThis.Get(), FName(TEXT("/Game/P1Generated/P1Terrain")));
        });
    });
}

void ASkiBootstrapGameMode::RefreshInstalledLibrary()
{
    if (!P1Widget) return;
    const uint64 Generation = ++LibraryRefreshGeneration;
    const FString Root = FPaths::ProjectSavedDir();
    const TWeakObjectPtr<ASkiBootstrapGameMode> WeakThis(this);
    P1Widget->SetSelectorStatus(TEXT("Checking installed resorts…"));
    Async(EAsyncExecution::ThreadPool, [WeakThis, Root, Generation]()
    {
        SkiPreparation::InstalledTerrainStore Store(Root);
        TArray<SkiPreparation::InstalledTerrainLibraryEntry> Verified;
        FString Error;
        const bool bListed = Store.ListVerified(Verified, Error);
        AsyncTask(ENamedThreads::GameThread,
            [WeakThis, Generation, bListed, Verified = MoveTemp(Verified), Error = MoveTemp(Error)]()
        {
            if (!WeakThis.IsValid() || WeakThis->LibraryRefreshGeneration != Generation
                || !WeakThis->P1Widget) return;
            TArray<FSkiInstalledResortItem> Items;
            if (bListed)
            {
                Items.Reserve(Verified.Num());
                for (const auto& Entry : Verified)
                {
                    FSkiInstalledResortItem& Item = Items.AddDefaulted_GetRef();
                    Item.ContentId = Entry.ContentId;
                    Item.DisplayName = TEXT("Installed resort ") + Entry.ContentId.Left(8);
                    Item.Detail = Entry.SourceId.IsEmpty() ? TEXT("Verified terrain package")
                        : Entry.SourceId;
                    if (!Entry.AcquisitionEpoch.IsEmpty())
                        Item.Detail += TEXT(" | ") + Entry.AcquisitionEpoch;
                }
            }
            WeakThis->P1Widget->SetInstalledResorts(Items);
            if (!WeakThis->PendingInstalledOpenId.IsEmpty()) return;
            WeakThis->P1Widget->SetSelectorStatus(!WeakThis->ReturnErrorNotice.IsEmpty()
                ? WeakThis->ReturnErrorNotice
                : bListed
                    ? FString::Printf(TEXT("%d verified installed resort%s."), Items.Num(),
                        Items.Num() == 1 ? TEXT("") : TEXT("s"))
                    : TEXT("Installed resort library could not be read: ") + Error);
        });
    });
}

bool ASkiBootstrapGameMode::OpenInstalledTerrain(const FString& ContentId,
    std::shared_ptr<FSkiPreparedInstalledTerrain> Prepared)
{
    const auto Fail = [this](const FString& Message)
    {
        ClearInstalledPhotoContext();
        const bool bStillInMountain = GetWorld()
            && GetWorld()->GetOutermost()->GetName() == TEXT("/Game/P1Generated/P1Terrain");
        if (!bStillInMountain) MountainAcquisitionDeny.Reset();
        if (P1Widget)
        {
            P1Widget->OpenSelector();
            P1Widget->SetSelectorStatus(Message);
        }
        return false;
    };
    ClearInstalledPhotoContext();
    if (!IsContentId(ContentId)) return Fail(TEXT("Installed terrain ID is invalid."));
    if (!MountainAcquisitionDeny)
        MountainAcquisitionDeny = MakeUnique<SkiPreparation::ScopedAcquisitionPortDeny>();
    if (!MountainAcquisitionDeny->IsActive())
        return Fail(TEXT("Offline terrain reopen could not disable acquisition."));
    if (!Prepared)
    {
        FString RequestedEditSetId;
        FParse::Value(FCommandLine::Get(), TEXT("SkiP1EditSetId="), RequestedEditSetId);
        Prepared = PrepareInstalledTerrain(FPaths::ProjectSavedDir(), ContentId,
            RequestedEditSetId);
    }
    if (!Prepared || !Prepared->bReady
        || (Prepared->Installation.SchemaVersion == SkiPreparation::CompositeInstallReceiptSchema
            && !Prepared->bHasVerifiedSiteContext))
        return Fail(TEXT("Installed terrain could not be verified: ")
            + (Prepared ? Prepared->Error : TEXT("No prepared package.")));
    const auto& Core = Prepared->Core;
    const auto& Ecology = Prepared->Ecology;
    const auto& Cover = Prepared->Cover;
    const auto& Validity = Prepared->Validity;
    const auto& PresentedRepository = Prepared->PresentedRepository;
    const SkiDomain::Revision Revision = Prepared->Revision;
    const FString& EditSetId = Prepared->EditSetId;
    if (MountainAcquisitionDeny->ObservedTransportCalls() != 0)
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
                if (FParse::Param(FCommandLine::Get(), TEXT("SkiM1FrontEndSmoke")))
                {
                    FString ReceiptPath, DataRoot, Token;
                    const bool bArguments = FParse::Value(FCommandLine::Get(),
                            TEXT("SkiP1Receipt="), ReceiptPath)
                        && FParse::Value(FCommandLine::Get(), TEXT("SkiP1DataRoot="), DataRoot)
                        && FParse::Value(FCommandLine::Get(), TEXT("SkiP1Token="), Token);
                    ReceiptPath = FPaths::ConvertRelativePathToFull(ReceiptPath);
                    DataRoot = FPaths::ConvertRelativePathToFull(DataRoot);
                    FPaths::NormalizeFilename(ReceiptPath);
                    FPaths::NormalizeFilename(DataRoot);
                    const bool bCanonical = FPaths::CollapseRelativeDirectories(ReceiptPath)
                        && FPaths::CollapseRelativeDirectories(DataRoot);
                    if (!DataRoot.EndsWith(TEXT("/"))) DataRoot += TEXT("/");
                    const bool bSafe = bArguments && bCanonical
                        && ReceiptPath.StartsWith(DataRoot, ESearchCase::IgnoreCase)
                        && FPaths::GetCleanFilename(ReceiptPath)
                            == Token + TEXT(".receipt.json");
                    const bool bOffline = WeakThis->MountainAcquisitionDeny
                        && WeakThis->MountainAcquisitionDeny->IsActive()
                        && WeakThis->MountainAcquisitionDeny->ObservedTransportCalls() == 0;
                    const FString Receipt = FString::Printf(
                        TEXT("{\"token\":\"%s\",\"scenario\":\"frontend\",\"passed\":true,\"nativeTitle\":true,\"nativePickerPlaceholder\":true,\"installedIdForwarded\":true,\"browserWidgetAbsent\":true,\"mountainTravel\":true,\"offlineReopen\":%s,\"contentId\":\"%s\"}"),
                        *Token, bOffline ? TEXT("true") : TEXT("false"), *ContentId);
                    const bool bWritten = bSafe && bOffline
                        && FFileHelper::SaveStringToFile(Receipt, *ReceiptPath,
                            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                    FPlatformMisc::RequestExitWithStatus(false, bWritten ? 0 : 1);
                }
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
        FString TerrainDetails = FString::Printf(
            TEXT("Installed terrain | reopened offline\n%s\nActual %u x %u samples | delivered %.2f x %.2f m\nNative source spacing: %s\nGround grid processing: %s\nDatum %s\nTerrainCore %s\nCoverEcology %s\nInstallation schema %u: %s%s"),
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
            UTF8_TO_TCHAR(Ecology.Manifest.ContentId.c_str()),
            Prepared->Installation.SchemaVersion, *ContentId,
            EditSetId.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("\nEdit sidecar %s"), *EditSetId));
        TerrainDetails = ComposeInstalledTerrainDetails(TerrainDetails,
            Prepared->Installation.SchemaVersion, Prepared->SiteContext.Manifest,
            Prepared->Installation.CompositeReceipt.Quality);
        P1Widget->SetTerrainDetails(TerrainDetails,
            Core.Manifest.Source.SourceId.find("fixture") != std::string::npos);
        P1Widget->SetNodeStatus(FString::Printf(
            TEXT("Runtime node view\nInstalled TerrainCore: %s\nCanonical/render/query: %llu/%llu/%llu"),
            UTF8_TO_TCHAR(Core.Manifest.ContentId.c_str()),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Canonical),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Render),
            static_cast<uint64>(TerrainCoreSession->Snapshot().Revisions.Query)));
        const bool bPhotoAvailable = USkiP1Widget::ShouldShowPhotoCommandForInstallation(
            Prepared->Installation.SchemaVersion, Prepared->bHasVerifiedSiteContext);
        if (bPhotoAvailable)
        {
            InstalledPhotoSiteContext = std::make_shared<const SkiPreparation::SiteContextPackageIndex>(
                Prepared->SiteContext);
            InstalledPhotoDataRoot = Prepared->DataRoot;
        }
        P1Widget->SetPhotoCommandAvailable(bPhotoAvailable);
        P1Widget->SetViewCommandHandler([
            WeakThis = TWeakObjectPtr<ASkiBootstrapGameMode>(this),
            WeakTerrain = TWeakObjectPtr<ASkiTerrainActor>(TerrainActor), bPhotoAvailable](
            const FName Command)
        {
            if (!WeakTerrain.IsValid()) return;
            const bool bLodSelectionCommand = Command == TEXT("LodAuto")
                || Command == TEXT("Lod0") || Command == TEXT("Lod1")
                || Command == TEXT("Lod2");
            if (Command == TEXT("Photo"))
            {
                if (!bPhotoAvailable || !WeakThis.IsValid()
                    || !WeakThis->InstalledPhotoSiteContext
                    || !WeakThis->MountainAcquisitionDeny
                    || !WeakThis->MountainAcquisitionDeny->IsActive()
                    || WeakThis->MountainAcquisitionDeny->ObservedTransportCalls() != 0)
                {
                    return;
                }
                WeakTerrain->SetViewMode(ESkiTerrainViewMode::Photo);
                WeakThis->BeginInstalledPhotoPresentation();
            }
            else if (Command == TEXT("Elevation"))
            {
                if (WeakThis.IsValid()) WeakThis->CancelInstalledPhotoPresentation();
                WeakTerrain->SetViewMode(ESkiTerrainViewMode::Elevation);
            }
            else if (Command == TEXT("Slope"))
            {
                if (WeakThis.IsValid()) WeakThis->CancelInstalledPhotoPresentation();
                WeakTerrain->SetViewMode(ESkiTerrainViewMode::Slope);
            }
            else if (Command == TEXT("Cover"))
            {
                if (WeakThis.IsValid()) WeakThis->CancelInstalledPhotoPresentation();
                WeakTerrain->SetViewMode(ESkiTerrainViewMode::Cover);
            }
            else if (Command == TEXT("Lod"))
            {
                if (WeakThis.IsValid()) WeakThis->CancelInstalledPhotoPresentation();
                WeakTerrain->SetViewMode(ESkiTerrainViewMode::TileLod);
            }
            else if (Command == TEXT("LodAuto"))
            {
                WeakTerrain->SetLodAuto();
                if (WeakThis.IsValid() && WeakTerrain->GetViewMode() == ESkiTerrainViewMode::Photo)
                    WeakThis->RefreshInstalledPhotoSelection();
            }
            else if (Command == TEXT("Lod0"))
            {
                WeakTerrain->SetLod(0);
                if (WeakThis.IsValid() && WeakTerrain->GetViewMode() == ESkiTerrainViewMode::Photo)
                    WeakThis->RefreshInstalledPhotoSelection();
            }
            else if (Command == TEXT("Lod1"))
            {
                WeakTerrain->SetLod(1);
                if (WeakThis.IsValid() && WeakTerrain->GetViewMode() == ESkiTerrainViewMode::Photo)
                    WeakThis->RefreshInstalledPhotoSelection();
            }
            else if (Command == TEXT("Lod2"))
            {
                WeakTerrain->SetLod(2);
                if (WeakThis.IsValid() && WeakTerrain->GetViewMode() == ESkiTerrainViewMode::Photo)
                    WeakThis->RefreshInstalledPhotoSelection();
            }
            else if (Command == TEXT("Vertical1")) WeakTerrain->SetVerticalExaggeration(1.0F);
            else if (Command == TEXT("Vertical2")) WeakTerrain->SetVerticalExaggeration(2.0F);
            else if (Command == TEXT("Vertical4")) WeakTerrain->SetVerticalExaggeration(4.0F);
            else if (Command == TEXT("Midday") || Command == TEXT("LowAngle")
                || Command == TEXT("Overcast")) WeakTerrain->SetLightingPreset(Command);
            else
            {
                if (WeakThis.IsValid()) WeakThis->CancelInstalledPhotoPresentation();
                WeakTerrain->SetViewMode(ESkiTerrainViewMode::Presentation);
            }
        });
        P1Widget->SetTransientStatus(TEXT("Verified installed terrain; streaming offline overview."));
    }
    return true;
}

void ASkiBootstrapGameMode::BeginInstalledPhotoPresentation()
{
    if (!TerrainActor || !InstalledPhotoSiteContext || InstalledPhotoDataRoot.IsEmpty()
        || TerrainActor->GetViewMode() != ESkiTerrainViewMode::Photo
        || !MountainAcquisitionDeny || !MountainAcquisitionDeny->IsActive()
        || MountainAcquisitionDeny->ObservedTransportCalls() != 0)
    {
        CancelInstalledPhotoPresentation();
        return;
    }

    if (UWorld* World = GetWorld(); World
        && !World->GetTimerManager().IsTimerActive(InstalledPhotoSelectionPollTimer))
    {
        World->GetTimerManager().SetTimer(InstalledPhotoSelectionPollTimer, this,
            &ASkiBootstrapGameMode::RefreshInstalledPhotoSelection, 0.25F, true);
    }
    RefreshInstalledPhotoSelection();
    if (P1Widget)
        P1Widget->SetTransientStatus(TEXT("Photo view — loading verified imagery for the current terrain view."));
}

void ASkiBootstrapGameMode::RefreshInstalledPhotoSelection()
{
    if (!IsInGameThread()) return;
    if (!TerrainActor || !InstalledPhotoSiteContext || InstalledPhotoDataRoot.IsEmpty()
        || TerrainActor->GetViewMode() != ESkiTerrainViewMode::Photo
        || !MountainAcquisitionDeny || !MountainAcquisitionDeny->IsActive()
        || MountainAcquisitionDeny->ObservedTransportCalls() != 0)
    {
        CancelInstalledPhotoPresentation();
        return;
    }

    uint64 Generation = 0;
    TArray<SkiApplication::TerrainCoreTileKey> DesiredKeys;
    if (!TerrainActor->GetTerrainCorePhotoRequestSnapshot(Generation, DesiredKeys)
        || DesiredKeys.IsEmpty() || DesiredKeys.Num() > 256)
    {
        // Preserve the poll timer so a temporarily unavailable selection can recover.
        if (InstalledPhotoStream)
        {
            if (InstalledPhotoStream->Mailbox)
                InstalledPhotoStream->Mailbox->Cancelled.store(true, std::memory_order_release);
            InstalledPhotoStream.reset();
            ++InstalledPhotoRequestSerial;
        }
        return;
    }

    PumpInstalledPhotoCompletions();

    if (!TerrainActor || TerrainActor->GetViewMode() != ESkiTerrainViewMode::Photo
        || !TerrainActor->GetTerrainCorePhotoRequestSnapshot(Generation, DesiredKeys)
        || DesiredKeys.IsEmpty() || DesiredKeys.Num() > 256)
    {
        if (InstalledPhotoStream)
        {
            if (InstalledPhotoStream->Mailbox)
                InstalledPhotoStream->Mailbox->Cancelled.store(true, std::memory_order_release);
            InstalledPhotoStream.reset();
            ++InstalledPhotoRequestSerial;
        }
        return;
    }

    if (InstalledPhotoStream
        && InstalledPhotoStream->Generation == Generation
        && InstalledPhotoStream->DesiredKeys == DesiredKeys)
    {
        return;
    }

    if (InstalledPhotoStream && InstalledPhotoStream->Mailbox)
        InstalledPhotoStream->Mailbox->Cancelled.store(true, std::memory_order_release);
    InstalledPhotoStream.reset();

    auto Stream = std::make_shared<FSkiInstalledPhotoStreamState>();
    Stream->RequestSerial = ++InstalledPhotoRequestSerial;
    Stream->Generation = Generation;
    Stream->DesiredKeys = MoveTemp(DesiredKeys);
    Stream->Mailbox = std::make_shared<FSkiInstalledPhotoMailbox>();
    Stream->SiteContext = InstalledPhotoSiteContext;
    Stream->DataRoot = InstalledPhotoDataRoot;
    InstalledPhotoStream = MoveTemp(Stream);
    IssueInstalledPhotoTileReads();
}

void ASkiBootstrapGameMode::PumpInstalledPhotoCompletions()
{
    if (!IsInGameThread() || !InstalledPhotoStream || !InstalledPhotoStream->Mailbox) return;
    const std::shared_ptr<FSkiInstalledPhotoMailbox> Mailbox = InstalledPhotoStream->Mailbox;
    if (Mailbox->Cancelled.load(std::memory_order_acquire)) return;

    TArray<FSkiInstalledPhotoCompletion> Completions;
    {
        FScopeLock Lock(&Mailbox->Mutex);
        Completions = MoveTemp(Mailbox->Completions);
        Mailbox->Completions.Reset();
    }
    for (FSkiInstalledPhotoCompletion& Completion : Completions)
    {
        CompleteInstalledPhotoTileRead(Completion.RequestSerial, Completion.Generation,
            Completion.Key, Completion.bSucceeded, MoveTemp(Completion.Pixels), Completion.Error);
    }
}

void ASkiBootstrapGameMode::IssueInstalledPhotoTileReads()
{
    if (!IsInGameThread()) return;
    const std::shared_ptr<FSkiInstalledPhotoStreamState> Stream = InstalledPhotoStream;
    if (!Stream || !Stream->SiteContext || !Stream->Mailbox
        || Stream->Mailbox->Cancelled.load(std::memory_order_acquire)) return;

    constexpr int32 MaximumConcurrentPhotoReads = 2;
    while (Stream->InFlightReads < MaximumConcurrentPhotoReads
        && Stream->NextKeyIndex < Stream->DesiredKeys.Num())
    {
        const SkiApplication::TerrainCoreTileKey Key =
            Stream->DesiredKeys[Stream->NextKeyIndex++];
        ++Stream->InFlightReads;

        // Capture only immutable package metadata and copied scalar request values on workers.
        const std::shared_ptr<const SkiPreparation::SiteContextPackageIndex> SiteContext =
            Stream->SiteContext;
        const std::shared_ptr<FSkiInstalledPhotoMailbox> Mailbox = Stream->Mailbox;
        const FString DataRoot = Stream->DataRoot;
        const uint64 RequestSerial = Stream->RequestSerial;
        const uint64 Generation = Stream->Generation;
        Async(EAsyncExecution::ThreadPool,
            [Mailbox, SiteContext, DataRoot, Key, RequestSerial, Generation]()
            {
                if (Mailbox->Cancelled.load(std::memory_order_acquire)) return;

                bool bSucceeded = false;
                TArray<FColor> Pixels;
                FString Error;
                const SkiPreparation::SiteContextManifest& Manifest = SiteContext->Manifest;
                const auto Imagery = std::find_if(Manifest.ImageryTiles.begin(),
                    Manifest.ImageryTiles.end(), [&Key](const SkiPreparation::SiteContextImageryTile& Tile)
                    {
                        return Tile.LodIndex == Key.Lod && Tile.TileX == Key.X
                            && Tile.TileY == Key.Y;
                    });
                if (Imagery == Manifest.ImageryTiles.end()
                    || Manifest.ImageryTilePixels != SkiPreparation::SiteContextPhotoTilePixels
                    || Imagery->PixelWidth != SkiPreparation::SiteContextPhotoTilePixels
                    || Imagery->PixelHeight != SkiPreparation::SiteContextPhotoTilePixels
                    || Manifest.ImageryEncoding != "jpeg-rgb8-v1"
                    || Imagery->AssetPath.empty())
                {
                    Error = TEXT("The verified SiteContext has no exact-key 256x256 imagery tile.");
                }
                else
                {
                    const std::string& AssetPath = Imagery->AssetPath;
                    const auto Asset = std::find_if(Manifest.Assets.begin(), Manifest.Assets.end(),
                        [&AssetPath](const SkiPreparation::SiteContextAsset& Candidate)
                        { return Candidate.Path == AssetPath; });
                    if (Asset == Manifest.Assets.end()
                        || Asset->Type != Manifest.ImageryEncoding
                        || Asset->Length == 0
                        || Asset->Length > SkiPreparation::SiteContextPhotoTileMaxCompressedBytes)
                    {
                        Error = TEXT("The exact-key imagery asset is missing or exceeds the photo tile cap.");
                    }
                    else if (!Mailbox->Cancelled.load(std::memory_order_acquire))
                    {
                        const FString RelativePath = UTF8_TO_TCHAR(AssetPath.c_str());
                        TArray<uint8> Compressed;
                        SkiPreparation::SiteContextStore Store(DataRoot);
                        if (Store.ReadAsset(*SiteContext, RelativePath, Compressed, Error)
                            && Compressed.Num() > 0
                            && static_cast<uint64>(Compressed.Num())
                                <= SkiPreparation::SiteContextPhotoTileMaxCompressedBytes
                            && !Mailbox->Cancelled.load(std::memory_order_acquire))
                        {
                            SkiPreparation::FSiteContextPhotoTile Decoded;
                            if (SkiPreparation::DecodeSiteContextPhotoTile(Compressed,
                                    Decoded, Error, [Mailbox]()
                                    {
                                        return Mailbox->Cancelled.load(std::memory_order_acquire);
                                    })
                                && Decoded.IsValid()
                                && !Mailbox->Cancelled.load(std::memory_order_acquire))
                            {
                                Pixels = MoveTemp(Decoded.Pixels);
                                bSucceeded = true;
                            }
                        }
                        else if (Error.IsEmpty() && !Mailbox->Cancelled.load(std::memory_order_acquire))
                        {
                            Error = TEXT("The exact-key imagery bytes are outside the compressed tile cap.");
                        }
                    }
                }

                if (Mailbox->Cancelled.load(std::memory_order_acquire)) return;
                FSkiInstalledPhotoCompletion Completion;
                Completion.RequestSerial = RequestSerial;
                Completion.Generation = Generation;
                Completion.Key = Key;
                Completion.bSucceeded = bSucceeded;
                Completion.Pixels = MoveTemp(Pixels);
                Completion.Error = MoveTemp(Error);
                AsyncTask(ENamedThreads::GameThread,
                    [Mailbox, Completion = MoveTemp(Completion)]() mutable
                    {
                        if (Mailbox->Cancelled.load(std::memory_order_acquire)) return;
                        FScopeLock Lock(&Mailbox->Mutex);
                        if (!Mailbox->Cancelled.load(std::memory_order_acquire))
                            Mailbox->Completions.Add(MoveTemp(Completion));
                    });
            });
    }
}

void ASkiBootstrapGameMode::CompleteInstalledPhotoTileRead(const uint64 RequestSerial,
    const uint64 Generation, const SkiApplication::TerrainCoreTileKey Key,
    const bool bSucceeded, TArray<FColor> Pixels, const FString& Error)
{
    if (!IsInGameThread()) return;
    const std::shared_ptr<FSkiInstalledPhotoStreamState> Stream = InstalledPhotoStream;
    if (!Stream || Stream->RequestSerial != RequestSerial || Stream->Generation != Generation
        || !Stream->Mailbox || Stream->Mailbox->Cancelled.load(std::memory_order_acquire)) return;

    Stream->InFlightReads = FMath::Max(0, Stream->InFlightReads - 1);
    uint64 CurrentGeneration = 0;
    TArray<SkiApplication::TerrainCoreTileKey> CurrentDesiredKeys;
    const bool bCurrentSnapshot = TerrainActor
        && TerrainActor->GetViewMode() == ESkiTerrainViewMode::Photo
        && TerrainActor->GetTerrainCorePhotoRequestSnapshot(CurrentGeneration, CurrentDesiredKeys);
    if (!bCurrentSnapshot || CurrentGeneration != Generation || !CurrentDesiredKeys.Contains(Key)
        || !MountainAcquisitionDeny || !MountainAcquisitionDeny->IsActive()
        || MountainAcquisitionDeny->ObservedTransportCalls() != 0)
    {
        return;
    }

    if (!bSucceeded || Pixels.Num() != static_cast<int32>(
            SkiPreparation::SiteContextPhotoTilePixels * SkiPreparation::SiteContextPhotoTilePixels)
        || !TerrainActor->SetTerrainCorePhotoTile(Generation, Key, MoveTemp(Pixels)))
    {
        if (!Stream->bReportedReadFailure)
        {
            Stream->bReportedReadFailure = true;
            if (P1Widget)
                P1Widget->SetTransientStatus(Error.IsEmpty()
                    ? TEXT("A verified photo tile no longer matched the active terrain view.")
                    : TEXT("A verified photo tile failed its integrity or decode check."));
        }
    }
    else
    {
        ++Stream->SubmittedTiles;
    }

    if (Stream->NextKeyIndex >= Stream->DesiredKeys.Num() && Stream->InFlightReads == 0)
    {
        if (!Stream->bReportedReadFailure && Stream->SubmittedTiles == Stream->DesiredKeys.Num()
            && P1Widget)
        {
            P1Widget->SetTransientStatus(FString::Printf(
                TEXT("Photo imagery loaded for %d current-view tiles."), Stream->SubmittedTiles));
        }
        return;
    }
    IssueInstalledPhotoTileReads();
}

void ASkiBootstrapGameMode::CancelInstalledPhotoPresentation()
{
    if (UWorld* World = GetWorld())
        World->GetTimerManager().ClearTimer(InstalledPhotoSelectionPollTimer);
    if (InstalledPhotoStream && InstalledPhotoStream->Mailbox)
        InstalledPhotoStream->Mailbox->Cancelled.store(true, std::memory_order_release);
    InstalledPhotoStream.reset();
    ++InstalledPhotoRequestSerial;
}

void ASkiBootstrapGameMode::ClearInstalledPhotoContext()
{
    CancelInstalledPhotoPresentation();
    InstalledPhotoSiteContext.reset();
    InstalledPhotoDataRoot.Empty();
    if (P1Widget) P1Widget->SetPhotoCommandAvailable(false);
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
    ClearInstalledPhotoContext();
    ++InstalledOpenGeneration;
    ++MountainPrepareGeneration;
    PendingInstalledOpenId.Empty();
    DeferredInstalledOpenId.Empty();
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
    if (GetWorld()->GetOutermost()->GetName() == TEXT("/Game/P1Generated/P1Terrain"))
    {
        UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/P0Generated/Bootstrap")));
        return;
    }
    MountainAcquisitionDeny.Reset();
    RefreshInstalledLibrary();
    if (P1Widget) P1Widget->ResetSelector();
}
