#include "jfg/renderer/widescreen.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

bool near(const double a, const double b) {
    return std::abs(a - b) < 1e-6;
}

struct Plane {
    float x, y, z, d;
};

// The game's plane construction (track.c func_800158BC) for one triangle in
// camera space, without its inverse-view transform.
Plane plane(const std::array<float, jfg::kTrackFrustumFloatCount>& t,
            const std::size_t triangle) {
    const float* v = t.data() + triangle * 9U;
    const float ax = v[0], ay = v[1], az = v[2];
    const float bx = v[3], by = v[4], bz = v[5];
    const float cx = v[6], cy = v[7], cz = v[8];
    float nx = (bz - cz) * ay + by * (cz - az) + cy * (az - bz);
    float ny = (bx - cx) * az + bz * (cx - ax) + cz * (ax - bx);
    float nz = (by - cy) * ax + bx * (cy - ay) + cx * (ay - by);
    const float scale = 1.0F / std::sqrt(nx * nx + ny * ny + nz * nz);
    nx *= scale;
    ny *= scale;
    nz *= scale;
    return {nx, ny, nz, -(ax * nx + ay * ny + az * nz)};
}

bool inside(const std::array<float, jfg::kTrackFrustumFloatCount>& t,
            const float x, const float y, const float z) {
    for (std::size_t index = 0U; index < 3U; ++index) {
        const Plane p = plane(t, index);
        if (p.x * x + p.y * y + p.z * z + p.d <= 0.0F)
            return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace jfg;

    // Aspect target for the normal 320x240 mode follows the window.
    require(near(widescreen_aspect_target(1920U, 1080U, 320U, 240U, 320U, 240U),
                 16.0 / 9.0),
        "16:9 window widens a 4:3 framebuffer to 16:9");
    require(near(widescreen_aspect_target(3440U, 1440U, 320U, 240U, 320U, 240U),
                 3440.0 / 1440.0),
        "ultrawide window");
    require(near(widescreen_aspect_target(640U, 480U, 320U, 240U, 320U, 240U),
                 4.0 / 3.0),
        "4:3 window keeps the original picture");
    require(near(widescreen_aspect_target(1000U, 1000U, 320U, 240U, 320U, 240U),
                 4.0 / 3.0),
        "narrow window never narrows the picture");
    require(near(widescreen_aspect_target(7680U, 1080U, 320U, 240U, 320U, 240U),
                 7680.0 / 1080.0),
        "triple 16:9 is not capped");
    require(near(widescreen_aspect_target(16000U, 1000U, 320U, 240U, 320U, 240U),
                 kMaximumWidescreenAspect),
        "extreme windows are capped");
    require(near(widescreen_aspect_target(5120U, 1440U, 320U, 240U, 320U, 240U,
                                          16.0 / 9.0),
                 16.0 / 9.0),
        "max aspect 16:9 pillarboxes 32:9");
    require(near(widescreen_aspect_target(1920U, 1080U, 320U, 240U, 320U, 240U,
                                          21.0 / 9.0),
                 16.0 / 9.0),
        "max aspect above the window changes nothing");
    require(near(widescreen_aspect_target(0U, 0U, 320U, 240U, 320U, 240U),
                 4.0 / 3.0),
        "unknown window keeps the source shape");

    // The game's own widescreen mode scans 240 lines into 180: already 16:9.
    require(near(widescreen_aspect_target(1920U, 1080U, 320U, 240U, 320U, 180U),
                 4.0 / 3.0),
        "stock widescreen at 16:9 needs no extra widening");
    require(near(widescreen_aspect_target(2560U, 1080U, 320U, 240U, 320U, 180U),
                 (2560.0 / 1080.0) * 0.75),
        "stock widescreen on ultrawide widens the remainder");

    require(widescreen_horizontal_extent(4.0 / 3.0, 320U, 240U) == 1.0F,
        "no extent at 4:3");
    require(near(widescreen_horizontal_extent(16.0 / 9.0, 320U, 240U), 4.0 / 3.0),
        "16:9 extent");
    require(widescreen_horizontal_extent(16.0 / 9.0, 0U, 240U) == 1.0F,
        "invalid framebuffer");

    // Clip codes: x beyond w is rejected only past the widened extent.
    require(widescreen_clip_code(1.2F, 0.0F, 1.0F, 1.0F) == 0x01U,
        "4:3 rejects right of w");
    require(widescreen_clip_code(-1.2F, 0.0F, 1.0F, 1.0F) == 0x02U,
        "4:3 rejects left of -w");
    require(widescreen_clip_code(1.2F, 0.0F, 1.0F, 4.0F / 3.0F) == 0U,
        "16:9 keeps geometry beside the 4:3 frustum");
    require(widescreen_clip_code(1.4F, 0.0F, 1.0F, 4.0F / 3.0F) == 0x01U,
        "16:9 still rejects beyond the wide frustum");
    require(widescreen_clip_code(0.0F, 1.2F, 1.0F, 4.0F / 3.0F) == 0x04U,
        "vertical clipping is unchanged");
    require(widescreen_clip_code(0.0F, 0.0F, 0.0F, 4.0F / 3.0F) == 0x10U,
        "near clipping is unchanged");

    // Track frustum: synthetic triangles with the same layout as the game's
    // (near plane, right side, left side; apex behind the camera, -Z forward).
    std::array<float, kTrackFrustumFloatCount> frustum{
        40.0F, 0.0F, 20.0F,  -40.0F, 0.0F, 20.0F,  -40.0F, 80.0F, 20.0F,
        0.0F, 0.0F, 20.0F,   100.0F, 50.0F, -80.0F,  100.0F, -50.0F, -80.0F,
        0.0F, 0.0F, 20.0F,   -100.0F, -50.0F, -80.0F, -100.0F, 50.0F, -80.0F,
    };
    // Slope 1.0: at z = -980 (1000 ahead of the apex), x = +-1000 is the edge.
    require(inside(frustum, 0.0F, 0.0F, -500.0F), "centre is visible");
    require(inside(frustum, 900.0F, 0.0F, -980.0F), "inside right edge");
    require(!inside(frustum, 1200.0F, 0.0F, -980.0F), "outside right edge");
    require(!inside(frustum, -1200.0F, 0.0F, -980.0F), "outside left edge");
    require(!inside(frustum, 0.0F, 0.0F, 100.0F), "behind the near plane");
    const auto original = frustum;
    widen_track_frustum(frustum, 1.0F);
    require(frustum == original, "extent 1 leaves the frustum unchanged");
    widen_track_frustum(frustum, 4.0F / 3.0F);
    require(inside(frustum, 1200.0F, 0.0F, -980.0F), "widened right edge");
    require(inside(frustum, -1200.0F, 0.0F, -980.0F), "widened left edge");
    require(!inside(frustum, 1400.0F, 0.0F, -980.0F), "still bounded");
    require(!inside(frustum, 0.0F, 0.0F, 100.0F), "near plane unchanged");
    for (std::size_t index = 0U; index < frustum.size(); ++index) {
        if (index % 3U != 0U)
            require(frustum[index] == original[index], "only X is scaled");
    }
    return 0;
}
