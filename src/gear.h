#ifndef VKGEARS_GEAR_H
#define VKGEARS_GEAR_H

// Procedural gear geometry for the demo.
//
// A gear is a 2D tooth profile extruded along Z.  Every gear in a train uses the
// same "module" (tooth size), which is what makes them mesh: two external gears
// with the same module mesh exactly when their centres are pitchRadius1 +
// pitchRadius2 apart and their rotations stay coupled as
//
//     teeth1 * angle1 + teeth2 * angle2 = constant
//
// buildGearTrain() uses that relation to compute the positions, the start
// phases and the angular speeds, so the gears form a correctly timed chain
// (each drives the next) instead of just spinning near each other.
//
// The train is a chain rather than a closed loop, and it is drawn with a mix of
// ratios.  A mesh between two gears of *equal* tooth count is an idler: the
// ratio is exactly 1:1 and all it does is reverse the direction of rotation,
// which is why a train of a dozen gears can still turn at a sensible speed.  A
// mesh against a smaller gear speeds the next one up, and against a larger one
// slows it down; makeGearTrain() draws mostly idlers with the occasional step so
// that the speed wanders within a band instead of collapsing to nothing.

#include <cstdint>
#include <string>
#include <vector>

namespace vkg {

struct Vertex {
    float position[3];
    float normal[3];
    float texCoord[2];
};

struct GearSpec {
    int         teeth;      // number of teeth
    float       module;     // tooth size; pitch radius = module * teeth / 2
    float       thickness;  // extrusion depth along Z
    std::string label;      // used in the console diagnostics
    float       color[3];   // checker tint
};

// How a train was generated, and what it adds up to.  Recorded rather than
// recomputed because the console report is the only place the structure of a
// random train is visible, and the ratios are the point of it.
struct GearTrainStats {
    int   gearCount;
    int   idlerCount;        // stages with equal tooth counts (ratio 1:1)
    int   speedUpCount;      // stages into a smaller gear (|ratio| > 1)
    int   slowDownCount;     // stages into a larger gear (|ratio| < 1)
    float overallRatio;      // last gear speed / first gear speed, signed
    float slowestSpeed;      // rad/s, smallest |angular speed| in the train
    float fastestSpeed;      // rad/s, largest |angular speed|
    int   crowdedJoints;     // joints placed without full clearance (want 0)
};

struct GearMesh {
    std::vector<Vertex>   vertices;
    std::vector<uint16_t> indices;
    float                 pitchRadius;
    float                 tipRadius;
    float                 rootRadius;
    float                 baseRadius;        // involute base circle
    uint32_t              profilePointCount; // points in the 2D tooth profile
    uint32_t              triangleCount;
};

struct GearPlacement {
    float position[2];    // centre in the gear plane (train is centred on 0,0)
    float phase;          // rotation at t = 0, radians
    float angularSpeed;   // signed rad/s
};

struct GearTrain {
    std::vector<GearMesh>      meshes;
    std::vector<GearPlacement> gears;

    // Timing error at each joint, as a percentage of one tooth pitch.  ~0 means
    // the teeth interlock exactly.  There is one fewer entry than there are
    // gears, because each joint is between a gear and the next.
    std::vector<float> meshResidualPercent;

    // Speed ratio of each joint: how much faster gear i+1 turns than gear i.
    // 1.00 always means an idler.  The sign is dropped, because every joint
    // reverses direction and that carries no information beyond the fact.
    std::vector<float> jointRatios;

    GearTrainStats stats;

    // Bounding box of the whole arrangement, centred on the origin.
    float halfWidth;
    float halfHeight;
    float boundingRadius;

    uint32_t triangleCount;

    GearTrain() : halfWidth(0.0f), halfHeight(0.0f), boundingRadius(0.0f), triangleCount(0) {
        stats.gearCount = 0;
        stats.idlerCount = 0;
        stats.speedUpCount = 0;
        stats.slowDownCount = 0;
        stats.overallRatio = 1.0f;
        stats.slowestSpeed = 0.0f;
        stats.fastestSpeed = 0.0f;
        stats.crowdedJoints = 0;
    }
};

// Bounds on a single gear's tooth count.  The floor is what the involute profile
// can draw without the root radius collapsing.  The ceiling is chosen for looks,
// not for a hard limit: the profile costs 14 points per tooth and the extrusion
// puts six vertices on each, so 16 bit indices would allow roughly 780 teeth
// before a single gear ran out of vertex indices.  Forty-eight is already the
// largest gear in a train of fifteen.
const int kMinGearTeeth = 8;
const int kMaxGearTeeth = 48;

// Bounds on how many gears a generated train may have.
const int kMinTrainGears = 3;
const int kMaxTrainGears = 15;

// Build the tooth geometry of a single gear.
bool buildGearMesh(const GearSpec& spec, GearMesh& out, std::string& error);

// Build a chain of meshes plus a correctly timed, centred placement for them.
//
// `specs` holds `count` gears in driving order and all must share one module.
// jointAngleDeg[i] is the requested direction from gear i to gear i+1; the
// generator below may choose a different one, in which case the value is
// updated in place so the caller can see where the train ended up.
//
// baseSpeed is gear 0's angular speed in rad/s.  Every other gear's speed
// follows from the tooth counts.
bool buildGearTrain(const GearSpec* specs,
                    int count,
                    float* jointAngleDeg,
                    float baseSpeed,
                    GearTrain& out,
                    std::string& error);

// Generate a train at random: how many gears, how many teeth each has, how they
// are arranged, and what colour and thickness they are.
//
// Deterministic for a given `seed`, so a train can be reproduced and a
// screenshot of one can be described exactly.  `requestedGears` of 0 means "pick
// a count in [kMinTrainGears, kMaxTrainGears]"; anything else is clamped into
// that range, so a bad value from a command line cannot produce a silly train.
bool makeGearTrain(int requestedGears,
                   uint32_t seed,
                   float baseSpeed,
                   std::vector<GearSpec>& specs,
                   std::vector<float>& jointAngleDeg,
                   GearTrain& out,
                   std::string& error);

// The tooth counts of a train, as "30-14-22", for the console report.
std::string toothProfile(const GearSpec* specs, int count);

// Procedurally generate a mipmapped checkerboard, RGBA8, as an sRGB texture.
// All mip levels are appended to "rgba" (level 0 first) and levelCount receives
// how many levels were produced.
bool buildCheckerTexture(int size,
                         int cells,
                         std::vector<unsigned char>& rgba,
                         int& levelCount,
                         std::string& error);

} // namespace vkg

#endif // VKGEARS_GEAR_H
