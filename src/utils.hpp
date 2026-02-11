#pragma once

#include <sys/stat.h>
#include <string>

namespace utils {

static inline bool starts_with(
    const std::string &s,
    const char *prefix
) {
  return s.rfind(
      prefix,
      0
  ) == 0;
}

static inline std::string base_without_ext(
    const std::string &path
) {
  size_t dot = path.rfind(
      '.'
  );
  if (dot == std::string::npos) {
    return path;
  }
  return path.substr(
      0,
      dot
  );
}

static inline bool is_directory(
    const std::string &path
) {
  struct stat st;
  if (stat(
          path.c_str(),
          &st
      ) != 0) {
    return false;
  }
  return S_ISDIR(
      st.st_mode
  );
}

static inline std::string normalize_output_path(
    const std::string &output_path
) {
  if (!output_path.empty() &&
      (output_path.back() == '/' || is_directory(output_path))) {
    std::string dir = output_path;
    if (dir.back() != '/') {
      dir.push_back('/');
    }
    return dir + "index.m3u8";
  }

  size_t slash = output_path.find_last_of('/');
  size_t dot = output_path.find_last_of('.');
  if (dot == std::string::npos ||
      (slash != std::string::npos && dot < slash)) {
    return output_path + ".m3u8";
  }

  return output_path;
}

}  // namespace utils

