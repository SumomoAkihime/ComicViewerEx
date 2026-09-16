#include "image.hpp"

#include <wincodec.h>
#include <wrl/client.h>
#include <oleauto.h>

#include <cmath>
#include <cstring>
#include <limits>

#include <webp/decode.h>
#include <webp/demux.h>

namespace cv {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kMaxPixels = 100000000ull;
constexpr uint64_t kMaxOutputPixels = 6000000ull;

[[noreturn]] void badImage(const char* message) {
    throw std::runtime_error(message);
}

void checkHr(HRESULT hr, const char* message) {
    if (FAILED(hr)) badImage(message);
}

void checkedGeometry(uint32_t width, uint32_t height, uint64_t maxPixels = kMaxPixels) {
    if (width == 0 || height == 0 || uint64_t(width) * uint64_t(height) > maxPixels)
        badImage("图像尺寸过大或无效");
}

size_t checkedBytes(uint32_t width, uint32_t height) {
    checkedGeometry(width, height, kMaxOutputPixels);
    const uint64_t bytes = uint64_t(width) * uint64_t(height) * 4ull;
    if (bytes > std::numeric_limits<size_t>::max()) badImage("图像数据长度溢出");
    return size_t(bytes);
}

bool isWebp(std::span<const uint8_t> data) {
    return data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 &&
           std::memcmp(data.data() + 8, "WEBP", 4) == 0;
}

struct TargetSize {
    uint32_t width{};
    uint32_t height{};
};

TargetSize outputSize(uint32_t width, uint32_t height, bool swapped) {
    checkedGeometry(width, height);
    const uint64_t logicalWidth = swapped ? height : width;
    const uint64_t logicalHeight = swapped ? width : height;
    const uint64_t pixels = logicalWidth * logicalHeight;
    long double scale = pixels > kMaxOutputPixels
        ? std::sqrt(static_cast<long double>(kMaxOutputPixels) /
                    static_cast<long double>(pixels))
        : 1.0L;
    uint64_t outWidth = std::max<uint64_t>(1, uint64_t(std::floor(logicalWidth * scale)));
    uint64_t outHeight = std::max<uint64_t>(1, uint64_t(std::floor(logicalHeight * scale)));
    while (outWidth * outHeight > kMaxOutputPixels) {
        if (outWidth >= outHeight) --outWidth;
        else --outHeight;
    }
    if (outWidth > std::numeric_limits<uint32_t>::max() ||
        outHeight > std::numeric_limits<uint32_t>::max()) badImage("图像尺寸溢出");
    return {uint32_t(outWidth), uint32_t(outHeight)};
}

bool rotate90(uint16_t orientation) {
    return orientation >= 5 && orientation <= 8;
}

uint16_t exifOrientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    uint16_t result = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value))) {
        if (value.vt == VT_UI2) result = value.uiVal;
        else if (value.vt == VT_UI1) result = value.bVal;
        if (result < 1 || result > 8) result = 1;
    }
    PropVariantClear(&value);
    return result;
}

std::shared_ptr<Pixels> orient(std::vector<uint8_t>&& source, uint32_t sourceWidth,
                               uint32_t sourceHeight, uint16_t orientation,
                               const Cancel& cancel) {
    const bool swapped = rotate90(orientation);
    const uint32_t width = swapped ? sourceHeight : sourceWidth;
    const uint32_t height = swapped ? sourceWidth : sourceHeight;
    auto result = std::make_shared<Pixels>();
    result->width = width;
    result->height = height;
    if (orientation == 1) { result->bgra = std::move(source); return result; }
    result->bgra.resize(checkedBytes(width, height));
    for (uint32_t y = 0; y < height; ++y) {
        checkCancel(cancel);
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t sx = x, sy = y;
            switch (orientation) {
            case 2: sx = sourceWidth - 1 - x; break;
            case 3: sx = sourceWidth - 1 - x; sy = sourceHeight - 1 - y; break;
            case 4: sy = sourceHeight - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = sourceHeight - 1 - x; break;
            case 7: sx = sourceWidth - 1 - y; sy = sourceHeight - 1 - x; break;
            case 8: sx = sourceWidth - 1 - y; sy = x; break;
            default: break;
            }
            const uint8_t* in = source.data() + (size_t(sy) * sourceWidth + sx) * 4;
            uint8_t* out = result->bgra.data() + (size_t(y) * width + x) * 4;
            std::memcpy(out, in, 4);
        }
    }
    return result;
}

std::vector<uint8_t> decodeWicSource(IWICBitmapFrameDecode* frame, uint32_t width,
                                     uint32_t height, const Cancel& cancel) {
    checkCancel(cancel);
    const size_t bytes = checkedBytes(width, height);
    std::vector<uint8_t> pixels(bytes);
    const UINT stride = width * 4;

    ComPtr<IWICBitmapSourceTransform> transform;
    if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&transform)))) {
        UINT actualWidth = width, actualHeight = height;
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
        if (SUCCEEDED(transform->GetClosestSize(&actualWidth, &actualHeight)) &&
            actualWidth == width && actualHeight == height &&
            SUCCEEDED(transform->CopyPixels(nullptr, width, height,
                                             &pixelFormat,
                                             WICBitmapTransformRotate0, stride,
                                             static_cast<UINT>(bytes), pixels.data()))) {
            checkCancel(cancel);
            return pixels;
        }
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        checkHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory)), "无法创建图像工厂");
    }
    ComPtr<IWICBitmapScaler> scaler;
    checkHr(factory->CreateBitmapScaler(&scaler), "无法创建图像缩放器");
    checkHr(scaler->Initialize(frame, width, height, WICBitmapInterpolationModeFant),
            "无法初始化图像缩放器");
    ComPtr<IWICFormatConverter> converter;
    checkHr(factory->CreateFormatConverter(&converter), "无法创建图像格式转换器");
    checkHr(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom), "无法转换图像格式");
    checkHr(converter->CopyPixels(nullptr, stride, static_cast<UINT>(bytes), pixels.data()),
            "无法读取图像像素");
    checkCancel(cancel);
    return pixels;
}

std::shared_ptr<Pixels> decodeWic(std::span<const uint8_t> data, const Cancel& cancel) {
    if (data.size() > std::numeric_limits<DWORD>::max()) badImage("图像数据过大");
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        checkHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory)), "无法创建图像工厂");
    }
    ComPtr<IWICStream> stream;
    checkHr(factory->CreateStream(&stream), "无法创建图像流");
    checkHr(stream->InitializeFromMemory(const_cast<BYTE*>(data.data()), DWORD(data.size())),
            "无法读取图像数据");
    ComPtr<IWICBitmapDecoder> decoder;
    checkHr(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                             &decoder), "不支持或损坏的图像");
    UINT frameCount = 0;
    checkHr(decoder->GetFrameCount(&frameCount), "无法读取图像帧");
    if (frameCount == 0) badImage("图像没有可用帧");
    ComPtr<IWICBitmapFrameDecode> frame;
    checkHr(decoder->GetFrame(0, &frame), "无法读取图像帧");
    UINT rawWidth = 0, rawHeight = 0;
    checkHr(frame->GetSize(&rawWidth, &rawHeight), "无法读取图像尺寸");
    checkedGeometry(rawWidth, rawHeight);
    const uint16_t orientation = exifOrientation(frame.Get());
    const TargetSize target = outputSize(rawWidth, rawHeight, rotate90(orientation));
    const uint32_t decodeWidth = rotate90(orientation) ? target.height : target.width;
    const uint32_t decodeHeight = rotate90(orientation) ? target.width : target.height;
    auto result = orient(decodeWicSource(frame.Get(), decodeWidth, decodeHeight, cancel),
                         decodeWidth, decodeHeight, orientation, cancel);
    result->originalWidth = rotate90(orientation) ? rawHeight : rawWidth;
    result->originalHeight = rotate90(orientation) ? rawWidth : rawHeight;
    return result;
}

void premultiplyRgbaToBgra(const uint8_t* rgba, uint8_t* bgra, uint32_t width,
                           uint32_t height, size_t stride, const Cancel& cancel) {
    for (uint32_t y = 0; y < height; ++y) {
        checkCancel(cancel);
        const uint8_t* source = rgba + size_t(y) * stride;
        uint8_t* target = bgra + size_t(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t a = source[x * 4 + 3];
            target[x * 4 + 0] = uint8_t((uint32_t(source[x * 4 + 2]) * a + 127) / 255);
            target[x * 4 + 1] = uint8_t((uint32_t(source[x * 4 + 1]) * a + 127) / 255);
            target[x * 4 + 2] = uint8_t((uint32_t(source[x * 4 + 0]) * a + 127) / 255);
            target[x * 4 + 3] = a;
        }
    }
}

bool decodeWebpBuffer(const uint8_t* data, size_t size, uint32_t width, uint32_t height,
                      std::vector<uint8_t>& output, const Cancel& cancel) {
    checkCancel(cancel);
    if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX) return false;
    const size_t stride = size_t(width) * 4;
    output.resize(checkedBytes(width, height));
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) badImage("WebP 解码器初始化失败");
    if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) return false;
    config.output.colorspace = MODE_RGBA;
    config.output.width = int(width);
    config.output.height = int(height);
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = output.data();
    config.output.u.RGBA.stride = int(stride);
    config.output.u.RGBA.size = output.size();
    config.options.use_scaling = 1;
    config.options.scaled_width = int(width);
    config.options.scaled_height = int(height);
    const VP8StatusCode status = WebPDecode(data, size, &config);
    checkCancel(cancel);
    return status == VP8_STATUS_OK;
}

std::shared_ptr<Pixels> decodeWebp(std::span<const uint8_t> data, const Cancel& cancel) {
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK)
        badImage("WebP 数据损坏");
    if (features.width <= 0 || features.height <= 0) badImage("WebP 尺寸无效");
    uint32_t canvasWidth = uint32_t(features.width), canvasHeight = uint32_t(features.height);
    std::vector<uint8_t> rgba;
    if (!features.has_animation) {
        checkedGeometry(canvasWidth, canvasHeight);
        const TargetSize target = outputSize(canvasWidth, canvasHeight, false);
        if (!decodeWebpBuffer(data.data(), data.size(), target.width, target.height, rgba, cancel))
            badImage("WebP 解码失败");
        auto result = std::make_shared<Pixels>();
        result->width = target.width;
        result->height = target.height;
        result->originalWidth = canvasWidth; result->originalHeight = canvasHeight;
        result->bgra.resize(checkedBytes(target.width, target.height));
        premultiplyRgbaToBgra(rgba.data(), result->bgra.data(), target.width, target.height,
                              size_t(target.width) * 4, cancel);
        checkCancel(cancel);
        return result;
    }

    WebPData webpData{data.data(), data.size()};
    WebPDemuxer* demux = WebPDemux(&webpData);
    if (!demux) badImage("WebP 动画数据损坏");
    struct DemuxGuard { WebPDemuxer* value; ~DemuxGuard() { WebPDemuxDelete(value); } } guard{demux};
    canvasWidth = WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
    canvasHeight = WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
    checkedGeometry(canvasWidth, canvasHeight);
    const TargetSize target = outputSize(canvasWidth, canvasHeight, false);
    auto result = std::make_shared<Pixels>();
    result->width = target.width;
    result->height = target.height;
    result->originalWidth = canvasWidth; result->originalHeight = canvasHeight;
    result->bgra.assign(checkedBytes(target.width, target.height), 0);
    WebPIterator iterator{};
    if (!WebPDemuxGetFrame(demux, 1, &iterator)) badImage("WebP 动画没有首帧");
    struct IteratorGuard { WebPIterator* value; ~IteratorGuard() { WebPDemuxReleaseIterator(value); } } iterGuard{&iterator};
    const uint32_t frameWidth = uint32_t(uint64_t(std::max(0, iterator.width)) * target.width /
                                         canvasWidth);
    const uint32_t frameHeight = uint32_t(uint64_t(std::max(0, iterator.height)) * target.height /
                                          canvasHeight);
    if (frameWidth == 0 || frameHeight == 0) badImage("WebP 首帧尺寸无效");
    if (!decodeWebpBuffer(iterator.fragment.bytes, iterator.fragment.size, frameWidth, frameHeight,
                          rgba, cancel)) badImage("WebP 首帧解码失败");
    const int64_t offsetX = int64_t(iterator.x_offset) * target.width / canvasWidth;
    const int64_t offsetY = int64_t(iterator.y_offset) * target.height / canvasHeight;
    for (uint32_t y = 0; y < frameHeight; ++y) {
        checkCancel(cancel);
        const int64_t dy = offsetY + y;
        if (dy < 0 || dy >= target.height) continue;
        for (uint32_t x = 0; x < frameWidth; ++x) {
            const int64_t dx = offsetX + x;
            if (dx < 0 || dx >= target.width) continue;
            const uint8_t* src = rgba.data() + (size_t(y) * frameWidth + x) * 4;
            uint8_t* dst = result->bgra.data() + (size_t(dy) * target.width + size_t(dx)) * 4;
            const uint8_t alpha = src[3];
            const uint8_t sb = uint8_t((uint32_t(src[2]) * alpha + 127) / 255);
            const uint8_t sg = uint8_t((uint32_t(src[1]) * alpha + 127) / 255);
            const uint8_t sr = uint8_t((uint32_t(src[0]) * alpha + 127) / 255);
            if (iterator.blend_method == WEBP_MUX_NO_BLEND) {
                dst[0] = sb; dst[1] = sg; dst[2] = sr; dst[3] = alpha;
            } else {
                const uint8_t inverse = uint8_t(255 - alpha);
                dst[0] = uint8_t(sb + (uint32_t(dst[0]) * inverse + 127) / 255);
                dst[1] = uint8_t(sg + (uint32_t(dst[1]) * inverse + 127) / 255);
                dst[2] = uint8_t(sr + (uint32_t(dst[2]) * inverse + 127) / 255);
                dst[3] = uint8_t(alpha + (uint32_t(dst[3]) * inverse + 127) / 255);
            }
        }
    }
    checkCancel(cancel);
    return result;
}

} // namespace

std::shared_ptr<Pixels> decodeImage(std::span<const uint8_t> data, const Cancel& cancel) {
    checkCancel(cancel);
    if (data.empty()) badImage("图像数据为空");
    auto result = isWebp(data) ? decodeWebp(data, cancel) : decodeWic(data, cancel);
    checkCancel(cancel);
    return result;
}

} // namespace cv
