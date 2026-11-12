#include "d3d12/common.h"

#include <cstdio>

namespace d3d12 {

void ThrowIfFailed(HRESULT hr, const char* expression, const char* file,
                   int line) {
  if (SUCCEEDED(hr)) {
    return;
  }
  char buffer[1024];
  std::snprintf(buffer, sizeof(buffer), "%s failed with HRESULT 0x%08X (%s:%d)",
                expression, static_cast<unsigned int>(hr), file, line);
  throw std::runtime_error(buffer);
}

std::wstring ToWide(const std::string& str) {
  if (str.empty()) {
    return std::wstring();
  }
  int size = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                                 static_cast<int>(str.size()), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      result.data(), size);
  return result;
}

std::string ToUtf8(const std::wstring& str) {
  if (str.empty()) {
    return std::string();
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, str.data(),
                                 static_cast<int>(str.size()), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

}  // namespace d3d12
