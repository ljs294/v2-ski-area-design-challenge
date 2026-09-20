#include "SkiApplication/Bootstrap.h"
#include "SkiDomain/Revision.h"

bool SkiApplication::CheckDomainBoundary()
{
    SkiDomain::Revision Next = 0;
    return SkiDomain::TryAdvanceRevision(0, 0, Next) && Next == 1;
}
