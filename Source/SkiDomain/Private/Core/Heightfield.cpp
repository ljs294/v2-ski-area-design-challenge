#include "SkiDomain/Heightfield.h"
#include "SkiDomain/TerrainPackage.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using SkiDomain::EnuVector;

EnuVector Subtract(const EnuVector& A, const EnuVector& B) noexcept
{
    return {A.East - B.East, A.North - B.North, A.Up - B.Up};
}

EnuVector Cross(const EnuVector& A, const EnuVector& B) noexcept
{
    return {A.North * B.Up - A.Up * B.North,
        A.Up * B.East - A.East * B.Up,
        A.East * B.North - A.North * B.East};
}

double Dot(const EnuVector& A, const EnuVector& B) noexcept
{
    return A.East * B.East + A.North * B.North + A.Up * B.Up;
}

bool IntersectTriangle(const SkiDomain::Ray& Ray, const EnuVector& A, const EnuVector& B,
    const EnuVector& C, double& OutDistance) noexcept
{
    constexpr double Epsilon = 1e-10;
    const EnuVector Edge1 = Subtract(B, A);
    const EnuVector Edge2 = Subtract(C, A);
    const EnuVector P = Cross(Ray.Direction, Edge2);
    const double Determinant = Dot(Edge1, P);
    if (std::abs(Determinant) <= Epsilon)
    {
        return false;
    }
    const double Inverse = 1.0 / Determinant;
    const EnuVector T = Subtract(Ray.Origin, A);
    const double U = Dot(T, P) * Inverse;
    if (U < 0.0 || U > 1.0)
    {
        return false;
    }
    const EnuVector Q = Cross(T, Edge1);
    const double V = Dot(Ray.Direction, Q) * Inverse;
    if (V < 0.0 || U + V > 1.0)
    {
        return false;
    }
    const double Distance = Dot(Edge2, Q) * Inverse;
    if (Distance < 0.0 || !std::isfinite(Distance))
    {
        return false;
    }
    OutDistance = Distance;
    return true;
}

bool ValidSample(const SkiDomain::Heightfield& Field, const float Value) noexcept
{
    return std::isfinite(Value) && static_cast<double>(Value) != Field.NoDataValue;
}

bool IntersectCell(const SkiDomain::Heightfield& Field, const SkiDomain::Ray& Ray,
    const std::uint32_t Row, const std::uint32_t Column, double& OutDistance) noexcept
{
    const std::size_t Width = Field.Width;
    const float H00 = Field.Samples[static_cast<std::size_t>(Row) * Width + Column];
    const float H10 = Field.Samples[static_cast<std::size_t>(Row) * Width + Column + 1];
    const float H01 = Field.Samples[static_cast<std::size_t>(Row + 1) * Width + Column];
    const float H11 = Field.Samples[static_cast<std::size_t>(Row + 1) * Width + Column + 1];
    if (!ValidSample(Field, H00) || !ValidSample(Field, H10)
        || !ValidSample(Field, H01) || !ValidSample(Field, H11))
    {
        return false;
    }
    const double West = Field.EastM(Column);
    const double East = Field.EastM(Column + 1);
    const double North = Field.SampleNorthM(Row);
    const double South = Field.SampleNorthM(Row + 1);
    const EnuVector NorthWest{West, North, H00};
    const EnuVector NorthEast{East, North, H10};
    const EnuVector SouthWest{West, South, H01};
    const EnuVector SouthEast{East, South, H11};
    double First = 0.0;
    double Second = 0.0;
    const bool HitFirst = IntersectTriangle(Ray, NorthWest, SouthWest, SouthEast, First);
    const bool HitSecond = IntersectTriangle(Ray, NorthWest, SouthEast, NorthEast, Second);
    if (!HitFirst && !HitSecond)
    {
        return false;
    }
    OutDistance = HitFirst && HitSecond ? std::min(First, Second) : (HitFirst ? First : Second);
    return true;
}

bool Slab(const double Origin, const double Direction, const double Minimum, const double Maximum,
    double& Enter, double& Exit) noexcept
{
    constexpr double Epsilon = 1e-12;
    if (std::abs(Direction) <= Epsilon)
    {
        return Origin >= Minimum && Origin <= Maximum;
    }
    double A = (Minimum - Origin) / Direction;
    double B = (Maximum - Origin) / Direction;
    if (A > B)
    {
        std::swap(A, B);
    }
    Enter = std::max(Enter, A);
    Exit = std::min(Exit, B);
    return Enter <= Exit;
}
}

bool SkiDomain::IsValidHeightfield(const Heightfield& Field) noexcept
{
    const std::uint64_t Product = static_cast<std::uint64_t>(Field.Width) * Field.Height;
    return Field.Width >= 2 && Field.Height >= 2 && Product <= MaxHeightSamples
        && Product == Field.Samples.size() && std::isfinite(Field.WestM)
        && std::isfinite(Field.NorthM) && std::isfinite(Field.EastSpacingM)
        && std::isfinite(Field.NorthSpacingM) && Field.EastSpacingM > 0.0
        && Field.NorthSpacingM > 0.0 && std::isfinite(Field.NoDataValue);
}

SkiDomain::RayHit SkiDomain::QueryHeightfield(const Heightfield& Field, const Ray& Query) noexcept
{
    RayHit Result;
    if (!IsValidHeightfield(Field) || !std::isfinite(Query.Origin.East)
        || !std::isfinite(Query.Origin.North) || !std::isfinite(Query.Origin.Up)
        || !std::isfinite(Query.Direction.East) || !std::isfinite(Query.Direction.North)
        || !std::isfinite(Query.Direction.Up))
    {
        return Result;
    }
    const double DirectionLength = std::sqrt(Dot(Query.Direction, Query.Direction));
    if (DirectionLength <= 1e-12)
    {
        return Result;
    }
    const double West = Field.WestM;
    const double East = Field.EastM(Field.Width - 1);
    const double North = Field.NorthM;
    const double South = Field.SampleNorthM(Field.Height - 1);
    double Enter = 0.0;
    double Exit = std::numeric_limits<double>::infinity();
    if (!Slab(Query.Origin.East, Query.Direction.East, West, East, Enter, Exit)
        || !Slab(Query.Origin.North, Query.Direction.North, South, North, Enter, Exit)
        || Exit < 0.0)
    {
        return Result;
    }
    Enter = std::max(0.0, Enter);
    const double StartEast = Query.Origin.East + Query.Direction.East * Enter;
    const double StartNorth = Query.Origin.North + Query.Direction.North * Enter;
    double GridX = (StartEast - West) / Field.EastSpacingM;
    double GridY = (North - StartNorth) / Field.NorthSpacingM;
    GridX = std::clamp(GridX, 0.0, static_cast<double>(Field.Width - 1) - 1e-9);
    GridY = std::clamp(GridY, 0.0, static_cast<double>(Field.Height - 1) - 1e-9);
    int Column = static_cast<int>(std::floor(GridX));
    int Row = static_cast<int>(std::floor(GridY));
    const double GridDirectionX = Query.Direction.East / Field.EastSpacingM;
    const double GridDirectionY = -Query.Direction.North / Field.NorthSpacingM;
    const int StepX = GridDirectionX > 0.0 ? 1 : (GridDirectionX < 0.0 ? -1 : 0);
    const int StepY = GridDirectionY > 0.0 ? 1 : (GridDirectionY < 0.0 ? -1 : 0);
    const double DeltaX = StepX == 0 ? std::numeric_limits<double>::infinity()
        : std::abs(1.0 / GridDirectionX);
    const double DeltaY = StepY == 0 ? std::numeric_limits<double>::infinity()
        : std::abs(1.0 / GridDirectionY);
    double NextX = StepX == 0 ? std::numeric_limits<double>::infinity()
        : Enter + ((StepX > 0 ? Column + 1.0 : Column) - GridX) / GridDirectionX;
    double NextY = StepY == 0 ? std::numeric_limits<double>::infinity()
        : Enter + ((StepY > 0 ? Row + 1.0 : Row) - GridY) / GridDirectionY;
    const int MaxSteps = static_cast<int>(Field.Width + Field.Height) * 2;
    for (int Step = 0; Step < MaxSteps && Row >= 0 && Column >= 0
        && Row < static_cast<int>(Field.Height - 1)
        && Column < static_cast<int>(Field.Width - 1); ++Step)
    {
        double Distance = 0.0;
        if (IntersectCell(Field, Query, static_cast<std::uint32_t>(Row),
                static_cast<std::uint32_t>(Column), Distance)
            && Distance >= Enter - 1e-8 && Distance <= Exit + 1e-8)
        {
            const double CellExit = std::min(NextX, NextY);
            if (Distance <= CellExit + 1e-8)
            {
                Result.Hit = true;
                Result.Distance = Distance;
                Result.Position = {Query.Origin.East + Query.Direction.East * Distance,
                    Query.Origin.North + Query.Direction.North * Distance,
                    Query.Origin.Up + Query.Direction.Up * Distance};
                Result.Row = static_cast<std::uint32_t>(Row);
                Result.Column = static_cast<std::uint32_t>(Column);
                Result.SourceRevision = Field.CurrentRevision;
                return Result;
            }
        }
        if (NextX < NextY)
        {
            if (NextX > Exit)
            {
                break;
            }
            Column += StepX;
            NextX += DeltaX;
        }
        else
        {
            if (NextY > Exit)
            {
                break;
            }
            Row += StepY;
            NextY += DeltaY;
        }
    }
    return Result;
}

bool SkiDomain::ApplyCircularHeightDelta(Heightfield& Field, const Revision ExpectedRevision,
    const double CenterEastM, const double CenterNorthM, const double RadiusM,
    const double DeltaM, MutationBounds& OutBounds) noexcept
{
    if (!IsValidHeightfield(Field) || !std::isfinite(CenterEastM)
        || !std::isfinite(CenterNorthM) || !std::isfinite(RadiusM)
        || !std::isfinite(DeltaM) || RadiusM <= 0.0 || DeltaM == 0.0)
    {
        return false;
    }
    Revision NextRevision = 0;
    if (!TryAdvanceRevision(Field.CurrentRevision, ExpectedRevision, NextRevision))
    {
        return false;
    }
    const int MinColumn = std::max(0, static_cast<int>(std::floor(
        (CenterEastM - RadiusM - Field.WestM) / Field.EastSpacingM)));
    const int MaxColumn = std::min(static_cast<int>(Field.Width) - 1, static_cast<int>(std::ceil(
        (CenterEastM + RadiusM - Field.WestM) / Field.EastSpacingM)));
    const int MinRow = std::max(0, static_cast<int>(std::floor(
        (Field.NorthM - CenterNorthM - RadiusM) / Field.NorthSpacingM)));
    const int MaxRow = std::min(static_cast<int>(Field.Height) - 1, static_cast<int>(std::ceil(
        (Field.NorthM - CenterNorthM + RadiusM) / Field.NorthSpacingM)));
    bool Changed = false;
    for (int Row = MinRow; Row <= MaxRow; ++Row)
    {
        for (int Column = MinColumn; Column <= MaxColumn; ++Column)
        {
            const double EastDelta = Field.EastM(static_cast<std::uint32_t>(Column)) - CenterEastM;
            const double NorthDelta = Field.SampleNorthM(static_cast<std::uint32_t>(Row)) - CenterNorthM;
            const double Distance = std::sqrt(EastDelta * EastDelta + NorthDelta * NorthDelta);
            if (Distance > RadiusM)
            {
                continue;
            }
            float& Sample = Field.Samples[static_cast<std::size_t>(Row) * Field.Width + Column];
            if (!ValidSample(Field, Sample))
            {
                continue;
            }
            const double Weight = 0.5 + 0.5 * std::cos(3.14159265358979323846 * Distance / RadiusM);
            Sample = static_cast<float>(static_cast<double>(Sample) + DeltaM * Weight);
            Changed = true;
        }
    }
    if (!Changed)
    {
        return false;
    }
    Field.CurrentRevision = NextRevision;
    OutBounds = {static_cast<std::uint32_t>(MinRow), static_cast<std::uint32_t>(MaxRow),
        static_cast<std::uint32_t>(MinColumn), static_cast<std::uint32_t>(MaxColumn)};
    return true;
}
