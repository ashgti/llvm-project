//===- ProtocolServerMCP.h ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PLUGINS_PROTOCOL_MCP_PROTOCOLSERVERMCP_H
#define LLDB_PLUGINS_PROTOCOL_MCP_PROTOCOLSERVERMCP_H

#include "Protocol.h"
#include "Tool.h"
#include "lldb/Core/ProtocolServer.h"
#include "lldb/Host/MCP.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Host/Socket.h"
#include "llvm/ADT/StringMap.h"
#include <thread>

namespace lldb_private::mcp {

class ProtocolServerMCP : public ProtocolServer {
public:
  ProtocolServerMCP();
  virtual ~ProtocolServerMCP() override;

  llvm::Error Start(ProtocolServer::Connection connection) override;
  llvm::Error Stop() override;
  llvm::Error ConnectToMultiplexer() override;

  static void Initialize();
  static void Terminate();
  static void DebuggerInitialize(lldb_private::Debugger &debugger);

  static llvm::StringRef GetPluginNameStatic() { return "MCP"; }
  static llvm::StringRef GetPluginDescriptionStatic();

  static lldb::ProtocolServerUP CreateInstance();

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

  Socket *GetSocket() const override { return m_listener.get(); }

protected:
  using RequestHandler = std::function<llvm::Expected<protocol::Response>(
      const protocol::Request &)>;
  using NotificationHandler =
      std::function<void(const protocol::Notification &)>;

  void AddTool(std::unique_ptr<Tool> tool);
  void AddRequestHandler(llvm::StringRef method, RequestHandler handler);
  void AddNotificationHandler(llvm::StringRef method,
                              NotificationHandler handler);

private:
  void AcceptCallback(std::unique_ptr<Socket> socket);

  llvm::Expected<std::optional<protocol::Message>>
  HandleData(llvm::StringRef data);

  llvm::Expected<protocol::Response> Handle(protocol::Request request);
  void Handle(protocol::Notification notification);

  llvm::Expected<protocol::Response>
  InitializeHandler(const protocol::Request &);
  llvm::Expected<protocol::Response>
  ToolsListHandler(const protocol::Request &);
  llvm::Expected<protocol::Response>
  ToolsCallHandler(const protocol::Request &);
  llvm::Expected<protocol::Response> EvaluateHandler(const protocol::Request &);

  protocol::Capabilities GetCapabilities();
  llvm::Error ReadCallback(Transport &client);
  void RunLoop();

  llvm::StringLiteral kName = "lldb-mcp";
  llvm::StringLiteral kVersion = "0.1.0";

  std::mutex m_server_mutex;

  bool m_running = false;

  MainLoop m_loop;
  std::thread m_loop_thread;

  std::unique_ptr<Socket> m_listener;
  ClientUP m_multiplexer;
  std::vector<MainLoopBase::ReadHandleUP> m_listen_handlers;

  std::vector<std::pair<TransportUP, MainLoopBase::ReadHandleUP>>
      m_connected_clients;
  llvm::StringMap<std::unique_ptr<Tool>> m_tools;

  llvm::StringMap<RequestHandler> m_request_handlers;
  llvm::StringMap<NotificationHandler> m_notification_handlers;
};
} // namespace lldb_private::mcp

#endif
