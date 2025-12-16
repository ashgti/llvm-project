//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Protocol/ProtocolRequests.h"
#include "ProtocolUtils.h"
#include "RequestHandler.h"
#include <mutex>

using namespace llvm;
using namespace lldb;
using namespace lldb_dap;
using namespace lldb_dap::protocol;

/// Retrieves the set of all sources currently loaded by the debugged process.
///
/// Clients should only call this request if the corresponding capability
/// `supportsLoadedSourcesRequest` is true.
Expected<LoadedSourcesResponseBody> LoadedSourcesRequestHandler::Run(
    const LoadedSourcesArguments &arguments) const {
  SBMutex api_mutex = dap.GetAPIMutex();
  const std::scoped_lock<SBMutex> lock(api_mutex);
  LoadedSourcesResponseBody body;
  body.sources = dap.source_tracker.ResetAndGetSources();
  return body;
}
