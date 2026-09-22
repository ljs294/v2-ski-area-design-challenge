#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiApplication/TerrainCoreEditedRepository.h"
#include "SkiApplication/TerrainCoreSession.h"
#include "SkiTerrainRuntime/TerrainCoreLodController.h"
#include "SkiTerrainRuntime/TerrainCoreMesh.h"
#include "SkiTerrainRuntime/TerrainCoreTileCache.h"

#include <atomic>
#include <algorithm>
#include <cmath>

namespace
{
constexpr const char* Hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

SkiDomain::TerrainCoreManifest MakeManifest(const std::uint32_t Width = 256,
    const std::uint32_t Height = 256)
{
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.ContentId = Hash;
    Manifest.GeneratorVersion = "runtime-test";
    Manifest.ProcessingVersions = {"runtime-test"};
    Manifest.Source.SourceId = "fixture";
    Manifest.Source.Product = "synthetic";
    Manifest.Source.AcquisitionEpoch = "2026-09-22";
    Manifest.Source.HorizontalCrs = "local-enu";
    Manifest.Source.HorizontalDatum = "fixture";
    Manifest.Source.VerticalDatum = "fixture";
    Manifest.Source.License = "fixture";
    Manifest.Source.Attribution = "fixture";
    Manifest.Source.NativeEastSpacingM = 1.0;
    Manifest.Source.NativeNorthSpacingM = 1.0;
    Manifest.Width = Width;
    Manifest.Height = Height;
    Manifest.DeliveredEastSpacingM = 1.0;
    Manifest.DeliveredNorthSpacingM = 1.0;
    Manifest.SampleCenterBounds = {0.0, 0.0, static_cast<double>(Width - 1U),
        static_cast<double>(Height - 1U)};
    SkiDomain::ComputeTerrainCoreBounds(Width, Height, 1.0, 1.0,
        Manifest.SampleCenterBounds, Manifest.OuterBounds);
    SkiDomain::TerrainCoreTilePlan Plan;
    SkiDomain::PlanTerrainCoreTiles(Width, Height, Plan);
    Manifest.Tiles = std::move(Plan.Tiles);
    std::uint64_t Cursor = 0;
    for (SkiDomain::TerrainCoreTileDescriptor& Tile : Manifest.Tiles)
    {
        const std::uint64_t StoredWidth = Tile.CoreWidth + Tile.HaloWest + Tile.HaloEast;
        const std::uint64_t StoredHeight = Tile.CoreHeight + Tile.HaloNorth + Tile.HaloSouth;
        const std::uint64_t Samples = StoredWidth * StoredHeight;
        Tile.HeightPath = "tiles.bin";
        Tile.HeightSha256 = Hash;
        Tile.HeightOffset = Cursor++;
        Tile.HeightBytes = 1;
        Tile.HeightRawBytes = Samples * sizeof(float);
        Tile.ValidityPath = "tiles.bin";
        Tile.ValiditySha256 = Hash;
        Tile.ValidityOffset = Cursor++;
        Tile.ValidityBytes = 1;
        Tile.ValidityRawBytes = (Samples + 7ULL) / 8ULL;
        Tile.ProvenanceId = "fixture";
        Tile.ProcessingVersion = "runtime-test";
    }
    Manifest.Shards.push_back({"tiles.bin", Hash, Cursor});
    return Manifest;
}

bool FillTile(const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
    SkiApplication::TerrainCoreTilePayload& Out, std::string&)
{
    const std::size_t StoredWidth = Descriptor.CoreWidth + Descriptor.HaloWest
        + Descriptor.HaloEast;
    const std::size_t StoredHeight = Descriptor.CoreHeight + Descriptor.HaloNorth
        + Descriptor.HaloSouth;
    Out.Heights.resize(StoredWidth * StoredHeight);
    Out.Validity.assign(Out.Heights.size(), 1);
    for (std::size_t Row = 0; Row < StoredHeight; ++Row)
    {
        for (std::size_t Column = 0; Column < StoredWidth; ++Column)
        {
            const std::uint32_t GlobalColumn = Descriptor.StartColumn
                + static_cast<std::uint32_t>(Column) - Descriptor.HaloWest;
            const std::uint32_t GlobalRow = Descriptor.StartRow
                + static_cast<std::uint32_t>(Row) - Descriptor.HaloNorth;
            Out.Heights[Row * StoredWidth + Column] = static_cast<float>(GlobalRow * 1000U
                + GlobalColumn + Descriptor.LodIndex * 100000U);
        }
    }
    return true;
}

std::shared_ptr<SkiApplication::TerrainCoreRepository> MakeRepository(
    std::atomic<int32>* ReadCount = nullptr, const float DelaySeconds = 0.0F)
{
    std::string Error;
    return SkiApplication::TerrainCoreRepository::Create(MakeManifest(511, 256),
        [ReadCount, DelaySeconds](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& Out, std::string& OutError)
        {
            if (ReadCount) ReadCount->fetch_add(1);
            if (DelaySeconds > 0.0F) FPlatformProcess::SleepNoStats(DelaySeconds);
            return FillTile(Descriptor, Out, OutError);
        }, Error);
}

class CancellableTestRepository final : public SkiApplication::ITerrainCoreRepository
{
public:
    explicit CancellableTestRepository(
        std::shared_ptr<const SkiApplication::ITerrainCoreRepository> InBase)
        : Base(std::move(InBase))
    {
    }

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata() const override
    {
        return Base->Metadata();
    }
    bool ReadTile(const SkiApplication::TerrainCoreTileKey& Key,
        SkiApplication::TerrainCoreTilePayload& Out, std::string& Error) const override
    {
        return Base->ReadTile(Key, Out, Error);
    }
    bool ReadTile(const SkiApplication::TerrainCoreTileKey& Key,
        SkiApplication::TerrainCoreTilePayload& Out, std::string& Error,
        const SkiApplication::TerrainCoreReadCancellation& Cancellation) const override
    {
        Entered.store(true);
        while (!Release.load() && !Cancellation.IsCancellationRequested())
        {
            FPlatformProcess::SleepNoStats(0.001F);
        }
        if (Cancellation.IsCancellationRequested())
        {
            Cancelled.store(true);
            Error = "cancelled";
            return false;
        }
        return Base->ReadTile(Key, Out, Error, Cancellation);
    }

    mutable std::atomic_bool Entered{false};
    mutable std::atomic_bool Release{false};
    mutable std::atomic_bool Cancelled{false};

private:
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Base;
};

class BlockingTestRepository final : public SkiApplication::ITerrainCoreRepository
{
public:
    explicit BlockingTestRepository(
        std::shared_ptr<const SkiApplication::ITerrainCoreRepository> InBase)
        : Base(std::move(InBase))
    {
    }

    std::shared_ptr<const SkiDomain::TerrainCoreManifest> Metadata() const override
    {
        return Base->Metadata();
    }
    bool ReadTile(const SkiApplication::TerrainCoreTileKey& Key,
        SkiApplication::TerrainCoreTilePayload& Out, std::string& Error) const override
    {
        return Base->ReadTile(Key, Out, Error);
    }
    bool ReadTile(const SkiApplication::TerrainCoreTileKey& Key,
        SkiApplication::TerrainCoreTilePayload& Out, std::string& Error,
        const SkiApplication::TerrainCoreReadCancellation&) const override
    {
        Entered.store(true);
        while (!Release.load()) FPlatformProcess::SleepNoStats(0.001F);
        return Base->ReadTile(Key, Out, Error);
    }

    mutable std::atomic_bool Entered{false};
    mutable std::atomic_bool Release{false};

private:
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Base;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreResidencyAndPublicationTest,
    "MountainPlanner.P1.TerrainCore.ResidencyAndPublication",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreResidencyAndPublicationTest::RunTest(const FString&)
{
    std::atomic<int32> Reads{0};
    const auto Repository = MakeRepository(&Reads, 0.01F);
    TestNotNull(TEXT("validated repository"), Repository.get());
    if (!Repository) return false;

    SkiApplication::TerrainCoreSession Session;
    TestTrue(TEXT("session installs immutable TerrainCore"), Session.Install(Repository));
    const SkiApplication::TerrainCoreSnapshot Installed = Session.Snapshot();
    TestTrue(TEXT("canonical is available before render/query publication"),
        Installed.CanonicalReady() && !Installed.RenderReady() && !Installed.QueryReady());
    TestFalse(TEXT("wrong generation cannot acknowledge render"),
        Session.AcknowledgeRender(Installed.Generation + 1, Installed.Revisions.Canonical));

    SkiTerrainRuntime::TerrainCoreCacheConfig Config;
    Config.ByteBudget = 350000;
    Config.MaximumWorkerJobs = 1;
    Config.MaximumPublicationsPerPump = 1;
    SkiTerrainRuntime::TerrainCoreTileCache Cache(Repository, Installed.Generation, Config);
    TestEqual(TEXT("configured cache budget is reported exactly"),
        Cache.Stats().ConfiguredBudgetBytes, Config.ByteBudget);
    const SkiApplication::TerrainCoreTileKey Finest{0, 0, 0};
    TestTrue(TEXT("first request accepted"), Cache.RequestTile(Finest));
    TestTrue(TEXT("duplicate request joins single flight"), Cache.RequestTile(Finest));
    TestTrue(TEXT("worker completed"), Cache.WaitForWorkers(2.0));
    TestEqual(TEXT("one physical read for duplicate request"), Reads.load(), 1);
    TestEqual(TEXT("publication is bounded"), Cache.PumpPublications(), 1U);
    const auto Resident = Cache.FindResident(Finest);
    TestNotNull(TEXT("tile resident after game-thread publication"), Resident.get());
    const auto FirstStats = Cache.Stats();
    TestEqual(TEXT("exact resident byte accounting"), FirstStats.ResidentBytes,
        Resident ? Resident->ResidentBytes() : 0ULL);
    TestTrue(TEXT("residency reported for matching generation"), Session.ReportResidency(
        Installed.Generation, {FirstStats.ResidentBytes, FirstStats.ResidentTiles,
            FirstStats.PendingTiles}));

    const SkiApplication::TerrainCoreTileKey Coarser{1, 0, 0};
    TestTrue(TEXT("resident tile can be pinned"), Cache.RequestTile(Finest, true));
    TestFalse(TEXT("pinned residency applies request backpressure"),
        Cache.RequestTile(Coarser));
    TestNotNull(TEXT("pinned tile is not evicted"), Cache.FindResident(Finest).get());
    TestTrue(TEXT("pin can be released"), Cache.ReleasePin(Finest));
    TestTrue(TEXT("coarser retry is accepted after pin release"), Cache.RequestTile(Coarser));
    TestTrue(TEXT("coarser retry worker completed"), Cache.WaitForWorkers(2.0));
    Cache.PumpPublications();
    const auto EvictedStats = Cache.Stats();
    TestNull(TEXT("least-recent unpinned tile is evicted"), Cache.FindResident(Finest).get());
    TestTrue(TEXT("eviction is recorded"), EvictedStats.EvictionCount >= 1);
    TestTrue(TEXT("resident bytes remain within budget"), EvictedStats.ResidentBytes <= Config.ByteBudget);
    TestTrue(TEXT("peak resident bytes remain within budget"), EvictedStats.PeakResidentBytes <= Config.ByteBudget);
    TestTrue(TEXT("all decoded cache states remain within budget"),
        EvictedStats.PeakAccountedBytes <= Config.ByteBudget
            && EvictedStats.AccountedBytes <= Config.ByteBudget);
    TestTrue(TEXT("worker concurrency remains bounded"), EvictedStats.PeakWorkerJobs <= Config.MaximumWorkerJobs);
    TestTrue(TEXT("publication concurrency remains bounded"),
        EvictedStats.PeakPublicationsPerPump <= Config.MaximumPublicationsPerPump);

    // The generation changes while this work is in flight. Its completion must be discarded.
    const SkiApplication::TerrainCoreTileKey Stale{2, 0, 0};
    Cache.RequestTile(Stale);
    Cache.ResetGeneration(Installed.Generation + 1, Repository);
    TestTrue(TEXT("stale worker completed"), Cache.WaitForWorkers(2.0));
    Cache.PumpPublications(8);
    TestNull(TEXT("stale completion is not published"), Cache.FindResident(Stale).get());
    TestEqual(TEXT("generation reset clears accounting"), Cache.Stats().ResidentBytes, 0ULL);

    SkiDomain::Revision Edited = 0;
    TestFalse(TEXT("revision-only edit transition is rejected"),
        Session.AdvanceEdit(Installed.Revisions.Canonical, Edited));
    const auto AfterEdit = Session.Snapshot();
    TestEqual(TEXT("rejected transition preserves canonical revision"),
        AfterEdit.Revisions.Canonical, Installed.Revisions.Canonical);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreRuntimeLodAndQueryTest,
    "MountainPlanner.P1.TerrainCore.RuntimeLodAndQuery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreRuntimeLodAndQueryTest::RunTest(const FString&)
{
    std::atomic<int32> Reads{0};
    const auto Repository = MakeRepository(&Reads);
    TestNotNull(TEXT("validated repository"), Repository.get());
    if (!Repository) return false;
    SkiTerrainRuntime::TerrainCoreTileCache Cache(Repository, 1);
    float Height = 0.0F;
    bool Valid = false;
    TestEqual(TEXT("canonical miss explicitly requests finest tile"),
        static_cast<uint8>(Cache.QueryCanonicalSample(17, 23, Height, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Pending));
    TestTrue(TEXT("finest tile read completes"), Cache.WaitForWorkers(2.0));
    Cache.PumpPublications();
    TestEqual(TEXT("canonical query becomes ready"),
        static_cast<uint8>(Cache.QueryCanonicalSample(17, 23, Height, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestTrue(TEXT("canonical sample is valid"), Valid);
    TestEqual(TEXT("canonical sample uses finest data"), Height, 23017.0F);
    TestEqual(TEXT("query never reads a visual derivative"), Reads.load(), 1);
    double CellHeight = 0.0;
    TestEqual(TEXT("north-east triangle query"), static_cast<uint8>(
        Cache.QueryCanonicalCell(17, 23, 0.75, 0.25, CellHeight, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestTrue(TEXT("north-east triangle is valid"), Valid);
    TestEqual(TEXT("north-east triangle follows NW-SE render split"), CellHeight, 23267.75);
    TestEqual(TEXT("south-west triangle query"), static_cast<uint8>(
        Cache.QueryCanonicalCell(17, 23, 0.25, 0.75, CellHeight, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestEqual(TEXT("south-west triangle follows NW-SE render split"), CellHeight, 23767.25);

    // A cell immediately east of the shared edge must request the adjacent finest tile.
    TestEqual(TEXT("shared-edge adjacent cell waits for its finest tile"), static_cast<uint8>(
        Cache.QueryCanonicalCell(255, 23, 0.5, 0.5, CellHeight, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Pending));
    TestTrue(TEXT("shared-edge tile read completes"), Cache.WaitForWorkers(2.0));
    Cache.PumpPublications();
    TestEqual(TEXT("shared-edge adjacent cell becomes queryable"), static_cast<uint8>(
        Cache.QueryCanonicalCell(255, 23, 0.5, 0.5, CellHeight, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestEqual(TEXT("shared-edge cell follows canonical split"), CellHeight, 23755.5);
    TestEqual(TEXT("out-of-bounds query is invalid"),
        static_cast<uint8>(Cache.QueryCanonicalSample(999, 0, Height, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Invalid));

    SkiDomain::TerrainEditSet EditSet;
    EditSet.TerrainCoreId = Repository->Metadata()->ContentId;
    EditSet.BaseRevision = 1;
    EditSet.EditRevision = 2;
    EditSet.Deltas = {{16, 24, 7.0F}, {17, 23, 10.0F},
        {254, 23, 3.0F}, {255, 23, 5.0F}};
    std::string EditError;
    const auto EditedRepository = SkiApplication::TerrainCoreEditedRepository::Create(
        Repository, EditSet, 1, EditError);
    TestNotNull(TEXT("valid offline edit overlay opens"), EditedRepository.get());
    if (!EditedRepository) return false;
    TestEqual(TEXT("edit revision is exposed"), EditedRepository->EditRevision(), 2ULL);
    SkiTerrainRuntime::TerrainCoreTileCache EditedCache(EditedRepository, 2);
    TestEqual(TEXT("edited canonical query initially waits"), static_cast<uint8>(
        EditedCache.QueryCanonicalSample(17, 23, Height, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Pending));
    TestTrue(TEXT("edited tile read completes"), EditedCache.WaitForWorkers(2.0));
    EditedCache.PumpPublications();
    TestEqual(TEXT("edited canonical query is ready"), static_cast<uint8>(
        EditedCache.QueryCanonicalSample(17, 23, Height, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestEqual(TEXT("offline edit changes finest canonical sample"), Height, 23027.0F);

    SkiApplication::TerrainCoreTilePayload Underlying;
    TestTrue(TEXT("underlying immutable repository remains readable"), Repository->ReadTile(
        {0, 0, 0}, Underlying, EditError));
    const std::size_t UnderlyingStoredWidth = Underlying.Descriptor.CoreWidth
        + Underlying.Descriptor.HaloWest + Underlying.Descriptor.HaloEast;
    const std::size_t UnderlyingIndex = (23U + Underlying.Descriptor.HaloNorth)
        * UnderlyingStoredWidth + 17U + Underlying.Descriptor.HaloWest;
    TestEqual(TEXT("underlying finest sample remains immutable"),
        Underlying.Heights[UnderlyingIndex], 23017.0F);
    SkiApplication::TerrainCoreTilePayload VisualBase;
    SkiApplication::TerrainCoreTilePayload VisualEdited;
    TestTrue(TEXT("base visual derivative reads"), Repository->ReadTile(
        {1, 0, 0}, VisualBase, EditError));
    TestTrue(TEXT("edited repository visual derivative reads"), EditedRepository->ReadTile(
        {1, 0, 0}, VisualEdited, EditError));
    const std::uint32_t VisualWidth = VisualEdited.Descriptor.CoreWidth
        + VisualEdited.Descriptor.HaloWest + VisualEdited.Descriptor.HaloEast;
    const std::size_t VisualEditedIndex = (12U + VisualEdited.Descriptor.HaloNorth)
        * VisualWidth + 8U + VisualEdited.Descriptor.HaloWest;
    TestEqual(TEXT("visual derivative is rebuilt from edited finest data"),
        VisualEdited.Heights[VisualEditedIndex], 24023.0F);
    TestTrue(TEXT("edited derivative never reuses stale base pyramid payload"),
        VisualBase.Heights != VisualEdited.Heights);

    SkiApplication::TerrainCoreTilePayload EditedWestTile;
    SkiApplication::TerrainCoreTilePayload EditedEastTile;
    TestTrue(TEXT("edited west finest tile reads"), EditedRepository->ReadTile(
        {0, 0, 0}, EditedWestTile, EditError));
    TestTrue(TEXT("edited east finest tile reads"), EditedRepository->ReadTile(
        {0, 1, 0}, EditedEastTile, EditError));
    const auto GlobalSample = [](const SkiApplication::TerrainCoreTilePayload& Tile,
        const std::uint32_t Column, const std::uint32_t Row)
    {
        const std::uint32_t StoredWidth = Tile.Descriptor.CoreWidth
            + Tile.Descriptor.HaloWest + Tile.Descriptor.HaloEast;
        const std::uint32_t LocalColumn = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(Column) - Tile.Descriptor.StartColumn
            + Tile.Descriptor.HaloWest);
        const std::uint32_t LocalRow = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(Row) - Tile.Descriptor.StartRow
            + Tile.Descriptor.HaloNorth);
        return Tile.Heights[static_cast<std::size_t>(LocalRow) * StoredWidth + LocalColumn];
    };
    TestEqual(TEXT("core occurrence receives boundary-neighbor edit"),
        GlobalSample(EditedWestTile, 254, 23), 23257.0F);
    TestEqual(TEXT("halo occurrence receives the same boundary-neighbor edit"),
        GlobalSample(EditedEastTile, 254, 23), 23257.0F);

    double WestEdge = 0.0;
    double EastEdge = 0.0;
    TestEqual(TEXT("edited west shared-edge tile queues"), static_cast<uint8>(
        EditedCache.QueryCanonicalCell(254, 23, 1.0, 0.0, WestEdge, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestEqual(TEXT("edited east shared-edge tile queues"), static_cast<uint8>(
        EditedCache.QueryCanonicalCell(255, 23, 0.0, 0.0, EastEdge, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Pending));
    TestTrue(TEXT("edited east shared-edge tile loads"), EditedCache.WaitForWorkers(2.0));
    EditedCache.PumpPublications();
    TestEqual(TEXT("edited east shared-edge tile is ready"), static_cast<uint8>(
        EditedCache.QueryCanonicalCell(255, 23, 0.0, 0.0, EastEdge, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestEqual(TEXT("shared core occurrences receive identical edit"), WestEdge, EastEdge);
    TestEqual(TEXT("shared-edge edit value"), WestEdge, 23260.0);

    SkiDomain::TerrainEditSet Mismatch = EditSet;
    Mismatch.TerrainCoreId = Hash;
    Mismatch.TerrainCoreId[0] = 'f';
    TestNull(TEXT("core-ID mismatch is rejected"),
        SkiApplication::TerrainCoreEditedRepository::Create(
            Repository, Mismatch, 1, EditError).get());
    TestNull(TEXT("stale expected base revision is rejected"),
        SkiApplication::TerrainCoreEditedRepository::Create(
            Repository, EditSet, 2, EditError).get());
    SkiDomain::TerrainEditSet Malformed = EditSet;
    Malformed.Deltas.push_back(Malformed.Deltas.front());
    TestNull(TEXT("malformed duplicate edit is rejected"),
        SkiApplication::TerrainCoreEditedRepository::Create(
            Repository, Malformed, 1, EditError).get());

    SkiTerrainRuntime::TerrainCoreLodController Lods;
    std::vector<std::uint8_t> Selected;
    const std::vector<double> Pixels{3.0, 0.01, 0.01};
    for (int32 Step = 0; Step < 5; ++Step)
    {
        TestTrue(TEXT("LOD grid update"), Lods.Update(3, 1, Pixels, Selected));
    }
    TestEqual(TEXT("LOD output count"), Selected.size(), static_cast<std::size_t>(3));
    TestTrue(TEXT("first edge respects adjacent-level rule"),
        SkiDomain::AreTerrainCoreLodsAdjacent(Selected[0], Selected[1]));
    TestTrue(TEXT("second edge respects adjacent-level rule"),
        SkiDomain::AreTerrainCoreLodsAdjacent(Selected[1], Selected[2]));
    TestTrue(TEXT("distance can still coarsen away from refined tile"),
        Selected[2] > Selected[0]);
    const std::vector<std::uint8_t> BeforeHysteresis = Selected;
    TestTrue(TEXT("hysteresis update"), Lods.Update(3, 1, {1.0, 0.5, 0.25}, Selected));
    TestTrue(TEXT("deadband preserves stabilized LOD"), Selected == BeforeHysteresis);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreAdversarialCacheTest,
    "MountainPlanner.P1.TerrainCore.AdversarialCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreAdversarialCacheTest::RunTest(const FString&)
{
    std::atomic<int32> Reads{0};
    const auto Repository = MakeRepository(&Reads, 0.01F);
    SkiTerrainRuntime::TerrainCoreCacheConfig Config;
    Config.ByteBudget = 350000;
    Config.MaximumWorkerJobs = 1;
    SkiTerrainRuntime::TerrainCoreTileCache Cache(Repository, 1, Config);
    const SkiApplication::TerrainCoreTileKey Key{0, 0, 0};
    TestTrue(TEXT("initial pinned request accepted"), Cache.RequestTile(Key, true));
    TestTrue(TEXT("pending pin can be released before publication"), Cache.ReleasePin(Key));
    for (int32 Index = 0; Index < 5000; ++Index)
    {
        TestTrue(TEXT("stalled-publication duplicate joins one flight"), Cache.RequestTile(Key));
    }
    TestTrue(TEXT("single decode completes"), Cache.WaitForWorkers(3.0));
    for (int32 Index = 0; Index < 5000; ++Index)
    {
        TestTrue(TEXT("completed unpublished request remains single-flight"), Cache.RequestTile(Key));
    }
    TestEqual(TEXT("thousands of duplicates launch one physical read"), Reads.load(), 1);
    const auto BeforePublish = Cache.Stats();
    TestEqual(TEXT("completed payload remains budget-accounted"),
        BeforePublish.AccountedBytes, BeforePublish.ReservedBytes);
    TestTrue(TEXT("completed payload stays inside hard byte budget"),
        BeforePublish.AccountedBytes <= Config.ByteBudget);
    TestEqual(TEXT("one publication commits the joined request"), Cache.PumpPublications(), 1U);
    TestNotNull(TEXT("released pending pin does not prevent later residency"),
        Cache.FindResident(Key).get());

    const auto Slow = std::make_shared<CancellableTestRepository>(Repository);
    SkiTerrainRuntime::TerrainCoreTileCache GenerationCache(Slow, 7, Config);
    TestTrue(TEXT("old generation request accepted"), GenerationCache.RequestTile(Key));
    for (int32 Spin = 0; Spin < 1000 && !Slow->Entered.load(); ++Spin)
    {
        FPlatformProcess::SleepNoStats(0.001F);
    }
    TestTrue(TEXT("old generation entered repository read"), Slow->Entered.load());
    GenerationCache.ResetGeneration(8, Repository);
    for (int32 Spin = 0; Spin < 1000 && !Slow->Cancelled.load(); ++Spin)
    {
        FPlatformProcess::SleepNoStats(0.001F);
    }
    TestTrue(TEXT("repository observes cooperative cancellation token"), Slow->Cancelled.load());
    TestTrue(TEXT("cancelled old worker exits before its slot is reused"),
        GenerationCache.WaitForWorkers(2.0));
    TestTrue(TEXT("new generation request proceeds after cancellation exits"),
        GenerationCache.RequestTile(Key));
    TestTrue(TEXT("new generation completes"), GenerationCache.WaitForWorkers(2.0));
    TestEqual(TEXT("new generation publishes"), GenerationCache.PumpPublications(), 1U);
    TestTrue(TEXT("generation reset keeps accounting bounded"),
        GenerationCache.Stats().AccountedBytes <= Config.ByteBudget);

    std::atomic<int32> CurrentReads{0};
    const auto CurrentRepository = MakeRepository(&CurrentReads);
    const auto Blocked = std::make_shared<BlockingTestRepository>(Repository);
    SkiTerrainRuntime::TerrainCoreCacheConfig ResetConfig;
    ResetConfig.ByteBudget = 700000;
    ResetConfig.MaximumWorkerJobs = 1;
    SkiTerrainRuntime::TerrainCoreTileCache ResetCache(Blocked, 100, ResetConfig);
    TestTrue(TEXT("non-interruptible old request accepted"), ResetCache.RequestTile(Key));
    for (int32 Spin = 0; Spin < 1000 && !Blocked->Entered.load(); ++Spin)
    {
        FPlatformProcess::SleepNoStats(0.001F);
    }
    TestTrue(TEXT("non-interruptible old reader is blocked"), Blocked->Entered.load());
    for (std::uint64_t Generation = 101; Generation <= 120; ++Generation)
    {
        ResetCache.ResetGeneration(Generation, CurrentRepository);
        TestTrue(TEXT("latest generation queues behind globally bounded worker"),
            ResetCache.RequestTile(Key));
        const auto DuringReset = ResetCache.Stats();
        TestTrue(TEXT("repeated reset never exceeds total worker cap"),
            DuringReset.ActiveWorkerJobs <= ResetConfig.MaximumWorkerJobs
                && DuringReset.PeakWorkerJobs <= ResetConfig.MaximumWorkerJobs);
        TestTrue(TEXT("repeated reset retains bounded in-flight reservations"),
            DuringReset.AccountedBytes <= ResetConfig.ByteBudget
                && DuringReset.PeakAccountedBytes <= ResetConfig.ByteBudget);
    }
    TestEqual(TEXT("new generation does not start while global slot is blocked"),
        CurrentReads.load(), 0);
    Blocked->Release.store(true);
    TestTrue(TEXT("old exit releases slot and runs latest generation"),
        ResetCache.WaitForWorkers(3.0));
    TestEqual(TEXT("only latest generation performs a current read"), CurrentReads.load(), 1);
    TestEqual(TEXT("latest generation publishes after old exit"),
        ResetCache.PumpPublications(), 1U);
    const auto AfterResets = ResetCache.Stats();
    TestTrue(TEXT("actual global peak workers remains capped"),
        AfterResets.PeakWorkerJobs <= ResetConfig.MaximumWorkerJobs);
    TestTrue(TEXT("actual global peak accounting remains capped"),
        AfterResets.PeakAccountedBytes <= ResetConfig.ByteBudget);

    std::atomic<int32> FailedReads{0};
    std::string FailureCreateError;
    const auto FailingRepository = SkiApplication::TerrainCoreRepository::Create(
        MakeManifest(511, 256),
        [&FailedReads](const SkiDomain::TerrainCoreTileDescriptor&,
            SkiApplication::TerrainCoreTilePayload&, std::string& Error)
        {
            FailedReads.fetch_add(1);
            Error = "deterministic decode failure";
            return false;
        }, FailureCreateError);
    SkiTerrainRuntime::TerrainCoreTileCache FailureCache(FailingRepository, 1, Config);
    TestTrue(TEXT("failing request is initially accepted"), FailureCache.RequestTile(Key));
    TestTrue(TEXT("failing request reaches terminal state"), FailureCache.WaitForWorkers(2.0));
    std::string FailureError;
    TestEqual(TEXT("terminal request state is exposed"), static_cast<uint8>(
        FailureCache.TileStatus(Key, &FailureError)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreRequestStatus::Failed));
    TestTrue(TEXT("terminal request error is exposed"),
        FailureError == "deterministic decode failure");
    float FailedHeight = 0.0F;
    bool FailedValid = false;
    TestEqual(TEXT("canonical query exposes terminal failure"), static_cast<uint8>(
        FailureCache.QueryCanonicalSample(1, 1, FailedHeight, FailedValid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Failed));
    TestFalse(TEXT("ordinary request does not spin on terminal failure"),
        FailureCache.RequestTile(Key));
    TestEqual(TEXT("terminal failure launches only one read"), FailedReads.load(), 1);
    TestTrue(TEXT("explicit retry clears terminal state and requeues"),
        FailureCache.RetryTile(Key));
    TestTrue(TEXT("retry reaches terminal state"), FailureCache.WaitForWorkers(2.0));
    TestEqual(TEXT("explicit retry performs exactly one additional read"),
        FailedReads.load(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreAtomicEditAndMeshTest,
    "MountainPlanner.P1.TerrainCore.AtomicEditAndMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreAtomicEditAndMeshTest::RunTest(const FString&)
{
    const auto Repository = MakeRepository();
    SkiDomain::TerrainEditSet Edits;
    Edits.TerrainCoreId = Repository->Metadata()->ContentId;
    Edits.BaseRevision = 1;
    Edits.EditRevision = 2;
    Edits.Deltas = {{16, 24, 7.0F}};
    std::string Error;
    const auto Edited = SkiApplication::TerrainCoreEditedRepository::Create(
        Repository, Edits, 1, Error);
    TestNotNull(TEXT("edited repository opens"), Edited.get());
    if (!Edited) return false;

    SkiApplication::TerrainCoreSession Session;
    TestTrue(TEXT("base session installs"), Session.Install(Repository, 1));
    const auto Before = Session.Snapshot();
    SkiApplication::TerrainCoreSnapshot Published;
    TestTrue(TEXT("repository and revision publish in one generation transition"),
        Session.PublishEdit(Before.Generation, Before.Revisions.Canonical,
            Edited, Edits.EditRevision, Published));
    TestEqual(TEXT("edit publication advances generation"),
        Published.Generation, Before.Generation + 1U);
    TestTrue(TEXT("published snapshot exposes edited repository"),
        Published.Repository == Edited && Published.Revisions.Canonical == 2
            && Published.Revisions.Edit == 2 && !Published.QueryReady()
            && !Published.RenderReady());
    TestTrue(TEXT("edited render readiness acknowledges published generation"),
        Session.AcknowledgeRender(Published.Generation, Published.Revisions.Canonical));
    TestTrue(TEXT("edited query readiness acknowledges published generation"),
        Session.AcknowledgeQuery(Published.Generation, Published.Revisions.Canonical));
    TestTrue(TEXT("edited generation becomes jointly ready"),
        Session.Snapshot().RenderReady() && Session.Snapshot().QueryReady());
    SkiApplication::TerrainCoreSnapshot Rejected;
    TestFalse(TEXT("stale publication cannot overwrite newer repository"),
        Session.PublishEdit(Before.Generation, 1, Repository, 3, Rejected));
    TestTrue(TEXT("stale rejection leaves atomic snapshot unchanged"),
        Session.Snapshot().Repository == Edited && Session.Snapshot().Generation == Published.Generation);

    SkiApplication::TerrainCoreTilePayload Coarse;
    TestTrue(TEXT("edited LOD payload derives from edited LOD0"),
        Edited->ReadTile({1, 0, 0}, Coarse, Error));
    SkiDomain::TerrainTileMesh Mesh;
    TestTrue(TEXT("edited derivative builds render mesh"),
        SkiTerrainRuntime::BuildTerrainCoreTileMesh(Coarse, *Edited->Metadata(), 2,
            true, 3.0, Mesh));
    const std::uint32_t Width = Coarse.Descriptor.CoreWidth;
    const std::size_t VertexIndex = 12U * Width + 8U;
    TestEqual(TEXT("edited LOD mesh height matches derivative payload"),
        Mesh.Vertices[VertexIndex].UpM, 24023.0F);
    TestTrue(TEXT("mesh uses canonical NW-SW-SE then NW-SE-NE diagonal"),
        Mesh.Indices.size() >= 6 && Mesh.Indices[0] == 0U
            && Mesh.Indices[1] == Width && Mesh.Indices[2] == Width + 1U
            && Mesh.Indices[3] == 0U && Mesh.Indices[4] == Width + 1U
            && Mesh.Indices[5] == 1U);
    TestTrue(TEXT("skirt adds border vertices"), Mesh.Vertices.size() >
        static_cast<std::size_t>(Coarse.Descriptor.CoreWidth) * Coarse.Descriptor.CoreHeight);

    SkiApplication::TerrainCoreTilePayload Finest;
    TestTrue(TEXT("finest tile reads for normal-ring proof"),
        Edited->ReadTile({0, 0, 0}, Finest, Error));
    const std::uint32_t StoredWidth = Finest.Descriptor.CoreWidth
        + Finest.Descriptor.HaloWest + Finest.Descriptor.HaloEast;
    const std::uint32_t EdgeStoredColumn = Finest.Descriptor.HaloWest
        + Finest.Descriptor.CoreWidth - 1U;
    const std::uint32_t EdgeStoredRow = Finest.Descriptor.HaloNorth + 20U;
    Finest.Heights[static_cast<std::size_t>(EdgeStoredRow) * StoredWidth
        + EdgeStoredColumn + 1U] += 5000.0F;
    SkiDomain::TerrainTileMesh HaloMesh;
    TestTrue(TEXT("mesh with modified normal-neighbor ring builds"),
        SkiTerrainRuntime::BuildTerrainCoreTileMesh(Finest, *Edited->Metadata(), 2,
            false, 0.0, HaloMesh));
    const std::size_t EdgeVertex = 20U * Finest.Descriptor.CoreWidth
        + Finest.Descriptor.CoreWidth - 1U;
    TestTrue(TEXT("edge normal consults east halo sample"),
        std::abs(HaloMesh.Vertices[EdgeVertex].NormalEast) > 0.4F);

    const auto PartialRepository = SkiApplication::TerrainCoreRepository::Create(
        MakeManifest(300, 260), FillTile, Error);
    SkiApplication::TerrainCoreTilePayload Partial;
    TestTrue(TEXT("partial southeast edge tile reads"),
        PartialRepository && PartialRepository->ReadTile({0, 1, 1}, Partial, Error));
    SkiDomain::TerrainTileMesh PartialMesh;
    TestTrue(TEXT("partial edge tile with skirts builds"), PartialRepository
        && SkiTerrainRuntime::BuildTerrainCoreTileMesh(Partial,
            *PartialRepository->Metadata(), 2, true, 2.0, PartialMesh));
    const std::size_t PartialCoreVertices = static_cast<std::size_t>(
        Partial.Descriptor.CoreWidth) * Partial.Descriptor.CoreHeight;
    TestTrue(TEXT("partial edge dimensions and skirt ring are preserved"),
        Partial.Descriptor.CoreWidth == 45 && Partial.Descriptor.CoreHeight == 5
            && PartialMesh.Vertices.size() > PartialCoreVertices);

    SkiApplication::TerrainCoreTilePayload AllNoData = Finest;
    std::fill(AllNoData.Validity.begin(), AllNoData.Validity.end(), 0);
    SkiDomain::TerrainTileMesh NoDataMesh = Mesh;
    TestTrue(TEXT("structurally valid all-nodata tile terminates successfully"),
        SkiTerrainRuntime::BuildTerrainCoreTileMesh(AllNoData, *Edited->Metadata(), 2,
            true, 2.0, NoDataMesh));
    TestTrue(TEXT("all-nodata build publishes no useless geometry"),
        NoDataMesh.Vertices.empty() && NoDataMesh.Indices.empty());

    SkiTerrainRuntime::TerrainCoreTileCache QueryCache(Edited, Published.Generation);
    float Height = 0.0F;
    bool Valid = false;
    QueryCache.QueryCanonicalSample(16, 24, Height, Valid);
    TestTrue(TEXT("edited query tile completes"), QueryCache.WaitForWorkers(2.0));
    QueryCache.PumpPublications();
    TestEqual(TEXT("edited query ready"), static_cast<uint8>(
        QueryCache.QueryCanonicalSample(16, 24, Height, Valid)),
        static_cast<uint8>(SkiTerrainRuntime::TerrainCoreQueryStatus::Ready));
    TestTrue(TEXT("edited LOD mesh and canonical query are coherent"),
        Valid && Height == Mesh.Vertices[VertexIndex].UpM);

    std::atomic<int32> RepeatedBaseReads{0};
    const auto RepeatedBase = MakeRepository(&RepeatedBaseReads);
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repeated = RepeatedBase;
    SkiDomain::Revision RepeatedRevision = 1;
    constexpr std::uint32_t RepeatedEditCount = 256;
    for (std::uint32_t Step = 0; Step < RepeatedEditCount; ++Step)
    {
        SkiDomain::TerrainEditSet StepEdit;
        StepEdit.TerrainCoreId = RepeatedBase->Metadata()->ContentId;
        StepEdit.BaseRevision = RepeatedRevision;
        StepEdit.EditRevision = RepeatedRevision + 1U;
        StepEdit.Deltas = {{16, 24, 0.25F}};
        const auto Next = SkiApplication::TerrainCoreEditedRepository::Create(
            Repeated, std::move(StepEdit), RepeatedRevision, Error);
        if (!Next)
        {
            AddError(FString::Printf(TEXT("repeated edit %u failed: %s"), Step,
                UTF8_TO_TCHAR(Error.c_str())));
            return false;
        }
        Repeated = Next;
        ++RepeatedRevision;
    }
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> FlattenedBase;
    std::shared_ptr<const SkiDomain::TerrainEditSet> CumulativeEdits;
    TestTrue(TEXT("repeated edits expose one flattened overlay"),
        Repeated->FlattenEditOverlay(FlattenedBase, CumulativeEdits));
    TestTrue(TEXT("repeated edits retain the immutable package repository"),
        FlattenedBase == RepeatedBase);
    TestTrue(TEXT("repeated edits merge duplicate sparse samples"), CumulativeEdits
        && CumulativeEdits->BaseRevision == 1
        && CumulativeEdits->EditRevision == RepeatedRevision
        && CumulativeEdits->Deltas.size() == 1
        && CumulativeEdits->Deltas.front().DeltaM == 64.0F);
    SkiDomain::Revision TransitionBase = 0;
    SkiDomain::Revision TransitionEdit = 0;
    TestTrue(TEXT("flattened overlay preserves the latest atomic transition"),
        Repeated->EditTransition(TransitionBase, TransitionEdit)
        && TransitionBase == RepeatedRevision - 1U
        && TransitionEdit == RepeatedRevision);
    SkiApplication::TerrainCoreTilePayload RepeatedTile;
    TestTrue(TEXT("flattened repeated edit tile reads"),
        Repeated->ReadTile({0, 0, 0}, RepeatedTile, Error));
    const std::uint32_t RepeatedStoredWidth = RepeatedTile.Descriptor.CoreWidth
        + RepeatedTile.Descriptor.HaloWest + RepeatedTile.Descriptor.HaloEast;
    const std::size_t RepeatedIndex = static_cast<std::size_t>(
        24U + RepeatedTile.Descriptor.HaloNorth) * RepeatedStoredWidth
        + 16U + RepeatedTile.Descriptor.HaloWest;
    TestEqual(TEXT("repeated edit read applies the cumulative sparse delta"),
        RepeatedTile.Heights[RepeatedIndex], 24080.0F);
    TestEqual(TEXT("repeated edit read reaches the package exactly once"),
        RepeatedBaseReads.load(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainCoreSaddleAndSpatialLodTest,
    "MountainPlanner.P1.TerrainCore.SaddleAndSpatialLod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainCoreSaddleAndSpatialLodTest::RunTest(const FString&)
{
    std::string Error;
    auto SaddleRepository = SkiApplication::TerrainCoreRepository::Create(MakeManifest(256, 256),
        [](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& Out, std::string& OutError)
        {
            if (!FillTile(Descriptor, Out, OutError)) return false;
            const std::uint32_t StoredWidth = Descriptor.CoreWidth
                + Descriptor.HaloWest + Descriptor.HaloEast;
            const auto Set = [&](const std::uint32_t X, const std::uint32_t Y, const float Value)
            {
                Out.Heights[static_cast<std::size_t>(Y + Descriptor.HaloNorth) * StoredWidth
                    + X + Descriptor.HaloWest] = Value;
            };
            Set(10, 10, 0.0F); Set(11, 10, 0.0F);
            Set(10, 11, 0.0F); Set(11, 11, 10.0F);
            return true;
        }, Error);
    SkiTerrainRuntime::TerrainCoreTileCache Cache(SaddleRepository, 1);
    double Height = 0.0;
    bool Valid = false;
    Cache.QueryCanonicalCell(10, 10, 0.75, 0.25, Height, Valid);
    TestTrue(TEXT("saddle tile completes"), Cache.WaitForWorkers(2.0));
    Cache.PumpPublications();
    Cache.QueryCanonicalCell(10, 10, 0.75, 0.25, Height, Valid);
    TestTrue(TEXT("north-east saddle uses NW-SE render diagonal"), Valid && Height == 2.5);
    Cache.QueryCanonicalCell(10, 10, 0.25, 0.75, Height, Valid);
    TestTrue(TEXT("south-west saddle uses NW-SE render diagonal"), Valid && Height == 2.5);

    auto TriangleValidityRepository = SkiApplication::TerrainCoreRepository::Create(
        MakeManifest(256, 256),
        [](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& Out, std::string& OutError)
        {
            if (!FillTile(Descriptor, Out, OutError)) return false;
            const std::uint32_t StoredWidth = Descriptor.CoreWidth
                + Descriptor.HaloWest + Descriptor.HaloEast;
            const std::size_t NorthEast = static_cast<std::size_t>(10U + Descriptor.HaloNorth)
                * StoredWidth + 11U + Descriptor.HaloWest;
            Out.Validity[NorthEast] = 0;
            return true;
        }, Error);
    TestNotNull(TEXT("triangle-validity repository opens"), TriangleValidityRepository.get());
    if (!TriangleValidityRepository) return false;
    SkiTerrainRuntime::TerrainCoreTileCache TriangleValidityCache(
        TriangleValidityRepository, 1);
    TriangleValidityCache.QueryCanonicalCell(10, 10, 0.25, 0.75, Height, Valid);
    TestTrue(TEXT("triangle-validity tile completes"),
        TriangleValidityCache.WaitForWorkers(2.0));
    TriangleValidityCache.PumpPublications();
    TriangleValidityCache.QueryCanonicalCell(10, 10, 0.25, 0.75, Height, Valid);
    TestTrue(TEXT("unused no-data corner does not reject rendered south-west triangle"), Valid);
    TriangleValidityCache.QueryCanonicalCell(10, 10, 0.75, 0.25, Height, Valid);
    TestFalse(TEXT("no-data corner rejects the triangle that actually uses it"), Valid);

    SkiTerrainRuntime::TerrainCoreLodController Controller;
    std::vector<SkiApplication::TerrainCoreTileKey> Keys;
    const std::vector<double> Pixels(16U * 8U, 0.01);
    Controller.Reset(4);
    TestTrue(TEXT("spatial LOD selection starts from requested overview"),
        Controller.SelectTileKeys(16, 8, Pixels, Keys));
    std::vector<std::uint8_t> Coverage(16U * 8U, 0);
    for (const auto& Key : Keys)
    {
        const std::uint32_t Factor = SkiDomain::TerrainCoreLodFactors[Key.Lod];
        const std::uint32_t StartX = Key.X * Factor;
        const std::uint32_t StartY = Key.Y * Factor;
        for (std::uint32_t Y = StartY; Y < std::min(8U, StartY + Factor); ++Y)
        {
            for (std::uint32_t X = StartX; X < std::min(16U, StartX + Factor); ++X)
            {
                ++Coverage[static_cast<std::size_t>(Y) * 16U + X];
            }
        }
    }
    TestTrue(TEXT("spatial LOD keys cover every finest region exactly once"),
        std::all_of(Coverage.begin(), Coverage.end(), [](const std::uint8_t Count)
        {
            return Count == 1;
        }));
    TestTrue(TEXT("coarse key coordinates are pyramid coordinates, not finest coordinates"),
        Keys.size() == 1 && Keys[0].Lod == 4 && Keys[0].X == 0 && Keys[0].Y == 0);

    // A relaxed finest row can still coalesce badly: cells 4-7 emit LOD 1 while
    // cells 8-15 would greedily emit LOD 3. The emitted keys, not only the desired
    // finest-cell field, must obey the adjacent-level rule.
    Controller.Reset(0);
    const std::vector<double> AdversarialPixels{
        1.0, 1.0, 1.0, 1.0, 0.5, 0.25, 0.25, 0.25,
        0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125, 0.125};
    for (int32 Step = 0; Step < 4; ++Step)
    {
        TestTrue(TEXT("adversarial row selection converges"),
            Controller.SelectTileKeys(16, 1, AdversarialPixels, Keys));
    }
    std::vector<std::uint8_t> EmittedLods(16U, 255U);
    for (const SkiApplication::TerrainCoreTileKey& Key : Keys)
    {
        const std::uint32_t Factor = SkiDomain::TerrainCoreLodFactors[Key.Lod];
        const std::uint32_t StartX = Key.X * Factor;
        for (std::uint32_t X = StartX; X < std::min(16U, StartX + Factor); ++X)
            EmittedLods[X] = Key.Lod;
    }
    bool bEmittedNeighborsAreAdjacent = true;
    for (std::size_t X = 1; X < EmittedLods.size(); ++X)
    {
        bEmittedNeighborsAreAdjacent = bEmittedNeighborsAreAdjacent
            && SkiDomain::AreTerrainCoreLodsAdjacent(EmittedLods[X - 1U], EmittedLods[X]);
    }
    TestTrue(TEXT("coalesced adversarial row preserves emitted-key adjacency"),
        bEmittedNeighborsAreAdjacent);
    TestTrue(TEXT("offending LOD 3 block is split to meet its LOD 1 neighbor"),
        std::none_of(Keys.begin(), Keys.end(), [](const SkiApplication::TerrainCoreTileKey& Key)
        {
            return Key.Lod == 3;
        }));
    return true;
}

#endif
