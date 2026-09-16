#include "reader.hpp"
#include <chrono>
#include <deque>
#include <iostream>

using namespace cv;
using namespace std::chrono;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        auto root = fs::path(__FILE__).parent_path().parent_path() / "test-output" / "fixtures";
        auto dir = root / (L"reader-" + std::to_wstring(GetCurrentProcessId())); fs::create_directories(dir);
        Store store(dir / "test.db");
        std::mutex mutex; std::condition_variable ready; std::deque<std::unique_ptr<ReaderEvent>> events;
        auto wait = [&](uint64_t generation) {
            std::unique_lock lock(mutex); auto deadline = steady_clock::now() + seconds(30);
            for (;;) {
                while (!events.empty()) { auto e = std::move(events.front()); events.pop_front(); if (e->generation == generation && e->kind != ReaderEvent::ProgressUpdate) return e; }
                if (ready.wait_until(lock, deadline) == std::cv_status::timeout) throw std::runtime_error("阅读线程等待超时");
            }
        };
        {
            Reader reader(store,dir/"cache",[&](auto event) { std::lock_guard lock(mutex); events.push_back(std::move(event)); ready.notify_one(); });
            auto id = store.ensureBook(root/L"漫画.cbz");
            auto event = wait(reader.open(root/L"漫画.cbz",id,{})); require(event->kind == ReaderEvent::Opened, "普通包打开失败");
            auto state = event->state; state.mode = 1;
            event = wait(reader.load(0,state)); require(event->first && event->second && event->page == 0, "双页失败");
            event = wait(reader.load(2,state)); require(event->first && !event->second, "横图应单独显示");
            event = wait(reader.load(1,state,true)); require(event->page == 0 && event->second, "双页回翻失败");
            state.mode = 2;
            event = wait(reader.load(2,state,true)); require(event->state.half == 1, "分页回翻位置错误");
            state.mode = 0;
            for (int i = 0; i < 30; ++i) reader.load(i%6,state);
            event = wait(reader.load(4,state)); require(event->page == 4, "快速切页没有采用最新请求");
            auto start = steady_clock::now(); event = wait(reader.load(4,state)); auto cached = duration<double,std::milli>(steady_clock::now()-start).count();
            std::cout << "缓存页面返回耗时(ms): " << cached << std::endl;
            auto protectedId = store.ensureBook(root/L"encrypted.7z");
            event = wait(reader.open(root/L"encrypted.7z",protectedId,{})); require(event->kind == ReaderEvent::NeedPassword, "应请求密码");
            event = wait(reader.open(root/L"encrypted.7z",protectedId,{},L"test",false)); require(event->kind == ReaderEvent::Opened && store.passwords().empty(), "未勾选保存仍写入密码");
            event = wait(reader.open(root/L"encrypted.7z",protectedId,{},L"test",true)); require(event->kind == ReaderEvent::Opened && store.bookPassword(protectedId)==L"test", "密码保存失败");
            event = wait(reader.open(root/L"encrypted.7z",protectedId,{})); require(event->kind == ReaderEvent::Opened, "保存密码未自动解锁");
            DWORD beforeHandles{}, afterHandles{};
            GetProcessHandleCount(GetCurrentProcess(), &beforeHandles);
            for (int i = 0; i < 20; ++i) {
                event = wait(reader.open(root/L"漫画.cbz",id,{})); require(event->kind == ReaderEvent::Opened, "连续开书失败");
                event = wait(reader.open(root/L"encrypted.7z",protectedId,{})); require(event->kind == ReaderEvent::Opened, "连续切换加密书失败");
            }
            GetProcessHandleCount(GetCurrentProcess(), &afterHandles);
            require(afterHandles <= beforeHandles + 4, "反复切书后句柄持续增加");
            reader.cancel();
        }
        std::cout << "阅读调度集成测试通过" << std::endl; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
