#include "interp/interpreter.hpp"
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif
#include "analysis/cfg.hpp"
#include "analysis/type_utils.hpp"
#include "error.hpp"
#include "frontend/diagnostics.hpp"
#include "internal.hpp"

namespace refractir {

  static std::ostream &operator<<(std::ostream &os, __int128 val) {
    if (val == 0) return os << "0";
    char buf[128];
    int pos = 127;
    buf[pos] = '\0';
    bool neg = false;
    unsigned __int128 u = val;
    if (val < 0) {
      neg = true;
      u = -val;
    }
    while (u > 0) {
      buf[--pos] = '0' + (u % 10);
      u /= 10;
    }
    if (neg) {
      buf[--pos] = '-';
    }
    return os << &buf[pos];
  }

  Interpreter::Interpreter(const Program &prog) : Interpreter(prog, std::cout) {}

  Interpreter::Interpreter(const Program &prog, std::ostream &out) :
      prog_(prog), out_(out), typeLayout_(prog), memory_(typeLayout_) {}

  void Interpreter::run(
      const std::string &entryFuncName, const SymBindings &symBindings,
      const std::vector<std::string> &paramArgs, bool dumpExec
  ) {
    // Ensure IEEE 754 RNE rounding mode regardless of process FP environment.
    std::fesetround(FE_TONEAREST);
    // Spec §2.9 guarantees subnormals are first-class (no flush-to-zero).
    // Defensively clear MXCSR FTZ and DAZ in case a parent process or an
    // upstream library toggled them on before invoking symiri — those bits
    // survive across some entry paths and would silently flush f32 subnormals
    // in the interpreter's arithmetic.
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_OFF);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_OFF);
#endif
    dumpExec_ = dumpExec;
    nextFrameId_ = 0;
    blockSteps_ = 0;
    const FunDecl *entry = nullptr;
    for (const auto &f: prog_.funs) {
      if (f.name.name == entryFuncName) {
        entry = &f;
        break;
      }
    }
    if (!entry) {
      throw std::runtime_error("Entry function not found: " + entryFuncName);
    }

    // [v0.2.2] Bind positional arguments to entry-fun parameters.
    if (paramArgs.size() != entry->params.size()) {
      throw std::runtime_error(
          "Entry function " + entryFuncName + " expects " + std::to_string(entry->params.size()) +
          " parameter argument(s) but got " + std::to_string(paramArgs.size()) +
          " — supply them as positional CLI args after the input file."
      );
    }
    std::vector<RuntimeValue> args;
    args.reserve(entry->params.size());
    for (size_t i = 0; i < entry->params.size(); ++i) {
      const auto &p = entry->params[i];
      RuntimeValue v;
      auto bits = TypeUtils::getIntBitWidth(p.type);
      if (p.type && std::holds_alternative<FloatType>(p.type->v)) {
        v.kind = RuntimeValue::Kind::Float;
        v.bits = (std::get<FloatType>(p.type->v).kind == FloatType::Kind::F32) ? 32 : 64;
        v.floatVal = parseFloatLiteral(paramArgs[i]);
      } else if (bits) {
        v.kind = RuntimeValue::Kind::Int;
        v.bits = *bits;
        v.intVal = parseIntegerLiteral(paramArgs[i]);
        // [v0.2.2] CLI positional args ARE literals being bound to the
        // entry function's parameters; apply the same signed-range
        // rule the typechecker enforces on in-source literals
        // (spec §6.12 + §6.4).  Without this an out-of-range CLI value
        // would silently bit-truncate at runtime, diverging from what
        // the static checker would reject for the same constant in
        // source code.
        if (*bits < 64) {
          int64_t minV = -(1LL << (*bits - 1));
          int64_t maxV = (1LL << (*bits - 1)) - 1;
          if (v.intVal < minV || v.intVal > maxV) {
            throw std::runtime_error(
                "CLI arg " + std::to_string(i + 1) + " (" + paramArgs[i] +
                "): value out of range for parameter type i" + std::to_string(*bits) + " ([" +
                std::to_string(minV) + ", " + std::to_string(maxV) + "])"
            );
          }
        }
      } else {
        throw std::runtime_error(
            "Entry-fun parameter " + p.name.name +
            ": unsupported type for CLI positional arg (only scalar int/float)"
        );
      }
      args.push_back(v);
    }
    symBindings_ = &symBindings;
    execFunction(*entry, args, symBindings);
    symBindings_ = nullptr;
  }

  void Interpreter::execFunction(
      const FunDecl &f, const std::vector<RuntimeValue> &args, const SymBindings &symBindings
  ) {
    // Reset per-function memory state
    memory_.reset();
    typeMap_.clear();

    // Build name→type map for addr provenance lookups.
    for (const auto &p: f.params)
      typeMap_[p.name.name] = p.type;
    for (const auto &s: f.syms)
      typeMap_[s.name.name] = s.type;
    for (const auto &l: f.lets)
      typeMap_[l.name.name] = l.type;

    Store store;
    DiagBag diags;

    for (size_t i = 0; i < f.params.size(); ++i) {
      RuntimeValue v = args[i];
      v.bits = TypeUtils::getScalarBitWidth(f.params[i].type).value_or(v.bits ? v.bits : 64);
      if (v.kind == RuntimeValue::Kind::Int)
        v.intVal = canonicalize(v.intVal, v.bits);
      // SPEC §6.4: f32 parameter values must be rounded to f32 precision at the
      // call boundary, just as init values are (see evalInit). Without this, a
      // literal like 16777217.0 keeps its f64 image while bits=32, so callee
      // arithmetic diverges from the lowered C/WASM.
      if (v.kind == RuntimeValue::Kind::Float && v.bits == 32)
        v.floatVal = static_cast<double>(static_cast<float>(v.floatVal));
      store[f.params[i].name.name] = v;
    }

    for (const auto &s: f.syms) {
      auto it = symBindings.find(s.name.name);
      if (it == symBindings.end()) {
        throw std::runtime_error(
            "Symbol " + s.name.name + " has no binding (provide --sym " + s.name.name + "=<value>)"
        );
      }
      store[s.name.name] = bindSymValue(s, it->second);
    }

    // Init locals
    for (const auto &l: f.lets) {
      if (l.init) {
        store[l.name.name] = evalInit(*l.init, l.type, store);
      } else {
        store[l.name.name] = makeUndef(l.type);
      }
    }

    runBlocks(f, store, /*outRet=*/nullptr);
  }

  RuntimeValue Interpreter::callFunction(const FunDecl &f, std::vector<RuntimeValue> args) {
    // [v0.2.2] §9.6.1 — interprocedural execution in the interpreter.
    // Memory state (heap, objects, addresses) is preserved across the
    // call: pointer arguments must remain valid in the callee. typeMap_
    // entries that share a name with a callee parameter or local are
    // saved here and restored on return.

    Store store;
    std::vector<std::pair<std::string, TypePtr>> savedTypes;
    auto pushType = [&](const std::string &name, const TypePtr &t) {
      auto it = typeMap_.find(name);
      if (it != typeMap_.end())
        savedTypes.emplace_back(name, it->second);
      else
        savedTypes.emplace_back(name, TypePtr{});
      typeMap_[name] = t;
    };
    auto restoreTypes = [&]() {
      for (auto it = savedTypes.rbegin(); it != savedTypes.rend(); ++it) {
        if (it->second)
          typeMap_[it->first] = it->second;
        else
          typeMap_.erase(it->first);
      }
    };
    // The address map resolves `addr %x` to a heap base for the *current*
    // frame's local %x. Without scoping, callee's `addr %x` aliases
    // the caller's %x (same name, possibly different type), and
    // callee's entries also leak into the caller after return —
    // corrupting any subsequent caller `addr %y` whose name happens
    // to match a callee local. Snapshot/restore the whole map at the
    // call boundary; pointer arguments don't go through the address map
    // (they carry a provId that resolves via the object table / heap), so this
    // doesn't break the cross-frame pointer-validity guarantee.
    auto savedAddrMap = memory_.addrMap();
    memory_.addrMap().clear();

    // Bind parameters.
    if (args.size() != f.params.size())
      throw std::runtime_error(
          "Interpreter: call to " + f.name.name + " expected " + std::to_string(f.params.size()) +
          " args, got " + std::to_string(args.size())
      );
    for (size_t i = 0; i < f.params.size(); ++i) {
      pushType(f.params[i].name.name, f.params[i].type);
      RuntimeValue v = args[i];
      v.bits = TypeUtils::getScalarBitWidth(f.params[i].type).value_or(v.bits ? v.bits : 64);
      if (v.kind == RuntimeValue::Kind::Int)
        v.intVal = canonicalize(v.intVal, v.bits);
      // SPEC §6.4: f32 parameter values must be rounded to f32 precision at the
      // call boundary, mirroring the init-value rounding in evalInit.
      if (v.kind == RuntimeValue::Kind::Float && v.bits == 32)
        v.floatVal = static_cast<double>(static_cast<float>(v.floatVal));
      store[f.params[i].name.name] = v;
    }

    // Bind syms from the shared bindings map.
    for (const auto &s: f.syms) {
      pushType(s.name.name, s.type);
      if (!symBindings_)
        throw std::runtime_error("Interpreter: no sym bindings available for nested call");
      auto it = symBindings_->find(s.name.name);
      if (it == symBindings_->end())
        throw std::runtime_error(
            "Symbol " + s.name.name + " has no binding (callee=" + f.name.name + ")"
        );
      store[s.name.name] = bindSymValue(s, it->second);
    }

    // Init lets.
    for (const auto &l: f.lets) {
      pushType(l.name.name, l.type);
      if (l.init)
        store[l.name.name] = evalInit(*l.init, l.type, store);
      else
        store[l.name.name] = makeUndef(l.type);
    }

    RuntimeValue ret;
    try {
      runBlocks(f, store, &ret);
    } catch (...) {
      restoreTypes();
      memory_.addrMap() = std::move(savedAddrMap);
      throw;
    }
    restoreTypes();
    memory_.addrMap() = std::move(savedAddrMap);
    return ret;
  }

  // [v0.2.2] §9.6.1 step 5: refresh caller-side Store entries from heap.
  // Walk every addr-promoted local (varName ∈ the address map) that has a Store
  // entry in `store`. Reconstruct its value by reading the heap at the base
  // address (and at the per-element offsets for arrays). Scalars, ptrs,
  // and arrays-of-scalars are handled; struct and array-of-struct refresh
  // is deferred (the existing per-store syncObj path handles them within
  // the same call frame, which is enough for most cases).
  void Interpreter::syncStoreFromHeap(Store &store) {
    for (const auto &[name, base]: memory_.addrMap()) {
      auto sit = store.find(name);
      if (sit == store.end())
        continue;
      RuntimeValue &slot = sit->second;
      if (slot.kind == RuntimeValue::Kind::Int || slot.kind == RuntimeValue::Kind::Float ||
          slot.kind == RuntimeValue::Kind::Ptr) {
        auto hit = memory_.heap().find(base);
        if (hit != memory_.heap().end()) {
          // Preserve declared bit-width metadata; only the value changes.
          RuntimeValue fresh = hit->second;
          if (slot.bits && !fresh.bits)
            fresh.bits = slot.bits;
          slot = fresh;
        }
        continue;
      }
      if (slot.kind == RuntimeValue::Kind::Array) {
        // Find a matching whole-array ObjectInfo (arrayIdx == -1,
        // fieldName == "") so we know elemSize / count.
        const ObjectInfo *root = nullptr;
        for (const auto &o: memory_.objects())
          if (o.varName == name && o.fieldName.empty() &&
              o.arrayIdx == static_cast<std::uint64_t>(-1) && o.base == base) {
            root = &o;
            break;
          }
        if (!root)
          continue;
        for (std::size_t i = 0; i < slot.arrayVal.size() && i < root->count; ++i) {
          auto hit = memory_.heap().find(base + i * root->elemSize);
          if (hit == memory_.heap().end())
            continue;
          RuntimeValue &cell = slot.arrayVal[i];
          if (cell.kind != RuntimeValue::Kind::Int && cell.kind != RuntimeValue::Kind::Float &&
              cell.kind != RuntimeValue::Kind::Ptr)
            continue;
          RuntimeValue fresh = hit->second;
          if (cell.bits && !fresh.bits)
            fresh.bits = cell.bits;
          cell = fresh;
        }
      }
    }
  }

  void Interpreter::runBlocks(const FunDecl &f, Store &store, RuntimeValue *outRet) {
    DiagBag diags;
    CFG cfg = CFG::build(f, diags);
    if (diags.hasErrors())
      throw std::runtime_error("CFG Build failed during interp");

    const std::uint64_t frameId = nextFrameId_++;
    std::size_t pc = cfg.entry;

    while (true) {
      // Block-step budget (see setMaxBlockSteps): bound a possibly non-
      // terminating run instead of hanging. Counts blocks across nested
      // calls (blockSteps_ is reset per run()).
      if (maxBlockSteps_ != 0 && ++blockSteps_ > maxBlockSteps_)
        throw StepLimitError("block-step budget exhausted");

      const Block &block = f.blocks[pc];
      if (dumpExec_) {
        out_ << block.label.name << ":\n";
      }

      // State-capture hook (see setStateHook): record the store the block
      // sees on entry, before any of its instructions run.
      if (stateHook_)
        stateHook_(f.name.name, frameId, block.label.name, -1, store, memory_);

      int instrIdx = 0;
      for (const auto &ins: block.instrs) {
        std::visit(
            [&](auto &&i) {
              using T = std::decay_t<decltype(i)>;
              if constexpr (std::is_same_v<T, AssignInstr>) {
                RuntimeValue rhs = evalExpr(i.rhs, store);
                if (dumpExec_) {
                  out_ << "  " << i.lhs.base.name;
                  for (const auto &acc: i.lhs.accesses) {
                    if (auto ai = std::get_if<AccessIndex>(&acc)) {
                      out_ << "[";
                      if (auto ilit = std::get_if<IntLit>(&ai->index)) {
                        out_ << ilit->value;
                      } else {
                        auto id = std::get<LocalOrSymId>(ai->index);
                        std::visit(
                            [&](auto &&id_val) {
                              auto it = store.find(id_val.name);
                              if (it != store.end())
                                out_ << it->second.intVal;
                              else
                                out_ << id_val.name;
                            },
                            id
                        );
                      }
                      out_ << "]";
                    } else if (auto af = std::get_if<AccessField>(&acc)) {
                      out_ << "." << af->field;
                    }
                  }
                  out_ << " = " << rvToString(rhs) << "\n";
                }
                setLValue(i.lhs, rhs, store);
              } else if constexpr (std::is_same_v<T, AssumeInstr>) {
                if (!evalCond(i.cond, store))
                  throw std::runtime_error("Assumption failed");
              } else if constexpr (std::is_same_v<T, RequireInstr>) {
                if (!evalCond(i.cond, store)) {
                  std::string msg = i.message.value_or("Requirement failed");
                  throw RequireViolationError(msg);
                }
              } else if constexpr (std::is_same_v<T, StoreInstr>) {
                RuntimeValue ptrVal = evalExpr(i.ptr, store);
                RuntimeValue val = evalExpr(i.val, store);
                if (ptrVal.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: Store through undef pointer");
                if (ptrVal.kind != RuntimeValue::Kind::Ptr)
                  throw std::runtime_error("Store requires a pointer operand");
                if (ptrVal.ptrVal == 0)
                  throw UndefinedBehaviorError("UB: Null pointer dereference in store");
                // Provenance-based bounds check.
                const ObjectInfo *obj = memory_.findObjectByProvId(ptrVal.ptrBase);
                if (!obj)
                  throw UndefinedBehaviorError("UB: Store to unknown address");
                if (ptrVal.ptrVal < obj->base || ptrVal.ptrVal >= obj->end)
                  throw UndefinedBehaviorError("UB: Store out of bounds");
                // [v0.2.1] Rule 15b: typed-access mismatch on store.
                // Derive the pointer's pointee type from the expression's first
                // atom — supports AddrAtom, RValueAtom, LoadAtom, PtrIndexAtom,
                // PtrFieldAtom, etc.
                const ObjectInfo *cellObj = memory_.findObject(ptrVal.ptrVal);
                if (cellObj && cellObj->type) {
                  TypePtr ptrPointeeType = nullptr;
                  std::visit(
                      [&](auto &&atom) {
                        using A = std::decay_t<decltype(atom)>;
                        if constexpr (std::is_same_v<A, RValueAtom>) {
                          if (auto t = getLValueType(atom.rval)) {
                            if (auto pt = std::get_if<PtrType>(&t->v))
                              ptrPointeeType = pt->pointee;
                          }
                        } else if constexpr (std::is_same_v<A, AddrAtom>) {
                          if (auto t = getLValueType(atom.lv)) {
                            ptrPointeeType = t;
                          }
                        }
                      },
                      i.ptr.first.v
                  );
                  if (ptrPointeeType) {
                    TypePtr cellType = typeLayout_.getCellTypeAtOffset(
                        cellObj->type, ptrVal.ptrVal - cellObj->base
                    );
                    if (!TypeUtils::areTypesEqual(ptrPointeeType, cellType))
                      throw UndefinedBehaviorError("UB: Typed-access mismatch (rule 15b) on store");
                  }
                }
                // SPEC §6.4: enforce destination precision. The cell-level
                // ObjectInfo's elemSize reflects the pointee type at this
                // exact address (4 for f32, 8 for f64).  Do NOT use the
                // outer provenance object's elemSize: when the provenance
                // is a whole struct, its elemSize is the minFieldSize
                // across all fields (line 497), which can be 4 even when
                // the cell being written is an f64 field — that would
                // silently narrow the store to f32 precision and corrupt
                // the bit pattern.  Falls back to the provenance object
                // when no cell-level ObjectInfo is registered.
                std::uint64_t storeElemSize = obj->elemSize;
                if (const ObjectInfo *precCell = memory_.findObject(ptrVal.ptrVal))
                  storeElemSize = precCell->elemSize;
                if (val.kind == RuntimeValue::Kind::Float && storeElemSize == 4) {
                  val.bits = 32;
                  val.floatVal = static_cast<double>(static_cast<float>(val.floatVal));
                }
                memory_.heap()[ptrVal.ptrVal] = val;
                // Sync back to store for Store/Heap consistency.
                // Strategy: find the most specific ObjectInfo that
                // contains the store address. If it's a field-level
                // ObjectInfo, sync to that field. If it's a whole-struct
                // ObjectInfo (from ptrfield provenance), find the field
                // ObjectInfo that contains the address and sync there.
                // Mirror the heap write back into the structured `store`
                // value so a later structured read (`%t.f[i]` in the exit
                // checksum, say) sees it. Walk from the root variable by
                // byte offset through arrays / structs / vectors down to the
                // scalar leaf. The previous two-level (fieldName + arrayIdx)
                // sync could only name `%v`, `%a[i]`, `%t.f`, and `%a[i].f`;
                // a store to a deeper cell such as `%t.f[i]` (a struct field
                // that is itself an array) fell through to overwriting the
                // whole array-typed field with the scalar, corrupting its
                // shape. This offset walk handles arbitrary nesting.
                const std::string &rootVar = obj->varName;
                auto baseIt = memory_.addrMap().find(rootVar);
                auto rootTyIt = typeMap_.find(rootVar);
                if (baseIt != memory_.addrMap().end() && rootTyIt != typeMap_.end() &&
                    store.count(rootVar)) {
                  std::function<bool(RuntimeValue &, const TypePtr &, std::uint64_t)> setLeaf =
                      [&](RuntimeValue &node, const TypePtr &ty, std::uint64_t off) -> bool {
                    if (!ty)
                      return false;
                    if (auto at = std::get_if<ArrayType>(&ty->v)) {
                      std::uint64_t es = typeLayout_.sizeofType(at->elem);
                      if (es == 0 || node.kind != RuntimeValue::Kind::Array)
                        return false;
                      std::uint64_t idx = off / es;
                      if (idx >= node.arrayVal.size())
                        return false;
                      return setLeaf(node.arrayVal[idx], at->elem, off % es);
                    }
                    if (auto vt = std::get_if<VecType>(&ty->v)) {
                      std::uint64_t es = typeLayout_.sizeofType(vt->elem);
                      if (es == 0 || node.kind != RuntimeValue::Kind::Vec)
                        return false;
                      std::uint64_t idx = off / es;
                      if (idx >= node.arrayVal.size())
                        return false;
                      return setLeaf(node.arrayVal[idx], vt->elem, off % es);
                    }
                    if (auto st = std::get_if<StructType>(&ty->v)) {
                      auto sit = typeLayout_.structs().find(st->name.name);
                      if (sit == typeLayout_.structs().end() ||
                          node.kind != RuntimeValue::Kind::Struct)
                        return false;
                      std::uint64_t fo = 0;
                      for (const auto &f: sit->second->fields) {
                        std::uint64_t fs = typeLayout_.sizeofType(f.type);
                        if (off >= fo && off < fo + fs)
                          return setLeaf(node.structVal[f.name], f.type, off - fo);
                        fo += fs;
                      }
                      return false;
                    }
                    node = val; // scalar / pointer leaf
                    return true;
                  };
                  setLeaf(store[rootVar], rootTyIt->second, ptrVal.ptrVal - baseIt->second);
                }
              }
            },
            ins
        );
        // pp-granularity capture: record the store after each instruction.
        if (stateHook_ && stateHookPerInstr_)
          stateHook_(f.name.name, frameId, block.label.name, instrIdx, store, memory_);
        ++instrIdx;
      }

      bool jumped = false;
      std::visit(
          [&](auto &&t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, BrTerm>) {
              if (t.isConditional) {
                if (evalCond(*t.cond, store))
                  pc = cfg.indexOf[t.thenLabel.name];
                else
                  pc = cfg.indexOf[t.elseLabel.name];
              } else {
                pc = cfg.indexOf[t.dest.name];
              }
              jumped = true;
            } else if constexpr (std::is_same_v<T, RetTerm>) {
              if (t.value) {
                RuntimeValue res = evalExpr(*t.value, store);
                if (res.kind == RuntimeValue::Kind::Undef)
                  throw UndefinedBehaviorError("UB: Reading undef in ret");
                if (outRet) {
                  *outRet = res;
                  return;
                }
                if (res.kind == RuntimeValue::Kind::Int)
                  out_ << "Result: " << res.intVal << "\n";
                else if (res.kind == RuntimeValue::Kind::Float) {
                  // Print floats as IEEE 754 hex (printf %a) so the output is
                  // bit-exact: round-trips losslessly, distinguishes +0/-0,
                  // and handles subnormals correctly. This is the format used
                  // for interp ⇄ compiled-C cross-validation in the xval
                  // tests; decimal would silently lose bits at the boundary.
                  char buf[64];
                  std::snprintf(buf, sizeof(buf), "%a", res.floatVal);
                  out_ << "Result: " << buf << "\n";
                } else if (res.kind == RuntimeValue::Kind::Ptr)
                  out_ << "Result: ptr(0x" << std::hex << res.ptrVal << std::dec << ")\n";
              } else {
                out_ << "Result: void\n";
              }
              return;
            } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
              throw std::runtime_error("Reached unreachable");
            }
          },
          block.term
      );

      if (!jumped)
        break;
    }
  }

} // namespace refractir
