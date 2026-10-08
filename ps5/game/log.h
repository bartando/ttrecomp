// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>
#include <spdlog/sinks/base_sink.h>

namespace tabletennis::ps5 {
inline int log_fd = -1;

inline void Write(int fd, const char* bytes, size_t size) {
  while (fd >= 0 && size) {
    const auto written = write(fd, bytes, size);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return;
    bytes += written;
    size -= size_t(written);
  }
}

inline void Print(const char* format, ...) {
  char text[2048];
  va_list args;
  va_start(args, format);
  const int length = std::vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (length > 0) Write(log_fd, text, std::min(size_t(length), sizeof text - 1));
}

inline void Line(const char* text) { Print("%s\n", text); }

class FdSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit FdSink(int fd) : fd_(fd) {}
 protected:
  void sink_it_(const spdlog::details::log_msg& message) override {
    spdlog::memory_buf_t text;
    formatter_->format(message, text);
    Write(fd_, text.data(), text.size());
  }
  void flush_() override {}
 private:
  int fd_;
};
}  // namespace tabletennis::ps5
