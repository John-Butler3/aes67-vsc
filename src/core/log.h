#pragma once
#include <string>
#include <functional>

namespace aes67 {
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };
void log_set_level(LogLevel l);
void log_set_sink(std::function<void(LogLevel, const std::string&)> sink); // default: stderr + log file
void log_set_file(const std::string& path);  // empty disables file logging
void logf(LogLevel l, const char* fmt, ...);
#define LOG_D(...) ::aes67::logf(::aes67::LogLevel::Debug, __VA_ARGS__)
#define LOG_I(...) ::aes67::logf(::aes67::LogLevel::Info, __VA_ARGS__)
#define LOG_W(...) ::aes67::logf(::aes67::LogLevel::Warn, __VA_ARGS__)
#define LOG_E(...) ::aes67::logf(::aes67::LogLevel::Error, __VA_ARGS__)
}
