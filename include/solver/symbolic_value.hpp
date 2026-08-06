#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "solver/smt.hpp"

namespace refractir {

  /**
   * Represents a symbolic value during symbolic execution.
   * Maps to SMT terms or nested aggregate structures.
   *
   * Namespace-scoped (rather than nested in SymbolicExecutor) so the
   * provenance collaborator and the solver's per-concern translation units
   * can share it without depending on the full SymbolicExecutor definition.
   */
  struct SymbolicValue {
    // [v0.2.1] Vec: N-lane tuple (held in arrayVal, same shape as Array).
    // Distinguished from Array so the solver can apply lane-wise UB
    // semantics and the C-backend-compatible 0/1 mask representation.
    enum class Kind { Int, Array, Struct, Undef, Vec } kind = Kind::Undef;
    smt::Term term;       // For scalar Int (the BV value)
    smt::Term is_defined; // Boolean term: true if value is defined
    std::vector<SymbolicValue> arrayVal;
    std::map<std::string, SymbolicValue> structVal;

    smt::Term prov_base; // [v0.2.1] Pointer provenance base tag (BV64)
    smt::Term prov_size; // [v0.2.1] Pointer provenance size in tag-units (BV64)

    SymbolicValue() = default;

    SymbolicValue(Kind k) : kind(k) {}

    SymbolicValue(Kind k, smt::Term t, smt::Term d) : kind(k), term(t), is_defined(d) {}

    SymbolicValue(Kind k, smt::Term t, smt::Term d, smt::Term pb, smt::Term ps) :
        kind(k), term(t), is_defined(d), prov_base(pb), prov_size(ps) {}

    SymbolicValue(const SymbolicValue &other) = default;
    SymbolicValue &operator=(const SymbolicValue &other) = default;
    SymbolicValue(SymbolicValue &&) = default;
    SymbolicValue &operator=(SymbolicValue &&) = default;
  };

  /// Symbolic local store: name → current symbolic value.
  using SymbolicStore = std::unordered_map<std::string, SymbolicValue>;

} // namespace refractir
