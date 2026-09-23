#pragma once

#include "CoreMinimal.h"

// Pure camera-fit math shared by the terrain view controller, auto-LOD estimates and the
// packaged layout harness. The camera projection is pinned (see PinnedCameraProjection) so the
// fit never depends on an assumed field-of-view axis.
namespace SkiTerrainViewMath
{
// Unreal's default AspectRatio_MaintainYFOV holds the vertical angle derived from the
// camera's horizontal FOV at the camera's own 16:9 aspect ratio.
constexpr double CameraHorizontalFovDegrees = 50.0;
constexpr double CameraAspectRatio = 16.0 / 9.0;
constexpr double FitMarginFraction = 0.08;

inline double VerticalHalfTangent()
{
    return FMath::Tan(FMath::DegreesToRadians(CameraHorizontalFovDegrees * 0.5)) / CameraAspectRatio;
}

inline double HorizontalHalfTangent(const double ViewportWidth, const double ViewportHeight)
{
    return VerticalHalfTangent() * ViewportWidth / FMath::Max(1.0, ViewportHeight);
}

/** Normalized-device target rectangle, x right and y up, both in [-1, 1]. */
struct FNdcRect
{
    double MinX = -1.0, MaxX = 1.0, MinY = -1.0, MaxY = 1.0;
};

/** The unobstructed pixel rect [0, width - rightInset] x [0, height], shrunk by the margin. */
inline FNdcRect UnobstructedTarget(const double ViewportWidth, const double ViewportHeight,
    const double RightInsetPixels, const double Margin = FitMarginFraction)
{
    const double Available = FMath::Clamp(ViewportWidth - RightInsetPixels, 1.0, ViewportWidth);
    const double Left = Available * Margin, Right = Available * (1.0 - Margin);
    const double Top = ViewportHeight * Margin, Bottom = ViewportHeight * (1.0 - Margin);
    FNdcRect Rect;
    Rect.MinX = 2.0 * Left / ViewportWidth - 1.0;
    Rect.MaxX = 2.0 * Right / ViewportWidth - 1.0;
    Rect.MinY = 1.0 - 2.0 * Bottom / ViewportHeight;
    Rect.MaxY = 1.0 - 2.0 * Top / ViewportHeight;
    return Rect;
}

struct FFit
{
    bool bValid = false;
    double Distance = 0.0;
    /** Camera offset along its right axis, as a fraction of Distance. */
    double ShiftRatio = 0.0;
};

/**
 * Smallest camera distance D (and a right-axis shift S) such that every point projects into
 * Target when the camera sits at Pivot - Forward * D + Right * S. Each projection constraint is
 * linear in S for a fixed D, so feasibility is a bisection on D.
 */
inline FFit FitPoints(const TArray<FVector>& Points, const FVector& Pivot, const FVector& Forward,
    const FVector& Right, const FVector& Up, const double TanX, const double TanY, const FNdcRect& Target)
{
    FFit Result;
    if (Points.IsEmpty() || TanX <= 0.0 || TanY <= 0.0 || Target.MinX >= Target.MaxX || Target.MinY >= Target.MaxY)
        return Result;
    const auto Feasible = [&](const double D, double& OutShift)
    {
        double Lower = -TNumericLimits<double>::Max(), Upper = TNumericLimits<double>::Max();
        for (const FVector& Point : Points)
        {
            const FVector Offset = Point - Pivot;
            const double A = FVector::DotProduct(Offset, Right);
            const double Depth = FVector::DotProduct(Offset, Forward) + D;
            const double C = FVector::DotProduct(Offset, Up);
            if (Depth <= 1.0) return false;
            if (C < Target.MinY * Depth * TanY || C > Target.MaxY * Depth * TanY) return false;
            Lower = FMath::Max(Lower, A - Target.MaxX * Depth * TanX);
            Upper = FMath::Min(Upper, A - Target.MinX * Depth * TanX);
        }
        if (Lower > Upper) return false;
        OutShift = 0.5 * (Lower + Upper);
        return true;
    };
    double Extent = 1.0;
    for (const FVector& Point : Points) Extent = FMath::Max(Extent, (Point - Pivot).Size());
    double Low = 0.0, High = Extent * 4.0, Shift = 0.0;
    while (!Feasible(High, Shift))
    {
        High *= 2.0;
        if (High > Extent * 1.0e6) return Result;
    }
    for (int32 Iteration = 0; Iteration < 60; ++Iteration)
    {
        const double Mid = 0.5 * (Low + High);
        double Candidate = 0.0;
        if (Feasible(Mid, Candidate)) High = Mid; else Low = Mid;
    }
    Feasible(High, Shift);
    Result.bValid = true;
    Result.Distance = High;
    Result.ShiftRatio = Shift / High;
    return Result;
}

inline TArray<FVector> BoxCorners(const FBox& Box)
{
    TArray<FVector> Corners;
    for (int32 Index = 0; Index < 8; ++Index)
        Corners.Add(FVector(Index & 1 ? Box.Max.X : Box.Min.X, Index & 2 ? Box.Max.Y : Box.Min.Y,
            Index & 4 ? Box.Max.Z : Box.Min.Z));
    return Corners;
}
}
