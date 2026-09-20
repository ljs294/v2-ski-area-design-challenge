#pragma once

#include "SkiDomain/Export.h"

#include <cstdint>

namespace SkiDomain
{
using Revision = std::uint64_t;

// A small pure boundary probe, not a document store or a simulation host.
// Failure never modifies Next; revision values never wrap.
SKI_DOMAIN_API bool TryAdvanceRevision(Revision Current, Revision Expected, Revision& Next) noexcept;
}
