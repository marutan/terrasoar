// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * 4×4 double-precision matrix.
 */
#ifndef TS_MATHS_MAT4_H
#define TS_MATHS_MAT4_H

#include <array>
#include <cstddef>

#include "ts/maths/vec3.h"
#include "ts/maths/vec4.h"

namespace ts
{

/**
 * A 4×4 matrix of doubles for affine and projective transforms.
 *
 * Elements are stored column-major, as OpenGL expects: element (row, column)
 * is at index column × 4 + row. Matrices act on column vectors, so
 * `a * b` applies b first, then a.
 *
 * The default is the identity matrix.
 *
 * @reentrant
 */
class Mat4
{
public:
  /** Creates the identity matrix. */
  constexpr Mat4()
      : m_elements{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                   0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0}
  {
  }

  /** Creates a matrix from 16 elements in column-major order. */
  explicit constexpr Mat4(const std::array<double, 16>& columnMajor)
      : m_elements(columnMajor)
  {
  }

  /** Returns the identity matrix. */
  static constexpr Mat4 identity() { return Mat4(); }

  /** Returns the translation by @p offset. */
  static constexpr Mat4 translation(const Vec3& offset)
  {
    Mat4 result;
    result.set(0, 3, offset.x);
    result.set(1, 3, offset.y);
    result.set(2, 3, offset.z);
    return result;
  }

  /** Returns the scaling by @p factors along x, y and z. */
  static constexpr Mat4 scaling(const Vec3& factors)
  {
    Mat4 result;
    result.set(0, 0, factors.x);
    result.set(1, 1, factors.y);
    result.set(2, 2, factors.z);
    return result;
  }

  /** Returns element (@p row, @p column), both in [0, 3]. */
  constexpr double at(int row, int column) const
  {
    // Row and column are in [0, 3] by contract; a bounds check on every
    // element access would cost too much in the maths hot paths.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return m_elements[index(row, column)];
  }

  /** Sets element (@p row, @p column), both in [0, 3], to @p value. */
  constexpr void set(int row, int column, double value)
  {
    // See at().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    m_elements[index(row, column)] = value;
  }

  /** Returns the 16 elements in column-major order. */
  constexpr const std::array<double, 16>& elements() const
  {
    return m_elements;
  }

  /** Returns @p v transformed by this matrix. */
  constexpr Vec4 operator*(const Vec4& v) const
  {
    return Vec4{
        at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
        at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
        at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
        at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w};
  }

  /** Returns the product: this matrix applied after @p other. */
  constexpr Mat4 operator*(const Mat4& other) const
  {
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) {
        double sum = 0.0;
        for (int k = 0; k < 4; ++k) {
          sum += at(row, k) * other.at(k, column);
        }
        result.set(row, column, sum);
      }
    }
    return result;
  }

  /**
   * Returns the point @p p transformed by this matrix, divided by the
   * resulting w (the perspective divide). For an affine matrix w stays 1.
   */
  constexpr Vec3 transformPoint(const Vec3& p) const
  {
    const Vec4 result = *this * Vec4::point(p);
    return result.w != 0.0 && result.w != 1.0 ? result.xyz() / result.w
                                              : result.xyz();
  }

  /** Returns the direction @p d transformed by this matrix (w = 0). */
  constexpr Vec3 transformDirection(const Vec3& d) const
  {
    return (*this * Vec4::direction(d)).xyz();
  }

  /** Returns the transposed matrix. */
  constexpr Mat4 transposed() const
  {
    // Element (i, j) of the result is element (j, i) of this matrix.
    Mat4 result;
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        result.set(i, j, at(j, i));
      }
    }
    return result;
  }

  /**
   * Returns the inverse matrix.
   *
   * @param invertible If not null, set to false when the matrix is singular
   *   (its determinant is 0); the identity is returned then.
   */
  Mat4 inverted(bool* invertible = nullptr) const;

  /**
   * Returns true if every element equals that of @p other exactly. (Not
   * constexpr: std::array's comparison is only constexpr from C++20.)
   */
  bool operator==(const Mat4& other) const
  {
    return m_elements == other.m_elements;
  }

  /** Returns true if any element differs from that of @p other. */
  bool operator!=(const Mat4& other) const { return !(*this == other); }

private:
  static constexpr std::size_t index(int row, int column)
  {
    return static_cast<std::size_t>(column) * 4U +
           static_cast<std::size_t>(row);
  }

  std::array<double, 16> m_elements;
};

} // namespace ts

#endif // TS_MATHS_MAT4_H
