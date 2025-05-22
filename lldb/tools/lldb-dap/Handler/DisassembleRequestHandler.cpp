//===-- DisassembleRequestHandler.cpp -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "EventHelper.h"
#include "JSONUtils.h"
#include "Protocol/ProtocolRequests.h"
#include "Protocol/ProtocolTypes.h"
#include "RequestHandler.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBInstruction.h"
#include "lldb/API/SBTarget.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <optional>

using namespace llvm;
using namespace lldb;
using namespace lldb_dap::protocol;

namespace lldb_dap {

static DisassembledInstruction GetInvalidInstruction() {
  DisassembledInstruction invalid_inst;
  invalid_inst.presentationHint =
      DisassembledInstruction::eDisassembledInstructionPresentationHintInvalid;
  return invalid_inst;
}

static SBAddress GetDisassembleStartAddress(SBTarget target, SBAddress addr,
                                            int64_t instruction_offset) {
  if (instruction_offset == 0)
    return addr;

  if (target.GetMinimumOpcodeByteSize() == target.GetMaximumOpcodeByteSize()) {
    // We have fixed opcode size, so we can calculate the address directly,
    // negative or positive.
    addr_t load_addr = addr.GetLoadAddress(target);
    load_addr += instruction_offset * target.GetMinimumOpcodeByteSize();
    return SBAddress(load_addr, target);
  }

  if (instruction_offset > 0) {
    SBInstructionList forward_insts =
        target.ReadInstructions(addr, instruction_offset + 1);
    return forward_insts.GetInstructionAtIndex(forward_insts.GetSize() - 1)
        .GetAddress();
  }

  // We have a negative instruction offset, so we need to disassemble backwards.
  // The opcode size is not fixed, use the max opcode size to approximate the
  // offset.
  const size_t backwards_instructions_count =
      static_cast<size_t>(std::abs(instruction_offset));

  SBAddress lookback_addr;
  // Binary search for the nearest valid lookback address using our
  // approximation offset.
  addr_t approx_inst_offset =
      backwards_instructions_count * target.GetMaximumOpcodeByteSize();
  while (approx_inst_offset >= 0 && !lookback_addr.IsValid()) {
    lookback_addr = target.ResolveLoadAddress(addr.GetLoadAddress(target) -
                                              approx_inst_offset);
    approx_inst_offset /= 2;
  }
  if (!lookback_addr.IsValid())
    return addr;

  // Add valid instructions before the current instruction using the symbol.
  SBInstructionList symbol_insts =
      target.ReadInstructions(lookback_addr, addr, nullptr);
  if (!symbol_insts.IsValid() || symbol_insts.GetSize() == 0)
    return addr;

  if (symbol_insts.GetSize() < backwards_instructions_count) {
    // We don't have enough instructions to disassemble backwards, so just
    // return the start address of the symbol.
    return symbol_insts.GetInstructionAtIndex(0).GetAddress();
  }

  return symbol_insts
      .GetInstructionAtIndex(symbol_insts.GetSize() -
                             backwards_instructions_count)
      .GetAddress();
}

static DisassembledInstruction ToDisassembledInstruction(SBTarget &target,
                                                         SBInstruction &inst,
                                                         bool resolve_symbols) {
  if (!inst.IsValid())
    return GetInvalidInstruction();

  auto addr = inst.GetAddress();
  const auto inst_addr = addr.GetLoadAddress(target);
  const char *m = inst.GetMnemonic(target);
  const char *o = inst.GetOperands(target);
  const char *c = inst.GetComment(target);
  auto d = inst.GetData(target);

  std::string bytes;
  raw_string_ostream sb(bytes);
  for (unsigned i = 0; i < inst.GetByteSize(); i++) {
    SBError error;
    uint8_t b = d.GetUnsignedInt8(error, i);
    if (error.Success())
      sb << format("%2.2x ", b);
  }

  DisassembledInstruction disassembled_inst;
  disassembled_inst.address = inst_addr;
  disassembled_inst.instructionBytes =
      bytes.size() > 0 ? bytes.substr(0, bytes.size() - 1) : "";

  std::string instruction;
  raw_string_ostream si(instruction);

  SBSymbol symbol = addr.GetSymbol();
  // Only add the symbol on the first line of the function.
  if (symbol.IsValid() && symbol.GetStartAddress() == addr) {
    // If we have a valid symbol, append it as a label prefix for the first
    // instruction. This is so you can see the start of a function/callsite
    // in the assembly, at the moment VS Code (1.80) does not visualize the
    // symbol associated with the assembly instruction.
    si << (symbol.GetMangledName() != nullptr ? symbol.GetMangledName()
                                              : symbol.GetName())
       << ": ";

    if (resolve_symbols)
      disassembled_inst.symbol = symbol.GetDisplayName();
  }

  si << formatv("{0,7} {1,12}", m, o);
  if (c && c[0]) {
    si << " ; " << c;
  }

  disassembled_inst.instruction = std::move(instruction);

  SBLineEntry line_entry = addr.GetLineEntry();
  // If the line number is 0 then the entry represents a compiler generated
  // location.
  if (line_entry.IsValid() && line_entry.GetFileSpec().IsValid() &&
      line_entry.GetLine() != 0) {
    Source source = CreateSource(line_entry);
    disassembled_inst.location = std::move(source);

    const auto line = line_entry.GetLine();
    if (line != 0 && line != LLDB_INVALID_LINE_NUMBER)
      disassembled_inst.line = line;

    const auto column = line_entry.GetColumn();
    if (column != 0 && column != LLDB_INVALID_COLUMN_NUMBER)
      disassembled_inst.column = column;

    auto end_line_entry = line_entry.GetEndAddress().GetLineEntry();
    if (end_line_entry.IsValid() &&
        end_line_entry.GetFileSpec() == line_entry.GetFileSpec()) {
      const auto end_line = end_line_entry.GetLine();
      if (end_line != 0 && end_line != LLDB_INVALID_LINE_NUMBER &&
          end_line != line) {
        disassembled_inst.endLine = end_line;

        const auto end_column = end_line_entry.GetColumn();
        if (end_column != 0 && end_column != LLDB_INVALID_COLUMN_NUMBER &&
            end_column != column)
          disassembled_inst.endColumn = end_column - 1;
      }
    }
  }

  return disassembled_inst;
}

/// Disassembles code stored at the provided location.
/// Clients should only call this request if the corresponding capability
/// `supportsDisassembleRequest` is true.
///
/// Implementation notes:
///
/// VSCode / DAP will make repeated requests for disassembly asking for
/// instruction offsets. VSCode will request an address with a negative
/// instruction offset to load instructions around the target address.
///
/// NOTE: Returning less than the requested number of instructions will cause
/// the Disassemble Viewer in VSCode to stop paging in new results.
///
/// However, the returned DisassembledInstruction's do not need to be
/// contigious.
///
/// To better support the Disassembly Viewer in VSCode we should make a best
/// effort attempt at representing the disassembly as a non-contigious set of
/// instructions loadded into memory.
///
/// This means the resulting disassembly can cross a module boundary and may
/// jump across memory ranges.
///
/// For example, the debuggee contains the following memory layout:
///
/// ```
/// # Modules:
///
/// [ 0] [0x100000400-0x100000900] a.out
/// [ 1] [0x100010000-0x100020000] libFoo.dylib
///
/// # Symbols:
///
/// [ 0] a.out`main: [0x1000004d0 - 0x100000510]
/// [ 1] libFoo.dylib`add: [0x100010000 - 0x100010034]
/// [ 2] libFoo.dylib`handler: [0x100010038 - 0x100010068]
/// ```
///
/// And the following request is recieved:
///
/// ```
/// {
///   "command": "disassemble",
///   "arguments": {
///     "memoryReference": "0x100010000", // libFoo.dylib`add address
///     "instructionOffset": -50,
///     "instructionCount": 100,
///   }
/// }
/// ```
///
/// Then lldb-dap should return DisassembledInstruction's reaching backwards
/// into a.out`main even though there is a jump in addresses.
Expected<DisassembleResponseBody>
DisassembleRequestHandler::Run(const DisassembleArguments &args) const {
  std::optional<addr_t> addr_opt = DecodeMemoryReference(args.memoryReference);
  if (!addr_opt.has_value())
    return make_error<DAPError>("Malformed memory reference: " +
                                args.memoryReference);

  addr_t addr_ptr = *addr_opt;
  addr_ptr += args.offset.value_or(0);
  SBAddress addr(addr_ptr, dap.target);
  if (!addr.IsValid())
    return make_error<DAPError>(
        formatv("Memory reference ({0}) not found in {1}.", addr_ptr,
                dap.target.GetProcess().GetProcessInfo().GetName())
            .str());

  std::string flavor_string;
  const auto target_triple = StringRef(dap.target.GetTriple());
  // This handles both 32 and 64bit x86 architecture. The logic is duplicated in
  // `CommandObjectDisassemble::CommandOptions::OptionParsingStarting`
  if (target_triple.starts_with("x86")) {
    const SBStructuredData flavor =
        dap.debugger.GetSetting("target.x86-disassembly-flavor");

    const size_t str_length = flavor.GetStringValue(nullptr, 0);
    if (str_length != 0) {
      flavor_string.resize(str_length + 1);
      flavor.GetStringValue(flavor_string.data(), flavor_string.length());
    }
  }

  std::vector<DisassembledInstruction> instructions;

  // FIXME: This should find valid address ranges by using an offset like
  // `target.GetMaximumOpcodeByteSize() * instOff` + adjusting for cross module
  // boundaries.

  // Calculate a sufficient address to start disassembling from.
  SBInstructionList insts =
      addr.GetSymbol().GetInstructions(dap.target, flavor_string.data());
  // for (int i = 0 ; i < insts.GetSize(); i++)
  //   insts.GetInstructionAtIndex(i).GetAddress() == addr

  // auto approx_addr_offset =
  //     addr.GetSymbol().GetSize(); // * dap.target.GetMaximumOpcodeByteSize();
  // if (!disassemble_start_addr.IsValid())
  //   return make_error<DAPError>(
  //       "Unexpected error while disassembling instructions.");

  // SBInstructionList insts = dap.target.ReadInstructions(
  //     disassemble_start_addr, args.instructionCount, flavor_string.c_str());
  // if (!insts.IsValid())
  //   return make_error<DAPError>(
  //       "Unexpected error while disassembling instructions.");

  // Conver the found instructions to the DAP format.
  const bool resolve_symbols = args.resolveSymbols.value_or(false);

  for (size_t i = 0; i < insts.GetSize(); ++i) {
    SBInstruction inst = insts.GetInstructionAtIndex(i);
    instructions.push_back(
        ToDisassembledInstruction(dap.target, inst, resolve_symbols));
  }

  // Trim excess instructions if needed.
  if (instructions.size() > args.instructionCount)
    instructions.resize(args.instructionCount);

  return DisassembleResponseBody{std::move(instructions)};
}

} // namespace lldb_dap
