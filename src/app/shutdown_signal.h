#pragma once

// Process shutdown: SIGINT and SIGTERM set one flag that the accept loop and
// the isolated health listener poll; SIGPIPE is ignored so a peer reset never
// kills the server mid-write.
namespace fortis::app {

// Ignores SIGPIPE and routes SIGINT/SIGTERM to the shutdown flag.
void InstallShutdownSignals();
/// Consume a SIGHUP certificate reload request on the ingress thread.
bool TakeCertificateReload();
// True once a shutdown signal arrived or MarkShutdown ran.
bool ShutdownRequested();
// Sets the flag (the accept loop ended): stops the health listener's loop.
void MarkShutdown();

}  // namespace fortis::app
