# Data files of the D3D12 demo

## `film_lut_65.bin`

The 3D LUT of the optional "Film LUT" tone mapping of the demo (key `m`). It
is a copy of `K64_SDR_Rec709_sRGB_inRec709_65.bin`, exported by the K64 film
emulation project (stock `neutral_overlap`).

- Size: 65 x 65 x 65 samples, i.e. 65^3 * 16 = 4,394,000 bytes.
- Layout: raw little endian float32 RGBA texels, in `[b][g][r][rgba]` order
  (the red input varies fastest, i.e. x = R, y = G, z = B).
- Input: scene linear Rec.709 RGB (mid gray 0.18, ExposureEV 0), encoded with
  a log2 shaper with a linear toe: `code = x * 47.1766955` for
  `0 <= x < 0.00191129199`, else `(log2(x / 0.18) + 8) / 16`, clamped to
  [0,1] (domain 0 to 46.08, i.e. 8 stops below and above mid gray).
- Output: display code values (Rec.709 primaries, sRGB transfer function), in
  the RGB components (alpha is unused).

The shaper constants are hardcoded in `shaders/demo/demo.hlsl`, and must match
the ones used to bake the LUT.
