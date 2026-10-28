#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <ctime>
#include <chrono>

namespace aes67 {
static LogLevel g_level = LogLevel::Info;
static std::function<void(LogLevel, const std::string&)> g_sink;
static std::string g_file;
static std::mutex g_mu;

void log_set_level(LogLevel l) { g_level = l; }
void log_set_sink(std::function<void(LogLevel, const std::string&)> sink) { std::lock_guard<std::mutex> g(g_mu); g_sink = std::move(sink); }
void log_set_file(const std::string& path) { std::lock_guard<std::mutex> g(g_mu); g_file = path; }

void logf(LogLevel l, const char* fmt, ...) {
    if (l < g_level) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::lock_guard<std::mutex> g(g_mu);
    if (g_sink) { g_sink(l, buf); return; }
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    char ts[32]; std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof ts, "%H:%M:%S", &tmv);
    const char* lv = l == LogLevel::Debug ? "D" : l == LogLevel::Info ? "I" : l == LogLevel::Warn ? "W" : "E";
    fprintf(stderr, "%s.%03d %s %s\n", ts, ms, lv, buf);
    fflush(stderr);
    if (!g_file.empty()) {
        FILE* f = fopen(g_file.c_str(), "a");
        if (f) { fprintf(f, "%s.%03d %s %s\n", ts, ms, lv, buf); fclose(f); }
    }
}
}
