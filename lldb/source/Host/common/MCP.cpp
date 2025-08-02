//===-- MCP.cpp -----------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Host/MCP.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/Socket.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/Status.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Threading.h"
#include <thread>
#include <variant>

using namespace llvm;

namespace lldb_private::mcp {
namespace protocol {
static bool mapRaw(const json::Value &Params, StringLiteral Prop,
                   std::optional<json::Value> &V, json::Path P) {
  const auto *O = Params.getAsObject();
  if (!O) {
    P.report("expected object");
    return false;
  }
  const json::Value *E = O->get(Prop);
  if (E)
    V = std::move(*E);
  return true;
}

json::Value toJSON(const Request &R) {
  json::Object Result{{"jsonrpc", "2.0"}, {"id", R.id}, {"method", R.method}};
  if (R.params)
    Result.insert({"params", R.params});
  return Result;
}

bool fromJSON(const json::Value &V, Request &R, json::Path P) {
  json::ObjectMapper O(V, P);
  if (!O || !O.map("id", R.id) || !O.map("method", R.method))
    return false;
  return mapRaw(V, "params", R.params, P);
}

json::Value toJSON(const Error &E) {
  json::Object details{{"code", E.code}, {"message", E.message}};
  if (E.data)
    details.insert({"data", E.data});
  return json::Object{
      {"jsonrpc", "2.0"}, {"id", E.id}, {"error", std::move(details)}};
}

bool fromJSON(const json::Value &V, Error &E, json::Path P) {
  json::ObjectMapper O(V, P);
  if (!O || !O.map("id", E.id))
    return false;
  const json::Value *details = V.getAsObject()->get("error");
  if (!details || details->kind() != json::Value::Kind::Object) {
    P.field("error").report("expected object");
    return false;
  }
  json::ObjectMapper OD(*details, P.field("error"));
  return OD.map("code", E.code) && OD.map("message", E.message) &&
         mapRaw(*details, "data", E.data, P.field("error"));
}

json::Value toJSON(const Response &R) {
  json::Object Result{{"jsonrpc", "2.0"}, {"id", R.id}};
  if (R.result)
    Result.insert({"result", R.result});
  if (R.error)
    Result.insert({"error", R.error});
  return Result;
}

bool fromJSON(const json::Value &V, Response &R, json::Path P) {
  json::ObjectMapper O(V, P);
  if (!O || !O.map("id", R.id) || !O.map("error", R.error))
    return false;
  return mapRaw(V, "result", R.result, P);
}

json::Value toJSON(const Notification &N) {
  json::Object Result{{"jsonrpc", "2.0"}, {"method", N.method}};
  if (N.params)
    Result.insert({"params", N.params});
  return Result;
}

bool fromJSON(const json::Value &V, Notification &N, json::Path P) {
  json::ObjectMapper O(V, P);
  if (!O || !O.map("method", N.method))
    return false;
  auto *Obj = V.getAsObject();
  if (!Obj)
    return false;
  if (auto *Params = Obj->get("params"))
    N.params = *Params;
  return true;
}

json::Value toJSON(const ToolCapability &TC) {
  return json::Object{{"listChanged", TC.listChanged}};
}

bool fromJSON(const json::Value &V, ToolCapability &TC, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("listChanged", TC.listChanged);
}

json::Value toJSON(const Capabilities &C) {
  return json::Object{{"tools", C.tools}};
}

bool fromJSON(const json::Value &V, Capabilities &C, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("tools", C.tools);
}

json::Value toJSON(const TextContent &TC) {
  return json::Object{{"type", "text"}, {"text", TC.text}};
}

bool fromJSON(const json::Value &V, TextContent &TC, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("text", TC.text);
}

json::Value toJSON(const TextResult &TR) {
  return json::Object{{"content", TR.content}, {"isError", TR.isError}};
}

bool fromJSON(const json::Value &V, TextResult &TR, json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("content", TR.content) && O.map("isError", TR.isError);
}

json::Value toJSON(const ToolDefinition &TD) {
  json::Object Result{{"name", TD.name}};
  if (!TD.title.empty())
    Result.insert({"title", TD.title});
  if (!TD.description.empty())
    Result.insert({"description", TD.description});
  if (TD.inputSchema)
    Result.insert({"inputSchema", TD.inputSchema});
  return Result;
}

bool fromJSON(const json::Value &V, ToolDefinition &TD, json::Path P) {
  json::ObjectMapper O(V, P);
  if (!O || !O.map("name", TD.name) ||
      !O.mapOptional("description", TD.description))
    return false;
  return mapRaw(V, "inputSchema", TD.inputSchema, P);
}

json::Value toJSON(const Message &M) {
  return std::visit([](auto &M) { return toJSON(M); }, M);
}

bool fromJSON(const json::Value &V, Message &M, json::Path P) {
  const auto *O = V.getAsObject();
  if (!O) {
    P.report("expected object");
    return false;
  }

  if (const json::Value *V = O->get("jsonrpc")) {
    if (V->getAsString().value_or("") != "2.0") {
      P.report("unsupported JSON RPC version");
      return false;
    }
  } else {
    P.report("not a valid JSON RPC message");
    return false;
  }

  // A message without an ID is a Notification.
  if (!O->get("id")) {
    protocol::Notification N;
    if (!fromJSON(V, N, P))
      return false;
    M = std::move(N);
    return true;
  }

  if (O->get("error")) {
    protocol::Error E;
    if (!fromJSON(V, E, P))
      return false;
    M = std::move(E);
    return true;
  }

  if (O->get("result")) {
    protocol::Response R;
    if (!fromJSON(V, R, P))
      return false;
    M = std::move(R);
    return true;
  }

  if (O->get("method")) {
    protocol::Request R;
    if (!fromJSON(V, R, P))
      return false;
    M = std::move(R);
    return true;
  }

  P.report("unrecognized message type");
  return false;
}

} // namespace protocol

char MCPError::ID;

std::vector<std::string> Transport::consumeMessages() {
  std::vector<std::string> lines;
  for (std::string::size_type pos;
       (pos = m_buffer.find('\n')) != std::string::npos;) {
    lines.emplace_back(m_buffer.data(), pos);
    m_buffer = m_buffer.erase(0, pos + 1);
  }
  return lines;
}

Status Transport::Write(llvm::StringRef data) {
  if (!m_io)
    return Status(std::make_error_code(std::errc::not_connected));

  size_t length = data.size();
  return m_io->Write(data.data(), length);
}

Status Transport::Read() { return Read(m_buffer); }

Status Transport::Read(std::string &buffer) {
  if (!m_io)
    return Status(std::make_error_code(std::errc::not_connected));

  char rbuf[kChunkSize];
  size_t bytes_read = sizeof(rbuf);
  Status status = m_io->Read(rbuf, bytes_read);
  if (status.Fail())
    return status;

  if (bytes_read == 0) {
    m_eof = true;
  }

  buffer += std::string(rbuf, bytes_read);
  return Status();
}

StringRef Transport::CommunicationSocketPath() {
  static once_flag f;
  static SmallString<256> socket_path;
  llvm::call_once(f, [] {
    assert(sys::path::home_directory(socket_path) &&
           "failed to get home directory");
    sys::path::append(socket_path, ".lldb-mcp-sock");
  });
  return socket_path.str();
}

static Expected<sys::ProcessInfo> StartServer() {
  static once_flag f;
  static FileSpec candidate;
  llvm::call_once(f, [] {
    HostInfo::Initialize();
    candidate = HostInfo::GetSupportExeDir();
    candidate.AppendPathComponent("lldb-mcp");
  });

  if (!FileSystem::Instance().Exists(candidate))
    return createStringError("lldb-mcp executable not found");
  std::vector<StringRef> args = {candidate.GetPath(), "--server"};
  sys::ProcessInfo proc =
      sys::ExecuteNoWait(candidate.GetPath(), args, std::nullopt, {}, 0,
                         nullptr, nullptr, nullptr, /*DetachProcess=*/true);
  if (proc.Pid == sys::ProcessInfo::InvalidPid)
    return createStringError("Failed to start server: " + candidate.GetPath());
  StringRef socket_path = Transport::CommunicationSocketPath();
  while (!sys::fs::exists(socket_path))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return proc;
}

Expected<TransportUP> Transport::Connect(std::string client_name) {
  StringRef socket_path = CommunicationSocketPath();
  if (!sys::fs::exists(socket_path))
    if (Error err = StartServer().takeError())
      return err;

  Socket::SocketProtocol protocol = Socket::ProtocolUnixDomain;
  Status error;
  auto socket = Socket::Create(protocol, error);
  if (error.Fail())
    return error.takeError();

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    Status error = socket->Connect(socket_path);
    if (error.Success())
      return std::make_unique<Transport>(std::move(socket), client_name);
    if (error.Fail() && error.GetError() != ECONNREFUSED &&
        error.GetError() != ENOENT)
      return error.takeError();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  return createStringError("failed to connect to lldb-mcp multiplexer");
}

json::Value toJSON(const CheckInNotification &C) {
  return json::Object{{"debuggers", C.debuggers}};
}

bool fromJSON(const llvm::json::Value &V, CheckInNotification &C,
              llvm::json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("debuggers", C.debuggers);
}

Status Client::Notify(CheckInNotification note) {
  protocol::Notification notification;
  notification.method = "$/checkin";
  notification.params = std::move(note);
  return Write(notification);
}

Status Client::Write(const protocol::Message &M) {
  std::string Output;
  llvm::raw_string_ostream OS(Output);
  OS << llvm::formatv("{0}", toJSON(M)) << '\n';
  return m_conn->Write(Output);
}

} // namespace lldb_private::mcp

void llvm::format_provider<lldb_private::mcp::Transport>::format(
    const lldb_private::mcp::Transport &T, raw_ostream &OS, StringRef Options) {
  OS << T;
}
