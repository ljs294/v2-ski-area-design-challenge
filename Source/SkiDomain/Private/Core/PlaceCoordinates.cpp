#include "SkiDomain/PlaceCoordinates.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
bool ParseNumbers(std::string_view Text, std::vector<double>& Out)
{
    std::string Buffer(Text);
    const char* Cursor = Buffer.c_str();
    while (*Cursor != '\0')
    {
        while (*Cursor == ' ' || *Cursor == '\t' || *Cursor == '\r'
            || *Cursor == '\n' || *Cursor == '\'' || *Cursor == '"'
            || *Cursor == 'd' || *Cursor == 'm' || *Cursor == 's'
            || static_cast<unsigned char>(*Cursor) == 0xC2
            || static_cast<unsigned char>(*Cursor) == 0xB0) ++Cursor;
        if (*Cursor == '\0') break;
        char* End = nullptr;
        const double Value = std::strtod(Cursor, &End);
        if (End == Cursor || !std::isfinite(Value)) return false;
        Out.push_back(Value);
        if (Out.size() > 3) return false;
        Cursor = End;
    }
    return !Out.empty();
}

bool ParseDms(std::string_view Text, const char Hemisphere, double& Out)
{
    std::vector<double> Parts;
    if (!ParseNumbers(Text, Parts)) return false;
    if (Parts[0] < 0.0 || (Parts.size() > 1 && (Parts[1] < 0.0 || Parts[1] >= 60.0))
        || (Parts.size() > 2 && (Parts[2] < 0.0 || Parts[2] >= 60.0))) return false;
    const double Magnitude = Parts[0] + (Parts.size() > 1 ? Parts[1] / 60.0 : 0.0)
        + (Parts.size() > 2 ? Parts[2] / 3600.0 : 0.0);
    Out = (Hemisphere == 'S' || Hemisphere == 'W') ? -Magnitude : Magnitude;
    return true;
}

bool ParseDecimal(std::string_view Text, double& Out)
{
    std::string Buffer(Text);
    const char* Begin = Buffer.c_str();
    while (*Begin == ' ' || *Begin == '\t') ++Begin;
    char* End = nullptr;
    const double Value = std::strtod(Begin, &End);
    if (End == Begin || !std::isfinite(Value)) return false;
    while (*End == ' ' || *End == '\t') ++End;
    if (*End != '\0') return false;
    Out = Value;
    return true;
}
}

bool SkiDomain::TryParsePlaceCoordinates(const std::string_view Text,
    PlaceCoordinates& OutCoordinates) noexcept
{
    PlaceCoordinates Parsed;
    const auto Comma = Text.find(',');
    if (Comma == std::string_view::npos) return false;
    const auto First = Text.substr(0, Comma);
    const auto Second = Text.substr(Comma + 1);
    if (Second.find(',') != std::string_view::npos) return false;

    const auto Hemisphere = [](std::string_view Component) -> char
    {
        while (!Component.empty() && (Component.back() == ' ' || Component.back() == '\t'))
            Component.remove_suffix(1);
        if (Component.empty()) return '\0';
        const char Last = Component.back();
        return Last >= 'a' && Last <= 'z' ? static_cast<char>(Last - 'a' + 'A') : Last;
    };
    const char LatHemisphere = Hemisphere(First);
    const char LonHemisphere = Hemisphere(Second);
    const bool Dms = LatHemisphere == 'N' || LatHemisphere == 'S'
        || LonHemisphere == 'E' || LonHemisphere == 'W';
    if (Dms)
    {
        if ((LatHemisphere != 'N' && LatHemisphere != 'S')
            || (LonHemisphere != 'E' && LonHemisphere != 'W')) return false;
        const auto RemoveHemisphere = [](std::string_view Component)
        {
            while (!Component.empty() && (Component.back() == ' ' || Component.back() == '\t'))
                Component.remove_suffix(1);
            Component.remove_suffix(1);
            return Component;
        };
        if (!ParseDms(RemoveHemisphere(First), LatHemisphere, Parsed.LatitudeDeg)
            || !ParseDms(RemoveHemisphere(Second), LonHemisphere, Parsed.LongitudeDeg)) return false;
    }
    else if (!ParseDecimal(First, Parsed.LatitudeDeg)
        || !ParseDecimal(Second, Parsed.LongitudeDeg)) return false;

    if (Parsed.LatitudeDeg < -90.0 || Parsed.LatitudeDeg > 90.0
        || Parsed.LongitudeDeg < -180.0 || Parsed.LongitudeDeg > 180.0) return false;
    OutCoordinates = Parsed;
    return true;
}
