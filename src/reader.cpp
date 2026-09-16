#include "reader.hpp"
#include <chrono>
#include <set>

namespace cv {
Reader::Reader(Store& store, fs::path cache, Notify notify) : store_(store), cacheRoot_(std::move(cache)), notify_(std::move(notify)), worker_([this] { run(); }) {}
Reader::~Reader() {
    { std::lock_guard lock(mutex_); stopping_ = true; ++generation_; pending_.reset(); }
    wake_.notify_one();
    worker_.join();
}
uint64_t Reader::enqueue(Request request) {
    std::lock_guard lock(mutex_);
    request.generation = ++generation_;
    pending_ = std::move(request);
    wake_.notify_one();
    return generation_;
}
uint64_t Reader::open(fs::path path, std::string id, ReadingState state, std::optional<std::wstring> password, bool remember, int64_t replacePassword) {
    Request request; request.open = true; request.path = std::move(path); request.id = std::move(id); request.state = state;
    request.page = state.page; request.password = std::move(password); request.remember = remember; request.replacePassword = replacePassword;
    return enqueue(std::move(request));
}
uint64_t Reader::load(int page, ReadingState state, bool backwards) {
    Request request; request.page = page; request.state = state; request.backwards = backwards;
    return enqueue(std::move(request));
}
uint64_t Reader::cancel() {
    std::lock_guard lock(mutex_); pending_.reset(); return ++generation_;
}
std::shared_ptr<Pixels> Reader::pixels(int page, const Cancel& cancelled, const Progress& progress) {
    checkCancel(cancelled);
    if (auto it = cache_.find(page); it != cache_.end()) return it->second;
    constexpr size_t budget = 96u * 1024 * 1024;
    constexpr size_t decodeReserve = 24u * 1024 * 1024;
    const auto entryBytes = source_->entries()[page].size;
    if (entryBytes > budget - decodeReserve) throw std::runtime_error("此图片压缩数据过大，超过内存预算");
    auto used = [&] { size_t n = static_cast<size_t>(entryBytes) + decodeReserve; for (auto& [i, p] : cache_) n += p->bytes(); return n; };
    while (!cache_.empty() && used() > budget) {
        auto victim = std::max_element(cache_.begin(), cache_.end(), [&](auto& a, auto& b) { return std::abs(a.first - activePage_) < std::abs(b.first - activePage_); });
        cache_.erase(victim);
    }
    auto bytes = source_->read(page, cancelled, progress);
    if (bytes.size() > budget - decodeReserve) throw std::runtime_error("此图片压缩数据过大，超过内存预算");
    auto result = decodeImage(bytes, cancelled);
    cache_[page] = result;
    return result;
}
void Reader::run() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Request request;
        { std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); }); if (stopping_) break; request = std::move(*pending_); pending_.reset(); }
        try { process(request); }
        catch (const Cancelled&) {}
        catch (const PasswordRequired&) {
            source_.reset(); cache_.clear();
            if (generation_ == request.generation) { auto e = std::make_unique<ReaderEvent>(); e->kind = ReaderEvent::NeedPassword; e->generation = request.generation; notify_(std::move(e)); }
        }
        catch (const std::exception& error) {
            if (generation_ == request.generation) { auto e = std::make_unique<ReaderEvent>(); e->kind = ReaderEvent::Error; e->generation = request.generation; e->message = wide(error.what()); notify_(std::move(e)); }
        }
    }
    cache_.clear(); source_.reset();
    if (SUCCEEDED(com)) CoUninitialize();
}
void Reader::process(const Request& request) {
    const Cancel cancelled = [&] { return generation_ != request.generation; };
    auto last = std::chrono::steady_clock::now();
    std::mutex progressMutex;
    const Progress progress = [&](uint64_t done, uint64_t total) {
        std::lock_guard progressLock(progressMutex);
        checkCancel(cancelled);
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(150)) return;
        last = now;
        auto event = std::make_unique<ReaderEvent>(); event->kind = ReaderEvent::ProgressUpdate; event->generation = request.generation;
        event->message = L"正在读取… " + (total ? std::to_wstring(std::min<uint64_t>(100, done * 100 / total)) + L"%" : std::to_wstring(done / 1024) + L" KiB");
        notify_(std::move(event));
    };
    ReadingState state = request.state;
    int page = request.page;
    if (request.open) {
        source_.reset(); cache_.clear();
        std::vector<std::wstring> passwords;
        if (request.password) passwords.push_back(*request.password);
        else {
            passwords.push_back(store_.bookPassword(request.id));
            for (auto& password : store_.passwords()) if (std::find(passwords.begin(), passwords.end(), password.value) == passwords.end()) passwords.push_back(password.value);
        }
        bool opened = false;
        for (const auto& password : passwords) {
            checkCancel(cancelled);
            try {
                source_ = openBook(request.path, password, cacheRoot_, cancelled, progress);
                if (source_->entries().empty()) throw std::runtime_error("没有找到支持的图片");
                auto& entries = source_->entries();
                page = std::clamp(state.page, 0, static_cast<int>(entries.size()) - 1);
                for (int i = 0; i < static_cast<int>(entries.size()); ++i) if (entries[i].name == state.entry) { page = i; break; }
                activePage_ = page;
                pixels(page, cancelled, progress); // Content-encrypted ZIP/RAR is validated here.
                checkCancel(cancelled);
                if (request.replacePassword) {
                    if (!source_->encrypted()) throw std::runtime_error("当前漫画未加密，无法用于验证密码修改");
                    store_.updatePassword(request.replacePassword, password);
                    store_.rememberPassword(request.id, password);
                } else if (source_->encrypted() && request.password && request.remember && !password.empty()) store_.rememberPassword(request.id, password);
                else if (source_->encrypted() && !request.password && !password.empty()) store_.rememberPassword(request.id, password);
                opened = true; break;
            } catch (const PasswordRequired&) { source_.reset(); cache_.clear(); }
        }
        if (!opened) throw PasswordRequired{};
    }
    if (!source_) throw std::runtime_error("请先打开漫画");
    page = std::clamp(page, 0, static_cast<int>(source_->entries().size()) - 1);
    activePage_ = page;
    auto first = pixels(page, cancelled, progress);
    if (request.backwards && state.mode == 1 && first->height > first->width && page > 0) {
        auto previous = pixels(page - 1, cancelled, progress);
        if (previous->height > previous->width) { --page; first = previous; }
    }
    std::shared_ptr<Pixels> second;
    if (state.mode == 1 && first->height > first->width && page + 1 < static_cast<int>(source_->entries().size())) {
        auto next = pixels(page + 1, cancelled, progress);
        if (next->height > next->width) second = next;
    }
    checkCancel(cancelled);
    state.page = page; state.entry = source_->entries()[page].name;
    if (request.backwards && state.mode == 2 && first->width > first->height) state.half = 1;
    if (state.mode != 2 || first->width <= first->height) state.half = 0;
    auto event = std::make_unique<ReaderEvent>(); event->kind = request.open ? ReaderEvent::Opened : ReaderEvent::Loaded;
    event->generation = request.generation; event->page = page; event->state = state; event->first = first; event->second = second; event->solid = source_->solid();
    if (request.open) event->entries = source_->entries();
    notify_(std::move(event));
    // Preload only immediate neighbours. Errors in speculative reads do not replace the displayed page.
    for (int neighbour : {page + (second ? 2 : 1), page - 1}) {
        if (neighbour < 0 || neighbour >= static_cast<int>(source_->entries().size()) || cancelled()) continue;
        try { pixels(neighbour, cancelled, {}); } catch (...) {}
    }
}
}
