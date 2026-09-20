#include "SkiDomain/TerrainTile.h"

#include <algorithm>
#include <cmath>

namespace
{
SkiDomain::EnuVector NormalAt(const SkiDomain::Heightfield& Field,
    const std::uint32_t Row, const std::uint32_t Column) noexcept
{
    const std::uint32_t Left = Column == 0 ? 0 : Column - 1;
    const std::uint32_t Right = std::min(Field.Width - 1, Column + 1);
    const std::uint32_t North = Row == 0 ? 0 : Row - 1;
    const std::uint32_t South = std::min(Field.Height - 1, Row + 1);
    const double HeightLeft = Field.Samples[static_cast<std::size_t>(Row) * Field.Width + Left];
    const double HeightRight = Field.Samples[static_cast<std::size_t>(Row) * Field.Width + Right];
    const double HeightNorth = Field.Samples[static_cast<std::size_t>(North) * Field.Width + Column];
    const double HeightSouth = Field.Samples[static_cast<std::size_t>(South) * Field.Width + Column];
    const double EastSlope = (HeightRight - HeightLeft)
        / (static_cast<double>(Right - Left == 0 ? 1 : Right - Left) * Field.EastSpacingM);
    const double NorthSlope = (HeightNorth - HeightSouth)
        / (static_cast<double>(South - North == 0 ? 1 : South - North) * Field.NorthSpacingM);
    const double Length = std::sqrt(EastSlope * EastSlope + NorthSlope * NorthSlope + 1.0);
    return {-EastSlope / Length, -NorthSlope / Length, 1.0 / Length};
}

std::vector<std::uint32_t> SampleAxis(const std::uint32_t First, const std::uint32_t Last,
    const std::uint32_t Stride)
{
    std::vector<std::uint32_t> Values;
    for (std::uint32_t Value = First; Value < Last; Value += Stride)
    {
        Values.push_back(Value);
    }
    Values.push_back(Last);
    return Values;
}
}

bool SkiDomain::BuildTerrainTile(const Heightfield& Field, const TileKey& Key,
    const bool AddSkirts, const double SkirtDepthM, TerrainTileMesh& OutMesh) noexcept
{
    if (!IsValidHeightfield(Field) || Key.Lod > 2 || !std::isfinite(SkirtDepthM)
        || SkirtDepthM < 0.0)
    {
        return false;
    }
    const std::uint32_t FirstColumn = Key.X * TerrainTileCells;
    const std::uint32_t FirstRow = Key.Y * TerrainTileCells;
    if (FirstColumn >= Field.Width - 1 || FirstRow >= Field.Height - 1)
    {
        return false;
    }
    const std::uint32_t LastColumn = std::min(Field.Width - 1, FirstColumn + TerrainTileCells);
    const std::uint32_t LastRow = std::min(Field.Height - 1, FirstRow + TerrainTileCells);
    const std::uint32_t Stride = 1U << Key.Lod;
    const std::vector<std::uint32_t> Columns = SampleAxis(FirstColumn, LastColumn, Stride);
    const std::vector<std::uint32_t> Rows = SampleAxis(FirstRow, LastRow, Stride);
    TerrainTileMesh Mesh;
    Mesh.Key = Key;
    Mesh.SourceRevision = Field.CurrentRevision;
    Mesh.Vertices.reserve(Rows.size() * Columns.size());
    for (const std::uint32_t Row : Rows)
    {
        for (const std::uint32_t Column : Columns)
        {
            const float Height = Field.Samples[static_cast<std::size_t>(Row) * Field.Width + Column];
            if (!std::isfinite(Height) || static_cast<double>(Height) == Field.NoDataValue)
            {
                return false;
            }
            const EnuVector Normal = NormalAt(Field, Row, Column);
            Mesh.Vertices.push_back({static_cast<float>(Field.EastM(Column)),
                static_cast<float>(Field.SampleNorthM(Row)), Height,
                static_cast<float>(Normal.East), static_cast<float>(Normal.North),
                static_cast<float>(Normal.Up),
                static_cast<float>(Column) / static_cast<float>(Field.Width - 1),
                static_cast<float>(Row) / static_cast<float>(Field.Height - 1)});
        }
    }
    const std::uint32_t RowWidth = static_cast<std::uint32_t>(Columns.size());
    for (std::uint32_t Row = 0; Row + 1 < Rows.size(); ++Row)
    {
        for (std::uint32_t Column = 0; Column + 1 < Columns.size(); ++Column)
        {
            const std::uint32_t NorthWest = Row * RowWidth + Column;
            const std::uint32_t NorthEast = NorthWest + 1;
            const std::uint32_t SouthWest = NorthWest + RowWidth;
            const std::uint32_t SouthEast = SouthWest + 1;
            Mesh.Indices.insert(Mesh.Indices.end(),
                {NorthWest, SouthWest, SouthEast, NorthWest, SouthEast, NorthEast});
        }
    }
    if (AddSkirts && SkirtDepthM > 0.0)
    {
        std::vector<std::uint32_t> Border;
        for (std::uint32_t Column = 0; Column < RowWidth; ++Column) Border.push_back(Column);
        for (std::uint32_t Row = 1; Row < Rows.size(); ++Row) Border.push_back(Row * RowWidth + RowWidth - 1);
        const std::uint32_t LastMeshRow = static_cast<std::uint32_t>(Rows.size() - 1);
        for (int Column = static_cast<int>(RowWidth) - 2; Column >= 0; --Column)
        {
            Border.push_back(LastMeshRow * RowWidth + static_cast<std::uint32_t>(Column));
        }
        for (int Row = static_cast<int>(LastMeshRow) - 1; Row >= 1; --Row)
        {
            Border.push_back(static_cast<std::uint32_t>(Row) * RowWidth);
        }
        const std::uint32_t SkirtStart = static_cast<std::uint32_t>(Mesh.Vertices.size());
        for (const std::uint32_t Index : Border)
        {
            TerrainVertex Vertex = Mesh.Vertices[Index];
            Vertex.UpM -= static_cast<float>(SkirtDepthM);
            Mesh.Vertices.push_back(Vertex);
        }
        for (std::uint32_t Index = 0; Index < Border.size(); ++Index)
        {
            const std::uint32_t Next = (Index + 1) % static_cast<std::uint32_t>(Border.size());
            Mesh.Indices.insert(Mesh.Indices.end(), {Border[Index], SkirtStart + Index,
                SkirtStart + Next, Border[Index], SkirtStart + Next, Border[Next]});
        }
    }
    OutMesh = std::move(Mesh);
    return true;
}

bool SkiDomain::ValidateAdjacentLods(const std::vector<TileKey>& Keys) noexcept
{
    for (std::size_t First = 0; First < Keys.size(); ++First)
    {
        if (Keys[First].Lod > 2) return false;
        for (std::size_t Second = First + 1; Second < Keys.size(); ++Second)
        {
            if (Keys[First].X == Keys[Second].X && Keys[First].Y == Keys[Second].Y) return false;
            const std::uint32_t DeltaX = Keys[First].X > Keys[Second].X
                ? Keys[First].X - Keys[Second].X : Keys[Second].X - Keys[First].X;
            const std::uint32_t DeltaY = Keys[First].Y > Keys[Second].Y
                ? Keys[First].Y - Keys[Second].Y : Keys[Second].Y - Keys[First].Y;
            if (DeltaX + DeltaY == 1)
            {
                const int Difference = static_cast<int>(Keys[First].Lod) - static_cast<int>(Keys[Second].Lod);
                if (std::abs(Difference) > 1) return false;
            }
        }
    }
    return true;
}
