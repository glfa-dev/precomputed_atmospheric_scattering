# Precomputed Atmospheric Scattering: HLSL / Direct3D 12 sample

This directory contains an HLSL port of the shaders of the parent project, and
a C++20 / Direct3D 12 sample application which uses them. It is self-contained:
it does not use any file outside of this directory.

## Contents

- `shaders/atmosphere/`: the atmosphere model, in HLSL. These files are a direct
  port of the GLSL files (same functions, same structure, same logic), and do not
  declare any resource or global variable, so they can be reused as is in other
  projects:
  - `definitions.hlsli`: constants, texture sizes and the `AtmosphereParameters`
    struct (port of `atmosphere/definitions.glsl`).
  - `functions.hlsli`: the core functions (port of `atmosphere/functions.glsl`).
    Every function reading a precomputed texture takes an additional
    `SamplerState lut_sampler` argument (a linear, clamp-to-edge sampler).
  - `atmosphere.hlsli`: the public rendering API (`GetSkyRadiance`,
    `GetSkyLuminance`, `GetSunAndSkyIlluminance`, etc), with explicit
    parameters instead of global uniforms.
  - `precompute.hlsl`: the compute shaders precomputing the textures (port of
    the `kCompute*Shader` fragment shaders of `atmosphere/model.cc`).
- `src/atmosphere/model.{h,cc}`: the `atmosphere::Model` class, a Direct3D 12
  port of the OpenGL `Model` class (same constructor parameters, `Init` runs
  the precomputations).
- `src/d3d12/`: minimal Direct3D 12 helpers (device, descriptor heap, texture
  with its views, DXC shader compiler, file watcher, GPU timer). This is not an
  RHI: everything else uses raw D3D12 and d3dx12.
- `src/demo/`, `shaders/demo/`: the demo application (port of
  `atmosphere/demo/`), with a Dear ImGui user interface.

As in the rest of the project, the source files are written in a literate
programming style: the block comments explain the code, and contain HTML.

## Requirements

- Windows 10/11 and a GPU supporting Shader Model 6.6 (dynamic resources,
  `ResourceDescriptorHeap`) and typed UAV loads of RGBA float textures (any
  recent NVIDIA, AMD or Intel GPU).
- Visual Studio 2022 or 2026 with the C++ workload, CMake 3.25 or later.
- [vcpkg](https://vcpkg.io), with the `VCPKG_ROOT` environment variable set.
  The dependencies (Dear ImGui, DirectX-Headers, DirectX Shader Compiler) are
  declared in `vcpkg.json` and installed automatically by CMake.

## Build and run

```
cd d3d
cmake --preset default        # Visual Studio 2026 (use --preset vs2022 for VS 2022)
cmake --build --preset release
build\Release\atmosphere_demo.exe
```

Shaders are compiled at runtime with DXC. They are loaded from the source
`shaders/` directory if it exists, and are recompiled automatically when they
change (if an atmosphere shader changes, the precomputations are run again).
Compilation errors are displayed in the application, which keeps using the
previous shaders. A copy of the `shaders/` directory is also placed next to the
executable, for when the source directory is not available.

The debug build enables the D3D12 debug layer (`--gpu-validation` also enables
GPU based validation).

## Controls

The demo window contains all the options of the OpenGL demo, which can also be
changed with the same keyboard shortcuts:

- mouse drag, CTRL + mouse drag, mouse wheel: view and Sun directions, distance,
- `1`-`9`: predefined views, `+`/`-`: exposure,
- `s`: solar spectrum, `o`: ozone, `t`: combined textures, `p`: half precision,
  `l`: luminance mode, `w`: white balance,
- `h`: show / hide the user interface, `v`: LUT viewer.

The "Atmosphere" window can edit all the atmosphere model parameters ("Apply"
re-runs the precomputations), the "LUT viewer" window displays the precomputed
textures, and the "Precomputation" window shows the GPU time of each
precomputation pass.

For automated tests, `--keys <characters>` replays keyboard shortcuts at
startup, and `--capture <file.png>` renders a few frames in a hidden window,
saves the last one and exits (D3D12 debug layer messages, if any, are written to
`<file.png>.log`).

## Differences with the OpenGL version

- The atmosphere parameters are stored in a constant buffer, instead of being
  compiled in the shaders as constants. The shaders are thus compiled once, and
  the precomputation of each group of 3 wavelengths (in precomputed luminance
  mode) simply uses different constants.
- The precomputations use compute shaders, writing a whole 2D or 3D texture per
  dispatch, instead of one quad per 3D texture layer. The hardware blending used
  in OpenGL to accumulate results is replaced with read-add-write operations in
  the shaders.
- All the precomputed textures use RGBA formats (RGB formats can't be used for
  unordered access views).
- Note: the precomputation of the scattering density is the most expensive
  dispatch (about 10 ms per scattering order on a recent GPU). On very slow GPUs,
  this dispatch could be split into several ones (e.g. one per 3D texture layer,
  as in OpenGL) to avoid GPU timeouts.
