#include "backend/c_backend.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include "analysis/type_utils.hpp"
#include "backend/vec_shapes.hpp"
#include "c_internal.hpp"

namespace refractir {

  void CBackend::indent() {
    for (int i = 0; i < indent_level_; ++i)
      out_ << "  ";
  }

  std::string CBackend::mangleName(const std::string &name) {
    if (name.empty())
      return name;
    if (noMainMangle_ && name == "@main") {
      return "main";
    }
    size_t start = 0;
    if (name[0] == '@' || name[0] == '%' || name[0] == '^') {
      start = 1;
      if (name.size() > 1 && name[1] == '?')
        start = 2;
    }
    // Prefix with refractir_ to avoid C keywords and collisions for internal identifiers
    return "refractir_" + name.substr(start);
  }

  std::string CBackend::stripSigil(const std::string &name) {
    if (name.empty())
      return name;
    size_t start = 0;
    if (name[0] == '@' || name[0] == '%' || name[0] == '^') {
      start = 1;
      if (name.size() > 1 && name[1] == '?')
        start = 2;
    }
    return name.substr(start);
  }

  std::string
  CBackend::getMangledSymbolName(const std::string &funcName, const std::string &symName) {
    // Follow docs/symirc.md format: <func>__<sym> with sigils removed
    return stripSigil(funcName) + "__" + stripSigil(symName);
  }

  // [v0.2.2] Encode a leaf type as a C-identifier suffix used by
  // `arrayPtrTypedefName`.  Walks the leaf only — the array shape is
  // encoded separately by the caller.  Pointer and vector leaves are
  // encoded recursively (`p_<inner>`, `v<N>_<inner>`) so that distinct
  // pointee / lane types do not collide on a shared catch-all name
  // (the historical `"x"` collapse silently merged e.g. `[N] ptr i16`
  // and `[N] ptr i32` into a single typedef, breaking rylink-merged
  // multi-function bundles).
  static std::string typedefLeafTag(const TypePtr &t) {
    if (!t)
      return "void";
    return std::visit(
        [](auto &&arg) -> std::string {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            uint32_t b = (arg.kind == IntType::Kind::I32)   ? 32
                         : (arg.kind == IntType::Kind::I64) ? 64
                                                            : (uint32_t) arg.bits.value_or(32);
            return "i" + std::to_string(b);
          } else if constexpr (std::is_same_v<T, FloatType>) {
            return (arg.kind == FloatType::Kind::F32) ? "f32" : "f64";
          } else if constexpr (std::is_same_v<T, StructType>) {
            return "S_" + arg.name.name.substr(arg.name.name.empty() ? 0 : 1);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            return "p_" + typedefLeafTag(arg.pointee);
          } else if constexpr (std::is_same_v<T, VecType>) {
            return "v" + std::to_string(arg.size) + "_" + typedefLeafTag(arg.elem);
          }
          return "x";
        },
        t->v
    );
  }

  // [v0.2.2] Generate a stable, C-identifier-safe typedef name for an
  // `ArrayType` used as a RefractIR pointer pointee.  The shape is encoded
  // outer-to-inner with `x` between dimensions, and the leaf is one of
  // `iN`, `fN`, `S_<name>`.  Examples:
  //   [3] i8         → _sym_arr_3_i8
  //   [3] [3] i8     → _sym_arr_3x3_i8
  //   [2] [2] [2] f64→ _sym_arr_2x2x2_f64
  //   [4] @S         → _sym_arr_4_S_S
  static std::string arrayPtrTypedefName(const TypePtr &arrTy) {
    std::string s = "_sym_arr_";
    TypePtr cur = arrTy;
    bool first = true;
    while (cur && std::holds_alternative<ArrayType>(cur->v)) {
      const auto &at = std::get<ArrayType>(cur->v);
      if (!first)
        s += "x";
      s += std::to_string(at.size);
      first = false;
      cur = at.elem;
    }
    s += "_" + typedefLeafTag(cur);
    return s;
  }

  void CBackend::emitType(const TypePtr &type) {
    if (!type) {
      out_ << "void";
      return;
    }
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, IntType>) {
            if (arg.kind == IntType::Kind::I32)
              out_ << "int32_t";
            else if (arg.kind == IntType::Kind::I64)
              out_ << "int64_t";
            else if (arg.kind == IntType::Kind::ICustom) {
              int b = arg.bits.value_or(32);
              if (b <= 8)
                out_ << "int8_t";
              else if (b <= 16)
                out_ << "int16_t";
              else if (b <= 32)
                out_ << "int32_t";
              else if (b <= 64)
                out_ << "int64_t";
              else
                out_ << "__int128";
            }
          } else if constexpr (std::is_same_v<T, FloatType>) {
            out_ << (arg.kind == FloatType::Kind::F32 ? "float" : "double");
          } else if constexpr (std::is_same_v<T, StructType>) {
            out_ << "struct " << mangleName(arg.name.name);
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            // Arrays are emitted as C arrays in context, but here we emit the base type
            emitType(arg.elem);
          } else if constexpr (std::is_same_v<T, PtrType>) {
            // [v0.2.2] When the pointee is an array (1D or nested), use
            // the typedef generated for that shape — `Elem (*)[N]…[M]`
            // is the proper C type and matches `&array` naturally,
            // unlike the flat `Elem *` collapse this branch used to do.
            // The typedef pre-emission lives in `emit()`'s preamble.
            if (arg.pointee && std::holds_alternative<ArrayType>(arg.pointee->v)) {
              out_ << arrayPtrTypedefName(arg.pointee) << " *";
            } else {
              emitType(arg.pointee);
              out_ << " *";
            }
          } else if constexpr (std::is_same_v<T, VecType>) {
            // [v0.2.1] Vector type — delegated to the lowering strategy.
            // CVecLowering produces a C type-string (a typedef name for
            // vecext, a struct name for structscalars / structarray, etc.).
            out_ << vecLowering_->typeString(arg);
          }
        },
        type->v
    );
  }

  // [v0.2.2] Walk a type collecting every ArrayType that appears as the
  // direct pointee of a PtrType.  Used to drive the array-pointer typedef
  // preamble — every `ptr [N] T` declaration needs a matching
  // `typedef T <name>[N]` so the assignment from `&array` type-checks.
  static void collectPtrArrayShapesInType(const TypePtr &t, std::vector<TypePtr> &out) {
    if (!t)
      return;
    std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, PtrType>) {
            if (arg.pointee && std::holds_alternative<ArrayType>(arg.pointee->v))
              out.push_back(arg.pointee);
            // Recurse: a `ptr ptr [N] T` nests, and we still want the
            // inner `[N] T` typedef collected.
            collectPtrArrayShapesInType(arg.pointee, out);
          } else if constexpr (std::is_same_v<T, ArrayType>) {
            collectPtrArrayShapesInType(arg.elem, out);
          } else if constexpr (std::is_same_v<T, VecType>) {
            collectPtrArrayShapesInType(arg.elem, out);
          }
        },
        t->v
    );
  }

  static std::vector<TypePtr> collectPtrArrayShapes(const Program &prog) {
    std::vector<TypePtr> raw;
    for (const auto &s: prog.structs)
      for (const auto &f: s.fields)
        collectPtrArrayShapesInType(f.type, raw);
    for (const auto &f: prog.funs) {
      collectPtrArrayShapesInType(f.retType, raw);
      for (const auto &p: f.params)
        collectPtrArrayShapesInType(p.type, raw);
      for (const auto &s: f.syms)
        collectPtrArrayShapesInType(s.type, raw);
      for (const auto &l: f.lets)
        collectPtrArrayShapesInType(l.type, raw);
    }
    for (const auto &d: prog.extDecls) {
      collectPtrArrayShapesInType(d.retType, raw);
      for (const auto &p: d.params)
        collectPtrArrayShapesInType(p.type, raw);
    }
    // De-dup by the generated typedef name (it's a stable key on the
    // outer-to-inner shape + leaf tag).
    std::unordered_map<std::string, TypePtr> uniq;
    std::vector<TypePtr> out;
    for (const auto &t: raw) {
      auto key = arrayPtrTypedefName(t);
      if (uniq.emplace(key, t).second)
        out.push_back(t);
    }
    return out;
  }

  // collectVecShapes moved to src/backend/vec_shapes.cpp ([v0.2.3]) —
  // it is target-neutral and the python backend's vec-lowering
  // preamble needs it too.

  // intrinsicHelperName and emitIntrinsicHelper are implemented in
  // src/backend/c_intrinsics.cpp — the single source of truth for
  // C-side intrinsic code generation. See that file to add new intrinsics.


  // [v0.2.2] §11.2 lowering for `decl`. Link-form decls become `extern`
  // prototypes (linker resolves them). Contract-form decls also emit an
  // `extern` plus a `// contract:` summary comment.
  void CBackend::emitExtDecl(const ExtDecl &d) {
    if (d.contract) {
      out_ << "// contract: " << d.name.name << "\n";
      for (const auto &pre: d.contract->pres)
        out_ << "//   pre  " << (pre.message ? *pre.message : "") << "\n";
      for (const auto &post: d.contract->posts)
        out_ << "//   post " << (post.message ? *post.message : "") << "\n";
    }
    out_ << "extern ";
    emitType(d.retType);
    out_ << " " << mangleName(d.name.name) << "(";
    if (d.params.empty()) {
      out_ << "void";
    } else {
      for (size_t i = 0; i < d.params.size(); ++i) {
        if (i)
          out_ << ", ";
        emitType(d.params[i].type);
        out_ << " " << mangleName(d.params[i].name.name);
      }
    }
    out_ << ");\n\n";
  }

  // [v0.2.2] Split emission: write common.h + one <stem>.c per
  // distinct source-file stem. Returns the list of written paths.
  std::vector<std::string> CBackend::emitSplit(
      const Program &prog, const std::string &outDir, const std::string &primaryStem
  ) {
    std::filesystem::create_directories(outDir);
    std::vector<std::string> written;

    // 1. common.h: the original preamble (includes, struct typedefs,
    //    intrinsic helpers, extern decls, forward decls for every fun),
    //    NO function bodies.
    std::string commonPath = (std::filesystem::path(outDir) / "common.h").string();
    {
      std::ofstream cofs(commonPath);
      if (!cofs)
        throw std::runtime_error("Cannot open " + commonPath);
      cofs << "#pragma once\n";
      // Write the preamble (struct typedefs, intrinsic helpers, extern
      // decls, fun forward decls) but NO fun bodies. The sentinel stem
      // matches no fun, so the body loop emits nothing.
      CBackend hdr(cofs);
      hdr.noRequire_ = noRequire_;
      hdr.noUbGuards_ = noUbGuards_;
      hdr.noMainMangle_ = noMainMangle_;
      hdr.vecLowering_ = makeCVecLowering(vecLowering_ ? vecLowering_->name() : "vecext");
      hdr.emitOnlySourceStem_ = "__refractir_no_fun_bodies__";
      hdr.emit(prog);
      written.push_back(commonPath);
    }

    // 2. Collect distinct stems that have at least one fun. The primary
    //    file always gets a `<primaryStem>.c` (even if empty of funs,
    //    consumers expect it).
    std::set<std::string> stems;
    stems.insert(primaryStem);
    for (const auto &f: prog.funs) {
      stems.insert(f.sourceStem.empty() ? primaryStem : f.sourceStem);
    }

    for (const auto &stem: stems) {
      std::string p = (std::filesystem::path(outDir) / (stem + ".c")).string();
      std::ofstream cofs(p);
      if (!cofs)
        throw std::runtime_error("Cannot open " + p);
      CBackend body(cofs);
      body.noRequire_ = noRequire_;
      body.noUbGuards_ = noUbGuards_;
      body.noMainMangle_ = noMainMangle_;
      body.structuredLowering_ = structuredLowering_;
      body.vecLowering_ = makeCVecLowering(vecLowering_ ? vecLowering_->name() : "vecext");
      body.suppressPreamble_ = true;
      // Map the primary stem to "" (empty sourceStem on FunDecl).
      body.emitOnlySourceStem_ = (stem == primaryStem) ? std::string{} : stem;
      // When emitting the primary stem's .c we still want funs whose
      // sourceStem is empty (the primary's own funs). Use a magic
      // marker the filter understands as "match empty stem".
      if (stem == primaryStem)
        body.emitOnlySourceStem_ = "__refractir_primary__";
      body.emit(prog);
      written.push_back(p);
    }
    return written;
  }

  void CBackend::emit(const Program &prog) {
    prog_ = &prog;
    // [v0.2.2] Populate structFields_ before any emission so getLValueType
    // / getCoefType work whether or not we end up writing the preamble.
    // (The original code populated it inline at line ~365 below — we
    // hoist it up so it runs even when `suppressPreamble_` is set, in
    // which case the per-stem `.c` files emit only fun bodies but
    // still need struct-field metadata to do so.)
    structFields_.clear();
    for (const auto &s: prog.structs) {
      std::vector<std::pair<std::string, TypePtr>> fields;
      fields.reserve(s.fields.size());
      for (const auto &f: s.fields)
        fields.emplace_back(f.name, f.type);
      structFields_[s.name.name] = std::move(fields);
    }

    if (suppressPreamble_) {
      // [v0.2.2] Per-stem `.c` file: skip the include / pragma / struct
      // / extern preamble (it lives in `common.h`, which the caller is
      // responsible for `#include`-ing at the top of the file).
      out_ << "#include \"common.h\"\n\n";
    } else {
      out_ << "#include <stdint.h>\n";
      out_ << "#include <stddef.h>\n";
      out_ << "#include <stdbool.h>\n";
      out_ << "#include <stdlib.h>\n";
      out_ << "#include <stdio.h>\n";
      out_ << "#include <float.h>\n";
      out_ << "#include <math.h>\n";
      out_ << "#include <string.h>\n";
      if (!noRequire_)
        out_ << "#include <assert.h>\n";
      out_ << "\n";
    }
    if (!suppressPreamble_) {

      // SPEC §2.9 conformance. C doesn't mandate IEEE 754 — the implementation
      // declares conformance by predefining __STDC_IEC_559__ (C99 §F.1). We
      // cannot define it ourselves; we MUST refuse to compile on a platform
      // that doesn't conform, because RefractIR's FP semantics (RNE rounding,
      // finite-only domain, deterministic NaN/inf handling) all rest on
      // IEC 60559 / IEEE 754. Also disable FP contraction — without this,
      // GCC may fuse `a*b + c` into a single-rounding `fma`, which violates
      // §2.9 "no contraction" and would diverge from the interpreter and WASM.
      out_ << "#if !defined(__STDC_IEC_559__) || __STDC_IEC_559__ != 1\n";
      out_ << "# error \"RefractIR-lowered C requires an IEC 60559 / IEEE 754 conforming \"\\\n";
      out_ << "          \"implementation (compiler must predefine __STDC_IEC_559__ to 1)\"\n";
      out_ << "#endif\n";
      out_ << "#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0\n";
      out_ << "# error \"RefractIR-lowered C requires an implementation with FLT_EVAL_METHOD == 0, "
              "\"\\\n";
      out_
          << "          \"i.e., do not promote float into double or long double for evaluation\"\n";
      out_ << "#endif\n";
      out_ << "#pragma STDC FP_CONTRACT OFF\n";
      out_ << "\n";

      // [v0.2.1] Vector-lowering strategy. Default to vecext.
      if (!vecLowering_) {
        vecLowering_ = makeCVecLowering("vecext");
      }
      out_ << "// vec-lowering: " << vecLowering_->name() << "\n";
      auto vecShapes = collectVecShapes(prog);
      if (!vecShapes.empty()) {
        // Validate fn-boundary capability before emitting anything.
        if (!vecLowering_->canCrossFnBoundary()) {
          for (const auto &f: prog.funs) {
            if (f.retType && std::holds_alternative<VecType>(f.retType->v))
              throw std::runtime_error(
                  "vec-lowering '" + vecLowering_->name() +
                  "' cannot cross function boundaries: function '" + f.name.name +
                  "' returns a vector"
              );
            for (const auto &p: f.params)
              if (p.type && std::holds_alternative<VecType>(p.type->v))
                throw std::runtime_error(
                    "vec-lowering '" + vecLowering_->name() +
                    "' cannot cross function boundaries: function '" + f.name.name +
                    "' has a vector parameter"
                );
          }
        }
        vecLowering_->emitPreamble(out_, vecShapes);
      }

      // 1. Forward decls for structs.
      for (const auto &s: prog.structs) {
        out_ << "struct " << mangleName(s.name.name) << ";\n";
      }
      out_ << "\n";

      // [v0.2.2] 1a. Pointer-to-array typedefs.  Emit scalar / float
      // leaves first so they're available inside struct field
      // declarations.  Struct-leaf typedefs are emitted in §1c below
      // after the struct definitions are complete (a typedef of
      // `struct S _n[N]` needs `sizeof(struct S)`).
      auto arrayShapes = collectPtrArrayShapes(prog);
      auto leafIsStruct = [](const TypePtr &arrTy) {
        TypePtr cur = arrTy;
        while (cur && std::holds_alternative<ArrayType>(cur->v))
          cur = std::get<ArrayType>(cur->v).elem;
        return cur && std::holds_alternative<StructType>(cur->v);
      };
      auto emitArrayTypedef = [&](const TypePtr &arrTy) {
        TypePtr cur = arrTy;
        std::vector<uint64_t> dims;
        while (cur && std::holds_alternative<ArrayType>(cur->v)) {
          dims.push_back(std::get<ArrayType>(cur->v).size);
          cur = std::get<ArrayType>(cur->v).elem;
        }
        out_ << "typedef ";
        emitType(cur);
        out_ << " " << arrayPtrTypedefName(arrTy);
        for (auto d: dims)
          out_ << "[" << d << "]";
        out_ << ";\n";
      };
      bool emittedScalarTypedef = false;
      for (const auto &arrTy: arrayShapes) {
        if (leafIsStruct(arrTy))
          continue;
        emitArrayTypedef(arrTy);
        emittedScalarTypedef = true;
      }
      if (emittedScalarTypedef)
        out_ << "\n";

      // 2. Struct definitions.  Hoisted ahead of intrinsic helpers and
      // function forward decls so the pointer-to-struct-array typedefs
      // (§1c) and any cross-reference that needs the full layout can
      // resolve correctly.
      for (const auto &s: prog.structs) {
        out_ << "struct " << mangleName(s.name.name) << " {\n";
        indent_level_++;
        for (const auto &f: s.fields) {
          indent();
          // Handle array fields
          TypePtr cur = f.type;
          std::vector<uint64_t> dims;
          while (auto at = std::get_if<ArrayType>(&cur->v)) {
            dims.push_back(at->size);
            cur = at->elem;
          }
          emitType(cur);
          out_ << " " << f.name;
          for (auto d: dims)
            out_ << "[" << d << "]";
          out_ << ";\n";
        }
        indent_level_--;
        out_ << "};\n\n";
      }

      // [v0.2.2] 1c. Pointer-to-array typedefs whose leaf is a struct.
      // Emit after struct definitions so `sizeof(struct S)` is known.
      bool emittedStructTypedef = false;
      for (const auto &arrTy: arrayShapes) {
        if (!leafIsStruct(arrTy))
          continue;
        emitArrayTypedef(arrTy);
        emittedStructTypedef = true;
      }
      if (emittedStructTypedef)
        out_ << "\n";

      // 1b. [v0.2.2] Emit intrinsic helpers and extern decls for link-form
      //     `decl`s. Contract-form `decl`s lower with extern + a structured
      //     comment summarizing the contract (§11.2).
      for (const auto &intr: prog.intrinsics) {
        emitIntrinsicHelper(intr);
      }
      for (const auto &d: prog.extDecls) {
        emitExtDecl(d);
      }
      // Forward decls for every `fun` so cross-references work regardless
      // of source order.
      for (const auto &f: prog.funs) {
        emitType(f.retType);
        out_ << " " << mangleName(f.name.name) << "(";
        if (f.params.empty()) {
          out_ << "void";
        } else {
          for (size_t i = 0; i < f.params.size(); ++i) {
            if (i)
              out_ << ", ";
            TypePtr cur = f.params[i].type;
            std::vector<uint64_t> dims;
            while (auto at = std::get_if<ArrayType>(&cur->v)) {
              dims.push_back(at->size);
              cur = at->elem;
            }
            emitType(cur);
            out_ << " " << mangleName(f.params[i].name.name);
            for (auto d: dims)
              out_ << "[" << d << "]";
          }
        }
        out_ << ");\n";
      }
      if (!prog.funs.empty())
        out_ << "\n";
    } // !suppressPreamble_

    auto getWidth = [](const TypePtr &t) -> std::uint32_t {
      if (auto it = std::get_if<IntType>(&t->v)) {
        switch (it->kind) {
          case IntType::Kind::I32:
            return 32;
          case IntType::Kind::I64:
            return 64;
          case IntType::Kind::ICustom:
            return it->bits.value_or(32);
        }
      }
      return 64;
    };

    // 3. Functions
    for (const auto &f: prog.funs) {
      // [v0.2.2] In --split-by-source mode emit only funs whose
      // sourceStem matches this output file's stem. Two sentinel
      // values: "__refractir_no_fun_bodies__" skips every fun (used when
      // writing common.h); "__refractir_primary__" matches funs with an
      // empty sourceStem (i.e. those that came from the primary file).
      if (emitOnlySourceStem_ == "__refractir_no_fun_bodies__")
        continue;
      if (emitOnlySourceStem_ == "__refractir_primary__") {
        if (!f.sourceStem.empty())
          continue;
      } else if (!emitOnlySourceStem_.empty() && f.sourceStem != emitOnlySourceStem_) {
        continue;
      }
      curFuncName_ = f.name.name;
      curFuncRetType_ = f.retType;
      varWidths_.clear();
      varTypes_.clear();
      auto recordVar = [&](const std::string &name, const TypePtr &t) {
        if (!t)
          return;
        varWidths_[name] = getWidth(t);
        varTypes_[name] = t;
      };
      for (const auto &p: f.params)
        recordVar(p.name.name, p.type);
      for (const auto &s: f.syms)
        recordVar(s.name.name, s.type);
      for (const auto &l: f.lets)
        recordVar(l.name.name, l.type);

      // 3a. Extern symbols
      for (const auto &s: f.syms) {
        out_ << "extern ";
        emitType(s.type);
        out_ << " " << getMangledSymbolName(f.name.name, s.name.name) << "(void);\n";
      }
      if (!f.syms.empty())
        out_ << "\n";

      // 3b. Function signature
      // [v0.2.3] FunDecl::Attributes hints. The backend reads only the
      // bits it knows how to express in C; the rest are silently dropped.
      // Skipped for `@main` because the C entry point is not a callee.
      // Each enabled bit becomes one attribute token. With both set we
      // emit a single `__attribute__((noinline, noclone))` to keep the
      // line compact; either alone uses its own attribute group.
      if (f.name.name != "@main" && (f.attributes.noInline || f.attributes.noClone)) {
        out_ << "__attribute__((";
        if (f.attributes.noInline && f.attributes.noClone)
          out_ << "noinline, noclone";
        else if (f.attributes.noInline)
          out_ << "noinline";
        else
          out_ << "noclone";
        out_ << ")) ";
      }
      emitType(f.retType);
      out_ << " " << mangleName(f.name.name) << "(";
      if (f.params.empty()) {
        out_ << "void";
      } else {
        for (size_t i = 0; i < f.params.size(); ++i) {
          const auto &p = f.params[i];
          TypePtr cur = p.type;
          std::vector<uint64_t> dims;
          while (auto at = std::get_if<ArrayType>(&cur->v)) {
            dims.push_back(at->size);
            cur = at->elem;
          }
          emitType(cur);
          out_ << " " << mangleName(p.name.name);
          for (auto d: dims)
            out_ << "[" << d << "]";
          if (i + 1 < f.params.size())
            out_ << ", ";
        }
      }
      out_ << ") {\n";
      indent_level_++;

      // 3c. Locals and their initializations
      for (const auto &l: f.lets) {
        CtxGuard ctx(isDoubleCtx_, isOrContainsF64(l.type));

        // [v0.2.1] Vector locals route through the strategy: emit the
        // declaration (which can be `T v[N]`, `T v_0, v_1, …` for scalars,
        // or `struct ... v`), then emit per-lane initializers as separate
        // statements so every strategy converges on the same code shape.
        if (std::holds_alternative<VecType>(l.type->v)) {
          auto &vt = std::get<VecType>(l.type->v);
          std::string vName = mangleName(l.name.name);
          indent();
          vecLowering_->emitLocalDecl(out_, vName, vt);
          out_ << ";\n";
          if (l.init) {
            if (l.init->kind == InitVal::Kind::Aggregate) {
              const auto &elems = std::get<std::vector<InitValPtr>>(l.init->value);
              for (std::uint64_t k = 0; k < vt.size && k < elems.size(); ++k) {
                indent();
                std::string lane = vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                out_ << lane << " = ";
                emitInitVal(*elems[k], vt.elem);
                out_ << ";\n";
              }
            } else if (l.init->kind == InitVal::Kind::Undef) {
              // undef: no init. Reading is UB by spec (caught by definite-init).
            } else {
              TypePtr initType = getInitValType(*l.init);
              if (initType && std::holds_alternative<VecType>(initType->v)) {
                if (l.init->kind == InitVal::Kind::Local || l.init->kind == InitVal::Kind::Sym) {
                  if (l.init->kind == InitVal::Kind::Local) {
                    // Local → plain variable name; emitWholeCopy handles all strategies.
                    std::string srcName = mangleName(std::get<LocalId>(l.init->value).name);
                    indent();
                    vecLowering_->emitWholeCopy(out_, vName, srcName, vt);
                    out_ << ";\n";
                  } else {
                    // [v0.2.1] Sym → extern function `funcName__symName(void)`.
                    // Every sym reference becomes a call (per symirc.md §"Symbol
                    // references become calls"). curFuncName_ tracks the enclosing
                    // function currently being emitted.
                    const SymId &sym = std::get<SymId>(l.init->value);
                    const std::string callExpr =
                        getMangledSymbolName(curFuncName_, sym.name) + "()";
                    if (!vecLowering_->needsLaneUnroll()) {
                      // vecext / structarray / structscalars: extern returns the full
                      // vector type, so one call + one whole-vector assignment.
                      indent();
                      vecLowering_->emitWholeCopy(out_, vName, callExpr, vt);
                      out_ << ";\n";
                    } else {
                      // array / scalars: cannot return a vector type across a C function
                      // boundary.  The test harness always generates a vecext-ABI stub
                      // (typedef T[N] __attribute__((vector_size(...)))), so we capture
                      // the call into a vecext-typed temp and copy per-lane into the
                      // strategy's representation.  The vecext typedef for this shape
                      // was already emitted in the file preamble (collectVecShapes).
                      const std::string tmpName = "_sym_tmp_" + stripSigil(sym.name);
                      const std::string vecextTy = vecLowering_->typeString(vt);
                      // typeString() returns "" for scalars/array — fall back to the
                      // canonical _vec_N_elem name (matches the vecext preamble typedef).
                      const std::string tmpTy =
                          vecextTy.empty()
                              ? ("_vec_" + std::to_string(vt.size) + "_" + [&]() -> std::string {
                                  if (auto *it = std::get_if<IntType>(&vt.elem->v)) {
                                    int b =
                                        it->bits.value_or(it->kind == IntType::Kind::I32 ? 32 : 64);
                                    return "i" + std::to_string(b);
                                  }
                                  if (auto *ft = std::get_if<FloatType>(&vt.elem->v))
                                    return ft->kind == FloatType::Kind::F32 ? "f32" : "f64";
                                  return "i32";
                                }())
                              : vecextTy;
                      indent();
                      out_ << tmpTy << " " << tmpName << " = " << callExpr << ";\n";
                      for (std::uint64_t k = 0; k < vt.size; ++k) {
                        indent();
                        std::string dstLn =
                            vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                        out_ << dstLn << " = " << tmpName << "[" << k << "];\n";
                      }
                    }
                  }
                } else if (l.init->kind == InitVal::Kind::Atom) {
                  if (vecLowering_->needsLaneUnroll()) {
                    for (std::uint64_t k = 0; k < vt.size; ++k) {
                      indent();
                      std::string dstLane =
                          vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                      out_ << dstLane << " = "
                           << emitVecAtomLane(*std::get<AtomPtr>(l.init->value), vt, k) << ";\n";
                    }
                  } else {
                    indent();
                    out_ << vName << " = ";
                    emitInitVal(*l.init, l.type);
                    out_ << ";\n";
                  }
                } // if (initType && holds VecType)
              } else {
                // Broadcast scalar.
                for (std::uint64_t k = 0; k < vt.size; ++k) {
                  indent();
                  std::string lane = vecLowering_->emitLaneRead(vName, vt, std::to_string(k));
                  out_ << lane << " = ";
                  emitInitVal(*l.init, vt.elem);
                  out_ << ";\n";
                }
              }
            }
          }
          continue;
        }

        indent();
        TypePtr cur = l.type;
        std::vector<uint64_t> dims;
        while (auto at = std::get_if<ArrayType>(&cur->v)) {
          dims.push_back(at->size);
          cur = at->elem;
        }
        emitType(cur);
        out_ << " " << mangleName(l.name.name);
        for (auto d: dims)
          out_ << "[" << d << "]";

        if (l.init && l.init->kind == InitVal::Kind::Aggregate) {
          out_ << " = ";
          emitInitVal(*l.init, l.type);
          out_ << ";\n";
        } else if (l.init) {
          TypePtr initType = getInitValType(*l.init);
          bool isWholeCopy = initType && TypeUtils::areTypesEqual(initType, l.type);
          if (isWholeCopy) {
            if (!dims.empty()) {
              out_ << " = {0};\n";
              indent();
              out_ << "memcpy(&" << mangleName(l.name.name) << ", &";
              emitInitVal(*l.init, l.type);
              out_ << ", sizeof(" << mangleName(l.name.name) << "));\n";
            } else {
              out_ << " = ";
              emitInitVal(*l.init, l.type);
              out_ << ";\n";
            }
          } else if (!dims.empty() || std::holds_alternative<StructType>(l.type->v)) {
            // Aggregate broadcast
            out_ << " = {0};\n";
            // Check if we need a loop for non-zero init
            bool isZero = false;
            if (l.init->kind == InitVal::Kind::Int && std::get<IntLit>(l.init->value).value == 0)
              isZero = true;

            if (!isZero) {
              if (!dims.empty()) {
                std::function<void(size_t, std::string)> genLoops = [&](size_t dim,
                                                                        std::string access) {
                  if (dim == dims.size()) {
                    indent();
                    out_ << mangleName(l.name.name) << access << " = ";
                    emitInitVal(*l.init, cur);
                    out_ << ";\n";
                    return;
                  }
                  indent();
                  out_ << "for (int i" << dim << " = 0; i" << dim << " < " << dims[dim] << "; ++i"
                       << dim << ") {\n";
                  indent_level_++;
                  genLoops(dim + 1, access + "[i" + std::to_string(dim) + "]");
                  indent_level_--;
                  indent();
                  out_ << "}\n";
                };
                genLoops(0, "");
              } else {
                indent();
                out_ << "/* Warning: non-zero broadcast init for struct not fully supported */\n";
              }
            }
          } else {
            // Scalar broadcast
            out_ << " = ";
            emitInitVal(*l.init, l.type);
            out_ << ";\n";
          }
        } else {
          out_ << ";\n";
        }
      }

      // 3d. Blocks — while/if reconstructed from the control tree
      // under --structured-lowering, labels+goto otherwise.
      if (structuredLowering_)
        emitStructuredBody(f);
      else
        for (const auto &b: f.blocks) {
          out_ << mangleName(b.label.name) << ": ;\n"; // semicolon for empty label case

          for (const auto &ins: b.instrs)
            emitInstr(ins);

          indent();
          std::visit(
              [this](auto &&arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, BrTerm>) {
                  if (arg.isConditional) {
                    out_ << "if (";
                    emitCond(*arg.cond);
                    out_ << ") goto " << mangleName(arg.thenLabel.name) << ";\n";
                    indent();
                    out_ << "else goto " << mangleName(arg.elseLabel.name) << ";\n";
                  } else {
                    out_ << "goto " << mangleName(arg.dest.name) << ";\n";
                  }
                } else if constexpr (std::is_same_v<T, RetTerm>) {
                  emitRetTerm(arg);
                } else if constexpr (std::is_same_v<T, UnreachableTerm>) {
                  out_ << "// unreachable\n";
                }
              },
              b.term
          );
        }

      indent_level_--;
      out_ << "}\n\n";
    }
  }

  // Statement-level emission of one instruction, indentation included.
  // Shared by the goto block loop in emit() and the structured
  // control-tree emitter (src/backend/c_structured.cpp).
  void CBackend::emitInstr(const Instr &ins) {
    indent();
    std::visit(
        [this](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, AssignInstr>) {
            CtxGuard ctx(isDoubleCtx_, isOrContainsF64(getLValueType(arg.lhs)));
            // [v0.2.1] Vector LHS goes through strategy-aware paths.
            auto lhsTy = getLValueType(arg.lhs);
            if (lhsTy && std::holds_alternative<VecType>(lhsTy->v) && arg.lhs.accesses.empty()) {
              auto &vt = std::get<VecType>(lhsTy->v);
              // CmpAtom and mask-form SelectAtom are always lane-unroll.
              if (arg.rhs.rest.empty()) {
                if (auto cmpA = std::get_if<CmpAtom>(&arg.rhs.first.v)) {
                  emitVecCmpAssign(arg.lhs, *cmpA, vt);
                  return;
                }
                if (auto sel = std::get_if<SelectAtom>(&arg.rhs.first.v)) {
                  if (sel->maskExpr) {
                    emitVecMaskSelectAssign(arg.lhs, *sel, vt);
                    return;
                  }
                }
              }
              // [v0.2.1] LShr can't be expressed inline on a vec-ext
              // type — the signed-to-unsigned reinterpret is illegal on
              // GCC vector types. Float `%` likewise has no inline form
              // (GCC vector types don't define `%` for floats, and the
              // semantics require a per-lane fmod call with the §2.9
              // intermediate-overflow UB check). Force lane-unroll for
              // either op regardless of strategy.
              bool forceLaneUnroll = false;
              if (arg.rhs.rest.empty()) {
                if (auto op = std::get_if<OpAtom>(&arg.rhs.first.v)) {
                  if (op->op == AtomOpKind::LShr)
                    forceLaneUnroll = true;
                  else if (op->op == AtomOpKind::Mod && vt.elem &&
                           std::holds_alternative<FloatType>(vt.elem->v))
                    forceLaneUnroll = true;
                }
              }
              if (vecLowering_->needsLaneUnroll() || forceLaneUnroll) {
                emitVecAssign(arg.lhs, arg.rhs, vt);
                return;
              }
              // vecext: fall through to the inline expression path.
            }
            // [v0.2.1] Scalar `cmp` assignment: emit `(l op r) ? 1 : 0`.
            // CmpAtom can't lower as an inline expression (see emitAtom),
            // so we special-case it at AssignInstr level for scalars too.
            if (arg.rhs.rest.empty()) {
              if (auto cmpA = std::get_if<CmpAtom>(&arg.rhs.first.v)) {
                emitLValue(arg.lhs);
                out_ << " = ((";
                emitSelectVal(cmpA->lhs);
                const char *op = "==";
                switch (cmpA->op) {
                  case RelOp::EQ:
                    op = "==";
                    break;
                  case RelOp::NE:
                    op = "!=";
                    break;
                  case RelOp::LT:
                    op = "<";
                    break;
                  case RelOp::LE:
                    op = "<=";
                    break;
                  case RelOp::GT:
                    op = ">";
                    break;
                  case RelOp::GE:
                    op = ">=";
                    break;
                }
                out_ << ") " << op << " (";
                emitSelectVal(cmpA->rhs);
                out_ << ")) ? 1 : 0;\n";
                return;
              }
            }
            if (lhsTy && std::holds_alternative<ArrayType>(lhsTy->v)) {
              out_ << "memcpy(&(";
              emitLValue(arg.lhs);
              out_ << "), &(";
              emitExpr(arg.rhs);
              out_ << "), sizeof(";
              emitLValue(arg.lhs);
              out_ << "));\n";
            } else {
              emitLValue(arg.lhs);
              out_ << " = ";
              emitExpr(arg.rhs);
              out_ << ";\n";
            }
            // [v0.2.1] FP vector lanes: per-lane finite check (rule 21
            // lifted to FP rules 6/7 — any lane producing ±∞ or NaN
            // is UB). The native vec-ext division won't trap on its
            // own and UBSan doesn't catch SIMD div-by-zero, so we
            // emit an explicit check.
            if (!noUbGuards_ && lhsTy && std::holds_alternative<VecType>(lhsTy->v) &&
                arg.lhs.accesses.empty()) {
              auto &vtFp = std::get<VecType>(lhsTy->v);
              if (vtFp.elem && std::holds_alternative<FloatType>(vtFp.elem->v)) {
                std::string base = mangleName(arg.lhs.base.name);
                for (std::uint64_t k = 0; k < vtFp.size; ++k) {
                  indent();
                  std::string lane = vecLowering_->emitLaneRead(base, vtFp, std::to_string(k));
                  out_ << "if (!__builtin_isfinite(" << lane << ")) __builtin_trap();\n";
                }
              }
            }
            // [v0.2.1] Scalar FP UB: any ±∞ or NaN result is UB
            // (rules 6/7). UBSan catches some FP issues but not NaN
            // from 0.0/0.0; emit an explicit `isfinite` check after
            // FP assignments so the spec's semantics are enforced.
            // Also fires for FP element writes (array element / struct
            // field / vector lane) — the check uses the LHS in place.
            bool lhsIsFp = lhsTy && std::holds_alternative<FloatType>(lhsTy->v);
            if (!noUbGuards_ && lhsIsFp) {
              indent();
              out_ << "if (!__builtin_isfinite(";
              emitLValue(arg.lhs);
              out_ << ")) __builtin_trap();\n";
            }
          } else if constexpr (std::is_same_v<T, AssumeInstr>) {
            out_ << "// assume ";
            emitCond(arg.cond);
            out_ << "\n";
          } else if constexpr (std::is_same_v<T, RequireInstr>) {
            if (!noRequire_) {
              out_ << "assert(";
              emitCond(arg.cond);
              if (arg.message)
                out_ << " && \"" << *arg.message << "\"";
              out_ << ");\n";
            }
          } else if constexpr (std::is_same_v<T, StoreInstr>) {
            TypePtr pointeeTy = nullptr;
            if (auto ptrTy = getExprType(arg.ptr)) {
              if (auto pt = std::get_if<PtrType>(&ptrTy->v))
                pointeeTy = pt->pointee;
            }
            CtxGuard ctx(isDoubleCtx_, isOrContainsF64(pointeeTy));
            out_ << "*";
            emitExpr(arg.ptr);
            out_ << " = ";
            emitExpr(arg.val);
            out_ << ";\n";
          }
        },
        ins
    );
  }

  // `ret` terminator emission. The caller indents; shared by the goto
  // block loop and the structured emitter.
  void CBackend::emitRetTerm(const RetTerm &ret) {
    out_ << "return";
    if (ret.value) {
      CtxGuard ctx(isDoubleCtx_, isOrContainsF64(curFuncRetType_));
      out_ << " ";
      emitExpr(*ret.value);
    }
    out_ << ";\n";
  }

} // namespace refractir
