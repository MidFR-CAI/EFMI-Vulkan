// Background logging. Nothing here touches the disk on the caller's thread.
#pragma once
#include <functional>
#include <string>

namespace efmivulkan {

// Creates the log directory and starts the writer threads. Call once.
void LogInit();

void LogInfo(const char* fmt, ...);
void LogError(const char* fmt, ...);

// Runs fn on the job thread, for work too slow for the render thread.
void PostJob(std::function<void()> fn);

const std::wstring& LogDir();

std::string ToUtf8(const std::wstring& s);

}  // namespace efmivulkan
