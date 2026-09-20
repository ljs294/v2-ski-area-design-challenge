#include "SkiDomain/Revision.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    const std::string_view Mode = argc > 1 ? argv[1] : "";
    if (Mode == "--crash")
    {
#if defined(_MSC_VER)
        // The deliberate failure must terminate unattended, without a CRT dialog
        // or a Windows error-report upload. This setting affects this test only.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        std::abort();
    }
    if (Mode == "--hang") { std::this_thread::sleep_for(std::chrono::hours(1)); }
    if (Mode == "--empty")
    {
        std::cout << "SKI_TEST_RESULT {\"total\":0,\"passed\":0,\"failed\":0}\n";
        return 0;
    }
    int Total = 0;
    int Failed = 0;
    auto Check = [&](bool Condition, const char* Name)
    {
        ++Total;
        if (!Condition) { ++Failed; std::cerr << "FAIL " << Name << '\n'; }
    };
    SkiDomain::Revision Next = 91;
    Check(SkiDomain::TryAdvanceRevision(4, 4, Next) && Next == 5, "matching revision advances once");
    Next = 91;
    Check(!SkiDomain::TryAdvanceRevision(5, 4, Next) && Next == 91, "stale revision leaves output unchanged");
    const auto Last = std::numeric_limits<SkiDomain::Revision>::max();
    Check(!SkiDomain::TryAdvanceRevision(Last, Last, Next) && Next == 91, "revision cannot wrap");
    if (Mode == "--fail") { Check(false, "deliberate harness failure"); }
    std::cout << "SKI_TEST_RESULT {\"total\":" << Total << ",\"passed\":" << Total - Failed << ",\"failed\":" << Failed << "}\n";
    return Failed ? 1 : 0;
}
