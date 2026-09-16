#include "archive.hpp"
#include "image.hpp"
#include <iostream>

using namespace cv;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        auto root = fs::path(__FILE__).parent_path().parent_path() / "test-output" / "fixtures";
        auto cache = root / "archive-cache"; fs::create_directories(cache);
        const wchar_t* names[] = {L"漫画.cbz",L"zipcrypto.zip",L"aes.zip",L"solid.7z",L"encrypted.7z",L"Rar.rar",L"Rar.solid.rar",L"Rar.encrypted_filesOnly.rar",L"Rar.encrypted_filesAndHeader.rar",L"Rar5.rar",L"Rar5.solid.rar",L"Rar5.encrypted_filesOnly.rar",L"Rar5.encrypted_filesAndHeader.rar"};
        for (auto name : names) {
            std::cout << "检查 " << utf8(name) << std::endl;
            auto source = openBook(root / name, L"test", cache, [] { return false; }, {});
            require(!source->entries().empty(), "未发现图片");
            if (std::wstring(name).find(L"solid") != std::wstring::npos) require(source->solid(), "未识别固实包");
            for (size_t i = 0; i < source->entries().size(); ++i) {
                auto bytes = source->read(i, [] { return false; }, {});
                auto image = decodeImage(bytes, {}); require(image && image->width, "图片解码失败");
            }
            auto bytes = source->read(0, {}, {}); require(!bytes.empty(), "回翻缓存失败");
            source.reset(); require(fs::is_empty(cache), "关闭书籍未清理磁盘缓存");
        }
        for (auto name : {L"zipcrypto.zip",L"aes.zip",L"encrypted.7z",L"Rar.encrypted_filesOnly.rar",L"Rar.encrypted_filesAndHeader.rar",L"Rar5.encrypted_filesOnly.rar",L"Rar5.encrypted_filesAndHeader.rar"}) {
            bool rejected = false;
            try { auto source = openBook(root/name,L"incorrect",cache,{},{}); source->read(0,{},{}); } catch (const PasswordRequired&) { rejected = true; }
            require(rejected, "错误密码未正确识别");
        }
        for (auto name : {L"corrupt.zip",L"empty.zip",L"Rar5.multi.part01.rar"}) {
            bool rejected = false; try { auto source = openBook(root/name,L"",cache,{},{}); source->read(0,{},{}); } catch (const std::exception&) { rejected = true; }
            require(rejected, "无效包/分卷包未拒绝");
        }
        bool cancelled = false; try { openBook(root/L"漫画.cbz",L"",cache,[]{return true;},{}); } catch (const Cancelled&) { cancelled = true; }
        require(cancelled, "打开取消失效");
        auto directory = openBook(root/L"中文图片",L"",cache,{},{});
        require(directory->entries().size() == 6, "图片目录递归不正确");
        require(directory->entries()[0].name == L"page1.png" && directory->entries()[1].name == L"page2.png", "自然排序不正确");
        {
            auto stress = openBook(root/L"cache-stress.7z",L"",cache,{},{});
            auto bytes = stress->read(59,{},{}); require(bytes.size() > 9*1024*1024, "缓存测试图片大小异常");
            uint64_t diskBytes{}; for (const auto& file : fs::directory_iterator(cache)) diskBytes += file.file_size();
            require(diskBytes <= 512ull*1024*1024 && diskBytes > 480ull*1024*1024, "磁盘缓存未正确限制或淘汰");
            bool interrupted = false;
            try { stress->read(0,[]{return true;},{}); } catch (const Cancelled&) { interrupted = true; }
            require(interrupted, "固实包重新解压取消失效");
        }
        require(fs::is_empty(cache), "缓存淘汰测试清理失败");
        std::cout << "压缩包集成测试通过" << std::endl;
        CoUninitialize(); return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; CoUninitialize(); return 1; }
}
