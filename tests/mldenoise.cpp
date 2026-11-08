// SPDX-FileCopyrightText: 2026 ProCam Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// The learned denoiser (src/mldenoise.*) against its PyTorch model: the CPU
// reference must reproduce the test vector written by train.py --export, and
// the GPU path (half precision) must stay close to the CPU. With a size, also
// times the GPU on a full frame of noise.
//
//   tst_mldenoise denoise.bin denoise-test.bin [width height]
#include "../src/mldenoise.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace MlDenoise;

static bool readTest(const QString &path, Tensor *z, Tensor *expected, float *norm)
{
    QFile file(path);
    char magic[4];
    quint32 h = 0, w = 0;
    if (!file.open(QIODevice::ReadOnly) || file.read(magic, 4) != 4 || std::memcmp(magic, "PCDT", 4) != 0
            || file.read(reinterpret_cast<char *>(&h), 4) != 4 || file.read(reinterpret_cast<char *>(&w), 4) != 4
            || file.read(reinterpret_cast<char *>(norm), 4) != 4)
        return false;
    z->channels = 8;
    expected->channels = 4;
    z->width = expected->width = int(w);
    z->height = expected->height = int(h);
    z->data.resize(std::size_t(8) * w * h);
    expected->data.resize(std::size_t(4) * w * h);
    const qint64 zb = qint64(z->data.size() * 4), eb = qint64(expected->data.size() * 4);
    return file.read(reinterpret_cast<char *>(z->data.data()), zb) == zb
            && file.read(reinterpret_cast<char *>(expected->data.data()), eb) == eb;
}

// Largest absolute difference, relative to the largest reference value.
static double compare(const Tensor &a, const Tensor &b, double *maxAbs)
{
    double worst = 0, range = 0;
    for (std::size_t i = 0; i < a.data.size(); ++i) {
        worst = std::max(worst, double(std::abs(a.data[i] - b.data[i])));
        range = std::max(range, double(std::abs(b.data[i])));
    }
    *maxAbs = worst;
    return worst / std::max(range, 1e-12);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::printf("usage: tst_mldenoise denoise.bin denoise-test.bin [width height]\n");
        return 2;
    }
    Model model;
    QString error;
    if (!loadModel(QString::fromLocal8Bit(argv[1]), &model, &error)) {
        std::printf("FAIL load: %s\n", qPrintable(error));
        return 1;
    }
    Tensor z, expected;
    float norm = 0;
    if (!readTest(QString::fromLocal8Bit(argv[2]), &z, &expected, &norm)) {
        std::printf("FAIL: cannot read test vector\n");
        return 1;
    }
    int failures = 0;
    const Tensor cpu = runCpu(model, z, norm);
    double cpuAbs = 0;
    const double cpuRel = compare(cpu, expected, &cpuAbs);
    const bool cpuOk = cpuRel < 1e-4;
    failures += !cpuOk;
    std::printf("%s CPU vs PyTorch: max error %.3g (relative %.2g), model width %d, %dx%d\n", cpuOk ? "PASS" : "FAIL",
                cpuAbs, cpuRel, model.width, z.width, z.height);

    Tensor gpu;
    GpuStats stats;
    if (!runGpu(model, z, norm, &gpu, &stats, &error)) {
        std::printf("FAIL GPU: %s\n", qPrintable(error));
        return 1;
    }
    double gpuAbs = 0;
    const double gpuRel = compare(gpu, cpu, &gpuAbs);
    // Half precision: activations and weights carry 11 significant bits.
    const bool gpuOk = gpuRel < 1e-2;
    failures += !gpuOk;
    // In noise units (the network's own scale): how much the GPU deviates
    // compared with the noise it removes.
    std::printf("%s GPU vs CPU: max error %.3g (relative %.2g, %.3f noise std), compile %.0f ms\n",
                gpuOk ? "PASS" : "FAIL", gpuAbs, gpuRel, gpuAbs / norm, stats.compileMs);

    if (argc >= 5) {
        const int w = std::atoi(argv[3]) & ~3, h = std::atoi(argv[4]) & ~3;
        Tensor big;
        big.channels = 8;
        big.width = w;
        big.height = h;
        big.data.resize(std::size_t(8) * w * h);
        std::mt19937 random(1);
        std::normal_distribution<float> noise(0.0f, 1.0f);
        for (std::size_t i = 0; i < big.data.size(); ++i)
            big.data[i] = i < big.data.size() / 2 ? 20.0f + noise(random) : 1.0f;
        for (int run = 0; run < 3; ++run) {
            QElapsedTimer timer;
            timer.start();
            if (!runGpu(model, big, norm, &gpu, &stats, &error)) {
                std::printf("FAIL GPU %dx%d: %s\n", w, h, qPrintable(error));
                return 1;
            }
            std::printf("GPU %dx%d: total %lld ms (compile %.0f, upload %.0f, compute %.0f, download %.0f)\n", w, h,
                        timer.elapsed(), stats.compileMs, stats.uploadMs, stats.computeMs, stats.downloadMs);
        }
    }
    return failures ? 1 : 0;
}
