//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Variables.h"
#include "DAPLog.h"
#include "JSONUtils.h"
#include "LLDBUtils.h"
#include "Protocol/DAPTypes.h"
#include "Protocol/ProtocolRequests.h"
#include "Protocol/ProtocolTypes.h"
#include "SBAPIExtras.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBBlock.h"
#include "lldb/API/SBDeclaration.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBLineEntry.h"
#include "lldb/API/SBSymbolContext.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBValueList.h"
#include "lldb/lldb-defines.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <sys/syslimits.h>
#include <vector>

using namespace llvm;
using namespace lldb_dap;
using namespace lldb_dap::protocol;

namespace {

bool HasInnerVarref(lldb::SBValue &v) {
  return v.MightHaveChildren() || ValuePointsToCode(v) ||
         v.GetDeclaration().IsValid();
}

Variable CreateVariable(lldb::SBValue v, var_ref_t var_ref, bool format_hex,
                        const Configuration &config) {
  VariableDescription desc(v, config.enableAutoVariableSummaries, format_hex);
  Variable var;
  var.name = desc.name;
  var.value = desc.display_value;
  var.type = desc.display_type_name;

  if (!desc.evaluate_name.empty())
    var.evaluateName = desc.evaluate_name;

  // If we have a type with many children, we would like to be able to
  // give a hint to the IDE that the type has indexed children so that the
  // request can be broken up in grabbing only a few children at a time. We
  // want to be careful and only call "v.GetNumChildren()" if we have an array
  // type or if we have a synthetic child provider producing indexed children.
  // We don't want to call "v.GetNumChildren()" on all objects as class, struct
  // and union types don't need to be completed if they are never expanded. So
  // we want to avoid calling this to only cases where we it makes sense to keep
  // performance high during normal debugging.
  //
  // If we have an array type, say that it is indexed and provide the number
  // of children in case we have a huge array. If we don't do this, then we
  // might take a while to produce all children at onces which can delay your
  // debug session.
  if (desc.type_obj.IsArrayType()) {
    var.indexedVariables = v.GetNumChildren();
  } else if (v.IsSynthetic()) {
    // For a type with a synthetic child provider, the SBType of "v" won't tell
    // us anything about what might be displayed. Instead, we check if the first
    // child's name is "[0]" and then say it is indexed. We call
    // GetNumChildren() only if the child name matches to avoid a potentially
    // expensive operation.
    if (lldb::SBValue first_child = v.GetChildAtIndex(0)) {
      llvm::StringRef first_child_name = first_child.GetName();
      if (first_child_name == "[0]") {
        size_t num_children = v.GetNumChildren();
        // If we are creating a "[raw]" fake child for each synthetic type, we
        // have to account for it when returning indexed variables.
        if (config.enableSyntheticChildDebugging)
          ++num_children;
        var.indexedVariables = num_children;
      }
    }
  }

  if (v.MightHaveChildren())
    var.variablesReference = var_ref;

  if (v.GetDeclaration().IsValid())
    var.valueLocationReference = PackLocation(var_ref.AsUInt32(), true);

  if (ValuePointsToCode(v))
    var.declarationLocationReference = PackLocation(var_ref.AsUInt32(), false);

  if (lldb::addr_t addr = v.GetLoadAddress(); addr != LLDB_INVALID_ADDRESS)
    var.memoryReference = addr;

  bool is_readonly = v.GetType().IsAggregateType() ||
                     v.GetValueType() == lldb::eValueTypeRegisterSet;
  if (is_readonly) {
    if (!var.presentationHint)
      var.presentationHint = {VariablePresentationHint()};
    var.presentationHint->attributes.push_back("readOnly");
  }

  return var;
}

template <typename T>
std::vector<Variable>
MakeVariables(T &container, VariableReferenceStorage &storage,
              const Configuration &config, const VariablesArguments &args,
              bool is_permanent) {
  std::vector<Variable> variables;

  const bool format_hex = args.format ? args.format->hex : false;
  auto start_it = begin(container) + args.start;
  auto end_it = args.count == 0 ? end(container) : start_it + args.count;

  // Now we construct the result with unique display variable names.
  for (lldb::SBValue variable : llvm::make_range(start_it, end_it)) {
    const var_ref_t var_ref = HasInnerVarref(variable)
                                  ? storage.Insert(variable, is_permanent)
                                  : var_ref_t::k_no_child;
    if (LLVM_UNLIKELY(var_ref.AsUInt32() >=
                      var_ref_t::k_variables_reference_threshold)) {
      DAP_LOG(storage.log,
              "warning: variablesReference threshold reached. "
              "current: {} threshold: {}, maximum {}.",
              var_ref.AsUInt32(), var_ref_t::k_variables_reference_threshold,
              var_ref_t::k_max_variables_references);
      break;
    }

    if (LLVM_UNLIKELY(var_ref.Kind() == eReferenceKindInvalid))
      break;

    variables.emplace_back(
        CreateVariable(variable, var_ref, format_hex, config));
  }

  return variables;
}

class RegisterStore final : public VariableStore {
public:
  explicit RegisterStore(const lldb::SBFrame &frame)
      : VariableStore(/*is_permanent=*/false), m_frame(frame) {}

  Expected<std::vector<Variable>>
  GetVariables(VariableReferenceStorage &storage, const Configuration &config,
               const VariablesArguments &args) override {
    LoadVariables();
    return MakeVariables(m_children, storage, config, args,
                         /*is_permanent=*/false);
  }

  lldb::SBValue FindVariable(llvm::StringRef name) override {
    LoadVariables();

    return m_children.GetFirstValueByName(name.data());
  }

  lldb::SBValue GetVariable() const override { return lldb::SBValue(); }

private:
  void LoadVariables() {
    if (m_variables_loaded)
      return;

    m_variables_loaded = true;

    m_children = m_frame.GetRegisters();
    // Change the default format of any pointer sized registers in the first
    // register set to be the lldb::eFormatAddressInfo so we show the pointer
    // and resolve what the pointer resolves to. Only change the format if the
    // format was set to the default format or if it was hex as some registers
    // have formats set for them.
    const uint32_t addr_size =
        m_frame.GetThread().GetProcess().GetAddressByteSize();
    for (lldb::SBValue reg : m_children.GetValueAtIndex(0)) {
      const lldb::Format format = reg.GetFormat();
      if (format == lldb::eFormatDefault || format == lldb::eFormatHex) {
        if (reg.GetByteSize() == addr_size)
          reg.SetFormat(lldb::eFormatAddressInfo);
      }
    }
  }

  lldb::SBFrame m_frame;
  lldb::SBValueList m_children;
  bool m_variables_loaded = false;
};

/// Variable store for expandable values.
///
/// Manages children variables of complex types (structs, arrays, pointers,
/// etc.) that can be expanded in the debugger UI.
class ExpandableValueStore final : public VariableStore {

public:
  explicit ExpandableValueStore(bool is_permanent, const lldb::SBValue &value)
      : VariableStore(is_permanent), m_value(value) {}

  llvm::Expected<std::vector<protocol::Variable>>
  GetVariables(VariableReferenceStorage &storage,
               const protocol::Configuration &config,
               const protocol::VariablesArguments &args) override {
    std::map<lldb::user_id_t, std::string> name_overrides;
    lldb::SBValueList list;
    for (auto inner : m_value)
      list.Append(inner);

    // We insert a new "[raw]" child that can be used to inspect the raw version
    // of a synthetic member. That eliminates the need for the user to go to the
    // debug console and type `frame var <variable> to get these values.
    if (config.enableSyntheticChildDebugging && m_value.IsSynthetic()) {
      lldb::SBValue synthetic_value = m_value.GetSyntheticValue();
      name_overrides[synthetic_value.GetID()] = "[raw]";
      // FIXME: Cloning the value seems to affect the type summary, see
      // https://github.com/llvm/llvm-project/issues/183578
      // m_value.GetSyntheticValue().Clone("[raw]");
      list.Append(synthetic_value);
    }

    return MakeVariables(list, storage, config, args, IsPermanent());
  }

  lldb::SBValue FindVariable(llvm::StringRef name) override {
    if (name == "[raw]" && m_value.IsSynthetic())
      return m_value.GetSyntheticValue();

    // Handle mapped index
    lldb::SBValue variable = m_value.GetChildMemberWithName(name.data());
    if (variable.IsValid())
      return variable;

    // Handle array indexes
    uint64_t index = 0;
    if (name.consume_front('[') && name.consume_back("]") &&
        !name.consumeInteger(0, index))
      variable = m_value.GetChildAtIndex(index);

    return variable;
  }

  [[nodiscard]] lldb::SBValue GetVariable() const override { return m_value; }

private:
  lldb::SBValue m_value;
};

/// A Variable store for fetching variables within a specific scope (locals,
/// globals, blocks, etc.) for a given stack frame.
class ExpandableValueListStore final : public VariableStore {

public:
  explicit ExpandableValueListStore(bool is_permanent,
                                    const lldb::SBValueList &list)
      : VariableStore(is_permanent), m_value_list(list) {}

  llvm::Expected<std::vector<protocol::Variable>>
  GetVariables(VariableReferenceStorage &storage,
               const protocol::Configuration &config,
               const protocol::VariablesArguments &args) override {
    return MakeVariables(m_value_list, storage, config, args, IsPermanent());
  }

  lldb::SBValue FindVariable(llvm::StringRef name) override {
    return m_value_list.GetFirstValueByName(name.data());
  }

  [[nodiscard]] lldb::SBValue GetVariable() const override {
    return lldb::SBValue();
  }

private:
  lldb::SBValueList m_value_list;
};

} // namespace

namespace lldb_dap {

lldb::SBValue VariableReferenceStorage::GetVariable(var_ref_t var_ref) {
  const ReferenceKind kind = var_ref.Kind();

  if (kind == eReferenceKindTemporary) {
    if (auto *store = m_temporary_kind_pool.GetVariableStore(var_ref))
      return store->GetVariable();
  }

  if (kind == eReferenceKindPermanent) {
    if (auto *store = m_permanent_kind_pool.GetVariableStore(var_ref))
      return store->GetVariable();
  }

  return {};
}

var_ref_t VariableReferenceStorage::Insert(lldb::SBValue &variable,
                                           bool permanent) {
  if (permanent)
    return m_permanent_kind_pool.Add<ExpandableValueStore>(true, variable);

  return m_temporary_kind_pool.Add<ExpandableValueStore>(false, variable);
}

var_ref_t VariableReferenceStorage::Insert(const lldb::SBValueList &values,
                                           bool permanent) {
  if (permanent)
    return m_permanent_kind_pool.Add<ExpandableValueListStore>(true, values);

  return m_temporary_kind_pool.Add<ExpandableValueListStore>(false, values);
}

std::vector<Scope>
VariableReferenceStorage::Insert(lldb::SBFrame &frame,
                                 llvm::StringRef last_step_out_frame) {
  lldb::user_id_t id = MakeDAPFrameID(frame);
  auto &frame_storage = m_frame_storage[id];

  lldb::SBValue return_value;
  if (frame_storage.return_value == std::nullopt && frame.GetFrameID() == 0 &&
      ((return_value = frame.GetThread().GetStopReturnValue()))) {
    lldb::SBValueList list;
    list.Append(return_value.Clone("(Return value)"));
    frame_storage.return_value = Insert(list, /*permanent=*/false);
  }

  for (lldb::SBBlock block = frame.GetBlock(); block;
       block = block.GetParent()) {
    if (frame_storage.blocks.find(block.GetID()) != frame_storage.blocks.end())
      continue;
    lldb::SBValueList list =
        block.GetVariables(frame, /*arguments=*/true,
                           /*locals=*/true,
                           /*statics=*/true, lldb::eDynamicDontRunTarget);
    frame_storage.blocks[block.GetID()] =
        list.GetSize() ? Insert(list, /*permanent=*/false)
                       : var_ref_t::k_invalid_var_ref;
  }

  if (frame_storage.static_storage == std::nullopt) {
    lldb::SBValueList list =
        frame.GetVariables(/*arguments=*/false, /*locals=*/false,
                           /*statics=*/true, /*in_scope_only=*/true);
    frame_storage.static_storage = list.GetSize()
                                       ? Insert(list, /*permanent=*/false)
                                       : var_ref_t::k_invalid_var_ref;
  }

  if (frame_storage.registers == std::nullopt)
    frame_storage.registers = m_temporary_kind_pool.Add<RegisterStore>(frame);

  std::vector<Scope> scopes;
  if (frame_storage.return_value != std::nullopt) {
    Scope s;
    s.name = std::string("Return value: ") +
             (last_step_out_frame.empty() ? frame.GetDisplayFunctionName()
                                          : last_step_out_frame.data());
    s.presentationHint = Scope::eScopePresentationHintReturnValue;
    s.variablesReference = *frame_storage.return_value;
    s.indexedVariables = 1;
    scopes.push_back(s);
  }

  lldb::SBBlock frame_block = frame.GetFrameBlock();
  for (lldb::SBBlock block = frame.GetBlock();
       block && frame_storage.blocks[block.GetID()];
       block = block.GetParent()) {
    Scope s;
    if (block == frame_block) {
      s.presentationHint = protocol::Scope::eScopePresentationHintArguments;
      s.name = std::string("Locals: ") + frame.GetDisplayFunctionName();
    } else {
      s.presentationHint = protocol::Scope::eScopePresentationHintLocals;
      std::string name = "Block: ";
      for (uint32_t i = 0; i < block.GetNumRanges(); i++) {
        lldb::SBAddress start = block.GetRangeStartAddress(i);
        lldb::SBAddress end = block.GetRangeEndAddress(i);
        name += llvm::formatv("[{0:x}-{1:x}) ", start.GetFileAddress(),
                              end.GetFileAddress())
                    .str();
      }
      s.name = name.substr(0, name.size() - 1);
    }

    for (uint32_t i = 0; i < block.GetNumRanges(); i++) {
      lldb::SBAddress start = block.GetRangeStartAddress(i);
      lldb::SBAddress end = block.GetRangeEndAddress(i);
      lldb::SBLineEntry line_entry = start.GetLineEntry();
      if (start && line_entry) {
        std::array<char, PATH_MAX> path = {0};
        auto len = line_entry.GetFileSpec().GetPath(path.data(), PATH_MAX);
        Source src;
        src.path = llvm::StringRef(path.data(), len);
        s.source = src;
        s.line = std::min(line_entry.GetLine(), s.line);
        s.column = line_entry.GetColumn();

        if (end && ((line_entry = end.GetLineEntry()))) {
          s.endLine =
              std::max(line_entry.GetLine(),
                       s.endLine == LLDB_INVALID_LINE_NUMBER ? 0 : s.endLine);
          s.endColumn = line_entry.GetColumn();
        }
      }
    }

    s.variablesReference = frame_storage.blocks[block.GetID()];
    scopes.push_back(s);
  }

  if (frame_storage.static_storage && *frame_storage.static_storage) {
    Scope s;
    s.name = std::string("Static: ") + frame.GetDisplayFunctionName();
    s.variablesReference = *frame_storage.static_storage;
    s.expensive = true;
    scopes.push_back(s);
  }

  if (frame_storage.registers && *frame_storage.registers) {
    Scope s;
    s.name = "Registers";
    s.variablesReference = *frame_storage.registers;
    s.expensive = true;
    scopes.push_back(s);
  }

  return scopes;
}

lldb::SBValue VariableReferenceStorage::FindVariable(var_ref_t var_ref,
                                                     llvm::StringRef name) {
  if (VariableStore *store = GetVariableStore(var_ref))
    return store->FindVariable(name);

  return {};
}

VariableStore *VariableReferenceStorage::GetVariableStore(var_ref_t var_ref) {
  const ReferenceKind kind = var_ref.Kind();
  switch (kind) {
  case eReferenceKindPermanent:
    return m_permanent_kind_pool.GetVariableStore(var_ref);
  case eReferenceKindTemporary:
    return m_temporary_kind_pool.GetVariableStore(var_ref);
  case eReferenceKindInvalid:
    return nullptr;
  }
  llvm_unreachable("Unknown reference kind.");
}

} // namespace lldb_dap
