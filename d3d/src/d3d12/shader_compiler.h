/*<h2>src/d3d12/shader_compiler.h</h2>

<p>A minimal wrapper around the DirectX Shader Compiler (DXC) library, to
compile HLSL shaders at runtime (which makes it easy to use preprocessor macros
for shader permutations, and to reload shaders when their source files change).
Shader files are specified relative to a root shader directory, and
<code>#include</code> directives are resolved relative to the including file.
*/

#ifndef D3D12_SHADER_COMPILER_H_
#define D3D12_SHADER_COMPILER_H_

// common.h includes windows.h, which must be included before dxcapi.h.
#include "d3d12/common.h"

#include <directx/d3d12shader.h>
#include <dxcapi.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace d3d12 {

// A preprocessor macro definition (name, value).
using ShaderDefine = std::pair<std::string, std::string>;

// The error thrown when a shader fails to compile.
class ShaderCompilationError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct Shader {
  ComPtr<IDxcBlob> bytecode;
  ComPtr<ID3D12ShaderReflection> reflection;

  D3D12_SHADER_BYTECODE GetBytecode() const {
    return {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
  }
};

class ShaderCompiler {
 public:
  explicit ShaderCompiler(std::filesystem::path shader_directory);

  // Compiles the given entry point of 'file' (relative to the shader
  // directory), for the given target profile (e.g. "cs_6_6"). Throws a
  // ShaderCompilationError if the compilation fails.
  Shader Compile(const std::filesystem::path& file,
                 const std::string& entry_point, const std::string& target,
                 const std::vector<ShaderDefine>& defines = {}) const;

  const std::filesystem::path& shader_directory() const {
    return shader_directory_;
  }

 private:
  std::filesystem::path shader_directory_;
  ComPtr<IDxcUtils> utils_;
  ComPtr<IDxcCompiler3> compiler_;
  ComPtr<IDxcIncludeHandler> include_handler_;
};

// Creates the root signature embedded in the given shader (declared with the
// HLSL RootSignature attribute).
ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                const Shader& shader);

// The expected offset, in bytes, of a constant buffer member. Nested struct
// members are named with dotted paths (e.g. "atmosphere.mie_scattering").
struct ConstantBufferMember {
  std::string name;
  size_t offset;
};

// Checks that the layout of the given constant buffer, as reflected from the
// compiled shader, matches the expected member offsets (i.e. the layout of the
// corresponding C++ struct), and that its size is at most 'max_size'. Throws a
// ShaderCompilationError if this is not the case. Does nothing if the shader
// does not use this constant buffer.
void ValidateConstantBufferLayout(
    const Shader& shader, const char* constant_buffer_name,
    std::span<const ConstantBufferMember> expected_members, size_t max_size);

}  // namespace d3d12

#endif  // D3D12_SHADER_COMPILER_H_
