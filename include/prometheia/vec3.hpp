// SPDX-License-Identifier: GPL-2.0-or-later
//
// 3-vector arithmetic for the mechanics core. Plain doubles, inline ops —
// the compiler vectorizes across call sites; no abstraction in the hot path.
#ifndef PROMETHEIA_VEC3_HPP
#define PROMETHEIA_VEC3_HPP

#include <cmath>

namespace prometheia {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3& operator+=(const Vec3& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    Vec3& operator-=(const Vec3& o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    Vec3& operator*=(double s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}
inline Vec3 operator*(Vec3 a, double s) {
    return Vec3(a.x * s, a.y * s, a.z * s);
}
inline Vec3 operator*(double s, Vec3 a) {
    return a * s;
}

inline double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline double norm(Vec3 a) {
    return std::sqrt(dot(a, a));
}
inline double norm2(Vec3 a) {
    return dot(a, a);
}
inline Vec3 normalized(Vec3 a) {
    const double n = norm(a);
    return n > 0.0 ? a * (1.0 / n) : Vec3{};
}

} // namespace prometheia

#endif // PROMETHEIA_VEC3_HPP
