#pragma once
#include <optional>
namespace fortis::operations {
/** Handle `fortis-node sandbox SOCKET start SPEC|status ID|stop ID` without root.
 * Returns nullopt for normal server startup. CLI errors are fixed/sanitized;
 * job arguments/output and private paths are not echoed on errors.
 * Local operator CLI only; does not register a public HTTP execution endpoint.
 */
std::optional<int> SandboxControlMain(int argc, char** argv);
}  // namespace fortis::operations
