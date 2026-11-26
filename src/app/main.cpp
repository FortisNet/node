#include "exception_boundary.h"
#include "sandbox_control.h"
// Production entry point: local sandbox control is handled before server
// startup; otherwise the normal composition owns all service lifetimes. Replay
// binaries replace this entry point and do not expose the operator CLI.

#include "composition_hooks.h"

int main(int argc, char** argv) {
  return fortis::base::FatalBoundary("main", [&] {
    if (const auto handled = fortis::operations::SandboxControlMain(argc, argv)) return *handled;
    fortis::app::NoCompositionHooks hooks;
    return fortis::app::ServerMain(argc, argv, hooks);
  });
}
