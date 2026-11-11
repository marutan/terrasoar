// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/projection/projection.h"

#include <cstddef>

#include "ts/projection/cylindricalprojection.h"

namespace ts
{

namespace
{

// atanh() of GLSL is commonly log((1 + x) / (1 − x)) / 2, which loses the
// small arguments that matter here (a metre is 1.6e-7 of the radius), so
// small arguments use the series instead.
//
// tsCylindricalDy: y(φt + d) − y(φt) on the unit sphere for the family
// of CylindricalProjection, y = c atanh(sin(s φ)) − e atanh(e sin(s φ)),
// with r0 = (sin(s φt), cos(s φt), least d, greatest d) and
// r1 = (1 − e² sin²(s φt), e, s, c). Uses
//   atanh(u) − atanh(v) = atanh((u − v) / (1 − u v)) and
//   sin(p + q) − sin(p) = 2 cos(p + q/2) sin(q/2),
// neither of which subtracts nearly equal numbers.
const char* const commonGlsl = R"(
float tsAtanh(float x)
{
  float x2 = x * x;
  if (x2 < 0.04) {
    return x * (1.0 + x2 * (1.0 / 3.0 + x2 * (1.0 / 5.0 + x2 * (1.0 / 7.0
           + x2 * (1.0 / 9.0 + x2 * (1.0 / 11.0))))));
  }
  return 0.5 * log((1.0 + x) / (1.0 - x));
}

float tsCylindricalDy(float dLatRad, vec4 r0, vec4 r1)
{
  float q = clamp(dLatRad, r0.z, r0.w) * r1.z;
  float sinHalf = sin(0.5 * q);
  float cosHalf = cos(0.5 * q);
  float dSin = 2.0 * (r0.y * cosHalf - r0.x * sinHalf) * sinHalf;
  float y = r1.w * tsAtanh(dSin / (r0.y * r0.y - r0.x * dSin));
  if (r1.y > 0.0) {
    y -= r1.y * tsAtanh(r1.y * dSin / (r1.x - r1.y * r1.y * r0.x * dSin));
  }
  return y;
}
)";

} // namespace

Projection::Projection(double centralMeridianDeg)
    : m_centralMeridianDeg(normaliseLonDeg(centralMeridianDeg))
{
}

Projection::~Projection() = default;

const std::vector<const Projection*>& projections()
{
  // To add a projection: write its class and list it here (see
  // docs/design/projections.md).
  static const MercatorProjection mercator;
  static const WebMercatorProjection webMercator;
  static const MillerProjection miller;
  static const std::vector<const Projection*> all = {&mercator, &webMercator,
                                                     &miller};
  return all;
}

const Projection* projectionById(const QString& id)
{
  for (const Projection* projection : projections()) {
    if (projection->id() == id) {
      return projection;
    }
  }
  return nullptr;
}

int projectionIndex(const Projection* projection)
{
  const std::vector<const Projection*>& all = projections();
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i] == projection) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

QByteArray projectionGlsl()
{
  QByteArray glsl = commonGlsl;
  const std::vector<const Projection*>& all = projections();
  for (const Projection* projection : all) {
    glsl += projection->glslSource();
  }
  glsl += "\nvec2 tsProject(int index, vec2 dLonLatRad, vec4 r0, vec4 r1)\n{\n";
  for (std::size_t i = 1; i < all.size(); ++i) {
    glsl += "  if (index == " + QByteArray::number(static_cast<int>(i)) +
            ") {\n    return " + all[i]->glslFunction() +
            "(dLonLatRad, r0, r1);\n  }\n";
  }
  glsl +=
      "  return " + all.front()->glslFunction() + "(dLonLatRad, r0, r1);\n}\n";
  return glsl;
}

} // namespace ts
