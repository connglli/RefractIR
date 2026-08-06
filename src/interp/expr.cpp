#include <cmath>
#include <cstdint>
#include "analysis/type_utils.hpp"
#include "error.hpp"
#include "frontend/diagnostics.hpp"
#include "internal.hpp"
#include "interp/interpreter.hpp"

namespace refractir {

  RuntimeValue Interpreter::evalExpr(const Expr &e, const Store &store) {
    RuntimeValue v = evalAtom(e.first, store);
    for (const auto &tail: e.rest) {
      RuntimeValue right = evalAtom(tail.atom, store);
      // [v0.2.1] Vector chain: lane-wise +/-. The other operand may be
      // another Vec OR a scalar literal that broadcasts (matches the
      // typechecker rule allowing literal broadcast in vec chains).
      if (v.kind == RuntimeValue::Kind::Vec &&
          (right.kind == RuntimeValue::Kind::Int || right.kind == RuntimeValue::Kind::Float)) {
        RuntimeValue bvec;
        bvec.kind = RuntimeValue::Kind::Vec;
        bvec.arrayVal.reserve(v.arrayVal.size());
        for (auto &laneL: v.arrayVal) {
          RuntimeValue lane = right;
          lane.bits = laneL.bits;
          if (lane.kind == RuntimeValue::Kind::Int)
            lane.intVal = canonicalize(lane.intVal, lane.bits);
          else if (lane.bits == 32)
            lane.floatVal = static_cast<double>(static_cast<float>(lane.floatVal));
          bvec.arrayVal.push_back(std::move(lane));
        }
        right = std::move(bvec);
      }
      if (v.kind == RuntimeValue::Kind::Vec && right.kind == RuntimeValue::Kind::Vec) {
        if (v.arrayVal.size() != right.arrayVal.size())
          throw std::runtime_error("Vector lane count mismatch in +/-");
        for (size_t k = 0; k < v.arrayVal.size(); ++k) {
          auto &lhsLane = v.arrayVal[k];
          auto &rhsLane = right.arrayVal[k];
          if (lhsLane.kind == RuntimeValue::Kind::Undef ||
              rhsLane.kind == RuntimeValue::Kind::Undef)
            throw UndefinedBehaviorError("UB: Reading undef vector lane");
          if (lhsLane.kind == RuntimeValue::Kind::Int) {
            __int128 a = lhsLane.intVal, b = rhsLane.intVal;
            __int128 r = (tail.op == AddOp::Plus) ? (a + b) : (a - b);
            int64_t smax =
                (lhsLane.bits == 64) ? INT64_MAX : ((INT64_C(1) << (lhsLane.bits - 1)) - 1);
            int64_t smin = (lhsLane.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (lhsLane.bits - 1)));
            if (r > (__int128) smax || r < (__int128) smin)
              throw UndefinedBehaviorError("UB: vector lane overflow in +/-");
            lhsLane.intVal = canonicalize(static_cast<int64_t>(r), lhsLane.bits);
          } else if (lhsLane.kind == RuntimeValue::Kind::Float) {
            uint32_t opBits = std::min(lhsLane.bits, rhsLane.bits);
            if (tail.op == AddOp::Plus)
              lhsLane.floatVal = checkFPResult(lhsLane.floatVal + rhsLane.floatVal, opBits);
            else
              lhsLane.floatVal = checkFPResult(lhsLane.floatVal - rhsLane.floatVal, opBits);
            lhsLane.bits = opBits;
          } else {
            throw std::runtime_error("Unsupported lane kind in vector +/-");
          }
        }
        continue;
      }
      if (v.kind == RuntimeValue::Kind::Undef || right.kind == RuntimeValue::Kind::Undef)
        throw UndefinedBehaviorError("UB: Reading undef in expr");

      // Promote Int to Float if needed (Literal inference support)
      if (v.kind == RuntimeValue::Kind::Float && right.kind == RuntimeValue::Kind::Int) {
        right.floatVal = static_cast<double>(right.intVal);
        right.kind = RuntimeValue::Kind::Float;
      } else if (v.kind == RuntimeValue::Kind::Int && right.kind == RuntimeValue::Kind::Float) {
        v.floatVal = static_cast<double>(v.intVal);
        v.kind = RuntimeValue::Kind::Float;
      }

      if (v.kind == RuntimeValue::Kind::Int && right.kind == RuntimeValue::Kind::Int) {
        // Check overflow against the *declared* bitwidth, not int64.
        // Use __int128 so the intermediate result never overflows before the check.
        __int128 a = v.intVal, b = right.intVal;
        __int128 result = (tail.op == AddOp::Plus) ? (a + b) : (a - b);
        int64_t smax = (v.bits == 64) ? INT64_MAX : ((INT64_C(1) << (v.bits - 1)) - 1);
        int64_t smin = (v.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (v.bits - 1)));
        if (result > (__int128) smax || result < (__int128) smin) {
          if (tail.op == AddOp::Plus)
            throw UndefinedBehaviorError("UB: Signed integer overflow in addition");
          else
            throw UndefinedBehaviorError("UB: Signed integer overflow in subtraction");
        }
        v.intVal = static_cast<int64_t>(result);
        v.intVal = canonicalize(v.intVal, v.bits);
      } else if (v.kind == RuntimeValue::Kind::Float && right.kind == RuntimeValue::Kind::Float) {
        // SPEC §6.7: FP expressions are homogeneous.  FloatLit::resolvedBits
        // is now set by the type checker (32 for f32, 64 for f64), so both
        // operands carry the correct precision.  We still take the narrower
        // of the two as a belt-and-suspenders measure for any edge case where
        // one operand's bits was not fully resolved (e.g. a cast-to-f32 whose
        // bits=32 paired with a literal whose resolvedBits=64).
        uint32_t opBits = std::min(v.bits, right.bits);
        if (tail.op == AddOp::Plus)
          v.floatVal = checkFPResult(v.floatVal + right.floatVal, opBits);
        else
          v.floatVal = checkFPResult(v.floatVal - right.floatVal, opBits);
        v.bits = opBits;
      } else if (v.kind == RuntimeValue::Kind::Ptr && right.kind == RuntimeValue::Kind::Int) {
        // ptr ± int: scale offset by element size; result must stay in [base, end].
        // One-past-the-end (== end) is valid for arithmetic but not for dereference.
        // Use ptrBase to find the exact provenance object (enforces field-level boundaries).
        const ObjectInfo *obj = memory_.findObjectByProvId(v.ptrBase);
        if (!obj)
          throw UndefinedBehaviorError("UB: Pointer arithmetic on out-of-bounds pointer");
        uint64_t elemSize = v.elemSize;
        int64_t delta = right.intVal * static_cast<int64_t>(elemSize);
        int64_t newAddr = (tail.op == AddOp::Plus) ? static_cast<int64_t>(v.ptrVal) + delta
                                                   : static_cast<int64_t>(v.ptrVal) - delta;
        if (newAddr < static_cast<int64_t>(obj->base) || newAddr > static_cast<int64_t>(obj->end))
          throw UndefinedBehaviorError("UB: Pointer arithmetic out of bounds");
        v.ptrVal = static_cast<uint64_t>(newAddr);
        // ptrBase preserved: arithmetic does not change provenance.
      } else if (v.kind == RuntimeValue::Kind::Ptr && right.kind == RuntimeValue::Kind::Ptr) {
        // ptr - ptr: element count distance (only subtraction makes sense)
        if (tail.op != AddOp::Minus)
          throw std::runtime_error("Cannot add two pointers");
        if (v.ptrBase != right.ptrBase)
          throw UndefinedBehaviorError("UB: Pointer subtraction across different objects");
        uint64_t elemSize = v.elemSize;
        int64_t diff = static_cast<int64_t>(v.ptrVal) - static_cast<int64_t>(right.ptrVal);
        v.kind = RuntimeValue::Kind::Int;
        v.intVal = diff / static_cast<int64_t>(elemSize);
        v.bits = 64;
      } else {
        throw std::runtime_error("Expr ops only on same scalar kinds (Int/Float)");
      }
    }
    return v;
  }

  RuntimeValue Interpreter::evalAtom(const Atom &a, const Store &store) {
    return std::visit(
        [&](auto &&arg) -> RuntimeValue {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            return evalOpAtom(arg, store);
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            return evalUnaryAtom(arg, store);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            return evalSelectAtom(arg, store);
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            return evalCmpAtom(arg, store);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            return evalCoef(arg.coef, store);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            return evalLValue(arg.rval, store);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            return evalAddrAtom(arg, store);
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            return evalLoadAtom(arg, store);
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            return evalPtrIndexAtom(arg, store);
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            return evalPtrFieldAtom(arg, store);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            return evalCastAtom(arg, store);
          } else if constexpr (std::is_same_v<T, CallAtom>) {
            return evalCallAtom(arg, store);
          }
          return RuntimeValue{};
        },
        a.v
    );
  }

  RuntimeValue Interpreter::evalOpAtom(const OpAtom &arg, const Store &store) {

    RuntimeValue c = evalCoef(arg.coef, store);
    RuntimeValue r = evalLValue(arg.rval, store);
    // [v0.2.1] Vector OpAtom: rval is Vec; coef is either Vec or a
    // scalar literal that broadcasts. Build a per-lane Coef-LValue
    // and recurse into a synthetic OpAtom evaluation for each lane.
    if (r.kind == RuntimeValue::Kind::Vec) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      res.arrayVal.reserve(r.arrayVal.size());
      for (size_t k = 0; k < r.arrayVal.size(); ++k) {
        auto &rL = r.arrayVal[k];
        RuntimeValue cL = (c.kind == RuntimeValue::Kind::Vec) ? c.arrayVal[k] : c;
        if (rL.kind == RuntimeValue::Kind::Undef || cL.kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: Reading undef vector lane");
        // Inherit the lane's bits for the coef literal (broadcast).
        if (c.kind != RuntimeValue::Kind::Vec) {
          if (cL.kind == RuntimeValue::Kind::Int)
            cL.bits = rL.bits;
          if (cL.kind == RuntimeValue::Kind::Float)
            cL.bits = rL.bits;
        }
        // Build a fake one-atom Atom { OpAtom with these scalar
        // operands } and reuse evalAtom — too involved; inline.
        RuntimeValue laneRes;
        if (rL.kind == RuntimeValue::Kind::Int) {
          laneRes.kind = RuntimeValue::Kind::Int;
          laneRes.bits = rL.bits;
          __int128 bw_smax = (rL.bits == 128) ? (((unsigned __int128)1 << 127) - 1) : ((rL.bits == 64) ? INT64_MAX : ((INT64_C(1) << (rL.bits - 1)) - 1));
          __int128 bw_smin = (rL.bits == 128) ? -((unsigned __int128)1 << 127) : ((rL.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (rL.bits - 1))));
          if (arg.op == AtomOpKind::Mul) {
            __int128 p = (__int128) cL.intVal * rL.intVal;
            if (p > (__int128) bw_smax || p < (__int128) bw_smin)
              throw UndefinedBehaviorError("UB: vector lane overflow in *");
            laneRes.intVal = p;
          } else if (arg.op == AtomOpKind::Div) {
            if (rL.intVal == 0)
              throw UndefinedBehaviorError("UB: vector lane division by zero");
            if (cL.intVal == bw_smin && rL.intVal == -1)
              throw UndefinedBehaviorError("UB: vector lane overflow in /");
            laneRes.intVal = cL.intVal / rL.intVal;
          } else if (arg.op == AtomOpKind::Mod) {
            if (rL.intVal == 0)
              throw UndefinedBehaviorError("UB: vector lane modulo by zero");
            if (cL.intVal == bw_smin && rL.intVal == -1)
              throw UndefinedBehaviorError("UB: vector lane overflow in %");
            laneRes.intVal = cL.intVal % rL.intVal;
          } else if (arg.op == AtomOpKind::And)
            laneRes.intVal = cL.intVal & rL.intVal;
          else if (arg.op == AtomOpKind::Or)
            laneRes.intVal = cL.intVal | rL.intVal;
          else if (arg.op == AtomOpKind::Xor)
            laneRes.intVal = cL.intVal ^ rL.intVal;
          else if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                   arg.op == AtomOpKind::LShr) {
            if (rL.intVal < 0 || (uint64_t) rL.intVal >= (uint64_t) laneRes.bits)
              throw UndefinedBehaviorError("UB: vector lane overshift");
            if (arg.op == AtomOpKind::Shl) {
              if (cL.intVal < 0)
                throw UndefinedBehaviorError("UB: vector lane left shift of negative");
              __int128 p = (__int128) cL.intVal << rL.intVal;
              if (p > (__int128) bw_smax || p < (__int128) bw_smin)
                throw UndefinedBehaviorError("UB: vector lane overflow in <<");
              laneRes.intVal = p;
            } else if (arg.op == AtomOpKind::Shr) {
              laneRes.intVal = cL.intVal >> rL.intVal;
            } else {
              unsigned __int128 mask = (laneRes.bits >= 128) ? ~(unsigned __int128)0 : ((unsigned __int128)1 << laneRes.bits) - 1;
              laneRes.intVal = (static_cast<unsigned __int128>(cL.intVal) & mask) >> rL.intVal;
            }
          }
          laneRes.intVal = canonicalize(laneRes.intVal, laneRes.bits);
        } else if (rL.kind == RuntimeValue::Kind::Float) {
          laneRes.kind = RuntimeValue::Kind::Float;
          laneRes.bits = std::min(cL.bits, rL.bits);
          if (arg.op == AtomOpKind::Mul)
            laneRes.floatVal = checkFPResult(cL.floatVal * rL.floatVal, laneRes.bits);
          else if (arg.op == AtomOpKind::Div)
            laneRes.floatVal = checkFPResult(cL.floatVal / rL.floatVal, laneRes.bits);
          else if (arg.op == AtomOpKind::Mod) {
            // Per §7.6 rule 21, the §2.9 intermediate-overflow rule
            // (see scalar `%` branch below) applies per lane.
            double q = cL.floatVal / rL.floatVal;
            if (laneRes.bits == 32)
              q = static_cast<double>(static_cast<float>(q));
            if (std::isinf(q) || std::isnan(q))
              throw UndefinedBehaviorError(
                  "UB: Floating-point intermediate quotient in vector "
                  "lane % is non-finite (spec §2.9)"
              );
            laneRes.floatVal = checkFPResult(std::fmod(cL.floatVal, rL.floatVal), laneRes.bits);
          } else
            throw std::runtime_error("Unsupported op for float vector lane");
        } else {
          throw std::runtime_error("Unsupported vector lane kind in OpAtom");
        }
        res.arrayVal.push_back(std::move(laneRes));
      }
      return res;
    }
    if (c.kind == RuntimeValue::Kind::Undef || r.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Reading undef in op");

    // Promote Int to Float if needed (Literal inference support)
    if (c.kind == RuntimeValue::Kind::Float && r.kind == RuntimeValue::Kind::Int) {
      r.floatVal = static_cast<double>(r.intVal);
      r.kind = RuntimeValue::Kind::Float;
    } else if (c.kind == RuntimeValue::Kind::Int && r.kind == RuntimeValue::Kind::Float) {
      c.floatVal = static_cast<double>(c.intVal);
      c.kind = RuntimeValue::Kind::Float;
    }

    if (c.kind == RuntimeValue::Kind::Int && r.kind == RuntimeValue::Kind::Int) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Int;
      res.bits = c.bits;

      // Compute bitwidth-specific signed min/max for overflow detection.
      __int128 bw_smax = (c.bits == 128) ? (((unsigned __int128)1 << 127) - 1) : ((c.bits == 64) ? INT64_MAX : ((INT64_C(1) << (c.bits - 1)) - 1));
      __int128 bw_smin = (c.bits == 128) ? -((unsigned __int128)1 << 127) : ((c.bits == 64) ? INT64_MIN : (-(INT64_C(1) << (c.bits - 1))));

      if (arg.op == AtomOpKind::Mul) {
        __int128 prod = c.intVal * r.intVal;
        if (prod > (__int128) bw_smax || prod < (__int128) bw_smin)
          throw UndefinedBehaviorError("UB: Signed integer overflow in multiplication");
        res.intVal = prod;
      } else if (arg.op == AtomOpKind::Div) {
        if (r.intVal == 0)
          throw UndefinedBehaviorError("UB: Division by zero");
        if (c.intVal == bw_smin && r.intVal == -1)
          throw UndefinedBehaviorError("UB: Signed integer overflow in division");
        res.intVal = c.intVal / r.intVal;
      } else if (arg.op == AtomOpKind::Mod) {
        if (r.intVal == 0)
          throw UndefinedBehaviorError("UB: Modulo by zero");
        if (c.intVal == bw_smin && r.intVal == -1)
          throw UndefinedBehaviorError("UB: Signed integer overflow in modulo");
        res.intVal = c.intVal % r.intVal;
      } else if (arg.op == AtomOpKind::And) {
        res.intVal = c.intVal & r.intVal;
      } else if (arg.op == AtomOpKind::Or) {
        res.intVal = c.intVal | r.intVal;
      } else if (arg.op == AtomOpKind::Xor) {
        res.intVal = c.intVal ^ r.intVal;
      } else if (arg.op == AtomOpKind::Shl || arg.op == AtomOpKind::Shr ||
                 arg.op == AtomOpKind::LShr) {
        if (r.intVal < 0 || (uint64_t) r.intVal >= (uint64_t) res.bits) {
          throw UndefinedBehaviorError("UB: Overshift");
        }
        if (arg.op == AtomOpKind::Shl) {
          // SPEC §7.1 rule 4: SHL on a negative operand is UB, and
          // result overflow on a non-negative operand is also UB
          // (signed-integer overflow, same footing as +/-/*).
          if (c.intVal < 0)
            throw UndefinedBehaviorError("UB: Left shift of negative");
          __int128 prod = c.intVal << r.intVal;
          if (prod > (__int128) bw_smax || prod < (__int128) bw_smin)
            throw UndefinedBehaviorError("UB: Signed integer overflow in shift");
          res.intVal = prod;
        } else if (arg.op == AtomOpKind::Shr) {
          res.intVal = c.intVal >> r.intVal;
        } else {
          // Logical shift right: mask to width first
          unsigned __int128 mask = (res.bits >= 128) ? ~(unsigned __int128)0 : ((unsigned __int128)1 << res.bits) - 1;
          res.intVal = (static_cast<unsigned __int128>(c.intVal) & mask) >> r.intVal;
        }
      }
      res.intVal = canonicalize(res.intVal, res.bits);
      return res;
    } else if (c.kind == RuntimeValue::Kind::Float && r.kind == RuntimeValue::Kind::Float) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Float;
      // SPEC §6.7: float expressions are homogeneous — all atoms must
      // have the same FP type. evalCoef always tags FloatLit with
      // bits=64 (it has no context), so use the rval's precision (the
      // typed variable) to recover the expression's true type. Without
      // this, e.g. `-4.0 * %v0` for %v0:f32 would yield bits=64 and
      // the surrounding chain would round at f64 precision.
      res.bits = std::min(c.bits, r.bits);
      if (arg.op == AtomOpKind::Mul)
        res.floatVal = checkFPResult(c.floatVal * r.floatVal, res.bits);
      else if (arg.op == AtomOpKind::Div)
        res.floatVal = checkFPResult(c.floatVal / r.floatVal, res.bits);
      else if (arg.op == AtomOpKind::Mod) {
        // Spec §2.9 encodes `%` as
        //   fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](x, y)), y))
        // The inner fp.div is subject to §7.4 rule 6: if x/y at the
        // operand precision overflows or is NaN, the path is UB even
        // when libm fmod would return a finite remainder.
        double q = c.floatVal / r.floatVal;
        if (res.bits == 32)
          q = static_cast<double>(static_cast<float>(q));
        if (std::isinf(q) || std::isnan(q))
          throw UndefinedBehaviorError(
              "UB: Floating-point intermediate quotient in % is "
              "non-finite (spec §2.9)"
          );
        res.floatVal = checkFPResult(std::fmod(c.floatVal, r.floatVal), res.bits);
      } else
        throw std::runtime_error("Unsupported op for floats");
      return res;
    }
    throw std::runtime_error("OpAtom requires same scalar kinds");

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalUnaryAtom(const UnaryAtom &arg, const Store &store) {

    RuntimeValue r = evalLValue(arg.rval, store);
    // [v0.2.1] Vector unary ~: lane-wise.
    if (r.kind == RuntimeValue::Kind::Vec) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      res.arrayVal.reserve(r.arrayVal.size());
      for (auto &lane: r.arrayVal) {
        if (lane.kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: Reading undef lane in unary");
        if (lane.kind != RuntimeValue::Kind::Int)
          throw std::runtime_error("Vector unary ~ requires integer lanes");
        RuntimeValue laneRes;
        laneRes.kind = RuntimeValue::Kind::Int;
        laneRes.bits = lane.bits;
        laneRes.intVal = canonicalize(~lane.intVal, lane.bits);
        res.arrayVal.push_back(std::move(laneRes));
      }
      return res;
    }
    if (r.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Reading undef in unary op");
    if (r.kind != RuntimeValue::Kind::Int)
      throw std::runtime_error("Unary op requires int");
    RuntimeValue res;
    res.kind = RuntimeValue::Kind::Int;
    if (arg.op == UnaryOpKind::Not) {
      res.intVal = ~r.intVal;
    }
    return res;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalSelectAtom(const SelectAtom &arg, const Store &store) {

    // [v0.2.1] Two forms. Mask form requires lane-wise (or scalar
    // i1) blend; Cond form is the existing scalar boolean select.
    if (arg.cond) {
      return evalCond(*arg.cond, store) ? evalSelectVal(arg.vtrue, store)
                                        : evalSelectVal(arg.vfalse, store);
    }
    // Mask form
    RuntimeValue mask = evalExpr(*arg.maskExpr, store);
    if (mask.kind == RuntimeValue::Kind::Vec) {
      RuntimeValue vt = evalSelectVal(arg.vtrue, store);
      RuntimeValue vf = evalSelectVal(arg.vfalse, store);
      if (vt.kind != RuntimeValue::Kind::Vec || vf.kind != RuntimeValue::Kind::Vec ||
          vt.arrayVal.size() != mask.arrayVal.size() || vf.arrayVal.size() != mask.arrayVal.size())
        throw std::runtime_error("Mask-based select: lane count mismatch");
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      res.arrayVal.reserve(mask.arrayVal.size());
      for (size_t k = 0; k < mask.arrayVal.size(); ++k) {
        const auto &mL = mask.arrayVal[k];
        if (mL.kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: undef mask lane");
        bool pick = (mL.intVal != 0);
        const auto &chosen = pick ? vt.arrayVal[k] : vf.arrayVal[k];
        if (chosen.kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: undef lane selected by mask");
        res.arrayVal.push_back(chosen);
      }
      return res;
    }
    // Scalar i1 mask: all-or-nothing.
    if (mask.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: undef scalar mask");
    return (mask.intVal != 0) ? evalSelectVal(arg.vtrue, store) : evalSelectVal(arg.vfalse, store);

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalCmpAtom(const CmpAtom &arg, const Store &store) {

    // [v0.2.1] Reified comparison. Both operands are SelectVal.
    RuntimeValue lv = evalSelectVal(arg.lhs, store);
    RuntimeValue rv = evalSelectVal(arg.rhs, store);
    auto cmpScalar = [&](const RuntimeValue &a, const RuntimeValue &b) -> bool {
      if (a.kind == RuntimeValue::Kind::Undef || b.kind == RuntimeValue::Kind::Undef)
        throw UndefinedBehaviorError("UB: undef in cmp");
      // [v0.2.1] Rule 14: relational compare of pointers from
      // different objects (or null vs non-null in a relational
      // op) is UB. Equality / inequality remain legal.
      if (a.kind == RuntimeValue::Kind::Ptr || b.kind == RuntimeValue::Kind::Ptr) {
        bool relational = arg.op == RelOp::LT || arg.op == RelOp::LE || arg.op == RelOp::GT ||
                          arg.op == RelOp::GE;
        if (relational) {
          uint64_t aBase = (a.kind == RuntimeValue::Kind::Ptr) ? a.ptrBase : 0;
          uint64_t bBase = (b.kind == RuntimeValue::Kind::Ptr) ? b.ptrBase : 0;
          if (a.kind != b.kind)
            throw UndefinedBehaviorError("UB: relational compare between pointer and non-pointer");
          if (aBase != bBase)
            throw UndefinedBehaviorError("UB: relational compare of cross-object pointers");
        }
        bool eq = a.ptrVal == b.ptrVal;
        switch (arg.op) {
          case RelOp::EQ:
            return eq;
          case RelOp::NE:
            return !eq;
          case RelOp::LT:
            return a.ptrVal < b.ptrVal;
          case RelOp::LE:
            return a.ptrVal <= b.ptrVal;
          case RelOp::GT:
            return a.ptrVal > b.ptrVal;
          case RelOp::GE:
            return a.ptrVal >= b.ptrVal;
        }
        return false;
      }
      double af =
          (a.kind == RuntimeValue::Kind::Float) ? a.floatVal : static_cast<double>(a.intVal);
      double bf =
          (b.kind == RuntimeValue::Kind::Float) ? b.floatVal : static_cast<double>(b.intVal);
      int64_t ai = a.intVal, bi = b.intVal;
      bool isFP = (a.kind == RuntimeValue::Kind::Float || b.kind == RuntimeValue::Kind::Float);
      switch (arg.op) {
        case RelOp::EQ:
          return isFP ? af == bf : ai == bi;
        case RelOp::NE:
          return isFP ? af != bf : ai != bi;
        case RelOp::LT:
          return isFP ? af < bf : ai < bi;
        case RelOp::LE:
          return isFP ? af <= bf : ai <= bi;
        case RelOp::GT:
          return isFP ? af > bf : ai > bi;
        case RelOp::GE:
          return isFP ? af >= bf : ai >= bi;
      }
      return false;
    };
    if (lv.kind == RuntimeValue::Kind::Vec) {
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      res.arrayVal.reserve(lv.arrayVal.size());
      for (size_t k = 0; k < lv.arrayVal.size(); ++k) {
        bool b = cmpScalar(lv.arrayVal[k], rv.arrayVal[k]);
        RuntimeValue lane;
        lane.kind = RuntimeValue::Kind::Int;
        lane.bits = 1;
        // Spec §6.4: i1 is signed; true = -1, false = 0.
        lane.intVal = b ? -1 : 0;
        res.arrayVal.push_back(std::move(lane));
      }
      return res;
    }
    RuntimeValue res;
    res.kind = RuntimeValue::Kind::Int;
    res.bits = 1;
    // Spec §6.4: i1 is signed; true = -1, false = 0.
    res.intVal = cmpScalar(lv, rv) ? -1 : 0;
    return res;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalAddrAtom(const AddrAtom &arg, const Store &store) {

    // addr <lv>: return the address of the lvalue's storage with provenance.
    const std::string &varName = arg.lv.base.name;
    if (!store.count(varName))
      throw std::runtime_error("UB: addr of unknown variable " + varName);

    // Allocate / materialize storage.  Use typeMap_ when available so
    // that structs get per-field ObjectInfos and scalars/arrays get a
    // typed ObjectInfo with the correct elemSize and count.
    uint64_t base;
    auto tit = typeMap_.find(varName);
    if (tit != typeMap_.end()) {
      if (auto st = std::get_if<StructType>(&tit->second->v)) {
        auto sit = typeLayout_.structs().find(st->name.name);
        if (sit == typeLayout_.structs().end())
          throw std::runtime_error("Unknown struct: " + st->name.name);
        base = memory_.materializeStruct(varName, *sit->second, store);
      } else {
        base = memory_.allocObject(varName, tit->second, store);
      }
    } else {
      // Fallback for variables without type info: infer from RuntimeValue shape.
      auto ait = memory_.addrMap().find(varName);
      if (ait != memory_.addrMap().end()) {
        base = ait->second;
      } else {
        const RuntimeValue &sv = store.at(varName);
        uint64_t elemSize = 8, count = 1;
        if (sv.kind == RuntimeValue::Kind::Array) {
          count = sv.arrayVal.size();
          elemSize = sv.arrayVal.empty() ? 4
                     : (sv.arrayVal[0].kind == RuntimeValue::Kind::Int)
                         ? (sv.arrayVal[0].bits + 7) / 8
                     : (sv.arrayVal[0].kind == RuntimeValue::Kind::Float) ? sv.arrayVal[0].bits / 8
                                                                          : 8;
        } else if (sv.kind == RuntimeValue::Kind::Int) {
          elemSize = (sv.bits + 7) / 8;
        } else if (sv.kind == RuntimeValue::Kind::Float) {
          elemSize = sv.bits / 8;
        }
        base = memory_.bumpAlloc(elemSize * count);
        TypePtr varType = typeMap_.at(varName);
        memory_.addObject(
            ObjectInfo{
                varName, "", base, base + elemSize * count, elemSize, count,
                static_cast<std::uint64_t>(-1), 0, varType
            }
        );
        memory_.addrMap()[varName] = base;
        if (sv.kind == RuntimeValue::Kind::Array) {
          for (size_t i = 0; i < sv.arrayVal.size(); ++i)
            memory_.heap()[base + i * elemSize] = sv.arrayVal[i];
        } else {
          memory_.heap()[base] = sv;
        }
      }
    }

    // Find the top-level ObjectInfo for the variable.
    const ObjectInfo *curObj = nullptr;
    for (const auto &o: memory_.objects()) {
      if (o.varName == varName && o.fieldName.empty() && o.base == base) {
        curObj = &o;
        break;
      }
    }
    if (!curObj) {
      for (const auto &o: memory_.objects()) {
        if (o.varName == varName && o.fieldName.empty()) {
          curObj = &o;
          break;
        }
      }
    }

    uint64_t addr = base;
    uint64_t provId = curObj ? curObj->provId : 0;

    // We also trace the static type of the sub-expression to identify sub-ObjectInfos.
    TypePtr curType = tit != typeMap_.end() ? tit->second : TypePtr{};

    for (const auto &acc: arg.lv.accesses) {
      if (auto ai = std::get_if<AccessIndex>(&acc)) {
        // Element count / size and the next address come from the
        // static `curType`, not from `curObj`: an array-typed struct
        // field (e.g. `f1: [2] i8`) is registered as a single-element
        // field object (count 1, elemSize = whole field), so trusting
        // curObj->count would falsely reject `…f1[1]` and curObj->base
        // + idx*elemSize would land at the wrong address. Advancing
        // the running `addr` by `idx * sizeof(elem)` is correct for
        // every nesting (top-level array, array-of-struct, 2-D array,
        // struct-of-array).
        auto at = curType ? std::get_if<ArrayType>(&curType->v) : nullptr;
        if (!at)
          throw std::runtime_error("Internal: index access on non-array type");
        TypePtr elemTy = at->elem;
        uint64_t cnt = at->size;
        uint64_t elemSz = typeLayout_.sizeofType(elemTy);
        int64_t idx = 0;
        if (std::holds_alternative<IntLit>(ai->index)) {
          idx = std::get<IntLit>(ai->index).value;
        } else {
          const auto &id = std::get<LocalOrSymId>(ai->index);
          RuntimeValue idxRv = std::get_if<LocalId>(&id) ? store.at(std::get_if<LocalId>(&id)->name)
                                                         : store.at(std::get_if<SymId>(&id)->name);
          idx = idxRv.intVal;
        }
        if (idx < 0 || (uint64_t) idx >= cnt)
          throw UndefinedBehaviorError("UB: addr of out-of-bounds array element");

        // Provenance = the containing array (spec rule 15).
        if (curObj)
          provId = curObj->provId;

        addr = addr + static_cast<uint64_t>(idx) * elemSz;
        curType = elemTy;
        if (curType && (std::holds_alternative<StructType>(curType->v) ||
                        std::holds_alternative<ArrayType>(curType->v)))
          curObj = memory_.findFieldOrStructObject(addr, curType);
        else
          curObj = nullptr;
      } else if (auto af = std::get_if<AccessField>(&acc)) {
        if (!curType)
          throw std::runtime_error("Internal: no type info for field access");
        auto st = std::get_if<StructType>(&curType->v);
        if (!st)
          throw std::runtime_error("Field access on non-struct variable");
        auto sit = typeLayout_.structs().find(st->name.name);
        if (sit == typeLayout_.structs().end())
          throw std::runtime_error("Unknown struct for field access");

        uint64_t off = typeLayout_.fieldOffset(*sit->second, af->field);
        addr = addr + off;

        // Provenance = the containing struct (spec rule 15).
        if (curObj)
          provId = curObj->provId;

        // Update type and find the field's ObjectInfo.
        TypePtr fieldType;
        for (const auto &f: sit->second->fields) {
          if (f.name == af->field) {
            fieldType = f.type;
            break;
          }
        }
        curType = fieldType;
        if (curType) {
          // Find the field ObjectInfo at 'addr'.
          curObj = memory_.findFieldOrStructObject(addr, curType);
        } else {
          curObj = nullptr;
        }
      }
    }

    RuntimeValue rv;
    rv.kind = RuntimeValue::Kind::Ptr;
    rv.ptrVal = addr;
    rv.ptrBase = provId;
    rv.elemSize = curType ? typeLayout_.sizeofType(curType) : 1;
    rv.bits = 64;
    return rv;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalLoadAtom(const LoadAtom &arg, const Store &store) {

    // load <rval>: dereference the pointer
    RuntimeValue ptrRv = evalLValue(arg.rval, store);
    if (ptrRv.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Load through undef pointer");
    if (ptrRv.kind != RuntimeValue::Kind::Ptr)
      throw std::runtime_error("load requires a pointer operand");
    if (ptrRv.ptrVal == 0)
      throw UndefinedBehaviorError("UB: Null pointer dereference in load");
    // Provenance-based bounds check: use ptrBase to locate the exact object.
    const ObjectInfo *obj = memory_.findObjectByProvId(ptrRv.ptrBase);
    if (!obj)
      throw UndefinedBehaviorError("UB: Load from unknown address");
    if (ptrRv.ptrVal < obj->base || ptrRv.ptrVal >= obj->end)
      throw UndefinedBehaviorError("UB: Load out of bounds");
    // [v0.2.1] Rule 15b: typed-access mismatch. The load address
    // must coincide with the start of a cell whose declared type
    // matches the pointer's static type. We check by finding the
    // most specific (field-level) ObjectInfo at the address; if
    // the pointer's type doesn't match that ObjectInfo's type,
    // the types disagree.
    const ObjectInfo *cellObj = memory_.findObject(ptrRv.ptrVal);
    if (cellObj && cellObj->type) {
      if (auto ptrTy = getLValueType(arg.rval)) {
        if (auto pt = std::get_if<PtrType>(&ptrTy->v)) {
          TypePtr cellType =
              typeLayout_.getCellTypeAtOffset(cellObj->type, ptrRv.ptrVal - cellObj->base);
          if (!TypeUtils::areTypesEqual(pt->pointee, cellType))
            throw UndefinedBehaviorError("UB: Typed-access mismatch (rule 15b)");
        }
      }
    }
    auto hit = memory_.heap().find(ptrRv.ptrVal);
    if (hit == memory_.heap().end())
      throw UndefinedBehaviorError("UB: Load from uninitialized memory");
    // Rule 3: reading a leaf whose stored value is undef is UB.
    // The pointer itself is valid, but the pointed-to value may be undef
    // (e.g. an array element initialised with {undef}).
    if (hit->second.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Load reads undef value (rule 3)");
    return hit->second;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalPtrIndexAtom(const PtrIndexAtom &arg, const Store &store) {

    // [v0.2.1] §6.8.9: ptrindex <ptr>, <index> navigates a ptr [N] T
    // to ptr T at element `index`. Strict UB at the navigation site
    // for null (rule 17), undef (rule 18), or one-past-end (rule 19)
    // sources, plus out-of-bounds index (rule 16, index in [0, N]).
    RuntimeValue ptrRv = evalLValue(arg.rval, store);
    if (ptrRv.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: 'ptrindex' through undef pointer");
    if (ptrRv.kind != RuntimeValue::Kind::Ptr)
      throw std::runtime_error("ptrindex requires a pointer operand");
    if (ptrRv.ptrVal == 0)
      throw UndefinedBehaviorError("UB: 'ptrindex' through null pointer");
    const ObjectInfo *obj = memory_.findObjectByProvId(ptrRv.ptrBase);
    if (!obj)
      throw UndefinedBehaviorError("UB: 'ptrindex' on pointer to unknown object");
    if (ptrRv.ptrVal == obj->end)
      throw UndefinedBehaviorError("UB: 'ptrindex' from a one-past-the-end pointer");

    // [v0.2.1 fix] Derive the array element count and element size
    // from the pointer's static type (ptr [N] T), NOT from the
    // provenance ObjectInfo. This is critical for nested arrays:
    // a ptr [3] i32 inside a [2][3] i32 must check against N=3,
    // not the outermost array's count.
    uint64_t arrSize = obj->count;     // fallback
    uint64_t elemSize = obj->elemSize; // fallback
    auto tit = typeMap_.find(arg.rval.base.name);
    TypePtr pointeeType;
    if (tit != typeMap_.end()) {
      TypePtr cur = tit->second;
      if (auto pt = std::get_if<PtrType>(&cur->v)) {
        pointeeType = pt->pointee;
        if (auto at = std::get_if<ArrayType>(&pt->pointee->v)) {
          arrSize = at->size;
          elemSize = typeLayout_.sizeofType(at->elem);
        }
      }
    }

    // Resolve the index (literal or local/sym).
    int64_t idx = 0;
    if (auto il = std::get_if<IntLit>(&arg.index)) {
      idx = il->value;
    } else {
      const auto &id = std::get<LocalOrSymId>(arg.index);
      RuntimeValue idxRv = std::get_if<LocalId>(&id) ? store.at(std::get_if<LocalId>(&id)->name)
                                                     : store.at(std::get_if<SymId>(&id)->name);
      if (idxRv.kind == RuntimeValue::Kind::Undef)
        throw UndefinedBehaviorError("UB: 'ptrindex' index is undef");
      idx = idxRv.intVal;
    }
    // Rule 16: index in [0, N] (N is one-past-end, valid for arithmetic).
    if (idx < 0 || (uint64_t) idx > arrSize)
      throw UndefinedBehaviorError("UB: 'ptrindex' index out of bounds");

    // Compute element address relative to the
    // current pointer position (ptrRv.ptrVal), NOT from the
    // provenance base. This is essential for nested ptrindex:
    // after ptrindex(%p_outer, 1), the inner ptrindex must
    // start from arr[1], not arr[0].
    uint64_t elemAddr = ptrRv.ptrVal + (uint64_t) idx * elemSize;

    // Provenance = the containing array (spec rule 15).
    // The containing array is located at ptrRv.ptrVal and has type `pointeeType` (e.g. [3]
    // i32).
    const ObjectInfo *containingArrayObj = nullptr;
    if (pointeeType) {
      containingArrayObj = memory_.findFieldOrStructObject(ptrRv.ptrVal, pointeeType);
    }
    if (!containingArrayObj) {
      // Fallback: search by base address or create a new ObjectInfo
      containingArrayObj = memory_.findObjectByBaseAddress(ptrRv.ptrVal);
      if (!containingArrayObj) {
        uint64_t subBase = ptrRv.ptrVal;
        uint64_t subEnd = ptrRv.ptrVal + arrSize * elemSize;
        TypePtr subType = pointeeType;
        if (pointeeType) {
          if (auto at = std::get_if<ArrayType>(&pointeeType->v)) {
            subType = at->elem;
          }
        }
        containingArrayObj = &memory_.addObject(
            ObjectInfo{
                obj->varName, "", subBase, subEnd, elemSize, arrSize,
                static_cast<std::uint64_t>(-1), 0, subType
            }
        );
      }
    }

    RuntimeValue rv;
    rv.kind = RuntimeValue::Kind::Ptr;
    rv.bits = 64;
    rv.ptrVal = elemAddr;
    // Provenance = the containing array's provId!
    rv.ptrBase = containingArrayObj->provId;
    rv.elemSize = elemSize;
    return rv;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalPtrFieldAtom(const PtrFieldAtom &arg, const Store &store) {

    // [v0.2.1] §6.8.10: ptrfield <ptr>, <fld> navigates ptr @S to
    // ptr FieldType at the field's static offset.
    RuntimeValue ptrRv = evalLValue(arg.rval, store);
    if (ptrRv.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: 'ptrfield' through undef pointer");
    if (ptrRv.kind != RuntimeValue::Kind::Ptr)
      throw std::runtime_error("ptrfield requires a pointer operand");
    if (ptrRv.ptrVal == 0)
      throw UndefinedBehaviorError("UB: 'ptrfield' through null pointer");
    const ObjectInfo *obj = memory_.findObjectByProvId(ptrRv.ptrBase);
    if (!obj)
      throw UndefinedBehaviorError("UB: 'ptrfield' on pointer to unknown object");
    if (ptrRv.ptrVal == obj->end)
      throw UndefinedBehaviorError("UB: 'ptrfield' from a one-past-the-end pointer");
    // Resolve the pointee struct decl. The rval is an LValue whose
    // base must be a `ptr @S` local; chase that through typeMap_.
    auto tit = typeMap_.find(arg.rval.base.name);
    if (tit == typeMap_.end())
      throw std::runtime_error("ptrfield: no type info for " + arg.rval.base.name);
    TypePtr cur = tit->second;
    for (const auto &acc: arg.rval.accesses) {
      if (auto af = std::get_if<AccessField>(&acc)) {
        if (auto st = std::get_if<StructType>(&cur->v)) {
          auto sit = typeLayout_.structs().find(st->name.name);
          if (sit == typeLayout_.structs().end())
            throw std::runtime_error("Unknown struct in ptrfield rval chain");
          bool found = false;
          for (const auto &f: sit->second->fields)
            if (f.name == af->field) {
              cur = f.type;
              found = true;
              break;
            }
          if (!found)
            throw std::runtime_error("Unknown field in ptrfield rval chain");
        } else {
          cur = nullptr;
          break;
        }
      } else if (auto ai = std::get_if<AccessIndex>(&acc)) {
        (void) ai;
        if (auto at = std::get_if<ArrayType>(&cur->v))
          cur = at->elem;
        else
          cur = nullptr;
        if (!cur)
          break;
      }
    }
    if (!cur || !std::holds_alternative<PtrType>(cur->v))
      throw std::runtime_error("ptrfield: rval is not pointer-typed");
    const auto &pt = std::get<PtrType>(cur->v);
    if (!std::holds_alternative<StructType>(pt.pointee->v))
      throw std::runtime_error("ptrfield: pointee is not a struct");
    auto &st = std::get<StructType>(pt.pointee->v);
    auto sit = typeLayout_.structs().find(st.name.name);
    if (sit == typeLayout_.structs().end())
      throw std::runtime_error("Unknown struct in ptrfield: " + st.name.name);
    uint64_t off = typeLayout_.fieldOffset(*sit->second, arg.field);

    // Find the ObjectInfo of the containing struct (which is @Inner, located at
    // ptrRv.ptrVal).
    const ObjectInfo *containingStructObj =
        memory_.findFieldOrStructObject(ptrRv.ptrVal, pt.pointee);
    if (!containingStructObj) {
      // Fallback: create one if it doesn't exist
      containingStructObj = &memory_.addObject(
          ObjectInfo{
              obj->varName, "", ptrRv.ptrVal, ptrRv.ptrVal + typeLayout_.sizeofType(pt.pointee),
              typeLayout_.sizeofType(pt.pointee), 1, static_cast<std::uint64_t>(-1), 0, pt.pointee
          }
      );
    }

    TypePtr fieldType;
    for (const auto &f: sit->second->fields) {
      if (f.name == arg.field) {
        fieldType = f.type;
        break;
      }
    }

    RuntimeValue rv;
    rv.kind = RuntimeValue::Kind::Ptr;
    rv.bits = 64;
    rv.ptrVal = ptrRv.ptrVal + off;
    // Provenance = the containing struct's provId!
    rv.ptrBase = containingStructObj->provId;
    rv.elemSize = fieldType ? typeLayout_.sizeofType(fieldType) : 1;
    return rv;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalCastAtom(const CastAtom &arg, const Store &store) {

    RuntimeValue v = std::visit(
        [&](auto &&src) -> RuntimeValue {
          using S = std::decay_t<decltype(src)>;
          if constexpr (std::is_same_v<S, IntLit>) {
            RuntimeValue rv;
            rv.kind = RuntimeValue::Kind::Int;
            rv.intVal = src.value;
            rv.bits = 64;
            return rv;
          } else if constexpr (std::is_same_v<S, FloatLit>) {
            RuntimeValue rv;
            rv.kind = RuntimeValue::Kind::Float;
            rv.floatVal = src.value;
            rv.bits = 64;
            return rv;
          } else if constexpr (std::is_same_v<S, SymId>) {
            return store.at(src.name);
          } else {
            return evalLValue(src, store);
          }
        },
        arg.src
    );
    if (v.kind == RuntimeValue::Kind::Undef)
      throw UndefinedBehaviorError("UB: Reading undef in cast");

    // [v0.2.1] Vector cast: lane-wise. v is a Vec; dstType is <N> U.
    if (v.kind == RuntimeValue::Kind::Vec) {
      auto vt = TypeUtils::asVec(arg.dstType);
      if (!vt)
        throw std::runtime_error("Vector cast requires vector dst");
      if (v.arrayVal.size() != vt->size)
        throw std::runtime_error("Vector cast: lane count mismatch");
      RuntimeValue res;
      res.kind = RuntimeValue::Kind::Vec;
      res.arrayVal.reserve(vt->size);
      auto laneBits = TypeUtils::getIntBitWidth(vt->elem);
      bool isFp = vt->elem && std::holds_alternative<FloatType>(vt->elem->v);
      bool isF32 = isFp && std::get<FloatType>(vt->elem->v).kind == FloatType::Kind::F32;
      uint32_t fpBits = isFp ? (isF32 ? 32u : 64u) : 0u;
      for (auto &lane: v.arrayVal) {
        if (lane.kind == RuntimeValue::Kind::Undef)
          throw UndefinedBehaviorError("UB: undef lane in cast");
        RuntimeValue r;
        if (laneBits) {
          r.kind = RuntimeValue::Kind::Int;
          r.bits = *laneBits;
          if (lane.kind == RuntimeValue::Kind::Int) {
            r.intVal = canonicalize(lane.intVal, r.bits);
          } else {
            double lo = -std::ldexp(1.0, static_cast<int>(r.bits) - 1);
            double hi = std::ldexp(1.0, static_cast<int>(r.bits) - 1);
            if (std::isnan(lane.floatVal) || std::isinf(lane.floatVal) || lane.floatVal < lo ||
                lane.floatVal >= hi)
              throw UndefinedBehaviorError("UB: vector lane float->int OOR");
            r.intVal = static_cast<int64_t>(lane.floatVal);
          }
        } else if (isFp) {
          r.kind = RuntimeValue::Kind::Float;
          r.bits = fpBits;
          double raw = (lane.kind == RuntimeValue::Kind::Int) ? static_cast<double>(lane.intVal)
                                                              : lane.floatVal;
          r.floatVal = isF32 ? static_cast<double>(static_cast<float>(raw)) : raw;
          if (isF32 && std::isinf(r.floatVal))
            throw UndefinedBehaviorError("UB: vector lane f32 overflow to inf");
        } else {
          throw std::runtime_error("Vector cast: unsupported lane dst type");
        }
        res.arrayVal.push_back(std::move(r));
      }
      return res;
    }

    RuntimeValue res;
    auto dstBits = TypeUtils::getIntBitWidth(arg.dstType);
    if (dstBits) {
      res.kind = RuntimeValue::Kind::Int;
      res.bits = *dstBits;
      if (v.kind == RuntimeValue::Kind::Int) {
        res.intVal = canonicalize(v.intVal, res.bits);
      } else {
        // Float to Int: check for out-of-range (spec §7.4 rule 8).
        // lo = -2^(bits-1), hi = 2^(bits-1); valid range is [lo, hi).
        double lo = -std::ldexp(1.0, static_cast<int>(res.bits) - 1);
        double hi = std::ldexp(1.0, static_cast<int>(res.bits) - 1);
        if (std::isnan(v.floatVal) || std::isinf(v.floatVal) || v.floatVal < lo || v.floatVal >= hi)
          throw UndefinedBehaviorError("UB: Float-to-integer cast out of range");
        res.intVal = static_cast<int64_t>(v.floatVal);
      }
    } else if (arg.dstType && std::holds_alternative<FloatType>(arg.dstType->v)) {
      res.kind = RuntimeValue::Kind::Float;
      bool isF32 = std::get<FloatType>(arg.dstType->v).kind == FloatType::Kind::F32;
      res.bits = isF32 ? 32 : 64;
      double raw = (v.kind == RuntimeValue::Kind::Int) ? static_cast<double>(v.intVal) : v.floatVal;
      // For f32 destination, round to f32 precision so the stored
      // value matches what C/WASM lowering would compute. Without
      // this, e.g. `268435457 as f32` would stay as 268435457.0 in
      // double precision instead of being rounded to 268435456.0f
      // (round-to-nearest-even at the f32 boundary).
      res.floatVal = isF32 ? static_cast<double>(static_cast<float>(raw)) : raw;
      // SPEC §6.4 / §7.4 rule 6: f64 -> f32 narrowing is UB if the
      // result overflows to ±∞ (e.g. 1e40 as f32). Trap here for any
      // FP -> f32 that produced ±∞ after rounding (integer sources
      // can't overflow per spec, but the check covers them harmlessly).
      if (isF32 && std::isinf(res.floatVal))
        throw UndefinedBehaviorError("UB: Float narrowing cast overflows to infinity");
    }
    return res;

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalCallAtom(const CallAtom &arg, const Store &store) {

    // [v0.2.2] Function call. Phase 4 only handles intrinsics; fun
    // calls and contract/link `decl` calls land in Phases 5/7/8.
    // §2.12 strict left-to-right evaluation order.
    std::vector<RuntimeValue> argVals;
    argVals.reserve(arg.args.size());
    for (const auto &ap: arg.args) {
      argVals.push_back(evalExpr(*ap, store));
    }
    // [v0.2.2] Honour the overload the type checker pinned onto
    // this call site. Falling back to a local heuristic would
    // diverge from the type checker (which uses return-type
    // context) — see `CallAtom::resolvedIntrinsic`. The fallback
    // path below only fires for un-typechecked ASTs (e.g. raw
    // tests that bypass the pass manager).
    const IntrinsicDecl *intr = arg.resolvedIntrinsic;
    if (!intr) {
      for (const auto &i: prog_.intrinsics) {
        if (i.name.name != arg.callee.name)
          continue;
        if (i.params.size() != arg.args.size())
          continue;
        if (!intr) {
          intr = &i;
          continue;
        }
        auto bw1 = TypeUtils::getIntBitWidth(intr->params[0].type);
        auto bw2 = TypeUtils::getIntBitWidth(i.params[0].type);
        if (!bw1 || !bw2)
          continue;
        std::uint32_t argBW = 32;
        if (!arg.args.empty()) {
          auto at = getExprType(*arg.args[0]);
          if (at) {
            if (auto b = TypeUtils::getIntBitWidth(at))
              argBW = *b;
          }
        }
        if (*bw2 == argBW && *bw1 != argBW)
          intr = &i;
      }
    }
    if (intr) {
      return callIntrinsic(*intr, argVals, arg.span);
    }
    // [v0.2.2 Phase 5] `fun` target: nested call frame.
    for (const auto &f: prog_.funs)
      if (f.name.name == arg.callee.name) {
        RuntimeValue rv = callFunction(f, argVals);
        // [v0.2.2] §9.6.1 step 5: callee may have mutated caller
        // memory through pointer arguments. Refresh any addr-
        // promoted caller-side local before the next read.
        syncStoreFromHeap(const_cast<Store &>(store));
        return rv;
      }
    for (const auto &d: prog_.extDecls)
      if (d.name.name == arg.callee.name) {
        if (d.contract) {
          // [v0.2.2] §11.3: interpreter rejects contract-form decl calls.
          throw std::runtime_error(
              "Interpreter rejects call to contract-form `decl` (no body): " + arg.callee.name
          );
        }
        throw std::runtime_error(
            "Interpreter: link-form `decl` has no body in any -I path: " + arg.callee.name
        );
      }
    throw std::runtime_error("Call to undeclared function: " + arg.callee.name);

    return RuntimeValue{};
  }

  RuntimeValue Interpreter::evalCoef(const Coef &c, const Store &store) {
    if (std::holds_alternative<IntLit>(c)) {
      const auto &lit = std::get<IntLit>(c);
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Int;
      rv.intVal = lit.value;
      // Use the bitwidth resolved by the type checker (i32 by default per
      // SPEC §3.3.1).  A zero resolvedBits means the literal was never
      // type-checked, which is a logic error — fall back to 64 defensively.
      rv.bits = (lit.resolvedBits != 0) ? lit.resolvedBits : 64;
      return rv;
    }
    if (std::holds_alternative<FloatLit>(c)) {
      const auto &flit = std::get<FloatLit>(c);
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Float;
      rv.floatVal = flit.value;
      // Use the bitwidth resolved by the type checker (32 for f32, 64 for
      // f64; default f32 per SPEC §3.3.1).  A zero resolvedBits means the
      // literal was never type-checked — fall back to 64 defensively.
      rv.bits = (flit.resolvedBits != 0) ? flit.resolvedBits : 64;
      return rv;
    }
    if (std::holds_alternative<NullLit>(c)) {
      RuntimeValue rv;
      rv.kind = RuntimeValue::Kind::Ptr;
      rv.ptrVal = 0;
      rv.bits = 64;
      return rv;
    }
    const auto &id = std::get<LocalOrSymId>(c);
    if (auto lid = std::get_if<LocalId>(&id))
      return store.at(lid->name);
    auto sid = std::get_if<SymId>(&id);
    if (store.count(sid->name))
      return store.at(sid->name);
    throw std::runtime_error("Internal error: Unbound symbol " + sid->name);
  }

  RuntimeValue Interpreter::evalSelectVal(const SelectVal &sv, const Store &store) {
    if (std::holds_alternative<RValue>(sv))
      return evalLValue(std::get<RValue>(sv), store);
    return evalCoef(std::get<Coef>(sv), store);
  }
} // namespace refractir
