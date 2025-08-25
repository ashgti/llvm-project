//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PROTOCOL_MCP_RELAY_H
#define LLDB_PROTOCOL_MCP_RELAY_H

#include "lldb/Host/Socket.h"
#include "lldb/Protocol/MCP/Binder.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Tool.h"
#include "lldb/Protocol/MCP/Transport.h"
#include "lldb/Utility/UUID.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <mutex>

namespace lldb_protocol::mcp {

class RelayClient {
public:
  explicit RelayClient(MCPTransport *conn) : m_bind(conn) {
    Initialize = m_bind.outgoingRequest<InitializeParams, InitializeResult>(
        "initialize");
    ToolsList = m_bind.outgoingRequest<Void, ToolsListResult>("tools/list");
    ToolsCall =
        m_bind.outgoingRequest<ToolsCallParams, ToolsCallResult>("tools/call");
    ResourcesList =
        m_bind.outgoingRequest<Void, ResourcesListResult>("resources/list");
    ResourcesRead =
        m_bind.outgoingRequest<ResourcesReadParams, ResourcesReadResult>(
            "resources/read");
    DebuggersChanged = m_bind.outgoingNotification<DebuggersChangedParams>(
        "_debuggersChanged");
    m_bind.request("_evaluate", &RelayClient::Evaluate, this);
  };

  RelayClient(const RelayClient &) = delete;
  RelayClient &operator=(const RelayClient &) = delete;

  OutgoingRequest<InitializeParams, InitializeResult> Initialize;
  OutgoingRequest<Void, ToolsListResult> ToolsList;
  OutgoingRequest<ToolsCallParams, ToolsCallResult> ToolsCall;
  OutgoingRequest<Void, ResourcesListResult> ResourcesList;
  OutgoingRequest<ResourcesReadParams, ResourcesReadResult> ResourcesRead;

  OutgoingNotification<DebuggersChangedParams> DebuggersChanged;
  llvm::Expected<ToolsCallResult> Evaluate(const EvaluateParams &);

  operator MCPTransport::MessageHandler &() { return m_bind; }

private:
  Binder m_bind;
};

class RelayServer {
public:
  RelayServer(lldb_private::MainLoop &loop, llvm::raw_ostream *);

  RelayServer(const RelayServer &) = delete;
  RelayServer &operator=(const RelayServer &) = delete;

  // client management

  void OnConnect(lldb_private::MainLoop &,
                 std::unique_ptr<lldb_private::Socket>);
  void OnDisconnect(MCPTransport *);

  // notification handlers

  // non-mcp internal handler, '_debuggersChanged'
  void OnDebuggersChanged(const DebuggersChangedParams &, Transport *);

  // request handlers

  llvm::Expected<InitializeResult> Initialize(const InitializeParams &);
  llvm::Expected<ToolsListResult> ToolsList(const Void &);
  void ToolsCall(const ToolsCallParams &,
                 llvm::unique_function<void(llvm::Expected<ToolsCallResult>)>);
  llvm::Expected<ResourcesListResult> ResourcesList(const Void &);
  llvm::Expected<ResourcesReadResult>
  ResourcesRead(const ResourcesReadParams &);

  // Internal helper
  void EvaluateForClient(const lldb_private::UUID &uuid,
                         const EvaluateParams &params,
                         Reply<ToolsCallResult> reply);

private:
  lldb_private::MainLoop &m_loop;
  llvm::raw_ostream *m_logger;

  struct ClientState {
    BinderUP binder;
    lldb_private::MainLoop::ReadHandleUP handle;
    TransportUP transport;

    OutgoingRequest<EvaluateParams, ToolsCallResult> Evaluate;

    lldb_private::UUID uuid = lldb_private::UUID::Generate();
    std::vector<DebuggerRef> debuggers = {};
  };
  std::mutex m_clients_mutex;
  std::map<MCPTransport *, ClientState> m_clients;

  llvm::StringMap<std::unique_ptr<Tool>> m_tools;
};

} // namespace lldb_protocol::mcp

#endif
