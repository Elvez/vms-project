#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>

static inline std::ofstream &log_stream() {
  static std::ofstream stream;
  return stream;
}

static inline bool log_init(
    const std::string &path
) {
  std::ofstream &stream = log_stream();
  if (stream.is_open()) {
    return true;
  }

  stream.open(
      path,
      std::ios::out | std::ios::app
  );
  if (!stream.is_open()) {
    std::fprintf(
        stderr,
        "Failed to open log file: %s\n",
        path.c_str()
    );
    return false;
  }

  return true;
}

static inline void log_close() {
  std::ofstream &stream = log_stream();
  if (stream.is_open()) {
    stream.flush();
    stream.close();
  }
}

static inline void log_message(
    const char *level,
    const char *fmt,
    ...
) {
  std::ofstream &stream = log_stream();
  if (!stream.is_open()) {
    return;
  }

  std::time_t now = std::time(nullptr);
  std::tm tm_buf;
#if defined(_WIN32)
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif

  char ts[32];
  std::strftime(
      ts,
      sizeof(ts),
      "%Y-%m-%d %H:%M:%S",
      &tm_buf
  );

  char msg[2048];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(
      msg,
      sizeof(msg),
      fmt,
      args
  );
  va_end(args);

  stream << "[" << level << "] " << ts << " " << msg << "\n";
  stream.flush();

  std::fprintf(
      stdout,
      "[%s] %s %s\n",
      level,
      ts,
      msg
  );
  std::fflush(stdout);
}

