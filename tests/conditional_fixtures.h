#pragma once

// Frozen from the earlier TypeScript implementation's conditional reads
// with its StructuredReadResponseSchema and @bufbuild/protobuf 2.16.0.
namespace conditional_fixtures {
constexpr char kNamespace[] = "fixture";
constexpr char kJsonBody[] = "{\"ok\":true,\"count\":7}";
constexpr char kJsonEtag[] = "W/\"fixture-05a1ad892782dbc76b8f0a95\"";
constexpr char kProtobufHex[] =
    "0a1e0a080a026f6b120220010a120a05636f756e741209110000000000001c40";
constexpr char kProtobufEtag[] = "W/\"fixture-protobuf-f3b09d523322f7048b8e3e5d\"";
constexpr char kJsonContentType[] = "application/json; charset=utf-8";
constexpr char kProtobufContentType[] = "application/protobuf";
}  // namespace conditional_fixtures
