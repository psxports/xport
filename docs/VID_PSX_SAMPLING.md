# Shared PS1 texture sampling

Concatenate `VID_PSX_SAMPLING_GLSL` from `src/vid_psx_sampling.h` into a game-owned fragment source passed to `vid_shader_create`. It supports native GLSL 330 and WebGL2 GLSL ES 300. No linked C module, VID vertex ABI, automatic shader preamble, decoded texture cache or OG GPU changes are required.

## Word transport

Define `uint vidPsxWord(ivec2 address)` in the same shader; the header supplies its prototype. Return the exact original PS1 word using `texelFetch`. The decoder supplies absolute VRAM word addresses wrapped to 1024 by 512. Keep the project's existing upload, channel order and VRAM version invalidation.

For Apocalypse/RapidRacer's canonical RGB5/STP transport, quantize `round(sample * vec4(31,31,31,1))` and reconstruct `r | (g << 5) | (b << 10) | (a << 15)`. Pandemonium2's existing RGB5_A1 transport reconstructs `(r << 11) | (g << 6) | (b << 1) | a`. These are transport reconstruction conventions; do not interchange them without the matching upload conversion.

## GLSL interface

- `int vidPsxAxis(int p, int size, bool repeatAxis)` clamps or wraps within a positive extent; negative inputs do not rely on signed GLSL remainder
- `ivec2 vidPsxAddress(ivec2 pixel, ivec4 region, uint repeatAxes)` converts a local integer texel to bounded page-local coordinates
- `uint vidPsxDecode(uint tpage, uint clut, ivec2 pixel)` decodes one page-local 4-bit, 8-bit or direct 16-bit texel to its original PS1 color word
- `vec4 vidPsxColor(uint word, uint stpClass, bool zeroOpaque)` expands RGB5 with `(c << 3) | (c >> 2)` and returns RGB with binary visibility
- `vec4 vidPsxTap(ivec2 pixel, uint tpage, uint clut, ivec4 region, uint repeatAxes, uint stpClass, bool zeroOpaque)` addresses, decodes and classifies one tap
- `vec4 vidPsxFilter(vec2 uv, uint tpage, uint clut, ivec4 region, uint repeatAxes, uint stpClass, bool zeroOpaque)` returns coverage-normalized four-tap color without reading or classifying a nearest word; callers using this lower-level function must retain their established nearest visibility/STP decision
- `vec4 vidPsxSample(vec2 uv, uint tpage, uint clut, ivec4 region, uint repeatAxes, uint stpClass, bool filtered, bool zeroOpaque, out uint nearestWord)` performs nearest or coverage-normalized bilinear color sampling

Inputs and invariants:

- `uv` uses continuous **local image** texel-space, before adding `region.xy`. The first texel center is `(0.5,0.5)`. Nearest uses `floor(uv)`; bilinear uses `floor(uv - 0.5)` and `fract(uv - 0.5)`. Subtract `region.xy` from page-local UV; multiply normalized UV by `region.zw`. Preserve the authored convention without another half-texel bias
- `region = (origin_x, origin_y, width, height)` uses page texels and exclusive upper bounds. Require positive extents and a contained image in the 256 by 256 texel coordinate domain. Split wrapped allocations before using this contract. Validate format (depth 0, 1 or 2), extents and resource identity during metadata preparation; the shader does not repair invalid inputs
- UVs must be finite; integer base and neighbor indices must fit signed 32-bit. Reduce unbounded analytic scroll before conversion. The sampler is not a floating-point overflow handler
- `repeatAxes` bit 0 repeats X; bit 1 repeats Y. Unset axes clamp. Each tap is bounded independently before decoding. VRAM wrapping does not replace logical bounds
- `stpClass=0` accepts both visible classes;1 accepts non-STP;2 accepts STP. Derive the value from pass selection, not filtered alpha
- `zeroOpaque=false` for ordinary textures, including UI. Use `true` only for an explicitly opaque raw RGB15 image with `stpClass=0`, preserving black zero pixels in framebuffer snapshots. RGB24 requires a separate byte decoder
- `nearestWord` returns the nearest decoded word even when rejected by STP/zero masking. Output alpha is binary visibility, **not** weighted coverage or framebuffer alpha

Discard an invisible result. Apply existing modulation, vertex/material alpha, fog, depth and PS1 blending afterwards. The nearest word first decides visibility. Four decoded taps exclude zero and the other STP class, mix RGB with coverage, then normalize RGB by coverage and return alpha 1. Never multiply material alpha by coverage again. This retains a hard nearest silhouette; it does not provide soft cutout antialiasing.

## Game adapters

Select filtering per surface through flat metadata. Resource ownership, metadata packing, pass selection and texture-window interpretation remain game-owned. Normalize a texture window to its logical repeat region where valid; do not apply a second mask that moves bounded taps outside the image.

For nonrectangular PS1 bit-mask windows, define GLSL macro `VID_PSX_ADDRESS` before the shared source and declare the selected hook with signature `ivec2 hook(ivec2 localPixel, ivec4 region, uint repeatAxes)`. The default is `vidPsxAddress`. Both nearest and all four taps use this hook, preserving one shared decoder/classifier/filter implementation. Return an absolute page-local texel **inside** `region`. Apocalypse reconstructs raw page coordinates by adding `region.xy`, applies the original 8-bit window mask/offset, then clamps to the actual resource bounds. Do not apply a window again after this hook. Record any clamping of originally out-of-image window addresses as an HD atlas-boundary policy; OG retains original addressing. Include noncontiguous masks (48/96-pixel resources) and misaligned window/resource origins in native/WebGL adapter tests.

When original nearest window addressing can fall outside the resource, preserve its visibility and STP classification explicitly: decode that original nearest word with `vidPsxDecode`, reject with `vidPsxColor`, then call `vidPsxFilter` for the four bounded color taps. This is Apocalypse's window adapter. Replacing that nearest decision with the clamped color address would introduce or remove visible pixels. Bilinear weights still come from the original continuous UV before masking; never recompute weights from mapped integer coordinates. The default `vidPsxSample` remains the combined path for ordinary contained images.

Get image identity/dimensions from the resource loader, never polygon UV extrema. Normalize inclusive/exclusive conventions once. Preserve animated frame identity, palette changes and scrolling. Do not mutate queued materials to select filtering.

Follow [ENHANCE.md section 9.1](ENHANCE.md#91-add-selective-bilinear-filtering): objects/environment and audited backdrops can filter; effects need a transparency/blend audit; HUD/fonts/icons remain nearest by default. A backdrop behind a menu is separate from its controls.

RapidRacer and Pandemonium2 reviewed this contract. RR maps square regions to `(x,y,size,size)`; P2 retains rectangular sizes and its word transport. Adding the header does not certify project migration or game appearance. Preserve project adapter and migration receipts separately.

## Verification

From a writable workspace:

```powershell
python -B C:\Projects\xports\xport\tools\test_vid_psx_sampling.py --output C:\Projects\xports\Apocalypse-xport\_build\enhance\shared-sampling
python -B -m http.server 8813 --bind 127.0.0.1 --directory C:\Projects\xports\Apocalypse-xport\_build\enhance\shared-sampling
```

The first command requires VS2022 Community x86 tools. It compiles/runs a hidden native GL fixture and writes `native.log`, `result.json`, `cases.json`, `cases.h`, `index.html`. Native failure returns nonzero. Native success deliberately leaves `web=NOT_EXECUTED`. Open `http://127.0.0.1:8813/` in an actual WebGL2 browser; require visible JSON `result=PASS` and the current case count. Save that observation with source hashes separately; HTML generation is not Web execution.

The 54 cases cover 4/8/16-bit decoding, palette updates, alternate word transports, rectangular atlas guards, independent axis repeat, negative/scrolled UV, nearest/fractional four-tap weights, zero/black STP, class rejection, returned nearest word, opaque zero images, half/add/subtract/quarter blending and independent material alpha. RGB tolerance is one 8-bit unit. Both platforms execute the same extracted header source and data.

The fixture uses lossless RGBA8 channels to exercise word reconstruction hooks. It does not certify a game's GPU upload format, animated resource lifetime, scaled geometry, complete pass/depth policy or appearance. Those require consuming-project current-source shader and live-scene checks from ENHANCE.md.
