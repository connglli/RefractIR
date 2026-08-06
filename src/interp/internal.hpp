#pragma once

// Interpreter-private helpers shared across the interpreter's translation
// units. Kept inline in this detail header (not a separate .cpp) so each
// interp TU that needs them can pull them in without an extra object file.
// Not part of the public interpreter interface — internal to src/interp.

#include <cmath>
#include <cstdint>
#include "error.hpp"

namespace refractir {

  // Enforce IEEE 754 finite-only semantics (spec §7.4 rules 6–7):
  // truncate to f32 if needed, then reject infinity or NaN.
  inline double checkFPResult(double val, std::uint32_t bits) {
    if (bits == 32)
      val = static_cast<double>(static_cast<float>(val));
    if (std::isinf(val))
      throw UndefinedBehaviorError("UB: Floating-point result is infinity");
    if (std::isnan(val))
      throw UndefinedBehaviorError("UB: Floating-point result is NaN");
    return val;
  }

  // Sign-canonicalize a 128-bit value to its declared N-bit signed width.
  inline __int128 canonicalize(__int128 val, std::uint32_t bits) {
    if (bits >= 128)
      return val;
    // [v0.2.2] Spec §6.4: i1 is a signed 1-bit integer.  The two
    // representable values are 0 (false) and -1 (true) — bit
    // pattern 1 sign-extended.  `iN as iM` widening sign-extends, so
    // an i1 true widened to i32 is -1; matches what the C backend
    // already emits and what the spec mandates.
    if (bits == 1)
      return (val & 1) ? -1 : 0;
    unsigned __int128 mask = ((unsigned __int128)1 << bits) - 1;
    unsigned __int128 sign_bit = (unsigned __int128)1 << (bits - 1);
    unsigned __int128 uval = static_cast<unsigned __int128>(val) & mask;
    if (uval & sign_bit)
      uval |= ~mask;
    return static_cast<__int128>(uval);
  }

} // namespace refractir
