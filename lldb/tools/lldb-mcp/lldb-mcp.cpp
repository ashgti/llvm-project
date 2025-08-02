//===-- lldb-mcp.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBDebugger.h"
#include "lldb/Host/File.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/MCP.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Host/MainLoopBase.h"
#include "lldb/Host/Socket.h"
#include "lldb/Utility/IOObject.h"
#include "lldb/Utility/Status.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-types.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

using namespace llvm;
using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::mcp;

static bool g_server = false;
static raw_ostream &logger() {
  static raw_fd_ostream *S;
  static once_flag flag;
  llvm::call_once(flag, []() {
    int fd;
    std::error_code EC = sys::fs::openFile(
        g_server ? "/tmp/lldb-mcp-server.log" : "/tmp/lldb-mcp-client.log", fd,
        sys::fs::CD_CreateAlways, llvm::sys::fs::FA_Write,
        sys::fs::OpenFlags::OF_None);
    assert(!EC && "Failed to open log file");
    S = new raw_fd_ostream(fd, false, true);
  });
  return *S;
}

namespace {

class Server;

struct Context {
  Transport *conn;
  Server *server;
};

class Tool {
public:
  Tool(std::string name, std::string title = "", std::string description = "")
      : m_name(name), m_title(title), m_description(description) {}
  virtual ~Tool() = default;

  virtual llvm::Expected<protocol::TextResult>
  Call(Context &, const protocol::ToolArguments &args) = 0;

  virtual std::optional<llvm::json::Value> GetSchema() const {
    return llvm::json::Object{{"type", "object"}};
  }

  protocol::ToolDefinition GetDefinition() const {
    protocol::ToolDefinition definition;
    definition.name = m_name;
    definition.title = m_title;
    definition.description = m_description;

    if (std::optional<llvm::json::Value> input_schema = GetSchema())
      definition.inputSchema = *input_schema;

    return definition;
  }

  const std::string &GetName() { return m_name; }

private:
  std::string m_name;
  std::string m_title;
  std::string m_description;
};
using ToolUP = std::unique_ptr<Tool>;

struct CommandArguments {
  std::string debugger_uri;
  std::string command;
};
json::Value toJSON(const CommandArguments &C) {
  return json::Object{{"debugger_uri", C.debugger_uri}, {"command", C.command}};
}
bool fromJSON(const json::Value &V, CommandArguments &C, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("debugger_uri", C.debugger_uri) &&
         O.map("command", C.command);
}

class CommandTool : public Tool {
public:
  using Tool::Tool;
  ~CommandTool() = default;

  Expected<protocol::TextResult>
  Call(Context &ctx, const protocol::ToolArguments &args) override;

  std::optional<json::Value> GetSchema() const override {
    using namespace json;
    Object properties{{"debugger_uri", Object{{"type", "string"}}},
                      {"command", Object{{"type", "string"}}}};
    Array required{"debugger_uri"};
    Object schema{{"type", "object"},
                  {"properties", std::move(properties)},
                  {"required", std::move(required)}};
    return schema;
  }
};

class DebuggerListTool : public Tool {
public:
  using Tool::Tool;
  ~DebuggerListTool() = default;

  Expected<protocol::TextResult>
  Call(Context &ctx, const protocol::ToolArguments &args) override;
};

struct LaunchArgs {
  std::string program;
  std::vector<std::string> args;
};
bool fromJSON(const json::Value &V, LaunchArgs &A, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("program", A.program) && O.mapOptional("args", A.args);
}

/// Helper function to create a TextResult from a string output.
static protocol::TextResult createTextResult(std::string output,
                                             bool is_error = false) {
  protocol::TextResult text_result;
  text_result.content.emplace_back(protocol::TextContent{{std::move(output)}});
  text_result.isError = is_error;
  return text_result;
}

const StringLiteral lldbBinary = "/Users/harjohn/Projects/lldb-build/bin/lldb";

class LaunchDebuggerTool : public Tool {
public:
  using Tool::Tool;
  ~LaunchDebuggerTool() = default;

  Expected<protocol::TextResult>
  Call(Context &ctx, const protocol::ToolArguments &args) override {
    if (!std::holds_alternative<json::Value>(args))
      return createStringError("launch requires arguments");

    json::Path::Root root;

    LaunchArgs arguments;
    if (!fromJSON(std::get<json::Value>(args), arguments, root))
      return root.getError();

    std::vector<StringRef> launch_args = {lldbBinary, arguments.program};
    if (!arguments.args.empty()) {
      launch_args.insert(launch_args.end(), arguments.args.begin(),
                         arguments.args.end());
    }
    sys::ProcessInfo info =
        sys::ExecuteNoWait(lldbBinary, launch_args, std::nullopt);

    std::string output;
    raw_string_ostream os(output);
    os << "Launched lldb with PID: " << info.Pid << "\n";
    return createTextResult(output);
  }

  std::optional<json::Value> GetSchema() const override {
    using namespace json;
    Object properties{
        {"program", Object{{"type", "string"},
                           {"description", "Path to the program to debug."}}},
        {"arguments", Object{{"type", "array"},
                             {"description", "Program arguments."},
                             {"items", Object{{"type", "string"}}}}}};
    Array required{"program"};
    Object schema{{"type", "object"},
                  {"properties", std::move(properties)},
                  {"required", std::move(required)}};
    return schema;
  }
};

class Resource {
public:
  Resource(std::string uri, std::string name, std::string title = "",
           std::string description = "", std::string mime_type = "",
           size_t size = 0)
      : m_uri(uri), m_name(name), m_title(title), m_description(description),
        m_mime_type(mime_type), m_size(size) {}
  virtual ~Resource() = default;

  json::Value toJSON() const {
    json::Object obj{
        {"uri", m_uri},
        {"name", m_name},
        {"title", m_title},
        {"description", m_description},
        {"mimeType", m_mime_type},
        {"size", m_size},
    };
    return obj;
  }

private:
  std::string m_uri;
  std::string m_name;
  std::string m_title = "";
  std::string m_description = "";
  std::string m_mime_type = "";
  size_t m_size = 0;
};

class Server {
  unsigned client_count = 0;

  using ConnectionList = std::vector<TransportUP>;
  struct LLDBInstance;
  ConnectionList m_conns;
  MainLoop m_loop;

  static constexpr StringLiteral kProtocolVersion = "2024-11-05";
  static constexpr StringLiteral kServerName = "lldb-mcp";
  static constexpr StringLiteral kServerVersion = "0.0.1";

public:
  protocol::Capabilities GetCapabilities() {
    protocol::Capabilities capabilities;
    capabilities.tools.listChanged = true;
    return capabilities;
  }

  llvm::Expected<protocol::Response>
  PingHandler(Transport &conn, const protocol::Request &request) {
    return protocol::Response{request.id, json::Object{}, std::nullopt};
  }

  void DebuggerCheckin(Transport &conn, const protocol::Notification &note) {
    std::scoped_lock<std::mutex> guard(m_server_mutex);
    CheckInNotification checkin;
    json::Path::Root root;
    if (!fromJSON(note.params, checkin, root))
      return;
    LLDBInstance inst;
    inst.conn = &conn;
    inst.debuggers = checkin.debuggers;
    m_instances.emplace_back(std::move(inst));
  }

  llvm::Expected<protocol::Response>
  InitializeHandler(Transport &conn, const protocol::Request &req) {
    logger() << conn << " Initialize request received: " << req << "\n";
    protocol::Response response;
    response.result.emplace(llvm::json::Object{
        {"protocolVersion", kProtocolVersion},
        {"capabilities", GetCapabilities()},
        {"serverInfo", llvm::json::Object{{"name", kServerName},
                                          {"version", kServerVersion}}}});
    return response;
  }

  llvm::Expected<protocol::Response>
  PromptsListHandler(Transport &conn, const protocol::Request &request) {
    protocol::Response response;
    response.result = json::Object{{"prompts", json::Array{}}};
    return response;
  }

  llvm::Expected<protocol::Response>
  ToolsListHandler(Transport &conn, const protocol::Request &request) {
    protocol::Response response;

    llvm::json::Array tools;
    for (const auto &tool : m_tools)
      tools.emplace_back(toJSON(tool.second->GetDefinition()));

    response.result.emplace(llvm::json::Object{{"tools", std::move(tools)}});

    return response;
  }

  llvm::Expected<protocol::Response>
  ToolsCallHandler(Transport &conn, const protocol::Request &request) {
    protocol::Response response;

    if (!request.params)
      return llvm::createStringError("no tool parameters");

    const json::Object *param_obj = request.params->getAsObject();
    if (!param_obj)
      return llvm::createStringError("no tool parameters");

    const json::Value *name = param_obj->get("name");
    if (!name)
      return llvm::createStringError("no tool name");

    llvm::StringRef tool_name = name->getAsString().value_or("");
    if (tool_name.empty())
      return llvm::createStringError("no tool name");

    auto it = m_tools.find(tool_name);
    if (it == m_tools.end())
      return llvm::createStringError(
          llvm::formatv("no tool \"{0}\"", tool_name));

    protocol::ToolArguments tool_args;
    if (const json::Value *args = param_obj->get("arguments"))
      tool_args = *args;

    Context ctx{&conn, this};
    llvm::Expected<protocol::TextResult> text_result =
        it->second->Call(ctx, tool_args);
    if (!text_result)
      return text_result.takeError();

    response.result.emplace(toJSON(*text_result));

    return response;
  }

  Server() {
    /// Internal multiplexer operation.
    AddNotificationHandler("$/checkin", std::bind(&Server::DebuggerCheckin,
                                                  this, std::placeholders::_1,
                                                  std::placeholders::_2));

    AddRequestHandler("initialize",
                      std::bind(&Server::InitializeHandler, this,
                                std::placeholders::_1, std::placeholders::_2));
    AddRequestHandler("ping",
                      std::bind(&Server::PingHandler, this,
                                std::placeholders::_1, std::placeholders::_2));
    AddRequestHandler("prompts/list",
                      std::bind(&Server::PromptsListHandler, this,
                                std::placeholders::_1, std::placeholders::_2));
    AddRequestHandler("tools/list",
                      std::bind(&Server::ToolsListHandler, this,
                                std::placeholders::_1, std::placeholders::_2));
    AddRequestHandler("tools/call",
                      std::bind(&Server::ToolsCallHandler, this,
                                std::placeholders::_1, std::placeholders::_2));
    // FIXME: Add 'notifications/cancelled' handler.
    AddNotificationHandler("notifications/initialized",
                           [](Transport &, const protocol::Notification &) {
                             logger() << "MCP initialization complete\n";
                           });
    AddTool(std::make_unique<CommandTool>(
        "lldb_command", "LLDB Debugger Command", "Run an lldb command."));
    AddTool(std::make_unique<DebuggerListTool>(
        "lldb_debugger_list", "LLDB Debug Sessions Provider",
        "List debugger instances with their debugger_id."));
    AddTool(std::make_unique<LaunchDebuggerTool>(
        "lldb_launch", "LLDB Debug Session Launcher",
        "Launch a new debugger instance with a program."));
  }
  ~Server() = default;

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;

  MainLoop &GetLoop() { return m_loop; }

  void onDisconnect(MainLoopBase &loop, Transport &conn) {
    std::lock_guard<std::mutex> guard(m_server_mutex);

    logger() << conn << " disconnected\n";
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
      if (it->get() == &conn) {
        m_conns.erase(it);
        loop.RequestTermination();
        logger() << "Client removed from the list\n";
        break;
      }
    }

    logger() << "Remaining connections: " << m_conns.size() << "\n";

    if (m_conns.empty()) {
      logger() << "No more clients, shutting down server...\n";
      m_loop.RequestTermination();
    } else {
    }
  }

  void onError(MainLoopBase &loop, Transport &conn, Error error) {
    WithColor::error(llvm::errs(), "MCP Server")
        << "Error: " << toString(std::move(error)) << "\n";
    onDisconnect(loop, conn);
  }

  llvm::Expected<protocol::Response> Handle(Transport &conn,
                                            protocol::Request request) {
    logger() << conn << " handling request: " << request.method << "("
             << request.id << ")\n";
    auto it = m_request_handlers.find(request.method);
    if (it != m_request_handlers.end()) {
      llvm::Expected<protocol::Response> response = it->second(conn, request);
      if (!response)
        return response;
      response->id = request.id;
      return *response;
    }

    return make_error<MCPError>(
        llvm::formatv("no handler for request: {0}", request.method).str(), 1);
  }

  void Handle(Transport &conn, protocol::Notification notification) {
    auto it = m_notification_handlers.find(notification.method);
    if (it != m_notification_handlers.end()) {
      it->second(conn, notification);
      return;
    }

    logger() << conn << " MPC notification: " << notification.method << " "
             << notification.params << "\n";
  }

  void Handle(Transport &conn, protocol::Response response) {
    std::lock_guard<std::mutex> guard(m_server_mutex);
    for (auto it = m_response_handlers.begin(); it != m_response_handlers.end();
         ++it)
      if (it->first.id == response.id) {
        it->second.set_value(response);
        m_response_handlers.erase(it);
      }
  }

  llvm::Expected<std::optional<protocol::Message>>
  HandleData(MainLoopBase &loop, Transport &conn, llvm::StringRef data) {
    auto message = llvm::json::parse<protocol::Message>(/*JSON=*/data);
    if (!message)
      return message.takeError();

    logger() << "<-- " << conn << " " << *message << "\n";

    if (const protocol::Request *request =
            std::get_if<protocol::Request>(&(*message))) {
      llvm::Expected<protocol::Response> response = Handle(conn, *request);

      // Handle failures by converting them into an Error message.
      if (!response) {
        protocol::Error protocol_error;
        llvm::handleAllErrors(
            response.takeError(),
            [&](const MCPError &err) {
              protocol_error.code = err.getCode();
              protocol_error.message = err.getMessage();
            },
            [&](const llvm::ErrorInfoBase &err) {
              protocol_error.code = -1;
              protocol_error.message = err.message();
            });
        protocol_error.id = request->id;
        return protocol_error;
      }

      return *response;
    }

    if (const protocol::Notification *notification =
            std::get_if<protocol::Notification>(&(*message))) {
      Handle(conn, *notification);
      return std::nullopt;
    }

    if (const protocol::Response *response =
            std::get_if<protocol::Response>(&(*message))) {
      Handle(conn, *response);
    }

    if (std::get_if<protocol::Error>(&(*message)))
      return llvm::createStringError("unexpected MCP message: error");

    llvm_unreachable("all message types handled");
  }

  void onData(MainLoopBase &loop, Transport &conn) {
    logger() << conn << " received data from client\n";

    if (Status status = conn.Read(); status.Fail())
      return onError(loop, conn, status.takeError());

    for (const auto &line : conn.consumeMessages()) {
      llvm::Expected<std::optional<protocol::Message>> message =
          HandleData(loop, conn, line);
      if (!message)
        return onError(loop, conn, message.takeError());

      if (*message) {
        logger() << "--> " << conn << " " << **message << "\n";
        std::string output;
        llvm::raw_string_ostream OS(output);
        OS << llvm::formatv("{0}", toJSON(**message)) << '\n';
        if (Status status = conn.Write(output); status.Fail())
          return onError(loop, conn, status.takeError());
      }
    }

    if (conn.IsEOF())
      return onDisconnect(loop, conn);
  }

  void onConnect(std::unique_ptr<Socket> client_socket) {
    logger() << "New client connected\n";

    std::string name = llvm::formatv("client-{0}", ++client_count).str();
    TransportUP conn_up =
        std::make_unique<Transport>(std::move(client_socket), name);
    Transport *conn = conn_up.get();

    logger() << *conn << " client connected...\n";

    std::lock_guard<std::mutex> guard(m_server_mutex);
    m_conns.emplace_back(std::move(conn_up));
    std::thread thr([this, conn]() {
      Status status;
      MainLoop loop;
      MainLoopBase::ReadHandleUP read_handle = loop.RegisterReadObject(
          conn->GetIO(),
          [this, conn](MainLoopBase &loop) { onData(loop, *conn); }, status);
      if (status.Fail())
        this->onError(loop, *conn, status.takeError());
      status = loop.Run();
      if (status.Fail())
        this->onError(loop, *conn, status.takeError());
    });
    thr.detach();

    logger() << *conn << " registered read handle for client\n";
  }

  // protocol
  using RequestHandler = std::function<llvm::Expected<protocol::Response>(
      Transport &, const protocol::Request &)>;
  using NotificationHandler =
      std::function<void(Transport &, const protocol::Notification &)>;

  void AddTool(ToolUP tool) {
    std::lock_guard<std::mutex> guard(m_server_mutex);

    if (!tool)
      return;
    m_tools[tool->GetName()] = std::move(tool);
  }

  void AddRequestHandler(llvm::StringRef method, RequestHandler handler) {
    std::lock_guard<std::mutex> guard(m_server_mutex);
    m_request_handlers[method] = std::move(handler);
  }

  void AddNotificationHandler(llvm::StringRef method,
                              NotificationHandler handler) {
    std::lock_guard<std::mutex> guard(m_server_mutex);
    m_notification_handlers[method] = std::move(handler);
  }

  std::vector<LLDBInstance> &GetInstances() {
    std::lock_guard<std::mutex> guard(m_server_mutex);
    return m_instances;
  }

  Expected<std::pair<Transport *, lldb::user_id_t>>
  FindDebugger(StringRef debugger_uri) {
    std::lock_guard<std::mutex> guard(m_server_mutex);
    if (!debugger_uri.consume_front("lldb:///debuggers/"))
      return createStringError("malformed uri %s", debugger_uri.data());

    auto [name, raw_id] = debugger_uri.split("/");
    lldb::user_id_t id = 0;
    if (!to_integer(raw_id, id))
      return createStringError("malformed debugger id %lld", id);

    for (const auto &inst : m_instances)
      if (inst.conn->GetName() == name &&
          std::find(inst.debuggers.begin(), inst.debuggers.end(), id) !=
              inst.debuggers.end())
        return std::make_pair(inst.conn, id);

    return createStringError("not found");
  }

  Expected<std::string> Evaluate(Transport *conn, lldb::user_id_t id,
                                 StringRef content) {
    protocol::Request req;
    req.id = ++m_reqs;
    req.method = "$/evaluate";
    req.params = json::Object{{"id", id}, {"content", content}};
    std::string O;
    raw_string_ostream OS(O);
    OS << req;
    if (Status status = conn->Write(O + "\n"); status.Fail())
      return status.takeError();

    std::promise<protocol::Response> response_promise;
    std::future<protocol::Response> response_future =
        response_promise.get_future();
    m_response_handlers.emplace_back(
        std::make_pair(req, std::move(response_promise)));
    protocol::Response resp = response_future.get();
    if (!resp.result)
      return createStringError("Evaluate failed");
    std::optional<StringRef> result = resp.result->getAsString();
    if (!result)
      return createStringError("Evaluate failed");
    return result->str();
  }

private:
  std::mutex m_server_mutex;
  int m_reqs = 0;
  llvm::StringMap<ToolUP> m_tools;
  struct LLDBInstance {
    Transport *conn;
    std::vector<lldb::user_id_t> debuggers;
  };
  std::vector<LLDBInstance> m_instances;

  llvm::StringMap<RequestHandler> m_request_handlers;
  llvm::StringMap<NotificationHandler> m_notification_handlers;
  std::vector<std::pair<protocol::Request, std::promise<protocol::Response>>>
      m_response_handlers;
};

struct Uri {};

llvm::Expected<protocol::TextResult>
CommandTool::Call(Context &ctx, const protocol::ToolArguments &args) {
  if (!std::holds_alternative<json::Value>(args))
    return createStringError("command requires arguments");

  json::Path::Root root;

  CommandArguments arguments;
  if (!fromJSON(std::get<json::Value>(args), arguments, root))
    return root.getError();

  auto maybe_debugger = ctx.server->FindDebugger(arguments.debugger_uri);
  if (!maybe_debugger)
    return protocol::TextResult{{{"Failed to find the debugger " +
                                  toString(maybe_debugger.takeError())}},
                                true};

  auto [conn, id] = *maybe_debugger;
  auto maybe_content = ctx.server->Evaluate(conn, id, arguments.command);
  if (!maybe_content)
    return protocol::TextResult{
        {{"Evaluate failed " + toString(maybe_content.takeError())}}, true};

  return protocol::TextResult{{{*maybe_content}}, false};
}

llvm::Expected<protocol::TextResult>
DebuggerListTool::Call(Context &ctx, const protocol::ToolArguments &args) {
  std::string O;
  raw_string_ostream OS(O);
  OS << "Debuggers available:\n";
  for (const auto &inst : ctx.server->GetInstances())
    for (const auto id : inst.debuggers)
      OS << " *  lldb:///debuggers/" << inst.conn->GetName() << "/" << id
         << "\n";

  return protocol::TextResult{{{O}}, false};
}

} // namespace

namespace {
using namespace llvm::opt;

enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

static constexpr llvm::opt::OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};
class MCPOptTable : public llvm::opt::GenericOptTable {
public:
  MCPOptTable()
      : llvm::opt::GenericOptTable(OptionStrTable, OptionPrefixesTable,
                                   InfoTable, true) {}
};
} // namespace

namespace {

enum class Mode { Server, Client };

std::string tool_name;

struct MCPOptions {
  Mode mode = Mode::Client;
  std::chrono::milliseconds timeout = std::chrono::seconds(30);
};

Error validateAndSetOptions(const llvm::opt::InputArgList &args,
                            MCPOptions &opts) {
  if (args.hasArg(OPT_server)) {
    opts.mode = Mode::Server;
  }

  return Error::success();
}

inline void error(Error Err, StringRef Prefix = "") {
  handleAllErrors(std::move(Err), [&](ErrorInfoBase &Info) {
    WithColor::error(errs(), Prefix) << Info.message() << '\n';
  });
  std::exit(EXIT_FAILURE);
}

Error runServer(const MCPOptions &opts) {
  g_server = true;

  StringRef socket_path = Transport::CommunicationSocketPath();
  Status error;
  auto listener = Socket::Create(Socket::ProtocolUnixDomain, error);
  if (error.Fail())
    return error.takeError();
  error = listener->Listen(socket_path, 5);
  if (error.Fail())
    return error.takeError();

  std::string addresses = join(listener->GetListeningConnectionURI(), ", ");
  logger() << "Listening on " << addresses << "\n";

  Server server;
  MainLoop &loop = server.GetLoop();
  Expected<std::vector<MainLoopBase::ReadHandleUP>> accept_handle =
      listener->Accept(loop, [&](std::unique_ptr<Socket> client) {
        server.onConnect(std::move(client));
      });
  if (Error error = accept_handle.takeError())
    return error;
  error = loop.Run();
  if (error.Fail())
    return error.takeError();

  logger() << "Shutting down server, no remaining clients...\n";

  if (::unlink(socket_path.data()) == -1)
    return createStringError(errnoAsErrorCode(), "unlink failed: %s",
                             socket_path.data());
  return Error::success();
}

Error runStdioClient(const MCPOptions &opts) {
  logger() << "Running stdio <-> lldb-mcp client\n";

  IOObjectSP input = std::make_shared<NativeFile>(
      STDIN_FILENO, NativeFile::OpenOptions::eOpenOptionReadOnly, false);
  IOObjectSP output = std::make_shared<NativeFile>(
      STDOUT_FILENO, NativeFile::OpenOptions::eOpenOptionWriteOnly, false);
  Expected<TransportUP> maybe_conn = Transport::Connect("stdio");
  if (auto Err = maybe_conn.takeError())
    error(std::move(Err), "Failed to connect to server");
  TransportUP conn = std::move(*maybe_conn);

  auto handleStatus = [&](Status status) {
    if (status.Success())
      return;

    WithColor::error(errs(), "lldb-mcp client")
        << "Error: " << toString(status.takeError()) << "\n";
    std::exit(EXIT_FAILURE);
  };

  MainLoop loop;
  Status status;
  MainLoopBase::ReadHandleUP client_to_stdout_handle = loop.RegisterReadObject(
      conn->GetIO(),
      [&](MainLoopBase &loop) {
        std::string buffer;
        handleStatus(conn->Read(buffer));
        size_t len = buffer.size();
        handleStatus(output->Write(buffer.data(), len));

        if (conn->IsEOF()) {
          logger() << "EOF received, disconnecting client\n";
          loop.RequestTermination();
          return;
        }
      },
      status);
  if (status.Fail())
    return status.takeError();

  MainLoopBase::ReadHandleUP stdin_to_client_handle = loop.RegisterReadObject(
      input,
      [&](MainLoopBase &loop) {
        char buf[1024] = {0};
        size_t bytes_read = sizeof(buf);
        handleStatus(input->Read(buf, bytes_read));

        if (bytes_read == 0) {
          // EOF, disconnect.
          logger() << "EOF received, disconnecting client\n";
          loop.RequestTermination();
          return;
        }

        handleStatus(conn->Write(StringRef(buf, bytes_read)));
      },
      status);

  if (Status status = loop.Run(); status.Fail())
    return status.takeError();

  return Error::success();
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  InitLLVM IL(argc, argv, /*InstallPipeSignalExitHandler=*/false);

  llvm::SmallString<256> program_path(argv[0]);
  llvm::sys::fs::make_absolute(program_path);
  tool_name = program_path.str();

  MCPOptTable T;
  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver{A};
  opt::InputArgList args =
      T.parseArgs(argc, argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        errs() << Msg << '\n';
        std::exit(1);
      });

  if (args.hasArg(OPT_help)) {
    T.printHelp(errs(), tool_name.data(),
                "lldb-mcp is a multiplexer for LLDB clients", false);
    return EXIT_SUCCESS;
  }

  if (args.hasArg(OPT_version)) {
    cl::PrintVersionMessage();
    return EXIT_SUCCESS;
  }

  MCPOptions opts;
  if (Error err = validateAndSetOptions(args, opts))
    error(std::move(err), "lldb-mcp");

  lldb::SBError err = lldb::SBDebugger::InitializeWithErrorHandling();
  if (err.Fail())
    return EXIT_FAILURE;

  FileSystem::Initialize();

  switch (opts.mode) {
  case Mode::Server:
    if (Error err = runServer(opts))
      error(std::move(err), "lldb-mcp server");
    break;
  case Mode::Client:
    if (Error err = runStdioClient(opts))
      error(std::move(err), "lldb-mcp client");
    break;
  }

  return EXIT_SUCCESS;
}