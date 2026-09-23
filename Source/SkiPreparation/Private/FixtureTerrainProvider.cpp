#include "SkiPreparation/FixtureTerrainProvider.h"

#include "SkiPreparation/CoverEcologyStore.h"
#include "SkiPreparation/TerrainCorePackageStore.h"

#include <cmath>

SkiPreparation::FixtureTerrainProvider::FixtureTerrainProvider(FString InDataRoot)
    : DataRoot(std::move(InDataRoot))
{
}

SkiPreparation::Result SkiPreparation::FixtureTerrainProvider::Prepare(const Request& RequestValue,
    const TSharedRef<Cancellation>& CancellationValue, const ProgressCallback& OnProgress)
{
    Result Output;
    const double Began = FPlatformTime::Seconds();
    const auto OperationCurrent = [&]()
    {
        return !CancellationValue->IsCancelled()
            && (!RequestValue.Lease || RequestValue.Lease->IsCurrent(
                RequestValue.SessionGeneration, RequestValue.OperationGeneration));
    };
    auto Report = [&](const State Phase, const uint64 Completed, const uint64 Total, const TCHAR* Detail)
    {
        if (OnProgress && OperationCurrent())
            OnProgress({Phase, Completed, Total, FPlatformTime::Seconds() - Began, Detail});
    };
    const auto Cancel = [&]()
    {
        Output = {};
        Output.FinalState = State::Cancelled;
        Output.Error = TEXT("Fixture preparation cancelled or superseded.");
    };
    Report(State::Validating, 0, 6, TEXT("Validating fixture request"));
    if (!ValidateRequest(RequestValue, Output.Error)) return Output;
    if (!OperationCurrent())
    {
        Cancel();
        return Output;
    }
    Report(State::Deriving, 1, 6, TEXT("Generating deterministic asymmetric terrain and cover"));
    const uint32 Dimension = RequestValue.Profile == SourceProfile::Medium ? 513U : 257U;
    const double CenterLatitude = (RequestValue.Bounds.SouthDeg + RequestValue.Bounds.NorthDeg) / 2.0;
    const double CenterLongitude = (RequestValue.Bounds.WestDeg + RequestValue.Bounds.EastDeg) / 2.0;
    constexpr double MetersPerDegree = 111320.0;
    const double WidthM = (RequestValue.Bounds.EastDeg - RequestValue.Bounds.WestDeg)
        * MetersPerDegree * std::cos(CenterLatitude * 3.14159265358979323846 / 180.0);
    const double HeightM = (RequestValue.Bounds.NorthDeg - RequestValue.Bounds.SouthDeg) * MetersPerDegree;
    SkiDomain::Heightfield Field;
    Field.Width = Dimension;
    Field.Height = Dimension;
    Field.WestM = -WidthM / 2.0;
    Field.NorthM = HeightM / 2.0;
    Field.EastSpacingM = WidthM / static_cast<double>(Dimension - 1);
    Field.NorthSpacingM = HeightM / static_cast<double>(Dimension - 1);
    Field.CurrentRevision = 1;
    Field.Samples.resize(static_cast<size_t>(Dimension) * Dimension);
    for (uint32 Row = 0; Row < Dimension; ++Row)
    {
        if (!OperationCurrent())
        {
            Cancel();
            return Output;
        }
        for (uint32 Column = 0; Column < Dimension; ++Column)
        {
            const double X = (static_cast<double>(Column) / (Dimension - 1) - 0.5) * 2.0;
            const double Y = (static_cast<double>(Row) / (Dimension - 1) - 0.5) * 2.0;
            Field.Samples[static_cast<size_t>(Row) * Dimension + Column] = static_cast<float>(
                1450.0 + 420.0 * (1.0 - Y) + 140.0 * std::sin(4.0 * X + 0.7 * Y)
                + 60.0 * std::cos(7.0 * Y - X));
        }
    }
    SkiDomain::TerrainManifest Manifest;
    Manifest.Name = TCHAR_TO_UTF8(*RequestValue.Name);
    Manifest.Source = "synthetic-p1-fixture";
    Manifest.RequestedAtUtc = "2026-09-22T00:00:00Z";
    Manifest.RequestedBounds = RequestValue.Bounds;
    Manifest.ActualBounds = RequestValue.Bounds;
    Manifest.LocalOrigin = {CenterLatitude, CenterLongitude, Field.Samples[(Dimension / 2) * Dimension + Dimension / 2]};
    Manifest.HeightWidth = Dimension;
    Manifest.HeightHeight = Dimension;
    Manifest.CoverWidth = Dimension;
    Manifest.CoverHeight = Dimension;
    Manifest.EastSpacingM = Field.EastSpacingM;
    Manifest.NorthSpacingM = Field.NorthSpacingM;
    Manifest.VerticalDatum = "synthetic-local";

    TArray<uint8> Cover;
    TArray<uint8> CoverValidity;
    Cover.SetNumUninitialized(static_cast<int32>(Field.Samples.size()));
    CoverValidity.Init(1, Cover.Num());
    for (int32 Index = 0; Index < Cover.Num(); ++Index)
    {
        const float Height = Field.Samples[static_cast<size_t>(Index)];
        Cover[Index] = Height > 1850.0F ? 70 : Height > 1650.0F ? 30 : 10;
    }

    SkiDomain::TerrainCoreManifest TerrainCore;
    TerrainCore.GeneratorVersion = "mountain-planner-terraincore-v2";
    TerrainCore.ProcessingVersions = {
        "deterministic-p1-fixture-v2", "terraincore-derivation-v1"};
    TerrainCore.LocalOrigin = Manifest.LocalOrigin;
    TerrainCore.Width = Field.Width;
    TerrainCore.Height = Field.Height;
    TerrainCore.DeliveredEastSpacingM = Field.EastSpacingM;
    TerrainCore.DeliveredNorthSpacingM = Field.NorthSpacingM;
    TerrainCore.Registration = SkiDomain::PixelRegistration::SampleCenter;
    TerrainCore.SampleCenterBounds = {Field.WestM,
        Field.SampleNorthM(Field.Height - 1U), Field.EastM(Field.Width - 1U), Field.NorthM};
    if (!SkiDomain::ComputeTerrainCoreBounds(TerrainCore.Width, TerrainCore.Height,
            TerrainCore.DeliveredEastSpacingM, TerrainCore.DeliveredNorthSpacingM,
            TerrainCore.SampleCenterBounds, TerrainCore.OuterBounds))
    {
        Output.Error = TEXT("Fixture TerrainCore bounds are invalid.");
        return Output;
    }
    TerrainCore.Source.SourceId = "synthetic-p1-fixture-ground";
    TerrainCore.Source.Product = "Deterministic synthetic bare-earth fixture";
    TerrainCore.Source.AcquisitionEpoch = Manifest.RequestedAtUtc;
    TerrainCore.Source.HorizontalCrs = "WGS84/local-ENU";
    TerrainCore.Source.HorizontalDatum = "WGS84";
    TerrainCore.Source.VerticalDatum = Manifest.VerticalDatum;
    TerrainCore.Source.License = "repository test fixture";
    TerrainCore.Source.Attribution = "Mountain Planner deterministic fixture";
    TerrainCore.Source.NativeEastSpacingM = Field.EastSpacingM;
    TerrainCore.Source.NativeNorthSpacingM = Field.NorthSpacingM;

    // Required surround: a coarse synthetic ring 3 km beyond the selection, installed as its
    // own TerrainCore in the same local frame as the core.
    constexpr uint32 SurroundDimension = 129U;
    constexpr double SurroundMarginM = 3000.0;
    SkiDomain::Heightfield Surround;
    Surround.Width = SurroundDimension;
    Surround.Height = SurroundDimension;
    Surround.WestM = Field.WestM - SurroundMarginM;
    Surround.NorthM = Field.NorthM + SurroundMarginM;
    Surround.EastSpacingM = (WidthM + 2.0 * SurroundMarginM) / (SurroundDimension - 1);
    Surround.NorthSpacingM = (HeightM + 2.0 * SurroundMarginM) / (SurroundDimension - 1);
    Surround.CurrentRevision = 1;
    Surround.Samples.resize(static_cast<size_t>(SurroundDimension) * SurroundDimension);
    for (uint32 Row = 0; Row < SurroundDimension; ++Row)
    {
        for (uint32 Column = 0; Column < SurroundDimension; ++Column)
        {
            const double East = Surround.WestM + Column * Surround.EastSpacingM;
            const double North = Surround.NorthM - Row * Surround.NorthSpacingM;
            const double X = East / (WidthM * 0.5);
            const double Y = -North / (HeightM * 0.5);
            Surround.Samples[static_cast<size_t>(Row) * SurroundDimension + Column] = static_cast<float>(
                1450.0 + 420.0 * (1.0 - FMath::Clamp(Y, -1.8, 1.8)) + 140.0 * std::sin(4.0 * X + 0.7 * Y)
                + 60.0 * std::cos(7.0 * Y - X));
        }
    }
    SkiDomain::TerrainCoreManifest SurroundCore = TerrainCore;
    SurroundCore.Width = Surround.Width;
    SurroundCore.Height = Surround.Height;
    SurroundCore.DeliveredEastSpacingM = Surround.EastSpacingM;
    SurroundCore.DeliveredNorthSpacingM = Surround.NorthSpacingM;
    SurroundCore.SampleCenterBounds = {Surround.WestM,
        Surround.SampleNorthM(Surround.Height - 1U), Surround.EastM(Surround.Width - 1U), Surround.NorthM};
    SurroundCore.Source.SourceId = "synthetic-p1-fixture-surround";
    SurroundCore.Source.Product = "Deterministic synthetic surrounding elevation";
    SurroundCore.Source.NativeEastSpacingM = Surround.EastSpacingM;
    SurroundCore.Source.NativeNorthSpacingM = Surround.NorthSpacingM;
    if (!SkiDomain::ComputeTerrainCoreBounds(SurroundCore.Width, SurroundCore.Height,
            SurroundCore.DeliveredEastSpacingM, SurroundCore.DeliveredNorthSpacingM,
            SurroundCore.SampleCenterBounds, SurroundCore.OuterBounds))
    {
        Output.Error = TEXT("Fixture surround bounds are invalid.");
        return Output;
    }

    Report(State::WritingStaging, 2, 6,
        TEXT("Writing fixture TerrainCore and CoverEcology staging"));
    if (!OperationCurrent())
    {
        Cancel();
        return Output;
    }
    TerrainCorePackageStore CoreStore(DataRoot);
    FString CoreDirectory;
    SkiDomain::TerrainCoreManifest InstalledCore;
    if (!CoreStore.WriteAndActivate(TerrainCore, Field, CoreDirectory,
            InstalledCore, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        if (!OperationCurrent()) Cancel();
        return Output;
    }
    FString SurroundDirectory;
    SkiDomain::TerrainCoreManifest InstalledSurround;
    if (!CoreStore.WriteAndActivate(SurroundCore, Surround, SurroundDirectory,
            InstalledSurround, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        if (!OperationCurrent()) Cancel();
        return Output;
    }
    if (!OperationCurrent())
    {
        Cancel();
        return Output;
    }

    SkiDomain::CoverEcologyManifest Ecology;
    Ecology.GeneratorVersion = "mountain-planner-cover-ecology-v1";
    Ecology.CoverRevision = 1;
    Ecology.Source = {"synthetic-p1-fixture-cover",
        "Deterministic synthetic analytical cover", "2026-09-22",
        "repository-generated-class-values", "repository test fixture",
        "Mountain Planner deterministic fixture"};
    Ecology.Transform.Width = Dimension;
    Ecology.Transform.Height = Dimension;
    Ecology.Transform.LongitudeStepDeg =
        (RequestValue.Bounds.EastDeg - RequestValue.Bounds.WestDeg) / (Dimension - 1);
    Ecology.Transform.LatitudeStepDeg =
        (RequestValue.Bounds.NorthDeg - RequestValue.Bounds.SouthDeg) / (Dimension - 1);
    Ecology.Transform.SampleCenterBounds = RequestValue.Bounds;
    if (!SkiDomain::ComputeCoverEcologyOuterBounds(Dimension, Dimension,
            Ecology.Transform.LongitudeStepDeg, Ecology.Transform.LatitudeStepDeg,
            Ecology.Transform.SampleCenterBounds, Ecology.Transform.OuterBounds))
    {
        Output.Error = TEXT("Fixture CoverEcology transform is invalid.");
        return Output;
    }
    TArray<uint8> PackedValidity;
    PackedValidity.Init(0, FMath::DivideAndRoundUp(CoverValidity.Num(), 8));
    for (int32 Index = 0; Index < CoverValidity.Num(); ++Index)
        PackedValidity[Index / 8] |= static_cast<uint8>(1U << (Index % 8));
    CoverEcologyStore EcologyStore(DataRoot);
    FString EcologyDirectory;
    SkiDomain::CoverEcologyManifest InstalledEcology;
    if (!EcologyStore.WriteAndActivate(Ecology, Cover, PackedValidity, EcologyDirectory,
            InstalledEcology, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        if (!OperationCurrent()) Cancel();
        return Output;
    }
    if (!OperationCurrent())
    {
        Cancel();
        return Output;
    }

    SkiDomain::InstalledTerrainReceipt Installation;
    Installation.GeneratorVersion = "mountain-planner-installed-terrain-v1";
    Installation.TerrainCoreId = InstalledCore.ContentId;
    Installation.SurroundTerrainCoreId = InstalledSurround.ContentId;
    Installation.CoverEcologyId = InstalledEcology.ContentId;
    Installation.OptionalSources = {
        {"naip", "USDA NAIP RGB+NIR", SkiDomain::OptionalSourceStatus::NotRequested, {},
            "SYNTHETIC_FIXTURE_NOT_REQUESTED", "USGS public domain", "USDA/USGS"},
        {"overpass", "OpenStreetMap vector context",
            SkiDomain::OptionalSourceStatus::NotRequested, {},
            "SYNTHETIC_FIXTURE_NOT_REQUESTED", "ODbL 1.0",
            "OpenStreetMap contributors"}};
    InstalledTerrainStore InstallationStore(DataRoot);
    FString InstallationDirectory;
    SkiDomain::InstalledTerrainReceipt InstalledReceipt;
    if (!InstallationStore.WriteAndActivate(Installation, InstallationDirectory,
            InstalledReceipt, Output.Error, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        if (!OperationCurrent()) Cancel();
        return Output;
    }
    if (!OperationCurrent())
    {
        Cancel();
        return Output;
    }

    Report(State::Verifying, 5, 6,
        TEXT("Reopening fixture TerrainCore, CoverEcology, and composite installation"));
    TerrainCorePackageIndex CoreIndex;
    CoverEcologyPackageIndex EcologyIndex;
    InstalledTerrainIndex InstallationIndex;
    TArray<uint8> VerifiedCover;
    TArray<uint8> VerifiedPackedValidity;
    if (!CoreStore.Open(UTF8_TO_TCHAR(InstalledCore.ContentId.c_str()), CoreIndex, Output.Error)
        || !CoreStore.Verify(CoreIndex, Output.Error)
        || !EcologyStore.Open(UTF8_TO_TCHAR(InstalledEcology.ContentId.c_str()),
            EcologyIndex, Output.Error)
        || !EcologyStore.Verify(EcologyIndex, Output.Error)
        || !EcologyStore.ReadChannels(EcologyIndex, VerifiedCover,
            VerifiedPackedValidity, Output.Error)
        || !InstallationStore.Open(UTF8_TO_TCHAR(InstalledReceipt.ContentId.c_str()),
            InstallationIndex, Output.Error)
        || VerifiedCover != Cover || VerifiedPackedValidity != PackedValidity)
    {
        if (Output.Error.IsEmpty())
            Output.Error = TEXT("Reopened fixture channels differ from staged analytical cover.");
        if (!OperationCurrent()) Cancel();
        return Output;
    }
    if (!OperationCurrent())
    {
        Cancel();
        return Output;
    }

    // Runtime compatibility metadata is never written as a schema-1 package. The two entries
    // are in-memory references to the verified native component IDs (no files are implied);
    // the legacy in-memory session validator requires them.
    Manifest.ContentId = InstalledReceipt.ContentId;
    Manifest.Assets = {
        {"native/terraincore.ref", "terraincore-v2-content-id", InstalledCore.ContentId,
            64, true, {}, "deterministic-p1-fixture", "repository test fixture"},
        {"native/coverecology.ref", "cover-ecology-v1-content-id", InstalledEcology.ContentId,
            64, true, {}, "deterministic-p1-fixture", "repository test fixture"}};
    const auto Publish = [&]()
    {
        Output.PackageDirectory = MoveTemp(InstallationDirectory);
        Output.Manifest = MoveTemp(Manifest);
        Output.Heightfield = MoveTemp(Field);
        Output.Cover = MoveTemp(VerifiedCover);
        Output.CoverValidity = MoveTemp(CoverValidity);
        Output.TerrainCoreManifest = MoveTemp(InstalledCore);
        Output.SurroundTerrainCoreManifest = MoveTemp(InstalledSurround);
        Output.SurroundHeightfield = MoveTemp(Surround);
        Output.CoverEcologyManifest = MoveTemp(InstalledEcology);
        Output.InstallationReceipt = MoveTemp(InstalledReceipt);
        Output.HasNativeV2Installation = true;
        Output.Warnings.Add(TEXT("Synthetic fixture intentionally did not request optional NAIP imagery."));
        Output.Warnings.Add(TEXT("Synthetic fixture intentionally did not request optional vector context."));
        Output.Ok = true;
        Output.FinalState = State::Installed;
    };
    const bool Published = !CancellationValue->IsCancelled()
        && (RequestValue.Lease
            ? RequestValue.Lease->RunIfCurrent(RequestValue.SessionGeneration,
                RequestValue.OperationGeneration, Publish)
            : (Publish(), true));
    if (!Published)
    {
        Cancel();
        return Output;
    }
    Report(State::Installed, 6, 6, TEXT("Native fixture installation activated"));
    return Output;
}
