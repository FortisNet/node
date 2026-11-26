#include "application.h"
#include <initializer_list>
namespace fortis::app {
void ApplicationShutdown::Fail() {
  if (finished_) return;
  finished_ = true;
  actions_.Execute(ShutdownStage::Listener);
  if (workers_) actions_.Execute(ShutdownStage::Workers);
  if (sampler_) actions_.Execute(ShutdownStage::Sampler);
  if (sweepers_) actions_.Execute(ShutdownStage::Sweepers);
  actions_.Execute(ShutdownStage::Owners);
  actions_.Execute(ShutdownStage::Coordinator);
  actions_.Execute(ShutdownStage::Hooks);
  actions_.Execute(ShutdownStage::Handoff);
}
void ApplicationShutdown::Complete() {
  if (finished_) return;
  finished_ = true;
  for (auto stage : {ShutdownStage::Listener, ShutdownStage::MarkShutdown,
       ShutdownStage::Health, ShutdownStage::Workers, ShutdownStage::Sampler,
       ShutdownStage::Attestation, ShutdownStage::Sweepers, ShutdownStage::Owners,
       ShutdownStage::Coordinator, ShutdownStage::Hooks, ShutdownStage::Handoff,
       ShutdownStage::Store}) actions_.Execute(stage);
}
}  // namespace fortis::app
