// Checks native-owned HTML templates and generated rendering, status bounds,
// native network counts, and the embedded background bytes.
#include <assert.h>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api_telemetry.h"
#include "bounded_json.h"
#include "landing_background.h"
#include "network_registry.h"
#include "operations_pages.h"
#include "operations_routes.h"
#include "release_routes.h"
#include "file_descriptor.h"
#include <sys/socket.h>
#include <unistd.h>
#include "guest_socket_stats.h"
#include "kv_store.h"
#include "ai_config.h"

using namespace fortis::ai;
using namespace fortis::codec;
using namespace fortis::networks;
using namespace fortis::operations;

namespace {

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file); free(data); return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

void check_landing() {
  NativeNetworkRegistry registry;
  size_t network_count = 0;
  registry.list(&network_count);
  assert(network_count <= 64);
  const char* values[64][kNetworkCapabilityCount]{};
  for (size_t i = 0; i < network_count; ++i)
    for (size_t j = 0; j < kNetworkCapabilityCount; ++j) values[i][j] = "none";
  // The generated registry contains only placeholders; the landing page must
  // count the served values from the same derivation as /v1/networks.
  values[0][0] = "ok";  // balances
  values[0][1] = "ok";  // transfers
  values[1][6] = "ok";  // token
  const LandingCounts counts = LandingCountsFromCapabilities(registry, values, network_count);
  assert(counts.networks > 0 && counts.families > 0);
  assert(counts.balances == 1 && counts.transfers == 1 && counts.token == 1);
  assert(counts.balances <= counts.networks && counts.tx_detail <= counts.networks);
  char count_text[32];
  snprintf(count_text, sizeof(count_text), "%u networks", counts.networks);
  static char actual[128 * 1024];
  for (int docs = 0; docs < 2; ++docs) {
    const size_t rendered = RenderLandingPage(counts, docs != 0, actual, sizeof(actual));
    assert(rendered && rendered < sizeof(actual));
    actual[rendered] = '\0';
    assert(strncmp(actual, "<!DOCTYPE html>", 15) == 0);
    assert(strstr(actual, count_text));
    assert(strstr(actual, "id=\"r-memory-chart\""));
    assert(strstr(actual, "id=\"r-cpu-chart\""));
    assert(strstr(actual, "id=\"r-memory-now\""));
    assert(strstr(actual, "id=\"r-cpu-now\""));
    assert(strstr(actual, "href=\"/networks\""));
    assert(strstr(actual, "href=\"/ai\""));
    assert(strstr(actual, "href=\"/cache/inventory\""));
    assert(strstr(actual, "href=\"/dashboard#guide\""));
    assert(strstr(actual, "id=\"detail-groups\""));
    assert(strstr(actual, "data-chart=\"disk\""));
    assert(!strstr(actual, "fonts.googleapis.com"));
    assert(strstr(actual, "Last known values remain visible"));
    assert(!strstr(actual, "{{"));
    assert(strstr(actual, docs ? "href=\"/doc\"" : "disabled</span>"));
    if (!docs) assert(!strstr(actual, "href=\"/doc\""));
  }
  // Largest possible counts still fit the route's 64 KiB page buffer.
  LandingCounts largest{};
  uint32_t* fields[] = {&largest.networks, &largest.families, &largest.balances,
      &largest.transfers, &largest.nfts, &largest.insight, &largest.tx_status, &largest.token,
      &largest.trustlines, &largest.tx_detail};
  for (uint32_t* field : fields) *field = 4294967295U;
  assert(RenderLandingPage(largest, true, actual, 64 * 1024) > 0);
  assert(RenderLandingPage(counts, true, actual, 1024) == 0);
  printf("native landing page: C++ template renders docs on/off (%zu bytes)\n",
         RenderLandingPage(counts, false, actual, sizeof(actual)));
}

void check_release_directory_lifetime() {
  fortis::http::HttpRouter router;
  assert(!RegisterReleaseRoutes(router, {}));
  assert(!RegisterReleaseRoutes(router, std::string_view("/tmp\0bad", 8)));
  char directory[] = "/tmp/fortis-release-view-XXXXXX";
  assert(mkdtemp(directory));
  const std::string owned(directory);
  const size_t length = strlen(directory);
  char filename[256];
  snprintf(filename, sizeof(filename), "%s/current.json", owned.c_str());
  FILE* file = fopen(filename, "wb");
  assert(file);
  const char document[] = "{\"schemaVersion\":1,\"ownedView\":true}";
  assert(fwrite(document, 1, sizeof(document) - 1, file) == sizeof(document) - 1);
  assert(fclose(file) == 0);
  directory[length] = 'X';  // Explicit borrowed view is not NUL terminated.
  assert(RegisterReleaseRoutes(router, std::string_view(directory, length)));
  memset(directory, 'z', sizeof(directory));
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  fortis::base::FileDescriptor server(sockets[0]), client(sockets[1]);
  const char request[] = "GET /releasesz HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
  assert(send(client.get(), request, sizeof(request) - 1, 0) == static_cast<ssize_t>(sizeof(request) - 1));
  assert(shutdown(client.get(), SHUT_WR) == 0);
  router.Dispatch(server.get()); server.reset();
  std::string response; char bytes[1024];
  for (;;) {
    const ssize_t count = recv(client.get(), bytes, sizeof(bytes), 0);
    assert(count >= 0);
    if (!count) break;
    response.append(bytes, static_cast<size_t>(count));
  }
  assert(response.find("\"ownedView\":true") != std::string::npos);
  assert(unlink(filename) == 0 && rmdir(owned.c_str()) == 0);
}

void check_node_home() {
  char page[32 * 1024];
  char exact[32 * 1024];
  for (unsigned mask = 0; mask < 128; ++mask) {
    const NodeServices services{(mask & 1U) != 0, (mask & 2U) != 0, (mask & 4U) != 0,
        (mask & 8U) != 0, (mask & 16U) != 0, (mask & 32U) != 0, (mask & 64U) != 0};
    const size_t length = RenderNodeHome(services, page, sizeof(page));
    assert(length && length < sizeof(page));
    page[length] = '\0';
    assert(strstr(page, "<!DOCTYPE html>") == page);
    assert(strstr(page, "What this node provides") && strstr(page, "What could come next"));
    assert(strstr(page, "og:image") && strstr(page, "/assets/fortis-favicon.svg"));
    assert(strstr(page, "/dashboard") && strstr(page, "/node-features.json"));
    assert(!strstr(page, "@SERVICES@") && !strstr(page, "@FUTURE_"));
    unsigned enabled = 0;
    for (const char* entry = page; (entry = strstr(entry, "class=\"on\"")); ++entry) ++enabled;
    unsigned expected = 1;
    for (unsigned bit = 0; bit < 7; ++bit) expected += (mask >> bit) & 1U;
    assert(enabled == expected);
    memset(exact, '#', sizeof(exact));
    assert(RenderNodeHome(services, exact, length) == length);
    assert(memcmp(exact, page, length) == 0 && exact[length] == '#');
    assert(RenderNodeHome(services, exact, length - 1) == 0);
  }
  assert(RenderNodeHome({}, nullptr, sizeof(page)) == 0);
  assert(RenderNodeHome({}, page, 0) == 0);
  assert(RenderNodeHome({}, page, 1) == 0);
  assert(!FindNodeBrandAsset("/assets/../node-home.html").bytes);
  assert(!FindNodeBrandAsset({}).bytes);
  const char borrowed[] = "/assets/fortis-favicon.svgEXTRA";
  assert(FindNodeBrandAsset(std::string_view(borrowed, sizeof(borrowed) - 6)).bytes);
  assert(!FindNodeBrandAsset("/assets/fortis-favicon.svg?x=1").bytes);
  const char* const paths[] = {"node-hall.webp", "fortis-favicon.svg", "fortisnet-lockup.svg", "social-preview.jpg"};
  for (const char* path : paths) {
    char url[128];
    snprintf(url, sizeof(url), "/assets/%s", path);
    const auto asset = FindNodeBrandAsset(url);
    char file[128];
    snprintf(file, sizeof(file), "assets/%s", path);
    size_t length = 0;
    char* bytes = read_file(file, &length);
    assert(bytes && asset.bytes && asset.content_type && asset.length == length);
    assert(memcmp(bytes, asset.bytes, length) == 0);
    free(bytes);
  }
}

void check_ai_page() {
  AiServiceConfig config{};
  char page[4096];
  size_t length = RenderAiPage(config, page, sizeof(page));
  assert(length && length < sizeof(page));
  page[length] = '\0';
  assert(strstr(page, "No AI models are configured"));
  config.native_ai_canary = true;
  config.ai_provider_configured = true;
  strcpy(config.ai_provider, "anthropic");
  config.ai_model_configured = true;
  strcpy(config.ai_model, "claude-opus-4-6");
  config.anthropic_api_key_path_configured = true;
  config.openai_model_configured = true;
  strcpy(config.openai_model, "gpt-legacy<test>");
  strcpy(config.openai_api_key, "secret-must-not-appear");
  config.openai_api_key_configured = true;
  length = RenderAiPage(config, page, sizeof(page));
  assert(length && length < sizeof(page));
  page[length] = '\0';
  assert(strstr(page, "claude-opus-4-6"));
  assert(strstr(page, "gpt-legacy&lt;test&gt;"));
  assert(!strstr(page, "secret-must-not-appear"));
  assert(!strstr(page, "gpt-legacy<test>"));
  assert(RenderAiPage(config, page, 64) == 0);
}

void check_errors_page() {
  assert(kErrorsPageHtmlLength > 1000);
  assert(strncmp(kErrorsPageHtml, "<!DOCTYPE html>", 15) == 0);
  assert(strstr(kErrorsPageHtml, "fetch('/errorsz')"));
  assert(strstr(kErrorsPageHtml, "</html>"));
  printf("errors page: embedded C++ template (%zu bytes)\n", kErrorsPageHtmlLength);
}

void check_background() {
  size_t wanted_length = 0, actual_length = 0;
  char* wanted = read_file("assets/background.jpg", &wanted_length);
  const uint8_t* actual = LandingBackground(&actual_length);
  assert(wanted && actual && actual_length == wanted_length &&
         memcmp(actual, wanted, actual_length) == 0);
  printf("background: native local asset (%zu bytes)\n", actual_length);
  free(wanted);
}

size_t render_status_for_test(uint64_t uptime, const int64_t* rss, const int64_t* heap,
                              const char* stats, size_t stats_length, char* output,
                              size_t capacity) {
  return RenderStatusDocument(uptime, rss, heap, nullptr, nullptr, nullptr,
                              stats, stats_length, output, capacity);
}

void check_status_document() {
  static const char stats[] = "{\"window5m\":{}}";
  static char output[768];
  const int64_t rss = 123, heap = 456;
  size_t length = render_status_for_test(65, &rss, &heap, stats, sizeof(stats) - 1, output,
                                       sizeof(output));
  output[length] = '\0';
  char expected[256];
  snprintf(expected, sizeof(expected),
           "{\"ok\":true,\"version\":\"%s\",\"node\":\"native-cpp\",\"uptimeSec\":65,"
           "\"memory\":{\"rss\":123,\"heapUsed\":456},\"cpu\":{\"percent\":null},\"disk\":null,\"stats\":{\"window5m\":{}}}",
           kOperationsPackageVersion);
  assert(strcmp(output, expected) == 0);
  length = render_status_for_test(0, nullptr, nullptr, stats, sizeof(stats) - 1, output,
                                sizeof(output));
  output[length] = '\0';
  // Unmeasurable memory stays null (OpenAPI integer | null), never zero.
  assert(strstr(output, "\"memory\":{\"rss\":null,\"heapUsed\":null},\"cpu\":{\"percent\":null},\"disk\":null"));
  assert(strstr(output, "\"node\":\"native-cpp\""));
  assert(render_status_for_test(0, nullptr, nullptr, stats, sizeof(stats) - 1, output, 16) == 0);
  // Exact fit and one byte short: all or nothing, never a partial document.
  length = render_status_for_test(65, &rss, &heap, stats, sizeof(stats) - 1, output,
                                sizeof(output));
  static char exact[256];
  memset(exact, '#', sizeof(exact));
  assert(render_status_for_test(65, &rss, &heap, stats, sizeof(stats) - 1, exact, length) ==
         length);
  assert(memcmp(exact, output, length) == 0 && exact[length] == '#');
  assert(render_status_for_test(65, &rss, &heap, stats, sizeof(stats) - 1, exact, length - 1) == 0);
  // No stats, or no output buffer: nothing renders (the handler answers 500).
  assert(render_status_for_test(65, &rss, &heap, nullptr, 5, output, sizeof(output)) == 0);
  assert(render_status_for_test(65, &rss, &heap, stats, 0, output, sizeof(output)) == 0);
  assert(render_status_for_test(65, &rss, &heap, stats, sizeof(stats) - 1, nullptr, 256) == 0);
  // A zero measurement is a real 0, distinct from an unmeasurable null.
  const int64_t zero = 0;
  length = render_status_for_test(0, &zero, nullptr, stats, sizeof(stats) - 1, output,
                                sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"uptimeSec\":0,\"memory\":{\"rss\":0,\"heapUsed\":null},\"cpu\":{\"percent\":null},\"disk\":null"));
  const double cpu = 12.5;
  const uint64_t used = 100, total = 1000;
  length = RenderStatusDocument(0, &zero, nullptr, &cpu, &used, &total,
                                stats, sizeof(stats) - 1, output, sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"cpu\":{\"percent\":12.50},\"disk\":{\"used\":100,\"total\":1000}"));
  length = RenderStatusDocument(0, &zero, nullptr, &cpu, &used, nullptr,
                                stats, sizeof(stats) - 1, output, sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"disk\":{\"used\":100,\"total\":null}"));
  const FortisGuestSocketStats sockets{8192, 273, 32, 275};
  length = RenderStatusDocument(0, nullptr, nullptr, nullptr, nullptr, nullptr,
                                stats, sizeof(stats) - 1, output, sizeof(output), &sockets);
  assert(length > 0 && length < sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"sockets\":{\"limit\":8192,\"used\":273,\"slotBytes\":32,\"tableBytes\":262144,\"sampledPeak\":275}"));
  fortis::storage::KvDashboardStats db{};
  db.indexed_keys = 17;
  db.compaction.file_bytes = 4096;
  db.compaction.live_bytes = 1024;
  db.compaction.phase = fortis::storage::KvCompactionPhase::Copy;
  db.recovery_pending = true;
  length = RenderStatusDocument(0, nullptr, nullptr, nullptr, nullptr, nullptr,
                                stats, sizeof(stats) - 1, output, sizeof(output), &sockets, &db, 4);
  assert(length > 0 && length < sizeof(output));
  output[length] = '\0';
  assert(strstr(output, "\"database\":{\"indexedKeys\":17,\"fileBytes\":4096,\"liveBytes\":1024,"));
  assert(strstr(output, "\"sampleAgeSec\":4,\"compactionPhase\":\"copying\",\"recoveryPending\":true}"));
  assert(strcmp(kStatusRuntime, "native-cpp") == 0);

  // The handler's buffers hold the largest stats block with every header
  // field at its widest value; the result is one valid JSON document whose
  // keys are in oracle order and whose stats bytes are the input verbatim.
  static char worst_stats[ApiTelemetry::kStatsCapacity];
  const size_t worst = ApiTelemetry::StatusStatsWorstCaseBytes();
  assert(worst <= sizeof(worst_stats) && worst > 2);
  worst_stats[0] = '"';
  memset(worst_stats + 1, 'x', worst - 2);
  worst_stats[worst - 1] = '"';
  static char document[kStatusDocumentCapacity];
  const int64_t widest = INT64_MAX;
  length = render_status_for_test(static_cast<uint64_t>(INT64_MAX), &widest, &widest, worst_stats,
                                worst, document, sizeof(document));
  assert(length > worst && length < sizeof(document));
  static BoundedJsonToken doc_tokens[64];
  BoundedJsonDocument parsed;
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(parsed.Parse(reinterpret_cast<const unsigned char*>(document), length, doc_tokens, 64,
                      &error));
  const char* const order[] = {"ok", "version", "node", "uptimeSec", "memory", "cpu", "disk", "stats"};
  size_t key_index = 0;
  for (uint32_t key = parsed.token(parsed.root())->first_child; key != kBoundedJsonNoToken;
       key = parsed.token(parsed.token(key)->next_sibling)->next_sibling) {
    assert(key_index < 8 && parsed.StringEquals(key, order[key_index]));
    ++key_index;
  }
  assert(key_index == 8);
  assert(memcmp(document + length - 1 - worst, worst_stats, worst) == 0);

  // All resource histograms must fit even beside the largest traffic block.
  fortis::storage::KvActivityStats activity{};
  uint64_t wake_lag[fortis::storage::KvActivityStats::kBuckets];
  for (size_t i = 0; i < fortis::storage::KvActivityStats::kBuckets; ++i) {
    activity.reads[i] = activity.writes[i] = activity.lock_wait[i] = wake_lag[i] = INT64_MAX;
    for (auto& phase : activity.phases) phase[i] = INT64_MAX;
  }
  length = RenderStatusDocument(INT64_MAX, &widest, &widest, &cpu, &used, &total,
                                worst_stats, worst, document, sizeof(document), &sockets, &db,
                                INT64_MAX, &activity, INT64_MAX, wake_lag);
  assert(length > worst && length < sizeof(document));
  assert(memcmp(document + length - 1 - worst, worst_stats, worst) == 0);

  ResourceSnapshot resources;
  resources.sampled_at_ms = resources.disk_sampled_at_ms = UINT64_MAX;
  resources.peak_rss_bytes = resources.virtual_bytes = resources.threads = resources.descriptors = UINT64_MAX;
  resources.read_bytes = resources.written_bytes = resources.cgroup_memory_bytes = UINT64_MAX;
  resources.cgroup_memory_limit_bytes = resources.disk_available_bytes = UINT64_MAX;
  resources.cpu_quota_cores = 1e18;
  resources.disk_scope = ResourceSnapshot::DiskScope::Filesystem;
  length = RenderStatusDocument(INT64_MAX, &widest, &widest, &cpu, &used, &total,
                                worst_stats, worst, document, sizeof(document), &sockets, &db,
                                INT64_MAX, &activity, INT64_MAX, wake_lag, true,
                                nullptr, nullptr, nullptr, 0, 0, &resources);
  assert(length > worst && length < sizeof(document));
  assert(memcmp(document + length - 1 - worst, worst_stats, worst) == 0);

  assert(strlen(kOperationsPackageVersion) > 0);
}

}  // namespace

int main() try {
  assert(strstr(kErrorsPageHtml, "Incorrect coin IDs"));
  assert(strstr(kErrorsPageHtml, "provider_rejected_coin_id"));
  assert(strstr(kErrorsPageHtml, "read-only"));
  check_landing();
  check_node_home();
  check_release_directory_lifetime();
  check_ai_page();
  check_errors_page();
  check_background();
  check_status_document();
  return 0;
} catch (...) {
  fputs("operations pages test exception\n", stderr);
  return 1;
}
