#include "SkiPreparation/S1mXmlReader.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

namespace
{
TArray<uint8> ToUtf8(const FString& Text)
{
    FTCHARToUTF8 Converted(*Text);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
    return Bytes;
}

SkiPreparation::FS1mLineageArtifactEvidence ArtifactFor(const TArray<uint8>& Bytes)
{
    SkiPreparation::FS1mLineageArtifactEvidence Artifact;
    Artifact.Product = TEXT("S1M");
    Artifact.TileId = TEXT("n4420w07120");
    Artifact.PublicationDate = TEXT("2026-06-16");
    Artifact.ObjectBytes = static_cast<uint64>(Bytes.Num());
    Artifact.bExactSizeProven = !Bytes.IsEmpty();
    return Artifact;
}

void ExpectFailClosed(FAutomationTestBase& Test, const FString& Label, const TArray<uint8>& Bytes,
    const TCHAR* ExpectedCode, const SkiPreparation::FS1mLineageLimits& Limits = {})
{
    SkiPreparation::FS1mXmlSidecarEvidence Evidence;
    FString FailureCode;
    FString FailureDetail;
    const bool bAccepted = SkiPreparation::PreflightS1mXmlSidecar(Bytes, ArtifactFor(Bytes),
        Limits, Evidence, FailureCode, FailureDetail);
    Test.TestFalse(*FString::Printf(TEXT("%s is not accepted as XML evidence"), *Label), bAccepted);
    Test.TestEqual(*FString::Printf(TEXT("%s has an explicit failure code"), *Label),
        FailureCode, FString(ExpectedCode));
    Test.TestFalse(*FString::Printf(TEXT("%s does not claim well-formed XML"), *Label), Evidence.bWellFormed);
    Test.TestFalse(*FString::Printf(TEXT("%s does not claim a horizontal CRS"), *Label), Evidence.bHorizontalCrsExplicit);
    Test.TestFalse(*FString::Printf(TEXT("%s does not claim NAVD88"), *Label), Evidence.bVerticalDatumExplicit);
    Test.TestFalse(*FString::Printf(TEXT("%s does not claim RMSE"), *Label), Evidence.bVerticalRmseExplicit);
    Test.TestFalse(*FString::Printf(TEXT("%s does not claim a tile footprint"), *Label), Evidence.bTileFootprintExplicit);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mXmlReaderSafetyPreflightTest,
    "MountainPlanner.M3.S1mXmlReader.BoundedSafetyPreflight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mXmlReaderSafetyPreflightTest::RunTest(const FString&)
{
    using namespace SkiPreparation;

    ExpectFailClosed(*this, TEXT("DTD and external entity declaration"), ToUtf8(
        TEXT("<!DOCTYPE metadata [<!ENTITY external SYSTEM 'file:///private/secret'>]><metadata/>")),
        TEXT("S1M_XML_DTD_FORBIDDEN"));
    ExpectFailClosed(*this, TEXT("unresolved named entity"), ToUtf8(
        TEXT("<metadata> &external; </metadata>")), TEXT("S1M_XML_ENTITY_UNSUPPORTED"));

    FS1mLineageLimits SmallLimit;
    SmallLimit.MaxXmlBytes = 16;
    ExpectFailClosed(*this, TEXT("oversized XML"), ToUtf8(TEXT("<metadata>more than sixteen bytes</metadata>")),
        TEXT("S1M_XML_SIZE_INVALID"), SmallLimit);

    const TArray<uint8> BoundedBytes = ToUtf8(TEXT("<metadata/>"));
    FS1mLineageArtifactEvidence WrongSizeArtifact = ArtifactFor(BoundedBytes);
    ++WrongSizeArtifact.ObjectBytes;
    FS1mXmlSidecarEvidence SizeEvidence;
    FString SizeFailureCode;
    FString SizeFailureDetail;
    TestFalse(TEXT("claimed object size must equal the raw buffer"), PreflightS1mXmlSidecar(BoundedBytes,
        WrongSizeArtifact, FS1mLineageLimits(), SizeEvidence, SizeFailureCode, SizeFailureDetail));
    TestEqual(TEXT("size mismatch has an explicit failure code"), SizeFailureCode,
        FString(TEXT("S1M_XML_ARTIFACT_SIZE_UNPROVEN")));

    FString DeepXml;
    for (int32 Index = 0; Index <= 64; ++Index) DeepXml += TEXT("<x>");
    for (int32 Index = 0; Index <= 64; ++Index) DeepXml += TEXT("</x>");
    ExpectFailClosed(*this, TEXT("deeply nested XML"), ToUtf8(DeepXml), TEXT("S1M_XML_DEPTH_LIMIT"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FS1mXmlReaderUnpinnedSchemaTest,
    "MountainPlanner.M3.S1mXmlReader.UnpinnedSchemaFailsClosed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FS1mXmlReaderUnpinnedSchemaTest::RunTest(const FString&)
{
    using namespace SkiPreparation;
    const TArray<uint8> MissingProof = ToUtf8(TEXT("<metadata/>"));
    ExpectFailClosed(*this, TEXT("XML without required lineage proof"), MissingProof,
        TEXT("S1M_XML_SCHEMA_UNPINNED"));

    // This FGDC-shaped fixture uses the collection title from the official example;
    // it cannot supply per-tile identity, EPSG:6350 footprint, NAVD88, or RMSE proof.
    const TArray<uint8> CollectionMetadata = ToUtf8(TEXT(
        "<metadata><idinfo><citation><citeinfo><title>1 meter Digital Elevation Models (DEMs) - "
        "USGS National Map 3DEP Downloadable Data Collection</title></citeinfo></citation></idinfo></metadata>"));
    ExpectFailClosed(*this, TEXT("collection-level FGDC metadata"), CollectionMetadata,
        TEXT("S1M_XML_SCHEMA_UNPINNED"));

    FS1mXmlSidecarEvidence Evidence;
    FString FailureCode;
    FString FailureDetail;
    const FS1mLineageArtifactEvidence Artifact = ArtifactFor(MissingProof);
    TestFalse(TEXT("bounded but unpinned XML is not accepted"), PreflightS1mXmlSidecar(MissingProof,
        Artifact, FS1mLineageLimits(), Evidence, FailureCode, FailureDetail));
    TestTrue(TEXT("exact caller identity and byte count are carried without semantic claims"),
        Evidence.Artifact.bExactSizeProven && Evidence.Artifact.ObjectBytes == static_cast<uint64>(MissingProof.Num())
        && Evidence.Artifact.TileId == Artifact.TileId && Evidence.Artifact.PublicationDate == Artifact.PublicationDate);
    return true;
}
#endif
