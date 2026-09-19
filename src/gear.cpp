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
    // Evaluated in double.  The terms here are tooth counts multiplied by angles,
    // so they reach ~100 while the phase has to be accurate to a small fraction
    // of a tooth pitch; in float that is a cancellation problem, and the error
    // it leaves behind accumulates along a long train, which is exactly what a
    // random one of a dozen gears exercises.
    const double i = teethI;
    const double j = teethJ;
    const double alpha = alphaIJ;
    const double phi = phiI;

    const double rhs = (i + j) * alpha + (j - 1.0) * PI - i * phi;
    const double pitch = TWO_PI / j;
    double solved = std::fmod(rhs / j, pitch);
    if (solved < 0.0) { solved += pitch; }
    return static_cast<float>(solved);
}

// How badly equation (2) is violated, as a percentage of one tooth pitch.
float jointResidualPercent(float teethI, float teethJ, float alphaIJ, float phiI, float phiJ) {
    const double i = teethI;
    const double j = teethJ;
    const double r = i * static_cast<double>(phiI) + j * static_cast<double>(phiJ) -
                     ((i + j) * static_cast<double>(alphaIJ) + (j - 1.0) * PI);
    double reduced = std::fmod(r, TWO_PI);
    if (reduced > PI)  { reduced -= TWO_PI; }
    if (reduced < -PI) { reduced += TWO_PI; }
    // reduced lives in "tooth phase" radians where 2*PI is exactly one pitch.
    return static_cast<float>(reduced / TWO_PI * 100.0);
}

} // namespace

// ---------------------------------------------------------------------------
// Gear mesh
// ---------------------------------------------------------------------------

bool buildGearMesh(const GearSpec& spec, GearMesh& out, std::string& error) {
    const std::string label = spec.label.empty() ? std::string("gear") : spec.label;

    if (spec.teeth < 6) {
        error = diag::format("%s: a gear needs at least 6 teeth (got %d)",
                             label.c_str(), spec.teeth);
        return false;
    }
    if (spec.module <= 0.0f) {
        error = diag::format("%s: module must be positive (got %g)",
                             label.c_str(), spec.module);
        return false;
    }
    if (spec.thickness <= 0.0f) {
        error = diag::format("%s: thickness must be positive (got %g)",
                             label.c_str(), spec.thickness);
        return false;
    }

    const int   teeth       = spec.teeth;
    const float module      = spec.module;
    const float pitchRadius = 0.5f * module * teeth;
    const float tipRadius   = pitchRadius + kAddendum * module;
    const float rootRadius  = pitchRadius - kDedendum * module;

    if (rootRadius <= 0.25f * pitchRadius) {
        error = diag::format("%s: too few teeth for a usable root radius", label.c_str());
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
// Gear train: N meshes, correctly timed and centred on the origin.
//
// The chain is built by walking from gear 0 outwards.  Everything about gear i+1
// follows from gear i and the tooth counts: where its centre is, what phase it
// starts at, and how fast it turns.  That is what makes an idler an idler -- with
// equal tooth counts the speed ratio is 1 and only the direction changes -- and
// it is also why a chain of a dozen gears is still a sensible object.
// ---------------------------------------------------------------------------

bool buildGearTrain(const GearSpec* specs,
                    int count,
                    float* jointAngleDeg,
                    float baseSpeed,
                    GearTrain& out,
                    std::string& error) {
    if (count < 2) {
        error = diag::format("a train needs at least 2 gears (got %d)", count);
        return false;
    }

    // One module across the train is the condition for meshing at all, so a
    // mismatch is caught here rather than producing gears that nearly touch.
    for (int i = 1; i < count; ++i) {
        if (std::fabs(specs[i].module - specs[0].module) > 1e-6f) {
            const std::string name = specs[i].label.empty() ? std::string("gear") : specs[i].label;
            const std::string first = specs[0].label.empty() ? std::string("gear 0") : specs[0].label;
            error = diag::format("%s: module %g differs from %s's %g, so the teeth cannot mesh",
                                 name.c_str(), specs[i].module,
                                 first.c_str(), specs[0].module);
            return false;
        }
    }

    out.meshes.assign(static_cast<size_t>(count), GearMesh());
    out.gears.assign(static_cast<size_t>(count), GearPlacement());
    out.meshResidualPercent.assign(static_cast<size_t>(count - 1), 0.0f);
    out.jointRatios.assign(static_cast<size_t>(count - 1), 1.0f);

    for (int i = 0; i < count; ++i) {
        if (!buildGearMesh(specs[i], out.meshes[i], error)) { return false; }
    }

    out.triangleCount = 0;
    for (int i = 0; i < count; ++i) { out.triangleCount += out.meshes[i].triangleCount; }

    // Centres, walked outwards from the first gear.  Each new gear sits one
    // pitch-distance from the previous one along that joint's direction, which
    // is exactly the condition for the two pitch circles to touch.
    std::vector<float> px(static_cast<size_t>(count), 0.0f);
    std::vector<float> py(static_cast<size_t>(count), 0.0f);
    std::vector<float> alpha(static_cast<size_t>(count > 0 ? count - 1 : 0), 0.0f);
    for (int i = 1; i < count; ++i) {
        alpha[i - 1] = toRadians(jointAngleDeg[i - 1]);
        px[i] = px[i - 1] + (out.meshes[i - 1].pitchRadius + out.meshes[i].pitchRadius) *
                            std::cos(alpha[i - 1]);
        py[i] = py[i - 1] + (out.meshes[i - 1].pitchRadius + out.meshes[i].pitchRadius) *
                            std::sin(alpha[i - 1]);
    }

    // Start phases from the joint equation, and speeds from the coupling.
    std::vector<float> teeth(static_cast<size_t>(count), 0.0f);
    std::vector<float> phi(static_cast<size_t>(count), 0.0f);
    std::vector<float> omega(static_cast<size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
        teeth[i] = static_cast<float>(specs[i].teeth);
    }
    phi[0] = 0.0f;
    omega[0] = baseSpeed;
    for (int i = 1; i < count; ++i) {
        phi[i] = solveJointPhase(teeth[i - 1], teeth[i], alpha[i - 1], phi[i - 1]);
        // Ni*wi + Nj*wj = 0, so neighbouring gears always turn in opposite
        // directions with a speed ratio of Ni/Nj.  Equal tooth counts give
        // |wj| == |wi|, which is the idler case.
        omega[i] = -omega[i - 1] * teeth[i - 1] / teeth[i];
    }

    // Centre the arrangement on the origin so the camera maths stays simple.
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < count; ++i) {
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
    for (int i = 0; i < count; ++i) {
        const float x = px[i] - cx;
        const float y = py[i] - cy;
        out.gears[i].position[0] = x;
        out.gears[i].position[1] = y;
        out.gears[i].phase = phi[i];
        out.gears[i].angularSpeed = omega[i];
        boundingRadius = std::max(boundingRadius, std::sqrt(x * x + y * y) + out.meshes[i].tipRadius);
    }
    out.boundingRadius = boundingRadius;

    // Report what was built: the timing error at each joint, the ratio at each
    // joint, and the structure of the whole train.
    out.stats = GearTrainStats();
    out.stats.gearCount = count;    out.stats.overallRatio = (omega[0] != 0.0f) ? omega[count - 1] / omega[0] : 1.0f;
    for (int i = 0; i < count; ++i) {
        const float speed = std::fabs(omega[i]);
        if (i == 0 || speed < out.stats.slowestSpeed) { out.stats.slowestSpeed = speed; }
        if (i == 0 || speed > out.stats.fastestSpeed) { out.stats.fastestSpeed = speed; }
    }
    for (int i = 0; i < count - 1; ++i) {
        out.meshResidualPercent[i] =
            jointResidualPercent(teeth[i], teeth[i + 1], alpha[i], phi[i], phi[i + 1]);

        // The physical speed ratio of this joint: how much faster the next gear
        // turns than this one.  It is the reciprocal of the tooth-count ratio,
        // because meshing into a *smaller* gear speeds the next one up.  Taking
        // it from the speeds rather than the tooth counts means the sign of the
        // ratio cannot be got wrong without contradicting omega above.
        const float speedRatio = (omega[i] != 0.0f)
                                     ? std::fabs(omega[i + 1] / omega[i])
                                     : 1.0f;
        out.jointRatios[i] = speedRatio;

        // "Idler" is the exact case, so it is compared exactly: equal tooth
        // counts, not merely a ratio close to 1.
        if (specs[i + 1].teeth == specs[i].teeth) {
            ++out.stats.idlerCount;
        } else if (speedRatio > 1.0f) {
            ++out.stats.speedUpCount;
        } else {
            ++out.stats.slowDownCount;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Random train generation
// ---------------------------------------------------------------------------

namespace {

// A small deterministic generator, written out rather than taken from <random>.
//
// std::mt19937 would do, but its *distributions* are not portable: the standard
// leaves the algorithm for uniform_int_distribution unspecified, so one seed can
// produce different trains on libstdc++, libc++ and MSVC.  A train that cannot be
// reproduced from its seed is a train whose screenshot cannot be explained, and
// it would make the geometry tests platform-dependent.  This is a 32 bit
// xorshift: a few lines, identical everywhere.
class Rng {
public:
    explicit Rng(uint32_t seed) : state_(seed != 0u ? seed : 0x9e3779b9u) {}

    uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // Uniform in [0, bound); `bound` must be positive.
    int below(int bound) { return static_cast<int>(next() % static_cast<uint32_t>(bound)); }

    int between(int low, int high) { return low + below(high - low + 1); }

    float unit() { return static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f; }

    float range(float low, float high) { return low + (high - low) * unit(); }

private:
    uint32_t state_;
};

float toDegrees(float radians) { return radians * 180.0f / PI; }

// HSV to RGB, h in [0,1) wrapping, s and v in [0,1].  Used for the per-gear
// tint: hues are spread evenly around the wheel (see below) and this turns each
// one into a colour the checker texture can be tinted by.
void hueToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h -= std::floor(h); // wrap into [0,1)
    const float sector = h * 6.0f;
    const int   index  = static_cast<int>(sector) % 6;
    const float f      = sector - std::floor(sector);
    const float p      = v * (1.0f - s);
    const float q      = v * (1.0f - s * f);
    const float t      = v * (1.0f - s * (1.0f - f));
    switch (index) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// How often a stage is a *step* rather than an idler.  Most stages are idlers,
// which is what keeps a long train turning at a watchable speed while still
// looking like a mechanism rather than a row of identical cogs.
const float kStepChance = 0.30f;

// How far the running speed ratio may wander from 1:1 before the generator
// steers back.  Without a limit, 15 stages of random stepping can reach 100:1 or
// 1:100, and neither is something anyone can watch: the late gears are either
// frozen or strobing.
const float kRatioCeiling = 3.0f;

} // namespace

bool makeGearTrain(int requestedGears,
                   uint32_t seed,
                   float baseSpeed,
                   std::vector<GearSpec>& specs,
                   std::vector<float>& jointAngleDeg,
                   GearTrain& out,
                   std::string& error) {
    Rng rng(seed);

    // 0 (or nothing) means "you choose"; anything else is clamped rather than
    // rejected, so a silly value on a command line still produces a train.
    int count = 0;
    if (requestedGears > 0) {
        count = std::max(kMinTrainGears, std::min(kMaxTrainGears, requestedGears));
    } else {
        count = rng.between(kMinTrainGears, kMaxTrainGears);
    }

    const float module = 1.0f;
    const float baseThickness = 3.4f;

    // The tooth count most gears share.  Idlers reuse it exactly, and steps are
    // drawn relative to it, which is what makes the result read as one train
    // rather than as a bag of unrelated gears.
    const int baseTeeth = rng.between(18, 34);

    specs.assign(static_cast<size_t>(count), GearSpec());
    jointAngleDeg.assign(static_cast<size_t>(count - 1), 0.0f);

    // Hues are spread evenly rather than drawn at random: over a dozen gears,
    // random hues collide, and then two unrelated gears look like one shaft.
    const float hueOffset = rng.range(0.0f, 1.0f);

    float ratio = 1.0f; // |speed of the latest gear| relative to the first

    // Which stages are steps rather than idlers is decided up front, one draw
    // per stage, rather than stage by stage inside the tooth loop.  The reason
    // is the guarantee below: choosing the stages as a set lets the "at least
    // one step" rule pick the least likely stage anywhere in the train, whereas
    // deciding in order could only ever append a step onto the end.  That is
    // the difference between a train that looks generated and one that looks
    // like a row of idlers with an afterthought bolted to it.
    std::vector<char> isStep(static_cast<size_t>(count), 0);
    {
        int steps = 0;
        int lowestKeyStage = 1;
        float lowestKey = 2.0f;
        for (int i = 1; i < count; ++i) {
            const float key = rng.unit();
            if (key < kStepChance) {
                isStep[static_cast<size_t>(i)] = 1;
                ++steps;
            }
            if (key < lowestKey) {
                lowestKey = key;
                lowestKeyStage = i;
            }
        }
        // A train of a dozen gears all the same size is a legitimate gear train
        // -- that is exactly what an idler is -- but it demonstrates nothing
        // about ratios, and it is the one outcome that makes the demo look
        // broken.  So the least likely idler is promoted.
        if (steps == 0 && count > 1) {
            isStep[static_cast<size_t>(lowestKeyStage)] = 1;
        }
    }

    for (int i = 0; i < count; ++i) {
        GearSpec& spec = specs[i];
        spec.module = module;
        spec.thickness = baseThickness * rng.range(0.85f, 1.15f);
        spec.color[0] = 0.0f;
        spec.color[1] = 0.0f;
        spec.color[2] = 0.0f;

        if (i == 0) {
            spec.teeth = baseTeeth;
        } else if (!isStep[static_cast<size_t>(i)]) {
            // An idler: the same tooth count, so the ratio is exactly 1:1 and
            // the only thing this stage does is reverse the direction.
            spec.teeth = specs[i - 1].teeth;
        } else {
            // A step stage.  Meshing into a smaller gear makes the next one turn
            // faster; into a larger one, slower.  Which way to go is free while
            // the ratio is moderate, and forced when it is not -- otherwise the
            // train walks away from a speed you can see.
            const float magnitude = std::fabs(ratio);
            bool speedUp;
            if (magnitude > kRatioCeiling) {
                speedUp = false;               // too fast already: use a larger gear
            } else if (magnitude < 1.0f / kRatioCeiling) {
                speedUp = true;                // too slow already: use a smaller gear
            } else {
                speedUp = rng.unit() < 0.5f;   // room either way, so choose freely
            }

            const int   from  = specs[i - 1].teeth;
            const float factor = rng.range(1.4f, 2.0f);
            int teeth = speedUp ? static_cast<int>(static_cast<float>(from) / factor)
                                : static_cast<int>(static_cast<float>(from) * factor);
            teeth = std::max(kMinGearTeeth, std::min(kMaxGearTeeth, teeth));

            // Clamping can land back on the previous count, which would quietly
            // turn a step into an idler.  Move off it where there is room; at the
            // very edge of the range an idler is the honest outcome.
            if (teeth == from) {
                if (from + 1 <= kMaxGearTeeth) {
                    teeth = from + 1;
                } else if (from - 1 >= kMinGearTeeth) {
                    teeth = from - 1;
                }
            }
            spec.teeth = teeth;
        }

        ratio = (i == 0) ? 1.0f
                         : ratio * static_cast<float>(spec.teeth) /
                                       static_cast<float>(specs[i - 1].teeth);

        float r = 0.0f, g = 0.0f, b = 0.0f;
        hueToRgb(hueOffset + static_cast<float>(i) / static_cast<float>(count),
                 0.55f, 0.95f, r, g, b);
        spec.color[0] = r;
        spec.color[1] = g;
        spec.color[2] = b;
        spec.label = diag::format("gear %d (%dT)", i, spec.teeth);
    }

    // --- layout ------------------------------------------------------------
    //
    // The train is walked outwards, choosing each joint direction so that the
    // result folds up rather than running away in a line.
    //
    // The search is over a ring of candidate directions.  A candidate is only
    // usable if the new gear clears every gear it does not mesh with --
    // neighbours are touching by design, everything else must not be -- and
    // among the usable ones the winner is the one that lands closest to where
    // the train already is.  Maximising clearance instead would be the obvious
    // rule and the wrong one: the direction pointing away from the whole train
    // always clears everything, so the train would stretch into a straight line
    // and the framing would shrink the gears to make room for it.
    const int   kDirectionCandidates = 72;   // 5 degree steps
    const float kMinClearance = 0.15f;       // world units, so tips do not touch

    std::vector<float> px(static_cast<size_t>(count), 0.0f);
    std::vector<float> py(static_cast<size_t>(count), 0.0f);
    std::vector<float> pitch(static_cast<size_t>(count), 0.0f);
    std::vector<float> tip(static_cast<size_t>(count), 0.0f);
    for (int i = 0; i < count; ++i) {
        // The same radii buildGearMesh will arrive at, computed here because the
        // layout needs them before any mesh exists.
        pitch[i] = 0.5f * module * static_cast<float>(specs[i].teeth);
        tip[i]   = pitch[i] + 0.85f * module;
    }

    float heading = rng.range(0.0f, TWO_PI);
    int crowdedJoints = 0;

    for (int i = 1; i < count; ++i) {
        const float distance = pitch[i - 1] + pitch[i];

        // Where the train is so far, so a new gear can be pulled toward it.
        float centreX = 0.0f;
        float centreY = 0.0f;
        for (int j = 0; j < i; ++j) {
            centreX += px[j];
            centreY += py[j];
        }
        centreX /= static_cast<float>(i);
        centreY /= static_cast<float>(i);

        float bestScore = -1e30f;
        float bestAngle = heading;
        bool haveUsable = false;
        float fallbackAngle = heading;
        float fallbackClearance = -1e30f;

        for (int c = 0; c < kDirectionCandidates; ++c) {
            const float angle = heading + TWO_PI * static_cast<float>(c) /
                                                    static_cast<float>(kDirectionCandidates);
            const float nx = px[i - 1] + distance * std::cos(angle);
            const float ny = py[i - 1] + distance * std::sin(angle);

            // Clearance from every gear this one does not mesh with.  Gear i-1
            // is the mesh partner and is expected to touch.
            float clearance = 1e30f;
            for (int j = 0; j < i - 1; ++j) {
                const float dx = nx - px[j];
                const float dy = ny - py[j];
                const float gap = std::sqrt(dx * dx + dy * dy) - (tip[i] + tip[j]);
                if (gap < clearance) { clearance = gap; }
            }

            // Track the most roomy candidate as a fallback for the case where
            // the train has closed in on itself and nothing is strictly free.
            // Squeezing a gear into the least-bad spot beats refusing to place
            // it, and the tests assert that in practice this never happens.
            if (clearance > fallbackClearance) {
                fallbackClearance = clearance;
                fallbackAngle = angle;
            }

            if (i > 1 && clearance < kMinClearance) { continue; }

            // Compact: stay near the middle of what has been placed already.
            const float dx = nx - centreX;
            const float dy = ny - centreY;
            const float spread = std::sqrt(dx * dx + dy * dy);
            // A slight preference for carrying on the way we were going, so the
            // train curves instead of turning back on itself at every joint.
            const float straightness = std::cos(angle - heading);
            const float score = -spread + 0.25f * straightness;

            if (!haveUsable || score > bestScore) {
                bestScore = score;
                bestAngle = angle;
                haveUsable = true;
            }
        }

        if (!haveUsable) {
            bestAngle = fallbackAngle;
            ++crowdedJoints;
        }

        jointAngleDeg[i - 1] = toDegrees(bestAngle);
        px[i] = px[i - 1] + distance * std::cos(bestAngle);
        py[i] = py[i - 1] + distance * std::sin(bestAngle);
        heading = bestAngle;
    }

    // The directions are settled, so the real thing can be built on top of them.
    if (!buildGearTrain(&specs[0], count, &jointAngleDeg[0], baseSpeed, out, error)) {
        return false;
    }
    out.stats.crowdedJoints = crowdedJoints;
    return true;
}

std::string toothProfile(const GearSpec* specs, int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        if (i != 0) { text += "-"; }
        text += diag::format("%d", specs[i].teeth);
    }
    return text;
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
