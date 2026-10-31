#include "config.h"
#include "media/MediaFile.h"

#if ENABLE(VIDEO) && defined(ZEROG_MEDIA)

#include <libyuv/convert_argb.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkData.h>
#include <skia/core/SkImageInfo.h>
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
    return openAudioFile(bytes, std::span<const uint8_t> { head }.first(length), error);
}

sk_sp<SkImage> convertToImage(const YUVPlanes& planes)
{
    size_t rowBytes = static_cast<size_t>(planes.width) * 4;
    auto pixels = SkData::MakeUninitialized(rowBytes * planes.height);
    auto* destination = static_cast<uint8_t*>(pixels->writable_data());
    // libyuv writes ARGB words; with U and V swapped and the mirrored ("Yvu")
    // constants the bytes come out R, G, B, A, Skia's RGBA.
    const YuvConstants* constants;
    switch (planes.matrix) {
    case ColorMatrix::BT709:
        constants = planes.fullRange ? &kYvuF709Constants : &kYvuH709Constants;
        break;
    case ColorMatrix::BT2020:
        constants = planes.fullRange ? &kYvuV2020Constants : &kYvu2020Constants;
        break;
    case ColorMatrix::BT601:
    default:
        constants = planes.fullRange ? &kYvuJPEGConstants : &kYvuI601Constants;
        break;
    }
    int result = -1;
    if (planes.highBitDepth) {
        // 16-bit samples: strides in samples.
        result = I010ToARGBMatrix(reinterpret_cast<const uint16_t*>(planes.y), planes.yStride / 2,
            reinterpret_cast<const uint16_t*>(planes.v), planes.vStride / 2, reinterpret_cast<const uint16_t*>(planes.u), planes.uStride / 2,
            destination, rowBytes, constants, planes.width, planes.height);
    } else {
        switch (planes.subsampling) {
        case YUVPlanes::Subsampling::I420:
            result = I420ToARGBMatrix(planes.y, planes.yStride, planes.v, planes.vStride, planes.u, planes.uStride, destination, rowBytes, constants, planes.width, planes.height);
            break;
        case YUVPlanes::Subsampling::I422:
            result = I422ToARGBMatrix(planes.y, planes.yStride, planes.v, planes.vStride, planes.u, planes.uStride, destination, rowBytes, constants, planes.width, planes.height);
            break;
        case YUVPlanes::Subsampling::I444:
            result = I444ToARGBMatrix(planes.y, planes.yStride, planes.v, planes.vStride, planes.u, planes.uStride, destination, rowBytes, constants, planes.width, planes.height);
            break;
        }
    }
    if (result)
        return nullptr;
    auto info = SkImageInfo::Make(planes.width, planes.height, kRGBA_8888_SkColorType, kOpaque_SkAlphaType);
    return SkImages::RasterFromData(info, std::move(pixels), rowBytes);
}

} // namespace ZeroG::Media

#endif
