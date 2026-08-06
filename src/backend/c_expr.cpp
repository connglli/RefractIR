#include <cmath>
#include <sstream>
#include "analysis/intrinsics.hpp"
#include "analysis/type_utils.hpp"
#include "backend/c_backend.hpp"
#include "c_internal.hpp"

namespace refractir {

  void CBackend::emitExpr(const Expr &expr) {
    out_ << "(";
    emitAtom(expr.first);
    for (const auto &t: expr.rest) {
      out_ << (t.op == AddOp::Plus ? " + " : " - ");
      emitAtom(t.atom);
    }
    out_ << ")";
  }

  void CBackend::emitAtom(const Atom &atom) {
    out_ << "(";
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, OpAtom>) {
            emitOpAtom(arg);
          } else if constexpr (std::is_same_v<T, SelectAtom>) {
            emitSelectAtom(arg);
          } else if constexpr (std::is_same_v<T, CmpAtom>) {
            emitCmpAtom(arg);
          } else if constexpr (std::is_same_v<T, CoefAtom>) {
            emitCoef(arg.coef);
          } else if constexpr (std::is_same_v<T, RValueAtom>) {
            emitLValue(arg.rval);
          } else if constexpr (std::is_same_v<T, UnaryAtom>) {
            emitUnaryAtom(arg);
          } else if constexpr (std::is_same_v<T, AddrAtom>) {
            emitAddrAtom(arg);
          } else if constexpr (std::is_same_v<T, LoadAtom>) {
            emitLoadAtom(arg);
          } else if constexpr (std::is_same_v<T, PtrIndexAtom>) {
            emitPtrIndexAtom(arg);
          } else if constexpr (std::is_same_v<T, PtrFieldAtom>) {
            emitPtrFieldAtom(arg);
          } else if constexpr (std::is_same_v<T, CallAtom>) {
            emitCallAtom(arg);
          } else if constexpr (std::is_same_v<T, CastAtom>) {
            emitCastAtom(arg);
          }
        },
        atom.v
    );
    out_ << ")";
  }

  void CBackend::emitOpAtom(const OpAtom &arg) {

    if (arg.op == AtomOpKind::LShr) {
      // LShr is the logical (unsigned) right shift. C's signed `>>`
      // is implementation-defined for negative LHS, so we cast through
      // unsigned to get well-defined zero-fill semantics matching the
      // RefractIR interpreter. (Shl/Shr deliberately use raw signed shifts:
      // RefractIR spec §7.1 rule 4 treats SHL result overflow as UB, so
      // any program reaching here with overflowing SHL is on an
      // infeasible path — UBSan-trap is the correct surfacing.)
      std::uint32_t bits = 32;
      if (std::holds_alternative<LocalOrSymId>(arg.coef)) {
        auto name = std::visit([](auto &&v) { return v.name; }, std::get<LocalOrSymId>(arg.coef));
        if (varWidths_.count(name))
          bits = varWidths_.at(name);
      } else {
        int64_t val = std::get<IntLit>(arg.coef).value;
        if (val < INT32_MIN || val > INT32_MAX)
          bits = 64;
      }

      out_ << "(";
      if (bits <= 8)
        out_ << "int8_t)((uint8_t)";
      else if (bits <= 16)
        out_ << "int16_t)((uint16_t)";
      else if (bits <= 32)
        out_ << "int32_t)((uint32_t)";
      else
        out_ << "int64_t)((uint64_t)";

      emitCoef(arg.coef);
      out_ << " >> ";
      emitLValue(arg.rval);
      out_ << ")";
    } else if (arg.op == AtomOpKind::Mod) {
      // C fmod (truncated quotient): same semantics as integer %, consistent
      // with RefractIR spec which aligns float % with integer % (truncate-toward-zero).
      // Detect float operand via coef type; for a literal coef, the rval
      // pins f32 vs f64 (operands share a type in RefractIR).
      auto floatKindOf = [this](const TypePtr &t) -> FloatType::Kind * {
        if (!t)
          return nullptr;
        if (auto ft = std::get_if<FloatType>(&t->v))
          return &const_cast<FloatType::Kind &>(ft->kind);
        return nullptr;
      };
      bool isFloat = std::holds_alternative<FloatLit>(arg.coef);
      bool isF32 = false;
      if (auto *lid = std::get_if<LocalOrSymId>(&arg.coef)) {
        auto name = std::visit([](auto &&v) { return v.name; }, *lid);
        auto it = varTypes_.find(name);
        if (it != varTypes_.end()) {
          if (auto *k = floatKindOf(it->second)) {
            isFloat = true;
            isF32 = (*k == FloatType::Kind::F32);
          }
        }
      }
      if (isFloat && !isF32) {
        if (auto *k = floatKindOf(getLValueType(arg.rval)))
          isF32 = (*k == FloatType::Kind::F32);
      }
      if (isFloat) {
        // Spec §2.9 encodes `%` as
        //   fp.sub(x, fp.mul(fp.roundToIntegral[RTZ](fp.div[RNE](x, y)), y))
        // The inner fp.div is subject to §7.4 rule 6: if x/y at the
        // operand precision overflows or is NaN, the path is UB even
        // when libm fmod would return a finite remainder. Emit the
        // intermediate-finiteness check inside a statement expression
        // so the operand emissions are not duplicated.
        const char *ty = isF32 ? "float" : "double";
        const char *fn = isF32 ? "fmodf" : "fmod";
        out_ << "({ " << ty << " _mx = ";
        emitCoef(arg.coef);
        out_ << ", _my = ";
        emitLValue(arg.rval);
        out_ << "; ";
        if (!noUbGuards_)
          out_ << "if (!__builtin_isfinite(_mx / _my)) __builtin_trap(); ";
        out_ << fn << "(_mx, _my); })";
      } else if (!noUbGuards_ && [&] {
                   auto rt = getLValueType(arg.rval);
                   return rt && std::holds_alternative<IntType>(rt->v);
                 }()) {
        // [v0.2.3] Scalar-integer remainder-by-zero guard (self-contained,
        // so the emitted C traps without depending on downstream UBSan).
        // Signed-overflow (INT_MIN % -1) is deliberately left to UBSan.
        // Only scalar ints: a vecext whole-vector `%` can't take a scalar
        // `_rz == 0` guard (and integer-vector div/rem stays with UBSan,
        // as before).
        out_ << "({ __typeof__(";
        emitLValue(arg.rval);
        out_ << ") _rz = ";
        emitLValue(arg.rval);
        out_ << "; if (_rz == 0) __builtin_trap(); ";
        emitCoef(arg.coef);
        out_ << " % _rz; })";
      } else {
        emitCoef(arg.coef);
        out_ << " % ";
        emitLValue(arg.rval);
      }
    } else if (arg.op == AtomOpKind::Div && !noUbGuards_ && [&] {
                 auto rt = getLValueType(arg.rval);
                 return rt && std::holds_alternative<IntType>(rt->v);
               }()) {
      // [v0.2.3] Integer division-by-zero guard (self-contained, so the
      // emitted C traps without depending on downstream UBSan; float
      // division-by-zero is caught by the post-assignment isfinite
      // check instead). Signed-overflow (INT_MIN / -1) is left to UBSan.
      out_ << "({ __typeof__(";
      emitLValue(arg.rval);
      out_ << ") _dz = ";
      emitLValue(arg.rval);
      out_ << "; if (_dz == 0) __builtin_trap(); ";
      emitCoef(arg.coef);
      out_ << " / _dz; })";
    } else {
      emitCoef(arg.coef);
      switch (arg.op) {
        case AtomOpKind::Mul:
          out_ << " * ";
          break;
        case AtomOpKind::Div:
          out_ << " / ";
          break;
        case AtomOpKind::Mod:
          // unreachable: handled above
          break;
        case AtomOpKind::And:
          out_ << " & ";
          break;
        case AtomOpKind::Or:
          out_ << " | ";
          break;
        case AtomOpKind::Xor:
          out_ << " ^ ";
          break;
        case AtomOpKind::Shl:
          out_ << " << ";
          break;
        case AtomOpKind::Shr:
          out_ << " >> ";
          break;
        default:
          break;
      }
      emitLValue(arg.rval);
    }
  }

  void CBackend::emitSelectAtom(const SelectAtom &arg) {

    // [v0.2.1] Cond form lowers to C's `?:` directly. The mask form
    // with a vector mask requires per-lane emission, special-cased
    // at AssignInstr level; with a scalar i1 mask it lowers to the
    // same `?:` after a `!= 0` predicate (so we don't depend on the
    // mask producing an exact `1` bit pattern).
    if (arg.cond) {
      out_ << "(";
      emitCond(*arg.cond);
      out_ << " ? ";
      emitSelectVal(arg.vtrue);
      out_ << " : ";
      emitSelectVal(arg.vfalse);
      out_ << ")";
    } else {
      // Mask form. Vector masks are out of expression-position scope.
      auto maskTy = getExprType(*arg.maskExpr);
      if (maskTy && std::holds_alternative<VecType>(maskTy->v)) {
        throw std::runtime_error(
            "vector mask-form select must be the RHS of an assignment "
            "(inline use not yet supported in this codegen)"
        );
      }
      out_ << "((";
      emitExpr(*arg.maskExpr);
      out_ << ") != 0 ? ";
      emitSelectVal(arg.vtrue);
      out_ << " : ";
      emitSelectVal(arg.vfalse);
      out_ << ")";
    }
  }

  void CBackend::emitCmpAtom(const CmpAtom & /*arg*/) {

    // [v0.2.1] Same restriction: cmp returns a vector value that
    // needs lane-wise emission, handled at AssignInstr level.
    throw std::runtime_error("cmp must be the RHS of an assignment (inline use not yet supported)");
  }

  void CBackend::emitUnaryAtom(const UnaryAtom &arg) {

    if (arg.op == UnaryOpKind::Not)
      out_ << "~";
    emitLValue(arg.rval);
  }

  void CBackend::emitAddrAtom(const AddrAtom &arg) {

    // [v0.2.2] With ptr-to-array typedefs in place, `&lv` has
    // the right C type for every aggregate / scalar lv:
    // `int8_t (*)[3][3]` for a 2D array maps to the typedef
    // `_sym_arr_3x3_i8 *`, `struct S *` for a struct, plain
    // `T *` for a scalar.  No special casts needed.
    out_ << "&";
    emitLValue(arg.lv);
  }

  void CBackend::emitLoadAtom(const LoadAtom &arg) {

    if (noUbGuards_) {
      // No null / typed-access guards: dereference directly.
      out_ << "(*(";
      emitLValue(arg.rval);
      out_ << "))";
      return;
    }

    // [v0.2.1] Rule 15b (typed-access mismatch): load through a
    // pointer that doesn't have enough room for the pointee. Use
    // __builtin_object_size so we trap when the pointer has been
    // walked past its originating field/array.
    out_ << "({ __typeof__(";
    emitLValue(arg.rval);
    out_ << ") _pl = ";
    emitLValue(arg.rval);
    out_ << "; if (!_pl) __builtin_trap();"
         << " if (__builtin_object_size(_pl, 0) != (size_t)-1 &&"
         << "     __builtin_object_size(_pl, 0) < sizeof(*_pl)) __builtin_trap();"
         << " *_pl; })";
  }

  void CBackend::emitPtrIndexAtom(const PtrIndexAtom &arg) {

    if (noUbGuards_) {
      // No null / index-bounds guards: navigate directly. `*p` is the
      // array pointee, which decays to a pointer on `+ i`.
      out_ << "((*(";
      emitLValue(arg.rval);
      out_ << ")) + (int64_t)(";
      emitIndex(arg.index);
      out_ << "))";
      return;
    }

    // [v0.2.1] ptrindex p, i → element pointer. Rule 17 (null nav),
    // rule 18 (undef nav — relies on UBSan), rule 16 (index bounds).
    // We emit:  ({ if (!p) __builtin_trap();
    //              if ((uint64_t)i > N) __builtin_trap();
    //              (*p) + i; })
    // The N comes from the source pointer's array pointee.
    //
    // [v0.2.2] The source pointer's C type is now `Elem (*)[N]…`
    // (typedef'd, see `arrayPtrTypedefName`).  Dereferencing
    // once gives the array, which decays to a pointer-to-
    // inner-element on `+ i`; the stride matches RefractIR's
    // ptrindex semantics naturally.
    uint64_t arrSize = 0;
    auto rvTy = getLValueType(arg.rval);
    if (rvTy) {
      if (auto pt = std::get_if<PtrType>(&rvTy->v))
        if (auto at = std::get_if<ArrayType>(&pt->pointee->v))
          arrSize = at->size;
    }
    out_ << "({ __typeof__(";
    emitLValue(arg.rval);
    out_ << ") _pi = ";
    emitLValue(arg.rval);
    out_ << "; int64_t _ii = (int64_t)(";
    emitIndex(arg.index);
    out_ << "); if (!_pi) __builtin_trap();";
    if (arrSize > 0)
      out_ << " if (_ii < 0 || (uint64_t)_ii > " << arrSize << "ULL) __builtin_trap();";
    out_ << " (*_pi) + _ii; })";
  }

  void CBackend::emitPtrFieldAtom(const PtrFieldAtom &arg) {

    if (noUbGuards_) {
      // No null / one-past-end guards: take the field address directly.
      out_ << "(&((";
      emitLValue(arg.rval);
      out_ << ")->" << arg.field << "))";
      return;
    }

    // [v0.2.1] ptrfield p, f → field pointer. Rule 17 (null nav)
    // and rule 19 (one-past-end nav). The one-past-end check uses
    // __builtin_object_size: a valid struct pointer has at least
    // sizeof(struct) bytes of object remaining; one-past-end has
    // zero bytes (and GCC reports 0).
    out_ << "({ __typeof__(";
    emitLValue(arg.rval);
    out_ << ") _pf = ";
    emitLValue(arg.rval);
    out_ << "; if (!_pf) __builtin_trap();"
         << " if (__builtin_object_size(_pf, 0) < sizeof(*_pf)) __builtin_trap();"
         << " &(_pf->" << arg.field << "); })";
  }

  void CBackend::emitCallAtom(const CallAtom &arg) {

    // [v0.2.2] Lower `call @f(args...)` to a C call expression.
    // For intrinsics, dispatch to the helper emitted in emit() §1b.
    // For other targets, use the mangled name directly (link-form
    // decls have extern prototypes; fun bodies are emitted later).
    // [v0.2.2] Use the overload the type checker pinned onto the
    // AST node — see CallAtom::resolvedIntrinsic. The fallback
    // path only runs for un-typechecked input.
    const IntrinsicDecl *intr = arg.resolvedIntrinsic;
    if (!intr) {
      for (const auto &i: prog_->intrinsics) {
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
        if (!bw1 || !bw2 || *bw1 == *bw2)
          continue;
        uint32_t argBW = *bw1;
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
      out_ << intrinsicHelperName(*intr);
    } else {
      out_ << mangleName(arg.callee.name);
    }
    // [v0.2.3 V1] A reduction takes the N lanes of its vector argument as
    // scalar parameters, emitted per-lane here. This makes the helper
    // strategy-independent — it works even for `scalars` / `array`, which
    // cannot pass a vector across a C function boundary by value.
    if (intr) {
      if (auto rk = getIntrinsicKind(intr->name.name); rk && isReductionIntrinsic(*rk)) {
        const VecType &vt = std::get<VecType>(intr->params[0].type->v);
        out_ << "(";
        for (std::uint64_t lane = 0; lane < vt.size; ++lane) {
          if (lane)
            out_ << ", ";
          out_ << emitVecExprLane(*arg.args[0], vt, lane);
        }
        out_ << ")";
        return;
      }
    }
    out_ << "(";
    // Per-arg context: the callee's i-th param type drives
    // isDoubleCtx_ so a FloatLit arg emits with/without the
    // `f` suffix correctly. Without this, args inherit the
    // surrounding caller context — e.g. an f64 callee param
    // receiving a -32018945.886718754 literal inside an i32
    // expression would get the (non-double) `f` suffix and
    // silently truncate to float precision before the call.
    std::vector<TypePtr> paramTypes;
    if (intr) {
      paramTypes.reserve(intr->params.size());
      for (const auto &p: intr->params)
        paramTypes.push_back(p.type);
    } else {
      for (const auto &fd: prog_->funs) {
        if (fd.name.name == arg.callee.name) {
          paramTypes.reserve(fd.params.size());
          for (const auto &p: fd.params)
            paramTypes.push_back(p.type);
          break;
        }
      }
    }
    for (size_t i = 0; i < arg.args.size(); ++i) {
      if (i)
        out_ << ", ";
      TypePtr argCtxTy = (i < paramTypes.size()) ? paramTypes[i] : TypePtr{};
      CtxGuard ctx(isDoubleCtx_, argCtxTy ? isOrContainsF64(argCtxTy) : isDoubleCtx_);
      emitExpr(*arg.args[i]);
    }
    out_ << ")";
  }

  void CBackend::emitCastAtom(const CastAtom &arg) {

    // [v0.2.1] Vector cast: a C-style `(target_vec_t)(src_vec)`
    // is a *bitcast* in GCC vec-ext, not a per-lane conversion.
    // The right primitive is `__builtin_convertvector`.
    if (arg.dstType && std::holds_alternative<VecType>(arg.dstType->v)) {
      out_ << "__builtin_convertvector(";
      std::visit(
          [&](auto &&src) {
            using S = std::decay_t<decltype(src)>;
            if constexpr (std::is_same_v<S, LValue>) {
              emitLValue(src);
            } else {
              out_ << "/*unsupported vec cast src*/";
            }
          },
          arg.src
      );
      out_ << ", ";
      emitType(arg.dstType);
      out_ << ")";
      return;
    }
    // [v0.2.2 §2 / bug-mine fix] All `iN` types are SIGNED bit
    // vectors. A narrow source (e.g. i1 = {0, -1}) stored in a
    // wider C type loses its sign bit at the iN boundary —
    // casting it directly with `(dst)(src)` zero-extends rather
    // than sign-extending. Pre-extend the source from its
    // declared iN bits up to int64_t with the shift-pair idiom
    // so the cast preserves signed semantics. The source type is
    // resolved through the full access path (array element,
    // struct field, vector lane), not just bare locals.
    std::optional<std::uint32_t> srcBits;
    if (auto lv = std::get_if<LValue>(&arg.src)) {
      TypePtr srcType = getLValueType(*lv);
      if (srcType && std::holds_alternative<IntType>(srcType->v))
        srcBits = TypeUtils::getIntBitWidth(srcType);
    }
    const bool intDst = arg.dstType && std::holds_alternative<IntType>(arg.dstType->v);
    const bool needSext = intDst && srcBits.has_value() && *srcBits < 64 && *srcBits > 0;
    // [P7] Narrowing to a CUSTOM destination width must truncate
    // mod 2^M and sign-extend from bit M-1 (SPEC §6.4). Native
    // widths get this for free from the C cast — `(int8_t)x`
    // keeps the low 8 bits — but a custom width's storage type
    // is wider (i20 lives in int32_t), so `(int32_t)x` keeps all
    // 32 bits and leaks out-of-range values. Emulate the C
    // type system with the shift-pair on the int64 value. The
    // inner `(int64_t)` also converts float sources, whose
    // out-of-range values are UB anyway (SPEC rule 8).
    std::uint32_t dstBits = intDst ? TypeUtils::getIntBitWidth(arg.dstType).value_or(64) : 64;
    const bool nativeDst = dstBits == 8 || dstBits == 16 || dstBits == 32 || dstBits == 64 || dstBits == 128;
    const bool needTrunc = intDst && !nativeDst;
    out_ << "(";
    emitType(arg.dstType);
    out_ << ")(";
    if (needTrunc) {
      if (dstBits > 64) {
        out_ << "(__int128)((unsigned __int128)((__int128)(";
      } else {
        out_ << "(int64_t)((uint64_t)((int64_t)(";
      }
    }
    if (needSext) {
      // Sign-extend via unsigned shift so UBSan doesn't trip on
      // "left shift of negative value". `(int64_t)((uint64_t)x
      // << K) >> K` widens unsigned, then arith-shifts right.
      out_ << "(int64_t)((uint64_t)(";
    }
    std::visit(
        [&](auto &&src) {
          using S = std::decay_t<decltype(src)>;
          if constexpr (std::is_same_v<S, IntLit>) {
            out_ << src.value;
          } else if constexpr (std::is_same_v<S, FloatLit>) {
            CtxGuard ctx(isDoubleCtx_, isOrContainsF64(arg.dstType));
            out_ << formatFloatLit(src.value);
            if (!isDoubleCtx_)
              out_ << "f";
          } else if constexpr (std::is_same_v<S, SymId>) {
            out_ << getMangledSymbolName(curFuncName_, src.name) << "()";
          } else {
            emitLValue(src);
          }
        },
        arg.src
    );
    if (needSext) {
      std::uint32_t shift = 64u - *srcBits;
      out_ << ") << " << shift << ") >> " << shift;
    }
    if (needTrunc) {
      std::uint32_t shift = (dstBits > 64 ? 128u : 64u) - dstBits;
      out_ << ")) << " << shift << ") >> " << shift;
    }
    out_ << ")";
  }

  void CBackend::emitCond(const Cond &cond) {
    // Take the type from either operand: RefractIR requires lhs and rhs to share
    // a type, so the disjunction is just defensive against missing lookups.
    CtxGuard ctx(
        isDoubleCtx_,
        isOrContainsF64(getExprType(cond.lhs)) || isOrContainsF64(getExprType(cond.rhs))
    );
    bool hasI1 = isI1Type(getExprType(cond.lhs)) || isI1Type(getExprType(cond.rhs));
    if (hasI1)
      out_ << "(";
    emitExpr(cond.lhs);
    if (hasI1)
      out_ << " & 1)";
    switch (cond.op) {
      case RelOp::EQ:
        out_ << " == ";
        break;
      case RelOp::NE:
        out_ << " != ";
        break;
      case RelOp::LT:
        out_ << " < ";
        break;
      case RelOp::LE:
        out_ << " <= ";
        break;
      case RelOp::GT:
        out_ << " > ";
        break;
      case RelOp::GE:
        out_ << " >= ";
        break;
    }
    if (hasI1)
      out_ << "(";
    emitExpr(cond.rhs);
    if (hasI1)
      out_ << " & 1)";
  }

  void CBackend::emitCoef(const Coef &coef) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else if constexpr (std::is_same_v<T, FloatLit>) {
            out_ << formatFloatLit(arg.value);
            if (!isDoubleCtx_)
              out_ << "f";
          } else if constexpr (std::is_same_v<T, NullLit>) {
            out_ << "NULL";
          } else {
            std::visit(
                [this](auto &&id) {
                  if constexpr (std::is_same_v<std::decay_t<decltype(id)>, SymId>) {
                    out_ << getMangledSymbolName(curFuncName_, id.name) << "()";
                  } else {
                    out_ << mangleName(id.name);
                  }
                },
                arg
            );
          }
        },
        coef
    );
  }

  void CBackend::emitSelectVal(const SelectVal &sv) {
    if (std::holds_alternative<RValue>(sv))
      emitLValue(std::get<RValue>(sv));
    else
      emitCoef(std::get<Coef>(sv));
  }

  void CBackend::emitIndex(const Index &idx) {
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntLit>) {
            out_ << arg.value;
          } else {
            std::visit(
                [this](auto &&id) {
                  if constexpr (std::is_same_v<std::decay_t<decltype(id)>, SymId>) {
                    out_ << getMangledSymbolName(curFuncName_, id.name) << "()";
                  } else {
                    out_ << mangleName(id.name);
                  }
                },
                arg
            );
          }
        },
        idx
    );
  }
} // namespace refractir
