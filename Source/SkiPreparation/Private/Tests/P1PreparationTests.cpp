#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "SkiApplication/TerrainSession.h"
#include "SkiPreparation/FixtureTerrainProvider.h"
#include "SkiPreparation/SelectorProtocol.h"
#include "SkiPreparation/TerrainPackageStore.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1PreparationContractTest,
    "MountainPlanner.P1.Preparation.PackageAndProtocol",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1PreparationContractTest::RunTest(const FString&)
{
    const FString Token = TEXT("13a15f75-5ba7-4ac0-bd1c-fbe114a6843a");
    const FString Valid = FString::Printf(TEXT("{\"token\":\"%s\",\"generation\":7,\"name\":\"Crystal\",\"profile\":\"standard\",\"west\":-121.49,\"south\":46.92,\"east\":-121.46,\"north\":46.95}"), *Token);
    SkiPreparation::Request Request;
    FString Error;
    TestTrue(TEXT("Tokened selector request validates"),
        SkiPreparation::ValidateSelectorMessage(Valid, Token, 7, Request, Error));
    TestFalse(TEXT("Stale selector generation rejected"),
        SkiPreparation::ValidateSelectorMessage(Valid, Token, 8, Request, Error));
    TestFalse(TEXT("Selector rejects added capability fields"),
        SkiPreparation::ValidateSelectorMessage(Valid.LeftChop(1) + TEXT(",\"path\":\"C:/escape\"}"), Token, 7, Request, Error));

    Request = {};
    Request.Name = TEXT("Crystal synthetic");
    Request.Bounds = {-121.49, 46.92, -121.46, 46.95};
    Request.Profile = SkiPreparation::SourceProfile::Standard;
    Request.SessionGeneration = 1;
    Request.OperationGeneration = 1;
    const FString Root = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("P1Tests"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*Root, true);
    const TSharedRef<SkiPreparation::Cancellation> Cancellation = MakeShared<SkiPreparation::Cancellation>();
    SkiPreparation::FixtureTerrainProvider Provider(Root);
    SkiPreparation::Result Result = Provider.Prepare(Request, Cancellation, {});
    TestTrue(TEXT("Fixture writes, verifies, and activates"), Result.Ok);
    TestEqual(TEXT("Fixture manifest includes required and explicit optional assets"),
        static_cast<int32>(Result.Manifest.Assets.size()), 7);
    TestEqual(TEXT("Verified cover grid is returned for runtime presentation"),
        Result.Cover.Num(), static_cast<int32>(Result.Manifest.CoverWidth * Result.Manifest.CoverHeight));

    SkiApplication::TerrainSession Session;
    std::vector<std::uint8_t> Cover(Result.Cover.GetData(), Result.Cover.GetData() + Result.Cover.Num());
    TestTrue(TEXT("Verified package installs"), Session.Install(std::move(Result.Heightfield),
        Result.Manifest, std::move(Cover)));
    const SkiApplication::TerrainSnapshot Before = Session.Snapshot();
    TestTrue(TEXT("Installed snapshot exposes immutable cover"), Before.Cover != nullptr);
    SkiDomain::MutationBounds Bounds;
    TestFalse(TEXT("Stale edit is rejected"), Session.ApplyScratchMutation(99, 0, 0, 50, 2, Bounds));
    TestTrue(TEXT("Expected revision edit succeeds"), Session.ApplyScratchMutation(
        Before.Readiness.Canonical, 0, 0, 50, 2, Bounds));
    const SkiApplication::TerrainSnapshot After = Session.Snapshot();
    TestTrue(TEXT("Edit advances canonical revision"), After.Readiness.Canonical > Before.Readiness.Canonical);
    TestFalse(TEXT("Readiness is withheld until render/query acknowledge"), After.Readiness.IsReady());
    TestTrue(TEXT("Render acknowledges edited revision"), Session.AcknowledgeRender(After.Readiness.Canonical));
    TestTrue(TEXT("Query acknowledges edited revision"), Session.AcknowledgeQuery(After.Readiness.Canonical));
    TestTrue(TEXT("Edited terrain becomes ready"), Session.Snapshot().Readiness.IsReady());

    const FString Resolved = FPaths::ConvertRelativePathToFull(Root);
    if (Resolved.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir())))
    {
        IFileManager::Get().DeleteDirectory(*Resolved, false, true);
    }
    return true;
}

#endif
