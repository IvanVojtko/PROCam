// SPDX-FileCopyrightText: 2026 RAWfish Contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Trains the RAISR 2x upscaling filters used by zoomed RAW renders from a
// folder of sharp photos (JPEG/PNG). Every 8th image is held out and reported
// as PSNR of bilinear vs learned upscaling. --eval scores existing filters
// on the --test images instead of training.
//
//   raisr-train [--prescale 1.5] [--augment] [--stride 2] [--test image]... <images...> <out.bin>
//   raisr-train --eval filters.bin [--prescale 1.5] --test image...
#include "../../src/raisr.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QStringList>
#include <QTextStream>
#include <cmath>

namespace {

double srgbToLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

// Brightness in the domain the renderer upscales: square root of linear luma.
Raisr::Image loadTone(const QString &path, double prescale)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read().convertToFormat(QImage::Format_RGB888);
    Raisr::Image tone;
    if (image.isNull()) return tone;
    static double lut[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) lut[i] = srgbToLinear(i / 255.0);
        ready = true;
    }
    tone.width = image.width();
    tone.height = image.height();
    tone.data.resize(std::size_t(tone.width) * tone.height);
    for (int y = 0; y < tone.height; ++y) {
        const uchar *p = image.constScanLine(y);
        for (int x = 0; x < tone.width; ++x, p += 3) {
            const double luma = 0.25 * (lut[p[0]] + 2 * lut[p[1]] + lut[p[2]]);
            tone.data[std::size_t(y) * tone.width + x] = float(std::sqrt(luma));
        }
    }
    // Downscaling first averages away JPEG and demosaic softness, so the
    // targets are crisper than the camera's own pixels.
    if (prescale > 1.0)
        tone = Raisr::resize(tone, int(tone.width / prescale), int(tone.height / prescale));
    return tone;
}

Raisr::Image flipped(const Raisr::Image &in, bool horizontal, bool transpose)
{
    Raisr::Image out;
    out.width = transpose ? in.height : in.width;
    out.height = transpose ? in.width : in.height;
    out.data.resize(in.data.size());
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            int sx = transpose ? y : x, sy = transpose ? x : y;
            if (horizontal) sx = in.width - 1 - sx;
            out.data[std::size_t(y) * out.width + x] = in.data[std::size_t(sy) * in.width + sx];
        }
    return out;
}

double psnr(const Raisr::Image &a, const Raisr::Image &b)
{
    double sum = 0;
    std::size_t n = 0;
    const int margin = 8;
    for (int y = margin; y < a.height - margin; ++y)
        for (int x = margin; x < a.width - margin; ++x) {
            const double d = a.data[std::size_t(y) * a.width + x] - b.data[std::size_t(y) * b.width + x];
            sum += d * d;
            ++n;
        }
    return n ? 10 * std::log10(1.0 / (sum / n + 1e-20)) : 0;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QStringList args = app.arguments().mid(1);
    double prescale = 1.0;
    int stride = 2;
    bool augment = false;
    QStringList files, held;
    QString evaluate;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--prescale") && i + 1 < args.size()) prescale = args[++i].toDouble();
        else if (args[i] == QLatin1String("--stride") && i + 1 < args.size()) stride = std::max(1, args[++i].toInt());
        else if (args[i] == QLatin1String("--augment")) augment = true;
        else if (args[i] == QLatin1String("--test") && i + 1 < args.size()) held.append(args[++i]);
        else if (args[i] == QLatin1String("--eval") && i + 1 < args.size()) evaluate = args[++i];
        else files.append(args[i]);
    }
    const auto report = [&](const Raisr::Filters &filters) {
        for (const QString &file : held) {
            const Raisr::Image tone = loadTone(file, prescale);
            if (tone.data.empty()) continue;
            const Raisr::Image low = Raisr::resize(tone, tone.width / 2, tone.height / 2);
            const Raisr::Image bilinear = Raisr::resize(low, tone.width, tone.height);
            const Raisr::Image learned = Raisr::upscale(low, tone.width, tone.height, filters);
            out << "held out " << file << ": bilinear " << psnr(bilinear, tone) << " dB, RAISR "
                << psnr(learned, tone) << " dB\n";
        }
    };
    if (!evaluate.isEmpty()) {
        Raisr::Filters filters;
        std::string error;
        if (!Raisr::load(QFile::encodeName(evaluate).toStdString(), &filters, &error)) {
            out << QString::fromStdString(error) << "\n";
            return 1;
        }
        report(filters);
        return 0;
    }
    if (files.size() < 2) {
        out << "usage: raisr-train [--prescale 1.5] [--augment] [--stride 2] [--test image]... <images...> <out.bin>\n"
               "       raisr-train --eval filters.bin [--prescale 1.5] --test image...\n";
        return 2;
    }
    const QString output = files.takeLast();
    QStringList training;
    for (int i = 0; i < files.size(); ++i) (files.size() >= 4 && i % 8 == 7 ? held : training).append(files[i]);

    QElapsedTimer timer;
    timer.start();
    Raisr::Trainer trainer(2.0);
    {
        std::vector<Raisr::Image> sample;
        for (int i = 0; i < training.size() && sample.size() < 4; i += std::max(1, training.size() / 4)) {
            Raisr::Image tone = loadTone(training[i], prescale);
            if (!tone.data.empty()) sample.push_back(std::move(tone));
        }
        trainer.calibrate(sample);
    }
    out << "strength thresholds " << trainer.hash().strengthThresholds[0] << " "
        << trainer.hash().strengthThresholds[1] << "\n";
    out.flush();
    for (const QString &file : training) {
        const Raisr::Image tone = loadTone(file, prescale);
        if (tone.data.empty()) {
            out << "skipped (unreadable) " << file << "\n";
            continue;
        }
        for (int t = 0; t < (augment ? 4 : 1); ++t)
            trainer.add(t == 0 ? tone : flipped(tone, t & 1, t & 2), stride);
        out << "trained " << file << " " << tone.width << "x" << tone.height << " ("
            << timer.elapsed() / 1000 << " s)\n";
        out.flush();
    }
    const Raisr::Filters filters = trainer.solve();
    std::string error;
    if (!Raisr::save(QFile::encodeName(output).toStdString(), filters, &error)) {
        out << QString::fromStdString(error) << "\n";
        return 1;
    }
    out << "wrote " << output << "\n";
    report(filters);
    return 0;
}
