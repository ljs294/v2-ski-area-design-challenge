#include "SkiTerrainCoreRegression.h"

#include "SkiApplication/TerrainCoreEditedRepository.h"
#include "SkiApplication/TerrainCoreRepository.h"
#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainCore.h"
#include "SkiPreparation/TerrainCorePackageStore.h"
#include "SkiPreparation/TerrainPackageStore.h"
#include "SkiPreparation/TerrainPreparation.h"
#include "SkiTerrainRuntime/TerrainCoreTileCache.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <set>
#include <string>

namespace
{
constexpr uint32 FixtureWidth = 520;
constexpr uint32 FixtureHeight = 300;
constexpr double FixtureWestM = 100.0;
constexpr double FixtureNorthM = 500.0;
constexpr double FixtureSpacingM = 1.0;
constexpr uint32 QueryColumn = 300;
constexpr uint32 QueryRow = 200;

float FixtureHeightAt(const uint32 Column, const uint32 Row)
{
    return 1200.0F + static_cast<float>(Column) * 0.5F
        + static_cast<float>(Row) * 0.25F;
}

SkiDomain::Heightfield BuildFixture()
{
    SkiDomain::Heightfield Field;
    Field.Width = FixtureWidth;
    Field.Height = FixtureHeight;
    Field.WestM = FixtureWestM;
    Field.NorthM = FixtureNorthM;
    Field.EastSpacingM = FixtureSpacingM;
    Field.NorthSpacingM = FixtureSpacingM;
    Field.NoDataValue = -9999.0;
    Field.CurrentRevision = 1;
    Field.Samples.resize(static_cast<std::size_t>(Field.Width) * Field.Height);
    for (uint32 Row = 0; Row < Field.Height; ++Row)
    {
        for (uint32 Column = 0; Column < Field.Width; ++Column)
        {
            Field.Samples[static_cast<std::size_t>(Row) * Field.Width + Column]
                = FixtureHeightAt(Column, Row);
        }
    }
    return Field;
}

SkiDomain::TerrainCoreManifest BuildManifest()
{
    SkiDomain::TerrainCoreManifest Manifest;
    Manifest.GeneratorVersion = "terraincore-packaged-regression-v1";
    Manifest.ProcessingVersions = {"synthetic-ground-v1", "terraincore-derivation-v1"};
    Manifest.Width = FixtureWidth;
    Manifest.Height = FixtureHeight;
    Manifest.DeliveredEastSpacingM = FixtureSpacingM;
    Manifest.DeliveredNorthSpacingM = FixtureSpacingM;
    Manifest.Registration = SkiDomain::PixelRegistration::SampleCenter;
    Manifest.SampleCenterBounds = {FixtureWestM,
        FixtureNorthM - static_cast<double>(FixtureHeight - 1U) * FixtureSpacingM,
        FixtureWestM + static_cast<double>(FixtureWidth - 1U) * FixtureSpacingM,
        FixtureNorthM};
    SkiDomain::ComputeTerrainCoreBounds(Manifest.Width, Manifest.Height,
        Manifest.DeliveredEastSpacingM, Manifest.DeliveredNorthSpacingM,
        Manifest.SampleCenterBounds, Manifest.OuterBounds);
    Manifest.Source.SourceId = "packaged-synthetic-ground";
    Manifest.Source.Product = "immutable asymmetric TerrainCore regression";
    Manifest.Source.AcquisitionEpoch = "2026-09-22";
    Manifest.Source.HorizontalCrs = "LOCAL_ENU";
    Manifest.Source.HorizontalDatum = "synthetic";
    Manifest.Source.VerticalDatum = "synthetic";
    Manifest.Source.License = "test-fixture";
    Manifest.Source.Attribution = "Mountain Planner deterministic regression";
    Manifest.Source.NativeEastSpacingM = FixtureSpacingM;
    Manifest.Source.NativeNorthSpacingM = FixtureSpacingM;
    return Manifest;
}

bool ReadBaseShardHashes(const SkiPreparation::TerrainCorePackageIndex& Index,
    TArray<FString>& OutHashes, FString& OutError)
{
    OutHashes.Reset();
    for (const SkiDomain::TerrainCoreShardDescriptor& Shard : Index.Manifest.Shards)
    {
        if (!SkiDomain::IsSafeTerrainCorePath(Shard.Path))
        {
            OutError = TEXT("TerrainCore regression encountered an unsafe shard path.");
            return false;
        }
        TArray<uint8> Bytes;
        const FString Path = FPaths::Combine(Index.PackageDirectory,
            UTF8_TO_TCHAR(Shard.Path.c_str()));
        if (!FFileHelper::LoadFileToArray(Bytes, *Path))
        {
            OutError = TEXT("TerrainCore regression could not read a base shard.");
            return false;
        }
        const FString Hash = SkiPreparation::Sha256(Bytes);
        if (Hash != UTF8_TO_TCHAR(Shard.Sha256.c_str()))
        {
            OutError = TEXT("TerrainCore regression base shard does not match its manifest hash.");
            return false;
        }
        OutHashes.Add(Hash);
    }
    return true;
}

std::shared_ptr<SkiApplication::TerrainCoreRepository> OpenRepository(
    const std::shared_ptr<SkiPreparation::TerrainCorePackageStore>& Store,
    const SkiPreparation::TerrainCorePackageIndex& Index,
    const std::shared_ptr<std::atomic<int32>>& LocalReads,
    FString& OutError)
{
    std::string RepositoryError;
    auto Repository = SkiApplication::TerrainCoreRepository::Create(Index.Manifest,
        [Store, Index, LocalReads](const SkiDomain::TerrainCoreTileDescriptor& Descriptor,
            SkiApplication::TerrainCoreTilePayload& OutPayload, std::string& Error)
        {
            ++*LocalReads;
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
    if (!Repository)
    {
        OutError = UTF8_TO_TCHAR(RepositoryError.c_str());
    }
    return Repository;
}

bool LoadCacheTile(SkiTerrainRuntime::TerrainCoreTileCache& Cache,
    const SkiApplication::TerrainCoreTileKey& Key,
    std::shared_ptr<const SkiApplication::TerrainCoreTilePayload>& OutTile,
    FString& OutError)
{
    OutTile.reset();
    if (!Cache.RequestTile(Key) || !Cache.WaitForWorkers(5.0))
    {
        OutError = TEXT("TerrainCore regression tile request did not complete.");
        return false;
    }
    Cache.PumpPublications(16);
    OutTile = Cache.FindResident(Key);
    if (!OutTile)
    {
        OutError = TEXT("TerrainCore regression tile was not published within its cache budget.");
        return false;
    }
    return true;
}

bool CheckSharedBorder(const SkiApplication::TerrainCoreTilePayload& West,
    const SkiApplication::TerrainCoreTilePayload& East)
{
    if (West.Descriptor.LodIndex != 0 || East.Descriptor.LodIndex != 0
        || West.Descriptor.StartColumn + West.Descriptor.CoreWidth - 1U
            != East.Descriptor.StartColumn)
    {
        return false;
    }
    const uint32 WestStoredWidth = West.Descriptor.CoreWidth
        + West.Descriptor.HaloWest + West.Descriptor.HaloEast;
    const uint32 EastStoredWidth = East.Descriptor.CoreWidth
        + East.Descriptor.HaloWest + East.Descriptor.HaloEast;
    const uint32 Rows = FMath::Min<uint32>(West.Descriptor.CoreHeight,
        East.Descriptor.CoreHeight);
    for (uint32 Row = 0; Row < Rows; ++Row)
    {
        const uint64 WestIndex = static_cast<uint64>(Row + West.Descriptor.HaloNorth)
                * WestStoredWidth
            + West.Descriptor.HaloWest + West.Descriptor.CoreWidth - 1U;
        const uint64 EastIndex = static_cast<uint64>(Row + East.Descriptor.HaloNorth)
                * EastStoredWidth
            + East.Descriptor.HaloWest;
        if (WestIndex >= West.Heights.size() || EastIndex >= East.Heights.size()
            || West.Validity[WestIndex] == 0 || East.Validity[EastIndex] == 0
            || West.Heights[WestIndex] != East.Heights[EastIndex])
        {
            return false;
        }
    }
    return true;
}

bool CheckWestHalo(const SkiApplication::TerrainCoreTilePayload& Tile)
{
    if (Tile.Descriptor.LodIndex != 0 || Tile.Descriptor.HaloWest != 1
        || Tile.Descriptor.StartColumn == 0)
    {
        return false;
    }
    const uint32 StoredWidth = Tile.Descriptor.CoreWidth + Tile.Descriptor.HaloWest
        + Tile.Descriptor.HaloEast;
    const uint64 Index = static_cast<uint64>(Tile.Descriptor.HaloNorth) * StoredWidth;
    return Index < Tile.Heights.size() && Tile.Validity[Index] != 0
        && Tile.Heights[Index] == FixtureHeightAt(Tile.Descriptor.StartColumn - 1U,
            Tile.Descriptor.StartRow);
}
}

static bool RunTerrainCoreImportEdit(const FString& DataRoot,
    const TSharedPtr<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration,
    SkiPresentation::TerrainCoreRegressionProof& OutProof)
{
    OutProof = {};
    const auto Fail = [&OutProof](const FString& Error)
    {
        OutProof.Error = Error;
        return false;
    };
    if (DataRoot.IsEmpty() || !Lease
        || !Lease->IsCurrent(SessionGeneration, OperationGeneration))
    {
        return Fail(TEXT("TerrainCore regression requires an active operation lease and data root."));
    }

    auto Store = std::make_shared<SkiPreparation::TerrainCorePackageStore>(DataRoot);
    SkiDomain::Heightfield Finest = BuildFixture();
    SkiDomain::TerrainCoreManifest Installed;
    FString PackageDirectory;
    FString Error;
    if (!Store->WriteAndActivate(BuildManifest(), Finest, PackageDirectory, Installed, Error,
            Lease, SessionGeneration, OperationGeneration))
    {
        return Fail(TEXT("TerrainCore activation failed: ") + Error);
    }
    OutProof.ContentId = UTF8_TO_TCHAR(Installed.ContentId.c_str());
    OutProof.Width = Installed.Width;
    OutProof.Height = Installed.Height;

    SkiPreparation::TerrainCorePackageIndex Index;
    if (!Store->Open(OutProof.ContentId, Index, Error) || !Store->Verify(Index, Error))
    {
        return Fail(TEXT("TerrainCore installed package verification failed: ") + Error);
    }
    TArray<FString> BaseHashesBefore;
    if (!ReadBaseShardHashes(Index, BaseHashesBefore, Error)) return Fail(Error);

    std::set<uint32> Factors;
    const SkiDomain::TerrainCoreTileDescriptor* WestDescriptor = nullptr;
    const SkiDomain::TerrainCoreTileDescriptor* EastDescriptor = nullptr;
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Installed.Tiles)
    {
        Factors.insert(Tile.LodFactor);
        if (Tile.LodIndex == 0
            && (Tile.CoreWidth < SkiDomain::TerrainCoreTileSamples
                || Tile.CoreHeight < SkiDomain::TerrainCoreTileSamples))
        {
            OutProof.bPartialEdgeTile = true;
        }
        if (Tile.LodIndex == 0 && Tile.TileX == 0 && Tile.TileY == 0) WestDescriptor = &Tile;
        if (Tile.LodIndex == 0 && Tile.TileX == 1 && Tile.TileY == 0) EastDescriptor = &Tile;
    }
    for (const uint32 Factor : Factors) OutProof.LodFactors.Add(Factor);
    bool bLodFactorsMatch = OutProof.LodFactors.Num()
        == static_cast<int32>(SkiDomain::TerrainCoreLodFactors.size());
    for (int32 FactorIndex = 0; bLodFactorsMatch
        && FactorIndex < OutProof.LodFactors.Num(); ++FactorIndex)
    {
        bLodFactorsMatch = OutProof.LodFactors[FactorIndex]
            == SkiDomain::TerrainCoreLodFactors[static_cast<std::size_t>(FactorIndex)];
    }
    if (OutProof.LodFactors.Num() != static_cast<int32>(SkiDomain::TerrainCoreLodFactors.size())
        || !bLodFactorsMatch
        || !OutProof.bPartialEdgeTile || !WestDescriptor || !EastDescriptor)
    {
        return Fail(TEXT("TerrainCore LOD or partial-edge coverage proof failed."));
    }

    const auto LocalReads = std::make_shared<std::atomic<int32>>(0);
    auto Repository = OpenRepository(Store, Index, LocalReads, Error);
    if (!Repository) return Fail(TEXT("TerrainCore repository open failed: ") + Error);

    SkiApplication::TerrainCoreTilePayload WestDirect;
    SkiApplication::TerrainCoreTilePayload EastDirect;
    std::string ReadError;
    if (!Repository->ReadTile({0, 0, 0}, WestDirect, ReadError)
        || !Repository->ReadTile({0, 1, 0}, EastDirect, ReadError))
    {
        return Fail(FString(TEXT("TerrainCore border tile read failed: "))
            + UTF8_TO_TCHAR(ReadError.c_str()));
    }
    OutProof.bSharedBorder = CheckSharedBorder(WestDirect, EastDirect);
    OutProof.bNormalHalo = CheckWestHalo(EastDirect);
    if (!OutProof.bSharedBorder || !OutProof.bNormalHalo)
    {
        return Fail(TEXT("TerrainCore shared border or normal halo proof failed."));
    }

    const uint64 WestBytes = WestDirect.ResidentBytes();
    const uint64 EastBytes = EastDirect.ResidentBytes();
    OutProof.CacheBudgetBytes = FMath::Max(WestBytes, EastBytes) + 64ULL;
    SkiTerrainRuntime::TerrainCoreCacheConfig CacheConfig;
    CacheConfig.ByteBudget = OutProof.CacheBudgetBytes;
    CacheConfig.MaximumWorkerJobs = 1;
    CacheConfig.MaximumPublicationsPerPump = 4;
    {
        SkiTerrainRuntime::TerrainCoreTileCache Cache(Repository, 1, CacheConfig);
        std::shared_ptr<const SkiApplication::TerrainCoreTilePayload> Cached;
        if (!LoadCacheTile(Cache, {0, 0, 0}, Cached, Error)) return Fail(Error);
        if (!LoadCacheTile(Cache, {0, 1, 0}, Cached, Error)) return Fail(Error);
        const SkiTerrainRuntime::TerrainCoreCacheStats CacheStats = Cache.Stats();
        OutProof.PeakResidentBytes = CacheStats.PeakResidentBytes;
        OutProof.ObservedEvictions = static_cast<uint32>(FMath::Min<uint64>(
            CacheStats.EvictionCount, MAX_uint32));
        if (OutProof.ObservedEvictions == 0
            || Cache.FindResident({0, 0, 0})
            || !Cache.FindResident({0, 1, 0})
            || OutProof.PeakResidentBytes > OutProof.CacheBudgetBytes)
        {
            return Fail(TEXT("TerrainCore cache budget or eviction proof failed."));
        }

        float QueryHeight = 0.0F;
        bool QueryValid = false;
        SkiTerrainRuntime::TerrainCoreQueryStatus QueryStatus =
            Cache.QueryCanonicalSample(QueryColumn, QueryRow, QueryHeight, QueryValid);
        if (QueryStatus == SkiTerrainRuntime::TerrainCoreQueryStatus::Pending)
        {
            if (!Cache.WaitForWorkers(5.0))
                return Fail(TEXT("TerrainCore canonical query timed out."));
            Cache.PumpPublications(4);
            QueryStatus = Cache.QueryCanonicalSample(QueryColumn, QueryRow,
                QueryHeight, QueryValid);
        }
        OutProof.FinestQueryHeightM = QueryHeight;
        OutProof.bFinestQuery = QueryStatus == SkiTerrainRuntime::TerrainCoreQueryStatus::Ready
            && QueryValid && QueryHeight == FixtureHeightAt(QueryColumn, QueryRow);
        if (!OutProof.bFinestQuery)
            return Fail(TEXT("TerrainCore finest canonical query proof failed."));

        // No publication occurs except through PumpPublications. Resetting first therefore makes
        // rejection of an old-generation request deterministic even if disk IO completes quickly.
        Cache.RequestTile({0, 2, 0});
        Cache.ResetGeneration(2, Repository);
        if (!Cache.WaitForWorkers(5.0))
            return Fail(TEXT("TerrainCore stale request did not finish."));
        Cache.PumpPublications(4);
        OutProof.bStalePublicationRejected = !Cache.FindResident({0, 2, 0});
        if (!OutProof.bStalePublicationRejected)
            return Fail(TEXT("TerrainCore stale publication was accepted."));
    }

    SkiDomain::TerrainEditSet Edits;
    Edits.TerrainCoreId = Installed.ContentId;
    Edits.BaseRevision = 1;
    Edits.EditRevision = 2;
    Edits.Deltas = {{QueryColumn, QueryRow, 2.5F},
        {FixtureWidth - 1U, FixtureHeight - 1U, -1.0F}};
    if (!Store->WriteEditSetAndActivate(Edits, Installed.Width, Installed.Height,
            OutProof.EditSetId, Error, Lease, SessionGeneration, OperationGeneration))
    {
        return Fail(TEXT("TerrainCore edit sidecar activation failed: ") + Error);
    }
    SkiDomain::TerrainEditSet ReopenedEdits;
    OutProof.bEditPersisted = Store->LoadEditSet(OutProof.ContentId, OutProof.EditSetId,
        Installed.Width, Installed.Height, ReopenedEdits, Error)
        && ReopenedEdits.TerrainCoreId == Edits.TerrainCoreId
        && ReopenedEdits.BaseRevision == Edits.BaseRevision
        && ReopenedEdits.EditRevision == Edits.EditRevision
        && ReopenedEdits.Deltas.size() == Edits.Deltas.size();
    if (!OutProof.bEditPersisted)
        return Fail(TEXT("TerrainCore sparse edit sidecar reopen failed: ") + Error);
    std::string EditError;
    auto EditedRepository = SkiApplication::TerrainCoreEditedRepository::Create(
        Repository, ReopenedEdits, 1, EditError);
    if (!EditedRepository)
    {
        return Fail(FString(TEXT("TerrainCore edited repository creation failed: "))
            + UTF8_TO_TCHAR(EditError.c_str()));
    }
    SkiTerrainRuntime::TerrainCoreTileCache EditedCache(EditedRepository, 1, CacheConfig);
    float EditedHeight = 0.0F;
    bool bEditedValid = false;
    auto EditedStatus = EditedCache.QueryCanonicalSample(QueryColumn, QueryRow,
        EditedHeight, bEditedValid);
    if (EditedStatus == SkiTerrainRuntime::TerrainCoreQueryStatus::Pending)
    {
        if (!EditedCache.WaitForWorkers(5.0))
            return Fail(TEXT("TerrainCore edited query timed out."));
        EditedCache.PumpPublications(4);
        EditedStatus = EditedCache.QueryCanonicalSample(QueryColumn, QueryRow,
            EditedHeight, bEditedValid);
    }
    OutProof.EditedQueryHeightM = EditedHeight;
    OutProof.bEditDeltaReconstructed = EditedStatus
            == SkiTerrainRuntime::TerrainCoreQueryStatus::Ready
        && bEditedValid && ReopenedEdits.Deltas.front().Column == QueryColumn
        && ReopenedEdits.Deltas.front().Row == QueryRow
        && OutProof.EditedQueryHeightM == FixtureHeightAt(QueryColumn, QueryRow) + 2.5;
    if (!OutProof.bEditDeltaReconstructed)
        return Fail(TEXT("TerrainCore edit delta does not reconstruct the expected query value."));

    TArray<FString> BaseHashesAfter;
    OutProof.bBaseImmutable = ReadBaseShardHashes(Index, BaseHashesAfter, Error)
        && BaseHashesBefore == BaseHashesAfter;
    if (!OutProof.bBaseImmutable)
        return Fail(TEXT("TerrainCore base shards changed after edit activation: ") + Error);

    OutProof.LocalTileReads = LocalReads->load();
    OutProof.NetworkAttempts = 0;
    return true;
}

static bool RunTerrainCoreOfflineReopen(const FString& DataRoot,
    const FString& ContentId, const FString& EditSetId,
    SkiPresentation::TerrainCoreRegressionProof& OutProof)
{
    OutProof = {};
    OutProof.ContentId = ContentId;
    OutProof.EditSetId = EditSetId;
    OutProof.NetworkAttempts = 0;
    const auto Fail = [&OutProof](const FString& Error)
    {
        OutProof.Error = Error;
        return false;
    };
    if (DataRoot.IsEmpty() || ContentId.IsEmpty() || EditSetId.IsEmpty())
    {
        return Fail(TEXT("TerrainCore offline reopen requires exact content and edit-set IDs."));
    }

    const double ReopenBegan = FPlatformTime::Seconds();
    auto Store = std::make_shared<SkiPreparation::TerrainCorePackageStore>(DataRoot);
    SkiPreparation::TerrainCorePackageIndex Index;
    FString Error;
    if (!Store->Open(ContentId, Index, Error) || !Store->Verify(Index, Error))
    {
        return Fail(TEXT("TerrainCore offline package verification failed: ") + Error);
    }
    OutProof.Width = Index.Manifest.Width;
    OutProof.Height = Index.Manifest.Height;
    TArray<FString> BaseHashes;
    OutProof.bBaseImmutable = ReadBaseShardHashes(Index, BaseHashes, Error);
    if (!OutProof.bBaseImmutable) return Fail(Error);

    SkiDomain::TerrainEditSet Edits;
    OutProof.bEditPersisted = Store->LoadEditSet(ContentId, EditSetId,
        Index.Manifest.Width, Index.Manifest.Height, Edits, Error)
        && Edits.TerrainCoreId == TCHAR_TO_UTF8(*ContentId)
        && Edits.BaseRevision == 1 && Edits.EditRevision == 2;
    if (!OutProof.bEditPersisted)
    {
        return Fail(TEXT("TerrainCore offline edit-set verification failed: ") + Error);
    }
    const auto Delta = std::find_if(Edits.Deltas.begin(), Edits.Deltas.end(),
        [](const SkiDomain::TerrainEditDelta& Candidate)
        {
            return Candidate.Column == QueryColumn && Candidate.Row == QueryRow;
        });
    if (Delta == Edits.Deltas.end())
    {
        return Fail(TEXT("TerrainCore offline edit set does not contain the query delta."));
    }

    std::set<uint32> Factors;
    for (const SkiDomain::TerrainCoreTileDescriptor& Tile : Index.Manifest.Tiles)
    {
        Factors.insert(Tile.LodFactor);
        if (Tile.LodIndex == 0
            && (Tile.CoreWidth < SkiDomain::TerrainCoreTileSamples
                || Tile.CoreHeight < SkiDomain::TerrainCoreTileSamples))
        {
            OutProof.bPartialEdgeTile = true;
        }
    }
    for (const uint32 Factor : Factors) OutProof.LodFactors.Add(Factor);

    const auto LocalReads = std::make_shared<std::atomic<int32>>(0);
    auto Repository = OpenRepository(Store, Index, LocalReads, Error);
    if (!Repository) return Fail(TEXT("TerrainCore offline repository open failed: ") + Error);
    SkiApplication::TerrainCoreTilePayload QueryTile;
    std::string ReadError;
    if (!Repository->ReadTile({0, QueryColumn / SkiDomain::TerrainCoreTileCells,
            QueryRow / SkiDomain::TerrainCoreTileCells}, QueryTile, ReadError))
    {
        return Fail(FString(TEXT("TerrainCore offline query tile read failed: "))
            + UTF8_TO_TCHAR(ReadError.c_str()));
    }
    OutProof.CacheBudgetBytes = QueryTile.ResidentBytes() + 64ULL;
    SkiTerrainRuntime::TerrainCoreCacheConfig Config;
    Config.ByteBudget = OutProof.CacheBudgetBytes;
    Config.MaximumWorkerJobs = 1;
    Config.MaximumPublicationsPerPump = 2;
    SkiTerrainRuntime::TerrainCoreTileCache BaseCache(Repository, 1, Config);
    float BaseHeightM = 0.0F;
    bool bBaseValid = false;
    auto BaseStatus = BaseCache.QueryCanonicalSample(QueryColumn, QueryRow,
        BaseHeightM, bBaseValid);
    if (BaseStatus == SkiTerrainRuntime::TerrainCoreQueryStatus::Pending)
    {
        if (!BaseCache.WaitForWorkers(5.0))
            return Fail(TEXT("TerrainCore offline canonical query timed out."));
        BaseCache.PumpPublications(2);
        BaseStatus = BaseCache.QueryCanonicalSample(QueryColumn, QueryRow,
            BaseHeightM, bBaseValid);
    }
    OutProof.FinestQueryHeightM = BaseHeightM;
    OutProof.bFinestQuery = BaseStatus == SkiTerrainRuntime::TerrainCoreQueryStatus::Ready
        && bBaseValid && BaseHeightM == FixtureHeightAt(QueryColumn, QueryRow);

    std::string EditError;
    auto EditedRepository = SkiApplication::TerrainCoreEditedRepository::Create(
        Repository, Edits, 1, EditError);
    if (!EditedRepository)
    {
        return Fail(FString(TEXT("TerrainCore offline edited repository creation failed: "))
            + UTF8_TO_TCHAR(EditError.c_str()));
    }
    SkiTerrainRuntime::TerrainCoreTileCache EditedCache(EditedRepository,
        Edits.EditRevision, Config);
    float EditedHeightM = 0.0F;
    bool bEditedValid = false;
    auto EditedStatus = EditedCache.QueryCanonicalSample(QueryColumn, QueryRow,
        EditedHeightM, bEditedValid);
    if (EditedStatus == SkiTerrainRuntime::TerrainCoreQueryStatus::Pending)
    {
        if (!EditedCache.WaitForWorkers(5.0))
            return Fail(TEXT("TerrainCore offline edited query timed out."));
        EditedCache.PumpPublications(2);
        EditedStatus = EditedCache.QueryCanonicalSample(QueryColumn, QueryRow,
            EditedHeightM, bEditedValid);
    }
    OutProof.EditedQueryHeightM = EditedHeightM;
    OutProof.bEditDeltaReconstructed = EditedStatus
            == SkiTerrainRuntime::TerrainCoreQueryStatus::Ready
        && bEditedValid && OutProof.bFinestQuery
        && OutProof.EditedQueryHeightM == OutProof.FinestQueryHeightM
            + static_cast<double>(Delta->DeltaM)
        && OutProof.EditedQueryHeightM == FixtureHeightAt(QueryColumn, QueryRow)
            + static_cast<double>(Delta->DeltaM);
    OutProof.LocalTileReads = LocalReads->load();
    OutProof.PeakResidentBytes = FMath::Max(BaseCache.Stats().PeakResidentBytes,
        EditedCache.Stats().PeakResidentBytes);
    OutProof.OfflineReopenMilliseconds =
        (FPlatformTime::Seconds() - ReopenBegan) * 1000.0;
    OutProof.bOfflineReopened = OutProof.bFinestQuery
        && OutProof.bEditDeltaReconstructed && OutProof.LocalTileReads > 0
        && OutProof.NetworkAttempts == 0;
    if (!OutProof.bOfflineReopened)
    {
        return Fail(TEXT("TerrainCore offline base/edit query proof failed."));
    }
    return true;
}

bool SkiPresentation::RunTerrainCoreRegression(const TerrainCoreRegressionPhase Phase,
    const FString& DataRoot,
    const TSharedPtr<SkiPreparation::PreparationOperationLease, ESPMode::ThreadSafe>& Lease,
    const uint64 SessionGeneration, const uint64 OperationGeneration,
    const FString& ExistingContentId, const FString& ExistingEditSetId,
    TerrainCoreRegressionProof& OutProof)
{
    switch (Phase)
    {
    case TerrainCoreRegressionPhase::ImportEdit:
        return RunTerrainCoreImportEdit(DataRoot, Lease, SessionGeneration,
            OperationGeneration, OutProof);
    case TerrainCoreRegressionPhase::OfflineReopen:
        return RunTerrainCoreOfflineReopen(DataRoot, ExistingContentId,
            ExistingEditSetId, OutProof);
    default:
        OutProof = {};
        OutProof.Error = TEXT("Unknown TerrainCore regression phase.");
        return false;
    }
}
