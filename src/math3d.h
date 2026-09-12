#ifndef VKGEARS_MATH3D_H
#define VKGEARS_MATH3D_H

// Minimal 3D math for the demo - deliberately dependency free (no GLM) so the
// project builds with nothing but a C++11 compiler and the vendored headers.
//
// Matrices are stored column-major as a flat 16-float array, which is exactly
// the memory layout GLSL expects for a mat4 in a uniform/push-constant block.

#include <cmath>

namespace vkg {

struct Vec3 {
    float x, y, z;

    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline Vec3 operator*(const Vec3& a, float s)       { return Vec3(a.x * s, a.y * s, a.z * s); }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const Vec3& v)             { return std::sqrt(dot(v, v)); }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x);
}

inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return (len > 1e-8f) ? v * (1.0f / len) : Vec3(0.0f, 0.0f, 0.0f);
}

const float PI = 3.14159265358979323846f;

inline float toRadians(float degrees) { return degrees * PI / 180.0f; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Mat4 {
    float m[16];

    Mat4() { setIdentity(); }

    void setIdentity() {
        for (int i = 0; i < 16; ++i) { m[i] = 0.0f; }
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    static Mat4 translation(const Vec3& t) {
        Mat4 r;
        r.m[12] = t.x;
        r.m[13] = t.y;
        r.m[14] = t.z;
        return r;
    }

    static Mat4 rotationZ(float radians) {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        Mat4 r;
        r.m[0] = c;  r.m[4] = -s;
        r.m[1] = s;  r.m[5] = c;
        return r;
    }

    // Right-handed perspective with Vulkan's 0..1 depth range.  Y is flipped
    // because Vulkan's framebuffer Y axis points down; without the flip the
    // scene would be rendered upside down.
    static Mat4 perspective(float fovyRadians, float aspect, float nearZ, float farZ) {
        Mat4 r;
        for (int i = 0; i < 16; ++i) { r.m[i] = 0.0f; }
        const float f = 1.0f / std::tan(fovyRadians * 0.5f);
        r.m[0]  = f / aspect;
        r.m[5]  = -f;
        r.m[10] = farZ / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = nearZ * farZ / (nearZ - farZ);
        return r;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        const Vec3 f = normalize(center - eye);
        const Vec3 s = normalize(cross(f, up));
        const Vec3 u = cross(s, f);

        Mat4 r;
        r.m[0] =  s.x; r.m[4] =  s.y; r.m[8]  =  s.z; r.m[12] = -dot(s, eye);
        r.m[1] =  u.x; r.m[5] =  u.y; r.m[9]  =  u.z; r.m[13] = -dot(u, eye);
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z; r.m[14] =  dot(f, eye);
        r.m[3] =  0.0f; r.m[7] = 0.0f; r.m[11] = 0.0f; r.m[15] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& rhs) const {
        Mat4 r;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[k * 4 + row] * rhs.m[col * 4 + k];
                }
                r.m[col * 4 + row] = sum;
            }
        }
        return r;
    }
};

} // namespace vkg

#endif // VKGEARS_MATH3D_H
