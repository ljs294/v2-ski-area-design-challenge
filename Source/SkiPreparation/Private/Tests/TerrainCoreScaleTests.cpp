#include "SkiPreparation/M0TerrainCoreScale.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformFileManager.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "M0TerrainCoreTiming.h"

#include <cstring>
#include <zlib.h>

thread_local SkiPreparation::FM0TerrainCoreTiming* SkiPreparation::GM0TerrainCoreTiming = nullptr;

namespace
{
class FAnalyticScratch
{
public:
    explicit FAnalyticScratch(uint32 InSide, const FString& Root)
        : Side(InSide), HeightPath(FPaths::Combine(Root, TEXT("lod0-heights.raw"))),
          ValidityPath(FPaths::Combine(Root, TEXT("lod0-validity.raw"))) {}

    bool Create(FString& Error)
    {
        IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
        TUniquePtr<IFileHandle> Heights(Files.OpenWrite(*HeightPath));
        TUniquePtr<IFileHandle> Validity(Files.OpenWrite(*ValidityPath));
        if (!Heights || !Validity) { Error = TEXT("Cannot create canonical LOD0 scratch."); return false; }
        TArray<float> HeightRow;
        TArray<uint8> ValidRow;
        HeightRow.SetNumUninitialized(Side);
        ValidRow.Init(1, Side);
        for (uint32 Row = 0; Row < Side; ++Row)
        {
            for (uint32 Column = 0; Column < Side; ++Column)
                HeightRow[Column] = static_cast<float>(
                    1000.0 + Row * 0.125 + Column * 0.0625);
            if (!Heights->Write(reinterpret_cast<const uint8*>(HeightRow.GetData()),
                    static_cast<int64>(Side) * sizeof(float))
                || !Validity->Write(ValidRow.GetData(), Side))
            {
                Error = TEXT("Cannot write canonical LOD0 scratch row.");
                return false;
            }
        }
        Heights.Reset();
        Validity.Reset();
        ReadHeights.Reset(Files.OpenRead(*HeightPath));
        ReadValidity.Reset(Files.OpenRead(*ValidityPath));
        if (!ReadHeights || !ReadValidity)
        {
            Error = TEXT("Cannot reopen canonical LOD0 scratch.");
            return false;
        }
        return true;
    }

    uint64 Bytes() const { return static_cast<uint64>(Side) * Side * (sizeof(float) + 1); }
    void Close() { ReadHeights.Reset(); ReadValidity.Reset(); }

    bool Encode(const SkiDomain::TerrainCoreTileDescriptor& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& Error)
    {
        Out = {};
        const uint32 Width = Planned.CoreWidth + Planned.HaloWest + Planned.HaloEast;
        const uint32 Height = Planned.CoreHeight + Planned.HaloNorth + Planned.HaloSouth;
        const uint64 Count = static_cast<uint64>(Width) * Height;
        if (Count == 0 || Count > SkiDomain::TerrainCoreMaxStoredSamples)
        {
            Error = TEXT("Invalid scratch tile dimensions."); return false;
        }
        TArray<float> TileHeights;
        TArray<uint8> Bits;
        TArray<float> HeightSpan;
        TArray<uint8> ValidSpan;
        TileHeights.SetNumUninitialized(static_cast<int32>(Count));
        Bits.Init(0, static_cast<int32>((Count + 7) / 8));
        const uint32 Span = (Width - 1) * Planned.LodFactor + 1;
        HeightSpan.SetNumUninitialized(Span);
        ValidSpan.SetNumUninitialized(Span);
        for (uint32 Row = 0; Row < Height; ++Row)
        {
            const int64 LodRow = static_cast<int64>(Planned.StartRow) + Row - Planned.HaloNorth;
            const int64 LodColumn = static_cast<int64>(Planned.StartColumn) - Planned.HaloWest;
            if (LodRow < 0 || LodColumn < 0) { Error = TEXT("Scratch halo underflow."); return false; }
            const uint32 SourceRow = FMath::Min<uint32>(static_cast<uint32>(LodRow) * Planned.LodFactor, Side - 1);
            const uint32 SourceColumn = FMath::Min<uint32>(static_cast<uint32>(LodColumn) * Planned.LodFactor, Side - 1);
            const uint32 Available = FMath::Min<uint32>(Span, Side - SourceColumn);
            const int64 SampleOffset = static_cast<int64>(SourceRow) * Side + SourceColumn;
            if (!ReadHeights->Seek(SampleOffset * sizeof(float))
                || !ReadHeights->Read(reinterpret_cast<uint8*>(HeightSpan.GetData()),
                    static_cast<int64>(Available) * sizeof(float))
                || !ReadValidity->Seek(SampleOffset)
                || !ReadValidity->Read(ValidSpan.GetData(), Available))
            {
                Error = TEXT("Canonical LOD0 scratch read failed."); return false;
            }
            for (uint32 Column = 0; Column < Width; ++Column)
            {
                const uint32 Sample = FMath::Min<uint32>(Column * Planned.LodFactor, Available - 1);
                const int32 Target = static_cast<int32>(Row * Width + Column);
                TileHeights[Target] = HeightSpan[Sample];
                if (ValidSpan[Sample] != 0) Bits[Target / 8] |= static_cast<uint8>(1U << (Target % 8));
            }
        }
        auto Compress = [&](const uint8* Raw, int32 RawBytes, TArray<uint8>& Result) -> bool
        {
            uLongf Bound = compressBound(static_cast<uLong>(RawBytes));
            Result.SetNumUninitialized(static_cast<int32>(Bound));
            if (compress2(Result.GetData(), &Bound, Raw, static_cast<uLong>(RawBytes), Z_BEST_COMPRESSION) != Z_OK)
            { Error = TEXT("Scratch tile compression failed."); return false; }
            Result.SetNum(static_cast<int32>(Bound));
            return true;
        };
        const int32 HeightBytes = static_cast<int32>(Count * sizeof(float));
        if (!Compress(reinterpret_cast<const uint8*>(TileHeights.GetData()), HeightBytes, Out.CompressedHeights)
            || !Compress(Bits.GetData(), Bits.Num(), Out.CompressedValidity)) return false;
        Out.Descriptor = Planned;
        Out.Descriptor.ProcessingVersion = "m0-analytic-indexed-lod-v1";
        Out.Descriptor.ProvenanceId = "m0-analytic";
        Out.Descriptor.HeightRawBytes = HeightBytes;
        Out.Descriptor.ValidityRawBytes = Bits.Num();
        Out.Descriptor.HeightBytes = Out.CompressedHeights.Num();
        Out.Descriptor.ValidityBytes = Out.CompressedValidity.Num();
        Out.Descriptor.HeightSha256 = TCHAR_TO_UTF8(*SkiPreparation::Sha256(MakeArrayView(Out.CompressedHeights)));
        Out.Descriptor.ValiditySha256 = TCHAR_TO_UTF8(*SkiPreparation::Sha256(MakeArrayView(Out.CompressedValidity)));
        return true;
    }

private:
    uint32 Side;
    FString HeightPath;
    FString ValidityPath;
    TUniquePtr<IFileHandle> ReadHeights;
    TUniquePtr<IFileHandle> ReadValidity;
};

SkiDomain::TerrainCoreManifest AnalyticManifest(const uint32 Side)
{
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "m0-synthetic-v1";
    Manifest.ProcessingVersions = {"m0-analytic-indexed-lod-v1"};
    Manifest.Source.SourceId = "m0-analytic";
    Manifest.Source.Product = "synthetic";
    Manifest.Source.AcquisitionEpoch = "2026-09-23";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "synthetic";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "CC0";
    Manifest.Source.Attribution = "Mountain Planner M0 scale fixture";
    Manifest.Source.NativeEastSpacingM = 1.0;
    Manifest.Source.NativeNorthSpacingM = 1.0;
    Manifest.LocalOrigin = {46.93, -121.50, 1500.0};
    Manifest.Width = Side;
    Manifest.Height = Side;
    Manifest.DeliveredEastSpacingM = 1.0;
    Manifest.DeliveredNorthSpacingM = 1.0;
    Manifest.SampleCenterBounds = {0.0, -static_cast<double>(Side - 1),
        static_cast<double>(Side - 1), 0.0};
    SkiDomain::ComputeTerrainCoreBounds(Side, Side, 1.0, 1.0,
        Manifest.SampleCenterBounds, Manifest.OuterBounds);
    return Manifest;
}

bool BuildAndVerify(const uint32 Side, FString& OutReport, FString& OutError,
    FString* OutPackageRoot, FString* OutContentId)
{
    const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("TerrainCoreM0Scale"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const double Begin = FPlatformTime::Seconds();
    IFileManager::Get().MakeDirectory(*Root, true);
    FAnalyticScratch Scratch(Side, Root);
    if (!Scratch.Create(OutError)) return false;
    const double Generated = FPlatformTime::Seconds();
    uint64 ObservedPeakPhysical = FPlatformMemory::GetStats().UsedPhysical;
    double LodSeconds = 0.0;
    const SkiPreparation::TerrainCoreTileSource Source = [&](const auto& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& Error)
    {
        ObservedPeakPhysical = FMath::Max<uint64>(ObservedPeakPhysical,
            FPlatformMemory::GetStats().UsedPhysical);
        const double Started = FPlatformTime::Seconds();
        const bool Encoded = Scratch.Encode(Planned, Out, Error);
        LodSeconds += FPlatformTime::Seconds() - Started;
        return Encoded;
    };
    SkiPreparation::TerrainCorePackageStore Store(Root);
    SkiPreparation::FM0TerrainCoreTiming StoreTiming;
    SkiPreparation::GM0TerrainCoreTiming = &StoreTiming;
    FString Directory, Error;
    SkiDomain::TerrainCoreManifest Written;
    const bool Activated = Store.WriteAndActivateFromTiles(
        AnalyticManifest(Side), Source, Directory, Written, Error);
    SkiPreparation::GM0TerrainCoreTiming = nullptr;
    ObservedPeakPhysical = FMath::Max<uint64>(ObservedPeakPhysical,
        StoreTiming.ObservedPeakPhysicalBytes);
    const double Built = FPlatformTime::Seconds();
    if (!Activated) { OutError = Error; return false; }
    SkiPreparation::TerrainCorePackageIndex Index;
    const bool Opened = Store.Open(UTF8_TO_TCHAR(Written.ContentId.c_str()), Index, Error);
    if (!Opened) { OutError = Error; return false; }
    const double Reopened = FPlatformTime::Seconds();
    const bool Verified = Store.Verify(Index, Error);
    if (!Verified) { OutError = Error; return false; }
    const double VerifiedAt = FPlatformTime::Seconds();
    ObservedPeakPhysical = FMath::Max<uint64>(ObservedPeakPhysical,
        FPlatformMemory::GetStats().UsedPhysical);
    SkiPreparation::TerrainCoreDecodedTile Visible;
    const bool Read = Store.ReadTile(Index, 0, 0, 0, Visible, Error);
    if (!Read) { OutError = Error; return false; }
    const double FirstRead = FPlatformTime::Seconds();
    if (Read && Side <= 513)
    {
        for (const auto& Descriptor : Written.Tiles)
        {
            if (Descriptor.LodIndex == 0) continue;
            SkiPreparation::TerrainCoreDecodedTile Tile;
            if (!Store.ReadTile(Index, Descriptor.LodIndex, Descriptor.TileX,
                Descriptor.TileY, Tile, Error)) { OutError = Error; return false; }
            for (uint32 Row = 0; Row < Tile.StoredHeight; ++Row)
                for (uint32 Column = 0; Column < Tile.StoredWidth; ++Column)
                {
                    const int64 LodRow = static_cast<int64>(Descriptor.StartRow)
                        + Row - Descriptor.HaloNorth;
                    const int64 LodColumn = static_cast<int64>(Descriptor.StartColumn)
                        + Column - Descriptor.HaloWest;
                    if (LodRow < 0 || LodColumn < 0)
                    {
                        OutError = TEXT("Planned coarse tile halo crosses the north or west border.");
                        return false;
                    }
                    const uint32 SourceRow = static_cast<uint32>(FMath::Min<int64>(
                        LodRow * Descriptor.LodFactor, Side - 1));
                    const uint32 SourceColumn = static_cast<uint32>(FMath::Min<int64>(
                        LodColumn * Descriptor.LodFactor, Side - 1));
                    const float Expected = static_cast<float>(
                        1000.0 + SourceRow * 0.125 + SourceColumn * 0.0625);
                    const float Actual = Tile.Heights[Row * Tile.StoredWidth + Column];
                    if (std::memcmp(&Expected, &Actual, sizeof(float)) != 0)
                    {
                        OutError = TEXT("Coarse LOD differs bitwise from indexed LOD0 source.");
                        return false;
                    }
                }
        }
    }
    uint64 InstalledBytes = 0;
    for (const auto& Shard : Written.Shards) InstalledBytes += Shard.Bytes;
    const int64 ManifestBytes = IFileManager::Get().FileSize(
        *FPaths::Combine(Directory, TEXT("terraincore.json")));
    if (ManifestBytes > 0) InstalledBytes += static_cast<uint64>(ManifestBytes);
    double CancelMs = -1.0;
    bool bCancelActivated = false;
    if (Side == 10001)
    {
        const FString CancelRoot = FPaths::Combine(FPaths::ProjectSavedDir(),
            TEXT("Automation"), TEXT("TerrainCoreM0Cancel"),
            FGuid::NewGuid().ToString(EGuidFormats::Digits));
        SkiPreparation::TerrainCorePackageStore CancelStore(CancelRoot);
        auto Lease = MakeShared<SkiPreparation::PreparationOperationLease,
            ESPMode::ThreadSafe>(1, 1);
        double CancelAt = 0.0;
        const SkiPreparation::TerrainCoreTileSource CancelSource = [&](const auto& Planned,
            SkiPreparation::TerrainCoreEncodedTile& Out, FString& TileError)
        {
            Lease->Invalidate();
            CancelAt = FPlatformTime::Seconds();
            return Scratch.Encode(Planned, Out, TileError);
        };
        FString CancelDirectory, CancelError;
        SkiDomain::TerrainCoreManifest CancelManifest;
        bCancelActivated = CancelStore.WriteAndActivateFromTiles(
            AnalyticManifest(Side), CancelSource, CancelDirectory, CancelManifest,
            CancelError, Lease, 1, 1);
        CancelMs = (FPlatformTime::Seconds() - CancelAt) * 1000.0;
        IFileManager::Get().DeleteDirectory(*CancelRoot, false, true);
        if (bCancelActivated || CancelAt == 0.0)
        {
            OutError = TEXT("M0 cancel probe did not stop activation after lease invalidation.");
            return false;
        }
    }
    Scratch.Close();
    const double CleanupStarted = FPlatformTime::Seconds();
    bool Cleaned = true;
    if (OutPackageRoot)
    {
        Cleaned = IFileManager::Get().Delete(*FPaths::Combine(Root, TEXT("lod0-heights.raw")),
            false, true, true)
            && IFileManager::Get().Delete(*FPaths::Combine(Root, TEXT("lod0-validity.raw")),
                false, true, true);
        *OutPackageRoot = Root;
        *OutContentId = UTF8_TO_TCHAR(Written.ContentId.c_str());
    }
    else
    {
        Cleaned = IFileManager::Get().DeleteDirectory(*Root, false, true);
    }
    const double CleanupMs = (FPlatformTime::Seconds() - CleanupStarted) * 1000.0;
    if (!Cleaned) { OutError = TEXT("M0 scratch/package cleanup failed."); return false; }
    OutReport = FString::Printf(TEXT("{\"side\":%u,\"generationSeconds\":%.6f,\"lodSeconds\":%.6f,\"shardWriteSeconds\":%.6f,\"internalVerifySeconds\":%.6f,\"buildAndActivateSeconds\":%.6f,\"reopenSeconds\":%.6f,\"verifySeconds\":%.6f,\"firstTileReadSeconds\":%.6f,\"installedBytes\":%llu,\"stagingBytes\":%llu,\"scratchBytes\":%llu,\"observedPeakPhysicalBytes\":%llu,\"cancelReturnMs\":%.3f,\"cleanupMs\":%.3f,\"cancelActivated\":%s}"),
        Side, Generated - Begin, LodSeconds, StoreTiming.ShardWriteSeconds,
        StoreTiming.InternalVerifySeconds, Built - Generated, Reopened - Built,
        VerifiedAt - Reopened, FirstRead - VerifiedAt,
        static_cast<unsigned long long>(InstalledBytes),
        static_cast<unsigned long long>(StoreTiming.StagingBytes),
        static_cast<unsigned long long>(Scratch.Bytes()),
        static_cast<unsigned long long>(ObservedPeakPhysical), CancelMs, CleanupMs,
        bCancelActivated ? TEXT("true") : TEXT("false"));
    UE_LOG(LogTemp, Display, TEXT("%s"), *OutReport);
    return Read;
}
}

bool SkiPreparation::RunM0TerrainCoreScale(const uint32 Side, FString& OutReport,
    FString& OutError)
{
    OutReport.Reset();
    OutError.Reset();
    if (Side != 513 && Side != 10001)
    {
        OutError = TEXT("M0 synthetic side must be 513 or 10001.");
        return false;
    }
    return BuildAndVerify(Side, OutReport, OutError, nullptr, nullptr);
}

bool SkiPreparation::RunM0TerrainCoreScale(const uint32 Side, FString& OutReport,
    FString& OutError, FString& OutPackageRoot, FString& OutContentId)
{
    OutPackageRoot.Reset();
    OutContentId.Reset();
    OutReport.Reset();
    OutError.Reset();
    if (Side != 513 && Side != 10001)
    {
        OutError = TEXT("M0 synthetic side must be 513 or 10001.");
        return false;
    }
    return BuildAndVerify(Side, OutReport, OutError, &OutPackageRoot, &OutContentId);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreM0BitExactTest,
    "MountainPlanner.M0.TerrainCore.BitExactLodThroughVerify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTerrainCoreM0BitExactTest::RunTest(const FString& Parameters)
{
    FString Report, Error;
    const bool Ok = SkiPreparation::RunM0TerrainCoreScale(513, Report, Error);
    TestTrue(*Error, Ok);
    return Ok;
}

// Explicitly selected scale spike: never part of the routine P1 automation filter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreM0ScaleTest,
    "MountainPlanner.M0.TerrainCore.Scale10001",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTerrainCoreM0ScaleTest::RunTest(const FString& Parameters)
{
    FString Report, Error;
    const bool Ok = SkiPreparation::RunM0TerrainCoreScale(10001, Report, Error);
    TestTrue(*Error, Ok);
    return Ok;
}

#endif
