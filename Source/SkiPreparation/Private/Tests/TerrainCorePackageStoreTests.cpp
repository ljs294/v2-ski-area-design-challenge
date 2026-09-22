#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "SkiPreparation/TerrainCoreDerivation.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"

#include <atomic>

namespace
{
FString TempRoot(const TCHAR* Name)
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

SkiDomain::Heightfield MakeField(const uint32 Width, const uint32 Height)
{
    SkiDomain::Heightfield Field;
    Field.Width = Width;
    Field.Height = Height;
    Field.WestM = -50.0;
    Field.NorthM = 80.0;
    Field.EastSpacingM = 2.5;
    Field.NorthSpacingM = 3.0;
    Field.NoDataValue = -9999.0;
    Field.CurrentRevision = 1;
    Field.Samples.resize(static_cast<uint64>(Width) * Height);
    for (uint32 Row = 0; Row < Height; ++Row)
        for (uint32 Column = 0; Column < Width; ++Column)
            Field.Samples[static_cast<uint64>(Row) * Width + Column] =
                100.0F + static_cast<float>(Row) * 0.5F + static_cast<float>(Column) * 0.25F;
    Field.Samples[static_cast<uint64>(Height / 2) * Width + Width / 2] = -9999.0F;
    return Field;
}

SkiDomain::TerrainCoreManifest MakeCoreManifest()
{
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "terraincore-store-test-v1";
    Manifest.ProcessingVersions = {"terraincore-derivation-v1"};
    Manifest.Source.SourceId = "fixture-ground-v1";
    Manifest.Source.Product = "synthetic bare earth";
    Manifest.Source.AcquisitionEpoch = "2026-09-22";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "WGS84";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "CC0";
    Manifest.Source.Attribution = "Mountain Planner deterministic fixture";
    Manifest.Source.NativeEastSpacingM = 2.5;
    Manifest.Source.NativeNorthSpacingM = 3.0;
    Manifest.LocalOrigin = {46.93, -121.50, 1500.0};
    return Manifest;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreLegacyPackageCompatibilityTest,
    "MountainPlanner.P1.TerrainCore.LegacyPackageCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreLegacyPackageCompatibilityTest::RunTest(const FString& Parameters)
{
    const FString Root = TempRoot(TEXT("TerrainCoreLegacy"));
    const SkiDomain::Heightfield Field = MakeField(3, 3);
    SkiDomain::TerrainManifest Legacy;
    Legacy.Name = "legacy-fixture";
    Legacy.Source = "fixture";
    Legacy.RequestedAtUtc = "2026-09-22T00:00:00Z";
    Legacy.RequestedBounds = {-121.0, 46.0, -120.99, 46.01};
    Legacy.ActualBounds = Legacy.RequestedBounds;
    Legacy.LocalOrigin = {46.005, -120.995, 0.0};
    Legacy.VerticalDatum = "unknown";
    SkiPreparation::PackageStore LegacyStore(Root);
    FString Directory, Error;
    SkiDomain::TerrainManifest Written;
    TestTrue(TEXT("Schema-1 package still writes"),
        LegacyStore.WriteAndActivate(Legacy, Field, Directory, Written, Error));
    SkiDomain::TerrainManifest LoadedManifest;
    SkiDomain::Heightfield LoadedField;
    TestTrue(TEXT("Schema-1 package still loads"), LegacyStore.Load(
        UTF8_TO_TCHAR(Written.ContentId.c_str()), LoadedManifest, LoadedField, Error));
    TestEqual(TEXT("Legacy schema remains one"), LoadedManifest.SchemaVersion, uint32(1));
    SkiPreparation::TerrainCorePackageStore CoreStore(Root);
    SkiPreparation::TerrainCorePackageIndex CoreIndex;
    TestFalse(TEXT("Schema-2 store never reinterprets a schema-1 package"), CoreStore.Open(
        UTF8_TO_TCHAR(Written.ContentId.c_str()), CoreIndex, Error));
    TestEqual(TEXT("Legacy heights are unchanged"), LoadedField.Samples[4], Field.Samples[4]);
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreStoreRoundTripAndAttacksTest,
    "MountainPlanner.P1.TerrainCore.StoreRoundTripAndAttacks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreStoreRoundTripAndAttacksTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("Reserved Windows device path is rejected"),
        SkiDomain::IsSafeTerrainCorePath("shards/CON.tcs"));
    TestFalse(TEXT("Trailing-dot path is rejected"),
        SkiDomain::IsSafeTerrainCorePath("shards/data.tcs."));
    const FString Root = TempRoot(TEXT("TerrainCoreStore"));
    const SkiDomain::Heightfield Field = MakeField(300, 260);
    SkiDomain::TerrainCoreManifest Input = MakeCoreManifest();
    Input.Width = Field.Width;
    Input.Height = Field.Height;
    Input.DeliveredEastSpacingM = Field.EastSpacingM;
    Input.DeliveredNorthSpacingM = Field.NorthSpacingM;
    Input.SampleCenterBounds = {Field.WestM, Field.SampleNorthM(Field.Height - 1),
        Field.EastM(Field.Width - 1), Field.NorthM};
    TestTrue(TEXT("Streaming bounds compute"), SkiDomain::ComputeTerrainCoreBounds(
        Input.Width, Input.Height, Input.DeliveredEastSpacingM,
        Input.DeliveredNorthSpacingM, Input.SampleCenterBounds, Input.OuterBounds));

    SkiDomain::TerrainCoreTilePlan Expected;
    TestTrue(TEXT("Streaming plan computes"),
        SkiDomain::PlanTerrainCoreTiles(Field.Width, Field.Height, Expected));
    int32 Calls = 0;
    std::atomic<int32> Active{0};
    std::atomic<int32> Maximum{0};
    const SkiPreparation::TerrainCoreTileSource Source = [&](const auto& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& Error)
    {
        const int32 Current = Active.fetch_add(1) + 1;
        Maximum.store(FMath::Max(Maximum.load(), Current));
        ++Calls;
        const bool Ok = SkiPreparation::DeriveTerrainCoreTile(Field, Planned, Out, Error);
        Active.fetch_sub(1);
        return Ok;
    };
    SkiPreparation::TerrainCorePackageStore Store(Root);
    FString Directory, Error;
    SkiDomain::TerrainCoreManifest Written;
    const bool Activated = Store.WriteAndActivateFromTiles(
        Input, Source, Directory, Written, Error);
    TestTrue(*FString::Printf(TEXT("Streaming schema-2 package activates: %s"), *Error), Activated);
    if (!Activated)
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }
    TestEqual(TEXT("Every planned tile requested exactly once"), Calls,
        static_cast<int32>(Expected.Tiles.size()));
    TestEqual(TEXT("Tile source has at most one live payload"), Maximum.load(), 1);
    TestEqual(TEXT("All five LOD factors are stored"), Written.Tiles.back().LodFactor, uint32(16));
    TestTrue(TEXT("Partial edge tile is represented"), Written.Tiles[1].CoreWidth < 256);

    SkiPreparation::TerrainCorePackageIndex Index;
    const FString Id = UTF8_TO_TCHAR(Written.ContentId.c_str());
    TestTrue(TEXT("Bounded index opens"), Store.Open(Id, Index, Error));
    TestEqual(TEXT("Local ENU origin latitude persists"),
        Index.Manifest.LocalOrigin.LatitudeDeg, Input.LocalOrigin.LatitudeDeg);
    TestEqual(TEXT("Local ENU origin height persists"),
        Index.Manifest.LocalOrigin.HeightM, Input.LocalOrigin.HeightM);
    TestTrue(TEXT("Incremental package verification passes"), Store.Verify(Index, Error));
    SkiPreparation::TerrainCoreDecodedTile First, East;
    TestTrue(TEXT("First finest tile reads"), Store.ReadTile(Index, 0, 0, 0, First, Error));
    TestTrue(TEXT("Adjacent finest tile reads"), Store.ReadTile(Index, 0, 1, 0, East, Error));
    const int32 FirstShared = First.Descriptor.HaloWest
        + First.Descriptor.CoreWidth - 1;
    const int32 EastShared = East.Descriptor.HaloWest;
    TestEqual(TEXT("Adjacent tiles share canonical border sample"),
        First.Heights[FirstShared], East.Heights[EastShared]);
    SkiPreparation::TerrainCorePackageIndex HostileIndex = Index;
    HostileIndex.Manifest.Tiles[0].HeightBytes = SkiDomain::TerrainCoreDeflateBound(
        HostileIndex.Manifest.Tiles[0].HeightRawBytes) + 1ULL;
    SkiPreparation::TerrainCoreDecodedTile ReusedTile = First;
    TestFalse(TEXT("Mutated index cannot allocate a codec-impossible compressed range"),
        Store.ReadTile(HostileIndex, 0, 0, 0, ReusedTile, Error));
    TestTrue(TEXT("Failed tile read clears reused decoded output"),
        ReusedTile.Heights.IsEmpty() && ReusedTile.Validity.IsEmpty());

#if PLATFORM_WINDOWS
    const FString OriginalShards = FPaths::Combine(Directory, TEXT("shards"));
    const FString HeldShards = TempRoot(TEXT("TerrainCoreReadJunctionHeld"));
    const FString OutsideShards = TempRoot(TEXT("TerrainCoreReadJunctionOutside"));
    IFileManager::Get().MakeDirectory(*OutsideShards, true);
    TArray<FString> ShardFiles;
    IFileManager::Get().FindFiles(ShardFiles,
        *FPaths::Combine(OriginalShards, TEXT("*.tcs")), true, false);
    for (const FString& ShardFile : ShardFiles)
    {
        IFileManager::Get().Copy(*FPaths::Combine(OutsideShards, ShardFile),
            *FPaths::Combine(OriginalShards, ShardFile));
    }
    const bool Held = IFileManager::Get().Move(
        *HeldShards, *OriginalShards, false, false, false, true);
    int32 ReadJunctionCode = -1;
    FString ReadJunctionOut, ReadJunctionError;
    const FString ReadJunctionArgs = FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""),
        *OriginalShards, *OutsideShards);
    const bool ReadJunctionCreated = Held && FPlatformProcess::ExecProcess(
        TEXT("cmd.exe"), *ReadJunctionArgs, &ReadJunctionCode,
        &ReadJunctionOut, &ReadJunctionError) && ReadJunctionCode == 0;
    TestTrue(TEXT("Shard-read junction attack fixture is created"), ReadJunctionCreated);
    if (ReadJunctionCreated)
    {
        SkiPreparation::TerrainCoreDecodedTile JunctionTile;
        TestFalse(TEXT("Pinned shard read rejects a junction substituted after index open"),
            Store.ReadTile(Index, 0, 0, 0, JunctionTile, Error));
        TestFalse(TEXT("Pinned shard hashing rejects a junction substituted after index open"),
            Store.Verify(Index, Error));
        const FString RemoveArgs = FString::Printf(TEXT("/c rmdir \"%s\""), *OriginalShards);
        FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *RemoveArgs,
            &ReadJunctionCode, &ReadJunctionOut, &ReadJunctionError);
    }
    if (Held) IFileManager::Get().Move(
        *OriginalShards, *HeldShards, false, false, false, true);
    IFileManager::Get().DeleteDirectory(*OutsideShards, false, true);
    IFileManager::Get().DeleteDirectory(*HeldShards, false, true);
#endif

    FString OversizedDirectory = TEXT("reuse-me");
    SkiDomain::TerrainCoreManifest OversizedManifest;
    OversizedManifest.ContentId = std::string(64, 'c');
    const SkiPreparation::TerrainCoreTileSource OversizedSource = [&](const auto& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& SourceError)
    {
        if (!SkiPreparation::DeriveTerrainCoreTile(Field, Planned, Out, SourceError)) return false;
        Out.Descriptor.HeightBytes = SkiDomain::TerrainCoreDeflateBound(
            Out.Descriptor.HeightRawBytes) + 1ULL;
        return true;
    };
    TestFalse(TEXT("Codec-impossible compressed declaration is rejected before allocation"),
        Store.WriteAndActivateFromTiles(Input, OversizedSource, OversizedDirectory,
            OversizedManifest, Error));
    TestTrue(TEXT("Failed package write clears reused directory output"),
        OversizedDirectory.IsEmpty());
    TestTrue(TEXT("Failed package write clears reused manifest output"),
        OversizedManifest.ContentId.empty());

    SkiDomain::Heightfield InconsistentField = Field;
    InconsistentField.Samples[255] += 7.0F;
    const SkiPreparation::TerrainCoreTileSource InconsistentSource = [&](const auto& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& SourceError)
    {
        const SkiDomain::Heightfield& Selected = Planned.LodIndex == 0 && Planned.TileX == 1
            && Planned.TileY == 0 ? InconsistentField : Field;
        return SkiPreparation::DeriveTerrainCoreTile(Selected, Planned, Out, SourceError);
    };
    FString InconsistentDirectory;
    SkiDomain::TerrainCoreManifest InconsistentManifest;
    TestFalse(TEXT("Individually valid tiles with a mismatched shared edge are not activated"),
        Store.WriteAndActivateFromTiles(Input, InconsistentSource, InconsistentDirectory,
            InconsistentManifest, Error));

    auto ExistingLease = MakeShared<SkiPreparation::PreparationOperationLease,
        ESPMode::ThreadSafe>(9, 11);
    int32 ExistingCalls = 0;
    const SkiPreparation::TerrainCoreTileSource ExistingSource = [&](const auto& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& SourceError)
    {
        const bool Ok = SkiPreparation::DeriveTerrainCoreTile(Field, Planned, Out, SourceError);
        if (++ExistingCalls == static_cast<int32>(Expected.Tiles.size())) ExistingLease->Invalidate();
        return Ok;
    };
    FString ExistingDirectory = TEXT("reuse-me");
    SkiDomain::TerrainCoreManifest ExistingManifest;
    ExistingManifest.ContentId = std::string(64, 'd');
    TestFalse(TEXT("Invalidated lease cannot publish an already-installed target"),
        Store.WriteAndActivateFromTiles(Input, ExistingSource, ExistingDirectory,
            ExistingManifest, Error, ExistingLease, 9, 11));
    TestTrue(TEXT("Existing-target lease failure clears reused outputs"),
        ExistingDirectory.IsEmpty() && ExistingManifest.ContentId.empty());

    const FString UndeclaredPath = FPaths::Combine(Directory, TEXT("undeclared.bin"));
    TestTrue(TEXT("Undeclared attack file can be placed"),
        FFileHelper::SaveStringToFile(TEXT("attack"), *UndeclaredPath));
    SkiPreparation::TerrainCorePackageIndex Undeclared;
    TestFalse(TEXT("Package with undeclared file is rejected"), Store.Open(Id, Undeclared, Error));
    IFileManager::Get().Delete(*UndeclaredPath, false, true, true);

    const FString ManifestPath = FPaths::Combine(Directory, TEXT("terraincore.json"));
    FString Json;
    TestTrue(TEXT("Manifest can be read for attack"), FFileHelper::LoadFileToString(Json, *ManifestPath));
    TestTrue(TEXT("Attack mutation is applied"), FFileHelper::SaveStringToFile(Json + TEXT(" "),
        *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    SkiPreparation::TerrainCorePackageIndex Attacked = Index;
    TestFalse(TEXT("Noncanonical/duplicate-tail manifest is rejected"), Store.Open(Id, Attacked, Error));
    TestTrue(TEXT("Failed package open clears reused index output"),
        Attacked.PackageDirectory.IsEmpty() && Attacked.Manifest.ContentId.empty());
    const TArray<uint8> InvalidUtf8{0xc3U, 0x28U};
    TestTrue(TEXT("Invalid UTF-8 manifest attack is applied"),
        FFileHelper::SaveArrayToFile(InvalidUtf8, *ManifestPath));
    TestFalse(TEXT("Invalid UTF-8 manifest is rejected before JSON decoding"),
        Store.Open(Id, Attacked, Error));

    auto Lease = MakeShared<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>(5, 7);
    Lease->Invalidate();
    FString RejectedDirectory = TEXT("reuse-me");
    SkiDomain::TerrainCoreManifest Rejected;
    Rejected.ContentId = std::string(64, 'e');
    TestFalse(TEXT("Stale operation cannot activate"), Store.WriteAndActivate(
        MakeCoreManifest(), MakeField(4, 4), RejectedDirectory, Rejected, Error, Lease, 5, 7));
    TestTrue(TEXT("Early stale failure clears public package outputs"),
        RejectedDirectory.IsEmpty() && Rejected.ContentId.empty());

#if PLATFORM_WINDOWS
    const FString JunctionRoot = TempRoot(TEXT("TerrainCoreJunction"));
    const FString JunctionTarget = TempRoot(TEXT("TerrainCoreJunctionTarget"));
    IFileManager::Get().MakeDirectory(*JunctionRoot, true);
    IFileManager::Get().MakeDirectory(*JunctionTarget, true);
    const FString Junction = FPaths::Combine(JunctionRoot, TEXT("TerrainCore"));
    int32 ReturnCode = -1;
    FString StdOut, StdErr;
    const FString JunctionArgs = FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""),
        *Junction, *JunctionTarget);
    const bool JunctionCreated = FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *JunctionArgs,
        &ReturnCode, &StdOut, &StdErr) && ReturnCode == 0;
    TestTrue(TEXT("Windows junction attack fixture is created without elevation"), JunctionCreated);
    if (JunctionCreated)
    {
        SkiPreparation::TerrainCorePackageStore JunctionStore(JunctionRoot);
        FString JunctionDirectory = TEXT("reuse-me"), JunctionError;
        SkiDomain::TerrainCoreManifest JunctionManifest;
        TestFalse(TEXT("TerrainCore activation rejects a junction package ancestor"),
            JunctionStore.WriteAndActivate(MakeCoreManifest(), MakeField(4, 4),
                JunctionDirectory, JunctionManifest, JunctionError));
        TestTrue(TEXT("Junction rejection clears public outputs"),
            JunctionDirectory.IsEmpty() && JunctionManifest.ContentId.empty());
        const FString RemoveArgs = FString::Printf(TEXT("/c rmdir \"%s\""), *Junction);
        FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *RemoveArgs,
            &ReturnCode, &StdOut, &StdErr);
    }
    if (!IFileManager::Get().DirectoryExists(*Junction))
        IFileManager::Get().DeleteDirectory(*JunctionRoot, false, true);
    IFileManager::Get().DeleteDirectory(*JunctionTarget, false, true);

    const FString SwapRoot = TempRoot(TEXT("TerrainCoreStageSwap"));
    const FString SwapOutside = TempRoot(TEXT("TerrainCoreStageSwapOutside"));
    IFileManager::Get().MakeDirectory(*SwapOutside, true);
    SkiPreparation::TerrainCorePackageStore SwapStore(SwapRoot);
    const SkiDomain::Heightfield SwapField = MakeField(4, 4);
    bool SwapCallbackRan = false;
    bool SwapSucceeded = false;
    FString HeldStage;
    FString LinkedStage;
    const SkiPreparation::TerrainCoreTileSource SwapSource = [&](const auto& Planned,
        SkiPreparation::TerrainCoreEncodedTile& Out, FString& SourceError)
    {
        if (!SwapCallbackRan)
        {
            SwapCallbackRan = true;
            const FString StagingParent = FPaths::Combine(SwapRoot,
                TEXT(".terraincore-staging"));
            TArray<FString> Stages;
            IFileManager::Get().FindFiles(Stages, *FPaths::Combine(StagingParent, TEXT("*")),
                false, true);
            if (Stages.Num() == 1)
            {
                LinkedStage = FPaths::Combine(StagingParent, Stages[0]);
                HeldStage = LinkedStage + TEXT("-held");
                if (IFileManager::Get().Move(*HeldStage, *LinkedStage, false, false, false, true))
                {
                    const FString Args = FString::Printf(TEXT("/c mklink /J \"%s\" \"%s\""),
                        *LinkedStage, *SwapOutside);
                    int32 Code = -1;
                    FString Output, ProcessError;
                    FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Args,
                        &Code, &Output, &ProcessError);
                    SwapSucceeded = Code == 0;
                }
            }
        }
        return SkiPreparation::DeriveTerrainCoreTile(SwapField, Planned, Out, SourceError);
    };
    FString SwapDirectory = TEXT("reuse-me"), SwapError;
    SkiDomain::TerrainCoreManifest SwapManifest;
    SkiDomain::TerrainCoreManifest SwapInput = MakeCoreManifest();
    SwapInput.Width = SwapField.Width;
    SwapInput.Height = SwapField.Height;
    SwapInput.DeliveredEastSpacingM = SwapField.EastSpacingM;
    SwapInput.DeliveredNorthSpacingM = SwapField.NorthSpacingM;
    SwapInput.SampleCenterBounds = {SwapField.WestM,
        SwapField.SampleNorthM(SwapField.Height - 1),
        SwapField.EastM(SwapField.Width - 1), SwapField.NorthM};
    SkiDomain::ComputeTerrainCoreBounds(SwapInput.Width, SwapInput.Height,
        SwapInput.DeliveredEastSpacingM, SwapInput.DeliveredNorthSpacingM,
        SwapInput.SampleCenterBounds, SwapInput.OuterBounds);
    const bool SwapWriteSucceeded = SwapStore.WriteAndActivateFromTiles(SwapInput, SwapSource,
        SwapDirectory, SwapManifest, SwapError, nullptr, 0, 0);
    TestTrue(TEXT("Staging swap callback ran"), SwapCallbackRan);
    TestTrue(TEXT("Pinned staging either denies the swap or rejects the swapped write"),
        (!SwapSucceeded && SwapWriteSucceeded) || (SwapSucceeded && !SwapWriteSucceeded));
    if (SwapSucceeded)
    {
        TestTrue(TEXT("Rejected swapped write clears public outputs"),
            SwapDirectory.IsEmpty() && SwapManifest.ContentId.empty());
    }
    TArray<FString> OutsideFiles;
    IFileManager::Get().FindFilesRecursive(OutsideFiles, *SwapOutside, TEXT("*"), true, false);
    TestEqual(TEXT("Staging junction swap writes nothing outside the pinned root"),
        OutsideFiles.Num(), 0);
    if (!LinkedStage.IsEmpty() && IFileManager::Get().IsSymlink(*LinkedStage))
    {
        int32 Code = -1;
        FString Output, ProcessError;
        const FString Args = FString::Printf(TEXT("/c rmdir \"%s\""), *LinkedStage);
        FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Args,
            &Code, &Output, &ProcessError);
    }
    if (!HeldStage.IsEmpty()) IFileManager::Get().DeleteDirectory(*HeldStage, false, true);
    if (LinkedStage.IsEmpty() || !IFileManager::Get().DirectoryExists(*LinkedStage))
        IFileManager::Get().DeleteDirectory(*SwapRoot, false, true);
    IFileManager::Get().DeleteDirectory(*SwapOutside, false, true);
#endif
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreEditPersistenceTest,
    "MountainPlanner.P1.TerrainCore.EditPersistence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreEditPersistenceTest::RunTest(const FString& Parameters)
{
    const FString Root = TempRoot(TEXT("TerrainCoreEdits"));
    SkiPreparation::TerrainCorePackageStore Store(Root);
    const SkiDomain::Heightfield Field = MakeField(64, 64);
    FString BaseDirectory, Error;
    SkiDomain::TerrainCoreManifest BaseManifest;
    const bool BaseActivated = Store.WriteAndActivate(
        MakeCoreManifest(), Field, BaseDirectory, BaseManifest, Error);
    TestTrue(*FString::Printf(TEXT("Immutable base TerrainCore activates: %s"), *Error),
        BaseActivated);
    if (!BaseActivated)
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }
    const FString BaseManifestPath = FPaths::Combine(BaseDirectory, TEXT("terraincore.json"));
    FString BaseJsonBefore;
    TestTrue(TEXT("Base manifest can be captured"),
        FFileHelper::LoadFileToString(BaseJsonBefore, *BaseManifestPath));
    SkiDomain::TerrainEditSet Edits;
    Edits.TerrainCoreId = BaseManifest.ContentId;
    Edits.BaseRevision = 3;
    Edits.EditRevision = 4;
    Edits.Deltas = {{10, 12, 1.25F}, {11, 12, -0.5F}};
    FString Id;
    TestTrue(TEXT("Edit sidecar atomically activates"),
        Store.WriteEditSetAndActivate(Edits, 64, 64, Id, Error));
    auto StaleEditLease = MakeShared<SkiPreparation::PreparationOperationLease,
        ESPMode::ThreadSafe>(12, 13);
    StaleEditLease->Invalidate();
    FString ReusedEditId = TEXT("reuse-me");
    TestFalse(TEXT("Stale lease cannot publish an already-installed edit target"),
        Store.WriteEditSetAndActivate(Edits, 64, 64, ReusedEditId, Error,
            StaleEditLease, 12, 13));
    TestTrue(TEXT("Failed edit activation clears reused ID output"), ReusedEditId.IsEmpty());
    SkiDomain::TerrainEditSet Loaded;
    TestTrue(TEXT("Edit sidecar reopens"), Store.LoadEditSet(
        UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id, 64, 64, Loaded, Error));
    TestEqual(TEXT("Edit delta count persists"), static_cast<int32>(Loaded.Deltas.size()), 2);
    TestEqual(TEXT("Edit revision persists"), Loaded.EditRevision, SkiDomain::Revision(4));
    FString BaseJsonAfter;
    TestTrue(TEXT("Base manifest remains readable"),
        FFileHelper::LoadFileToString(BaseJsonAfter, *BaseManifestPath));
    TestEqual(TEXT("Edit persistence never mutates the base package"), BaseJsonAfter, BaseJsonBefore);

    SkiDomain::TerrainEditSet WrongTerrain = Edits;
    WrongTerrain.TerrainCoreId = std::string(64, 'b');
    TestFalse(TEXT("TerrainCore identity mismatch cannot load an edit"), Store.LoadEditSet(
        UTF8_TO_TCHAR(WrongTerrain.TerrainCoreId.c_str()), Id, 64, 64, Loaded, Error));
    TestTrue(TEXT("Failed edit load clears reused edit output"), Loaded.Deltas.empty()
        && Loaded.TerrainCoreId.empty());
    const FString EditPath = FPaths::Combine(Root, TEXT("TerrainEdits"),
        UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id, TEXT("edit.json"));
    const FString UndeclaredEditPath = FPaths::Combine(FPaths::GetPath(EditPath),
        TEXT("undeclared.bin"));
    TestTrue(TEXT("Undeclared edit attack file can be placed"),
        FFileHelper::SaveStringToFile(TEXT("attack"), *UndeclaredEditPath));
    TestFalse(TEXT("Edit package with undeclared file is rejected"), Store.LoadEditSet(
        UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id, 64, 64, Loaded, Error));
    IFileManager::Get().Delete(*UndeclaredEditPath, false, true, true);
    FString Json;
    TestTrue(TEXT("Edit can be read for attack"), FFileHelper::LoadFileToString(Json, *EditPath));
    TestTrue(TEXT("Edit attack mutation is applied"), FFileHelper::SaveStringToFile(
        Json + TEXT(" "), *EditPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestFalse(TEXT("Noncanonical edit content is rejected"), Store.LoadEditSet(
        UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id, 64, 64, Loaded, Error));
    const TArray<uint8> InvalidUtf8{0xedU, 0xa0U, 0x80U};
    TestTrue(TEXT("Invalid UTF-8 edit attack is applied"),
        FFileHelper::SaveArrayToFile(InvalidUtf8, *EditPath));
    TestFalse(TEXT("Invalid UTF-8 edit metadata is rejected before JSON decoding"),
        Store.LoadEditSet(UTF8_TO_TCHAR(Edits.TerrainCoreId.c_str()), Id,
            64, 64, Loaded, Error));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

#endif
