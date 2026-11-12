#include "d3d12/shader_watcher.h"

#include <system_error>

namespace d3d12 {

ShaderWatcher::ShaderWatcher(std::filesystem::path directory)
    : directory_(std::move(directory)),
      last_poll_(std::chrono::steady_clock::now()) {
  times_ = Scan();
}

std::map<std::filesystem::path, std::filesystem::file_time_type>
ShaderWatcher::Scan() const {
  std::map<std::filesystem::path, std::filesystem::file_time_type> times;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(directory_,
                                                               error);
       !error && it != std::filesystem::recursive_directory_iterator();
       it.increment(error)) {
    if (it->is_regular_file(error)) {
      auto time = it->last_write_time(error);
      if (!error) {
        times[std::filesystem::relative(it->path(), directory_, error)] = time;
      }
    }
  }
  return times;
}

std::vector<std::filesystem::path> ShaderWatcher::GetChangedFiles() {
  auto now = std::chrono::steady_clock::now();
  if (now - last_poll_ < kPollInterval) {
    return {};
  }
  last_poll_ = now;
  auto times = Scan();
  std::vector<std::filesystem::path> changed_files;
  for (const auto& [path, time] : times) {
    auto it = times_.find(path);
    if (it == times_.end() || it->second != time) {
      changed_files.push_back(path);
    }
  }
  times_ = std::move(times);
  return changed_files;
}

}  // namespace d3d12
