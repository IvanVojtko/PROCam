// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// RAISR-style learned upscaling (Romano, Isidoro, Milanfar 2016, "RAISR:
// Rapid and Accurate Image Super Resolution"): a cheap bilinear upscale is
// refined by one of a few hundred small learned filters, chosen per pixel by
// the local edge angle, strength and coherence. Brightness only; a few
// megabytes of filters, ~100 multiply-adds per pixel. Training and inference
// share the hashing code below.
#pragma once
#include <array>
#include <string>
#include <vector>

namespace Raisr {

const int Angles = 24, Strengths = 3, Coherences = 3;
const int Buckets = Angles * Strengths * Coherences;
const int Patch = 9; // filter size
const int Taps = Patch * Patch;

struct Filters {
    std::array<float, Strengths - 1> strengthThresholds{{0.0f, 0.0f}};
    std::array<float, Coherences - 1> coherenceThresholds{{0.25f, 0.5f}};
    std::vector<float> weights; // Buckets * Taps; empty = not loaded
    bool valid() const { return weights.size() == std::size_t(Buckets) * Taps; }
};

struct Image {
    int width = 0, height = 0;
    std::vector<float> data;
    float at(int x, int y) const;
};

// Bilinear resampling to (width, height), pixel centres aligned.
Image resize(const Image &source, int width, int height);
// Bucket of every pixel of `image` (the bilinear upscale).
std::vector<int> hashImage(const Image &image, const Filters &filters, std::vector<float> *strength = nullptr);
// Upscales `source` to (width, height): bilinear, then the learned filters
// (without valid filters: bilinear only).
Image upscale(const Image &source, int width, int height, const Filters &filters);

bool load(const std::string &path, Filters *filters, std::string *error);
bool save(const std::string &path, const Filters &filters, std::string *error);

// Least-squares training from (high resolution) images: each is downscaled
// by `factor`, upscaled back bilinearly, and the filters learn to restore it.
class Trainer {
public:
    explicit Trainer(double factor = 2.0) : m_factor(factor) {}
    // First pass over a few images: strength quantiles for the hash.
    void calibrate(const std::vector<Image> &images);
    void add(const Image &highResolution, int sampleStride = 2);
    Filters solve(double regularisation = 1e-3) const;
    const Filters &hash() const { return m_hash; }

private:
    double m_factor;
    Filters m_hash;
    std::vector<double> m_normal = std::vector<double>(std::size_t(Buckets) * Taps * Taps, 0.0);
    std::vector<double> m_target = std::vector<double>(std::size_t(Buckets) * Taps, 0.0);
    std::vector<double> m_count = std::vector<double>(Buckets, 0.0);
};

}
