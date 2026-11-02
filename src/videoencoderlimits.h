#ifndef VIDEOENCODERLIMITS_H
#define VIDEOENCODERLIMITS_H

#include <QByteArray>
#include <QSize>
#include <QString>
#include <QXmlStreamReader>
#include <QtGlobal>

// What the hardware encoder for one MIME type can take, from Android's
// media_codecs*.xml: the largest frame, and its macroblock budgets (frame
// and per second). Software (c2.android, OMX.google) and secure encoders
// are skipped; a later file's entry for the same codec adds its limits.
struct VideoEncoderLimits
{
    QSize maxSize;
    qint64 maxBlocks = 0;
    qint64 maxBlocksPerSecond = 0;
    int blockSize = 16;

    bool known() const { return maxSize.isValid() && !maxSize.isEmpty(); }

    bool supports(const QSize &size, int fps) const
    {
        if (!known())
            return false;
        // Either orientation, as MediaCodecInfo accepts.
        const bool fits = (size.width() <= maxSize.width() && size.height() <= maxSize.height())
                || (size.height() <= maxSize.width() && size.width() <= maxSize.height());
        const qint64 blocks = qint64((size.width() + blockSize - 1) / blockSize)
                * ((size.height() + blockSize - 1) / blockSize);
        return fits && (maxBlocks <= 0 || blocks <= maxBlocks)
                && (maxBlocksPerSecond <= 0 || blocks * fps <= maxBlocksPerSecond);
    }
};

inline QSize videoEncoderParseSize(const QString &text)
{
    const int x = text.indexOf(QLatin1Char('x'));
    if (x <= 0)
        return QSize();
    bool okWidth = false, okHeight = false;
    const QSize size(text.left(x).toInt(&okWidth), text.mid(x + 1).toInt(&okHeight));
    return okWidth && okHeight ? size : QSize();
}

// The upper end of "range" ("80-14400"), or "value"/"max".
inline qint64 videoEncoderLimitMax(const QXmlStreamAttributes &attributes)
{
    QString text = attributes.value(QStringLiteral("range")).toString();
    if (text.isEmpty())
        text = attributes.value(QStringLiteral("max")).toString();
    if (text.isEmpty())
        text = attributes.value(QStringLiteral("value")).toString();
    const int dash = text.lastIndexOf(QLatin1Char('-'));
    return (dash >= 0 ? text.mid(dash + 1) : text).toLongLong();
}

// Merges one media_codecs XML file into limits for mime; the largest
// hardware encoder wins.
inline void videoEncoderReadLimits(const QByteArray &xml, const QString &mime,
                                   VideoEncoderLimits *limits)
{
    QXmlStreamReader reader(xml);
    bool inEncoders = false;
    bool inCodec = false;
    VideoEncoderLimits codec;
    while (!reader.atEnd()) {
        reader.readNext();
        const QStringRef name = reader.name();
        if (reader.isStartElement()) {
            if (name == QLatin1String("Encoders")) {
                inEncoders = true;
            } else if (inEncoders && name == QLatin1String("MediaCodec")) {
                const QString codecName = reader.attributes().value(QStringLiteral("name")).toString();
                inCodec = reader.attributes().value(QStringLiteral("type")) == mime
                        && !codecName.startsWith(QLatin1String("c2.android."))
                        && !codecName.startsWith(QLatin1String("OMX.google."))
                        && !codecName.contains(QLatin1String(".secure"));
                codec = VideoEncoderLimits();
            } else if (inCodec && name == QLatin1String("Limit")) {
                const QXmlStreamAttributes attributes = reader.attributes();
                const QString limit = attributes.value(QStringLiteral("name")).toString();
                if (limit == QLatin1String("size"))
                    codec.maxSize = videoEncoderParseSize(attributes.value(QStringLiteral("max")).toString());
                else if (limit == QLatin1String("block-count"))
                    codec.maxBlocks = videoEncoderLimitMax(attributes);
                else if (limit == QLatin1String("blocks-per-second"))
                    codec.maxBlocksPerSecond = videoEncoderLimitMax(attributes);
                else if (limit == QLatin1String("block-size"))
                    codec.blockSize = qMax(1, videoEncoderParseSize(
                            attributes.value(QStringLiteral("value")).toString()).width());
            }
        } else if (reader.isEndElement()) {
            if (name == QLatin1String("Encoders")) {
                inEncoders = false;
            } else if (inCodec && name == QLatin1String("MediaCodec")) {
                inCodec = false;
                // Entries that only add measured rates carry no size.
                if (codec.known() && (!limits->known()
                        || qint64(codec.maxSize.width()) * codec.maxSize.height()
                           > qint64(limits->maxSize.width()) * limits->maxSize.height())) {
                    *limits = codec;
                }
            }
        }
    }
}

#endif
