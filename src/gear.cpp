#include "gear.h"

#include "diag.h"
#include "math3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vkg {
namespace {

const float TWO_PI = 2.0f * PI;

// Tooth shape.  Real gears use an involute flank because it is the only curve
// that stays conjugate as the gears turn; straight sided teeth look plausible
// but their tips collide with the mating flank.  These parameters describe a
// standard 20 degree pressure angle gear, with the tooth thinned very slightly
// (0.48 of the circular pitch instead of 0.5) so there is visible backlash.
const float kPressureAngleDegrees = 20.0f;
const float kToothThicknessFactor = 0.48f;
const int   kFlankSamples         = 5;

// Number of segments the root arc between two teeth is split into.
const int kRootArcSegments = 3;

// Tip / root radius relative to the pitch radius, in units of the module.
const float kAddendum = 0.85f;
const float kDedendum = 1.00f;

// Checker cells per texture repeat (must match buildCheckerTexture's `cells`).
const float kCheckerCells = 8.0f;

struct ProfilePoint {
    float x;
    float y;
    float angle; // angle it was placed at, used to detect collinear neighbours
    float arc;   // arc length from the first profile point, along the profile
};

// Angular half width of an involute tooth at the given radius.  Below the base
// circle the involute does not exist, so the flank continues radially.
float involuteHalfAngle(float radius,
                        float baseRadius,
                        float halfThicknessAtPitch,
                        float involuteAtPressureAngle) {
    if (radius <= baseRadius) { return halfThicknessAtPitch + involuteAtPressureAngle; }
    const float alpha = std::acos(baseRadius / radius);
    return halfThicknessAtPitch + involuteAtPressureAngle - (std::tan(alpha) - alpha);
}

bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

// ---------------------------------------------------------------------------
// Gear train timing.
//
// Two external gears with the same module mesh exactly when their centres are
// pitchRadius1 + pitchRadius2 apart and
//
//     teeth1 * angle1 + teeth2 * angle2 = constant                        (1)
//
// which makes the teeth pass the line of centres at the same instants.  With
// the constant evaluated at t = 0 (all angles 0) the remaining requirement is
// that the tooth of gear i and the gap of gear j meet at the joint:
//
//     Ni*phi_i + Nj*phi_j = (Ni + Nj)*alpha_ij + (Nj - 1)*PI   (mod 2*PI)  (2)
//
// where alpha_ij is the direction from the centre of gear i to gear j and
// phi are the start phases.  Equation (2) is solved for phi_j below; applying
// it to (A,B) and then to (B,C) yields a correctly timed train.
// ---------------------------------------------------------------------------
float solveJointPhase(float teethI, float teethJ, float alphaIJ, float phiI) {
    const float rhs = (teethI + teethJ) * alphaIJ + (teethJ - 1.0f) * PI - teethI * phiI;
    const float pitch = TWO_PI / teethJ;
    float phi = rhs / teethJ;
    phi = std::fmod(phi, pitch);
    if (phi < 0.0f) { phi += pitch; }
    return phi;
}

// How badly equation (2) is violated, as a percentage of one tooth pitch.
float jointResidualPercent(float teethI, float teethJ, float alphaIJ, float phiI, float phiJ) {
    float r = teethI * phiI + teethJ * phiJ - ((teethI + teethJ) * alphaIJ + (teethJ - 1.0f) * PI);
    r = std::fmod(r, TWO_PI);
    if (r > PI)  { r -= TWO_PI; }
    if (r < -PI) { r += TWO_PI; }
    // r lives in "tooth phase" radians where 2*PI is exactly one tooth pitch.
    return r / TWO_PI * 100.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Gear mesh
// ---------------------------------------------------------------------------

bool buildGearMesh(const GearSpec& spec, GearMesh& out, std::string& error) {
    const char* label = (spec.label != 0) ? spec.label : "gear";

    if (spec.teeth < 6) {
        error = diag::format("%s: a gear needs at least 6 teeth (got %d)", label, spec.teeth);
        return false;
    }
    if (spec.module <= 0.0f) {
        error = diag::format("%s: module must be positive (got %g)", label, spec.module);
        return false;
    }
    if (spec.thickness <= 0.0f) {
        error = diag::format("%s: thickness must be positive (got %g)", label, spec.thickness);
        return false;
    }

    const int   teeth       = spec.teeth;
    const float module      = spec.module;
    const float pitchRadius = 0.5f * module * teeth;
    const float tipRadius   = pitchRadius + kAddendum * module;
    const float rootRadius  = pitchRadius - kDedendum * module;

    if (rootRadius <= 0.25f * pitchRadius) {
        error = diag::format("%s: too few teeth for a usable root radius", label);
        return false;
    }

    const float pitchAngle = TWO_PI / static_cast<float>(teeth);

    // Involute geometry for this gear.
    const float pressureAngle = toRadians(kPressureAngleDegrees);
    const float baseRadius = pitchRadius * std::cos(pressureAngle);
    const float involuteAtPressureAngle = std::tan(pressureAngle) - pressureAngle;
    const float halfThicknessAtPitch =
        kToothThicknessFactor * PI / static_cast<float>(teeth);
    const float halfAngleAtRoot =
        involuteHalfAngle(rootRadius, baseRadius, halfThicknessAtPitch, involuteAtPressureAngle);

    // The involute only starts at the base circle; below it the flank is radial.
    const bool  hasRadialRoot = rootRadius < baseRadius;
    const float flankStart = hasRadialRoot ? baseRadius : rootRadius;
    const int   pointsPerTooth = 2 * kFlankSamples + 2 * (hasRadialRoot ? 1 : 0) +
                                 (kRootArcSegments - 1);

    // --- 2D tooth profile, ordered by increasing angle ---------------------
    std::vector<ProfilePoint> profile;
    profile.reserve(static_cast<size_t>(teeth) * static_cast<size_t>(pointsPerTooth));

    for (int t = 0; t < teeth; ++t) {
        const float base = static_cast<float>(t) * pitchAngle;

        // Radial piece of the flank when the root is inside the base circle.
        if (hasRadialRoot) {
            ProfilePoint p;
            p.x = rootRadius * std::cos(base - halfAngleAtRoot);
            p.y = rootRadius * std::sin(base - halfAngleAtRoot);
            p.angle = base - halfAngleAtRoot;
            p.arc = 0.0f;
            profile.push_back(p);
        }

        // Left flank, root -> tip: the half angle shrinks as the radius grows,
        // so the angle keeps increasing.
        for (int s = 0; s < kFlankSamples; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(kFlankSamples - 1);
            const float radius = flankStart + (tipRadius - flankStart) * u;
            const float angle = base - involuteHalfAngle(radius, baseRadius, halfThicknessAtPitch,
                                                         involuteAtPressureAngle);
            ProfilePoint p;
            p.x = radius * std::cos(angle);
            p.y = radius * std::sin(angle);
            p.angle = angle;
            p.arc = 0.0f;
            profile.push_back(p);
        }

        // Right flank, tip -> root (the first sample is the tip corner).
        for (int s = kFlankSamples - 1; s >= 0; --s) {
            const float u = static_cast<float>(s) / static_cast<float>(kFlankSamples - 1);
            const float radius = flankStart + (tipRadius - flankStart) * u;
            const float angle = base + involuteHalfAngle(radius, baseRadius, halfThicknessAtPitch,
                                                         involuteAtPressureAngle);
            ProfilePoint p;
            p.x = radius * std::cos(angle);
            p.y = radius * std::sin(angle);
            p.angle = angle;
            p.arc = 0.0f;
            profile.push_back(p);
        }

        if (hasRadialRoot) {
            ProfilePoint p;
            p.x = rootRadius * std::cos(base + halfAngleAtRoot);
            p.y = rootRadius * std::sin(base + halfAngleAtRoot);
            p.angle = base + halfAngleAtRoot;
            p.arc = 0.0f;
            profile.push_back(p);
        }

        // Root arc leading to the next tooth, so the root circle is round
        // instead of a coarse polygon.
        const float gapStart = base + halfAngleAtRoot;
        const float gapEnd   = base + pitchAngle - halfAngleAtRoot;
        for (int s = 1; s < kRootArcSegments; ++s) {
            const float a = gapStart + (gapEnd - gapStart) *
                            (static_cast<float>(s) / static_cast<float>(kRootArcSegments));
            ProfilePoint p;
            p.x = rootRadius * std::cos(a);
            p.y = rootRadius * std::sin(a);
            p.angle = a;
            p.arc = 0.0f;
            profile.push_back(p);
        }
    }

    const size_t pointCount = profile.size();

    // Cumulative arc length around the profile.
    float perimeter = 0.0f;
    for (size_t i = 0; i < pointCount; ++i) {
        profile[i].arc = perimeter;
        const ProfilePoint& a = profile[i];
        const ProfilePoint& b = profile[(i + 1) % pointCount];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        perimeter += std::sqrt(dx * dx + dy * dy);
    }

    // Checker mapping.  One texture repeat holds kCheckerCells cells, so
    // picking "repeatWorld" fixes the world size of a checker cell.  Using the
    // circular pitch keeps the cells about one tooth wide, and snapping the
    // rim to a whole number of repeats keeps the texture seamless where the
    // profile closes.
    const float cellWorld  = PI * module;
    const float repeatWorld = kCheckerCells * cellWorld;
    const float capUvScale = 1.0f / repeatWorld;

    float rimRepeats = std::floor(perimeter / repeatWorld + 0.5f);
    if (rimRepeats < 1.0f) { rimRepeats = 1.0f; }
    const float rimUvScale = rimRepeats / perimeter;

    const float halfThickness = 0.5f * spec.thickness;

    out.vertices.clear();
    out.indices.clear();
    out.vertices.reserve(2 * (1 + pointCount) + 4 * pointCount);
    out.indices.reserve(12 * pointCount);

    // --- helper used to append a vertex ------------------------------------
    struct Pusher {
        std::vector<Vertex>& vertices;
        std::vector<uint16_t>& indices;

        uint16_t add(float px, float py, float pz,
                     float nx, float ny, float nz,
                     float u, float v) {
            Vertex vertex;
            vertex.position[0] = px; vertex.position[1] = py; vertex.position[2] = pz;
            vertex.normal[0]   = nx; vertex.normal[1]   = ny; vertex.normal[2]   = nz;
            vertex.texCoord[0] = u;  vertex.texCoord[1] = v;
            const uint16_t index = static_cast<uint16_t>(vertices.size());
            vertices.push_back(vertex);
            return index;
        }
    };
    Pusher push = { out.vertices, out.indices };

    // --- front and back caps ----------------------------------------------
    const float capNormals[2][3] = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f } };
    const float capZ[2] = { halfThickness, -halfThickness };

    for (int face = 0; face < 2; ++face) {
        const uint16_t centre = push.add(0.0f, 0.0f, capZ[face],
                                         capNormals[face][0], capNormals[face][1], capNormals[face][2],
                                         0.5f, 0.5f);

        std::vector<uint16_t> ring(pointCount);
        for (size_t i = 0; i < pointCount; ++i) {
            const float u = 0.5f + profile[i].x * capUvScale;
            const float v = 0.5f + profile[i].y * capUvScale;
            ring[i] = push.add(profile[i].x, profile[i].y, capZ[face],
                               capNormals[face][0], capNormals[face][1], capNormals[face][2],
                               u, v);
        }

        for (size_t i = 0; i < pointCount; ++i) {
            const size_t j = (i + 1) % pointCount;

            // The radial root wall makes two consecutive profile points share
            // an angle, so the fan triangle from the centre has zero area.
            // Comparing angles (instead of a cross product) detects that
            // exactly, without being fooled by floating point noise.
            float delta = profile[j].angle - profile[i].angle;
            if (delta < -PI) { delta += TWO_PI; }
            if (std::fabs(delta) < 1e-5f) { continue; }

            // Wound counter-clockwise when seen from outside the solid.
            if (face == 0) {
                push.indices.push_back(centre);
                push.indices.push_back(ring[i]);
                push.indices.push_back(ring[j]);
            } else {
                push.indices.push_back(centre);
                push.indices.push_back(ring[j]);
                push.indices.push_back(ring[i]);
            }
        }
    }

    // --- rim ---------------------------------------------------------------
    for (size_t i = 0; i < pointCount; ++i) {
        const size_t j = (i + 1) % pointCount;
        const ProfilePoint& a = profile[i];
        const ProfilePoint& b = profile[j];

        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        float nx = dy;
        float ny = -dx;
        const float len = std::sqrt(nx * nx + ny * ny);
        if (len > 1e-8f) { nx /= len; ny /= len; } else { nx = 0.0f; ny = 0.0f; }

        const float u0 = a.arc * rimUvScale;
        const float u1 = (j == 0) ? rimRepeats : (b.arc * rimUvScale);
        const float v0 = 0.0f;
        const float v1 = spec.thickness * rimUvScale;

        // Outward-facing quad: (top_i, bottom_i, bottom_j) + (top_i, bottom_j, top_j)
        const uint16_t topI    = push.add(a.x, a.y, halfThickness, nx, ny, 0.0f, u0, v1);
        const uint16_t bottomI = push.add(a.x, a.y, -halfThickness, nx, ny, 0.0f, u0, v0);
        const uint16_t bottomJ = push.add(b.x, b.y, -halfThickness, nx, ny, 0.0f, u1, v0);
        const uint16_t topJ    = push.add(b.x, b.y, halfThickness, nx, ny, 0.0f, u1, v1);

        push.indices.push_back(topI);
        push.indices.push_back(bottomI);
        push.indices.push_back(bottomJ);

        push.indices.push_back(topI);
        push.indices.push_back(bottomJ);
        push.indices.push_back(topJ);
    }

    out.pitchRadius   = pitchRadius;
    out.tipRadius     = tipRadius;
    out.rootRadius    = rootRadius;
    out.baseRadius    = baseRadius;
    out.profilePointCount = static_cast<uint32_t>(pointCount);
    out.triangleCount = static_cast<uint32_t>(out.indices.size() / 3);
    return true;
}

// ---------------------------------------------------------------------------
// Gear train: three meshes, correctly timed and centred on the origin.
// ---------------------------------------------------------------------------

bool buildGearTrain(const GearSpec specs[3],
                    const float jointAngleDeg[2],
                    float baseSpeed,
                    GearTrain& out,
                    std::string& error) {
    for (int i = 0; i < 3; ++i) {
        if (!buildGearMesh(specs[i], out.meshes[i], error)) { return false; }
    }

    out.triangleCount = 0;
    for (int i = 0; i < 3; ++i) { out.triangleCount += out.meshes[i].triangleCount; }

    const float alphaAB = toRadians(jointAngleDeg[0]);
    const float alphaBC = toRadians(jointAngleDeg[1]);

    // Centres: gear B sits one pitch-distance from A along alphaAB, and C one
    // pitch-distance from B along alphaBC.  A drives B drives C, so the train
    // is a chain (a closed loop of three external gears could not turn at all).
    float px[3];
    float py[3];
    px[0] = 0.0f;
    py[0] = 0.0f;
    px[1] = px[0] + (out.meshes[0].pitchRadius + out.meshes[1].pitchRadius) * std::cos(alphaAB);
    py[1] = py[0] + (out.meshes[0].pitchRadius + out.meshes[1].pitchRadius) * std::sin(alphaAB);
    px[2] = px[1] + (out.meshes[1].pitchRadius + out.meshes[2].pitchRadius) * std::cos(alphaBC);
    py[2] = py[1] + (out.meshes[1].pitchRadius + out.meshes[2].pitchRadius) * std::sin(alphaBC);

    // Start phases from the joint equation.
    const float teeth[3] = { static_cast<float>(specs[0].teeth),
                             static_cast<float>(specs[1].teeth),
                             static_cast<float>(specs[2].teeth) };
    float phi[3];
    phi[0] = 0.0f;
    phi[1] = solveJointPhase(teeth[0], teeth[1], alphaAB, phi[0]);
    phi[2] = solveJointPhase(teeth[1], teeth[2], alphaBC, phi[1]);

    // Angular speeds follow from (1): Ni*wi + Nj*wj = 0, so neighbouring gears
    // always turn in opposite directions with speed ratio Ni/Nj.
    float omega[3];
    omega[0] = baseSpeed;
    omega[1] = -omega[0] * teeth[0] / teeth[1];
    omega[2] = -omega[1] * teeth[1] / teeth[2];

    // Centre the arrangement on the origin so the camera maths stays simple.
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < 3; ++i) {
        const float r = out.meshes[i].tipRadius;
        minX = std::min(minX, px[i] - r);
        maxX = std::max(maxX, px[i] + r);
        minY = std::min(minY, py[i] - r);
        maxY = std::max(maxY, py[i] + r);
    }
    const float cx = 0.5f * (minX + maxX);
    const float cy = 0.5f * (minY + maxY);

    out.halfWidth  = 0.5f * (maxX - minX);
    out.halfHeight = 0.5f * (maxY - minY);

    float boundingRadius = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float x = px[i] - cx;
        const float y = py[i] - cy;
        out.gears[i].position[0] = x;
        out.gears[i].position[1] = y;
        out.gears[i].phase = phi[i];
        out.gears[i].angularSpeed = omega[i];
        boundingRadius = std::max(boundingRadius, std::sqrt(x * x + y * y) + out.meshes[i].tipRadius);
    }
    out.boundingRadius = boundingRadius;

    out.meshResidualPercent[0] = jointResidualPercent(teeth[0], teeth[1], alphaAB, phi[0], phi[1]);
    out.meshResidualPercent[1] = jointResidualPercent(teeth[1], teeth[2], alphaBC, phi[1], phi[2]);
    return true;
}

// ---------------------------------------------------------------------------
// Checker texture
// ---------------------------------------------------------------------------

bool buildCheckerTexture(int size,
                         int cells,
                         std::vector<unsigned char>& rgba,
                         int& levelCount,
                         std::string& error) {
    if (!isPowerOfTwo(size)) {
        error = diag::format("checker texture size must be a power of two (got %d)", size);
        return false;
    }
    if (cells < 1 || size % cells != 0) {
        error = diag::format("checker texture: %d cells do not divide size %d", cells, size);
        return false;
    }

    const int cellPixels = size / cells;

    // Light / dark squares, in sRGB.  The dark square is slightly blue so the
    // per-gear tint reads clearly on both halves.
    const unsigned char light[3] = { 232, 234, 240 };
    const unsigned char dark[3]  = { 44, 47, 60 };

    rgba.clear();
    rgba.resize(static_cast<size_t>(size) * static_cast<size_t>(size) * 4);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool isLight = (((x / cellPixels) + (y / cellPixels)) % 2) == 0;
            const unsigned char* base = isLight ? light : dark;

            unsigned char* px = &rgba[(static_cast<size_t>(y) * size + x) * 4];
            px[0] = base[0];
            px[1] = base[1];
            px[2] = base[2];
            px[3] = 255;
        }
    }

    levelCount = 1;
    int levelSize = size;
    while (levelSize > 1) {
        const int nextSize = levelSize / 2;
        std::vector<unsigned char> next(static_cast<size_t>(nextSize) * static_cast<size_t>(nextSize) * 4);

        // Levels are stored back to back, so the level being downsampled is the
        // last levelSize^2*4 bytes of the buffer.
        const size_t srcOffset = rgba.size() - static_cast<size_t>(levelSize) * static_cast<size_t>(levelSize) * 4;

        for (int y = 0; y < nextSize; ++y) {
            for (int x = 0; x < nextSize; ++x) {
                for (int c = 0; c < 4; ++c) {
                    unsigned int sum = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const size_t sx = static_cast<size_t>(x * 2 + dx);
                            const size_t sy = static_cast<size_t>(y * 2 + dy);
                            sum += rgba[srcOffset + (sy * static_cast<size_t>(levelSize) + sx) * 4 + static_cast<size_t>(c)];
                        }
                    }
                    next[(static_cast<size_t>(y) * nextSize + x) * 4 + static_cast<size_t>(c)] =
                        static_cast<unsigned char>(sum / 4);
                }
            }
        }

        rgba.insert(rgba.end(), next.begin(), next.end());
        levelSize = nextSize;
        ++levelCount;
    }

    return true;
}

} // namespace vkg
