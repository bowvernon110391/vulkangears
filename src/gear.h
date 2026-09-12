#ifndef VKGEARS_GEAR_H
#define VKGEARS_GEAR_H

// Procedural gear geometry for the demo.
//
// A gear is a 2D tooth profile extruded along Z.  The profiles of the three
// gears all use the same "module" (tooth size), which is what makes them mesh:
// two external gears with the same module mesh exactly when their centres are
// pitchRadius1 + pitchRadius2 apart and their rotations stay coupled as
//
//     teeth1 * angle1 + teeth2 * angle2 = constant
//
// buildGearTrain() uses that relation to compute the positions, the start
// phases and the angular speeds, so the three gears form a correctly timed
// gear train (A drives B, B drives C) instead of just spinning near each other.

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
    const char* label;      // used in the console diagnostics
    float       color[3];   // checker tint
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
    GearMesh      meshes[3];
    GearPlacement gears[3];

    // Bounding box of the whole arrangement, centred on the origin.
    float halfWidth;
    float halfHeight;
    float boundingRadius;

    uint32_t triangleCount;

    // Timing error at each of the two joints, as a percentage of one tooth
    // pitch.  ~0 means the teeth interlock exactly.
    float meshResidualPercent[2];
};

// Build the tooth geometry of a single gear.
bool buildGearMesh(const GearSpec& spec, GearMesh& out, std::string& error);

// Build the three meshes plus a correctly meshed, centred placement for them.
// jointAngleDeg[0] is the direction from gear A to gear B, jointAngleDeg[1] the
// direction from gear B to gear C.  baseSpeed is gear A's angular speed in rad/s.
bool buildGearTrain(const GearSpec specs[3],
                    const float jointAngleDeg[2],
                    float baseSpeed,
                    GearTrain& out,
                    std::string& error);

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
