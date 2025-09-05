//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_PROTOCOL_MCP_TRANSPORT_H
#define LLDB_PROTOCOL_MCP_TRANSPORT_H

#include "lldb/Host/JSONTransport.h"
#include "lldb/Protocol/MCP/MCPError.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace lldb_protocol::mcp {

/// Generic transport that uses the MCP protocol.
using MCPTransport = lldb_private::Transport<Request, Response, Notification>;

/// Generic logging callback, to allow the MCP server / client / transport layer
/// to be independent of the lldb log implementation.
using LogCallback = llvm::unique_function<void(llvm::StringRef message)>;

class Transport final
    : public lldb_private::JSONRPCTransport<Request, Response, Notification> {
public:
  Transport(lldb::IOObjectSP in, lldb::IOObjectSP out,
            LogCallback log_callback = {});
  virtual ~Transport() = default;

  /// Transport is not copyable.
  /// @{
  Transport(const Transport &) = delete;
  void operator=(const Transport &) = delete;
  /// @}

  void Log(llvm::StringRef message) override;

private:
  LogCallback m_log_callback;
};

} // namespace lldb_protocol::mcp

namespace lldb_private {

template <>
inline llvm::json::Value get_params(const lldb_protocol::mcp::Request &req) {
  return req.params;
}

template <>
inline llvm::Expected<llvm::json::Value>
get_result<lldb_protocol::mcp::Response>(
    lldb_protocol::mcp::Response const &resp) {
  if (const auto *err = std::get_if<lldb_protocol::mcp::Error>(&resp.result))
    return llvm::make_error<lldb_protocol::mcp::MCPError>(err->message,
                                                          err->code);
  return std::get<llvm::json::Value>(resp.result);
}

template <>
inline llvm::json::Value
get_params(const lldb_protocol::mcp::Notification &note) {
  return note.params;
}

template <>
inline lldb_protocol::mcp::Response
make_response(const lldb_protocol::mcp::Request &req, llvm::Error err) {
  lldb_protocol::mcp::Response resp;
  resp.id = req.id;
  resp.result =
      lldb_protocol::mcp::Error{lldb_protocol::mcp::eErrorCodeInternalError,
                                llvm::toString(std::move(err))};
  return resp;
}

template <>
inline lldb_protocol::mcp::Response
make_response(const lldb_protocol::mcp::Request &req,
              llvm::json::Value result) {
  lldb_protocol::mcp::Response resp;
  resp.id = req.id;
  resp.result = std::move(result);
  return resp;
}

template <>
inline lldb_protocol::mcp::Request
make_request(int64_t id, llvm::StringRef method,
             const std::optional<llvm::json::Value> &params) {
  return lldb_protocol::mcp::Request{id, method.str(), params};
}

template <>
inline lldb_protocol::mcp::Notification
make_event(llvm::StringRef method,
           std::optional<llvm::json::Value> const &params) {
  return lldb_protocol::mcp::Notification{method.str(), params};
}

template <> inline int64_t get_id(const lldb_protocol::mcp::Request &req) {
  return std::get<int64_t>(req.id);
}

template <> inline int64_t get_id(const lldb_protocol::mcp::Response &resp) {
  return std::get<int64_t>(resp.id);
}

template <>
inline llvm::StringRef
get_method(const lldb_protocol::mcp::Notification &note) {
  return note.method;
}

template <>
inline llvm::StringRef get_method(const lldb_protocol::mcp::Request &req) {
  return req.method;
}

} // namespace lldb_private

#endif
