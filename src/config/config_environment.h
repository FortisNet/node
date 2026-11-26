#pragma once
// Startup environment capability, independent of process configuration fields.
namespace fortis::config {
using NativeEnvLookup = const char* (*)(void* context, const char* name);

// The process environment (getenv) as a NativeEnvLookup. ServerMain() passes
// it to LoadNativeConfig and LoadPlayIntegrityConfig; it is the only
// environment reader of the composition root.
const char* ProcessEnvironment(void* context, const char* name);

}  // namespace fortis::config
