// SPDX-License-Identifier: GPL-2.0-or-later
/*
  Terra Soar - A virtual globe

  Copyright (C) 2026 Peter Howkins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
#include "ts/orbits/sgp4.h"

#include <cmath>
#include <cstdint>

#include <QCoreApplication>

#include "ts/maths/angles.h"
#include "ts/orbits/earthframes.h"

// The names of variables, and the order of each calculation, follow the
// reference (Vallado et al. 2006, sgp4unit.cpp) so that the two can be
// read side by side and agree to the last bits; see
// docs/design/satellites.md.

namespace ts
{

namespace
{

constexpr double twoPi = 2.0 * pi;
constexpr double x2o3 = 2.0 / 3.0;
constexpr double temp4 = 1.5e-12;
/** The Earth's rotation, radians per minute. */
constexpr double rptim = 4.37526908801129966e-3;

/** The gravity constants (reference: getgravconst). */
struct Gravity {
  double tumin = 0.0;
  double mu = 0.0;
  double radiusKm = 0.0;
  double xke = 0.0;
  double j2 = 0.0;
  double j3 = 0.0;
  double j4 = 0.0;
  double j3oj2 = 0.0;
};

Gravity gravityOf(GravityModel model)
{
  Gravity g;
  g.j2 = 0.001082616;
  g.j3 = -0.00000253881;
  g.j4 = -0.00000165597;
  g.radiusKm = 6378.135;
  switch (model) {
  case GravityModel::Wgs72Old:
    g.mu = 398600.79964;
    g.xke = 0.0743669161;
    break;
  case GravityModel::Wgs72:
    g.mu = 398600.8;
    g.xke = 60.0 / std::sqrt(g.radiusKm * g.radiusKm * g.radiusKm / g.mu);
    break;
  case GravityModel::Wgs84:
    g.mu = 398600.5;
    g.radiusKm = 6378.137;
    g.xke = 60.0 / std::sqrt(g.radiusKm * g.radiusKm * g.radiusKm / g.mu);
    g.j2 = 0.00108262998905;
    g.j3 = -0.00000253215306;
    g.j4 = -0.00000161098761;
    break;
  }
  g.tumin = 1.0 / g.xke;
  g.j3oj2 = g.j3 / g.j2;
  return g;
}

/** Elements being perturbed (radians). */
struct Angles {
  double ecc = 0.0;
  double incl = 0.0;
  double node = 0.0;
  double argp = 0.0;
  double mo = 0.0;
};

/** The Sun's and Moon's long-period periodic terms (reference: dpper). */
struct LunarSolar {
  double e3 = 0.0;
  double ee2 = 0.0;
  double peo = 0.0;
  double pgho = 0.0;
  double pho = 0.0;
  double pinco = 0.0;
  double plo = 0.0;
  double se2 = 0.0;
  double se3 = 0.0;
  double sgh2 = 0.0;
  double sgh3 = 0.0;
  double sgh4 = 0.0;
  double sh2 = 0.0;
  double sh3 = 0.0;
  double si2 = 0.0;
  double si3 = 0.0;
  double sl2 = 0.0;
  double sl3 = 0.0;
  double sl4 = 0.0;
  double xgh2 = 0.0;
  double xgh3 = 0.0;
  double xgh4 = 0.0;
  double xh2 = 0.0;
  double xh3 = 0.0;
  double xi2 = 0.0;
  double xi3 = 0.0;
  double xl2 = 0.0;
  double xl3 = 0.0;
  double xl4 = 0.0;
  double zmol = 0.0;
  double zmos = 0.0;
};

/** Deep-space secular rates and resonance terms (reference: dsinit). */
struct Resonance {
  /** 0 none, 1 one-day (synchronous), 2 half-day resonance. */
  int32_t irez = 0;
  double d2201 = 0.0;
  double d2211 = 0.0;
  double d3210 = 0.0;
  double d3222 = 0.0;
  double d4410 = 0.0;
  double d4422 = 0.0;
  double d5220 = 0.0;
  double d5232 = 0.0;
  double d5421 = 0.0;
  double d5433 = 0.0;
  double dedt = 0.0;
  double didt = 0.0;
  double dmdt = 0.0;
  double dnodt = 0.0;
  double domdt = 0.0;
  double del1 = 0.0;
  double del2 = 0.0;
  double del3 = 0.0;
  double xfact = 0.0;
  double xlamo = 0.0;
};

/** Everything propagation needs, computed once (reference: elsetrec). */
struct ModelData {
  Gravity g;
  Sgp4Mode mode = Sgp4Mode::Improved;
  Sgp4Error epochError = Sgp4Error::None;
  /** False for elements SGP4 can't start from (no positions at all). */
  bool usable = false;
  /** Days from 1949-12-31T00:00 to the epoch. */
  double epoch = 0.0;
  double bstar = 0.0;
  double ecco = 0.0;
  double argpo = 0.0;
  double inclo = 0.0;
  double mo = 0.0;
  double noKozai = 0.0;
  double nodeo = 0.0;
  double noUnkozai = 0.0;
  bool isimp = false;
  bool deep = false;
  double aycof = 0.0;
  double con41 = 0.0;
  double cc1 = 0.0;
  double cc4 = 0.0;
  double cc5 = 0.0;
  double d2 = 0.0;
  double d3 = 0.0;
  double d4 = 0.0;
  double delmo = 0.0;
  double eta = 0.0;
  double argpdot = 0.0;
  double omgcof = 0.0;
  double sinmao = 0.0;
  double t2cof = 0.0;
  double t3cof = 0.0;
  double t4cof = 0.0;
  double t5cof = 0.0;
  double x1mth2 = 0.0;
  double x7thm1 = 0.0;
  double mdot = 0.0;
  double nodedot = 0.0;
  double xlcof = 0.0;
  double xmcof = 0.0;
  double nodecf = 0.0;
  double gsto = 0.0;
  LunarSolar ls;
  Resonance res;
};

/** What initl gives. */
struct Initl {
  double ainv = 0.0;
  double ao = 0.0;
  double con42 = 0.0;
  double cosio = 0.0;
  double cosio2 = 0.0;
  double eccsq = 0.0;
  double omeosq = 0.0;
  double posq = 0.0;
  double rp = 0.0;
  double rteosq = 0.0;
  double sinio = 0.0;
};

/** The mean elements as secular effects move them. */
struct Secular {
  double em = 0.0;
  double argpm = 0.0;
  double inclm = 0.0;
  double mm = 0.0;
  double nodem = 0.0;
  double nm = 0.0;
};

/** One body's (the Sun's or the Moon's) terms in dscom. */
struct ThirdBody {
  double zcosg = 0.0;
  double zsing = 0.0;
  double zcosi = 0.0;
  double zsini = 0.0;
  double zcosh = 0.0;
  double zsinh = 0.0;
  double cc = 0.0;
};

/** The s and z terms dscom computes for each body. */
struct BodyTerms {
  double s1 = 0.0;
  double s2 = 0.0;
  double s3 = 0.0;
  double s4 = 0.0;
  double s5 = 0.0;
  double s6 = 0.0;
  double s7 = 0.0;
  double z1 = 0.0;
  double z2 = 0.0;
  double z3 = 0.0;
  double z11 = 0.0;
  double z12 = 0.0;
  double z13 = 0.0;
  double z21 = 0.0;
  double z22 = 0.0;
  double z23 = 0.0;
  double z31 = 0.0;
  double z32 = 0.0;
  double z33 = 0.0;
};

/** What dscom gives besides the LunarSolar terms. */
struct Dscom {
  double snodm = 0.0;
  double cnodm = 0.0;
  double sinim = 0.0;
  double cosim = 0.0;
  double sinomm = 0.0;
  double cosomm = 0.0;
  double day = 0.0;
  double em = 0.0;
  double emsq = 0.0;
  double gam = 0.0;
  double rtemsq = 0.0;
  double nm = 0.0;
  /** The Sun's (ss and sz in the reference). */
  BodyTerms sun;
  /** The Moon's (s and z in the reference). */
  BodyTerms moon;
};

Initl initl(ModelData& m)
{
  const Gravity& g = m.g;
  Initl il;
  il.eccsq = m.ecco * m.ecco;
  il.omeosq = 1.0 - il.eccsq;
  il.rteosq = std::sqrt(il.omeosq);
  il.cosio = std::cos(m.inclo);
  il.cosio2 = il.cosio * il.cosio;
  const double ak = std::pow(g.xke / m.noKozai, x2o3);
  const double d1 =
      0.75 * g.j2 * (3.0 * il.cosio2 - 1.0) / (il.rteosq * il.omeosq);
  double del = d1 / (ak * ak);
  const double adel =
      ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
  del = d1 / (adel * adel);
  m.noUnkozai = m.noKozai / (1.0 + del);
  il.ao = std::pow(g.xke / m.noUnkozai, x2o3);
  il.sinio = std::sin(m.inclo);
  const double po = il.ao * il.omeosq;
  il.con42 = 1.0 - 5.0 * il.cosio2;
  m.con41 = -il.con42 - il.cosio2 - il.cosio2;
  il.ainv = 1.0 / il.ao;
  il.posq = po * po;
  il.rp = il.ao * (1.0 - m.ecco);
  if (m.mode == Sgp4Mode::Afspc) {
    const double ts70 = m.epoch - 7305.0;
    const double ds70 = std::floor(ts70 + 1.0e-8);
    const double tfrac = ts70 - ds70;
    const double c1 = 1.72027916940703639e-2;
    const double thgr70 = 1.7321343856509374;
    const double fk5r = 5.07551419432269442e-15;
    const double c1p2p = c1 + twoPi;
    m.gsto = std::fmod(thgr70 + c1 * ds70 + c1p2p * tfrac + ts70 * ts70 * fk5r,
                       twoPi);
    if (m.gsto < 0.0) {
      m.gsto += twoPi;
    }
  } else {
    m.gsto = greenwichSiderealRad(m.epoch + 2433281.5);
  }
  return il;
}

/** What the near-Earth initialisation leaves for the rest. */
struct NearEarth {
  double tsi = 0.0;
  double sfour = 0.0;
  double xpidot = 0.0;
};

NearEarth nearEarthInit(ModelData& m, const Initl& il)
{
  const Gravity& g = m.g;
  const double ss = 78.0 / g.radiusKm + 1.0;
  const double qzms2ttemp = (120.0 - 78.0) / g.radiusKm;
  const double qzms2t = qzms2ttemp * qzms2ttemp * qzms2ttemp * qzms2ttemp;
  NearEarth out;
  m.isimp = il.rp < (220.0 / g.radiusKm + 1.0);
  out.sfour = ss;
  double qzms24 = qzms2t;
  const double perige = (il.rp - 1.0) * g.radiusKm;
  if (perige < 156.0) {
    out.sfour = perige - 78.0;
    if (perige < 98.0) {
      out.sfour = 20.0;
    }
    const double qzms24temp = (120.0 - out.sfour) / g.radiusKm;
    qzms24 = qzms24temp * qzms24temp * qzms24temp * qzms24temp;
    out.sfour = out.sfour / g.radiusKm + 1.0;
  }
  const double pinvsq = 1.0 / il.posq;
  const double ao = il.ao;
  out.tsi = 1.0 / (ao - out.sfour);
  const double tsi = out.tsi;
  m.eta = ao * m.ecco * tsi;
  const double etasq = m.eta * m.eta;
  const double eeta = m.ecco * m.eta;
  const double psisq = std::fabs(1.0 - etasq);
  const double coef = qzms24 * std::pow(tsi, 4.0);
  const double coef1 = coef / std::pow(psisq, 3.5);
  const double cc2 = coef1 * m.noUnkozai *
                     (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                      0.375 * g.j2 * tsi / psisq * m.con41 *
                          (8.0 + 3.0 * etasq * (8.0 + etasq)));
  m.cc1 = m.bstar * cc2;
  double cc3 = 0.0;
  if (m.ecco > 1.0e-4) {
    cc3 = -2.0 * coef * tsi * g.j3oj2 * m.noUnkozai * il.sinio / m.ecco;
  }
  m.x1mth2 = 1.0 - il.cosio2;
  m.cc4 =
      2.0 * m.noUnkozai * coef1 * ao * il.omeosq *
      (m.eta * (2.0 + 0.5 * etasq) + m.ecco * (0.5 + 2.0 * etasq) -
       g.j2 * tsi / (ao * psisq) *
           (-3.0 * m.con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
            0.75 * m.x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) *
                std::cos(2.0 * m.argpo)));
  m.cc5 = 2.0 * coef1 * ao * il.omeosq *
          (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
  const double cosio2 = il.cosio2;
  const double cosio4 = cosio2 * cosio2;
  const double temp1 = 1.5 * g.j2 * pinvsq * m.noUnkozai;
  const double temp2 = 0.5 * temp1 * g.j2 * pinvsq;
  const double temp3 = -0.46875 * g.j4 * pinvsq * pinvsq * m.noUnkozai;
  m.mdot = m.noUnkozai + 0.5 * temp1 * il.rteosq * m.con41 +
           0.0625 * temp2 * il.rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
  m.argpdot = (-0.5 * temp1 * il.con42 +
               0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
               temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4));
  const double xhdot1 = -temp1 * il.cosio;
  m.nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) +
                        2.0 * temp3 * (3.0 - 7.0 * cosio2)) *
                           il.cosio;
  out.xpidot = m.argpdot + m.nodedot;
  m.omgcof = m.bstar * cc3 * std::cos(m.argpo);
  m.xmcof = 0.0;
  if (m.ecco > 1.0e-4) {
    m.xmcof = -x2o3 * coef * m.bstar / eeta;
  }
  m.nodecf = 3.5 * il.omeosq * xhdot1 * m.cc1;
  m.t2cof = 1.5 * m.cc1;
  // Divide by zero at 180° inclination (sgp4fix).
  const double divisor =
      std::fabs(il.cosio + 1.0) > 1.5e-12 ? 1.0 + il.cosio : temp4;
  m.xlcof = -0.25 * g.j3oj2 * il.sinio * (3.0 + 5.0 * il.cosio) / divisor;
  m.aycof = -0.5 * g.j3oj2 * il.sinio;
  const double delmotemp = 1.0 + m.eta * std::cos(m.mo);
  m.delmo = delmotemp * delmotemp * delmotemp;
  m.sinmao = std::sin(m.mo);
  m.x7thm1 = 7.0 * cosio2 - 1.0;
  return out;
}

/** The higher-order drag terms (reference: the end of sgp4init). */
void dragInit(ModelData& m, const Initl& il, const NearEarth& ne)
{
  const double ao = il.ao;
  const double cc1sq = m.cc1 * m.cc1;
  m.d2 = 4.0 * ao * ne.tsi * cc1sq;
  const double temp = m.d2 * ne.tsi * m.cc1 / 3.0;
  m.d3 = (17.0 * ao + ne.sfour) * temp;
  m.d4 = 0.5 * temp * ao * ne.tsi * (221.0 * ao + 31.0 * ne.sfour) * m.cc1;
  m.t3cof = m.d2 + 2.0 * cc1sq;
  m.t4cof = 0.25 * (3.0 * m.d3 + m.cc1 * (12.0 * m.d2 + 10.0 * cc1sq));
  m.t5cof = 0.2 * (3.0 * m.d4 + 12.0 * m.cc1 * m.d3 + 6.0 * m.d2 * m.d2 +
                   15.0 * cc1sq * (2.0 * m.d2 + cc1sq));
}

BodyTerms bodyTerms(const ThirdBody& b, const Dscom& d, double betasq)
{
  const double xnoi = 1.0 / d.nm;
  const double a1 = b.zcosg * b.zcosh + b.zsing * b.zcosi * b.zsinh;
  const double a3 = -b.zsing * b.zcosh + b.zcosg * b.zcosi * b.zsinh;
  const double a7 = -b.zcosg * b.zsinh + b.zsing * b.zcosi * b.zcosh;
  const double a8 = b.zsing * b.zsini;
  const double a9 = b.zsing * b.zsinh + b.zcosg * b.zcosi * b.zcosh;
  const double a10 = b.zcosg * b.zsini;
  const double a2 = d.cosim * a7 + d.sinim * a8;
  const double a4 = d.cosim * a9 + d.sinim * a10;
  const double a5 = -d.sinim * a7 + d.cosim * a8;
  const double a6 = -d.sinim * a9 + d.cosim * a10;

  const double x1 = a1 * d.cosomm + a2 * d.sinomm;
  const double x2 = a3 * d.cosomm + a4 * d.sinomm;
  const double x3 = -a1 * d.sinomm + a2 * d.cosomm;
  const double x4 = -a3 * d.sinomm + a4 * d.cosomm;
  const double x5 = a5 * d.sinomm;
  const double x6 = a6 * d.sinomm;
  const double x7 = a5 * d.cosomm;
  const double x8 = a6 * d.cosomm;

  const double emsq = d.emsq;
  BodyTerms t;
  t.z31 = 12.0 * x1 * x1 - 3.0 * x3 * x3;
  t.z32 = 24.0 * x1 * x2 - 6.0 * x3 * x4;
  t.z33 = 12.0 * x2 * x2 - 3.0 * x4 * x4;
  t.z1 = 3.0 * (a1 * a1 + a2 * a2) + t.z31 * emsq;
  t.z2 = 6.0 * (a1 * a3 + a2 * a4) + t.z32 * emsq;
  t.z3 = 3.0 * (a3 * a3 + a4 * a4) + t.z33 * emsq;
  t.z11 = -6.0 * a1 * a5 + emsq * (-24.0 * x1 * x7 - 6.0 * x3 * x5);
  t.z12 = -6.0 * (a1 * a6 + a3 * a5) +
          emsq * (-24.0 * (x2 * x7 + x1 * x8) - 6.0 * (x3 * x6 + x4 * x5));
  t.z13 = -6.0 * a3 * a6 + emsq * (-24.0 * x2 * x8 - 6.0 * x4 * x6);
  t.z21 = 6.0 * a2 * a5 + emsq * (24.0 * x1 * x5 - 6.0 * x3 * x7);
  t.z22 = 6.0 * (a4 * a5 + a2 * a6) +
          emsq * (24.0 * (x2 * x5 + x1 * x6) - 6.0 * (x4 * x7 + x3 * x8));
  t.z23 = 6.0 * a4 * a6 + emsq * (24.0 * x2 * x6 - 6.0 * x4 * x8);
  t.z1 = t.z1 + t.z1 + betasq * t.z31;
  t.z2 = t.z2 + t.z2 + betasq * t.z32;
  t.z3 = t.z3 + t.z3 + betasq * t.z33;
  t.s3 = b.cc * xnoi;
  t.s2 = -0.5 * t.s3 / d.rtemsq;
  t.s4 = t.s3 * d.rtemsq;
  t.s1 = -15.0 * d.em * t.s4;
  t.s5 = x1 * x3 + x2 * x4;
  t.s6 = x2 * x3 + x1 * x4;
  t.s7 = x2 * x4 - x1 * x3;
  return t;
}

/** Reference: dscom (with tc = 0, at the epoch). */
Dscom dscom(const ModelData& m, LunarSolar& ls)
{
  constexpr double zes = 0.01675;
  constexpr double zel = 0.05490;
  constexpr double c1ss = 2.9864797e-6;
  constexpr double c1l = 4.7968065e-7;
  constexpr double zsinis = 0.39785416;
  constexpr double zcosis = 0.91744867;
  constexpr double zcosgs = 0.1945905;
  constexpr double zsings = -0.98088458;

  Dscom d;
  d.nm = m.noUnkozai;
  d.em = m.ecco;
  d.snodm = std::sin(m.nodeo);
  d.cnodm = std::cos(m.nodeo);
  d.sinomm = std::sin(m.argpo);
  d.cosomm = std::cos(m.argpo);
  d.sinim = std::sin(m.inclo);
  d.cosim = std::cos(m.inclo);
  d.emsq = d.em * d.em;
  const double betasq = 1.0 - d.emsq;
  d.rtemsq = std::sqrt(betasq);

  // The Moon's orbit.
  d.day = m.epoch + 18261.5;
  const double xnodce = std::fmod(4.5236020 - 9.2422029e-4 * d.day, twoPi);
  const double stem = std::sin(xnodce);
  const double ctem = std::cos(xnodce);
  const double zcosil = 0.91375164 - 0.03568096 * ctem;
  const double zsinil = std::sqrt(1.0 - zcosil * zcosil);
  const double zsinhl = 0.089683511 * stem / zsinil;
  const double zcoshl = std::sqrt(1.0 - zsinhl * zsinhl);
  d.gam = 5.8351514 + 0.0019443680 * d.day;
  double zx = 0.39785416 * stem / zsinil;
  const double zy = zcoshl * ctem + 0.91744867 * zsinhl * stem;
  zx = std::atan2(zx, zy);
  zx = d.gam + zx - xnodce;

  const ThirdBody sun{zcosgs, zsings, zcosis, zsinis, d.cnodm, d.snodm, c1ss};
  d.sun = bodyTerms(sun, d, betasq);
  const ThirdBody moon{std::cos(zx),
                       std::sin(zx),
                       zcosil,
                       zsinil,
                       zcoshl * d.cnodm + zsinhl * d.snodm,
                       d.snodm * zcoshl - d.cnodm * zsinhl,
                       c1l};
  d.moon = bodyTerms(moon, d, betasq);

  ls.zmol = std::fmod(4.7199672 + 0.22997150 * d.day - d.gam, twoPi);
  ls.zmos = std::fmod(6.2565837 + 0.017201977 * d.day, twoPi);

  const BodyTerms& s = d.sun;
  ls.se2 = 2.0 * s.s1 * s.s6;
  ls.se3 = 2.0 * s.s1 * s.s7;
  ls.si2 = 2.0 * s.s2 * s.z12;
  ls.si3 = 2.0 * s.s2 * (s.z13 - s.z11);
  ls.sl2 = -2.0 * s.s3 * s.z2;
  ls.sl3 = -2.0 * s.s3 * (s.z3 - s.z1);
  ls.sl4 = -2.0 * s.s3 * (-21.0 - 9.0 * d.emsq) * zes;
  ls.sgh2 = 2.0 * s.s4 * s.z32;
  ls.sgh3 = 2.0 * s.s4 * (s.z33 - s.z31);
  ls.sgh4 = -18.0 * s.s4 * zes;
  ls.sh2 = -2.0 * s.s2 * s.z22;
  ls.sh3 = -2.0 * s.s2 * (s.z23 - s.z21);

  const BodyTerms& l = d.moon;
  ls.ee2 = 2.0 * l.s1 * l.s6;
  ls.e3 = 2.0 * l.s1 * l.s7;
  ls.xi2 = 2.0 * l.s2 * l.z12;
  ls.xi3 = 2.0 * l.s2 * (l.z13 - l.z11);
  ls.xl2 = -2.0 * l.s3 * l.z2;
  ls.xl3 = -2.0 * l.s3 * (l.z3 - l.z1);
  ls.xl4 = -2.0 * l.s3 * (-21.0 - 9.0 * d.emsq) * zel;
  ls.xgh2 = 2.0 * l.s4 * l.z32;
  ls.xgh3 = 2.0 * l.s4 * (l.z33 - l.z31);
  ls.xgh4 = -18.0 * l.s4 * zel;
  ls.xh2 = -2.0 * l.s2 * l.z22;
  ls.xh3 = -2.0 * l.s2 * (l.z23 - l.z21);
  return d;
}

/** The Lyddane modification for inclinations under 0.2 rad (in dpper). */
void lyddane(double ph, double pinc, double pl, double pgh, Sgp4Mode mode,
             Angles& p)
{
  const double sinip = std::sin(p.incl);
  const double cosip = std::cos(p.incl);
  const double sinop = std::sin(p.node);
  const double cosop = std::cos(p.node);
  double alfdp = sinip * sinop;
  double betdp = sinip * cosop;
  const double dalf = ph * cosop + pinc * cosip * sinop;
  const double dbet = -ph * sinop + pinc * cosip * cosop;
  alfdp = alfdp + dalf;
  betdp = betdp + dbet;
  p.node = std::fmod(p.node, twoPi);
  // The operational code's intrinsic functions (sgp4fix).
  if (p.node < 0.0 && mode == Sgp4Mode::Afspc) {
    p.node = p.node + twoPi;
  }
  const double xls = p.mo + p.argp + pl + pgh + (cosip - pinc * sinip) * p.node;
  const double xnoh = p.node;
  p.node = std::atan2(alfdp, betdp);
  if (p.node < 0.0 && mode == Sgp4Mode::Afspc) {
    p.node = p.node + twoPi;
  }
  if (std::fabs(xnoh - p.node) > pi) {
    p.node = p.node < xnoh ? p.node + twoPi : p.node - twoPi;
  }
  p.mo = p.mo + pl;
  p.argp = xls - p.mo - cosip * p.node;
}

/** Reference: dpper, after the epoch (init 'n'). */
void dpper(const LunarSolar& ls, double t, Sgp4Mode mode, Angles& p)
{
  constexpr double zns = 1.19459e-5;
  constexpr double zes = 0.01675;
  constexpr double znl = 1.5835218e-4;
  constexpr double zel = 0.05490;

  // The Sun.
  double zm = ls.zmos + zns * t;
  double zf = zm + 2.0 * zes * std::sin(zm);
  double sinzf = std::sin(zf);
  double f2 = 0.5 * sinzf * sinzf - 0.25;
  double f3 = -0.5 * sinzf * std::cos(zf);
  const double ses = ls.se2 * f2 + ls.se3 * f3;
  const double sis = ls.si2 * f2 + ls.si3 * f3;
  const double sls = ls.sl2 * f2 + ls.sl3 * f3 + ls.sl4 * sinzf;
  const double sghs = ls.sgh2 * f2 + ls.sgh3 * f3 + ls.sgh4 * sinzf;
  const double shs = ls.sh2 * f2 + ls.sh3 * f3;
  // The Moon.
  zm = ls.zmol + znl * t;
  zf = zm + 2.0 * zel * std::sin(zm);
  sinzf = std::sin(zf);
  f2 = 0.5 * sinzf * sinzf - 0.25;
  f3 = -0.5 * sinzf * std::cos(zf);
  const double sel = ls.ee2 * f2 + ls.e3 * f3;
  const double sil = ls.xi2 * f2 + ls.xi3 * f3;
  const double sll = ls.xl2 * f2 + ls.xl3 * f3 + ls.xl4 * sinzf;
  const double sghl = ls.xgh2 * f2 + ls.xgh3 * f3 + ls.xgh4 * sinzf;
  const double shll = ls.xh2 * f2 + ls.xh3 * f3;

  const double pe = ses + sel - ls.peo;
  const double pinc = sis + sil - ls.pinco;
  const double pl = sls + sll - ls.plo;
  double pgh = sghs + sghl - ls.pgho;
  double ph = shs + shll - ls.pho;
  p.incl = p.incl + pinc;
  p.ecc = p.ecc + pe;
  const double sinip = std::sin(p.incl);
  const double cosip = std::cos(p.incl);
  // The Lyddane choice on the perturbed inclination, as GSFC (sgp4fix).
  if (p.incl >= 0.2) {
    ph /= sinip;
    pgh -= cosip * ph;
    p.argp += pgh;
    p.node += ph;
    p.mo += pl;
  } else {
    lyddane(ph, pinc, pl, pgh, mode, p);
  }
}

/** The half-day resonance terms (reference: dsinit, irez 2). */
void halfDayResonance(const ModelData& m, const Dscom& d, double aonv,
                      Resonance& r)
{
  const double cosim = d.cosim;
  const double sinim = d.sinim;
  const double cosisq = cosim * cosim;
  const double em = m.ecco;
  const double emsq = m.ecco * m.ecco;
  const double eoc = em * emsq;
  const double g201 = -0.306 - (em - 0.64) * 0.440;
  double g211 = 0.0;
  double g310 = 0.0;
  double g322 = 0.0;
  double g410 = 0.0;
  double g422 = 0.0;
  double g520 = 0.0;
  if (em <= 0.65) {
    g211 = 3.616 - 13.2470 * em + 16.2900 * emsq;
    g310 = -19.302 + 117.3900 * em - 228.4190 * emsq + 156.5910 * eoc;
    g322 = -18.9068 + 109.7927 * em - 214.6334 * emsq + 146.5816 * eoc;
    g410 = -41.122 + 242.6940 * em - 471.0940 * emsq + 313.9530 * eoc;
    g422 = -146.407 + 841.8800 * em - 1629.014 * emsq + 1083.4350 * eoc;
    g520 = -532.114 + 3017.977 * em - 5740.032 * emsq + 3708.2760 * eoc;
  } else {
    g211 = -72.099 + 331.819 * em - 508.738 * emsq + 266.724 * eoc;
    g310 = -346.844 + 1582.851 * em - 2415.925 * emsq + 1246.113 * eoc;
    g322 = -342.585 + 1554.908 * em - 2366.899 * emsq + 1215.972 * eoc;
    g410 = -1052.797 + 4758.686 * em - 7193.992 * emsq + 3651.957 * eoc;
    g422 = -3581.690 + 16178.110 * em - 24462.770 * emsq + 12422.520 * eoc;
    g520 = em > 0.715
               ? -5149.66 + 29936.92 * em - 54087.36 * emsq + 31324.56 * eoc
               : 1464.74 - 4664.75 * em + 3763.64 * emsq;
  }
  double g533 = 0.0;
  double g521 = 0.0;
  double g532 = 0.0;
  if (em < 0.7) {
    g533 = -919.22770 + 4988.6100 * em - 9064.7700 * emsq + 5542.21 * eoc;
    g521 = -822.71072 + 4568.6173 * em - 8491.4146 * emsq + 5337.524 * eoc;
    g532 = -853.66600 + 4690.2500 * em - 8624.7700 * emsq + 5341.4 * eoc;
  } else {
    g533 = -37995.780 + 161616.52 * em - 229838.20 * emsq + 109377.94 * eoc;
    g521 = -51752.104 + 218913.95 * em - 309468.16 * emsq + 146349.42 * eoc;
    g532 = -40023.880 + 170470.89 * em - 242699.48 * emsq + 115605.82 * eoc;
  }
  const double sini2 = sinim * sinim;
  const double f220 = 0.75 * (1.0 + 2.0 * cosim + cosisq);
  const double f221 = 1.5 * sini2;
  const double f321 = 1.875 * sinim * (1.0 - 2.0 * cosim - 3.0 * cosisq);
  const double f322 = -1.875 * sinim * (1.0 + 2.0 * cosim - 3.0 * cosisq);
  const double f441 = 35.0 * sini2 * f220;
  const double f442 = 39.3750 * sini2 * sini2;
  const double f522 = 9.84375 * sinim *
                      (sini2 * (1.0 - 2.0 * cosim - 5.0 * cosisq) +
                       0.33333333 * (-2.0 + 4.0 * cosim + 6.0 * cosisq));
  const double f523 =
      sinim * (4.92187512 * sini2 * (-2.0 - 4.0 * cosim + 10.0 * cosisq) +
               6.56250012 * (1.0 + 2.0 * cosim - 3.0 * cosisq));
  const double f542 =
      29.53125 * sinim *
      (2.0 - 8.0 * cosim + cosisq * (-12.0 + 8.0 * cosim + 10.0 * cosisq));
  const double f543 =
      29.53125 * sinim *
      (-2.0 - 8.0 * cosim + cosisq * (12.0 + 8.0 * cosim - 10.0 * cosisq));

  constexpr double root22 = 1.7891679e-6;
  constexpr double root44 = 7.3636953e-9;
  constexpr double root54 = 2.1765803e-9;
  constexpr double root32 = 3.7393792e-7;
  constexpr double root52 = 1.1428639e-7;
  const double nm = d.nm;
  const double xno2 = nm * nm;
  const double ainv2 = aonv * aonv;
  double temp1 = 3.0 * xno2 * ainv2;
  double temp = temp1 * root22;
  r.d2201 = temp * f220 * g201;
  r.d2211 = temp * f221 * g211;
  temp1 = temp1 * aonv;
  temp = temp1 * root32;
  r.d3210 = temp * f321 * g310;
  r.d3222 = temp * f322 * g322;
  temp1 = temp1 * aonv;
  temp = 2.0 * temp1 * root44;
  r.d4410 = temp * f441 * g410;
  r.d4422 = temp * f442 * g422;
  temp1 = temp1 * aonv;
  temp = temp1 * root52;
  r.d5220 = temp * f522 * g520;
  r.d5232 = temp * f523 * g532;
  temp = 2.0 * temp1 * root54;
  r.d5421 = temp * f542 * g521;
  r.d5433 = temp * f543 * g533;
  const double theta = std::fmod(m.gsto, twoPi);
  r.xlamo = std::fmod(m.mo + m.nodeo + m.nodeo - theta - theta, twoPi);
  r.xfact = m.mdot + r.dmdt + 2.0 * (m.nodedot + r.dnodt - rptim) - m.noUnkozai;
}

/** The one-day (synchronous) resonance terms (reference: dsinit, irez 1). */
// aonv, then xpidot, as the reference computes them.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void oneDayResonance(const ModelData& m, const Dscom& d, double aonv,
                     double xpidot, Resonance& r)
{
  constexpr double q22 = 1.7891679e-6;
  constexpr double q31 = 2.1460748e-6;
  constexpr double q33 = 2.2123015e-7;
  const double emsq = d.emsq;
  const double cosim = d.cosim;
  const double sinim = d.sinim;
  const double nm = d.nm;
  const double g200 = 1.0 + emsq * (-2.5 + 0.8125 * emsq);
  const double g310 = 1.0 + 2.0 * emsq;
  const double g300 = 1.0 + emsq * (-6.0 + 6.60937 * emsq);
  const double f220 = 0.75 * (1.0 + cosim) * (1.0 + cosim);
  const double f311 =
      0.9375 * sinim * sinim * (1.0 + 3.0 * cosim) - 0.75 * (1.0 + cosim);
  double f330 = 1.0 + cosim;
  f330 = 1.875 * f330 * f330 * f330;
  r.del1 = 3.0 * nm * nm * aonv * aonv;
  r.del2 = 2.0 * r.del1 * f220 * g200 * q22;
  r.del3 = 3.0 * r.del1 * f330 * g300 * q33 * aonv;
  r.del1 = r.del1 * f311 * g310 * q31 * aonv;
  const double theta = std::fmod(m.gsto, twoPi);
  r.xlamo = std::fmod(m.mo + m.nodeo + m.argpo - theta, twoPi);
  r.xfact = m.mdot + xpidot - rptim + r.dmdt + r.domdt + r.dnodt - m.noUnkozai;
}

/** Reference: dsinit, at the epoch. */
Resonance dsinit(const ModelData& m, const Dscom& d, double xpidot)
{
  constexpr double znl = 1.5835218e-4;
  constexpr double zns = 1.19459e-5;
  const BodyTerms& ss = d.sun;
  const BodyTerms& s = d.moon;
  const double nm = d.nm;
  const double em = d.em;
  const double emsq = d.emsq;
  Resonance r;
  if (0.0034906585 < nm && nm < 0.0052359877) {
    r.irez = 1;
  }
  if (8.26e-3 <= nm && nm <= 9.24e-3 && em >= 0.5) {
    r.irez = 2;
  }
  const bool equatorial = m.inclo < 5.2359877e-2 || m.inclo > pi - 5.2359877e-2;
  // Solar terms.
  const double ses = ss.s1 * zns * ss.s5;
  const double sis = ss.s2 * zns * (ss.z11 + ss.z13);
  const double sls = -zns * ss.s3 * (ss.z1 + ss.z3 - 14.0 - 6.0 * emsq);
  const double sghs = ss.s4 * zns * (ss.z31 + ss.z33 - 6.0);
  double shs = -zns * ss.s2 * (ss.z21 + ss.z23);
  if (equatorial) {
    shs = 0.0;
  }
  if (d.sinim != 0.0) {
    shs = shs / d.sinim;
  }
  const double sgs = sghs - d.cosim * shs;
  // Lunar terms.
  r.dedt = ses + s.s1 * znl * s.s5;
  r.didt = sis + s.s2 * znl * (s.z11 + s.z13);
  r.dmdt = sls - znl * s.s3 * (s.z1 + s.z3 - 14.0 - 6.0 * emsq);
  const double sghl = s.s4 * znl * (s.z31 + s.z33 - 6.0);
  double shll = -znl * s.s2 * (s.z21 + s.z23);
  if (equatorial) {
    shll = 0.0;
  }
  r.domdt = sgs + sghl;
  r.dnodt = shs;
  if (d.sinim != 0.0) {
    r.domdt = r.domdt - d.cosim / d.sinim * shll;
    r.dnodt = r.dnodt + shll / d.sinim;
  }
  if (r.irez == 0) {
    return r;
  }
  const double aonv = std::pow(nm / m.g.xke, x2o3);
  if (r.irez == 2) {
    halfDayResonance(m, d, aonv, r);
  } else {
    oneDayResonance(m, d, aonv, xpidot, r);
  }
  return r;
}

/** The resonance's rates at one step of the integrator. */
struct ResonanceRates {
  double xndt = 0.0;
  double xldot = 0.0;
  double xnddt = 0.0;
};

// The integrator's state, in the reference's order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ResonanceRates resonanceRates(const ModelData& m, double atime, double xli,
                              double xni)
{
  constexpr double fasx2 = 0.13130908;
  constexpr double fasx4 = 2.8843198;
  constexpr double fasx6 = 0.37448087;
  constexpr double g22 = 5.7686396;
  constexpr double g32 = 0.95240898;
  constexpr double g44 = 1.8014998;
  constexpr double g52 = 1.0508330;
  constexpr double g54 = 4.4108898;
  const Resonance& r = m.res;
  ResonanceRates rates;
  rates.xldot = xni + r.xfact;
  if (r.irez != 2) {
    rates.xndt = r.del1 * std::sin(xli - fasx2) +
                 r.del2 * std::sin(2.0 * (xli - fasx4)) +
                 r.del3 * std::sin(3.0 * (xli - fasx6));
    rates.xnddt = r.del1 * std::cos(xli - fasx2) +
                  2.0 * r.del2 * std::cos(2.0 * (xli - fasx4)) +
                  3.0 * r.del3 * std::cos(3.0 * (xli - fasx6));
    rates.xnddt = rates.xnddt * rates.xldot;
    return rates;
  }
  const double xomi = m.argpo + m.argpdot * atime;
  const double x2omi = xomi + xomi;
  const double x2li = xli + xli;
  rates.xndt =
      (r.d2201 * std::sin(x2omi + xli - g22) + r.d2211 * std::sin(xli - g22) +
       r.d3210 * std::sin(xomi + xli - g32) +
       r.d3222 * std::sin(-xomi + xli - g32) +
       r.d4410 * std::sin(x2omi + x2li - g44) + r.d4422 * std::sin(x2li - g44) +
       r.d5220 * std::sin(xomi + xli - g52) +
       r.d5232 * std::sin(-xomi + xli - g52) +
       r.d5421 * std::sin(xomi + x2li - g54) +
       r.d5433 * std::sin(-xomi + x2li - g54));
  rates.xnddt =
      (r.d2201 * std::cos(x2omi + xli - g22) + r.d2211 * std::cos(xli - g22) +
       r.d3210 * std::cos(xomi + xli - g32) +
       r.d3222 * std::cos(-xomi + xli - g32) +
       r.d5220 * std::cos(xomi + xli - g52) +
       r.d5232 * std::cos(-xomi + xli - g52) +
       2.0 * (r.d4410 * std::cos(x2omi + x2li - g44) +
              r.d4422 * std::cos(x2li - g44) +
              r.d5421 * std::cos(xomi + x2li - g54) +
              r.d5433 * std::cos(-xomi + x2li - g54)));
  rates.xnddt = rates.xnddt * rates.xldot;
  return rates;
}

/**
 * Reference: dspace. The resonance is integrated from the epoch in steps
 * of 720 minutes each time (the reference keeps the last step to start
 * from, which gives the same numbers).
 */
void dspace(const ModelData& m, double t, Secular& s)
{
  constexpr double stepp = 720.0;
  constexpr double stepn = -720.0;
  constexpr double step2 = 259200.0;
  const Resonance& r = m.res;
  const double theta = std::fmod(m.gsto + t * rptim, twoPi);
  s.em = s.em + r.dedt * t;
  s.inclm = s.inclm + r.didt * t;
  s.argpm = s.argpm + r.domdt * t;
  s.nodem = s.nodem + r.dnodt * t;
  s.mm = s.mm + r.dmdt * t;
  if (r.irez == 0) {
    return;
  }
  double atime = 0.0;
  double xni = m.noUnkozai;
  double xli = r.xlamo;
  const double delt = t > 0.0 ? stepp : stepn;
  double ft = 0.0;
  ResonanceRates rates;
  for (;;) {
    rates = resonanceRates(m, atime, xli, xni);
    if (std::fabs(t - atime) < stepp) {
      ft = t - atime;
      break;
    }
    xli = xli + rates.xldot * delt + rates.xndt * step2;
    xni = xni + rates.xndt * delt + rates.xnddt * step2;
    atime = atime + delt;
  }
  s.nm = xni + rates.xndt * ft + rates.xnddt * ft * ft * 0.5;
  const double xl = xli + rates.xldot * ft + rates.xndt * ft * ft * 0.5;
  if (r.irez != 1) {
    s.mm = xl - 2.0 * s.nodem + 2.0 * theta;
  } else {
    s.mm = xl - s.nodem - s.argpm + theta;
  }
  const double dndt = s.nm - m.noUnkozai;
  s.nm = m.noUnkozai + dndt;
}

/** The coefficients that depend on the inclination (deep space: moving). */
struct InclinationTerms {
  double sinip = 0.0;
  double cosip = 0.0;
  double aycof = 0.0;
  double xlcof = 0.0;
  double con41 = 0.0;
  double x1mth2 = 0.0;
  double x7thm1 = 0.0;
};

InclinationTerms deepInclinationTerms(const ModelData& m, double incl)
{
  InclinationTerms t;
  t.sinip = std::sin(incl);
  t.cosip = std::cos(incl);
  t.aycof = -0.5 * m.g.j3oj2 * t.sinip;
  const double divisor =
      std::fabs(t.cosip + 1.0) > 1.5e-12 ? 1.0 + t.cosip : temp4;
  t.xlcof = -0.25 * m.g.j3oj2 * t.sinip * (3.0 + 5.0 * t.cosip) / divisor;
  const double cosisq = t.cosip * t.cosip;
  t.con41 = 3.0 * cosisq - 1.0;
  t.x1mth2 = 1.0 - cosisq;
  t.x7thm1 = 7.0 * cosisq - 1.0;
  return t;
}

/** The secular effects of gravity and drag (the start of sgp4). */
Secular secular(const ModelData& m, double t, double& tempa, double& tempe,
                double& templ)
{
  const double xmdf = m.mo + m.mdot * t;
  const double argpdf = m.argpo + m.argpdot * t;
  const double nodedf = m.nodeo + m.nodedot * t;
  Secular s;
  s.argpm = argpdf;
  s.mm = xmdf;
  const double t2 = t * t;
  s.nodem = nodedf + m.nodecf * t2;
  tempa = 1.0 - m.cc1 * t;
  tempe = m.bstar * m.cc4 * t;
  templ = m.t2cof * t2;
  if (!m.isimp) {
    const double delomg = m.omgcof * t;
    const double delmtemp = 1.0 + m.eta * std::cos(xmdf);
    const double delm = m.xmcof * (delmtemp * delmtemp * delmtemp - m.delmo);
    const double temp = delomg + delm;
    s.mm = xmdf + temp;
    s.argpm = argpdf - temp;
    const double t3 = t2 * t;
    const double t4 = t3 * t;
    tempa = tempa - m.d2 * t2 - m.d3 * t3 - m.d4 * t4;
    tempe = tempe + m.bstar * m.cc5 * (std::sin(s.mm) - m.sinmao);
    templ = templ + m.t3cof * t3 + t4 * (m.t4cof + t * m.t5cof);
  }
  s.nm = m.noUnkozai;
  s.em = m.ecco;
  s.inclm = m.inclo;
  if (m.deep) {
    dspace(m, t, s);
  }
  return s;
}

/** Kepler's equation for the eccentric longitude (in sgp4). */
struct Kepler {
  double sineo1 = 0.0;
  double coseo1 = 0.0;
};

Kepler solveKepler(double u, double axnl, double aynl)
{
  double eo1 = u;
  double tem5 = 9999.9;
  int32_t ktr = 1;
  Kepler k;
  while (std::fabs(tem5) >= 1.0e-12 && ktr <= 10) {
    k.sineo1 = std::sin(eo1);
    k.coseo1 = std::cos(eo1);
    tem5 = 1.0 - k.coseo1 * axnl - k.sineo1 * aynl;
    tem5 = (u - aynl * k.coseo1 + axnl * k.sineo1 - eo1) / tem5;
    if (std::fabs(tem5) >= 0.95) {
      tem5 = tem5 > 0.0 ? 0.95 : -0.95;
    }
    eo1 = eo1 + tem5;
    ktr = ktr + 1;
  }
  return k;
}

/** The short-period periodics and the state (the end of sgp4). */
Sgp4Result shortPeriodics(const ModelData& m, const Angles& p, double am,
                          double nm, const InclinationTerms& it)
{
  Sgp4Result result;
  const Gravity& g = m.g;
  const double vkmpersec = g.radiusKm * g.xke / 60.0;
  const double axnl = p.ecc * std::cos(p.argp);
  double temp = 1.0 / (am * (1.0 - p.ecc * p.ecc));
  const double aynl = p.ecc * std::sin(p.argp) + temp * it.aycof;
  const double xl = p.mo + p.argp + p.node + temp * it.xlcof * axnl;
  const double u = std::fmod(xl - p.node, twoPi);
  const Kepler k = solveKepler(u, axnl, aynl);
  const double ecose = axnl * k.coseo1 + aynl * k.sineo1;
  const double esine = axnl * k.sineo1 - aynl * k.coseo1;
  const double el2 = axnl * axnl + aynl * aynl;
  const double pl = am * (1.0 - el2);
  if (pl < 0.0) {
    result.error = Sgp4Error::SemiLatusRectum;
    return result;
  }
  const double rl = am * (1.0 - ecose);
  const double rdotl = std::sqrt(am) * esine / rl;
  const double rvdotl = std::sqrt(pl) / rl;
  const double betal = std::sqrt(1.0 - el2);
  temp = esine / (1.0 + betal);
  const double sinu = am / rl * (k.sineo1 - aynl - axnl * temp);
  const double cosu = am / rl * (k.coseo1 - axnl + aynl * temp);
  double su = std::atan2(sinu, cosu);
  const double sin2u = (cosu + cosu) * sinu;
  const double cos2u = 1.0 - 2.0 * sinu * sinu;
  temp = 1.0 / pl;
  const double temp1 = 0.5 * g.j2 * temp;
  const double temp2 = temp1 * temp;

  const double mrt = rl * (1.0 - 1.5 * temp2 * betal * it.con41) +
                     0.5 * temp1 * it.x1mth2 * cos2u;
  su = su - 0.25 * temp2 * it.x7thm1 * sin2u;
  const double xnode = p.node + 1.5 * temp2 * it.cosip * sin2u;
  const double xinc = p.incl + 1.5 * temp2 * it.cosip * it.sinip * cos2u;
  const double mvt = rdotl - nm * temp1 * it.x1mth2 * sin2u / g.xke;
  const double rvdot =
      rvdotl + nm * temp1 * (it.x1mth2 * cos2u + 1.5 * it.con41) / g.xke;

  const double sinsu = std::sin(su);
  const double cossu = std::cos(su);
  const double snod = std::sin(xnode);
  const double cnod = std::cos(xnode);
  const double sini = std::sin(xinc);
  const double cosi = std::cos(xinc);
  const double xmx = -snod * cosi;
  const double xmy = cnod * cosi;
  const double ux = xmx * sinsu + cnod * cossu;
  const double uy = xmy * sinsu + snod * cossu;
  const double uz = sini * sinsu;
  const double vx = xmx * cossu - cnod * sinsu;
  const double vy = xmy * cossu - snod * sinsu;
  const double vz = sini * cossu;

  const double mr = mrt * g.radiusKm;
  result.teme.positionKm = Vec3{mr * ux, mr * uy, mr * uz};
  result.teme.velocityKmS = Vec3{(mvt * ux + rvdot * vx) * vkmpersec,
                                 (mvt * uy + rvdot * vy) * vkmpersec,
                                 (mvt * uz + rvdot * vz) * vkmpersec};
  if (mrt < 1.0) {
    result.error = Sgp4Error::Decayed;
  }
  return result;
}

/** Reference: sgp4, at @p t minutes from the epoch. */
Sgp4Result propagateModel(const ModelData& m, double t)
{
  Sgp4Result result;
  double tempa = 0.0;
  double tempe = 0.0;
  double templ = 0.0;
  Secular s = secular(m, t, tempa, tempe, templ);
  if (s.nm <= 0.0) {
    result.error = Sgp4Error::MeanMotion;
    return result;
  }
  const double am = std::pow(m.g.xke / s.nm, x2o3) * tempa * tempa;
  s.nm = m.g.xke / std::pow(am, 1.5);
  s.em = s.em - tempe;
  if (s.em >= 1.0 || s.em < -0.001) {
    result.error = Sgp4Error::MeanEccentricity;
    return result;
  }
  if (s.em < 1.0e-6) {
    s.em = 1.0e-6;
  }
  s.mm = s.mm + m.noUnkozai * templ;
  double xlm = s.mm + s.argpm + s.nodem;
  s.nodem = std::fmod(s.nodem, twoPi);
  s.argpm = std::fmod(s.argpm, twoPi);
  xlm = std::fmod(xlm, twoPi);
  s.mm = std::fmod(xlm - s.argpm - s.nodem, twoPi);

  Angles p{s.em, s.inclm, s.nodem, s.argpm, s.mm};
  InclinationTerms it;
  if (m.deep) {
    dpper(m.ls, t, m.mode, p);
    if (p.incl < 0.0) {
      p.incl = -p.incl;
      p.node = p.node + pi;
      p.argp = p.argp - pi;
    }
    if (p.ecc < 0.0 || p.ecc > 1.0) {
      result.error = Sgp4Error::PerturbedEccentricity;
      return result;
    }
    it = deepInclinationTerms(m, p.incl);
  } else {
    it.sinip = std::sin(s.inclm);
    it.cosip = std::cos(s.inclm);
    it.aycof = m.aycof;
    it.xlcof = m.xlcof;
    it.con41 = m.con41;
    it.x1mth2 = m.x1mth2;
    it.x7thm1 = m.x7thm1;
  }
  return shortPeriodics(m, p, am, s.nm, it);
}

/** Reference: sgp4init. */
void initialise(ModelData& m)
{
  const Initl il = initl(m);
  if (il.omeosq < 0.0 && m.noUnkozai < 0.0) {
    return;
  }
  const NearEarth ne = nearEarthInit(m, il);
  if (twoPi / m.noUnkozai >= 225.0) {
    m.deep = true;
    m.isimp = true;
    const Dscom d = dscom(m, m.ls);
    m.res = dsinit(m, d, ne.xpidot);
  }
  if (!m.isimp) {
    dragInit(m, il, ne);
  }
}

} // namespace

/** The propagator's data (ModelData, named for the class). */
struct Sgp4::Model : ModelData {
};

QString sgp4ErrorText(Sgp4Error error)
{
  switch (error) {
  case Sgp4Error::None:
    return QString();
  case Sgp4Error::MeanEccentricity:
    return QCoreApplication::translate(
        "ts::Sgp4", "The mean eccentricity left its range (0 to 1).");
  case Sgp4Error::MeanMotion:
    return QCoreApplication::translate("ts::Sgp4",
                                       "The mean motion fell below zero.");
  case Sgp4Error::PerturbedEccentricity:
    return QCoreApplication::translate(
        "ts::Sgp4", "The perturbed eccentricity left its range (0 to 1).");
  case Sgp4Error::SemiLatusRectum:
    return QCoreApplication::translate(
        "ts::Sgp4", "The semi-latus rectum fell below zero.");
  case Sgp4Error::Decayed:
    return QCoreApplication::translate(
        "ts::Sgp4", "The satellite has decayed (its orbit is inside the "
                    "Earth).");
  }
  return QString();
}

Sgp4::Sgp4(const OrbitalElements& elements, GravityModel gravity, Sgp4Mode mode)
    : m_elements(elements)
{
  std::shared_ptr<Model> model = std::make_shared<Model>();
  model->g = gravityOf(gravity);
  model->mode = mode;
  // As the reference: the Julian date in one number, from 1949-12-31.
  model->epoch = (elements.epoch.day + elements.epoch.fraction) - 2433281.5;
  model->bstar = elements.bstar;
  model->ecco = elements.eccentricity;
  model->argpo = degToRad(elements.argOfPerigeeDeg);
  model->inclo = degToRad(elements.inclinationDeg);
  model->mo = degToRad(elements.meanAnomalyDeg);
  // Revolutions per day to radians per minute.
  model->noKozai = elements.meanMotionRevPerDay / (1440.0 / twoPi);
  model->nodeo = degToRad(elements.raanDeg);
  const double e = elements.eccentricity;
  if (std::isnan(e) || e < 0.0 || e >= 1.0) {
    model->epochError = Sgp4Error::MeanEccentricity;
  } else if (std::isnan(model->noKozai) || model->noKozai <= 0.0) {
    model->epochError = Sgp4Error::MeanMotion;
  } else {
    initialise(*model);
    model->epochError = propagateModel(*model, 0.0).error;
    model->usable = true;
  }
  m_model = std::move(model);
}

Sgp4Error Sgp4::epochError() const
{
  return m_model->epochError;
}

bool Sgp4::isDeepSpace() const
{
  return m_model->deep;
}

double Sgp4::meanMotionRadPerMin() const
{
  return m_model->noUnkozai;
}

double Sgp4::periodMinutes() const
{
  return m_model->noUnkozai > 0.0 ? twoPi / m_model->noUnkozai : 0.0;
}

Sgp4Result Sgp4::propagate(double minutes) const
{
  if (!m_model->usable) {
    // Elements SGP4 can't start from.
    Sgp4Result result;
    result.error = m_model->epochError;
    return result;
  }
  return propagateModel(*m_model, minutes);
}

Sgp4Result Sgp4::propagate(JulianDate time) const
{
  return propagate(time.minutesSince(m_elements.epoch));
}

} // namespace ts
