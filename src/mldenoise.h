// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <array>
#include <vector>

// Learned denoiser for merged bursts (tools/mltrain/train.py): a small U-Net
// on the four CFA planes after the temporal merge, in place of the merge's
// spatial (frequency domain) denoise. It sees the planes at the rendered
// brightness plus the expected noise per pixel, both divided by the image's
// mean noise std, and predicts the clean planes.
//
// The network runs on the GPU (OpenGL ES 3.1 compute shaders on a headless
// EGL context, half precision, one generated shader per layer with the
// weights as constants). runCpu() is the float32 reference the GPU path and
// the PyTorch model are tested against; it is too slow for photos.
namespace MlDenoise {

struct Conv {
    int out = 0, in = 0, kernel = 0; // kernel 3 (padding 1), 2 (stride 2) or 1
    std::vector<float> weight;       // [out][in][kernel][kernel]
    std::vector<float> bias;         // [out]
};

struct Model {
    int width = 0;
    // enc0.0, enc0.2, down1, enc1.1, down2, mid.1, mid.3, up2, dec1.0, up1,
    // dec0.0, out (train.py EXPORT_LAYERS).
    std::vector<Conv> convs;
    QString path;
    bool valid() const { return convs.size() == 12; }
};

bool loadModel(const QString &path, Model *model, QString *error);
// denoise.bin from the first directory that has a valid one, loaded once.
const Model *modelFrom(const QStringList &dirs);

// Planar float images: channel c at data[c * width * height].
struct Tensor {
    int channels = 0, width = 0, height = 0;
    std::vector<float> data;
    float *plane(int c) { return data.data() + std::size_t(c) * width * height; }
    const float *plane(int c) const { return data.data() + std::size_t(c) * width * height; }
};

// The image's noise relative to the single-frame noise model
// (variance = scale * x + offset per CFA phase) and the resulting mean noise
// std, as train.py's noise_estimate().
struct NoiseEstimate {
    double ratio = 0, level = 0;
};
NoiseEstimate estimateNoise(const Tensor &planes, const std::array<double, 4> &scale,
                            const std::array<double, 4> &offset);

// Network input z (8 channels) for planes x (4 channels, white 1): x and its
// expected noise std, times `gain`, divided by norm = level * gain.
Tensor networkInput(const Tensor &planes, const std::array<double, 4> &scale, const std::array<double, 4> &offset,
                    const NoiseEstimate &noise, double gain, double *norm);

// The network's output for z: the denoised planes in z's units times norm
// (the rendered brightness). Width and height must be multiples of 4.
Tensor runCpu(const Model &model, const Tensor &z, double norm);

struct GpuStats {
    double uploadMs = 0, computeMs = 0, downloadMs = 0, compileMs = 0;
};
// Same on the GPU. False with *error if no ES 3.1 context or a shader fails.
bool runGpu(const Model &model, const Tensor &z, double norm, Tensor *out, GpuStats *stats, QString *error);
// Whether a GPU context can be made (tried once).
bool gpuAvailable(QString *error = nullptr);

// Denoises planes in place (white 1, black 0): estimate the noise, run the
// network on the GPU at gain, convert back. info receives timing and noise.
bool denoisePlanes(const Model &model, Tensor *planes, const std::array<double, 4> &scale,
                   const std::array<double, 4> &offset, double gain, QJsonObject *info, QString *error);

}
