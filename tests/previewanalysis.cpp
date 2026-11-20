// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Viewfinder motion and face measurements (src/previewanalysis.h) on
// synthetic frames.
#include "../src/previewanalysis.h"

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <random>

using namespace PreviewAnalysis;

namespace {

const int W = 480, H = 360;

// A textured scene, sampled at an offset (sub-pixel by bilinear
// interpolation), with an optional bright square moving on its own.
std::vector<uint8_t> frame(double dx, double dy, int squareX, unsigned seed)
{
    static std::vector<float> texture;
    const int tw = W + 64, th = H + 64;
    if (texture.empty()) {
        std::mt19937 random(3);
        std::uniform_real_distribution<float> u(0, 1);
        std::vector<float> coarse((tw / 8 + 2) * (th / 8 + 2));
        for (float &v : coarse) v = u(random);
        texture.resize(tw * th);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const float fx = x / 8.0f, fy = y / 8.0f;
                const int ix = int(fx), iy = int(fy);
                const float ax = fx - ix, ay = fy - iy;
                const int cw = tw / 8 + 2;
                const float a = coarse[iy * cw + ix], b = coarse[iy * cw + ix + 1];
                const float c = coarse[(iy + 1) * cw + ix], d = coarse[(iy + 1) * cw + ix + 1];
                texture[y * tw + x] = 40 + 160 * ((a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay);
            }
    }
    std::mt19937 random(seed);
    std::normal_distribution<float> noise(0, 2.0f);
    std::vector<uint8_t> out(W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double sx = x + 32 - dx, sy = y + 32 - dy;
            const int ix = int(sx), iy = int(sy);
            const double ax = sx - ix, ay = sy - iy;
            const float *t = &texture[iy * tw + ix];
            double v = (t[0] * (1 - ax) + t[1] * ax) * (1 - ay) + (t[tw] * (1 - ax) + t[tw + 1] * ax) * ay;
            if (squareX >= 0 && x >= squareX && x < squareX + 90 && y >= 120 && y < 240) v = 245;
            out[y * W + x] = uint8_t(std::max(0.0, std::min(255.0, v + noise(random))));
        }
    return out;
}

LumaView view(const std::vector<uint8_t> &f) { return LumaView{f.data(), W, H, W, 1}; }

} // namespace

int main()
{
    // Hand shake: 2.5 px right, 1.5 px down per 33 ms frame.
    MotionMeter shake;
    for (int i = 0; i < 12; ++i) {
        const auto f = frame(2.5 * i, 1.5 * i, -1, 10 + i);
        shake.update(view(f), 33LL * i);
    }
    const double expected = std::hypot(2.5, 1.5) / W / 0.033;
    std::printf("shake: measured %.4f, expected %.4f widths/s; subject %.3f\n",
                shake.motion(), expected, shake.subjectMotion());
    assert(std::abs(shake.motion() - expected) < 0.2 * expected);
    assert(shake.subjectMotion() < 0.05);

    // A steady phone: the noise alone reads as almost no motion.
    MotionMeter still;
    for (int i = 0; i < 12; ++i) {
        const auto f = frame(0, 0, -1, 40 + i);
        still.update(view(f), 33LL * i);
    }
    std::printf("still: %.4f widths/s\n", still.motion());
    assert(still.motion() >= 0 && still.motion() < 0.01);

    // A subject crossing a steady frame.
    MotionMeter subject;
    for (int i = 0; i < 12; ++i) {
        const auto f = frame(0, 0, 40 + 30 * i, 70 + i);
        subject.update(view(f), 33LL * i);
    }
    std::printf("subject: motion %.4f, subject share %.3f\n", subject.motion(), subject.subjectMotion());
    assert(subject.subjectMotion() > 0.05);

    // Unknown until two frames arrive; reset forgets.
    MotionMeter fresh;
    assert(fresh.motion() < 0);
    const auto first = frame(0, 0, -1, 1);
    fresh.update(view(first), 0);
    assert(fresh.motion() < 0);
    shake.reset();
    assert(shake.motion() < 0);

    // Face exposure: the largest face's centre.
    std::vector<uint8_t> faces(W * H, 60);
    for (int y = 100; y < 260; ++y)
        for (int x = 150; x < 290; ++x) faces[y * W + x] = 252; // a blown face
    for (int y = 20; y < 50; ++y)
        for (int x = 20; x < 50; ++x) faces[y * W + x] = 120; // a small one
    const FaceExposure exposure = faceExposure(view(faces), {{{20.0f / W, 20.0f / H, 50.0f / W, 50.0f / H}},
                                                             {{150.0f / W, 100.0f / H, 290.0f / W, 260.0f / H}}});
    std::printf("face: count %d, luma %.3f, clipped %.3f\n", exposure.count, exposure.luma, exposure.clipped);
    assert(exposure.count == 2 && exposure.luma > 0.95 && exposure.clipped > 0.9);
    assert(faceExposure(view(faces), {}).luma < 0);
    // A silhouette against a bright window: the centre is the subject; an
    // evenly lit frame or a dark room is not backlit.
    std::vector<uint8_t> backlit(W * H, 235);
    for (int y = H / 4; y < H; ++y)
        for (int x = W / 4; x < 3 * W / 4; ++x) backlit[y * W + x] = 14;
    std::array<float, 4> centre;
    assert(backlitSubject(view(backlit), &centre) && centre[0] == 0.3f && centre[3] == 0.7f);
    std::vector<uint8_t> even(W * H, 120), dark(W * H, 12);
    assert(!backlitSubject(view(even), &centre));
    assert(!backlitSubject(view(dark), &centre));
    // Gyroscope: a hand's tremor (about 1 degree per second, alternating)
    // is never still; a phone on a table (sensor noise of a few hundredths
    // of a degree per second) is still after a second; a nudge starts over,
    // and so does a gap in the readings.
    const double pi = 3.14159265358979323846;
    GyroMeter hand;
    for (int i = 0; i < 100; ++i) {
        const double s = std::sin(2 * pi * 9 * i / 50.0);
        hand.update(0.8 * s, 0.6 * s, 0.2 * s, i * 20);
    }
    std::printf("gyro hand: tilt %.4f roll %.4f rad/s, still %lld ms\n", hand.tiltSpeed(), hand.rollSpeed(),
                hand.stillMs());
    assert(hand.tiltSpeed() > 0.005 && hand.tiltSpeed() < 0.02 && hand.stillMs() == 0);
    GyroMeter table;
    assert(table.tiltSpeed() < 0 && table.stillMs() == 0);
    for (int i = 0; i < 75; ++i) table.update(0.03 * ((i % 3) - 1), 0.02, 0.01 * (i % 2), i * 20);
    std::printf("gyro table: tilt %.4f rad/s, still %lld ms\n", table.tiltSpeed(), table.stillMs());
    assert(table.tiltSpeed() < 0.003 && table.stillMs() >= 1400);
    table.update(5, 0, 0, 1500);
    table.update(5, 0, 0, 1520);
    assert(table.stillMs() == 0);
    table.reset();
    table.update(0, 0, 0, 0);
    table.update(0, 0, 0, 20);
    table.update(0, 0, 0, 2000); // a gap: the sensor paused
    assert(table.stillMs() == 0);
    std::printf("Preview analysis tests passed\n");
    return 0;
}
