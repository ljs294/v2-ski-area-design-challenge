#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "SkiBootstrapGameMode.h"
#include "SkiP1Widget.h"
#include "SkiPreparation/CoverEcologyStore.h"

#include <algorithm>
#include <string>

FString ComposeInstalledTerrainDetails(const FString& ExistingDetails,
    std::uint32_t InstallationSchema,
    const SkiPreparation::SiteContextManifest& SiteContext,
    const SkiDomain::TerrainQualityReport& Quality);

namespace
{
std::string PresentationTestUtf8(const FString& Value)
{
    const FTCHARToUTF8 Converted(*Value);
    return std::string(Converted.Get(), Converted.Length());
}

SkiPreparation::CompositeInstallReceipt MakePresentationCompositeReceipt()
{
    using namespace SkiPreparation;
    CompositeInstallReceipt Receipt;
    Receipt.GeneratorVersion = "presentation-handoff-test-v1";
    Receipt.ProvenanceCounts.S1MNativeQualified = 1;
    SkiDomain::TrySummarizeTerrainQuality(Receipt.ProvenanceCounts, Receipt.Quality);
    const std::string QualityId = PresentationTestUtf8(ComputeTerrainQualityReportId(
        Receipt.ProvenanceCounts, Receipt.Quality));
    Receipt.Components = {
        {CompositeInstallComponentKind::TerrainCore, CompositeInstallComponentStatus::Verified,
            std::string(64, '1'), std::string(64, 'a')},
        {CompositeInstallComponentKind::CoverEcology, CompositeInstallComponentStatus::Verified,
            std::string(64, '2'), std::string(64, 'b')},
        {CompositeInstallComponentKind::SiteContext, CompositeInstallComponentStatus::Verified,
            std::string(64, '3'), std::string(64, 'c')},
        {CompositeInstallComponentKind::QualityReport, CompositeInstallComponentStatus::Verified,
            QualityId, QualityId},
    };
    Receipt.ContentId = PresentationTestUtf8(ComputeCompositeInstallReceiptId(Receipt));
    return Receipt;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1ResponsiveUiLayoutTest,
    "MountainPlanner.P1.Presentation.UI.ResponsiveLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1ResponsiveUiLayoutTest::RunTest(const FString&)
{
    struct FCase { FIntPoint Viewport; double ExpectedPanelWidth; };
    const FCase Cases[]{{{1280,720},520},{{1920,1080},520},{{2560,1080},520},
        {{2560,1440},520},{{576,1024},520}};
    for (const FCase& Value : Cases)
    {
        const double PanelWidth = USkiP1Widget::CalculateStatusPanelWidth(Value.Viewport.X);
        TestEqual(FString::Printf(TEXT("Panel width at %dx%d"), Value.Viewport.X, Value.Viewport.Y),
            PanelWidth, Value.ExpectedPanelWidth);
        TestTrue(FString::Printf(TEXT("Panel leaves viewport margin at %dx%d"),
            Value.Viewport.X, Value.Viewport.Y), PanelWidth <= Value.Viewport.X - 48.0);
        const FVector2D Selector = USkiP1Widget::CalculateSelectorPanelSize(Value.Viewport);
        TestTrue(FString::Printf(TEXT("Selector fits viewport at %dx%d"),
            Value.Viewport.X, Value.Viewport.Y), Selector.X <= Value.Viewport.X - 48.0
                && Selector.Y <= Value.Viewport.Y - 48.0);
        TestTrue(TEXT("Selector retains a usable minimum"), Selector.X >= 280.0
            && Selector.Y >= 320.0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1SitePickerPanelLayoutTest,
    "MountainPlanner.P1.Presentation.UI.SitePickerPanelLayout",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1SitePickerPanelLayoutTest::RunTest(const FString&)
{
    struct FCase { FIntPoint Viewport; FVector2D ExpectedPanel; };
    const FCase Cases[]{{{1280, 720}, {440.0, 672.0}},
        {{1920, 1080}, {440.0, 740.0}}, {{2560, 1080}, {440.0, 740.0}},
        {{2560, 1440}, {440.0, 740.0}}, {{576, 1024}, {440.0, 740.0}}};

    for (const FCase& Value : Cases)
    {
        const FVector2D Panel = USkiP1Widget::CalculateSitePickerPanelSize(Value.Viewport);
        const FString Label = FString::Printf(TEXT("Site picker panel at %dx%d"),
            Value.Viewport.X, Value.Viewport.Y);
        TestEqual(Label + TEXT(" width"), Panel.X, Value.ExpectedPanel.X);
        TestEqual(Label + TEXT(" height"), Panel.Y, Value.ExpectedPanel.Y);
        TestTrue(Label + TEXT(" has positive layout space"), Panel.X > 0.0 && Panel.Y > 0.0);
        TestTrue(Label + TEXT(" stays within the 24 px viewport inset"),
            Panel.X <= Value.Viewport.X - 48.0 && Panel.Y <= Value.Viewport.Y - 48.0);
        TestTrue(Label + TEXT(" retains the minimum planned outer panel height"),
            Panel.Y >= 672.0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1PickerViewportScrollReachabilityTest,
    "MountainPlanner.P1.Presentation.SitePicker.ZeroOverflowIsAlreadyAtEnd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1PickerViewportScrollReachabilityTest::RunTest(const FString&)
{
    TestTrue(TEXT("A layout with no scroll extent is already at its end"),
        ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(0.0F, 0.0F));
    TestTrue(TEXT("A sub-pixel scroll extent is within the end tolerance"),
        ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(0.0F, 0.5F));
    TestFalse(TEXT("A nonzero scroll extent must be reached"),
        ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(0.0F, 20.0F));
    TestTrue(TEXT("The end tolerance allows small layout rounding differences"),
        ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(19.5F, 20.0F));
    TestFalse(TEXT("A real overflow cannot be mistaken for the end"),
        ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(18.0F, 20.0F));
    TestFalse(TEXT("Negative scroll extents are invalid"),
        ASkiBootstrapGameMode::IsPickerViewportScrollAtEnd(0.0F, -1.0F));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1SitePickerCancellationTokenTest,
    "MountainPlanner.P1.Presentation.SitePicker.CancellationToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1SitePickerCancellationTokenTest::RunTest(const FString&)
{
    SkiPreparation::Cancellation Cancellation;
    TestFalse(TEXT("A new search cancellation token starts active"), Cancellation.IsCancelled());
    Cancellation.Cancel();
    TestTrue(TEXT("Cancel marks the token"), Cancellation.IsCancelled());
    Cancellation.Cancel();
    TestTrue(TEXT("Repeated cancellation remains cancelled"), Cancellation.IsCancelled());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1InstalledTerrainMountainHandoffTest,
    "MountainPlanner.P1.Presentation.InstalledTerrain.VerifiedMountainHandoff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1InstalledTerrainMountainHandoffTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    FString TerrainCoreId;
    FString CoverEcologyId;
    const FString LegacyId = FString::ChrN(64, 'a');
    const FString LegacyCoreId = FString::ChrN(64, 'b');
    const FString LegacySurroundId = FString::ChrN(64, 'c');
    const FString LegacyCoverId = FString::ChrN(64, 'd');
    InstalledTerrainIndex Legacy;
    Legacy.SchemaVersion = SkiDomain::InstalledTerrainSchema;
    Legacy.Receipt.SchemaVersion = SkiDomain::InstalledTerrainSchema;
    Legacy.Receipt.ContentId = PresentationTestUtf8(LegacyId);
    Legacy.Receipt.GeneratorVersion = "legacy-handoff-test-v2";
    Legacy.Receipt.TerrainCoreId = PresentationTestUtf8(LegacyCoreId);
    Legacy.Receipt.SurroundTerrainCoreId = PresentationTestUtf8(LegacySurroundId);
    Legacy.Receipt.CoverEcologyId = PresentationTestUtf8(LegacyCoverId);

    TestFalse(TEXT("Schema-2 cannot transition before store verification"),
        ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(LegacyId,
            Legacy, false, TerrainCoreId, CoverEcologyId));
    TestTrue(TEXT("Verified schema-2 legacy receipt resolves its legacy component references"),
        ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(LegacyId,
            Legacy, true, TerrainCoreId, CoverEcologyId));
    TestEqual(TEXT("Schema-2 primary component is preserved"), TerrainCoreId, LegacyCoreId);
    TestEqual(TEXT("Schema-2 cover component is preserved"), CoverEcologyId, LegacyCoverId);

    InstalledTerrainIndex Composite;
    Composite.SchemaVersion = CompositeInstallReceiptSchema;
    Composite.CompositeReceipt = MakePresentationCompositeReceipt();
    const FString CompositeId = UTF8_TO_TCHAR(Composite.CompositeReceipt.ContentId.c_str());
    TestFalse(TEXT("Schema-3 cannot transition before store verification"),
        ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(CompositeId,
            Composite, false, TerrainCoreId, CoverEcologyId));
    TestTrue(TEXT("Verified complete schema-3 composite resolves its required components"),
        ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(CompositeId,
            Composite, true, TerrainCoreId, CoverEcologyId));
    TestEqual(TEXT("Schema-3 TerrainCore component is selected"), TerrainCoreId,
        FString::ChrN(64, '1'));
    TestEqual(TEXT("Schema-3 CoverEcology component is selected"), CoverEcologyId,
        FString::ChrN(64, '2'));

    Composite.CompositeReceipt.Components.erase(std::remove_if(
        Composite.CompositeReceipt.Components.begin(), Composite.CompositeReceipt.Components.end(),
        [](const CompositeInstallComponent& Component)
        { return Component.Kind == CompositeInstallComponentKind::SiteContext; }),
        Composite.CompositeReceipt.Components.end());
    TestFalse(TEXT("Partial schema-3 receipt is not eligible for Mountain handoff"),
        ASkiBootstrapGameMode::ResolveVerifiedInstalledTerrainComponents(CompositeId,
            Composite, true, TerrainCoreId, CoverEcologyId));
    TestTrue(TEXT("Rejected composite clears previously resolved package IDs"),
        TerrainCoreId.IsEmpty() && CoverEcologyId.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1InstalledSiteContextSummaryTest,
    "MountainPlanner.P1.Presentation.InstalledTerrain.SiteContextSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1InstalledSiteContextSummaryTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    SiteContextManifest SiteContext;
    SiteContext.ImageryTiles.resize(24);
    SiteContext.Attributions = {
        {"USGS", "Public Domain", "USGS imagery"},
        {"OpenStreetMap contributors", "ODbL-1.0", "© OpenStreetMap contributors"},
    };
    SiteContext.VectorSource.Provider = "openstreetmap-overpass";
    SiteContext.VectorSource.Endpoint = "https://overpass-api.de/api/interpreter";
    SiteContext.VectorSource.SourceTimestampUtc = "2026-09-23T10:11:12Z";
    SiteContext.VectorSource.RetrievedAtUtc = "2026-09-24T13:14:15Z";
    SiteContext.VectorSource.License = "ODbL-1.0";
    SiteContext.Assets.push_back({SiteContext.VectorAssetPath,
        SiteContextVectorEncodingV2, std::string(64, 'a'), 8192});

    SkiDomain::TerrainQualityReport Quality;
    Quality.Grade = SkiDomain::TerrainGrade::B;
    Quality.SourceMix = {0.75, 0.20, 0.05, false, false};
    Quality.UnknownMetadataFraction = 0.12;
    Quality.NoDataFraction = 0.01;

    const FString ExistingDetails = TEXT("Installed terrain | reopened offline\nLegacy fields");
    const FString LegacyDetails = ComposeInstalledTerrainDetails(ExistingDetails,
        SkiDomain::InstalledTerrainSchema, SiteContext, Quality);
    TestEqual(TEXT("Schema-2 legacy detail text is unchanged"), LegacyDetails, ExistingDetails);

    const FString Summary = ComposeInstalledTerrainDetails(ExistingDetails,
        CompositeInstallReceiptSchema, SiteContext, Quality);
    TestTrue(TEXT("Schema-3 metadata follows the existing terrain details"),
        Summary.StartsWith(ExistingDetails + TEXT("\n\nSite context metadata")));
    TestTrue(TEXT("Verified imagery tile count is shown"),
        Summary.Contains(TEXT("24 verified tiles")));
    TestTrue(TEXT("All imagery and vector attributions are shown"),
        Summary.Contains(TEXT("USGS imagery"))
            && Summary.Contains(TEXT("© OpenStreetMap contributors"))
            && Summary.Contains(TEXT("ODbL-1.0")));
    TestTrue(TEXT("OSM acquisition lineage and UTC timestamps are shown"),
        Summary.Contains(TEXT("openstreetmap-overpass"))
            && Summary.Contains(TEXT("https://overpass-api.de/api/interpreter"))
            && Summary.Contains(TEXT("2026-09-23T10:11:12Z"))
            && Summary.Contains(TEXT("2026-09-24T13:14:15Z")));
    TestTrue(TEXT("The installed OSM feature asset path and length are shown"),
        Summary.Contains(TEXT("vectors/osm-enu-polylines.json"))
            && Summary.Contains(TEXT("8192 bytes")));
    TestTrue(TEXT("Grade, source mix, unknown metadata, and NoData are shown"),
        Summary.Contains(TEXT("Grade B"))
            && Summary.Contains(TEXT("S1M 75.0%"))
            && Summary.Contains(TEXT("Project 1 m 20.0%"))
            && Summary.Contains(TEXT("1/3 arc-second 5.0%"))
            && Summary.Contains(TEXT("unknown metadata 12.0%"))
            && Summary.Contains(TEXT("NoData 1.0%")));
    TestTrue(TEXT("Photo presentation is described as bounded on-demand view streaming"),
        Summary.Contains(TEXT("Photo presentation: verified tiles load on demand for the current terrain view")));
    TestTrue(TEXT("OSM remains a separately identified vector presentation gap"),
        Summary.Contains(TEXT("Vector presentation gap")));
    TestFalse(TEXT("The summary does not claim spacing is accuracy"),
        Summary.Contains(TEXT("accuracy"), ESearchCase::IgnoreCase));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FP1InstalledPhotoCommandSchemaGateTest,
    "MountainPlanner.P1.Presentation.InstalledTerrain.PhotoCommandSchemaGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FP1InstalledPhotoCommandSchemaGateTest::RunTest(const FString&)
{
    TestFalse(TEXT("Schema-2 legacy installs do not expose Photo even if metadata is present"),
        USkiP1Widget::ShouldShowPhotoCommandForInstallation(2U, true));
    TestFalse(TEXT("Schema-3 without a verified SiteContext cannot expose Photo"),
        USkiP1Widget::ShouldShowPhotoCommandForInstallation(3U, false));
    TestTrue(TEXT("Only a verified schema-3 SiteContext enables Photo"),
        USkiP1Widget::ShouldShowPhotoCommandForInstallation(3U, true));
    TestFalse(TEXT("Unknown future receipt schemas remain closed by default"),
        USkiP1Widget::ShouldShowPhotoCommandForInstallation(4U, true));
    return true;
}

#endif
