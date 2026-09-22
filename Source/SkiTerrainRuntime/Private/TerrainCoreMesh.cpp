#include "SkiTerrainRuntime/TerrainCoreMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
bool IsValidAt(const SkiApplication::TerrainCoreTilePayload& Payload,
    const std::size_t Index) noexcept
{
    return Index < Payload.Heights.size() && Index < Payload.Validity.size()
        && Payload.Validity[Index] != 0 && std::isfinite(Payload.Heights[Index]);
}
}

bool SkiTerrainRuntime::BuildTerrainCoreTileMesh(
    const SkiApplication::TerrainCoreTilePayload& Payload,
    const SkiDomain::TerrainCoreManifest& Manifest,
    const SkiDomain::Revision Revision,
    const bool AddSkirts,
    const double SkirtDepthM,
    SkiDomain::TerrainTileMesh& OutMesh) noexcept
{
    OutMesh = {};
    const SkiDomain::TerrainCoreTileDescriptor& Tile = Payload.Descriptor;
    if (Revision == 0 || !SkiDomain::ValidateTerrainCore(Manifest).Ok()
        || Payload.Key.Lod != Tile.LodIndex || Payload.Key.X != Tile.TileX
        || Payload.Key.Y != Tile.TileY || Tile.CoreWidth < 2 || Tile.CoreHeight < 2
        || !std::isfinite(SkirtDepthM) || SkirtDepthM < 0.0)
    {
        return false;
    }
    const std::uint32_t StoredWidth = static_cast<std::uint32_t>(Tile.CoreWidth)
        + Tile.HaloWest + Tile.HaloEast;
    const std::uint32_t StoredHeight = static_cast<std::uint32_t>(Tile.CoreHeight)
        + Tile.HaloNorth + Tile.HaloSouth;
    const std::uint64_t StoredSamples = static_cast<std::uint64_t>(StoredWidth) * StoredHeight;
    if (StoredSamples != Payload.Heights.size() || StoredSamples != Payload.Validity.size())
    {
        return false;
    }

    SkiDomain::TerrainTileMesh Mesh;
    Mesh.Key = {Tile.TileX, Tile.TileY, Tile.LodIndex};
    Mesh.SourceRevision = Revision;
    const std::uint32_t CoreWidth = Tile.CoreWidth;
    const std::uint32_t CoreHeight = Tile.CoreHeight;
    const double EastStep = Manifest.DeliveredEastSpacingM * Tile.LodFactor;
    const double NorthStep = Manifest.DeliveredNorthSpacingM * Tile.LodFactor;
    Mesh.Vertices.reserve(static_cast<std::size_t>(CoreWidth) * CoreHeight
        + (AddSkirts ? static_cast<std::size_t>(CoreWidth + CoreHeight) * 2U : 0U));
    std::vector<std::uint8_t> CoreValid(static_cast<std::size_t>(CoreWidth) * CoreHeight, 0);

    const auto StoredIndex = [StoredWidth](const std::uint32_t Row,
        const std::uint32_t Column) noexcept
    {
        return static_cast<std::size_t>(Row) * StoredWidth + Column;
    };
    for (std::uint32_t Row = 0; Row < CoreHeight; ++Row)
    {
        for (std::uint32_t Column = 0; Column < CoreWidth; ++Column)
        {
            const std::uint32_t StoredRow = Row + Tile.HaloNorth;
            const std::uint32_t StoredColumn = Column + Tile.HaloWest;
            const std::size_t Center = StoredIndex(StoredRow, StoredColumn);
            const bool Valid = IsValidAt(Payload, Center);
            CoreValid[static_cast<std::size_t>(Row) * CoreWidth + Column] = Valid ? 1 : 0;

            const std::uint32_t LeftColumn = StoredColumn == 0 ? 0 : StoredColumn - 1;
            const std::uint32_t RightColumn = std::min(StoredWidth - 1, StoredColumn + 1);
            const std::uint32_t NorthRow = StoredRow == 0 ? 0 : StoredRow - 1;
            const std::uint32_t SouthRow = std::min(StoredHeight - 1, StoredRow + 1);
            double EastSlope = 0.0;
            double NorthSlope = 0.0;
            const std::size_t Left = StoredIndex(StoredRow, LeftColumn);
            const std::size_t Right = StoredIndex(StoredRow, RightColumn);
            const std::size_t North = StoredIndex(NorthRow, StoredColumn);
            const std::size_t South = StoredIndex(SouthRow, StoredColumn);
            if (IsValidAt(Payload, Left) && IsValidAt(Payload, Right)
                && RightColumn != LeftColumn)
            {
                EastSlope = (Payload.Heights[Right] - Payload.Heights[Left])
                    / (static_cast<double>(RightColumn - LeftColumn) * EastStep);
            }
            if (IsValidAt(Payload, North) && IsValidAt(Payload, South)
                && SouthRow != NorthRow)
            {
                NorthSlope = (Payload.Heights[North] - Payload.Heights[South])
                    / (static_cast<double>(SouthRow - NorthRow) * NorthStep);
            }
            const double NormalLength = std::sqrt(EastSlope * EastSlope
                + NorthSlope * NorthSlope + 1.0);
            const std::uint64_t FineColumn = static_cast<std::uint64_t>(Tile.StartColumn + Column)
                * Tile.LodFactor;
            const std::uint64_t FineRow = static_cast<std::uint64_t>(Tile.StartRow + Row)
                * Tile.LodFactor;
            const double EastM = Manifest.SampleCenterBounds.WestM
                + static_cast<double>(FineColumn) * Manifest.DeliveredEastSpacingM;
            const double NorthM = Manifest.SampleCenterBounds.NorthM
                - static_cast<double>(FineRow) * Manifest.DeliveredNorthSpacingM;
            Mesh.Vertices.push_back({static_cast<float>(EastM), static_cast<float>(NorthM),
                Valid ? Payload.Heights[Center] : 0.0F,
                static_cast<float>(-EastSlope / NormalLength),
                static_cast<float>(-NorthSlope / NormalLength),
                static_cast<float>(1.0 / NormalLength),
                static_cast<float>(FineColumn) / static_cast<float>(Manifest.Width - 1U),
                static_cast<float>(FineRow) / static_cast<float>(Manifest.Height - 1U)});
        }
    }

    for (std::uint32_t Row = 0; Row + 1 < CoreHeight; ++Row)
    {
        for (std::uint32_t Column = 0; Column + 1 < CoreWidth; ++Column)
        {
            const std::uint32_t NorthWest = Row * CoreWidth + Column;
            const std::uint32_t NorthEast = NorthWest + 1U;
            const std::uint32_t SouthWest = NorthWest + CoreWidth;
            const std::uint32_t SouthEast = SouthWest + 1U;
            const auto SampleValid = [&CoreValid](const std::uint32_t Index) noexcept
            {
                return Index < CoreValid.size() && CoreValid[Index] != 0;
            };
            if (SampleValid(NorthWest) && SampleValid(SouthWest) && SampleValid(SouthEast))
            {
                Mesh.Indices.insert(Mesh.Indices.end(),
                    {NorthWest, SouthWest, SouthEast});
            }
            if (SampleValid(NorthWest) && SampleValid(SouthEast) && SampleValid(NorthEast))
            {
                Mesh.Indices.insert(Mesh.Indices.end(),
                    {NorthWest, SouthEast, NorthEast});
            }
        }
    }

    // A decoded tile can be structurally valid while contributing no triangles (for
    // example, an all-nodata tile). Treat that as a completed empty tile so streaming
    // does not retry it, and do not retain vertices that can never be rendered.
    if (Mesh.Indices.empty())
    {
        Mesh.Vertices.clear();
        OutMesh = std::move(Mesh);
        return true;
    }

    if (AddSkirts && SkirtDepthM > 0.0)
    {
        std::vector<std::uint32_t> Border;
        Border.reserve(static_cast<std::size_t>(CoreWidth + CoreHeight) * 2U - 4U);
        for (std::uint32_t Column = 0; Column < CoreWidth; ++Column) Border.push_back(Column);
        for (std::uint32_t Row = 1; Row < CoreHeight; ++Row)
            Border.push_back(Row * CoreWidth + CoreWidth - 1U);
        for (std::int64_t Column = static_cast<std::int64_t>(CoreWidth) - 2; Column >= 0; --Column)
            Border.push_back((CoreHeight - 1U) * CoreWidth + static_cast<std::uint32_t>(Column));
        for (std::int64_t Row = static_cast<std::int64_t>(CoreHeight) - 2; Row >= 1; --Row)
            Border.push_back(static_cast<std::uint32_t>(Row) * CoreWidth);
        const std::uint32_t SkirtStart = static_cast<std::uint32_t>(Mesh.Vertices.size());
        for (const std::uint32_t VertexIndex : Border)
        {
            SkiDomain::TerrainVertex Vertex = Mesh.Vertices[VertexIndex];
            Vertex.UpM -= static_cast<float>(SkirtDepthM);
            Mesh.Vertices.push_back(Vertex);
        }
        for (std::uint32_t Index = 0; Index < Border.size(); ++Index)
        {
            const std::uint32_t Next = (Index + 1U) % static_cast<std::uint32_t>(Border.size());
            if (CoreValid[Border[Index]] == 0 || CoreValid[Border[Next]] == 0) continue;
            Mesh.Indices.insert(Mesh.Indices.end(), {Border[Index], SkirtStart + Index,
                SkirtStart + Next, Border[Index], SkirtStart + Next, Border[Next]});
        }
    }
    OutMesh = std::move(Mesh);
    return true;
}
