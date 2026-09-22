#include "SkiPreparation/FixtureTerrainProvider.h"

#include "SkiPreparation/TerrainPackageStore.h"
#include "Misc/DateTime.h"

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
    auto Report = [&](const State Phase, const uint64 Completed, const uint64 Total, const TCHAR* Detail)
    {
        if (OnProgress) OnProgress({Phase, Completed, Total, FPlatformTime::Seconds() - Began, Detail});
    };
    Report(State::Validating, 0, 5, TEXT("Validating fixture request"));
    if (!ValidateRequest(RequestValue, Output.Error)) return Output;
    if (CancellationValue->IsCancelled())
    {
        Output.FinalState = State::Cancelled;
        Output.Error = TEXT("Fixture preparation cancelled.");
        return Output;
    }
    Report(State::Deriving, 1, 5, TEXT("Generating deterministic asymmetric terrain"));
    const uint32 Dimension = RequestValue.Profile == SourceProfile::High ? 513U : 257U;
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
        if (CancellationValue->IsCancelled())
        {
            Output.FinalState = State::Cancelled;
            Output.Error = TEXT("Fixture preparation cancelled.");
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
    Manifest.RequestedAtUtc = TCHAR_TO_UTF8(*FDateTime::UtcNow().ToIso8601());
    Manifest.RequestedBounds = RequestValue.Bounds;
    Manifest.ActualBounds = RequestValue.Bounds;
    Manifest.LocalOrigin = {CenterLatitude, CenterLongitude, Field.Samples[(Dimension / 2) * Dimension + Dimension / 2]};
    Manifest.HeightWidth = Dimension;
    Manifest.HeightHeight = Dimension;
    Manifest.CoverWidth = Dimension;
    Manifest.CoverHeight = Dimension;
    Manifest.EastSpacingM = Field.EastSpacingM;
    Manifest.NorthSpacingM = Field.NorthSpacingM;
    TArray<PackageAssetBytes> Assets;
    PackageAssetBytes Cover{TEXT("cover.u8"), TEXT("worldcover-byte-grid"), {}, true, {},
        TEXT("deterministic-p1-fixture"), TEXT("repository test fixture")};
    Cover.Bytes.SetNumUninitialized(static_cast<int32>(Field.Samples.size()));
    for (int32 Index = 0; Index < Cover.Bytes.Num(); ++Index)
    {
        const float Height = Field.Samples[static_cast<size_t>(Index)];
        Cover.Bytes[Index] = Height > 1850.0F ? 3 : Height > 1650.0F ? 2 : 1;
    }
    Assets.Add(std::move(Cover));
    PackageAssetBytes Surround{TEXT("surround.f32le"), TEXT("heightfield-surround-f32le"), {}, true, {},
        TEXT("deterministic-p1-fixture"), TEXT("repository test fixture")};
    Surround.Bytes.Append(reinterpret_cast<const uint8*>(Field.Samples.data()),
        static_cast<int32>(Field.Samples.size() * sizeof(float)));
    Assets.Add(std::move(Surround));
    PackageAssetBytes Contours{TEXT("contours.f32le"), TEXT("contour-segments-f32le"), {}, true, {},
        TEXT("derived from fixture elevation"), TEXT("repository test fixture")};
    for (uint32 Row = 0; Row < Dimension; Row += 32)
    {
        const float Segment[4]{static_cast<float>(Field.WestM), static_cast<float>(Field.SampleNorthM(Row)),
            static_cast<float>(Field.EastM(Dimension - 1)), static_cast<float>(Field.SampleNorthM(Row))};
        Contours.Bytes.Append(reinterpret_cast<const uint8*>(Segment), sizeof(Segment));
    }
    Assets.Add(std::move(Contours));
    PackageAssetBytes CoverDisplay{TEXT("cover-display.u8"), TEXT("cover-display-compact"), {}, true, {},
        TEXT("derived from fixture cover"), TEXT("repository test fixture")};
    CoverDisplay.Bytes = Assets[0].Bytes;
    Assets.Add(std::move(CoverDisplay));
    Assets.Add({TEXT("imagery.jpg"), TEXT("image/jpeg"), {}, false,
        TEXT("Synthetic fixture intentionally has no NAIP acquisition."), TEXT("none"), TEXT("not applicable")});
    Assets.Add({TEXT("vectors.json"), TEXT("overpass-json"), {}, false,
        TEXT("Synthetic fixture intentionally has no Overpass response."), TEXT("none"), TEXT("not applicable")});
    Report(State::WritingStaging, 2, 5, TEXT("Writing fixture package staging"));
    PackageStore Store(DataRoot);
    if (!Store.WriteAndActivate(std::move(Manifest), Field, Output.PackageDirectory,
            Output.Manifest, Output.Error, Assets, RequestValue.Lease,
            RequestValue.SessionGeneration, RequestValue.OperationGeneration))
    {
        return Output;
    }
    Report(State::Verifying, 3, 5, TEXT("Reloading activated fixture"));
    if (!Store.Load(UTF8_TO_TCHAR(Output.Manifest.ContentId.c_str()), Output.Manifest,
            Output.Heightfield, Output.Error, &Output.Cover))
    {
        return Output;
    }
    Report(State::Installed, 5, 5, TEXT("Fixture package installed"));
    Output.Ok = true;
    Output.FinalState = State::Installed;
    return Output;
}
