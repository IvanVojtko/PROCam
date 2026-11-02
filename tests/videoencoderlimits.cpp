#include "../src/videoencoderlimits.h"
#undef NDEBUG
#include <cassert>

// The Jolla Phone 2's /vendor/etc/media_codecs_c2.xml encoders (abridged).
static const char jp2[] = R"(<MediaCodecs>
    <Decoders>
        <MediaCodec name="c2.mtk.hevc.decoder" type="video/hevc">
            <Limit name="size" min="64x64" max="4096x2176" />
        </MediaCodec>
    </Decoders>
    <Encoders>
        <MediaCodec name="c2.mtk.avc.encoder" type="video/avc" >
            <Limit name="size" min="160x128" max="2560x1440" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="80-14400" />
            <Limit name="blocks-per-second" range="80-432000" />
            <Limit name="bitrate" range="1-60000000" />
        </MediaCodec>
        <MediaCodec name="c2.mtk.avc.encoder.secure" type="video/avc" >
            <Limit name="size" min="160x128" max="1920x1088" />
        </MediaCodec>
        <MediaCodec name="c2.mtk.hevc.encoder" type="video/hevc" >
            <Limit name="size" min="160x128" max="2560x1440" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="80-14400" />
            <Limit name="blocks-per-second" range="80-432000" />
        </MediaCodec>
        <MediaCodec name="c2.android.avc.encoder" type="video/avc" update="true">
            <Limit name="size" min="16x16" max="1808x1808" />
        </MediaCodec>
    </Encoders>
</MediaCodecs>)";

// media_codecs_performance.xml: measured rates only, no size.
static const char performance[] = R"(<MediaCodecs><Encoders>
        <MediaCodec name="c2.mtk.avc.encoder" type="video/avc" update="true">
            <Limit name="measured-frame-rate-1920x1080" range="53-117" />
        </MediaCodec>
</Encoders></MediaCodecs>)";

int main()
{
    VideoEncoderLimits avc;
    videoEncoderReadLimits(QByteArray(jp2), QStringLiteral("video/avc"), &avc);
    videoEncoderReadLimits(QByteArray(performance), QStringLiteral("video/avc"), &avc);
    assert(avc.maxSize == QSize(2560, 1440));
    assert(avc.maxBlocks == 14400 && avc.maxBlocksPerSecond == 432000);
    assert(avc.supports(QSize(1920, 1080), 30));
    assert(avc.supports(QSize(2560, 1440), 30));
    assert(!avc.supports(QSize(2560, 1440), 60));
    assert(!avc.supports(QSize(3840, 2160), 30));
    assert(avc.supports(QSize(1440, 2560), 30)); // rotated
    // The decoder's 4096x2176 must not count for the encoder.
    VideoEncoderLimits hevc;
    videoEncoderReadLimits(QByteArray(jp2), QStringLiteral("video/hevc"), &hevc);
    assert(hevc.maxSize == QSize(2560, 1440));
    assert(!hevc.supports(QSize(3840, 2160), 30));
    // Nothing known: nothing supported (the caller falls back to 1080p).
    VideoEncoderLimits none;
    videoEncoderReadLimits(QByteArray("<MediaCodecs/>"), QStringLiteral("video/avc"), &none);
    assert(!none.known() && !none.supports(QSize(1920, 1080), 30));
    // A 4K encoder elsewhere.
    VideoEncoderLimits big;
    videoEncoderReadLimits(QByteArray(R"(<MediaCodecs><Encoders>
        <MediaCodec name="c2.qti.avc.encoder" type="video/avc">
            <Limit name="size" min="96x96" max="4096x2304" />
            <Limit name="block-count" range="1-36864" />
            <Limit name="blocks-per-second" range="1-1105920" />
        </MediaCodec></Encoders></MediaCodecs>)"), QStringLiteral("video/avc"), &big);
    assert(big.supports(QSize(3840, 2160), 30));
    return 0;
}
