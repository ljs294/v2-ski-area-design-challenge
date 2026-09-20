#include "SkiDomain/Revision.h"

#include <limits>

bool SkiDomain::TryAdvanceRevision(Revision Current, Revision Expected, Revision& Next) noexcept
{
    if (Current != Expected || Current == std::numeric_limits<Revision>::max())
    {
        return false;
    }
    Next = Current + 1;
    return true;
}
