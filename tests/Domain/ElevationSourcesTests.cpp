#include "SkiDomain/ElevationSources.h"

#include <iostream>
#include <vector>

int main()
{
    int Total = 0;
    int Failed = 0;
    const auto Check = [&](const bool Condition, const char* Name)
    {
        ++Total;
        if (!Condition) { ++Failed; std::cerr << "FAIL " << Name << '\n'; }
    };
    using SkiDomain::ElevationProduct;
    using SkiDomain::ElevationSourceCandidate;
    const ElevationSourceCandidate ProjectOld{ElevationProduct::Project1m,
        "project-old", true, true, true, 2, "2019-01-01", "2020-01-01"};
    const ElevationSourceCandidate ProjectNew{ElevationProduct::Project1m,
        "project-new", true, true, true, 2, "2022-01-01", "2023-01-01"};
    const ElevationSourceCandidate ProjectNoDatum{ElevationProduct::Project1m,
        "crystal-unproven", true, false, true, 1, "2025-01-01", "2026-01-01"};
    const ElevationSourceCandidate S1M{ElevationProduct::S1M,
        "s1m-tile", true, true, true, 3, "2018-01-01", "2020-01-01"};
    const ElevationSourceCandidate Coarse{ElevationProduct::ArcSec13,
        "arcsec13", true, true, true, 0, "", "2026-01-01"};
    std::vector<ElevationSourceCandidate> Resolved;
    Check(SkiDomain::ResolveElevationSources(
            {Coarse, ProjectOld, ProjectNoDatum, S1M, ProjectNew}, Resolved)
        && Resolved.size() == 4 && Resolved[0].SourceId == "s1m-tile"
        && Resolved[1].SourceId == "project-new"
        && Resolved[2].SourceId == "project-old"
        && Resolved[3].SourceId == "arcsec13",
        "S1M priority, project chronology and datum rejection are deterministic");
    Check(SkiDomain::ResolveElevationSources({ProjectNoDatum, Coarse}, Resolved)
        && Resolved.size() == 1 && Resolved[0].SourceId == "arcsec13",
        "unproven Crystal project falls to coarse tier");
    Check(!SkiDomain::ResolveElevationSources({ProjectNoDatum}, Resolved)
        && Resolved.empty(), "unproven project alone does not qualify");
    ElevationSourceCandidate BadEncoding = ProjectOld;
    BadEncoding.SourceId = "bad-encoding";
    BadEncoding.SupportedEncoding = false;
    Check(SkiDomain::ResolveElevationSources({BadEncoding, Coarse}, Resolved)
        && Resolved.size() == 1 && Resolved[0].SourceId == "arcsec13",
        "unsupported COG encoding is excluded");
    Check(!SkiDomain::ResolveElevationSources({ProjectOld, ProjectOld}, Resolved)
        && Resolved.empty(), "duplicate source IDs fail closed");
    ElevationSourceCandidate BadDate = ProjectOld;
    BadDate.CollectionEndDate = "2025/01/01";
    Check(!SkiDomain::ResolveElevationSources({BadDate}, Resolved)
        && Resolved.empty(), "unorderable metadata date is rejected");
    BadDate.CollectionEndDate = "2024-02-31";
    Check(!SkiDomain::ResolveElevationSources({BadDate}, Resolved)
        && Resolved.empty(), "impossible calendar date is rejected");
    BadDate.CollectionEndDate = "2024-02-29";
    Check(SkiDomain::ResolveElevationSources({BadDate}, Resolved)
        && Resolved.size() == 1, "leap-day date is accepted");
    ElevationSourceCandidate BadProduct = ProjectOld;
    BadProduct.Product = static_cast<ElevationProduct>(255);
    Check(!SkiDomain::ResolveElevationSources({BadProduct}, Resolved)
        && Resolved.empty(), "unknown source product is rejected");
    std::cout << "SKI_TEST_RESULT {\"total\":" << Total << ",\"passed\":"
        << Total - Failed << ",\"failed\":" << Failed << "}\n";
    return Failed ? 1 : 0;
}
