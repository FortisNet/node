#pragma once
namespace fortis::app {
class CompositionHooks;
class Application {
 public:
  explicit Application(CompositionHooks& hooks) : hooks_(hooks) {}
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  int Run(int argc, char** argv);
 private:
  CompositionHooks& hooks_;
};
enum class ShutdownStage {
  Listener, MarkShutdown, Health, Workers, Sampler, Attestation, Sweepers,
  Owners, Coordinator, Hooks, Handoff, Store,
};
// Borrowed lifecycle effect port. The native adapter owns no process resource;
// module/HTTP owners keep their existing resource handles and join contracts.
class ShutdownActions {
 public:
  virtual void Execute(ShutdownStage stage) = 0;
 protected:
  ~ShutdownActions() = default;
};
class ApplicationShutdown {
 public:
  explicit ApplicationShutdown(ShutdownActions& actions) : actions_(actions) {}
  ApplicationShutdown(const ApplicationShutdown&) = delete;
  ApplicationShutdown& operator=(const ApplicationShutdown&) = delete;
  void WorkersStarting() { workers_ = true; }
  void SamplerStarted() { sampler_ = true; }
  void SweepersStarting() { sweepers_ = true; }
  void Fail();
  void Complete();
 private:
  ShutdownActions& actions_;
  bool workers_ = false;
  bool sampler_ = false;
  bool sweepers_ = false;
  bool finished_ = false;
};
}  // namespace fortis::app
