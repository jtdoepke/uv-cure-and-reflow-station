// Schema identity for the shared wire contract (design.md §9).
//
// kSchemaHash is generated per-build by tools/schema_hash_extra_script.py from
// proto/oven.proto's compiled FileDescriptorSet. Both sides send it in
// Hello.schema_hash; any mismatch fails closed (controller stays in safe state,
// CYD shows a schema-mismatch error) until both boards are reflashed together.
#pragma once

#include <cstdint>

#include "schema_hash.h" // generated into $BUILD_DIR/schema by the pre-script

namespace protocol {

// Bumped only for semantic protocol changes that the schema hash can't express
// (e.g. reinterpreting an existing field). The hash gates structural drift.
inline constexpr uint32_t kProtoVer = 1;

inline constexpr uint64_t kSchemaHash = OVEN_SCHEMA_HASH;

} // namespace protocol
