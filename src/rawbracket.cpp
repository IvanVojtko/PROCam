// SPDX-License-Identifier: BSD-3-Clause
#include "rawbracket.h"
#include "rawbracketmath.h"
#include "imageadjustments.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QImage>
#include <QImageWriter>
#include <QSaveFile>
#include <QTransform>
#include <QElapsedTimer>
#include <QDebug>
#include <limits>
#include <stdexcept>

namespace RawBracket {
namespace {
double rational(const QJsonValue &v) {
    if (v.isDouble()) return v.toDouble();
    const QJsonArray r=v.toArray();
    if (r.size()!=2 || r[1].toDouble()==0) throw std::runtime_error("Invalid RAW colour transform");
    return r[0].toDouble()/r[1].toDouble();
}
QJsonObject load(const QString &path, Plane &p) {
    QFile metadata(path);
    if (!metadata.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read bracket metadata");
    const QJsonObject m=QJsonDocument::fromJson(metadata.readAll()).object();
    p.width=m["width"].toInt(); p.height=m["height"].toInt(); p.white=m["white_level"].toDouble();
    const QStringList cfas={"RGGB","GRBG","GBRG","BGGR"};
    p.cfa=cfas.indexOf(m["cfa"].toString());
    const QJsonArray black=m["black_level_pattern"].toArray();
    const int bits=m["bits_per_sample"].toInt(m["format"].toString()=="RAW10"?10:16);
    const int stride=m["row_stride"].toInt();
    if (p.width<4 || p.height<4 || p.cfa<0 || black.size()!=4
            || (bits!=10 && bits!=16) || !std::isfinite(p.white) || p.white<=0 || p.white>65535
            || qint64(stride)<(bits==10?((qint64(p.width)+3)/4)*5:qint64(p.width)*2))
        throw std::runtime_error("Invalid RAW bracket geometry or levels");
    for (int k=0;k<4;++k) {
        p.black[k]=black[k].toDouble(-1);
        if (!std::isfinite(p.black[k]) || p.black[k]<0 || p.black[k]>=p.white)
            throw std::runtime_error("Invalid RAW bracket black levels");
    }
    QFile raw(m["raw_path"].toString());
    if (!raw.open(QIODevice::ReadOnly) || raw.size()<qint64(stride)*p.height)
        throw std::runtime_error("Missing or truncated RAW bracket frame");
    p.pixels.resize(std::size_t(p.width)*p.height);
    for (int y=0;y<p.height;++y) {
        const QByteArray row=raw.read(stride);
        if (row.size()!=stride) throw std::runtime_error("Truncated RAW bracket row");
        const auto *b=reinterpret_cast<const unsigned char *>(row.constData());
        for (int x=0;x<p.width;++x) {
            const int g=x/4, o=x%4;
            p.pixels[std::size_t(y)*p.width+x]=bits==10
                    ? (uint16_t(b[g*5+o])<<2)|((b[g*5+4]>>(o*2))&3)
                    : uint16_t(b[x*2])|(uint16_t(b[x*2+1])<<8);
        }
    }
    return m;
}
double number(const QJsonObject &m, const char *key) {
    const auto v=m[key]; return v.isString()?v.toString().toDouble():v.toDouble();
}
}
bool render(const QString &target, const QStringList &paths, int quality, QString *error) {
    try {
        if (paths.size()!=2) throw std::runtime_error("RAW bracket needs two frames");
        QElapsedTimer timer; timer.start();
        Plane l,s;
        QJsonObject lm=load(paths[0],l), sm=load(paths[1],s);
        if (l.width!=s.width || l.height!=s.height || l.cfa!=s.cfa
                || lm["camera_id"].toString().isEmpty() || lm["camera_id"]!=sm["camera_id"]
                || lm["active_array"]!=sm["active_array"]
                || lm["physical_camera_id"]!=sm["physical_camera_id"])
            throw std::runtime_error("RAW bracket camera or geometry changed");
        const double lt=number(lm,"exposure_time_ns"), st=number(sm,"exposure_time_ns");
        const double li=number(lm,"iso"), si=number(sm,"iso");
        if (lt<=0 || st<=0 || li<=0 || si<=0) throw std::runtime_error("Missing actual RAW exposure metadata");
        const double nominal=(lt/st)*(li/si);
        if (!std::isfinite(nominal) || nominal<=1) throw std::runtime_error("HAL did not produce a shorter bracket exposure");
        for (const QJsonObject &m : {lm,sm}) {
            const double imageTime=number(m,"image_timestamp_ns"), resultTime=number(m,"sensor_timestamp_ns");
            if (imageTime<=0 || resultTime<=0 || imageTime!=resultTime)
                throw std::runtime_error("RAW image/result timestamp mismatch");
        }
        const double gap=number(sm,"sensor_timestamp_ns")-number(lm,"sensor_timestamp_ns");
        if (gap<=0) throw std::runtime_error("RAW bracket frames are out of order");
        const Estimate gain=estimate(l,s,nominal);
        const QJsonArray gains=lm["color_correction_gains"].toArray();
        const QJsonArray transform=lm["capture_color_transform"].toArray();
        // Require real colour metadata instead of silently substituting an identity matrix.
        if (gains.size()!=4 || transform.size()!=9) throw std::runtime_error("RAW bracket needs colour gains and transform");
        const RGB wb{{gains[0].toDouble(),(gains[1].toDouble()+gains[2].toDouble())*.5,gains[3].toDouble()}};
        std::array<double,9> matrix;
        for (int i=0;i<9;++i) { matrix[i]=rational(transform[i]); if (!std::isfinite(matrix[i])) throw std::runtime_error("Invalid colour matrix"); }
        RGB luma;
        for (int c=0;c<3;++c) {
            if (!(wb[c]>0) || !std::isfinite(wb[c])) throw std::runtime_error("Invalid white balance");
            luma[c]=wb[c]*(.2126*matrix[c]+.7152*matrix[3+c]+.0722*matrix[6+c]);
        }
        const double display=lm["render_exposure"].toString("1.0").toDouble();
        if (!(display>0) || !std::isfinite(display)) throw std::runtime_error("Invalid render exposure");
        QImage image(l.width,l.height,QImage::Format_RGB888);
        if (image.isNull()) throw std::runtime_error("Not enough memory for bracket output");
        for (int y=0;y<l.height;++y) {
            uchar *row=image.scanLine(y);
            for (int x=0;x<l.width;++x) {
                bool clipped=false;
                RGB rgb=reconstruct(l,l,x,y,&clipped);
                if (clipped) {
                    RGB shorter=reconstruct(s,l,x,y,nullptr);
                    for (double &v:shorter) v*=gain.gain;
                    rgb=merge(rgb,shorter,true,wb,luma);
                }
                RGB out;
                for (int c=0;c<3;++c) out[c]=display*std::max(0.0,
                        matrix[c*3]*rgb[0]*wb[0]+matrix[c*3+1]*rgb[1]*wb[1]+matrix[c*3+2]*rgb[2]*wb[2]);
                const double shoulder=1+std::max(out[0],std::max(out[1],out[2]));
                for (int c=0;c<3;++c) row[x*3+c]=uchar(qBound(0,int(std::round(255*srgb(out[c]/shoulder))),255));
            }
        }
        const int rotation=lm["bracket_rotation"].toInt();
        if (rotation) image=image.transformed(QTransform().rotate(rotation));
        if (lm["bracket_mirror"].toBool()) image=image.mirrored(true,false);
        ImageAdjustments::apply(&image,1.0,lm["color_temperature_requested_kelvin"].toInt(),lm["color_tint_requested"].toInt());
        QJsonArray frames;
        for (QJsonObject m : {lm,sm}) {
            m.remove("raw_path"); m.remove("_original_exif"); frames.append(m);
        }
        lm.remove("raw_path"); lm.remove("_original_exif");
        lm["bracket_combined"]=true; lm["bracket_count"]=2; lm["bracket_frames"]=frames;
        lm["merge_algorithm"]="rawbracket_strict_clip_and_luminance_gated_wb_white_lighten";
        lm["alignment_enabled"]=false; lm["motion_rejection_enabled"]=false;
        lm["nominal_gain"]=nominal; lm["applied_gain"]=gain.gain;
        lm["gain_refinement_accepted"]=gain.accepted; lm["gain_samples"]=gain.samples;
        lm["gain_relative_mad"]=gain.samples>=64?QJsonValue(gain.scatter):QJsonValue(QJsonValue::Null);
        lm["gain_reason"]=gain.accepted?"accepted":gain.samples<64?"insufficient_signal_or_overlap":"inconsistent_ratios";
        lm["bracket_actual_separation_ev"]=std::log2(nominal);
        lm["bracket_frame_gap_ms"]=gap/1e6;
        const double wanted=-number(sm,"bracket_ev");
        lm["bracket_requested_separation_ev"]=wanted;
        lm["bracket_range_limited"]=std::log2(nominal)+.05<wanted;
        lm["width"]=image.width(); lm["height"]=image.height();
        lm["pixels_rotated"]=true;
        lm["render_engine"]="rawbracket_cpu";
        lm["merge_ms"]=double(timer.elapsed());
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open merged JPEG");
        QImageWriter writer(&output,"JPG"); writer.setQuality(quality);
        writer.setProgressiveScanWrite(lm["progressive_jpeg"].toBool());
        if (!writer.write(image) || !output.commit()) throw std::runtime_error("Cannot save merged JPEG");
        const QFileInfo info(target);
        QSaveFile sidecar(info.absolutePath()+"/"+info.completeBaseName()+".json");
        const QByteArray json=QJsonDocument(lm).toJson();
        if (!sidecar.open(QIODevice::WriteOnly) || sidecar.write(json)!=json.size() || !sidecar.commit()) {
            QFile::remove(target);
            throw std::runtime_error("Cannot save merged RAW metadata");
        }
        qDebug()<<"raw-bracket merge"<<"gain"<<gain.gain<<"refined"<<gain.accepted<<"gap_ms"<<gap/1e6<<"total_ms"<<timer.elapsed();
        return true;
    } catch (const std::exception &e) { *error=QString::fromUtf8(e.what()); return false; }
}
}
