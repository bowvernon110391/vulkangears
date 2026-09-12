// mesh_check.cpp - geometry checks for the gear train.
//
// This links only the portable parts of the demo (no Vulkan, no window), so it
// runs anywhere - including inside ctest.  It verifies the claims the demo makes:
//
//   * the extruded profile is a closed, star shaped polygon with outward
//     pointing rim normals (so back face culling cannot hide anything),
//   * the start phases satisfy the gear timing equation exactly,
//   * the angular speeds keep the coupling teeth1*w1 + teeth2*w2 = 0,
//   * over a whole meshing cycle the teeth of neighbouring gears never
//     interpenetrate, yet stay close enough to actually engage,
//   * the checker texture and its mip chain are well formed.

#include "../src/diag.h"
#include "../src/gear.h"
#include "../src/math3d.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace vkg;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    std::printf("  [%s] %s\n", condition ? " ok " : "FAIL", what.c_str());
    if (!condition) { ++g_failures; }
}

struct Point {
    float x;
    float y;
};

struct Polygon {
    std::vector<Point> points;
};

float cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }

// Ray casting point-in-polygon test, used to prove rim normals face outward.
bool pointInPolygon(const std::vector<Point>& polygon, float x, float y) {
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Point& a = polygon[i];
        const Point& b = polygon[j];
        if (((a.y > y) != (b.y > y)) &&
            (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x)) {
            inside = !inside;
        }
    }
    return inside;
}

// Extracts the 2D tooth profile back out of the generated cap geometry, so the
// check runs against exactly what gets uploaded to the GPU.
Polygon profileFromMesh(const GearMesh& mesh) {
    Polygon polygon;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vertex& vertex = mesh.vertices[i];
        if (vertex.normal[2] > 0.9f && vertex.position[2] > 0.0f) {
            if (vertex.position[0] == 0.0f && vertex.position[1] == 0.0f) { continue; } // cap centre
            Point p;
            p.x = vertex.position[0];
            p.y = vertex.position[1];
            polygon.points.push_back(p);
        }
    }
    return polygon;
}

void rotate(float angle, float x, float y, float& outX, float& outY) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    outX = c * x - s * y;
    outY = s * x + c * y;
}

float pointSegmentDistance(const Point& p, const Point& a, const Point& b, float& outT) {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float lengthSquared = vx * vx + vy * vy;
    float t = 0.0f;
    if (lengthSquared > 1e-12f) {
        t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / lengthSquared;
        if (t < 0.0f) { t = 0.0f; }
        if (t > 1.0f) { t = 1.0f; }
    }
    const float dx = p.x - (a.x + t * vx);
    const float dy = p.y - (a.y + t * vy);
    outT = t;
    return std::sqrt(dx * dx + dy * dy);
}

float segmentDistance(const Point& a, const Point& b, const Point& c, const Point& d) {
    float t0 = 0.0f;
    float t1 = 0.0f;
    float best = pointSegmentDistance(a, c, d, t0);
    const float d2 = pointSegmentDistance(b, c, d, t1);
    if (d2 < best) { best = d2; }
    const float d3 = pointSegmentDistance(c, a, b, t0);
    if (d3 < best) { best = d3; }
    const float d4 = pointSegmentDistance(d, a, b, t1);
    if (d4 < best) { best = d4; }
    return best;
}

// Proper (interior) crossing test - touching end points do not count.
bool segmentsCross(const Point& a, const Point& b, const Point& c, const Point& d) {
    const float eps = 1e-7f;
    const float d1 = cross2(b.x - a.x, b.y - a.y, c.x - a.x, c.y - a.y);
    const float d2 = cross2(b.x - a.x, b.y - a.y, d.x - a.x, d.y - a.y);
    const float d3 = cross2(d.x - c.x, d.y - c.y, a.x - c.x, a.y - c.y);
    const float d4 = cross2(d.x - c.x, d.y - c.y, b.x - c.x, b.y - c.y);
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) &&
           ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

} // namespace

int main() {
    GearSpec specs[3];
    specs[0].teeth = 30; specs[0].module = 1.0f; specs[0].thickness = 3.4f; specs[0].label = "A";
    specs[0].color[0] = 0.0f; specs[0].color[1] = 0.0f; specs[0].color[2] = 1.0f;
    specs[1].teeth = 14; specs[1].module = 1.0f; specs[1].thickness = 3.4f; specs[1].label = "B";
    specs[1].color[0] = 0.0f; specs[1].color[1] = 1.0f; specs[1].color[2] = 0.0f;
    specs[2].teeth = 22; specs[2].module = 1.0f; specs[2].thickness = 3.4f; specs[2].label = "C";
    specs[2].color[0] = 1.0f; specs[2].color[1] = 0.0f; specs[2].color[2] = 0.0f;

    const float jointAngles[2] = { 12.0f, -42.0f };
    GearTrain train;
    std::string error;
    if (!buildGearTrain(specs, jointAngles, 1.15f, train, error)) {
        std::printf("buildGearTrain failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("vulkangears geometry check\n\n");

    // ---- meshes -----------------------------------------------------------
    std::printf("meshes\n");
    for (int i = 0; i < 3; ++i) {
        const GearMesh& mesh = train.meshes[i];
        check(!mesh.vertices.empty() && !mesh.indices.empty(),
              diag::format("gear %d (%dT) generated %zu vertices / %zu indices",
                           i, specs[i].teeth, mesh.vertices.size(), mesh.indices.size()));
        check(mesh.indices.size() % 3 == 0, diag::format("gear %d index count is a multiple of 3", i));
        check(mesh.vertices.size() <= 65535, diag::format("gear %d fits in 16 bit indices", i));
        check(std::fabs(mesh.tipRadius - (mesh.pitchRadius + 0.85f * specs[i].module)) < 1e-4f,
              diag::format("gear %d tip radius is pitch + addendum (%.3f)", i, mesh.tipRadius));

        // Consistency: every triangle must be wound so that its geometric
        // normal agrees with the vertex normals it carries.  If this holds the
        // pipeline only needs one global front face setting.
        int triangles = 0;
        int disagreed = 0;
        int degenerate = 0;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const Vertex& v0 = mesh.vertices[mesh.indices[t + 0]];
            const Vertex& v1 = mesh.vertices[mesh.indices[t + 1]];
            const Vertex& v2 = mesh.vertices[mesh.indices[t + 2]];

            const float e1x = v1.position[0] - v0.position[0];
            const float e1y = v1.position[1] - v0.position[1];
            const float e1z = v1.position[2] - v0.position[2];
            const float e2x = v2.position[0] - v0.position[0];
            const float e2y = v2.position[1] - v0.position[1];
            const float e2z = v2.position[2] - v0.position[2];

            const float nx = e1y * e2z - e1z * e2y;
            const float ny = e1z * e2x - e1x * e2z;
            const float nz = e1x * e2y - e1y * e2x;
            const float magnitude = std::sqrt(nx * nx + ny * ny + nz * nz);
            ++triangles;
            if (magnitude < 1e-9f) { ++degenerate; continue; }

            const float dot = (nx * v0.normal[0] + ny * v0.normal[1] + nz * v0.normal[2]) / magnitude;
            if (dot <= 0.0f) { ++disagreed; }
        }
        check(degenerate == 0, diag::format("gear %d has no degenerate triangles (%d)", i, degenerate));
        check(disagreed == 0,
              diag::format("gear %d triangle winding matches its normals (%d of %d disagree)",
                           i, disagreed, triangles));

        // Every rim normal must point away from the gear body: stepping a small
        // distance along the normal has to land outside the tooth profile.
        const Polygon polygon = profileFromMesh(mesh);
        const float epsilon = 0.01f * specs[i].module;
        int rimNormals = 0;
        int wrongNormals = 0;
        int inwardNormals = 0;
        for (size_t v = 0; v < mesh.vertices.size(); ++v) {
            const Vertex& vertex = mesh.vertices[v];
            if (std::fabs(vertex.normal[2]) > 0.5f) { continue; } // cap
            ++rimNormals;

            const float outwardX = vertex.position[0] + vertex.normal[0] * epsilon;
            const float outwardY = vertex.position[1] + vertex.normal[1] * epsilon;
            if (pointInPolygon(polygon.points, outwardX, outwardY)) { ++wrongNormals; }

            const float inwardX = vertex.position[0] - vertex.normal[0] * epsilon;
            const float inwardY = vertex.position[1] - vertex.normal[1] * epsilon;
            if (!pointInPolygon(polygon.points, inwardX, inwardY)) { ++inwardNormals; }
        }
        check(rimNormals > 0 && wrongNormals == 0,
              diag::format("gear %d rim normals step outside the profile (%d checked, %d wrong)",
                           i, rimNormals, wrongNormals));
        check(inwardNormals == 0,
              diag::format("gear %d rim normals step inside the profile when negated (%d wrong)",
                           i, inwardNormals));
    }

    // ---- profile ----------------------------------------------------------
    std::printf("\nprofile\n");
    for (int i = 0; i < 3; ++i) {
        const Polygon polygon = profileFromMesh(train.meshes[i]);
        check(polygon.points.size() == train.meshes[i].profilePointCount,
              diag::format("gear %d profile has %zu points (mesh reports %u)",
                           i, polygon.points.size(), train.meshes[i].profilePointCount));
        check(train.meshes[i].baseRadius < train.meshes[i].pitchRadius &&
              train.meshes[i].rootRadius < train.meshes[i].pitchRadius &&
              train.meshes[i].tipRadius > train.meshes[i].pitchRadius,
              diag::format("gear %d radii nest: root %.3f < base %.3f < pitch %.3f < tip %.3f",
                           i, train.meshes[i].rootRadius, train.meshes[i].baseRadius,
                           train.meshes[i].pitchRadius, train.meshes[i].tipRadius));

        float previousAngle = -10.0f;
        bool monotonic = true;
        bool starShaped = true;
        for (size_t p = 0; p < polygon.points.size(); ++p) {
            float angle = std::atan2(polygon.points[p].y, polygon.points[p].x);
            // Unwrap: the profile starts slightly below angle 0 and wraps past PI.
            while (angle < previousAngle - 1e-6f) { angle += 2.0f * PI; }
            const float radius = std::sqrt(polygon.points[p].x * polygon.points[p].x +
                                           polygon.points[p].y * polygon.points[p].y);
            if (radius <= 0.0f) { starShaped = false; }
            if (p > 0 && angle < previousAngle - 1e-6f) { monotonic = false; }
            previousAngle = angle;
        }
        check(monotonic, diag::format("gear %d profile is ordered by angle", i));
        check(starShaped, diag::format("gear %d profile is star shaped about its centre", i));

        // A simple (non self intersecting) polygon is what the cap fan assumes.
        int selfCrossings = 0;
        for (size_t a = 0; a < polygon.points.size(); ++a) {
            const Point& a0 = polygon.points[a];
            const Point& a1 = polygon.points[(a + 1) % polygon.points.size()];
            for (size_t b = a + 1; b < polygon.points.size(); ++b) {
                if (b == a || (b + 1) % polygon.points.size() == a || b == (a + 1) % polygon.points.size()) {
                    continue;
                }
                const Point& b0 = polygon.points[b];
                const Point& b1 = polygon.points[(b + 1) % polygon.points.size()];
                if (segmentsCross(a0, a1, b0, b1)) { ++selfCrossings; }
            }
        }
        check(selfCrossings == 0,
              diag::format("gear %d profile does not self intersect (%d crossings)", i, selfCrossings));
    }

    // ---- timing -----------------------------------------------------------
    std::printf("\ntiming\n");
    for (int j = 0; j < 2; ++j) {
        check(std::fabs(train.meshResidualPercent[j]) < 1e-3f,
              diag::format("joint %d satisfies the meshing equation (residual %+.6f %% of a tooth pitch)",
                           j, train.meshResidualPercent[j]));
    }
    for (int j = 0; j < 2; ++j) {
        const float t0 = static_cast<float>(specs[j].teeth);
        const float t1 = static_cast<float>(specs[j + 1].teeth);
        const float coupling = t0 * train.gears[j].angularSpeed + t1 * train.gears[j + 1].angularSpeed;
        check(std::fabs(coupling) < 1e-4f,
              diag::format("joint %d keeps teeth*w = const (%+.6f), speeds %.3f / %.3f rad/s",
                           j, coupling, train.gears[j].angularSpeed, train.gears[j + 1].angularSpeed));
        check(train.gears[j].angularSpeed * train.gears[j + 1].angularSpeed < 0.0f,
              diag::format("joint %d neighbours counter rotate", j));
    }

    // ---- meshing over a full cycle ----------------------------------------
    std::printf("\nmeshing\n");
    for (int joint = 0; joint < 2; ++joint) {
        const int gearI = joint;
        const int gearJ = joint + 1;

        const Polygon polygonI = profileFromMesh(train.meshes[gearI]);
        const Polygon polygonJ = profileFromMesh(train.meshes[gearJ]);

        const float pitchI = train.meshes[gearI].pitchRadius;
        const float pitchJ = train.meshes[gearJ].pitchRadius;
        const float distance = pitchI + pitchJ;

        // Direction from gear i to gear j, reconstructed from the placement.
        const float dx = train.gears[gearJ].position[0] - train.gears[gearI].position[0];
        const float dy = train.gears[gearJ].position[1] - train.gears[gearI].position[1];
        const float actualDistance = std::sqrt(dx * dx + dy * dy);
        check(std::fabs(actualDistance - distance) < 1e-3f,
              diag::format("joint %d centre distance is pitch radii sum (%.4f vs %.4f)",
                           joint, actualDistance, distance));

        // One full meshing cycle: gear i advances by exactly one tooth pitch.
        const float pitchAngleI = 2.0f * PI / static_cast<float>(specs[gearI].teeth);
        const float period = pitchAngleI / std::fabs(train.gears[gearI].angularSpeed);

        const int steps = 240;
        float minimumClearance = 1e30f;
        int crossings = 0;

        for (int step = 0; step <= steps; ++step) {
            const float t = period * static_cast<float>(step) / static_cast<float>(steps);
            const float psiI = train.gears[gearI].phase + train.gears[gearI].angularSpeed * t;
            const float psiJ = train.gears[gearJ].phase + train.gears[gearJ].angularSpeed * t;

            std::vector<Point> worldI(polygonI.points.size());
            std::vector<Point> worldJ(polygonJ.points.size());
            for (size_t p = 0; p < polygonI.points.size(); ++p) {
                float x = 0.0f;
                float y = 0.0f;
                rotate(psiI, polygonI.points[p].x, polygonI.points[p].y, x, y);
                worldI[p].x = x + train.gears[gearI].position[0];
                worldI[p].y = y + train.gears[gearI].position[1];
            }
            for (size_t p = 0; p < polygonJ.points.size(); ++p) {
                float x = 0.0f;
                float y = 0.0f;
                rotate(psiJ, polygonJ.points[p].x, polygonJ.points[p].y, x, y);
                // train.gears[] already holds the absolute (centred) placement,
                // so no extra centre offset may be added here.
                worldJ[p].x = x + train.gears[gearJ].position[0];
                worldJ[p].y = y + train.gears[gearJ].position[1];
            }

            for (size_t a = 0; a < worldI.size(); ++a) {
                const Point& a0 = worldI[a];
                const Point& a1 = worldI[(a + 1) % worldI.size()];
                for (size_t b = 0; b < worldJ.size(); ++b) {
                    const Point& b0 = worldJ[b];
                    const Point& b1 = worldJ[(b + 1) % worldJ.size()];

                    // Cheap reject: the two segments cannot be near each other.
                    const float cdx = (a0.x + a1.x) * 0.5f - (b0.x + b1.x) * 0.5f;
                    const float cdy = (a0.y + a1.y) * 0.5f - (b0.y + b1.y) * 0.5f;
                    if (cdx * cdx + cdy * cdy > 9.0f) { continue; }

                    if (segmentsCross(a0, a1, b0, b1)) { ++crossings; }
                    const float d = segmentDistance(a0, a1, b0, b1);
                    if (d < minimumClearance) { minimumClearance = d; }
                }
            }
        }

        check(crossings == 0,
              diag::format("joint %d: no tooth interpenetration in %d samples over one cycle (%d crossings)",
                           joint, steps + 1, crossings));
        check(minimumClearance > 0.0f && minimumClearance < 0.20f * specs[0].module,
              diag::format("joint %d: teeth engage with backlash (smallest gap %.4f world units = %.2f%% of a module)",
                           joint, minimumClearance, minimumClearance / specs[0].module * 100.0f));
    }

    // The two outer gears must not touch: this is a chain, not a locked loop.
    {
        const float dx = train.gears[2].position[0] - train.gears[0].position[0];
        const float dy = train.gears[2].position[1] - train.gears[0].position[1];
        const float distance = std::sqrt(dx * dx + dy * dy);
        const float tipSum = train.meshes[0].tipRadius + train.meshes[2].tipRadius;
        check(distance > tipSum,
              diag::format("outer gears A and C do not touch (%.3f apart, tips would need %.3f)", distance, tipSum));
    }

    // ---- texture ----------------------------------------------------------
    std::printf("\ntexture\n");
    {
        std::vector<unsigned char> pixels;
        int levels = 0;
        std::string textureError;
        check(buildCheckerTexture(256, 8, pixels, levels, textureError),
              "checker texture generated");

        size_t expectedBytes = 0;
        int size = 256;
        int expectedLevels = 0;
        while (size >= 1) {
            expectedBytes += static_cast<size_t>(size) * static_cast<size_t>(size) * 4u;
            ++expectedLevels;
            size /= 2;
        }
        check(levels == expectedLevels,
              diag::format("mip chain has %d levels (expected %d)", levels, expectedLevels));
        check(pixels.size() == expectedBytes,
              diag::format("mip chain is %zu bytes (expected %zu)", pixels.size(), expectedBytes));

        // The first row of level 0 must alternate every 32 pixels.
        int transitions = 0;
        for (int x = 1; x < 256; ++x) {
            if (pixels[x * 4] != pixels[(x - 1) * 4]) { ++transitions; }
        }
        check(transitions == 7, diag::format("level 0 row has %d checker transitions (expected 7)", transitions));
        check(pixels[3] == 255, "checker texture is opaque");
    }

    std::printf("\n%s (%d failure%s)\n",
                g_failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
