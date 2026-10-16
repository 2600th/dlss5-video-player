# Can the player hand DLSS-NR a control mask? (P2.44 spike) — 16 October 2026

**Verdict: not reachable on either add-on.** The player cannot get a per-pixel mask to the
NR evaluation through RenoDX 6.5.3 (pinned) or 8.5.0-rc10, either by NGX parameter or by
configuration. No experiment was run.

1. **The player's parameter block never reaches the NR evaluation.** The player creates
   and evaluates a DLSS Super Resolution feature (feature 1, `src/DLSSBackend.cpp`).
   RenoDX, with `EnableHooks=2`, intercepts that evaluation, takes its colour, depth and
   motion resources, then creates and evaluates feature 18 (DLSS-NR) itself ("feature 18
   created via the signed snippet after DLSS/DLAA" in every `ReShade.log`). It sets the
   NR parameters on its own block: both binaries carry
   `NVSDK_NGX_D3D12_AllocateParameters` and the string "DLSS-NR AllocateParameters
   failed", and both name every `DLSSNR.*` key they set.
2. **Neither add-on names the mask.** `DLSSNR.ControlMask` and its four subrect keys are
   in `nvngx_dlssnr` 310.8.SF-v2, and `kBufferTypeUpliftControlMask` is in
   `sl.dlss_nr.dll`. No `ControlMask`, `UpliftControlMask` or `NRControlMask` string
   occurs in either add-on (ASCII or UTF-16). An `NVSDK_NGX_Parameter` cannot be
   enumerated, so an add-on forwards only keys it names. A mask set on the player's SR
   block would be read by nobody.
3. **No configuration route.** The only mask keys are `NRAutoMask` (both) and
   `NRPass{2..4}AutoMask` (8.5), which drive `DLSSNR.UseAutoMask`. 8.5 calls it the
   "Character mask" and its text says it "also changes the rest of the picture
   slightly: brightness by about 2 percent".

What it would take:

- **An upstream RenoDX change** that reads `DLSSNR.ControlMask` (and its subrect) from
  the intercepted evaluation's block and sets it on its own. The player would then bind
  a texture on the SR block it already fills.
- **The player creating feature 18 itself**, as NVIDIA's integrations do (dxvk-remix
  ecf646d sets `pInControlMask` and nulls it under the automatic mask).
  `docs/DLSS5_SETUP.md` records why not: the signed-snippet load, RenoDX's HDR and
  normalisation contract, and a second NGX session. That is the rewrite, not a spike.

So P2.36/P2.45 stay post-model blends on both add-ons, and the per-channel effect could
not be measured.

## Method

Static: `strings -n 4` and `strings -el -n 4` over both add-ons, the NR DLL and
`sl.dlss_nr.dll`; the call path in `src/DLSSBackend.cpp`; the feature-create lines in the
`ReShade.log` of every render in passes-drift-20261016 and renodx-85-20261016.

## Environment

| Item | Value |
|---|---|
| RenoDX 6.5.3 | `renodx-dlss5.addon64` 0.2026.917.1432, SHA-256 `342341f669f1d64e0c70c8593a07a2fab5075e073dfae97c331c9a6776260a0a` |
| RenoDX 8.5.0-rc10 | `renodx-dlss5.addon64` 0.2026.926.210, SHA-256 `dcd93881e976ad033d83c2bb01f4bc3e4ddc59c15fe0dd4ca165bc5fc7d1ac68` |
| DLSS-NR | `nvngx_dlssnr_310.8.SF-v2.dll` 310.8.2.0, SHA-256 `6eb209e7…0a27` |
| Streamline | `sl.dlss_nr.dll` 2.13.0.0, SHA-256 `9f6672e5…c11d` |

## Results

| String | dlssnr 310.8.SF-v2 | sl.dlss_nr 2.13 | RenoDX 6.5.3 | RenoDX 8.5.0-rc10 |
|---|---|---|---|---|
| `DLSSNR.ControlMask` (+4 subrect keys) | yes | yes | **no** | **no** |
| `kBufferTypeUpliftControlMask` | - | yes | no | no |
| `DLSSNR.UseAutoMask` | yes | - | yes | yes |
| `NVSDK_NGX_D3D12_AllocateParameters` | - | - | yes | yes |
| `DLSSNR.GlobalToneStrength` | no | - | yes | no (dropped) |
| config keys with "Mask" | - | - | `NRAutoMask` | `NRAutoMask`, `NRPass{2..4}AutoMask` |

## Caveats

- String absence is strong evidence, not proof. Every other `DLSSNR.*` key is a literal,
  and NGX offers no way to forward unnamed keys.
- The Streamline route tags a numeric buffer type that would not show as a string. It is
  moot: the player runs `EnableHooks=2` and does not use Streamline.
- Visual Enhancer v15's R/G/B mask controls remain unverified.
