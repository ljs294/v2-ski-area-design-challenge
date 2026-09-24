#include "SkiPreparation/SkiSiteSelection.h"

#include <cmath>

bool SkiPreparation::ValidateResortName(const FString& Candidate,
    FString& OutTrimmedName, FString& OutError)
{
    OutTrimmedName.Reset();
    OutError.Reset();
    FString Trimmed = Candidate.TrimStartAndEnd();
    if (Trimmed.IsEmpty() || Trimmed.Len() > 60)
    {
        OutError = TEXT("Resort name must contain 1 to 60 characters after trimming.");
        return false;
    }
    for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
    {
        const uint32 Code = static_cast<uint32>(Trimmed[Index]);
        if (Code <= 0x1fU || (Code >= 0x7fU && Code <= 0x9fU))
        {
            OutError = TEXT("Resort name cannot contain control characters.");
            return false;
        }
    }
    OutTrimmedName = MoveTemp(Trimmed);
    return true;
}

SkiPreparation::SiteSelectionDecision SkiPreparation::ValidateSiteSelection(
    const SiteRectangleM& Rectangle, const SiteCoverage Coverage,
    const double MaximumSideM)
{
    SiteSelectionDecision Decision;
    if (!std::isfinite(Rectangle.WestM) || !std::isfinite(Rectangle.SouthM)
        || !std::isfinite(Rectangle.EastM) || !std::isfinite(Rectangle.NorthM)
        || !std::isfinite(MaximumSideM) || MaximumSideM < 2000.0
        || MaximumSideM > 10000.0)
    {
        Decision.Reason = TEXT("Site rectangle has invalid coordinates or size ceiling.");
        return Decision;
    }
    Decision.WidthM = Rectangle.EastM - Rectangle.WestM;
    Decision.HeightM = Rectangle.NorthM - Rectangle.SouthM;
    if (!SkiDomain::IsValidSiteRectangle(Rectangle, MaximumSideM))
    {
        Decision.Reason = TEXT("Each site side must be between 2 km and the current size ceiling.");
        return Decision;
    }
    Decision.bValidGeometry = true;
    Decision.AreaSqKm = Decision.WidthM * Decision.HeightM / 1000000.0;
    if (Coverage != SiteCoverage::SupportedUsUsgs3Dep)
    {
        Decision.Reason = Coverage == SiteCoverage::Unknown
            ? TEXT("USGS 3DEP coverage has not been confirmed.")
            : TEXT("This area is outside supported U.S. USGS 3DEP coverage.");
        return Decision;
    }
    Decision.bMayDownload = true;
    return Decision;
}
