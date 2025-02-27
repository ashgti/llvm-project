//===-- SourceRequestHandler.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "LLDBUtils.h"
#include "Protocol.h"
#include "RequestHandler.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBInstructionList.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "llvm/Support/Error.h"

using namespace lldb_dap;
using namespace lldb_dap::protocol;

namespace lldb_dap {

// "SourceRequest": {
//   "allOf": [ { "$ref": "#/definitions/Request" }, {
//     "type": "object",
//     "description": "Source request; value of command field is 'source'. The
//     request retrieves the source code for a given source reference.",
//     "properties": {
//       "command": {
//         "type": "string",
//         "enum": [ "source" ]
//       },
//       "arguments": {
//         "$ref": "#/definitions/SourceArguments"
//       }
//     },
//     "required": [ "command", "arguments"  ]
//   }]
// },
llvm::Expected<SourceResponseBody>
SourceRequestHandler::Execute(const SourceArguments &arguments) const {
  int64_t source_ref = arguments.source ? *arguments.source->sourceReference
                                        : arguments.sourceReference;

  if (!source_ref) {
    return llvm::createStringError(
        "invalid arguments, expected source.sourceReference to be set");
  }

  lldb::SBProcess process = dap.target.GetProcess();
  // Upper 32 bits is the thread index ID
  lldb::SBThread thread =
      process.GetThreadByIndexID(GetLLDBThreadIndexID(source_ref));
  // Lower 32 bits is the frame index
  lldb::SBFrame frame = thread.GetFrameAtIndex(GetLLDBFrameID(source_ref));
  if (!frame.IsValid()) {
    return llvm::createStringError("source not found");
  }

  lldb::SBInstructionList insts = frame.GetSymbol().GetInstructions(dap.target);
  lldb::SBStream stream;
  insts.GetDescription(stream);

  SourceResponseBody body;
  body.content = stream.GetData();
  body.mimeType = "text/x-lldb.disassembly";
  return std::move(body);
}

} // namespace lldb_dap
