#pragma once

#include <cstdint>

// Keep DLL visibility independent of Unreal's platform headers.
#if defined(SKI_DOMAIN_SHARED) && defined(_WIN32)
#if defined(SKI_DOMAIN_EXPORTS)
#define SKI_DOMAIN_API __declspec(dllexport)
#else
#define SKI_DOMAIN_API __declspec(dllimport)
#endif
#else
#define SKI_DOMAIN_API
#endif

namespace SkiDomain
{
using Revision = std::uint64_t;

// A small pure boundary probe, not a document store or a simulation host.
// Failure never modifies Next; revision values never wrap.
SKI_DOMAIN_API bool TryAdvanceRevision(Revision Current, Revision Expected, Revision& Next) noexcept;
}
