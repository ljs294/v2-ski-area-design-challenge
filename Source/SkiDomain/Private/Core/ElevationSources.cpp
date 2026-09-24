#include "SkiDomain/ElevationSources.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace
{
bool ValidDate(const std::string& Value) noexcept
{
    if (Value.empty()) return true;
    if (Value.size() != 10 || Value[4] != '-' || Value[7] != '-') return false;
    for (std::size_t Index = 0; Index < Value.size(); ++Index)
        if (Index != 4 && Index != 7 && (Value[Index] < '0' || Value[Index] > '9'))
            return false;
    const int Year = (Value[0] - '0') * 1000 + (Value[1] - '0') * 100
        + (Value[2] - '0') * 10 + Value[3] - '0';
    const int Month = (Value[5] - '0') * 10 + Value[6] - '0';
    const int Day = (Value[8] - '0') * 10 + Value[9] - '0';
    if (Year < 1 || Month < 1 || Month > 12 || Day < 1) return false;
    constexpr int DaysPerMonth[] = {0, 31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31};
    const bool Leap = Year % 4 == 0 && (Year % 100 != 0 || Year % 400 == 0);
    return Day <= DaysPerMonth[Month] + (Month == 2 && Leap ? 1 : 0);
}

std::uint8_t Rank(const SkiDomain::ElevationProduct Product) noexcept
{
    switch (Product)
    {
    case SkiDomain::ElevationProduct::S1M: return 0;
    case SkiDomain::ElevationProduct::Project1m: return 1;
    case SkiDomain::ElevationProduct::ArcSec13: return 2;
    }
    return std::numeric_limits<std::uint8_t>::max();
}
}

bool SkiDomain::ResolveElevationSources(
    const std::vector<ElevationSourceCandidate>& Candidates,
    std::vector<ElevationSourceCandidate>& OutSources)
{
    OutSources.clear();
    std::unordered_set<std::string> SeenIds;
    for (const ElevationSourceCandidate& Candidate : Candidates)
    {
        if (Candidate.SourceId.empty() || !ValidDate(Candidate.CollectionEndDate)
            || !ValidDate(Candidate.PublicationDate)
            || Rank(Candidate.Product) == std::numeric_limits<std::uint8_t>::max()
            || !SeenIds.insert(Candidate.SourceId).second)
        {
            OutSources.clear();
            return false;
        }
        if (!Candidate.SupportedHorizontalCrs || !Candidate.Navd88Proven
            || !Candidate.SupportedEncoding) continue;
        OutSources.push_back(Candidate);
    }
    std::sort(OutSources.begin(), OutSources.end(),
        [](const ElevationSourceCandidate& A, const ElevationSourceCandidate& B)
    {
        if (Rank(A.Product) != Rank(B.Product)) return Rank(A.Product) < Rank(B.Product);
        const std::uint8_t AQuality = A.QualityLevel == 0 ? 255 : A.QualityLevel;
        const std::uint8_t BQuality = B.QualityLevel == 0 ? 255 : B.QualityLevel;
        if (AQuality != BQuality) return AQuality < BQuality;
        if (A.CollectionEndDate != B.CollectionEndDate)
            return A.CollectionEndDate > B.CollectionEndDate;
        if (A.PublicationDate != B.PublicationDate)
            return A.PublicationDate > B.PublicationDate;
        return A.SourceId < B.SourceId;
    });
    return !OutSources.empty();
}
