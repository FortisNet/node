#include "../src/http/outbound_https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::http;

namespace {
bool allow_loopback(void*, uint32_t ip) { return (ip >> 24) == 127; }
bool deny_all(void*, uint32_t) { return false; }

bool replace_ca(const char* destination, const char* source) {
  FILE* input = fopen(source, "rb");
  if (!input) return false;
  unsigned char bytes[1024 * 1024];
  const size_t length = fread(bytes, 1, sizeof(bytes), input);
  const bool complete = feof(input) != 0;
  fclose(input);
  if (!complete) return false;
  FILE* output = fopen(destination, "wb");
  if (!output) return false;
  const bool copied = fwrite(bytes, 1, length, output) == length;
  const bool closed = fclose(output) == 0;
  return copied && closed;
}

void print_response(const OutboundResponse& response, const unsigned char* body) {
  printf("%d %d %zu|%s|%s|%s|", static_cast<int>(response.error), response.status,
         response.body_length, response.content_type, response.location,
         response.retry_after);
  const size_t preview = response.body_length < 64 ? response.body_length : 64;
  fwrite(body, 1, preview, stdout);
  putchar('\n');
}
}

int main(int argc, char** argv) {
  if (argc != 6) return 2;
  OutboundRequest request{};
  request.host = argv[1];
  request.port = argv[2];
  request.path = argv[3];
  request.ca_file = argv[4];
  request.timeout_ms = static_cast<uint32_t>(strtoul(argv[5], nullptr, 10));
  request.max_response_bytes = 64;
  const char* custom_name = getenv("TEST_HEADER_NAME");
  const char* custom_value = getenv("TEST_HEADER_VALUE");
  const char* second_name = getenv("TEST_SECOND_HEADER_NAME");
  const char* second_value = getenv("TEST_SECOND_HEADER_VALUE");
  const char* custom_count = getenv("TEST_HEADER_COUNT");
  if (custom_count) request.header_count = strtoul(custom_count, nullptr, 10);
  if (!custom_count && (second_name || second_value)) request.header_count = 2;
  else if (!custom_count && (custom_name || custom_value)) request.header_count = 1;
  for (size_t i = 0; i < request.header_count && i < 2; ++i) {
    const char* name = i == 0 ? custom_name : second_name;
    const char* value = i == 0 ? custom_value : second_value;
    if (name) {
      if (strlen(name) >= sizeof(request.headers[i].name))
        memset(request.headers[i].name, 'A', sizeof(request.headers[i].name));
      else snprintf(request.headers[i].name, sizeof(request.headers[i].name), "%s", name);
    }
    if (value) {
      if (strlen(value) >= sizeof(request.headers[i].value))
        memset(request.headers[i].value, 'A', sizeof(request.headers[i].value));
      else snprintf(request.headers[i].value, sizeof(request.headers[i].value), "%s", value);
    }
  }
  const char* response_limit = getenv("MAX_RESPONSE_BYTES");
  if (response_limit) request.max_response_bytes = strtoul(response_limit, nullptr, 10);
  if (getenv("ALLOW_LOCAL")) request.allow_address = allow_loopback;
  if (getenv("DENY_ALL")) request.allow_address = deny_all;
  if (getenv("REQUIRE_ALL")) request.require_all_addresses_allowed = true;
  if (getenv("CONNECT_LOOPBACK")) request.connect_ipv4 = 0x7f000001;
  if (getenv("PLAINTEXT")) {
    request.plaintext = true;
    if (strcmp(request.ca_file, "-") == 0) request.ca_file = nullptr;
  }
  const char* method = getenv("TEST_METHOD");
  if (method && strcmp(method, "POST") == 0) {
    request.method = OutboundMethod::kPostJson;
    static const unsigned char payload[] = "{\"hello\":\"world\"}";
    request.body = payload;
    request.body_length = sizeof(payload) - 1;
  }

  static unsigned char body[1024 * 1024];
  const OutboundResponse response =
      OutboundHttpsRequest(request, body, sizeof(body));
  print_response(response, body);
  const char* second_path = getenv("SECOND_PATH");
  if (second_path) {
    const char* replacement = getenv("ROTATE_CA_TO");
    if (replacement && !replace_ca(request.ca_file, replacement)) return 3;
    request.path = second_path;
    const OutboundResponse second = OutboundHttpsRequest(request, body, sizeof(body));
    print_response(second, body);
  }
  return 0;
}
