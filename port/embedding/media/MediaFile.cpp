#include "config.h"
#include "media/MediaFile.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include <atomic>
#include <libyuv/convert_argb.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkImageGenerator.h>
#include <skia/core/SkImageInfo.h>
#include <skia/core/SkYUVAPixmaps.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace ZeroG::Media {

using namespace libyuv;

std::unique_ptr<MediaFile> MediaFile::open(MediaBytes& bytes, String& error)
{
    // Enough of the start to tell the formats apart (an ID3 tag is skipped
    // by the MP3 decoder itself).
    std::array<uint8_t, 64> head { };
    size_t length = bytes.read(0, head);
    auto starts = [&](std::span<const uint8_t> magic) {
        return length >= magic.size() && std::equal(magic.begin(), magic.end(), head.begin());
    };
    static constexpr uint8_t ebml[] = { 0x1A, 0x45, 0xDF, 0xA3 };
    if (starts(ebml))
        return openWebM(bytes, error);
    if (!length) {
        error = bytes.isFailed() ? "the media could not be loaded"_s : "the media is empty"_s;
        return nullptr;
    }
    // MP4 and its relatives start with a box named "ftyp".
    bool isMP4 = length >= 8 && !memcmp(head.data() + 4, "ftyp", 4);
    if (isMP4) {
        // VP9, AV1 and Opus with the engine's decoders; H.264 and AAC with the
        // OS's (Windows; elsewhere they do not play).
#if OS(WINDOWS)
        if (auto file = openMP4(bytes, true, error))
            return file;
        return openMediaFoundation(bytes, std::span<const uint8_t> { head }.first(length), error);
#else
        return openMP4(bytes, false, error);
#endif
    }
    return openAudioFile(bytes, std::span<const uint8_t> { head }.first(length), error);
}

namespace {

// The decode thread's choice (setDecodedFramesStayYUV).
thread_local bool t_framesStayYUV;
std::atomic<uint64_t> s_yuvFrames;

// libyuv writes ARGB words; with U and V swapped and the mirrored ("Yvu")
// constants the bytes come out R, G, B, A, Skia's RGBA.
const YuvConstants* mirroredConstants(ColorMatrix matrix, bool fullRange)
{
    switch (matrix) {
    case ColorMatrix::BT709:
        return fullRange ? &kYvuF709Constants : &kYvuH709Constants;
    case ColorMatrix::BT2020:
        return fullRange ? &kYvuV2020Constants : &kYvu2020Constants;
    case ColorMatrix::BT601:
    default:
        return fullRange ? &kYvuJPEGConstants : &kYvuI601Constants;
    }
}

bool convertInto(const YUVPlanes& planes, uint8_t* destination, size_t rowBytes)
{
    auto* constants = mirroredConstants(planes.matrix, planes.fullRange);
    if (planes.highBitDepth) {
        // 16-bit samples: strides in samples.
        return !I010ToARGBMatrix(reinterpret_cast<const uint16_t*>(planes.y), planes.yStride / 2,
            reinterpret_cast<const uint16_t*>(planes.v), planes.vStride / 2, reinterpret_cast<const uint16_t*>(planes.u), planes.uStride / 2,
            destination, rowBytes, constants, planes.width, planes.height);
    }
    switch (planes.subsampling) {
    case YUVPlanes::Subsampling::I420:
        return !I420ToARGBMatrix(planes.y, planes.yStride, planes.v, planes.vStride, planes.u, planes.uStride, destination, rowBytes, constants, planes.width, planes.height);
    case YUVPlanes::Subsampling::I422:
        return !I422ToARGBMatrix(planes.y, planes.yStride, planes.v, planes.vStride, planes.u, planes.uStride, destination, rowBytes, constants, planes.width, planes.height);
    case YUVPlanes::Subsampling::I444:
        return !I444ToARGBMatrix(planes.y, planes.yStride, planes.v, planes.vStride, planes.u, planes.uStride, destination, rowBytes, constants, planes.width, planes.height);
    }
    return false;
}

// NV12 read as NV21 with the mirrored constants: R, G, B, A again (libyuv's
// NV12ToABGRMatrix).
bool convertNV12Into(const uint8_t* y, int yStride, const uint8_t* uv, int uvStride, int width, int height, ColorMatrix matrix, bool fullRange, uint8_t* destination, size_t rowBytes)
{
    return !NV21ToARGBMatrix(y, yStride, uv, uvStride, destination, rowBytes, mirroredConstants(matrix, fullRange), width, height);
}

template<typename Convert>
sk_sp<SkImage> rasterImage(int width, int height, const Convert& convert)
{
    size_t rowBytes = static_cast<size_t>(width) * 4;
    auto pixels = SkData::MakeUninitialized(rowBytes * height);
    if (!convert(static_cast<uint8_t*>(pixels->writable_data()), rowBytes))
        return nullptr;
    auto info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kOpaque_SkAlphaType);
    return SkImages::RasterFromData(info, std::move(pixels), rowBytes);
}

SkYUVColorSpace skiaColorSpace(ColorMatrix matrix, bool fullRange)
{
    switch (matrix) {
    case ColorMatrix::BT709:
        return fullRange ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    case ColorMatrix::BT2020:
        return fullRange ? kBT2020_8bit_Full_SkYUVColorSpace : kBT2020_8bit_Limited_SkYUVColorSpace;
    case ColorMatrix::BT601:
    default:
        return fullRange ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
    }
}

// A frame kept as its YUV planes: drawn by Skia on the GPU, the planes are
// uploaded and converted in a shader (SkImage_Lazy's YUVA path); drawn on the
// CPU, it is converted to RGBA then.
class YUVFrameGenerator final : public SkImageGenerator {
public:
    YUVFrameGenerator(SkYUVAPixmaps&& pixmaps, ColorMatrix matrix, bool fullRange)
        : SkImageGenerator(SkImageInfo::Make(pixmaps.yuvaInfo().dimensions(), kRGBA_8888_SkColorType, kOpaque_SkAlphaType))
        , m_pixmaps(WTF::move(pixmaps))
        , m_matrix(matrix)
        , m_fullRange(fullRange)
    {
    }

private:
    bool onQueryYUVAInfo(const SkYUVAPixmapInfo::SupportedDataTypes& supported, SkYUVAPixmapInfo* info) const final
    {
        auto pixmapsInfo = m_pixmaps.pixmapsInfo();
        if (!pixmapsInfo.isSupported(supported))
            return false;
        *info = pixmapsInfo;
        return true;
    }

    bool onGetYUVAPlanes(const SkYUVAPixmaps& planes) final
    {
        for (int i = 0; i < m_pixmaps.numPlanes(); ++i) {
            if (!m_pixmaps.plane(i).readPixels(planes.plane(i)))
                return false;
        }
        return true;
    }

    bool onGetPixels(const SkImageInfo& info, void* pixels, size_t rowBytes, const Options&) final
    {
        auto& y = m_pixmaps.plane(0);
        auto& second = m_pixmaps.plane(1);
        auto convert = [&](uint8_t* destination, size_t destinationRowBytes) {
            int width = y.width();
            int height = y.height();
            if (m_pixmaps.yuvaInfo().planeConfig() == SkYUVAInfo::PlaneConfig::kY_UV)
                return convertNV12Into(y.addr8(), y.rowBytes(), second.addr8(), second.rowBytes(), width, height, m_matrix, m_fullRange, destination, destinationRowBytes);
            auto& third = m_pixmaps.plane(2);
            YUVPlanes planes { y.addr8(), second.addr8(), third.addr8(), static_cast<int>(y.rowBytes()), static_cast<int>(second.rowBytes()), static_cast<int>(third.rowBytes()), width, height };
            auto subsampling = m_pixmaps.yuvaInfo().subsampling();
            planes.subsampling = subsampling == SkYUVAInfo::Subsampling::k444 ? YUVPlanes::Subsampling::I444 : subsampling == SkYUVAInfo::Subsampling::k422 ? YUVPlanes::Subsampling::I422 : YUVPlanes::Subsampling::I420;
            planes.matrix = m_matrix;
            planes.fullRange = m_fullRange;
            return convertInto(planes, destination, destinationRowBytes);
        };
        if (info.colorType() == kRGBA_8888_SkColorType && info.dimensions() == y.dimensions())
            return convert(static_cast<uint8_t*>(pixels), rowBytes);
        // Another format asked for: through RGBA.
        auto image = rasterImage(y.width(), y.height(), convert);
        return image && image->readPixels(nullptr, info, pixels, rowBytes, 0, 0);
    }

    SkYUVAPixmaps m_pixmaps;
    ColorMatrix m_matrix;
    bool m_fullRange;
};

struct PlaneSource {
    const uint8_t* data;
    int stride;
};

// The planes copied out of the decoder (its buffers are reused).
sk_sp<SkImage> yuvImage(SkYUVAInfo::PlaneConfig config, SkYUVAInfo::Subsampling subsampling, std::span<const PlaneSource> sources, int width, int height, ColorMatrix matrix, bool fullRange)
{
    SkYUVAInfo yuvaInfo({ width, height }, config, subsampling, skiaColorSpace(matrix, fullRange));
    SkYUVAPixmapInfo pixmapInfo(yuvaInfo, SkYUVAPixmapInfo::DataType::kUnorm8, nullptr);
    auto pixmaps = SkYUVAPixmaps::Allocate(pixmapInfo);
    if (!pixmaps.isValid() || static_cast<size_t>(pixmaps.numPlanes()) != sources.size())
        return nullptr;
    for (int i = 0; i < pixmaps.numPlanes(); ++i) {
        auto& plane = pixmaps.plane(i);
        size_t rowLength = plane.width() * plane.info().bytesPerPixel();
        for (int row = 0; row < plane.height(); ++row)
            memcpy(static_cast<uint8_t*>(plane.writable_addr(0, row)), sources[i].data + static_cast<size_t>(row) * sources[i].stride, rowLength);
    }
    ++s_yuvFrames;
    return SkImages::DeferredFromGenerator(std::make_unique<YUVFrameGenerator>(WTF::move(pixmaps), matrix, fullRange));
}

} // namespace

void setDecodedFramesStayYUV(bool stayYUV)
{
    t_framesStayYUV = stayYUV;
}

uint64_t framesKeptAsYUV()
{
    return s_yuvFrames;
}

sk_sp<SkImage> convertNV12ToImage(const uint8_t* y, int yStride, const uint8_t* uv, int uvStride, int width, int height, ColorMatrix matrix, bool fullRange)
{
    if (t_framesStayYUV) {
        PlaneSource sources[] = { { y, yStride }, { uv, uvStride } };
        return yuvImage(SkYUVAInfo::PlaneConfig::kY_UV, SkYUVAInfo::Subsampling::k420, sources, width, height, matrix, fullRange);
    }
    return rasterImage(width, height, [&](uint8_t* destination, size_t rowBytes) {
        return convertNV12Into(y, yStride, uv, uvStride, width, height, matrix, fullRange, destination, rowBytes);
    });
}

sk_sp<SkImage> convertToImage(const YUVPlanes& planes)
{
    // 10-bit frames are converted here (Skia's planes are 8 or 16 bits).
    if (t_framesStayYUV && !planes.highBitDepth) {
        PlaneSource sources[] = { { planes.y, planes.yStride }, { planes.u, planes.uStride }, { planes.v, planes.vStride } };
        auto subsampling = planes.subsampling == YUVPlanes::Subsampling::I444 ? SkYUVAInfo::Subsampling::k444 : planes.subsampling == YUVPlanes::Subsampling::I422 ? SkYUVAInfo::Subsampling::k422 : SkYUVAInfo::Subsampling::k420;
        return yuvImage(SkYUVAInfo::PlaneConfig::kY_U_V, subsampling, sources, planes.width, planes.height, planes.matrix, planes.fullRange);
    }
    return rasterImage(planes.width, planes.height, [&](uint8_t* destination, size_t rowBytes) {
        return convertInto(planes, destination, rowBytes);
    });
}

} // namespace ZeroG::Media

#endif
