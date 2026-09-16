#include "archive.hpp"

#include <objbase.h>
#include <oleauto.h>
#include <propidl.h>
#include <initguid.h>

#include "../third_party/7zip/CPP/7zip/Archive/IArchive.h"
#include "../third_party/7zip/CPP/7zip/IPassword.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <sstream>

namespace cv {
namespace {

constexpr uint64_t kMaxImageSize = 128ull * 1024 * 1024;
constexpr uint64_t kMaxCacheSize = 512ull * 1024 * 1024;

[[noreturn]] void fail(const char *message) { throw std::runtime_error(message); }

bool shouldCancel(const Cancel *cancel) noexcept {
    if (!cancel || !*cancel) return false;
    try { return (*cancel)(); }
    catch (...) { return true; }
}

void checkHr(HRESULT hr, const char *message) {
    if (FAILED(hr)) {
        std::ostringstream ss;
        ss << message << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ')';
        throw std::runtime_error(ss.str());
    }
}

class RefCounted {
protected:
    std::atomic<ULONG> refs_{1};
    virtual ~RefCounted() = default;
public:
    ULONG addRef() { return ++refs_; }
    ULONG release() {
        const ULONG value = --refs_;
        if (value == 0) delete this;
        return value;
    }
};

class FileInStream final : public IInStream, public IStreamGetSize, public RefCounted {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    const Cancel *cancel_ = nullptr;

public:
    FileInStream(const fs::path &path, const Cancel *cancel) : cancel_(cancel) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) fail("无法读取归档文件");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_, &length) || length.QuadPart < 0) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            fail("无法获取归档文件大小");
        }
        size_ = static_cast<uint64_t>(length.QuadPart);
    }
    ~FileInStream() override { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) noexcept override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialInStream || iid == IID_IInStream)
            *object = static_cast<IInStream *>(this);
        else if (iid == IID_IStreamGetSize)
            *object = static_cast<IStreamGetSize *>(this);
        else return E_NOINTERFACE;
        addRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }

    HRESULT STDMETHODCALLTYPE Read(void *data, UInt32 count, UInt32 *processed) noexcept override {
        UInt32 ignored{};
        if (!processed) processed = &ignored;
        *processed = 0;
        if (shouldCancel(cancel_)) return E_ABORT;
        if (!ReadFile(file_, data, count, reinterpret_cast<LPDWORD>(processed), nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64 *position) noexcept override {
        UInt64 ignored{};
        if (!position) position = &ignored;
        if (shouldCancel(cancel_)) return E_ABORT;
        LARGE_INTEGER move{};
        move.QuadPart = offset;
        LARGE_INTEGER result{};
        if (!SetFilePointerEx(file_, move, &result, origin)) return HRESULT_FROM_WIN32(GetLastError());
        if (result.QuadPart < 0) return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
        *position = static_cast<UInt64>(result.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSize(UInt64 *size) noexcept override {
        if (!size) return E_POINTER;
        *size = size_;
        return S_OK;
    }
};

class PasswordCallback : public ICryptoGetTextPassword {
protected:
    virtual ~PasswordCallback() = default;
protected:
    std::atomic<ULONG> passwordRefs_{1};
    std::wstring password_;
    bool passwordRequested_ = false;
    std::atomic_bool cancelled_{false};
    const Cancel *cancel_ = nullptr;

    PasswordCallback(const std::wstring &password, const Cancel *cancel) : password_(password), cancel_(cancel) {}
    HRESULT passwordQuery(BSTR *password) {
        if (!password) return E_POINTER;
        *password = nullptr;
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        passwordRequested_ = true;
        *password = SysAllocString(password_.c_str());
        return *password || password_.empty() ? S_OK : E_OUTOFMEMORY;
    }

public:
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR *password) noexcept override { return passwordQuery(password); }
    ULONG passwordAddRef() { return ++passwordRefs_; }
    ULONG passwordRelease() {
        const ULONG value = --passwordRefs_;
        if (value == 0) delete this;
        return value;
    }
};

class OpenCallback final : public IArchiveOpenCallback, public PasswordCallback {
    const Cancel *cancel_ = nullptr;
    const Progress *progress_ = nullptr;

public:
    OpenCallback(const std::wstring &password, const Cancel *cancel, const Progress *progress)
        : PasswordCallback(password, cancel), cancel_(cancel), progress_(progress) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) noexcept override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback)
            *object = static_cast<IArchiveOpenCallback *>(this);
        else if (iid == IID_ICryptoGetTextPassword)
            *object = static_cast<ICryptoGetTextPassword *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return passwordAddRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return passwordRelease(); }

    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64 *files, const UInt64 *bytes) noexcept override {
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        if (progress_ && *progress_) {
            try { (*progress_)(0, bytes ? *bytes : 0); }
            catch (...) { return E_FAIL; }
        }
        (void)files;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64 *files, const UInt64 *bytes) noexcept override {
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        if (progress_ && *progress_) {
            try { (*progress_)(bytes ? *bytes : 0, bytes ? *bytes : 0); }
            catch (...) { return E_FAIL; }
        }
        (void)files;
        return S_OK;
    }

    bool passwordRequested() const { return passwordRequested_; }
    bool cancelled() const { return cancelled_; }
};

class MemoryOutStream final : public ISequentialOutStream, public RefCounted {
    std::vector<uint8_t> &data_;
    uint64_t limit_;
    const Cancel *cancel_;
public:
    MemoryOutStream(std::vector<uint8_t> &data, uint64_t limit, const Cancel *cancel)
        : data_(data), limit_(limit), cancel_(cancel) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) noexcept override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream)
            *object = static_cast<ISequentialOutStream *>(this);
        else return E_NOINTERFACE;
        addRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return release(); }
    HRESULT STDMETHODCALLTYPE Write(const void *data, UInt32 count, UInt32 *processed) noexcept override {
        UInt32 ignored{};
        if (!processed) processed = &ignored;
        *processed = 0;
        if (shouldCancel(cancel_)) return E_ABORT;
        if (count > limit_ - std::min<uint64_t>(limit_, data_.size())) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        try {
            const auto *bytes = static_cast<const uint8_t *>(data);
            data_.insert(data_.end(), bytes, bytes + count);
        } catch (...) { return E_OUTOFMEMORY; }
        *processed = count;
        return S_OK;
    }
};

class ExtractCallback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
    std::atomic<ULONG> refs_{1};
    std::map<uint32_t, std::vector<uint8_t> *> outputs_;
    std::map<uint32_t, MemoryOutStream *> streams_;
    std::wstring password_;
    const Cancel *cancel_;
    const Progress *progress_;
    bool passwordRequested_ = false;
    std::atomic_bool cancelled_{false};
    Int32 operationResult_ = NArchive::NExtract::NOperationResult::kOK;
    std::map<uint32_t, Int32> operationResults_;
    uint32_t currentIndex_ = std::numeric_limits<uint32_t>::max();
    UInt64 total_ = 0;
    UInt64 completed_ = 0;
    std::function<void(uint32_t, std::vector<uint8_t>&)> completedOutput_;

public:
    ExtractCallback(const std::vector<std::pair<uint32_t, std::vector<uint8_t> *>> &outputs,
        const std::wstring &password, const Cancel *cancel, const Progress *progress,
        std::function<void(uint32_t, std::vector<uint8_t>&)> completedOutput)
        : password_(password), cancel_(cancel), progress_(progress), completedOutput_(std::move(completedOutput)) {
        for (const auto &entry : outputs) outputs_[entry.first] = entry.second;
    }
    ~ExtractCallback() {
        for (auto &[index, stream] : streams_) { (void)index; stream->Release(); }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) noexcept override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback || iid == IID_IProgress)
            *object = static_cast<IArchiveExtractCallback *>(this);
        else if (iid == IID_ICryptoGetTextPassword)
            *object = static_cast<ICryptoGetTextPassword *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG value = --refs_;
        if (!value) delete this;
        return value;
    }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR *password) noexcept override {
        if (!password) return E_POINTER;
        *password = nullptr;
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        passwordRequested_ = true;
        *password = SysAllocString(password_.c_str());
        return *password || password_.empty() ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) noexcept override {
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        total_ = total;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64 *completed) noexcept override {
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        completed_ = completed ? *completed : 0;
        if (progress_ && *progress_) {
            try { (*progress_)(completed_, total_); }
            catch (...) { return E_FAIL; }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream **out, Int32 askMode) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        currentIndex_ = std::numeric_limits<uint32_t>::max();
        if (shouldCancel(cancel_)) { cancelled_ = true; return E_ABORT; }
        if (askMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
        const auto found = outputs_.find(index);
        if (found == outputs_.end()) return S_OK;
        currentIndex_ = index;
        auto *stream = new (std::nothrow) MemoryOutStream(*found->second, kMaxImageSize, cancel_);
        if (!stream) return E_OUTOFMEMORY;
        try { streams_[index] = stream; }
        catch (...) { delete stream; return E_OUTOFMEMORY; }
        stream->AddRef();
        *out = stream;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        if (result != NArchive::NExtract::NOperationResult::kOK) operationResult_ = result;
        if (currentIndex_ != std::numeric_limits<uint32_t>::max()) {
            try {
                operationResults_[currentIndex_] = result;
                if (result == NArchive::NExtract::NOperationResult::kOK && completedOutput_)
                    completedOutput_(currentIndex_, *outputs_.at(currentIndex_));
            }
            catch (...) { return E_OUTOFMEMORY; }
        }
        currentIndex_ = std::numeric_limits<uint32_t>::max();
        return S_OK;
    }
    bool passwordRequested() const { return passwordRequested_; }
    bool cancelled() const { return cancelled_; }
    Int32 operationResult() const { return operationResult_; }
    Int32 operationResult(uint32_t index) const {
        const auto found = operationResults_.find(index);
        return found == operationResults_.end() ? NArchive::NExtract::NOperationResult::kUnavailable : found->second;
    }
};

struct CacheRecord {
    fs::path path;
    uint64_t size = 0;
    uint64_t used = 0;
};

class ArchiveSource final : public BookSource {
    struct Item {
        PageEntry entry;
        bool encrypted = false;
        bool split = false;
    };
    HMODULE module_ = nullptr;
    IInArchive *archive_ = nullptr;
    fs::path archivePath_;
    fs::path cacheRoot_;
    std::wstring password_;
    Cancel cancel_;
    Progress progress_;
    std::vector<Item> items_;
    std::vector<PageEntry> entries_;
    std::map<uint32_t, CacheRecord> cache_;
    uint64_t cacheBytes_ = 0;
    uint64_t cacheClock_ = 0;
    std::vector<fs::path> ownedCacheFiles_;
    bool solid_ = false;

    static GUID formatGuid(const fs::path &path) {
        const auto extension = path.extension().wstring();
        const auto ext = lower(extension);
        uint8_t format = ext == L".7z" ? 0x07 : ext == L".rar" || ext == L".cbr" ? 0x03 : 0x01;
        if (format == 0x03) {
            std::ifstream input(path, std::ios::binary);
            std::array<unsigned char, 8> signature{};
            input.read(reinterpret_cast<char *>(signature.data()), static_cast<std::streamsize>(signature.size()));
            if (input.gcount() == static_cast<std::streamsize>(signature.size()) && signature == std::array<unsigned char,8>{'R','a','r','!',0x1a,0x07,0x01,0x00}) format = 0xCC;
        }
        return GUID{0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01, 0x10, format, 0x00, 0x00}};
    }
    static std::wstring cacheName(const fs::path &archive, uint32_t index) {
        std::wstring text = archive.wstring() + L"#" + std::to_wstring(index);
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t c : text) { hash ^= static_cast<uint16_t>(c); hash *= 1099511628211ull; }
        std::wstringstream ss;
        ss << std::hex << std::setw(16) << std::setfill(L'0') << hash << L".cvpage";
        return ss.str();
    }
    void clearCache() noexcept {
        for (const auto &path : ownedCacheFiles_) {
            std::error_code ec;
            fs::remove(path, ec);
        }
        ownedCacheFiles_.clear();
        cache_.clear();
        cacheBytes_ = 0;
    }
    void unload() noexcept {
        if (archive_) { archive_->Close(); archive_->Release(); archive_ = nullptr; }
        if (module_) { FreeLibrary(module_); module_ = nullptr; }
    }
    bool pruneCache(uint64_t incoming) {
        while (cacheBytes_ + incoming > kMaxCacheSize && !cache_.empty()) {
            auto victim = cache_.begin();
            for (auto it = cache_.begin(); it != cache_.end(); ++it)
                if (it->second.used < victim->second.used) victim = it;
            std::error_code ec;
            fs::remove(victim->second.path, ec);
            if (ec) return false;
            cacheBytes_ -= victim->second.size;
            cache_.erase(victim);
        }
        return cacheBytes_ + incoming <= kMaxCacheSize;
    }
    void cacheOutput(uint32_t index, const std::vector<uint8_t> &data) {
        if (cacheRoot_.empty() || data.empty()) return;
        std::error_code ec;
        fs::create_directories(cacheRoot_, ec);
        if (ec) return;
        const fs::path target = cacheRoot_ / cacheName(archivePath_, index);
        const auto own = std::find(ownedCacheFiles_.begin(), ownedCacheFiles_.end(), target);
        if (fs::exists(target, ec) && own == ownedCacheFiles_.end()) return;
        auto old = cache_.find(index);
        if (old != cache_.end()) {
            fs::remove(old->second.path, ec);
            if (ec) return;
            cacheBytes_ -= old->second.size; cache_.erase(old);
        }
        if (!pruneCache(data.size())) return;
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) return;
        if (own == ownedCacheFiles_.end()) ownedCacheFiles_.push_back(target);
        output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!output) { output.close(); fs::remove(target, ec); return; }
        cache_[index] = CacheRecord{target, data.size(), ++cacheClock_};
        cacheBytes_ += data.size();
    }
    bool readCached(uint32_t index, std::vector<uint8_t> &data) {
        auto found = cache_.find(index);
        if (found == cache_.end()) return false;
        std::ifstream input(found->second.path, std::ios::binary);
        if (!input) { cacheBytes_ -= found->second.size; cache_.erase(found); return false; }
        try {
            data.resize(static_cast<size_t>(found->second.size));
            input.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
            if (input.gcount() != static_cast<std::streamsize>(data.size())) { data.clear(); return false; }
        } catch (...) { return false; }
        found->second.used = ++cacheClock_;
        return true;
    }
    std::vector<uint8_t> extract(const std::vector<uint32_t> &indices, uint32_t requested) {
        checkCancel(cancel_);
        std::map<uint32_t, std::vector<uint8_t>> result;
        std::vector<std::pair<uint32_t, std::vector<uint8_t> *>> outputs;
        for (uint32_t index : indices) outputs.emplace_back(index, &result[index]);
        auto *callback = new (std::nothrow) ExtractCallback(outputs, password_, &cancel_, &progress_,
            [&](uint32_t index, std::vector<uint8_t>& bytes) {
                if (solid_) cacheOutput(index, bytes);
                if (index != requested) std::vector<uint8_t>().swap(bytes);
            });
        if (!callback) fail("归档解压内存不足");
        HRESULT hr = archive_->Extract(indices.data(), static_cast<UInt32>(indices.size()), 0, callback);
        const bool password = callback->passwordRequested();
        const bool cancelled = callback->cancelled();
        const Int32 operationResult = callback->operationResult();
        const Int32 requestedResult = callback->operationResult(requested);
        bool encrypted = false;
        for (const auto &item : items_) {
            if (item.entry.index == requested) { encrypted = item.encrypted; break; }
        }
        callback->Release();
        const bool extractionFailed = FAILED(hr) || hr == S_FALSE;
        if (cancelled || (extractionFailed && hr == E_ABORT && shouldCancel(&cancel_))) throw Cancelled{};
        if (requestedResult == NArchive::NExtract::NOperationResult::kWrongPassword ||
            (encrypted && (requestedResult == NArchive::NExtract::NOperationResult::kCRCError || requestedResult == NArchive::NExtract::NOperationResult::kDataError)) ||
            (password && extractionFailed && requestedResult != NArchive::NExtract::NOperationResult::kOK))
            throw PasswordRequired{};
        if (extractionFailed && requestedResult != NArchive::NExtract::NOperationResult::kOK) {
            if (FAILED(hr)) checkHr(hr, "归档解压失败");
            fail("归档解压失败");
        }
        if (operationResult != NArchive::NExtract::NOperationResult::kOK &&
            requestedResult != NArchive::NExtract::NOperationResult::kOK) fail("归档数据损坏或无法解压");
        auto found = result.find(requested);
        if (found == result.end()) fail("归档条目不存在");
        return std::move(found->second);
    }

public:
    ArchiveSource(const fs::path &path, const std::wstring &password, const fs::path &cacheRoot,
        const Cancel &cancel, const Progress &progress)
        : archivePath_(path), cacheRoot_(cacheRoot), password_(password), cancel_(cancel), progress_(progress) {
        const fs::path absolute = fs::absolute(path);
        const fs::path dll = executableDir() / L"7z.dll";
        if (!fs::is_regular_file(dll)) fail("找不到程序目录中的 7z.dll");
        module_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) fail("无法加载 7z.dll");
        auto create = reinterpret_cast<Func_CreateObject>(GetProcAddress(module_, "CreateObject"));
        if (!create) { unload(); fail("7z.dll 接口不可用"); }
        const GUID clsid = formatGuid(absolute);
        HRESULT hr = create(&clsid, &IID_IInArchive, reinterpret_cast<void **>(&archive_));
        if (FAILED(hr) || !archive_) { unload(); fail("不支持的归档格式"); }
        FileInStream *stream = nullptr;
        try {
            stream = new FileInStream(absolute, &cancel_);
            auto *callback = new (std::nothrow) OpenCallback(password_, &cancel_, &progress_);
            if (!callback) { stream->Release(); stream = nullptr; fail("归档打开内存不足"); }
            hr = archive_->Open(stream, nullptr, callback);
            stream->Release();
            stream = nullptr;
            if (FAILED(hr) || hr == S_FALSE) {
                const bool cancelled = callback->cancelled();
                const bool wasPasswordRequested = callback->passwordRequested();
                callback->Release();
                unload();
                if (cancelled || (hr == E_ABORT && shouldCancel(&cancel_))) throw Cancelled{};
                if (wasPasswordRequested || hr == E_ABORT) throw PasswordRequired{};
                if (hr == S_FALSE) fail("无法打开归档");
                checkHr(hr, "无法打开归档");
            }
            callback->Release();
            UInt32 count = 0;
            checkHr(archive_->GetNumberOfItems(&count), "无法读取归档目录");
            items_.reserve(count);
            for (UInt32 index = 0; index < count; ++index) {
                checkCancel(cancel_);
                PROPVARIANT pathValue{};
                PROPVARIANT sizeValue{};
                PROPVARIANT dirValue{};
                PROPVARIANT encryptedValue{};
                PROPVARIANT beforeValue{};
                PROPVARIANT afterValue{};
                hr = archive_->GetProperty(index, kpidPath, &pathValue);
                if (FAILED(hr)) { PropVariantClear(&pathValue); fail("无法读取归档条目名称"); }
                archive_->GetProperty(index, kpidSize, &sizeValue);
                archive_->GetProperty(index, kpidIsDir, &dirValue);
                archive_->GetProperty(index, kpidEncrypted, &encryptedValue);
                archive_->GetProperty(index, kpidSplitBefore, &beforeValue);
                archive_->GetProperty(index, kpidSplitAfter, &afterValue);
                const bool isDir = dirValue.vt == VT_BOOL && dirValue.boolVal != VARIANT_FALSE;
                const bool split = beforeValue.vt == VT_BOOL && beforeValue.boolVal != VARIANT_FALSE ||
                    afterValue.vt == VT_BOOL && afterValue.boolVal != VARIANT_FALSE;
                if (!isDir && pathValue.vt == VT_BSTR && pathValue.bstrVal && isImage(fs::path(pathValue.bstrVal))) {
                    Item item;
                    item.entry.name.assign(pathValue.bstrVal, SysStringLen(pathValue.bstrVal));
                    item.entry.index = index;
                    item.entry.size = sizeValue.vt == VT_UI8 ? sizeValue.uhVal.QuadPart : 0;
                    item.encrypted = encryptedValue.vt == VT_BOOL && encryptedValue.boolVal != VARIANT_FALSE;
                    item.split = split;
                    if (item.entry.size > kMaxImageSize) { PropVariantClear(&pathValue); PropVariantClear(&sizeValue); PropVariantClear(&dirValue); PropVariantClear(&encryptedValue); PropVariantClear(&beforeValue); PropVariantClear(&afterValue); fail("图片解压大小超过 128 MiB 限制"); }
                    items_.push_back(std::move(item));
                }
                PropVariantClear(&pathValue); PropVariantClear(&sizeValue); PropVariantClear(&dirValue);
                PropVariantClear(&encryptedValue); PropVariantClear(&beforeValue); PropVariantClear(&afterValue);
            }
            for (const auto &item : items_) if (item.split) { unload(); fail("不支持分卷归档"); }
            std::sort(items_.begin(), items_.end(), [](const Item &a, const Item &b) {
                if (naturalLess(a.entry.name, b.entry.name)) return true;
                if (naturalLess(b.entry.name, a.entry.name)) return false;
                return a.entry.index < b.entry.index;
            });
            for (const auto &item : items_) entries_.push_back(item.entry);
            PROPVARIANT solidValue{};
            if (SUCCEEDED(archive_->GetArchiveProperty(kpidSolid, &solidValue)) &&
                solidValue.vt == VT_BOOL && solidValue.boolVal != VARIANT_FALSE) solid_ = true;
            PropVariantClear(&solidValue);
            if (entries_.empty()) { unload(); fail("归档中没有图片"); }
        } catch (...) {
            if (stream) stream->Release();
            unload();
            throw;
        }
    }
    ~ArchiveSource() override { clearCache(); unload(); }
    const std::vector<PageEntry> &entries() const override { return entries_; }
    bool solid() const override { return solid_; }
    bool encrypted() const override { return std::any_of(items_.begin(), items_.end(), [](const auto& item) { return item.encrypted; }); }
    std::vector<uint8_t> read(size_t page, const Cancel &cancel, const Progress &progress) override {
        if (page >= items_.size()) fail("图片页码超出范围");
        cancel_ = cancel;
        progress_ = progress;
        const uint32_t requested = items_[page].entry.index;
        std::vector<uint8_t> cached;
        if (solid_ && readCached(requested, cached)) return cached;
        std::vector<uint32_t> indices{requested};
        if (solid_) {
            uint32_t last = requested;
            if (page + 1 < items_.size()) last = std::max(last, items_[page + 1].entry.index);
            for (const auto& item : items_) if (item.entry.index <= last && !cache_.contains(item.entry.index)) indices.push_back(item.entry.index);
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        }
        const auto data = extract(indices, requested);
        return data;
    }
};

class DirectorySource final : public BookSource {
    fs::path root_;
    std::vector<PageEntry> entries_;
public:
    DirectorySource(const fs::path &root, const Cancel &cancel) : root_(root) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            checkCancel(cancel);
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec) || ec || !isImage(it->path())) continue;
            PageEntry entry;
            entry.name = fs::relative(it->path(), root, ec).wstring();
            if (ec) { ec.clear(); entry.name = it->path().filename().wstring(); }
            entry.index = static_cast<uint32_t>(entries_.size());
            entry.size = it->file_size(ec);
            if (ec) { ec.clear(); entry.size = 0; }
            entries_.push_back(std::move(entry));
        }
        std::sort(entries_.begin(), entries_.end(), [](const PageEntry &a, const PageEntry &b) {
            if (naturalLess(a.name, b.name)) return true;
            if (naturalLess(b.name, a.name)) return false;
            return a.index < b.index;
        });
        if (entries_.empty()) fail("目录中没有图片");
    }
    const std::vector<PageEntry> &entries() const override { return entries_; }
    bool solid() const override { return false; }
    std::vector<uint8_t> read(size_t page, const Cancel &cancel, const Progress &progress) override {
        if (page >= entries_.size()) fail("图片页码超出范围");
        const auto path = root_ / fs::path(entries_[page].name);
        std::ifstream input(path, std::ios::binary);
        if (!input) fail("无法读取图片文件");
        const uint64_t size = entries_[page].size;
        if (size > kMaxImageSize) fail("图片读取大小超过 128 MiB 限制");
        std::vector<uint8_t> data(static_cast<size_t>(size));
        uint64_t done = 0;
        while (done < size) {
            checkCancel(cancel);
            const auto chunk = static_cast<std::streamsize>(std::min<uint64_t>(size - done, 1u << 20));
            input.read(reinterpret_cast<char *>(data.data() + done), chunk);
            const auto got = input.gcount();
            if (got <= 0) fail("读取图片文件失败");
            done += static_cast<uint64_t>(got);
            if (progress) progress(done, size);
        }
        return data;
    }
};

} // namespace

std::unique_ptr<BookSource> openBook(const fs::path &path, const std::wstring &password,
    const fs::path &cacheRoot, const Cancel &cancel, const Progress &progress) {
    checkCancel(cancel);
    std::error_code ec;
    if (fs::is_directory(path, ec)) return std::make_unique<DirectorySource>(path, cancel);
    if (!fs::is_regular_file(path, ec)) fail("文件或目录不存在");
    if (!isArchive(path)) fail("不支持的文件格式");
    return std::make_unique<ArchiveSource>(path, password, cacheRoot, cancel, progress);
}

} // namespace cv
