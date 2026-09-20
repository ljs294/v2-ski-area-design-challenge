#include "SkiPreparation/TerrainPreparation.h"

#include <cmath>

namespace
{
double HaversineMeters(const double LatitudeA, const double LongitudeA,
    const double LatitudeB, const double LongitudeB)
{
    constexpr double RadiusM = 6371008.8;
    constexpr double Radians = 3.14159265358979323846 / 180.0;
    const double DLatitude = (LatitudeB - LatitudeA) * Radians;
    const double DLongitude = (LongitudeB - LongitudeA) * Radians;
    const double A = std::sin(DLatitude / 2.0) * std::sin(DLatitude / 2.0)
        + std::cos(LatitudeA * Radians) * std::cos(LatitudeB * Radians)
        * std::sin(DLongitude / 2.0) * std::sin(DLongitude / 2.0);
    return 2.0 * RadiusM * std::asin(std::min(1.0, std::sqrt(A)));
}
}

bool SkiPreparation::ValidateRequest(const Request& RequestValue, FString& OutError)
{
    const auto& Bounds = RequestValue.Bounds;
    if (RequestValue.Name.TrimStartAndEnd().IsEmpty() || RequestValue.Name.Len() > 128)
    {
        OutError = TEXT("Terrain name must contain 1-128 characters.");
        return false;
    }
    if (!std::isfinite(Bounds.WestDeg) || !std::isfinite(Bounds.EastDeg)
        || !std::isfinite(Bounds.SouthDeg) || !std::isfinite(Bounds.NorthDeg)
        || Bounds.WestDeg < -180.0 || Bounds.EastDeg > 180.0
        || Bounds.SouthDeg < -90.0 || Bounds.NorthDeg > 90.0
        || Bounds.WestDeg >= Bounds.EastDeg || Bounds.SouthDeg >= Bounds.NorthDeg)
    {
        OutError = TEXT("Terrain bounds are invalid.");
        return false;
    }
    const double CenterLatitude = (Bounds.SouthDeg + Bounds.NorthDeg) / 2.0;
    const double CenterLongitude = (Bounds.WestDeg + Bounds.EastDeg) / 2.0;
    const double WidthM = HaversineMeters(CenterLatitude, Bounds.WestDeg,
        CenterLatitude, Bounds.EastDeg);
    const double HeightM = HaversineMeters(Bounds.SouthDeg, CenterLongitude,
        Bounds.NorthDeg, CenterLongitude);
    if (WidthM < 1990.0 || HeightM < 1990.0 || WidthM > 10010.0 || HeightM > 10010.0)
    {
        OutError = FString::Printf(TEXT("Terrain sides must be 2-10 km (received %.0f x %.0f m)."), WidthM, HeightM);
        return false;
    }
    if (RequestValue.SessionGeneration == 0 || RequestValue.OperationGeneration == 0)
    {
        OutError = TEXT("Session and operation generations must be nonzero.");
        return false;
    }
    return true;
}

const TCHAR* SkiPreparation::StateName(const State Value) noexcept
{
    switch (Value)
    {
    case State::Selected: return TEXT("Selected");
    case State::Validating: return TEXT("Validating");
    case State::Acquiring: return TEXT("Acquiring");
    case State::Decoding: return TEXT("Decoding");
    case State::Deriving: return TEXT("Deriving");
    case State::WritingStaging: return TEXT("WritingStaging");
    case State::Verifying: return TEXT("Verifying");
    case State::Activating: return TEXT("Activating");
    case State::Installed: return TEXT("Installed");
    case State::Failed: return TEXT("Failed");
    case State::Cancelled: return TEXT("Cancelled");
    }
    return TEXT("Unknown");
}
