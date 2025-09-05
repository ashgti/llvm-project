//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Protocol/MCP/Server.h"
#include "lldb/Host/File.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/JSONTransport.h"
#include "lldb/Protocol/MCP/MCPError.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Transport.h"
#include "llvm/Support/JSON.h"

using namespace llvm;
using namespace lldb_private;
using namespace lldb_protocol::mcp;

json::Value lldb_protocol::mcp::toJSON(const ServerInfo &SM) {
  return json::Object{{"connection_uri", SM.connection_uri}, {"pid", SM.pid}};
}

bool lldb_protocol::mcp::fromJSON(const json::Value &V, ServerInfo &SM,
                                  json::Path P) {
  json::ObjectMapper O(V, P);
  return O && O.map("connection_uri", SM.connection_uri) &&
         O.map("pid", SM.pid);
}

llvm::Error ServerInfo::Write(const ServerInfo &info) {
  std::string buf = formatv("{0}", toJSON(info)).str();
  size_t num_bytes = buf.size();

  FileSpec user_lldb_dir = HostInfo::GetUserLLDBDir();

  Status error(llvm::sys::fs::create_directory(user_lldb_dir.GetPath()));
  if (error.Fail())
    return error.takeError();

  FileSpec mcp_registry_entry_path = user_lldb_dir.CopyByAppendingPathComponent(
      formatv("lldb-mcp-{0}.json", getpid()).str());

  const File::OpenOptions flags = File::eOpenOptionWriteOnly |
                                  File::eOpenOptionCanCreate |
                                  File::eOpenOptionTruncate;
  llvm::Expected<lldb::FileUP> file = FileSystem::Instance().Open(
      mcp_registry_entry_path, flags, lldb::eFilePermissionsFileDefault, false);
  if (!file)
    return file.takeError();
  if (llvm::Error error = (*file)->Write(buf.data(), num_bytes).takeError())
    return error;
  return llvm::Error::success();
}

llvm::Expected<std::vector<ServerInfo>> ServerInfo::Load() {
  FileSpec user_lldb_dir = HostInfo::GetUserLLDBDir();
  namespace path = llvm::sys::path;
  FileSystem &fs = FileSystem::Instance();
  std::error_code EC;
  llvm::vfs::directory_iterator it = fs.DirBegin(user_lldb_dir, EC);
  llvm::vfs::directory_iterator end;
  std::vector<ServerInfo> infos;
  for (; it != end && !EC; it.increment(EC)) {
    auto &entry = *it;
    auto name = path::filename(entry.path());
    if (!name.starts_with("lldb-mcp-") || !name.ends_with(".json")) {
      continue;
    }

    llvm::Expected<std::unique_ptr<File>> file =
        fs.Open(FileSpec(entry.path()), File::eOpenOptionReadOnly);
    if (!file)
      return file.takeError();

    char buf[1024] = {0};
    size_t bytes_read = sizeof(buf);
    if (llvm::Error error = (*file)->Read(buf, bytes_read).takeError())
      return std::move(error);

    auto info = json::parse<ServerInfo>(StringRef(buf, bytes_read));
    if (!info)
      return info.takeError();

    infos.emplace_back(std::move(*info));
  }

  return infos;
}

Server::Server(std::string name, std::string version, MCPTransport &client,
               lldb_private::MainLoop &loop, LogCallback log_callback)
    : m_binder(client, /*seq=*/1), m_name(std::move(name)),
      m_version(std::move(version)), m_client(client), m_loop(loop),
      m_log_callback(std::move(log_callback)) {
  m_binder.bind<InitializeResult, InitializeParams>(
      "initialize", &Server::InitializeHandler, this, std::placeholders::_1);
  m_binder.bind<ListToolsResult, lldb_private::VoidT>(
      "tools/list", &Server::ToolsListHandler, this);
  m_binder.bind<CallToolResult, CallToolParams>(
      "tools/call", &Server::ToolsCallHandler, this, std::placeholders::_1);
  m_binder.bind<ListResourcesResult, lldb_private::VoidT>(
      "resources/list", &Server::ResourcesListHandler, this);
  m_binder.bind<ReadResourceResult, ReadResourceParams>(
      "resources/read", &Server::ResourcesReadHandler, this,
      std::placeholders::_1);
}

void Server::AddTool(std::unique_ptr<Tool> tool) {
  if (!tool)
    return;
  m_tools[tool->GetName()] = std::move(tool);
}

void Server::AddResourceProvider(
    std::unique_ptr<ResourceProvider> resource_provider) {
  if (!resource_provider)
    return;
  m_resource_providers.push_back(std::move(resource_provider));
}

Expected<InitializeResult>
Server::InitializeHandler(const InitializeParams &request) {
  InitializeResult result;
  result.protocolVersion = mcp::kProtocolVersion;
  result.capabilities = GetCapabilities();
  result.serverInfo.name = m_name;
  result.serverInfo.version = m_version;
  return result;
}

llvm::Expected<ListToolsResult> Server::ToolsListHandler() {
  ListToolsResult result;
  for (const auto &tool : m_tools)
    result.tools.emplace_back(tool.second->GetDefinition());

  return result;
}

llvm::Expected<CallToolResult>
Server::ToolsCallHandler(const CallToolParams &params) {
  llvm::StringRef tool_name = params.name;
  if (tool_name.empty())
    return llvm::createStringError("no tool name");

  auto it = m_tools.find(tool_name);
  if (it == m_tools.end())
    return llvm::createStringError(llvm::formatv("no tool \"{0}\"", tool_name));

  ToolArguments tool_args;
  if (params.arguments)
    tool_args = *params.arguments;

  llvm::Expected<CallToolResult> text_result = it->second->Call(tool_args);
  if (!text_result)
    return text_result.takeError();

  return text_result;
}

llvm::Expected<ListResourcesResult> Server::ResourcesListHandler() {
  ListResourcesResult result;
  for (std::unique_ptr<ResourceProvider> &resource_provider_up :
       m_resource_providers)
    for (const Resource &resource : resource_provider_up->GetResources())
      result.resources.push_back(resource);

  return result;
}

Expected<ReadResourceResult>
Server::ResourcesReadHandler(const ReadResourceParams &params) {
  StringRef uri_str = params.uri;
  if (uri_str.empty())
    return createStringError("no resource uri");

  for (std::unique_ptr<ResourceProvider> &resource_provider_up :
       m_resource_providers) {
    Expected<ReadResourceResult> result =
        resource_provider_up->ReadResource(uri_str);
    if (result.errorIsA<UnsupportedURI>()) {
      consumeError(result.takeError());
      continue;
    }
    if (!result)
      return result.takeError();

    return *result;
  }

  return make_error<MCPError>(
      formatv("no resource handler for uri: {0}", uri_str).str(),
      MCPError::kResourceNotFound);
}

ServerCapabilities Server::GetCapabilities() {
  lldb_protocol::mcp::ServerCapabilities capabilities;
  capabilities.supportsToolsList = true;
  capabilities.supportsResourcesList = true;
  // FIXME: Support sending notifications when a debugger/target are
  // added/removed.
  capabilities.supportsResourcesSubscribe = false;
  return capabilities;
}

Expected<lldb_private::MainLoop::ReadHandleUP> Server::RegisterClient() {
  return m_client.RegisterMessageHandler(m_loop, m_binder);
}

llvm::Error Server::Run() {
  Expected<lldb_private::MainLoop::ReadHandleUP> handle = RegisterClient();
  if (!handle)
    return handle.takeError();
  return m_loop.Run().takeError();
}

void Server::TerminateLoop() {
  m_loop.AddPendingCallback(
      [](lldb_private::MainLoopBase &loop) { loop.RequestTermination(); });
}
