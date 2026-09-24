#include "SkiPreparation/SkiProjectionFallback.h"

#include <cmath>
#include <algorithm>

namespace SkiPreparation::ProjectionFallback
{
namespace
{
constexpr double Pi = 3.141592653589793238462643383279502884;
double Rad(double Deg) { return Deg * Pi / 180.0; }
double Deg(double Radians) { return Radians * 180.0 / Pi; }
bool Valid(const Ellipsoid& E)
{
    return std::isfinite(E.SemiMajorM) && E.SemiMajorM > 0.0
        && std::isfinite(E.InverseFlattening) && E.InverseFlattening > 250.0;
}
bool Valid(Geodetic P)
{
    return std::isfinite(P.LatitudeDeg) && std::isfinite(P.LongitudeDeg)
        && std::abs(P.LatitudeDeg) < 90.0 && std::abs(P.LongitudeDeg) <= 180.0;
}
double E2(Ellipsoid E)
{
    const double F = 1.0 / E.InverseFlattening;
    return F * (2.0 - F);
}
struct TMConstants
{
    double B, E, MO;
    double H[4], ReverseH[4];
};
TMConstants Constants(const TransverseMercator& CRS)
{
    const double F = 1.0 / CRS.Surface.InverseFlattening;
    const double N = F / (2.0 - F), N2 = N*N, N3 = N2*N, N4 = N2*N2;
    TMConstants C{};
    C.B = CRS.Surface.SemiMajorM / (1.0 + N) * (1.0 + N2/4.0 + N4/64.0);
    C.E = std::sqrt(E2(CRS.Surface));
    C.H[0] = N/2.0 - 2.0*N2/3.0 + 5.0*N3/16.0 + 41.0*N4/180.0;
    C.H[1] = 13.0*N2/48.0 - 3.0*N3/5.0 + 557.0*N4/1440.0;
    C.H[2] = 61.0*N3/240.0 - 103.0*N4/140.0;
    C.H[3] = 49561.0*N4/161280.0;
    C.ReverseH[0] = N/2.0 - 2.0*N2/3.0 + 37.0*N3/96.0 - N4/360.0;
    C.ReverseH[1] = N2/48.0 + N3/15.0 - 437.0*N4/1440.0;
    C.ReverseH[2] = 17.0*N3/480.0 - 37.0*N4/840.0;
    C.ReverseH[3] = 4397.0*N4/161280.0;
    const double Phi = Rad(CRS.OriginLatitudeDeg);
    const double Q = std::asinh(std::tan(Phi)) - C.E * std::atanh(C.E * std::sin(Phi));
    double Xi = std::atan(std::sinh(Q));
    const double Xi0 = Xi;
    for (int K=1; K<=4; ++K) Xi += C.H[K-1] * std::sin(2.0*K*Xi0);
    C.MO = C.B * Xi;
    return C;
}
double AuthalicQ(double Phi, double Eccentricity)
{
    const double E2Value = Eccentricity * Eccentricity;
    const double S = std::sin(Phi);
    return (1.0 - E2Value) * (S/(1.0-E2Value*S*S)
        + std::atanh(Eccentricity*S)/Eccentricity);
}
double AlbersM(double Phi, double E2Value)
{
    return std::cos(Phi)/std::sqrt(1.0-E2Value*std::sin(Phi)*std::sin(Phi));
}
bool AlbersConstants(const AlbersEqualArea& C, double& N, double& Factor,
    double& OriginRho)
{
    if (!Valid(C.Surface)) return false;
    const double E2Value = E2(C.Surface), E = std::sqrt(E2Value);
    const double P1 = Rad(C.FirstStandardParallelDeg), P2 = Rad(C.SecondStandardParallelDeg);
    const double Q1 = AuthalicQ(P1,E), Q2 = AuthalicQ(P2,E);
    if (std::abs(Q2-Q1) < 1e-12) return false;
    N = (std::pow(AlbersM(P1,E2Value),2)-std::pow(AlbersM(P2,E2Value),2))/(Q2-Q1);
    Factor = std::pow(AlbersM(P1,E2Value),2) + N*Q1;
    const double Term = Factor - N*AuthalicQ(Rad(C.OriginLatitudeDeg),E);
    if (std::abs(N)<1e-12 || Term<0.0) return false;
    OriginRho = C.Surface.SemiMajorM*std::sqrt(Term)/N;
    return true;
}
}

TransverseMercator Nad83UtmNorth(int32 Zone)
{
    TransverseMercator C;
    if (Zone != 10) { C.Scale = 0.0; return C; }
    C.CentralMeridianDeg = Zone*6.0 - 183.0;
    return C;
}

TransverseMercator Epsg26910() { return Nad83UtmNorth(10); }

AlbersEqualArea Epsg6350()
{
    AlbersEqualArea C;
    C.OriginLatitudeDeg = 23.0;
    C.CentralMeridianDeg = -96.0;
    C.FirstStandardParallelDeg = 29.5;
    C.SecondStandardParallelDeg = 45.5;
    return C;
}

bool ForwardTransverseMercator(const TransverseMercator& CRS, Geodetic Point, Projected& Out)
{
    Out = {};
    if (!Valid(CRS.Surface) || !Valid(Point) || !std::isfinite(CRS.Scale)
        || CRS.Scale<=0.0 || std::abs(Point.LongitudeDeg-CRS.CentralMeridianDeg)>4.0) return false;
    const TMConstants C = Constants(CRS);
    const double Phi=Rad(Point.LatitudeDeg), Lambda=Rad(Point.LongitudeDeg-CRS.CentralMeridianDeg);
    const double Q=std::asinh(std::tan(Phi))-C.E*std::atanh(C.E*std::sin(Phi));
    const double Beta=std::atan(std::sinh(Q));
    const double Eta0=std::atanh(std::cos(Beta)*std::sin(Lambda));
    const double Xi0=std::asin(std::sin(Beta)*std::cosh(Eta0));
    double Xi=Xi0,Eta=Eta0;
    for (int K=1;K<=4;++K)
    {
        Xi+=C.H[K-1]*std::sin(2.0*K*Xi0)*std::cosh(2.0*K*Eta0);
        Eta+=C.H[K-1]*std::cos(2.0*K*Xi0)*std::sinh(2.0*K*Eta0);
    }
    Out={CRS.FalseEastingM+CRS.Scale*C.B*Eta,
        CRS.FalseNorthingM+CRS.Scale*(C.B*Xi-C.MO)};
    return std::isfinite(Out.EastingM)&&std::isfinite(Out.NorthingM);
}

bool InverseTransverseMercator(const TransverseMercator& CRS, Projected Point, Geodetic& Out)
{
    Out={};
    if (!Valid(CRS.Surface)||!std::isfinite(Point.EastingM)||!std::isfinite(Point.NorthingM)
        || CRS.Scale<=0.0) return false;
    const TMConstants C=Constants(CRS);
    const double Eta= (Point.EastingM-CRS.FalseEastingM)/(C.B*CRS.Scale);
    const double Xi=((Point.NorthingM-CRS.FalseNorthingM)/CRS.Scale+C.MO)/C.B;
    double Xi0=Xi,Eta0=Eta;
    for(int K=1;K<=4;++K)
    {
        Xi0-=C.ReverseH[K-1]*std::sin(2.0*K*Xi)*std::cosh(2.0*K*Eta);
        Eta0-=C.ReverseH[K-1]*std::cos(2.0*K*Xi)*std::sinh(2.0*K*Eta);
    }
    const double Beta=std::asin(std::sin(Xi0)/std::cosh(Eta0));
    double Q=std::asinh(std::tan(Beta));
    for(int I=0;I<12;++I)
    {
        const double Next=std::asinh(std::tan(Beta))+C.E*std::atanh(C.E*std::tanh(Q));
        if(std::abs(Next-Q)<1e-14){Q=Next;break;}
        Q=Next;
    }
    Out={Deg(std::atan(std::sinh(Q))),
        CRS.CentralMeridianDeg+Deg(std::asin(std::tanh(Eta0)/std::cos(Beta)))};
    return Valid(Out)&&std::abs(Out.LongitudeDeg-CRS.CentralMeridianDeg)<=4.0;
}

bool ForwardAlbers(const AlbersEqualArea& CRS, Geodetic Point, Projected& Out)
{
    Out={};
    if(!Valid(Point)) return false;
    double N,F,Rho0;
    if(!AlbersConstants(CRS,N,F,Rho0)) return false;
    const double Term=F-N*AuthalicQ(Rad(Point.LatitudeDeg),std::sqrt(E2(CRS.Surface)));
    if(Term<0.0) return false;
    const double Rho=CRS.Surface.SemiMajorM*std::sqrt(Term)/N;
    const double Theta=N*Rad(Point.LongitudeDeg-CRS.CentralMeridianDeg);
    Out={CRS.FalseEastingM+Rho*std::sin(Theta),
        CRS.FalseNorthingM+Rho0-Rho*std::cos(Theta)};
    return std::isfinite(Out.EastingM)&&std::isfinite(Out.NorthingM);
}

bool InverseAlbers(const AlbersEqualArea& CRS, Projected Point, Geodetic& Out)
{
    Out={};
    double N,F,Rho0;
    if(!AlbersConstants(CRS,N,F,Rho0)||!std::isfinite(Point.EastingM)
        ||!std::isfinite(Point.NorthingM)) return false;
    const double X=Point.EastingM-CRS.FalseEastingM;
    const double Y=Rho0-(Point.NorthingM-CRS.FalseNorthingM);
    const double Rho=std::copysign(std::hypot(X,Y),N);
    const double Theta=std::atan2(N >= 0.0 ? X : -X,
        N >= 0.0 ? Y : -Y);
    const double Q=(F-std::pow(Rho*N/CRS.Surface.SemiMajorM,2))/N;
    const double E=std::sqrt(E2(CRS.Surface));
    double Phi=std::asin(std::clamp(Q/AuthalicQ(Pi/2,E),-1.0,1.0));
    for(int I=0;I<15;++I)
    {
        const double Difference=AuthalicQ(Phi,E)-Q;
        const double Derivative=2.0*(1.0-E*E)*std::cos(Phi)
            /std::pow(1.0-E*E*std::sin(Phi)*std::sin(Phi),2);
        if(std::abs(Derivative)<1e-14) break;
        const double Next=Phi-Difference/Derivative;
        if(std::abs(Next-Phi)<1e-14){Phi=Next;break;}
        Phi=Next;
    }
    Out={Deg(Phi),CRS.CentralMeridianDeg+Deg(Theta/N)};
    return Valid(Out);
}

bool ApproximateWgs84AsNad83(Geodetic Wgs84, Geodetic& OutNad83,
    HorizontalErrorBudget& InOutBudget)
{
    if(!Valid(Wgs84)) return false;
    OutNad83=Wgs84;
    InOutBudget.DatumApproximationM=std::max(InOutBudget.DatumApproximationM,1.5);
    return true;
}
}
