#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiPreparation/NativeTerrainProvider.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeGatewayDefaultTest,
    "Ski.P1.Preparation.Gateway.ProviderDefaultDeny",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeGatewayDefaultTest::RunTest(const FString&)
{
    const FString Root = FPaths::Combine(FPaths::ProjectIntermediateDir(),
        TEXT("NativeGatewayDefault"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    SkiPreparation::NativeTerrainProvider Provider(Root);
    SkiPreparation::Request Request;
    Request.Name = TEXT("Gateway default policy probe");
    Request.Bounds = {-121.4875, 46.9260, -121.4605, 46.9440};
    Request.Profile = SkiPreparation::SourceProfile::Medium;
    Request.SessionGeneration = 1;
    Request.OperationGeneration = 1;
    const SkiPreparation::Result Result = Provider.Prepare(Request,
        MakeShared<SkiPreparation::Cancellation>(), {});
    TestFalse(TEXT("Unsupported legacy elevation endpoint cannot acquire"), Result.Ok);
    TestTrue(TEXT("Provider returns a structured acquisition failure"), Result.Failure.IsSet());
    if (Result.Failure.IsSet())
    {
        TestEqual(TEXT("Default gateway rejects the host before network handle creation"),
            Result.Failure->RequestStatus, FString(TEXT("INVALID_URL")));
        TestEqual(TEXT("Failure identifies core elevation"), Result.Failure->Product,
            SkiPreparation::ProviderProduct::CoreElevation);
    }
    return true;
}

#endif
