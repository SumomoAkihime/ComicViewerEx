#include "image.hpp"

#include <wincodec.h>
#include <wrl/client.h>
#include <objidl.h>
#include <webp/encode.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using Microsoft::WRL::ComPtr;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

ComPtr<IWICImagingFactory> makeFactory() {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&factory));
    require(SUCCEEDED(hr), "无法创建 WIC 工厂");
    return factory;
}

std::vector<uint8_t> encodeWic(REFGUID container, const std::vector<uint8_t>& bgra) {
    auto factory = makeFactory();
    ComPtr<IWICBitmap> bitmap;
    require(SUCCEEDED(factory->CreateBitmapFromMemory(2, 2, GUID_WICPixelFormat32bppBGRA,
                                                       8, static_cast<UINT>(bgra.size()),
                                                       const_cast<BYTE*>(bgra.data()), &bitmap)),
            "无法创建测试位图");
    ComPtr<IStream> stream;
    require(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "无法创建测试流");
    ComPtr<IWICBitmapEncoder> encoder;
    require(SUCCEEDED(factory->CreateEncoder(container, nullptr, &encoder)),
            "无法创建测试编码器");
    require(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)),
            "无法初始化测试编码器");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    require(SUCCEEDED(encoder->CreateNewFrame(&frame, &options)), "无法创建测试帧");
    require(SUCCEEDED(frame->Initialize(options.Get())), "无法初始化测试帧");
    require(SUCCEEDED(frame->SetSize(2, 2)), "无法设置测试尺寸");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    require(SUCCEEDED(frame->SetPixelFormat(&format)), "无法设置测试格式");
    require(SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)), "无法写入测试帧");
    require(SUCCEEDED(frame->Commit()), "无法提交测试帧");
    require(SUCCEEDED(encoder->Commit()), "无法提交测试图像");
    STATSTG stat{};
    require(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
                stat.cbSize.QuadPart <= SIZE_MAX,
            "无法读取测试图像大小");
    const size_t size = static_cast<size_t>(stat.cbSize.QuadPart);
    HGLOBAL handle = nullptr;
    require(SUCCEEDED(GetHGlobalFromStream(stream.Get(), &handle)), "无法读取测试图像");
    void* memory = GlobalLock(handle);
    require(memory != nullptr, "无法锁定测试图像");
    std::vector<uint8_t> result(size);
    std::memcpy(result.data(), memory, size);
    GlobalUnlock(handle);
    return result;
}

void checkImage(const std::vector<uint8_t>& encoded, uint8_t b, uint8_t g, uint8_t r) {
    auto pixels = cv::decodeImage(encoded, {});
    require(pixels && pixels->width == 2 && pixels->height == 2, "解码尺寸错误");
    require(pixels->bgra.size() == 16, "解码数据长度错误");
    const auto close = [](uint8_t actual, uint8_t expected) {
        return std::abs(int(actual) - int(expected)) <= 12;
    };
    require(close(pixels->bgra[0], b) && close(pixels->bgra[1], g) &&
                close(pixels->bgra[2], r) && pixels->bgra[3] == 255,
            "解码颜色错误");
}

void testWic() {
    const std::vector<uint8_t> bgra = {
        0, 0, 255, 255,       0, 255, 0, 255,
        255, 0, 0, 255,       255, 255, 255, 255,
    };
    checkImage(encodeWic(GUID_ContainerFormatPng, bgra), 0, 0, 255);
    // A 2x2 multicolour JPEG is chroma-subsampled; use a uniform patch for colour fidelity.
    checkImage(encodeWic(GUID_ContainerFormatJpeg, {0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255}), 0, 0, 255);
    checkImage(encodeWic(GUID_ContainerFormatBmp, bgra), 0, 0, 255);
    checkImage(encodeWic(GUID_ContainerFormatGif, bgra), 0, 0, 255);
    checkImage(encodeWic(GUID_ContainerFormatTiff, bgra), 0, 0, 255);
}

void testWebp() {
    const uint8_t rgba[] = {255, 0, 0, 128, 0, 255, 0, 255,
                            0, 0, 255, 255, 255, 255, 255, 255};
    uint8_t* encoded = nullptr;
    size_t size = WebPEncodeLosslessRGBA(rgba, 2, 2, 8, &encoded);
    require(encoded != nullptr && size != 0, "无法编码测试 WebP");
    std::vector<uint8_t> data(encoded, encoded + size);
    WebPFree(encoded);
    auto pixels = cv::decodeImage(data, {});
    require(pixels && pixels->width == 2 && pixels->height == 2, "WebP 尺寸错误");
    require(pixels->bgra[3] == 128 && pixels->bgra[2] == 128, "WebP 透明预乘错误");
}

void testErrors() {
    bool failed = false;
    try { (void)cv::decodeImage(std::vector<uint8_t>{'x', 'y'}, {}); }
    catch (const std::exception&) { failed = true; }
    require(failed, "损坏图像未被拒绝");
    bool cancelled = false;
    try { (void)cv::decodeImage(std::vector<uint8_t>{'x'}, [] { return true; }); }
    catch (const cv::Cancelled&) { cancelled = true; }
    require(cancelled, "取消未生效");
}

} // namespace

int main() {
    try {
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "无法初始化 COM");
        testWic();
        testWebp();
        testErrors();
        CoUninitialize();
        std::cout << "image tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
