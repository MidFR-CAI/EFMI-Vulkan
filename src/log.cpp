#include "log.h"

#include <windows.h>

#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace efmivulkan {
namespace {

constexpr size_t kMaxQueuedLines = 20000;

// Heap allocated and never freed: the writer threads outlive static destructors.
struct State {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> lines;
    size_t dropped = 0;
    std::mutex jobMu;
    std::condition_variable jobCv;
    std::deque<std::function<void()>> jobs;
    std::wstring dir;
    HANDLE file = INVALID_HANDLE_VALUE;
};
State* g = nullptr;

void WriteLines(const std::vector<std::string>& lines) {
    for (const auto& l : lines) {
        DWORD written = 0;
        WriteFile(g->file, l.data(), static_cast<DWORD>(l.size()), &written, nullptr);
    }
}

DWORD WINAPI WriterThread(void*) {
    std::vector<std::string> batch;
    for (;;) {
        size_t dropped = 0;
        {
            std::unique_lock lock(g->mu);
            g->cv.wait_for(lock, std::chrono::milliseconds(100));
            batch.swap(g->lines);
            dropped = g->dropped;
            g->dropped = 0;
        }
        if (dropped) batch.push_back("[log] dropped " + std::to_string(dropped) + " lines\r\n");
        if (!batch.empty()) {
            WriteLines(batch);
            batch.clear();
        }
    }
}

DWORD WINAPI JobThread(void*) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(g->jobMu);
            g->jobCv.wait(lock, [] { return !g->jobs.empty(); });
            job = std::move(g->jobs.front());
            g->jobs.pop_front();
        }
        try {
            job();
        } catch (...) {
            LogError("job threw an exception");
        }
    }
}

void Emit(const char* level, const char* fmt, va_list args) {
    if (!g) return;
    char buf[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u [%s] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, level);
    if (n < 0) return;
    int m = vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, args);
    if (m < 0) return;
    size_t len = std::min<size_t>(n + m, sizeof(buf) - 3);
    buf[len++] = '\r';
    buf[len++] = '\n';
    std::string line(buf, len);
    {
        std::lock_guard lock(g->mu);
        if (g->lines.size() >= kMaxQueuedLines) {
            ++g->dropped;
            return;
        }
        g->lines.push_back(std::move(line));
    }
}

}  // namespace

void LogInit() {
    g = new State();
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"EFMIVULKAN_LOG_DIR", buf, MAX_PATH);
    if (n && n < MAX_PATH) {
        g->dir = buf;
    } else {
        n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        g->dir = std::wstring(buf, n) + L"\\efmivulkan";
    }
    CreateDirectoryW(g->dir.c_str(), nullptr);
    g->file = CreateFileW((g->dir + L"\\efmivulkan.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CloseHandle(CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, JobThread, nullptr, 0, nullptr));
}

void LogInfo(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    Emit("info", fmt, a);
    va_end(a);
}

void LogError(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    Emit("ERROR", fmt, a);
    va_end(a);
}

void PostJob(std::function<void()> fn) {
    if (!g) return;
    {
        std::lock_guard lock(g->jobMu);
        g->jobs.push_back(std::move(fn));
    }
    g->jobCv.notify_one();
}

const std::wstring& LogDir() { return g->dir; }

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace efmivulkan
