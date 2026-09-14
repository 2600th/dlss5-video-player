#pragma once

#include "NeuralWorkerProtocol.h"
#include "RuntimePolicy.h"

#include <windows.h>

#include <filesystem>

// Worker side. Primes feature 18 on a synthetic sequence, reads the runtime
// evidence and returns the receipt. Never writes to the cache.
neural_worker_protocol::PreflightPayload RunNeuralPreflightProbe(
    HWND renderWindow, const std::filesystem::path& moduleDirectory, const DetectedGpu& gpu);
