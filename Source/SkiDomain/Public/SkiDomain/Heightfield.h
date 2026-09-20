#pragma once

#include "SkiDomain/Revision.h"

#include <cstdint>
#include <vector>

namespace SkiDomain
{
struct EnuVector
{
    double East = 0.0;
    double North = 0.0;
    double Up = 0.0;
};

struct Heightfield
{
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    double WestM = 0.0;
    double NorthM = 0.0;
    double EastSpacingM = 0.0;
    double NorthSpacingM = 0.0;
    double NoDataValue = -9999.0;
    Revision CurrentRevision = 0;
    std::vector<float> Samples;

    double EastM(const std::uint32_t Column) const noexcept
    {
        return WestM + static_cast<double>(Column) * EastSpacingM;
    }
    double SampleNorthM(const std::uint32_t Row) const noexcept
    {
        return NorthM - static_cast<double>(Row) * NorthSpacingM;
    }
};

struct Ray
{
    EnuVector Origin;
    EnuVector Direction;
};

struct RayHit
{
    bool Hit = false;
    double Distance = 0.0;
    EnuVector Position;
    std::uint32_t Row = 0;
    std::uint32_t Column = 0;
    Revision SourceRevision = 0;
};

struct MutationBounds
{
    std::uint32_t MinRow = 0;
    std::uint32_t MaxRow = 0;
    std::uint32_t MinColumn = 0;
    std::uint32_t MaxColumn = 0;
};

struct TerrainReadiness
{
    Revision Canonical = 0;
    Revision Render = 0;
    Revision Query = 0;
    Revision Collision = 0;
    bool CollisionRequired = false;

    bool IsReady() const noexcept
    {
        return Canonical != 0 && Render == Canonical && Query == Canonical
            && (!CollisionRequired || Collision == Canonical);
    }
};

SKI_DOMAIN_API bool IsValidHeightfield(const Heightfield& Field) noexcept;
SKI_DOMAIN_API RayHit QueryHeightfield(const Heightfield& Field, const Ray& Query) noexcept;
SKI_DOMAIN_API bool ApplyCircularHeightDelta(Heightfield& Field, Revision ExpectedRevision,
    double CenterEastM, double CenterNorthM, double RadiusM, double DeltaM,
    MutationBounds& OutBounds) noexcept;
}
