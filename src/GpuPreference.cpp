// Hybrid-laptop GPU placement.
//
// On a laptop with an integrated GPU and a discrete NVIDIA or AMD one, the
// display driver decides which of the two a process runs on before that
// process executes a single instruction of its own. Both vendors take that
// decision from one exported symbol in the main module's export table:
// NvOptimusEnablement for Optimus, AmdPowerXpressRequestHighPerformance for
// PowerXpress/Enduro. A non-zero value asks for the discrete GPU.
//
// That is why this file holds exported variables and not a function. There is
// nothing to call: by the time any code here could run, the driver has already
// placed the process, and it read the export table to do it. The symbols also
// have to be exported from the executable being placed, not from a library it
// loads, which is why both DLSSVideoPlayer and NeuralWorker compile this
// translation unit - the worker is the process that loads the neural stack and
// renders every cached frame, so a worker left on the integrated GPU is the
// failure mode that matters even when the player itself was placed correctly.
//
// What they do not do: they are a request, not a guarantee. Windows' own
// Graphics settings and the NVIDIA Control Panel's per-application setting
// both override them, a machine whose discrete GPU is disabled in firmware or
// muxed away from the panel ignores them, and on a single-GPU desktop they
// change nothing. Nothing here can report whether the request was honoured
// either - the adapter line D3D12Renderer logs at device creation is what
// says where the process actually landed, and docs/TROUBLESHOOTING.md
// documents the Windows-side remedy for when it landed wrong.

// GpuPreference.h is not needed here: an export table is all this translation
// unit contributes, and the adapter identity it declares is compiled where it
// is used. DWORD and int respectively, spelled without windows.h, because the
// driver reads the byte pattern the vendors document rather than a typedef.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
