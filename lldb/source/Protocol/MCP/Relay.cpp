//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Protocol/MCP/Relay.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Transport.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <optional>

using namespace llvm;
using namespace lldb;
using namespace lldb_private;

namespace lldb_protocol::mcp {

class LaunchTool final : public Tool {
public:
  LaunchTool()
      : Tool(/*name=*/"launch",
             /*description=*/"Launch a new debugger instance.") {}

  void Call(const ToolArguments &args,
            llvm::unique_function<
                void(llvm::Expected<lldb_protocol::mcp::ToolsCallResult>)>
                reply) override {
    outs() << "Launching lldb....\n";
    reply(ToolsCallResult{{{"launching lldb..."}}, false, std::nullopt});
  }

  std::optional<json::Value> GetSchema() const override {
    using namespace json;
    return Object{{"type", "object"}};
  }
};

struct CommandToolArguments {
  std::string debugger_uri;
  std::string arguments;
};

static bool fromJSON(const llvm::json::Value &V, CommandToolArguments &A,
                     llvm::json::Path P) {
  llvm::json::ObjectMapper O(V, P);
  return O && O.map("debugger_uri", A.debugger_uri) &&
         O.mapOptional("arguments", A.arguments);
}

class EvaluateTool final : public Tool {
public:
  EvaluateTool(RelayServer &server)
      : Tool(/*name=*/"evaluate",
             /*description=*/"Evaluate an lldb expression."),
        m_server(server) {}

  void Call(const ToolArguments &args,
            llvm::unique_function<
                void(llvm::Expected<lldb_protocol::mcp::ToolsCallResult>)>
                reply) override {
    if (!std::holds_alternative<json::Value>(args))
      return reply(createStringError("CommandTool requires arguments"));

    json::Path::Root root;

    CommandToolArguments arguments;
    if (!fromJSON(std::get<json::Value>(args), arguments, root))
      return reply(root.getError());

    llvm::StringRef uri = StringRef(arguments.debugger_uri);
    if (!uri.consume_front("lldb://session/"))
      return reply(createStringError("malformed URI, invalid prefix"));
    EvaluateParams params;
    params.expression = arguments.arguments;
    auto spl = llvm::split(uri, "/");
    auto it = spl.begin();
    if (it == spl.end())
      return reply(createStringError("malformed URI, missing uuid"));
    StringRef raw_uuid = *it;
    lldb_private::UUID uuid;
    if (!uuid.SetFromStringRef(raw_uuid))
      return reply(createStringError("malformed URI, invalid uuid"));
    ++it;
    if (it == spl.end())
      return reply(createStringError("malformed URI, missing debugger ID"));
    StringRef raw_debugger_id = *it;
    if (!llvm::to_integer(raw_debugger_id, params.debugger_id))
      return reply(createStringError("malformed URI, invalid debugger ID"));
    ++it;
    if (it != spl.end()) {
      StringRef raw_target_id = *it;
      if (!llvm::to_integer(raw_target_id, params.target_id))
        return reply(createStringError("malformed URI, invalid target ID"));
    }
    ++it;
    if (it != spl.end())
      return reply(
          createStringError("malformed URI, too many parts to the path"));

    m_server.EvaluateForClient(
        uuid, params,
        [reply =
             std::move(reply)](llvm::Expected<ToolsCallResult> result) mutable {
          if (result)
            return reply(*result);

          llvm::consumeError(result.takeError());
          reply(ToolsCallResult{{{"error"}}, true, std::nullopt});
        });
  }

  std::optional<json::Value> GetSchema() const override {
    using namespace json;
    Object id_type{{"type", "number"}};
    Object str_type{{"type", "string"}};
    Object properties{{"debugger_uri", std::move(id_type)},
                      {"arguments", std::move(str_type)}};
    Array required{"debugger_id"};
    Object schema{{"type", "object"},
                  {"properties", std::move(properties)},
                  {"required", std::move(required)}};
    return schema;
  }

private:
  RelayServer &m_server;
};

llvm::Expected<ToolsCallResult>
RelayClient::Evaluate(const EvaluateParams &params) {
  ToolsCallResult result;
  result.content = {TextContent{
      "Hello world! " + params.expression,
  }};
  return result;
}

RelayServer::RelayServer(MainLoop &loop, raw_ostream *logger)
    : m_loop(loop), m_logger(logger) {
  m_tools["launch"] = std::make_unique<LaunchTool>();
  m_tools["evaluate"] = std::make_unique<EvaluateTool>(*this);
}

void RelayServer::OnConnect(MainLoop &loop, std::unique_ptr<Socket> sock) {
  IOObjectSP io = std::move(sock);
  TransportUP conn =
      std::make_unique<Transport>(io, io, "client", [this](StringRef msg) {
        if (m_logger)
          *m_logger << msg << "\n";
      });
  Transport *conn_p = conn.get();

  std::lock_guard<std::mutex> guard(m_clients_mutex);
  auto it = m_clients.emplace(conn_p, ClientState{});
  assert(it.second);
  it.first->second.transport = std::move(conn);
  it.first->second.binder = std::make_unique<Binder>(conn_p);

  auto &bind = it.first->second.binder;

  bind->request("initialize", &RelayServer::Initialize, this);
  bind->request("tools/list", &RelayServer::ToolsList, this);
  bind->asyncRequest("tools/call", &RelayServer::ToolsCall, this);
  bind->request("resources/list", &RelayServer::ResourcesList, this);
  bind->request("resources/read", &RelayServer::ResourcesRead, this);

  // lldb-mcp --server <-> lldb communication
  bind->notification("_debuggersChanged", &RelayServer::OnDebuggersChanged,
                     this, conn_p);
  it.first->second.Evaluate =
      bind->outgoingRequest<EvaluateParams, ToolsCallResult>("_evaluate");

  bind->disconnected(&RelayServer::OnDisconnect, this);

  auto handle = it.first->second.transport->RegisterMessageHandler(loop, *bind);
  if (!handle)
    return consumeError(handle.takeError());

  it.first->second.handle = std::move(*handle);
}

void RelayServer::OnDisconnect(MCPTransport *conn) {
  std::lock_guard<std::mutex> guard(m_clients_mutex);
  assert(m_clients.find(conn) != m_clients.end() && "client not found");
  m_clients.erase(conn);

  if (m_clients.empty())
    m_loop.AddPendingCallback(
        [](MainLoopBase &m_loop) -> void { m_loop.RequestTermination(); });
}

void RelayServer::OnDebuggersChanged(const DebuggersChangedParams &params,
                                     Transport *conn) {
  std::lock_guard<std::mutex> guard(m_clients_mutex);
  m_clients[conn].debuggers = params.debuggers;
}

Expected<InitializeResult> RelayServer::Initialize(const InitializeParams &) {
  InitializeResult result;
  result.protocolVersion = "2025-06-18";
  result.capabilities.supportsToolsList = true;
  result.capabilities.supportsResourcesList = true;
  result.serverInfo.name = "lldb-mcp";
  result.serverInfo.version = "0.1.0";
  return result;
}

Expected<ToolsListResult> RelayServer::ToolsList(const Void &) {
  ToolsListResult result;
  for (const auto &tool : m_tools)
    result.tools.push_back(tool.second->GetDefinition());
  return result;
}

void RelayServer::ToolsCall(
    const ToolsCallParams &params,
    llvm::unique_function<void(llvm::Expected<ToolsCallResult>)> reply) {
  ToolsCallResult result;
  auto it = m_tools.find(params.name);
  if (it == m_tools.end())
    return reply(make_error<MCPError>("tool not found"));
  it->second->Call(params.arguments, std::move(reply));
}

Expected<ResourcesListResult> RelayServer::ResourcesList(const Void &) {
  std::lock_guard<std::mutex> guard(m_clients_mutex);
  ResourcesListResult result;
  for (const auto &[conn, client] : m_clients) {
    for (const auto &debugger : client.debuggers) {
      // lldb://session/<uuid>/<debugger-id>(/<target-id>)?
      std::string base_uri = "lldb://session/" + client.uuid.GetAsString() +
                             "/" + std::to_string(debugger.id);
      result.resources.emplace_back(Resource{
          base_uri,
          "Debugger",
          "LLDB Debugger Session 1",
          "",
      });

      for (const auto &target : debugger.targets) {
        result.resources.emplace_back(Resource{
            base_uri + "/" + std::to_string(target.id),
            "Target",
            "Debugger target",
            "",
        });
      }
    }
  }
  return result;
}
Expected<ResourcesReadResult>
RelayServer::ResourcesRead(const ResourcesReadParams &params) {
  ResourcesReadResult result;
  result.contents.emplace_back(ResourceContents{
      params.URI,
      "Debugger instance....",
      "text/plain",
  });
  return result;
}

void RelayServer::EvaluateForClient(const lldb_private::UUID &uuid,
                                    const EvaluateParams &params,
                                    Reply<ToolsCallResult> reply) {
  std::lock_guard<std::mutex> guard(m_clients_mutex);
  for (auto &[_, client] : m_clients)
    if (client.uuid == uuid)
      return client.Evaluate(params, std::move(reply));
  ToolsCallResult result;
  result.isError = true;
  result.content = {{"not found"}};
  reply(result);
}

} // namespace lldb_protocol::mcp
