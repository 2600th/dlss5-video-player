# Related DLSS 5 neural-rendering implementations

These projects were reviewed on September 1, 2026 to avoid duplicating solved
integration work and to compare real-time performance strategies.

## Adopted ideas

- [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) documents the
  RenoDX `[RenoDX.DLSS5]` controls. This player adapted its MIT-licensed
  configuration contract: use its raw-NGX-only hook mode (`EnableHooks=2`),
  enable neural uplift, explicitly disable RenoDX upscaling, and preserve
  unrelated user settings. The Streamline hook mode is unnecessary because
  this player calls NGX directly.
- [DLSS5 Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot) treats the
  feeder route as native DLAA and warns against mixing independently versioned
  runtime components. This project likewise defaults to DLAA and hash-locks an
  atomic runtime set.
- [DLSS 5 Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer)
  records exact component fingerprints and requires feature-18 evidence before
  accepting offline output. This project already hash-locks runtime inputs and
  keeps configured state separate from successful NGX evaluation evidence.
- [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR) confirms a
  persistent feature-18 lifetime, native 1:1 output, driver-store NGX discovery,
  and caller-validation shim mechanics. Its direct bridge is a useful reference
  for a future official/runtime-authorized backend.

## Deliberately not copied

The Visual Enhancer's native worker source is not present in its repository;
its Python layer transports frames through worker pipes. ComfyUI-DLSS5-NR v0.2
uses CPU staging for every D3D12 upload and readback. Both are reasonable for
offline or tensor workflows, but would add latency and bandwidth overhead to a
real-time player that already owns the D3D12 resources.

This project therefore keeps its persistent GPU resource ring and RenoDX inline
interception path. It does not combine a direct feature-18 caller shim with the
RenoDX add-on because doing so risks two neural evaluations for one output and
two undocumented NGX lifetimes. Only the MIT-licensed setting names and policy
were adapted; no renderer or bridge implementation was copied. No proprietary
runtime or game file from any related project is committed here.

Source licenses apply independently. The DLSS5-Feeder MIT notice used by this
project is retained under `THIRD_PARTY_LICENSES/`.
