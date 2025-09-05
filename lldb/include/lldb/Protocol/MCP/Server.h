//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PROTOCOL_MCP_SERVER_H
#define LLDB_PROTOCOL_MCP_SERVER_H

#include "lldb/Host/JSONTransport.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Resource.h"
#include "lldb/Protocol/MCP/Tool.h"
#include "lldb/Protocol/MCP/Transport.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include <memory>
#include <string>
#include <vector>

namespace lldb_private::mcp {
class ProtocolServerMCP;
}

namespace lldb_protocol::mcp {

/// Information about this instance of lldb's MCP server for lldb-mcp to use to
/// coordinate connecting an lldb-mcp client.
struct ServerInfo {
  std::string connection_uri;
  lldb::pid_t pid;

  static llvm::Error Write(const ServerInfo &);
  static llvm::Expected<std::vector<ServerInfo>> Load();
};
llvm::json::Value toJSON(const ServerInfo &);
bool fromJSON(const llvm::json::Value &, ServerInfo &, llvm::json::Path);

class Server {
public:
  Server(std::string name, std::string version, MCPTransport &client,
         lldb_private::MainLoop &loop, LogCallback log_callback = {});
  ~Server() = default;

  void AddTool(std::unique_ptr<Tool> tool);
  void AddResourceProvider(std::unique_ptr<ResourceProvider> resource_provider);

  llvm::Expected<lldb_private::MainLoop::ReadHandleUP> RegisterClient();
  llvm::Error Run();

  operator MCPTransport::MessageHandler &() { return m_binder; }

  friend class lldb_private::mcp::ProtocolServerMCP;

protected:
  ServerCapabilities GetCapabilities();

  llvm::Expected<InitializeResult> InitializeHandler(const InitializeParams &);

  llvm::Expected<ListToolsResult> ToolsListHandler();
  llvm::Expected<CallToolResult> ToolsCallHandler(const CallToolParams &);

  llvm::Expected<ListResourcesResult> ResourcesListHandler();
  llvm::Expected<ReadResourceResult>
  ResourcesReadHandler(const ReadResourceParams &);

  void TerminateLoop();

  template <typename... Ts> inline auto Logv(const char *Fmt, Ts &&...Vals) {
    Log(llvm::formatv(Fmt, std::forward<Ts>(Vals)...).str());
  }
  void Log(llvm::StringRef message) {
    if (m_log_callback)
      m_log_callback(message);
  }

  MCPTransport::Binder<> m_binder;

private:
  const std::string m_name;
  const std::string m_version;

  MCPTransport &m_client;
  lldb_private::MainLoop &m_loop;
  LogCallback m_log_callback;

  llvm::StringMap<std::unique_ptr<Tool>> m_tools;
  std::vector<std::unique_ptr<ResourceProvider>> m_resource_providers;
};

} // namespace lldb_protocol::mcp

#endif
