#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace refractir {

  /**
   * Represents a value during interpreter runtime.
   *
   * Namespace-scoped (rather than nested in Interpreter) so the memory and
   * type-layout collaborators can operate on it without depending on the
   * full Interpreter definition.
   */
  struct RuntimeValue {
    enum class Kind { Int, Float, Array, Struct, Undef, Ptr, Vec } kind = Kind::Undef;
    __int128 intVal = 0;
    double floatVal = 0.0;
    std::uint32_t bits = 64;    // bitwidth for Int or Float (32/64)
    std::uint64_t ptrVal = 0;   // for Ptr kind: raw address
    std::uint64_t ptrBase = 0;  // for Ptr kind: provenance object ID (ObjectInfo::provId)
    std::uint64_t elemSize = 1; // for Ptr kind: static element size of the pointee type
    std::vector<RuntimeValue> arrayVal;
    std::map<std::string, RuntimeValue> structVal;
    // [v0.2.1] Vec: same shape as Array (per-lane RuntimeValue tuple),
    // but represents a vector value (no address; not in heap_; lane-wise
    // arithmetic). Element kind matches the lane scalar type.
  };

  /// Local variable store: name → current value.
  using Store = std::unordered_map<std::string, RuntimeValue>;

} // namespace refractir
