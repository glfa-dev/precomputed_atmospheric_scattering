/*<h2>src/d3d12/shader_watcher.h</h2>

<p>A very simple file watcher, used to reload shaders when their source files
change. It periodically polls the last write time of all the files in a
directory (recursively), which is largely sufficient for a few shader files.
*/

#ifndef D3D12_SHADER_WATCHER_H_
#define D3D12_SHADER_WATCHER_H_

#include <chrono>
#include <filesystem>
#include <map>
#include <vector>

namespace d3d12 {

class ShaderWatcher {
 public:
  explicit ShaderWatcher(std::filesystem::path directory);

  // Returns the files (relative to the watched directory) which have been
  // added or modified since the last call. Polls the file system at most every
  // 'kPollInterval', and returns an empty list otherwise.
  std::vector<std::filesystem::path> GetChangedFiles();

 private:
  static constexpr std::chrono::milliseconds kPollInterval{250};

  std::map<std::filesystem::path, std::filesystem::file_time_type> Scan() const;

  std::filesystem::path directory_;
  std::map<std::filesystem::path, std::filesystem::file_time_type> times_;
  std::chrono::steady_clock::time_point last_poll_;
};

}  // namespace d3d12

#endif  // D3D12_SHADER_WATCHER_H_
