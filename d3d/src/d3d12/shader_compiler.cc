#include "d3d12/shader_compiler.h"

#include <map>
#include <sstream>

namespace d3d12 {

ShaderCompiler::ShaderCompiler(std::filesystem::path shader_directory)
    : shader_directory_(std::filesystem::absolute(shader_directory)) {
  D3D12_CHECK(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils_)));
  D3D12_CHECK(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_)));
  D3D12_CHECK(utils_->CreateDefaultIncludeHandler(&include_handler_));
}

Shader ShaderCompiler::Compile(const std::filesystem::path& file,
                               const std::string& entry_point,
                               const std::string& target,
                               const std::vector<ShaderDefine>& defines) const {
  const std::filesystem::path path = shader_directory_ / file;
  ComPtr<IDxcBlobEncoding> source;
  if (FAILED(utils_->LoadFile(path.c_str(), nullptr, &source))) {
    throw ShaderCompilationError("Cannot read " + path.string());
  }

  // The full path is passed as the source file name, so that the #include
  // directives are resolved relative to the including file.
  std::vector<std::wstring> arguments = {
      path.wstring(),
      L"-E", ToWide(entry_point),
      L"-T", ToWide(target),
      L"-HV", L"2021",
      L"-I", shader_directory_.wstring(),
      // The precomputations are very slow without optimizations.
      L"-O3",
  };
#ifndef NDEBUG
  // Embed debug information, for PIX.
  arguments.insert(arguments.end(), {L"-Zi", L"-Qembed_debug"});
#endif
  for (const auto& [name, value] : defines) {
    arguments.push_back(L"-D");
    arguments.push_back(ToWide(value.empty() ? name : name + "=" + value));
  }
  std::vector<LPCWSTR> argument_pointers;
  for (const std::wstring& argument : arguments) {
    argument_pointers.push_back(argument.c_str());
  }

  DxcBuffer source_buffer = {source->GetBufferPointer(),
                             source->GetBufferSize(), DXC_CP_ACP};
  ComPtr<IDxcResult> result;
  D3D12_CHECK(compiler_->Compile(
      &source_buffer, argument_pointers.data(),
      static_cast<UINT32>(argument_pointers.size()), include_handler_.Get(),
      IID_PPV_ARGS(&result)));

  ComPtr<IDxcBlobUtf8> errors;
  result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
  std::string messages =
      errors && errors->GetStringLength() > 0 ? errors->GetStringPointer() : "";
  HRESULT status;
  D3D12_CHECK(result->GetStatus(&status));
  const std::string name =
      file.generic_string() + " (" + entry_point + ", " + target + ")";
  if (FAILED(status)) {
    throw ShaderCompilationError(name + ":\n" + messages);
  }
  if (!messages.empty()) {
    OutputDebugStringA((name + " warnings:\n" + messages).c_str());
  }

  Shader shader;
  D3D12_CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader.bytecode),
                                nullptr));
  ComPtr<IDxcBlob> reflection;
  if (SUCCEEDED(result->GetOutput(DXC_OUT_REFLECTION,
                                  IID_PPV_ARGS(&reflection), nullptr)) &&
      reflection) {
    DxcBuffer reflection_buffer = {reflection->GetBufferPointer(),
                                   reflection->GetBufferSize(), 0};
    D3D12_CHECK(utils_->CreateReflection(&reflection_buffer,
                                         IID_PPV_ARGS(&shader.reflection)));
  }
  return shader;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                const Shader& shader) {
  ComPtr<ID3D12RootSignature> root_signature;
  D3D12_CHECK(device->CreateRootSignature(
      0, shader.bytecode->GetBufferPointer(), shader.bytecode->GetBufferSize(),
      IID_PPV_ARGS(&root_signature)));
  return root_signature;
}

namespace {

// Adds the offsets of all the (nested) members of 'type', whose start offset
// is 'offset', to 'offsets'.
void CollectMemberOffsets(ID3D12ShaderReflectionType* type,
                          const std::string& name, size_t offset,
                          std::map<std::string, size_t>* offsets) {
  (*offsets)[name] = offset;
  D3D12_SHADER_TYPE_DESC desc;
  D3D12_CHECK(type->GetDesc(&desc));
  for (UINT i = 0; i < desc.Members; ++i) {
    ID3D12ShaderReflectionType* member_type = type->GetMemberTypeByIndex(i);
    D3D12_SHADER_TYPE_DESC member_desc;
    D3D12_CHECK(member_type->GetDesc(&member_desc));
    CollectMemberOffsets(member_type, name + "." + type->GetMemberTypeName(i),
                         offset + member_desc.Offset, offsets);
  }
}

}  // anonymous namespace

void ValidateConstantBufferLayout(
    const Shader& shader, const char* constant_buffer_name,
    std::span<const ConstantBufferMember> expected_members, size_t max_size) {
  if (!shader.reflection) {
    return;
  }
  ID3D12ShaderReflectionConstantBuffer* constant_buffer =
      shader.reflection->GetConstantBufferByName(constant_buffer_name);
  D3D12_SHADER_BUFFER_DESC buffer_desc;
  if (FAILED(constant_buffer->GetDesc(&buffer_desc))) {
    // The constant buffer is not used by this shader (and has been removed by
    // the compiler), so there is nothing to check.
    return;
  }

  std::map<std::string, size_t> offsets;
  for (UINT i = 0; i < buffer_desc.Variables; ++i) {
    ID3D12ShaderReflectionVariable* variable =
        constant_buffer->GetVariableByIndex(i);
    D3D12_SHADER_VARIABLE_DESC variable_desc;
    D3D12_CHECK(variable->GetDesc(&variable_desc));
    CollectMemberOffsets(variable->GetType(), variable_desc.Name,
                         variable_desc.StartOffset, &offsets);
  }

  std::ostringstream errors;
  if (buffer_desc.Size > max_size) {
    errors << "size is " << buffer_desc.Size << " instead of at most "
           << max_size << "\n";
  }
  for (const ConstantBufferMember& member : expected_members) {
    auto it = offsets.find(member.name);
    if (it == offsets.end()) {
      errors << member.name << " not found\n";
    } else if (it->second != member.offset) {
      errors << member.name << " offset is " << it->second << " instead of "
             << member.offset << "\n";
    }
  }
  if (!errors.str().empty()) {
    throw ShaderCompilationError(
        std::string("Unexpected layout for constant buffer ") +
        constant_buffer_name + ":\n" + errors.str());
  }
}

}  // namespace d3d12
