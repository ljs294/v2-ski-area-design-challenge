#include "SkiTerrainRuntime/TerrainCoreTileCache.h"

#include "Async/Async.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace
{
using SkiApplication::TerrainCoreTileKey;
using SkiApplication::TerrainCoreTilePayload;
struct RequestIdentity
{
    std::uint64_t Generation = 0;
    TerrainCoreTileKey Key;
    bool operator<(const RequestIdentity& Other) const noexcept
    {
        if (Generation != Other.Generation) return Generation < Other.Generation;
        return Key < Other.Key;
    }
};
}

struct SkiTerrainRuntime::TerrainCoreTileCache::State
{
    struct ResidentEntry
    {
        std::shared_ptr<const TerrainCoreTilePayload> Payload;
        std::uint64_t LastUse = 0;
        std::uint32_t Pins = 0;
    };
    enum class RequestPhase : std::uint8_t { Pending, InFlight, Completed };
    struct RequestEntry
    {
        std::uint64_t ReservedBytes = 0;
        std::uint32_t Pins = 0;
        RequestPhase Phase = RequestPhase::Pending;
        std::shared_ptr<std::atomic_bool> Cancelled;
    };
    struct Completion { RequestIdentity Identity; TerrainCoreTilePayload Payload; };

    mutable std::mutex Mutex;
    mutable std::condition_variable WorkersChanged;
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repository;
    std::shared_ptr<std::atomic_bool> GenerationCancelled;
    TerrainCoreCacheConfig Config;
    std::uint64_t Generation = 0;
    std::uint64_t UseSerial = 0;
    std::uint64_t ResidentBytes = 0;
    std::uint64_t ReservedBytes = 0;
    std::uint64_t RejectedPublications = 0;
    std::uint64_t EvictionCount = 0;
    std::uint64_t PeakResidentBytes = 0;
    std::uint64_t PeakAccountedBytes = 0;
    std::uint32_t ActiveWorkers = 0;
    std::uint32_t PeakWorkers = 0;
    std::uint32_t PeakPublicationsPerPump = 0;
    std::map<TerrainCoreTileKey, ResidentEntry> Resident;
    std::deque<RequestIdentity> Pending;
    std::deque<Completion> Completed;
    // The request map is the single-flight table and survives successful decode until publish.
    std::map<RequestIdentity, RequestEntry> Requests;
    std::map<TerrainCoreTileKey, std::string> Failures;
};

namespace
{
using CacheState = SkiTerrainRuntime::TerrainCoreTileCache::State;

std::uint64_t DecodedBytes(const SkiDomain::TerrainCoreTileDescriptor& Descriptor)
{
    const std::uint64_t Width = static_cast<std::uint64_t>(Descriptor.CoreWidth)
        + Descriptor.HaloWest + Descriptor.HaloEast;
    const std::uint64_t Height = static_cast<std::uint64_t>(Descriptor.CoreHeight)
        + Descriptor.HaloNorth + Descriptor.HaloSouth;
    return Width * Height * (sizeof(float) + sizeof(std::uint8_t));
}

std::uint64_t ReservationFor(const SkiApplication::ITerrainCoreRepository& Repository,
    const TerrainCoreTileKey& Key)
{
    const auto Metadata = Repository.Metadata();
    if (!Metadata) return 0;
    const auto Found = std::find_if(Metadata->Tiles.begin(), Metadata->Tiles.end(),
        [&Key](const SkiDomain::TerrainCoreTileDescriptor& Descriptor)
        {
            return Descriptor.LodIndex == Key.Lod && Descriptor.TileX == Key.X
                && Descriptor.TileY == Key.Y;
        });
    return Found == Metadata->Tiles.end() ? 0 : DecodedBytes(*Found);
}

bool EvictUntilAccountedFits(CacheState& State, const std::uint64_t IncomingBytes)
{
    if (IncomingBytes > State.Config.ByteBudget) return false;
    while (State.ResidentBytes + State.ReservedBytes
        > State.Config.ByteBudget - IncomingBytes)
    {
        auto Victim = State.Resident.end();
        for (auto It = State.Resident.begin(); It != State.Resident.end(); ++It)
        {
            if (It->second.Pins == 0
                && (Victim == State.Resident.end() || It->second.LastUse < Victim->second.LastUse))
            {
                Victim = It;
            }
        }
        if (Victim == State.Resident.end()) return false;
        State.ResidentBytes -= Victim->second.Payload->ResidentBytes();
        State.Resident.erase(Victim);
        ++State.EvictionCount;
    }
    return true;
}

void ReleaseRequest(CacheState& State,
    const std::map<RequestIdentity, CacheState::RequestEntry>::iterator It)
{
    State.ReservedBytes -= It->second.ReservedBytes;
    State.Requests.erase(It);
}

bool RequestLocked(CacheState& State, const TerrainCoreTileKey& Key, const bool Pin,
    const std::uint64_t ExpectedGeneration,
    const std::shared_ptr<const SkiApplication::ITerrainCoreRepository>& ExpectedRepository)
{
    if (!ExpectedRepository || ExpectedGeneration == 0
        || Key.Lod >= SkiDomain::TerrainCoreLodFactors.size()
        || State.Generation != ExpectedGeneration || State.Repository != ExpectedRepository)
    {
        return false;
    }
    auto Resident = State.Resident.find(Key);
    if (Resident != State.Resident.end())
    {
        Resident->second.LastUse = ++State.UseSerial;
        if (Pin) ++Resident->second.Pins;
        return true;
    }
    if (State.Failures.find(Key) != State.Failures.end()) return false;
    const RequestIdentity Identity{ExpectedGeneration, Key};
    auto Existing = State.Requests.find(Identity);
    if (Existing != State.Requests.end())
    {
        if (Pin) ++Existing->second.Pins;
        return true;
    }
    const std::uint64_t Bytes = ReservationFor(*ExpectedRepository, Key);
    if (Bytes == 0 || !EvictUntilAccountedFits(State, Bytes)) return false;
    CacheState::RequestEntry Entry;
    Entry.ReservedBytes = Bytes;
    Entry.Pins = Pin ? 1U : 0U;
    Entry.Cancelled = State.GenerationCancelled;
    State.Requests.emplace(Identity, std::move(Entry));
    State.Pending.push_back(Identity);
    State.ReservedBytes += Bytes;
    State.PeakAccountedBytes = std::max(State.PeakAccountedBytes,
        State.ResidentBytes + State.ReservedBytes);
    return true;
}

void ScheduleAvailable(const std::shared_ptr<CacheState>& State)
{
    for (;;)
    {
        RequestIdentity Identity;
        std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repository;
        std::shared_ptr<std::atomic_bool> Cancelled;
        {
            std::lock_guard Lock(State->Mutex);
            if (State->ActiveWorkers >= State->Config.MaximumWorkerJobs
                || State->Pending.empty()) return;
            Identity = State->Pending.front();
            State->Pending.pop_front();
            auto Entry = State->Requests.find(Identity);
            if (Entry == State->Requests.end() || Identity.Generation != State->Generation) continue;
            Entry->second.Phase = CacheState::RequestPhase::InFlight;
            Cancelled = Entry->second.Cancelled;
            Repository = State->Repository;
            ++State->ActiveWorkers;
            State->PeakWorkers = std::max(State->PeakWorkers, State->ActiveWorkers);
        }
        Async(EAsyncExecution::ThreadPool,
            [State, Identity, Repository = std::move(Repository), Cancelled = std::move(Cancelled)]()
            {
                TerrainCoreTilePayload Payload;
                std::string Error;
                const SkiApplication::TerrainCoreReadCancellation Token(Cancelled);
                const bool Succeeded = Repository && Repository->ReadTile(
                    Identity.Key, Payload, Error, Token);
                {
                    std::lock_guard Lock(State->Mutex);
                    --State->ActiveWorkers;
                    auto Entry = State->Requests.find(Identity);
                    if (Entry != State->Requests.end())
                    {
                        const bool CancelledOrStale = Token.IsCancellationRequested()
                            || Identity.Generation != State->Generation;
                        if (!Succeeded || CancelledOrStale
                            || Payload.ResidentBytes() > Entry->second.ReservedBytes)
                        {
                            if (!CancelledOrStale)
                            {
                                State->Failures[Identity.Key] = !Succeeded
                                    ? (Error.empty() ? "TerrainCore tile read failed" : Error)
                                    : "TerrainCore decoded tile exceeds its reservation";
                            }
                            ReleaseRequest(*State, Entry);
                        }
                        else
                        {
                            Entry->second.Phase = CacheState::RequestPhase::Completed;
                            State->Completed.push_back({Identity, std::move(Payload)});
                        }
                    }
                }
                State->WorkersChanged.notify_all();
                ScheduleAvailable(State);
            });
    }
}
}

SkiTerrainRuntime::TerrainCoreTileCache::TerrainCoreTileCache(
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repository,
    const std::uint64_t Generation, TerrainCoreCacheConfig Config)
    : Shared(std::make_shared<State>())
{
    Config.ByteBudget = std::max<std::uint64_t>(1, Config.ByteBudget);
    Config.MaximumWorkerJobs = std::max<std::uint32_t>(1, Config.MaximumWorkerJobs);
    Config.MaximumPublicationsPerPump = std::max<std::uint32_t>(1, Config.MaximumPublicationsPerPump);
    Shared->Repository = std::move(Repository);
    Shared->Generation = Generation;
    Shared->Config = Config;
    Shared->GenerationCancelled = std::make_shared<std::atomic_bool>(false);
}

SkiTerrainRuntime::TerrainCoreTileCache::~TerrainCoreTileCache()
{
    CancelPending();
}

bool SkiTerrainRuntime::TerrainCoreTileCache::RequestTile(
    const SkiApplication::TerrainCoreTileKey& Key, const bool Pin)
{
    bool Accepted;
    {
        std::lock_guard Lock(Shared->Mutex);
        Accepted = RequestLocked(*Shared, Key, Pin, Shared->Generation, Shared->Repository);
    }
    if (Accepted) ScheduleAvailable(Shared);
    return Accepted;
}

bool SkiTerrainRuntime::TerrainCoreTileCache::ReleasePin(
    const SkiApplication::TerrainCoreTileKey& Key)
{
    std::lock_guard Lock(Shared->Mutex);
    auto Resident = Shared->Resident.find(Key);
    if (Resident != Shared->Resident.end() && Resident->second.Pins != 0)
    {
        --Resident->second.Pins;
        return true;
    }
    auto Pending = Shared->Requests.find({Shared->Generation, Key});
    if (Pending == Shared->Requests.end() || Pending->second.Pins == 0) return false;
    --Pending->second.Pins;
    return true;
}

SkiTerrainRuntime::TerrainCoreRequestStatus
SkiTerrainRuntime::TerrainCoreTileCache::TileStatus(
    const SkiApplication::TerrainCoreTileKey& Key, std::string* OutError) const
{
    std::lock_guard Lock(Shared->Mutex);
    if (OutError) OutError->clear();
    if (Shared->Resident.find(Key) != Shared->Resident.end())
    {
        return TerrainCoreRequestStatus::Resident;
    }
    const auto Failure = Shared->Failures.find(Key);
    if (Failure != Shared->Failures.end())
    {
        if (OutError) *OutError = Failure->second;
        return TerrainCoreRequestStatus::Failed;
    }
    if (Shared->Requests.find({Shared->Generation, Key}) != Shared->Requests.end())
    {
        return TerrainCoreRequestStatus::Pending;
    }
    return TerrainCoreRequestStatus::NotRequested;
}

bool SkiTerrainRuntime::TerrainCoreTileCache::RetryTile(
    const SkiApplication::TerrainCoreTileKey& Key, const bool Pin)
{
    bool Accepted = false;
    {
        std::lock_guard Lock(Shared->Mutex);
        const auto Failure = Shared->Failures.find(Key);
        if (Failure == Shared->Failures.end()) return false;
        Shared->Failures.erase(Failure);
        Accepted = RequestLocked(*Shared, Key, Pin, Shared->Generation, Shared->Repository);
    }
    if (Accepted) ScheduleAvailable(Shared);
    return Accepted;
}

std::shared_ptr<const SkiApplication::TerrainCoreTilePayload>
SkiTerrainRuntime::TerrainCoreTileCache::FindResident(const SkiApplication::TerrainCoreTileKey& Key)
{
    std::lock_guard Lock(Shared->Mutex);
    auto It = Shared->Resident.find(Key);
    if (It == Shared->Resident.end()) return {};
    It->second.LastUse = ++Shared->UseSerial;
    return It->second.Payload;
}

std::uint32_t SkiTerrainRuntime::TerrainCoreTileCache::PumpPublications(std::uint32_t Maximum)
{
    if (Maximum == 0) Maximum = Shared->Config.MaximumPublicationsPerPump;
    std::uint32_t Published = 0;
    std::uint32_t Processed = 0;
    std::lock_guard Lock(Shared->Mutex);
    while (Processed < Maximum && !Shared->Completed.empty())
    {
        State::Completion Completion = std::move(Shared->Completed.front());
        Shared->Completed.pop_front();
        ++Processed;
        auto Request = Shared->Requests.find(Completion.Identity);
        if (Request == Shared->Requests.end()) continue;
        const std::uint64_t Reserved = Request->second.ReservedBytes;
        const std::uint32_t Pins = Request->second.Pins;
        if (Completion.Identity.Generation != Shared->Generation)
        {
            ++Shared->RejectedPublications;
            ReleaseRequest(*Shared, Request);
            continue;
        }
        if (Completion.Payload.ResidentBytes() > Reserved)
        {
            ++Shared->RejectedPublications;
            Shared->Failures[Completion.Identity.Key]
                = "TerrainCore decoded tile exceeds its reservation";
            ReleaseRequest(*Shared, Request);
            continue;
        }
        auto Payload = std::make_shared<const SkiApplication::TerrainCoreTilePayload>(
            std::move(Completion.Payload));
        const std::uint64_t Bytes = Payload->ResidentBytes();
        Shared->ReservedBytes -= Reserved;
        Shared->Requests.erase(Request);
        Shared->ResidentBytes += Bytes;
        Shared->PeakResidentBytes = std::max(Shared->PeakResidentBytes, Shared->ResidentBytes);
        Shared->Resident[Completion.Identity.Key] = {std::move(Payload), ++Shared->UseSerial, Pins};
        ++Published;
    }
    Shared->PeakPublicationsPerPump = std::max(Shared->PeakPublicationsPerPump, Published);
    return Published;
}

void SkiTerrainRuntime::TerrainCoreTileCache::CancelPending()
{
    if (!Shared) return;
    {
        std::lock_guard Lock(Shared->Mutex);
        if (Shared->GenerationCancelled)
        {
            Shared->GenerationCancelled->store(true, std::memory_order_release);
        }
        Shared->Generation = 0;
        Shared->Repository.reset();
        Shared->Pending.clear();
        Shared->Completed.clear();
        Shared->Resident.clear();
        Shared->ResidentBytes = 0;
        Shared->Failures.clear();
        for (auto It = Shared->Requests.begin(); It != Shared->Requests.end();)
        {
            if (It->second.Phase == State::RequestPhase::InFlight)
            {
                ++It;
                continue;
            }
            Shared->ReservedBytes -= It->second.ReservedBytes;
            It = Shared->Requests.erase(It);
        }
    }
    Shared->WorkersChanged.notify_all();
}

void SkiTerrainRuntime::TerrainCoreTileCache::ResetGeneration(const std::uint64_t Generation,
    std::shared_ptr<const SkiApplication::ITerrainCoreRepository> Repository)
{
    {
        std::lock_guard Lock(Shared->Mutex);
        if (Shared->GenerationCancelled)
        {
            Shared->GenerationCancelled->store(true, std::memory_order_release);
        }
        Shared->Generation = Generation;
        if (Repository) Shared->Repository = std::move(Repository);
        Shared->GenerationCancelled = std::make_shared<std::atomic_bool>(false);
        Shared->Resident.clear();
        Shared->Pending.clear();
        Shared->Completed.clear();
        Shared->ResidentBytes = 0;
        Shared->Failures.clear();
        // Pending and completed stale work owns no external job and can be released now.
        // In-flight work remains reserved/accounted until its reader actually returns.
        for (auto It = Shared->Requests.begin(); It != Shared->Requests.end();)
        {
            if (It->second.Phase == State::RequestPhase::InFlight)
            {
                ++It;
                continue;
            }
            Shared->ReservedBytes -= It->second.ReservedBytes;
            It = Shared->Requests.erase(It);
        }
    }
    Shared->WorkersChanged.notify_all();
    ScheduleAvailable(Shared);
}

SkiTerrainRuntime::TerrainCoreCacheStats SkiTerrainRuntime::TerrainCoreTileCache::Stats() const
{
    std::lock_guard Lock(Shared->Mutex);
    TerrainCoreCacheStats Result;
    Result.ConfiguredBudgetBytes = Shared->Config.ByteBudget;
    Result.ResidentBytes = Shared->ResidentBytes;
    Result.ReservedBytes = Shared->ReservedBytes;
    Result.AccountedBytes = Shared->ResidentBytes + Shared->ReservedBytes;
    Result.ResidentTiles = static_cast<std::uint32_t>(Shared->Resident.size());
    Result.PendingTiles = static_cast<std::uint32_t>(Shared->Requests.size());
    Result.ActiveWorkerJobs = Shared->ActiveWorkers;
    Result.FailedTiles = static_cast<std::uint32_t>(Shared->Failures.size());
    Result.RejectedPublications = Shared->RejectedPublications;
    Result.EvictionCount = Shared->EvictionCount;
    Result.PeakResidentBytes = Shared->PeakResidentBytes;
    Result.PeakAccountedBytes = Shared->PeakAccountedBytes;
    Result.PeakWorkerJobs = Shared->PeakWorkers;
    Result.PeakPublicationsPerPump = Shared->PeakPublicationsPerPump;
    return Result;
}

SkiTerrainRuntime::TerrainCoreQueryStatus
SkiTerrainRuntime::TerrainCoreTileCache::QueryCanonicalSample(const std::uint32_t Column,
    const std::uint32_t Row, float& OutHeightM, bool& OutValid)
{
    OutHeightM = 0.0F;
    OutValid = false;
    bool Queued = false;
    {
        std::lock_guard Lock(Shared->Mutex);
        const auto Repository = Shared->Repository;
        const std::uint64_t Generation = Shared->Generation;
        const auto Metadata = Repository ? Repository->Metadata() : nullptr;
        if (!Metadata || Column >= Metadata->Width || Row >= Metadata->Height)
        {
            return TerrainCoreQueryStatus::Invalid;
        }
        const std::uint32_t MaximumTileX = (Metadata->Width - 2U) / SkiDomain::TerrainCoreTileCells;
        const std::uint32_t MaximumTileY = (Metadata->Height - 2U) / SkiDomain::TerrainCoreTileCells;
        const TerrainCoreTileKey Key{0,
            std::min(Column / SkiDomain::TerrainCoreTileCells, MaximumTileX),
            std::min(Row / SkiDomain::TerrainCoreTileCells, MaximumTileY)};
        auto Resident = Shared->Resident.find(Key);
        if (Resident == Shared->Resident.end())
        {
            if (Shared->Failures.find(Key) != Shared->Failures.end())
            {
                return TerrainCoreQueryStatus::Failed;
            }
            Queued = RequestLocked(*Shared, Key, false, Generation, Repository);
        }
        else
        {
            Resident->second.LastUse = ++Shared->UseSerial;
            const auto& Tile = *Resident->second.Payload;
            const std::uint32_t LocalColumn = Column - Tile.Descriptor.StartColumn + Tile.Descriptor.HaloWest;
            const std::uint32_t LocalRow = Row - Tile.Descriptor.StartRow + Tile.Descriptor.HaloNorth;
            const std::uint32_t StoredWidth = Tile.Descriptor.CoreWidth
                + Tile.Descriptor.HaloWest + Tile.Descriptor.HaloEast;
            const std::uint64_t Index = static_cast<std::uint64_t>(LocalRow) * StoredWidth + LocalColumn;
            if (Index >= Tile.Heights.size()) return TerrainCoreQueryStatus::Invalid;
            OutHeightM = Tile.Heights[static_cast<std::size_t>(Index)];
            OutValid = Tile.Validity[static_cast<std::size_t>(Index)] != 0;
            return TerrainCoreQueryStatus::Ready;
        }
    }
    if (Queued) ScheduleAvailable(Shared);
    return TerrainCoreQueryStatus::Pending;
}

bool SkiTerrainRuntime::TerrainCoreTileCache::WaitForWorkers(const double TimeoutSeconds) const
{
    if (TimeoutSeconds < 0.0) return false;
    std::unique_lock Lock(Shared->Mutex);
    return Shared->WorkersChanged.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds),
        [this]
        {
            return Shared->ActiveWorkers == 0 && Shared->Pending.empty();
        });
}

SkiTerrainRuntime::TerrainCoreQueryStatus
SkiTerrainRuntime::TerrainCoreTileCache::QueryCanonicalCell(const std::uint32_t Column,
    const std::uint32_t Row, const double EastFraction, const double SouthFraction,
    double& OutHeightM, bool& OutValid)
{
    OutHeightM = 0.0;
    OutValid = false;
    bool Queued = false;
    {
        std::lock_guard Lock(Shared->Mutex);
        const auto Repository = Shared->Repository;
        const std::uint64_t Generation = Shared->Generation;
        const auto Metadata = Repository ? Repository->Metadata() : nullptr;
        if (!Metadata || Column >= Metadata->Width - 1U || Row >= Metadata->Height - 1U
            || !std::isfinite(EastFraction) || !std::isfinite(SouthFraction)
            || EastFraction < 0.0 || EastFraction > 1.0
            || SouthFraction < 0.0 || SouthFraction > 1.0)
        {
            return TerrainCoreQueryStatus::Invalid;
        }
        const TerrainCoreTileKey Key{0, Column / SkiDomain::TerrainCoreTileCells,
            Row / SkiDomain::TerrainCoreTileCells};
        auto Resident = Shared->Resident.find(Key);
        if (Resident == Shared->Resident.end())
        {
            if (Shared->Failures.find(Key) != Shared->Failures.end())
            {
                return TerrainCoreQueryStatus::Failed;
            }
            Queued = RequestLocked(*Shared, Key, false, Generation, Repository);
        }
        else
        {
            Resident->second.LastUse = ++Shared->UseSerial;
            const auto& Tile = *Resident->second.Payload;
            const std::uint32_t StoredWidth = Tile.Descriptor.CoreWidth
                + Tile.Descriptor.HaloWest + Tile.Descriptor.HaloEast;
            const std::uint32_t LocalColumn = Column - Tile.Descriptor.StartColumn + Tile.Descriptor.HaloWest;
            const std::uint32_t LocalRow = Row - Tile.Descriptor.StartRow + Tile.Descriptor.HaloNorth;
            const auto Sample = [&](const std::uint32_t X, const std::uint32_t Y, float& Height)
            {
                const std::uint64_t Index = static_cast<std::uint64_t>(Y) * StoredWidth + X;
                if (Index >= Tile.Heights.size() || Tile.Validity[static_cast<std::size_t>(Index)] == 0)
                {
                    return false;
                }
                Height = Tile.Heights[static_cast<std::size_t>(Index)];
                return std::isfinite(Height);
            };
            float NorthWest = 0.0F, NorthEast = 0.0F, SouthWest = 0.0F, SouthEast = 0.0F;
            // Match the exact NW-SE triangle emitted by BuildTerrainCoreTileMesh. A
            // no-data sample in the opposite triangle must not invalidate a rendered hit.
            if (SouthFraction >= EastFraction)
            {
                if (!Sample(LocalColumn, LocalRow, NorthWest)
                    || !Sample(LocalColumn, LocalRow + 1U, SouthWest)
                    || !Sample(LocalColumn + 1U, LocalRow + 1U, SouthEast))
                {
                    return TerrainCoreQueryStatus::Ready;
                }
                OutHeightM = NorthWest + SouthFraction * (SouthWest - NorthWest)
                    + EastFraction * (SouthEast - SouthWest);
            }
            else
            {
                if (!Sample(LocalColumn, LocalRow, NorthWest)
                    || !Sample(LocalColumn + 1U, LocalRow + 1U, SouthEast)
                    || !Sample(LocalColumn + 1U, LocalRow, NorthEast))
                {
                    return TerrainCoreQueryStatus::Ready;
                }
                OutHeightM = NorthWest + EastFraction * (NorthEast - NorthWest)
                    + SouthFraction * (SouthEast - NorthEast);
            }
            OutValid = std::isfinite(OutHeightM);
            return TerrainCoreQueryStatus::Ready;
        }
    }
    if (Queued) ScheduleAvailable(Shared);
    return TerrainCoreQueryStatus::Pending;
}
