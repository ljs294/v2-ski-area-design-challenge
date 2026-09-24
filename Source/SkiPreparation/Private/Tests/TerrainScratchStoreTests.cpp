#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "SkiPreparation/TerrainScratchStore.h"
#include "SkiPreparation/TerrainPreparation.h"

#include <cstring>
#include <limits>

namespace
{
FString ScratchTestRoot(const TCHAR* Name)
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), Name,
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

TArray<SkiDomain::TerrainCoreSource> ScratchSources()
{
    TArray<SkiDomain::TerrainCoreSource> Sources;
    SkiDomain::TerrainCoreSource S1m;
    S1m.SourceId = "fixture-s1m-north";
    S1m.Product = "S1M";
    Sources.Add(S1m);
    SkiDomain::TerrainCoreSource Project;
    Project.SourceId = "fixture-project-1m-south";
    Project.Product = "Project1m";
    Sources.Add(Project);
    return Sources;
}

void MakeCanonicalSamples(const uint32 Width, const uint32 Height,
    TArray<float>& OutHeights, TArray<uint8>& OutValidity,
    TArray<uint8>& OutProvenance, TArray<uint8>& OutSources)
{
    const int32 Count = static_cast<int32>(static_cast<uint64>(Width) * Height);
    OutHeights.SetNumUninitialized(Count);
    OutValidity.Init(1, Count);
    OutProvenance.SetNumUninitialized(Count);
    OutSources.SetNumUninitialized(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const uint32 Bits = 0x3f000000U | (static_cast<uint32>(Index) & 0x007fffffU);
        FMemory::Memcpy(&OutHeights[Index], &Bits, sizeof(float));
        OutProvenance[Index] = Index % 2 == 0
            ? static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::S1MNative)
            : static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::Project1m);
        OutSources[Index] = Index % 2 == 0 ? 0 : 1;
    }
    if (Count > 1)
    {
        const uint32 NegativeZeroBits = 0x80000000U;
        FMemory::Memcpy(&OutHeights[1], &NegativeZeroBits, sizeof(float));
    }
    if (Count > 0)
    {
        const int32 NoDataIndex = Count - 1;
        const uint32 NaNBits = 0x7fc00001U;
        FMemory::Memcpy(&OutHeights[NoDataIndex], &NaNBits, sizeof(float));
        OutValidity[NoDataIndex] = 0;
        OutProvenance[NoDataIndex] = static_cast<uint8>(
            SkiPreparation::TerrainScratchProvenance::NoData);
        OutSources[NoDataIndex] = SkiPreparation::TerrainScratchNoSourceIndex;
    }
}

bool WriteAllScratchTiles(SkiPreparation::TerrainScratchStore& Store,
    const uint32 Width, const uint32 Height, const TArray<float>& Heights,
    const TArray<uint8>& Validity, const TArray<uint8>& Provenance,
    const TArray<uint8>& Sources, FString& Error)
{
    const uint32 TilesX = (Width + SkiPreparation::TerrainScratchTileSamples - 1U)
        / SkiPreparation::TerrainScratchTileSamples;
    const uint32 TilesY = (Height + SkiPreparation::TerrainScratchTileSamples - 1U)
        / SkiPreparation::TerrainScratchTileSamples;
    for (uint32 ReverseY = 0; ReverseY < TilesY; ++ReverseY)
    {
        const uint32 TileY = TilesY - 1U - ReverseY;
        const uint32 StartRow = TileY * SkiPreparation::TerrainScratchTileSamples;
        const uint32 TileHeight = FMath::Min(SkiPreparation::TerrainScratchTileSamples,
            Height - StartRow);
        for (uint32 ReverseX = 0; ReverseX < TilesX; ++ReverseX)
        {
            const uint32 TileX = TilesX - 1U - ReverseX;
            const uint32 StartColumn = TileX * SkiPreparation::TerrainScratchTileSamples;
            const uint32 TileWidth = FMath::Min(SkiPreparation::TerrainScratchTileSamples,
                Width - StartColumn);
            const uint64 Stride = TileWidth + 2ULL;
            const int32 BufferCount = static_cast<int32>(Stride * TileHeight);
            TArray<float> HeightBuffer;
            TArray<uint8> ValidBuffer, ProvenanceBuffer, SourceBuffer;
            HeightBuffer.Init(0.0F, BufferCount);
            ValidBuffer.Init(0, BufferCount);
            ProvenanceBuffer.Init(static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::NoData), BufferCount);
            SourceBuffer.Init(SkiPreparation::TerrainScratchNoSourceIndex, BufferCount);
            for (uint32 Row = 0; Row < TileHeight; ++Row)
            {
                for (uint32 Column = 0; Column < TileWidth; ++Column)
                {
                    const int32 GlobalIndex = static_cast<int32>(
                        static_cast<uint64>(StartRow + Row) * Width + StartColumn + Column);
                    const int32 TileIndex = static_cast<int32>(static_cast<uint64>(Row) * Stride + Column);
                    FMemory::Memcpy(&HeightBuffer[TileIndex], &Heights[GlobalIndex], sizeof(float));
                    ValidBuffer[TileIndex] = Validity[GlobalIndex];
                    ProvenanceBuffer[TileIndex] = Provenance[GlobalIndex];
                    SourceBuffer[TileIndex] = Sources[GlobalIndex];
                }
            }
            if (!Store.WriteLod0Tile(TileX, TileY, HeightBuffer, Stride,
                    ValidBuffer, Stride, ProvenanceBuffer, Stride, SourceBuffer, Stride, Error))
            {
                return false;
            }
        }
    }
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainScratchStoreBitExactLodAndProvenanceTest,
    "MountainPlanner.P1.TerrainScratch.BitExactIndexedLodsAndProvenance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainScratchStoreBitExactLodAndProvenanceTest::RunTest(const FString& Parameters)
{
    constexpr uint32 Width = 35;
    constexpr uint32 Height = 19;
    uint64 RequiredBytes = 0;
    uint32 TilesX = 0, TilesY = 0;
    TestTrue(TEXT("Checked storage preflight accepts a small 1 m grid"),
        SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(
            Width, Height, RequiredBytes, &TilesX, &TilesY));
    TestEqual(TEXT("Three disjoint sample tiles cover the east axis"), TilesX, uint32(1));
    TestEqual(TEXT("One disjoint sample tile covers the south axis"), TilesY, uint32(1));
    TestEqual(TEXT("Storage includes float, validity, class, and source index bytes"),
        RequiredBytes, static_cast<uint64>(static_cast<uint64>(Width) * Height * 7ULL));

    TArray<float> Heights;
    TArray<uint8> Validity, Provenance, Sources;
    MakeCanonicalSamples(Width, Height, Heights, Validity, Provenance, Sources);
    const FString Root = ScratchTestRoot(TEXT("TerrainScratchBitExact"));
    SkiPreparation::TerrainScratchStore Store;
    FString Error;
    TestTrue(TEXT("1 m canonical store creates within budget"), Store.Create(Root,
        Width, Height, 1.0, 1.0, ScratchSources(), RequiredBytes, Error));
    TestTrue(TEXT("Padded input rows and out-of-order disjoint writes are accepted"),
        WriteAllScratchTiles(Store, Width, Height, Heights, Validity, Provenance, Sources, Error));
    TestFalse(TEXT("A completed canonical LOD0 tile cannot be written twice"),
        Store.WriteLod0Tile(0, 0, Heights, Width, Validity, Width, Provenance, Width,
            Sources, Width, Error));
    TestTrue(TEXT("All canonical LOD0 tiles seal into a readable store"), Store.Finalize(Error));

    SkiDomain::TerrainCoreTilePlan Plan;
    TestTrue(TEXT("Existing TerrainCore plan is available"),
        SkiDomain::PlanTerrainCoreTiles(Width, Height, Plan));
    uint32 LodTilesChecked = 0;
    for (const SkiDomain::TerrainCoreTileDescriptor& Descriptor : Plan.Tiles)
    {
        SkiPreparation::TerrainScratchLodTile Tile;
        TestTrue(*FString::Printf(TEXT("LOD %u tile (%u,%u) copies canonical bytes: %u"),
            Descriptor.LodIndex, Descriptor.TileX, Descriptor.TileY, LodTilesChecked),
            Store.ReadLodTile(Descriptor, Tile, Error));
        if (Tile.Heights.IsEmpty()) continue;
        TestEqual(TEXT("Stored tile dimensions include the planned halo"),
            Tile.Width, static_cast<uint16>(Descriptor.CoreWidth + Descriptor.HaloWest + Descriptor.HaloEast));
        TestEqual(TEXT("Stored tile has one validity, provenance, and source byte per height"),
            Tile.Validity.Num(), Tile.Heights.Num());
        TestEqual(TEXT("Provenance plane remains sample-aligned"),
            Tile.Provenance.Num(), Tile.Heights.Num());
        TestEqual(TEXT("Source-index plane remains sample-aligned"),
            Tile.SourceIndices.Num(), Tile.Heights.Num());
        const uint32 Factor = Descriptor.LodFactor;
        const uint32 StoredWidth = Tile.Width;
        for (uint32 Row = 0; Row < Tile.Height; ++Row)
        {
            const uint64 LodRow = static_cast<uint64>(Descriptor.StartRow) + Row - Descriptor.HaloNorth;
            const uint32 SourceRow = static_cast<uint32>(FMath::Min<uint64>(LodRow * Factor, Height - 1ULL));
            for (uint32 Column = 0; Column < Tile.Width; ++Column)
            {
                const uint64 LodColumn = static_cast<uint64>(Descriptor.StartColumn) + Column - Descriptor.HaloWest;
                const uint32 SourceColumn = static_cast<uint32>(FMath::Min<uint64>(LodColumn * Factor, Width - 1ULL));
                const int32 SourceSample = static_cast<int32>(static_cast<uint64>(SourceRow) * Width + SourceColumn);
                const int32 TileSample = static_cast<int32>(static_cast<uint64>(Row) * StoredWidth + Column);
                TestTrue(TEXT("LOD height bits equal the selected LOD0 height bits"),
                    FMemory::Memcmp(&Tile.Heights[TileSample], &Heights[SourceSample], sizeof(float)) == 0);
                TestEqual(TEXT("LOD validity byte copies its selected LOD0 sample"),
                    Tile.Validity[TileSample], Validity[SourceSample]);
                TestEqual(TEXT("LOD provenance class copies its selected LOD0 sample"),
                    Tile.Provenance[TileSample], Provenance[SourceSample]);
                TestEqual(TEXT("LOD product source index copies its selected LOD0 sample"),
                    Tile.SourceIndices[TileSample], Sources[SourceSample]);
            }
        }
        ++LodTilesChecked;
    }
    TestEqual(TEXT("One LOD tile per level is exercised for this fixture"), LodTilesChecked, uint32(5));
    TestTrue(TEXT("Scratch directory is removed explicitly"), Store.Cleanup(Error));
    TestFalse(TEXT("Cleanup removed its owned directory"), IFileManager::Get().DirectoryExists(*Root));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainScratchStoreBoundsAndMalformedInputsTest,
    "MountainPlanner.P1.TerrainScratch.BoundsAndMalformedInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainScratchStoreBoundsAndMalformedInputsTest::RunTest(const FString& Parameters)
{
    uint64 RequiredBytes = 0;
    TestFalse(TEXT("Single-column grids are rejected"),
        SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(1, 8, RequiredBytes));
    TestFalse(TEXT("Product-overflow and TerrainCore maximum dimensions are rejected"),
        SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(
            MAX_uint32, MAX_uint32, RequiredBytes));
    TestFalse(TEXT("Dimensions exceeding the TerrainCore sample limit are rejected"),
        SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(
            10002, 10001, RequiredBytes));

    const TArray<SkiDomain::TerrainCoreSource> Sources = ScratchSources();
    const FString Root = ScratchTestRoot(TEXT("TerrainScratchMalformed"));
    SkiPreparation::TerrainScratchStore Store;
    FString Error;
    TestFalse(TEXT("Non-1 m spacing cannot enter the canonical store"), Store.Create(
        Root, 4, 3, 2.0, 1.0, Sources, MAX_uint64, Error));
    TestFalse(TEXT("Under-budget scratch creation is rejected before directory creation"), Store.Create(
        Root, 4, 3, 1.0, 1.0, Sources, 1, Error));
    TestFalse(TEXT("Duplicate source IDs are rejected"), [&]()
    {
        TArray<SkiDomain::TerrainCoreSource> DuplicateSources = Sources;
        DuplicateSources[1].SourceId = DuplicateSources[0].SourceId;
        return Store.Create(Root, 4, 3, 1.0, 1.0, DuplicateSources, 1024, Error);
    }());
    TestTrue(TEXT("Valid dimensions create after failed preflight attempts"),
        Store.Create(Root, 4, 3, 1.0, 1.0, Sources, 1024, Error));

    TArray<float> Heights;
    TArray<uint8> Validity, Provenance, SourceIndices;
    MakeCanonicalSamples(4, 3, Heights, Validity, Provenance, SourceIndices);
    TestFalse(TEXT("A too-short height stride is rejected"), Store.WriteLod0Tile(
        0, 0, Heights, 3, Validity, 4, Provenance, 4, SourceIndices, 4, Error));
    TestFalse(TEXT("An overflowing row stride is rejected"), Store.WriteLod0Tile(
        0, 0, Heights, MAX_uint64, Validity, 4, Provenance, 4, SourceIndices, 4, Error));
    TestFalse(TEXT("Insufficient source row data is rejected"), Store.WriteLod0Tile(
        0, 0, Heights, 4, Validity, 4, Provenance, 4, TArrayView<const uint8>(), 4, Error));

    TArray<uint8> BadValidity = Validity;
    BadValidity[0] = 2;
    TestFalse(TEXT("Validity is exactly zero or one"), Store.WriteLod0Tile(
        0, 0, Heights, 4, BadValidity, 4, Provenance, 4, SourceIndices, 4, Error));
    TArray<uint8> BadProvenance = Provenance;
    BadProvenance[0] = 42;
    TestFalse(TEXT("Unknown provenance classes are rejected"), Store.WriteLod0Tile(
        0, 0, Heights, 4, Validity, 4, BadProvenance, 4, SourceIndices, 4, Error));
    TArray<uint8> BadSources = SourceIndices;
    BadSources[0] = 2;
    TestFalse(TEXT("Sample source indices must resolve through the source table"), Store.WriteLod0Tile(
        0, 0, Heights, 4, Validity, 4, Provenance, 4, BadSources, 4, Error));
    TArray<float> BadHeights = Heights;
    BadHeights[0] = std::numeric_limits<float>::quiet_NaN();
    TestFalse(TEXT("Valid samples cannot contain NaN height"), Store.WriteLod0Tile(
        0, 0, BadHeights, 4, Validity, 4, Provenance, 4, SourceIndices, 4, Error));
    BadValidity = Validity;
    BadProvenance = Provenance;
    BadSources = SourceIndices;
    BadValidity[0] = 0;
    BadProvenance[0] = static_cast<uint8>(SkiPreparation::TerrainScratchProvenance::NoData);
    BadSources[0] = SkiPreparation::TerrainScratchNoSourceIndex;
    TestTrue(TEXT("Well-formed tile writes after rejected malformed inputs"), Store.WriteLod0Tile(
        0, 0, Heights, 4, BadValidity, 4, BadProvenance, 4, BadSources, 4, Error));
    TestTrue(TEXT("Well-formed canonical tile finalizes"), Store.Finalize(Error));
    SkiDomain::TerrainCoreTilePlan Plan;
    TestTrue(TEXT("A read plan exists for the finalized raster"), SkiDomain::PlanTerrainCoreTiles(4, 3, Plan));
    SkiPreparation::TerrainScratchLodTile Tile;
    SkiDomain::TerrainCoreTileDescriptor WrongTile = Plan.Tiles[0];
    WrongTile.LodFactor = 2;
    TestFalse(TEXT("A plan with a mutated LOD factor is rejected"), Store.ReadLodTile(WrongTile, Tile, Error));
    TestTrue(TEXT("Read failure clears the reused tile"), Tile.Heights.IsEmpty()
        && Tile.Validity.IsEmpty() && Tile.Provenance.IsEmpty() && Tile.SourceIndices.IsEmpty());
    TestTrue(TEXT("Malformed-input scratch cleans up"), Store.Cleanup(Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainScratchStoreMultiBlockEdgeTest,
    "MountainPlanner.P1.TerrainScratch.MultiBlockEdgesAndClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainScratchStoreMultiBlockEdgeTest::RunTest(const FString& Parameters)
{
    constexpr uint32 Width = 257;
    constexpr uint32 Height = 258;
    uint64 RequiredBytes = 0;
    uint32 TilesX = 0, TilesY = 0;
    TestTrue(TEXT("Multi-block dimensions pass checked storage preflight"),
        SkiPreparation::TerrainScratchStore::TryCalculateRequiredStorageBytes(
            Width, Height, RequiredBytes, &TilesX, &TilesY));
    TestEqual(TEXT("East edge requires a second disjoint scratch block"), TilesX, uint32(2));
    TestEqual(TEXT("South edge requires a second disjoint scratch block"), TilesY, uint32(2));

    TArray<float> Heights;
    TArray<uint8> Validity, Provenance, Sources;
    MakeCanonicalSamples(Width, Height, Heights, Validity, Provenance, Sources);
    const FString Root = ScratchTestRoot(TEXT("TerrainScratchEdges"));
    SkiPreparation::TerrainScratchStore Store;
    FString Error;
    TestTrue(TEXT("Multi-block canonical scratch creates"), Store.Create(Root, Width,
        Height, 1.0, 1.0, ScratchSources(), RequiredBytes, Error));
    TestTrue(TEXT("Partial east and south blocks write without overlap"),
        WriteAllScratchTiles(Store, Width, Height, Heights, Validity, Provenance, Sources, Error));
    TestTrue(TEXT("Every disjoint block seals"), Store.Finalize(Error));

    SkiDomain::TerrainCoreTilePlan Plan;
    TestTrue(TEXT("TerrainCore plans all LOD edge tiles"),
        SkiDomain::PlanTerrainCoreTiles(Width, Height, Plan));
    for (const SkiDomain::TerrainCoreTileDescriptor& Descriptor : Plan.Tiles)
    {
        if (Descriptor.LodIndex == 0 && Descriptor.TileX == 1 && Descriptor.TileY == 1)
        {
            SkiPreparation::TerrainScratchLodTile Tile;
            TestTrue(TEXT("Bottom-right LOD0 tile and halo read"),
                Store.ReadLodTile(Descriptor, Tile, Error));
            if (!Tile.Heights.IsEmpty())
            {
                const int32 Last = Tile.Heights.Num() - 1;
                const int32 SourceLast = static_cast<int32>(static_cast<uint64>(Height - 1) * Width + Width - 1);
                TestTrue(TEXT("Bottom-right partial-block height remains bit exact"),
                    FMemory::Memcmp(&Tile.Heights[Last], &Heights[SourceLast], sizeof(float)) == 0);
                TestEqual(TEXT("Bottom-right partial-block validity is copied"),
                    Tile.Validity[Last], Validity[SourceLast]);
                TestEqual(TEXT("Bottom-right partial-block provenance is copied"),
                    Tile.Provenance[Last], Provenance[SourceLast]);
                TestEqual(TEXT("Bottom-right partial-block source identity is copied"),
                    Tile.SourceIndices[Last], Sources[SourceLast]);
            }
        }
        if (Descriptor.LodIndex == 4)
        {
            SkiPreparation::TerrainScratchLodTile Tile;
            TestTrue(TEXT("Coarsest LOD reads a selected sample across scratch blocks"),
                Store.ReadLodTile(Descriptor, Tile, Error));
            if (!Tile.Heights.IsEmpty())
            {
                const int32 Last = Tile.Heights.Num() - 1;
                const int32 SourceLast = static_cast<int32>(static_cast<uint64>(Height - 1) * Width + Width - 1);
                TestTrue(TEXT("Coarsest edge sample uses edge-clamped LOD0 float bytes"),
                    FMemory::Memcmp(&Tile.Heights[Last], &Heights[SourceLast], sizeof(float)) == 0);
                TestEqual(TEXT("Coarsest nodata validity is an indexed copy"),
                    Tile.Validity[Last], Validity[SourceLast]);
                TestEqual(TEXT("Coarsest nodata provenance is an indexed copy"),
                    Tile.Provenance[Last], Provenance[SourceLast]);
                TestEqual(TEXT("Coarsest nodata source sentinel is an indexed copy"),
                    Tile.SourceIndices[Last], Sources[SourceLast]);
            }
        }
    }
    TestTrue(TEXT("Multi-block edge scratch cleans up"), Store.Cleanup(Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTerrainScratchStoreCancellationTest,
    "MountainPlanner.P1.TerrainScratch.CancellationInvalidatesStore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTerrainScratchStoreCancellationTest::RunTest(const FString& Parameters)
{
    const FString Root = ScratchTestRoot(TEXT("TerrainScratchCancellation"));
    SkiPreparation::TerrainScratchStore Store;
    FString Error;
    TestTrue(TEXT("Cancellation fixture store creates"), Store.Create(Root, 4, 3,
        1.0, 1.0, ScratchSources(), 1024, Error));
    TArray<float> Heights;
    TArray<uint8> Validity, Provenance, SourceIndices;
    MakeCanonicalSamples(4, 3, Heights, Validity, Provenance, SourceIndices);
    SkiPreparation::Cancellation Cancelled;
    Cancelled.Cancel();
    TestFalse(TEXT("Cancelled tile production is rejected"), Store.WriteLod0Tile(
        0, 0, Heights, 4, Validity, 4, Provenance, 4, SourceIndices, 4,
        Error, &Cancelled));
    TestFalse(TEXT("Cancellation invalidates all future writes"), Store.WriteLod0Tile(
        0, 0, Heights, 4, Validity, 4, Provenance, 4, SourceIndices, 4, Error));
    TestFalse(TEXT("Cancelled store cannot finalize"), Store.Finalize(Error));
    TestTrue(TEXT("Cancelled scratch cleanup succeeds"), Store.Cleanup(Error));
    return true;
}

#endif
