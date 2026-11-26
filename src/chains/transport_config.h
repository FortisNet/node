#pragma once

// The chain transport's slice of the startup configuration: the validated
// FORTIS_CA_BUNDLE_PATH (NativeConfig fields of the same names) that every
// provider request trusts. The composition root, or the service that owns a
// transport, copies it at startup.
namespace fortis::chains {

struct TransportConfig {
  bool ca_bundle_configured = false;
  char ca_bundle_path[1024]{};
};

}  // namespace fortis::chains
