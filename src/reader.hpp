#pragma once
#include "archive.hpp"
#include "image.hpp"
#include "store.hpp"
#include <condition_variable>
#include <map>
#include <optional>
#include <thread>

namespace cv {
struct ReaderEvent {
    enum Kind { Opened, Loaded, Error, NeedPassword, ProgressUpdate } kind{Loaded};
    uint64_t generation{};
    int page{};
    ReadingState state;
    std::vector<PageEntry> entries;
    std::shared_ptr<Pixels> first, second;
    std::wstring message;
    bool solid{};
};
class Reader {
public:
    using Notify = std::function<void(std::unique_ptr<ReaderEvent>)>;
    Reader(Store& store, fs::path cache, Notify notify);
    ~Reader();
    uint64_t open(fs::path path, std::string id, ReadingState state, std::optional<std::wstring> password = {}, bool remember = false, int64_t replacePassword = 0);
    uint64_t load(int page, ReadingState state, bool backwards = false);
    uint64_t cancel();
private:
    struct Request {
        bool open{}, backwards{}, remember{};
        uint64_t generation{};
        fs::path path;
        std::string id;
        ReadingState state;
        std::optional<std::wstring> password;
        int64_t replacePassword{};
        int page{};
    };
    uint64_t enqueue(Request request);
    void run();
    void process(const Request& request);
    std::shared_ptr<Pixels> pixels(int page, const Cancel& cancelled, const Progress& progress);
    Store& store_;
    fs::path cacheRoot_;
    Notify notify_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::atomic<uint64_t> generation_{};
    bool stopping_{};
    std::thread worker_;
    std::unique_ptr<BookSource> source_;
    std::map<int, std::shared_ptr<Pixels>> cache_;
    int activePage_{};
};
}
