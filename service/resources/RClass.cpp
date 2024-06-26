/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RClass.h"

#include <limits>
#include <string>

#include "CFGMutation.h"
#include "ConstantPropagationAnalysis.h"
#include "ControlFlow.h"
#include "DexAnnotation.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "GlobalConfig.h"
#include "InitDeps.h"
#include "LiveRange.h"
#include "LocalDce.h"
#include "RedexResources.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Timer.h"
#include "Trace.h"

namespace {
// Crude check for if the name matches the pattern of autogenerated R class.
bool is_resource_class_name(const std::string_view& cls_name) {
  return cls_name.find("/R$") != std::string::npos;
}

// Welcome to clowntown.
bool is_styleable(const DexClass* cls) {
  const auto c_name = cls->get_name()->str();
  const auto d_name = cls->get_deobfuscated_name_or_empty();
  return c_name.find("R$styleable") != std::string::npos ||
         d_name.find("R$styleable") != std::string::npos;
}

// See
// https://github.com/facebook/buck/blob/main/src/com/facebook/buck/android/MergeAndroidResourcesStep.java#L385
// https://github.com/facebook/buck/commit/ec583c559239256ba0478d4bfdfc8d2c21426c4b
bool is_customized_resource_class_name(
    const std::string_view& cls_name,
    const ResourceConfig& global_resources_config) {
  for (const auto& s : global_resources_config.customized_r_classes) {
    if (cls_name == s) {
      return true;
    }
  }
  return false;
}

bool is_customized_resource_class(
    const DexClass* cls, const ResourceConfig& global_resources_config) {
  const auto c_name = cls->get_name()->str();
  const auto d_name = cls->get_deobfuscated_name_or_empty();
  return is_customized_resource_class_name(c_name, global_resources_config) ||
         is_customized_resource_class_name(d_name, global_resources_config);
}

bool is_external_ref(const DexFieldRef* field_ref) {
  auto field_cls = type_class(field_ref->get_class());
  if (field_cls == nullptr) {
    return false;
  }
  return field_cls->is_external();
}
} // namespace

namespace resources {
bool is_non_customized_r_class(const DexClass* cls) {
  const auto c_name = cls->get_name()->str();
  const auto d_name = cls->get_deobfuscated_name_or_empty();
  return is_resource_class_name(c_name) || is_resource_class_name(d_name);
}

bool RClassReader::is_r_class(const DexClass* cls) const {
  return is_non_customized_r_class(cls) ||
         is_customized_resource_class(cls, m_global_resources_config);
}

bool RClassReader::is_r_class(const DexFieldRef* field_ref) const {
  auto field_cls = type_class(field_ref->get_class());
  if (field_cls == nullptr) {
    return false;
  }
  return is_r_class(field_cls);
}

namespace cp = constant_propagation;
using ArrayAnalyzer = InstructionAnalyzerCombiner<cp::ClinitFieldAnalyzer,
                                                  cp::LocalArrayAnalyzer,
                                                  cp::HeapEscapeAnalyzer,
                                                  cp::PrimitiveAnalyzer>;

FieldArrayValues RClassReader::analyze_clinit(
    DexClass* cls, const FieldArrayValues& known_field_values) const {
  FieldArrayValues values;
  auto clinit = cls->get_clinit();
  if (clinit == nullptr) {
    return values;
  }
  always_assert(clinit->get_code()->editable_cfg_built());
  auto& cfg = clinit->get_code()->cfg();
  cfg.calculate_exit_block();

  cp::intraprocedural::FixpointIterator intra_cp(
      cfg, ArrayAnalyzer(cls->get_type(), nullptr, nullptr, nullptr));
  intra_cp.run(ConstantEnvironment());

  Lazy<live_range::UseDefChains> udchain(
      [&]() { return live_range::Chains(cfg).get_use_def_chains(); });

  std::unordered_set<DexField*> locally_built_fields;
  for (auto* block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_SPUT_OBJECT &&
          insn->get_field()->get_class() == clinit->get_class()) {
        // NOTE: this entire job may be best performed as interprocedural.
        // Some day.
        auto field_type = insn->get_field()->get_type();
        always_assert(type::is_array(field_type));
        const DexType* element_type =
            type::get_array_component_type(field_type);
        always_assert_log(type::is_int(element_type),
                          "R clinit array are expected to be [I. Got %s",
                          SHOW(field_type));

        auto array_domain =
            env.get_pointee<ConstantValueArrayDomain>(insn->src(0));
        if (!array_domain.is_value()) {
          // assert that this is coming from a different array that is already
          // known; if so then this "reuse" does not need to be tracked
          // specially.
          const auto& defs = udchain->at((live_range::Use){insn, 0});
          always_assert_log(defs.size() == 1,
                            "Expecting single def flowing into field %s in %s ",
                            SHOW(insn->get_field()), SHOW(cfg));
          IRInstruction* def = *defs.begin();
          if (opcode::is_move_result_pseudo_object(def->opcode())) {
            auto it =
                cfg.primary_instruction_of_move_result(cfg.find_insn(def));
            def = it->insn;
          }
          always_assert_log(def->opcode() == OPCODE_SGET_OBJECT,
                            "Unsupported array definition at %s in %s",
                            SHOW(def), SHOW(cfg));
          auto source_field = def->get_field();
          // No need to rewrite values for external field refs, or field ref
          // of another R class (which would be eligible for rewriting).
          always_assert_log(known_field_values.count(source_field) > 0 ||
                                is_external_ref(source_field),
                            "Field %s was not analyzed", SHOW(source_field));
        } else {
          locally_built_fields.emplace(insn->get_field()->as_def());
        }
      }
      intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
    }
  }

  auto env = intra_cp.get_exit_state_at(cfg.exit_block());
  for (auto f : locally_built_fields) {
    auto field_value = env.get(f);
    auto heap_ptr = field_value.maybe_get<AbstractHeapPointer>();
    always_assert_log(heap_ptr && heap_ptr->is_value(),
                      "Could not determine field value %s", SHOW(f));
    auto array_domain = env.get_pointee<ConstantValueArrayDomain>(*heap_ptr);
    always_assert(array_domain.is_value());
    std::vector<uint32_t> array_content;
    auto len = array_domain.length();
    for (size_t i = 0; i < len; ++i) {
      auto value = array_domain.get(i).maybe_get<SignedConstantDomain>();
      always_assert_log(value,
                        "%s is not in the SignedConstantDomain, "
                        "stored at %zu in %s:\n%s",
                        SHOW(array_domain.get(i)), i, SHOW(clinit), SHOW(cfg));
      auto cst = value->get_constant();
      always_assert_log(cst, "%s is not a constant", SHOW(*value));
      array_content.emplace_back(static_cast<uint32_t>(*cst));
    }
    values.emplace(f, std::move(array_content));
  }
  return values;
}

void RClassReader::ordered_r_class_iteration(
    const Scope& scope, const std::function<void(DexClass*)>& callback) const {
  Scope apply_scope;
  for (auto cls : scope) {
    if (is_r_class(cls)) {
      apply_scope.emplace_back(cls);
    }
  }
  size_t clinit_cycles = 0;
  Scope ordered_scope =
      init_deps::reverse_tsort_by_clinit_deps(apply_scope, clinit_cycles);
  always_assert_log(clinit_cycles == 0, "Found %zu clinit cycles",
                    clinit_cycles);

  for (auto cls : ordered_scope) {
    callback(cls);
  }
}

void RClassReader::extract_resource_ids_from_static_arrays(
    const Scope& scope,
    const std::unordered_set<DexField*>& array_fields,
    std::unordered_set<uint32_t>* out_values) const {
  Timer t("extract_resource_ids_from_static_arrays");
  FieldArrayValues field_values;
  ordered_r_class_iteration(scope, [&](DexClass* cls) {
    auto class_state = analyze_clinit(cls, field_values);
    field_values.insert(class_state.begin(), class_state.end());
  });
  for (auto&& [f, vec] : field_values) {
    auto field_def = f->as_def();
    if (field_def != nullptr && array_fields.count(field_def) > 0) {
      out_values->insert(vec.begin(), vec.end());
    }
  }
}

void RClassWriter::remap_resource_class_scalars(
    DexStoresVector& stores,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) const {
  auto scope = build_class_scope(stores);
  RClassReader r_class_reader(m_global_resources_config);
  for (auto clazz : scope) {
    if (r_class_reader.is_r_class(clazz)) {
      const std::vector<DexField*>& fields = clazz->get_sfields();
      for (auto& field : fields) {
        if (!type::is_int(field->get_type())) {
          continue;
        }
        auto encoded_val = field->get_static_value()->value();
        always_assert(encoded_val <= std::numeric_limits<int32_t>::max());
        auto encoded_int = (uint32_t)encoded_val;
        if (encoded_int > PACKAGE_RESID_START &&
            old_to_remapped_ids.count(encoded_int)) {
          field->get_static_value()->value(old_to_remapped_ids.at(encoded_int));
        }
      }
    }
  }
}

namespace {
// Writes a remapped vector of new values to output, returning whether or not it
// is actually different.
bool remap_array(const std::vector<uint32_t>& original_values,
                 const std::map<uint32_t, uint32_t>& old_to_remapped_ids,
                 const bool zero_out_values,
                 std::vector<uint32_t>* new_values) {
  bool changed{false};
  for (auto payload : original_values) {
    if (payload > PACKAGE_RESID_START) {
      bool keep = old_to_remapped_ids.count(payload);
      if (keep) {
        auto remapped = old_to_remapped_ids.at(payload);
        new_values->emplace_back(remapped);
        changed = changed || remapped != payload;
      } else {
        changed = true;
        // For styleable, we avoid actually deleting entries since
        // there are offsets that will point to the wrong positions
        // in the array. Instead, we zero out the values.
        if (zero_out_values) {
          new_values->emplace_back(0);
        }
      }
    } else {
      new_values->emplace_back(payload);
    }
  }
  return changed;
}
} // namespace

FieldArrayValues RClassWriter::remap_resource_class_clinit(
    const DexClass* cls,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids,
    const FieldArrayValues& known_field_values,
    DexMethod* clinit) const {
  IRCode* ir_code = clinit->get_code();
  always_assert(ir_code->editable_cfg_built());

  // For styleable, we avoid actually deleting entries since there are offsets
  // that will point to the wrong positions in the array. Instead, zero out the
  // values.
  bool zero_out_values = is_styleable(cls);

  RClassReader r_class_reader(m_global_resources_config);
  // Fields that must be patched to new array values.
  FieldArrayValues pending_new_values;
  // The up-to-date map that reflects all rewriting.
  FieldArrayValues return_values;
  for (auto&& [f, vec] :
       r_class_reader.analyze_clinit((DexClass*)cls, known_field_values)) {
    std::vector<uint32_t> new_values;
    if (remap_array(vec, old_to_remapped_ids, zero_out_values, &new_values)) {
      pending_new_values.emplace(f, new_values);
      return_values.emplace(f, new_values);
    } else {
      return_values.emplace(f, vec);
    }
  }

  if (pending_new_values.empty()) {
    return return_values;
  }
  auto& cfg = ir_code->cfg();
  cfg::CFGMutation mutation(cfg);

  // Regenerate the filling of the changed fields, leaving the old code behind.
  std::map<int32_t, reg_t> values_to_reg;
  auto get_register_for_value = [&](int32_t value) {
    auto search = values_to_reg.find(value);
    if (search != values_to_reg.end()) {
      return search->second;
    }
    auto reg = cfg.allocate_temp();
    values_to_reg.emplace(value, reg);
    return reg;
  };
  auto iterable = cfg::InstructionIterable(cfg);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    auto insn = it->insn;
    if (insn->opcode() == OPCODE_SPUT_OBJECT &&
        pending_new_values.count(insn->get_field()) > 0) {
      auto new_values = pending_new_values.at(insn->get_field());
      // Generate new instructions to create array of new size, fill it, move it
      // to field. Something like the following, except CONST instructions will
      // be slapped into the beginning of entry block:
      // OPCODE: CONST v0, 10
      // OPCODE: NEW_ARRAY v0, [I
      // OPCODE: IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v0
      // OPCODE: FILL_ARRAY_DATA v0, <data>
      //   fill-array-data-payload { [10 x 4] { ... yada yada ... } }
      // OPCODE: SPUT_OBJECT v0, Lcom/redextest/R;.six:[I
      auto size_reg = get_register_for_value(new_values.size());
      auto array_reg = cfg.allocate_temp();

      auto new_array = new IRInstruction(OPCODE_NEW_ARRAY);
      new_array->set_src(0, size_reg);
      new_array->set_type(insn->get_field()->get_type());
      auto move_result_pseudo =
          new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
      move_result_pseudo->set_dest(array_reg);
      auto fill_array_data = new IRInstruction(OPCODE_FILL_ARRAY_DATA);
      fill_array_data->set_src(0, array_reg);
      auto op_data = encode_fill_array_data_payload(new_values);
      fill_array_data->set_data(std::move(op_data));

      mutation.insert_before(cfg.find_insn(insn),
                             {new_array, move_result_pseudo, fill_array_data});
      insn->set_src(0, array_reg);
    }
  }

  // Ensure all constants are at beginning of entry block and available to succs
  std::vector<IRInstruction*> consts;
  for (auto&& [lit, reg] : values_to_reg) {
    auto insn = new IRInstruction(OPCODE_CONST);
    insn->set_dest(reg);
    insn->set_literal(lit);
    consts.emplace_back(insn);
  }
  auto i2 = cfg::InstructionIterable(cfg);
  mutation.insert_before(i2.begin(), consts);

  mutation.flush();
  // OSDCE has the capability to mop up array creation and fills that go nowhere
  // but as a simple cleanup effort (for now) run LocalDce to perform some
  // cleanup since the former is not easily runnable on per-method basis right
  // now.
  LocalDce(/* init_classes_with_side_effects */ nullptr, {}).dce(ir_code);
  return return_values;
}

void RClassWriter::remap_resource_class_arrays(
    DexStoresVector& stores,
    const std::map<uint32_t, uint32_t>& old_to_remapped_ids) const {
  Timer t("remap_resource_class_arrays");
  FieldArrayValues field_values;
  RClassReader r_class_reader(m_global_resources_config);
  auto scope = build_class_scope(stores);
  r_class_reader.ordered_r_class_iteration(scope, [&](DexClass* cls) {
    DexMethod* clinit = cls->get_clinit();
    if (clinit == nullptr) {
      return;
    }
    TRACE(OPTRES, 2, "remap_resource_class_arrays, class %s", SHOW(cls));
    IRCode* ir_code = clinit->get_code();
    if (ir_code == nullptr) {
      return;
    }
    auto class_state = remap_resource_class_clinit(cls, old_to_remapped_ids,
                                                   field_values, clinit);
    field_values.insert(class_state.begin(), class_state.end());
  });
}
} // namespace resources
