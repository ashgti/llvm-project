//===-- MCP.h -------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_HOST_MCP_H
#define LLDB_HOST_MCP_H

#include "lldb/Utility/IOObject.h"
#include "lldb/Utility/UUID.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-types.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <string>

namespace lldb_private::mcp {

namespace protocol {
//===----------------------------------------------------------------------===//
//
// This file contains POD structs based on the MCP specification at
// https://github.com/modelcontextprotocol/modelcontextprotocol/blob/main/schema/2024-11-05/schema.json
//
//===----------------------------------------------------------------------===//

/// A request that expects a response.
struct Request {
  uint64_t id = 0;
  std::string method;
  std::optional<llvm::json::Value> params;
};
llvm::json::Value toJSON(const Request &R);
bool fromJSON(const llvm::json::Value &V, Request &R, llvm::json::Path P);

struct Error {
  uint64_t id = 0;
  int64_t code = 0;
  std::string message;
  std::optional<llvm::json::Value> data;
};
llvm::json::Value toJSON(const Error &R);
bool fromJSON(const llvm::json::Value &V, Error &R, llvm::json::Path P);

// Standard JSON-RPC error codes
enum class ErrorCode : int64_t {
  parseError = -32700,
  invalidRequest = -32600,
  methodNotFound = -32601,
  invalidParams = -32602,
  internalError = -32603,
};

struct Response {
  uint64_t id = 0;
  std::optional<llvm::json::Value> result;
  std::optional<Error> error;
};
llvm::json::Value toJSON(const Response &R);
bool fromJSON(const llvm::json::Value &V, Response &R, llvm::json::Path P);

/// A notification which does not expect a response.
struct Notification {
  std::string method;
  std::optional<llvm::json::Value> params;
};
llvm::json::Value toJSON(const Notification &N);
bool fromJSON(const llvm::json::Value &V, Notification &N, llvm::json::Path P);

using Message = std::variant<Request, Response, Notification, Error>;
llvm::json::Value toJSON(const Message &M);
bool fromJSON(const llvm::json::Value &V, Message &M, llvm::json::Path P);
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Message &M) {
  OS << toJSON(M);
  return OS;
}

struct ToolCapability {
  /// Whether this server supports notifications for changes to the tool list.
  bool listChanged = false;
};
llvm::json::Value toJSON(const ToolCapability &TC);
bool fromJSON(const llvm::json::Value &V, ToolCapability &TC,
              llvm::json::Path P);

/// Capabilities that a server may support. Known capabilities are defined here,
/// in this schema, but this is not a closed set: any server can define its own,
/// additional capabilities.
struct Capabilities {
  /// Present if the server offers any tools to call.
  ToolCapability tools;
};
llvm::json::Value toJSON(const Capabilities &C);
bool fromJSON(const llvm::json::Value &V, Capabilities &C, llvm::json::Path P);

/// Text provided to or from an LLM.
struct TextContent {
  /// The text content of the message.
  std::string text;
};
llvm::json::Value toJSON(const TextContent &TC);
bool fromJSON(const llvm::json::Value &V, TextContent &TC, llvm::json::Path P);

struct TextResult {
  std::vector<TextContent> content;
  bool isError = false;
};
llvm::json::Value toJSON(const TextResult &TR);
bool fromJSON(const llvm::json::Value &V, TextResult &TR, llvm::json::Path P);

struct ToolDefinition {
  /// Unique identifier for the tool.
  std::string name;

  /// Human-readable name of the tool for display purposes.
  std::string title;

  /// Human-readable description.
  std::string description;

  // JSON Schema for the tool's parameters.
  std::optional<llvm::json::Value> inputSchema;
};
llvm::json::Value toJSON(const ToolDefinition &TD);
bool fromJSON(const llvm::json::Value &V, ToolDefinition &TD,
              llvm::json::Path P);

using ToolArguments = std::variant<std::monostate, llvm::json::Value>;

} // namespace protocol

class MCPError : public llvm::ErrorInfo<MCPError> {
public:
  static char ID;

  MCPError(std::string message, int64_t error_code)
      : m_message(message), m_error_code(error_code) {}

  void log(llvm::raw_ostream &OS) const override { OS << m_message; }
  std::error_code convertToErrorCode() const override {
    return llvm::inconvertibleErrorCode();
  }

  const int64_t &getCode() const { return m_error_code; }
  const std::string &getMessage() const { return m_message; }

private:
  std::string m_message;
  int64_t m_error_code;
};

class Transport;
using TransportUP = std::unique_ptr<Transport>;

class Transport {
public:
  Transport(lldb::IOObjectSP io, std::string name)
      : m_io(std::move(io)), m_name(std::move(name)) {}
  ~Transport() = default;

  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;

  const std::string GetName() const {
    return m_name.empty() ? "<nil>" : m_name;
  }
  const lldb::IOObjectSP &GetIO() const { return m_io; }
  bool IsEOF() const { return m_eof; };

  static llvm::StringRef CommunicationSocketPath();
  // static llvm::Expected<llvm::sys::ProcessInfo> StartServer();
  static llvm::Expected<TransportUP> Connect(std::string client_name);

  std::vector<std::string> consumeMessages();
  Status Write(llvm::StringRef data);
  Status Read();
  Status Read(std::string &);

private:
  lldb::IOObjectSP m_io;
  std::string m_name;
  // FIXME: This is temporay until my other refactor is in for JSONTransport.
  std::string m_buffer;
  bool m_eof = false;

  constexpr static size_t kChunkSize = 4 * 1024; // page size, 4 KiB
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const Transport &T) {
  OS << "Transport(" << &T << ", " << T.GetName() << ")";
  return OS;
}

/// Notification from lldb <-> lldb-mcp for registering debuggers.
struct CheckInNotification {
  /// A list of debuggers.
  std::vector<lldb::user_id_t> debuggers;
};
llvm::json::Value toJSON(const CheckInNotification &);
bool fromJSON(const llvm::json::Value &, CheckInNotification &,
              llvm::json::Path);

class Client final {
public:
  Client(TransportUP conn) : m_conn(std::move(conn)) {}
  ~Client() = default;

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  const lldb::IOObjectSP &GetIO() const { return m_conn->GetIO(); }
  Transport *GetConn() { return m_conn.get(); };
  Transport *GetConn() const { return m_conn.get(); };

  Status Notify(CheckInNotification note);

protected:
  Status Write(const protocol::Message &);

private:
  TransportUP m_conn;
};
using ClientUP = std::unique_ptr<Client>;

} // namespace lldb_private::mcp

template <> struct ::llvm::format_provider<lldb_private::mcp::Transport> {
  static void format(const lldb_private::mcp::Transport &C, raw_ostream &OS,
                     StringRef Options);
};

#endif
