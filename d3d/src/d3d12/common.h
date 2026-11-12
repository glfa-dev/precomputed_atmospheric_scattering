/*<h2>src/d3d12/common.h</h2>

<p>Common includes and error checking utilities for the minimal Direct3D 12
helpers used by this sample. Note that the DirectX-Headers package must be
included before any other DirectX header, so that its (more recent) version of
<code>d3d12.h</code> is used instead of the Windows SDK one.
*/

#ifndef D3D12_COMMON_H_
#define D3D12_COMMON_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <directx/d3d12.h>
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>

namespace d3d12 {

using Microsoft::WRL::ComPtr;

// Throws a std::runtime_error describing 'expression' if 'hr' is a failure.
void ThrowIfFailed(HRESULT hr, const char* expression, const char* file,
                   int line);

// Converts between UTF-8 and UTF-16 strings.
std::wstring ToWide(const std::string& str);
std::string ToUtf8(const std::wstring& str);

}  // namespace d3d12

#define D3D12_CHECK(expression) \
  ::d3d12::ThrowIfFailed((expression), #expression, __FILE__, __LINE__)

#endif  // D3D12_COMMON_H_
