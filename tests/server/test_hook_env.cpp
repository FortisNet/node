#include "test_hook_env.h"

namespace {

const char* (*g_lookup)(void* context, const char* name) = nullptr;
void* g_context = nullptr;

}  // namespace

void LoadTestHookEnv(const char* (*lookup)(void* context, const char* name), void* context) {
  g_lookup = lookup;
  g_context = context;
}

const char* TestHookEnv(const char* name) {
  return g_lookup && name ? g_lookup(g_context, name) : nullptr;
}
