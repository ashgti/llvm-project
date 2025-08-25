//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBDebugger.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/JSONTransport.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Host/MainLoopBase.h"
#include "lldb/Host/Socket.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Relay.h"
#include "lldb/Protocol/MCP/Transport.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>

using namespace llvm;
using namespace lldb;
using lldb_private::FileSystem;
using lldb_private::MainLoop;
using lldb_private::MainLoopBase;
using lldb_private::NativeFile;
using lldb_private::Socket;
using lldb_private::Status;
using lldb_protocol::mcp::InitializeParams;
using lldb_protocol::mcp::InitializeResult;
using lldb_protocol::mcp::MCPTransportUP;
using lldb_protocol::mcp::RelayClient;
using lldb_protocol::mcp::RelayServer;
using lldb_protocol::mcp::ToolsListResult;
using lldb_protocol::mcp::Transport;
using lldb_protocol::mcp::Void;

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

static std::string tool_name;
enum class Mode { Server, Client, TestClient };
struct MCPOptions {
  Mode mode = Mode::Client;
  std::chrono::milliseconds timeout = std::chrono::seconds(30);
};

Error validateAndSetOptions(const opt::InputArgList &args, MCPOptions &opts) {
  if (args.hasArg(OPT_server)) {
    opts.mode = Mode::Server;
  }
  if (args.hasArg(OPT_test_client)) {
    opts.mode = Mode::TestClient;
  }

  return Error::success();
}

inline void error(Error Err, StringRef Prefix = "") {
  handleAllErrors(std::move(Err), [&](ErrorInfoBase &Info) {
    WithColor::error(errs(), Prefix) << Info.message() << '\n';
  });
  std::exit(EXIT_FAILURE);
}

static raw_ostream &logger() {
  // static raw_fd_ostream *S;
  // static once_flag flag;
  // call_once(flag, []() {
  //   int fd;
  //   std::error_code EC =
  //       sys::fs::openFile("/tmp/lldb-mcp-" + std::to_string(getpid()) +
  //       ".log",
  //                         fd, sys::fs::CD_CreateAlways,
  //                         sys::fs::FA_Write,
  //                         sys::fs::OpenFlags::OF_None);
  //   assert(!EC && "Failed to open log file");
  //   S = new raw_fd_ostream(fd, false, true);
  // });
  // return *S;
  return errs();
}

static constexpr size_t kBufferSize = 1024;

Error runClient(const MCPOptions &opts) {
  logger() << "Running client\n";
  IOObjectSP input = std::make_shared<NativeFile>(
      STDIN_FILENO, NativeFile::OpenOptions::eOpenOptionReadOnly, false);
  IOObjectSP output = std::make_shared<NativeFile>(
      STDOUT_FILENO, NativeFile::OpenOptions::eOpenOptionWriteOnly, false);

  Expected<IOObjectSP> maybe_conn = lldb_protocol::mcp::Connect();
  if (auto Err = maybe_conn.takeError())
    error(std::move(Err), "Failed to connect to server");

  IOObjectSP conn = std::move(*maybe_conn);

  MainLoop loop;
  Status status;
  auto conn_handle = loop.RegisterReadObject(
      conn,
      [conn, output](MainLoopBase &loop) {
        char buf[kBufferSize] = {0};
        size_t bytes_read = 0;
        if (Error err = conn->Read(buf, bytes_read).takeError())
          error(std::move(err), "reading from server failed");
        if (Error err = output->Write(buf, bytes_read).takeError())
          error(std::move(err), "writing to stdout failed");
      },
      status);
  if (status.Fail())
    return status.takeError();
  auto in_handle = loop.RegisterReadObject(
      input,
      [conn, input](MainLoopBase &loop) {
        char buf[kBufferSize] = {0};
        size_t bytes_read = 0;
        if (Error err = input->Read(buf, bytes_read).takeError())
          error(std::move(err), "reading from stdin failed");
        if (Error err = conn->Write(buf, bytes_read).takeError())
          error(std::move(err), "writing to conn failed");
      },
      status);
  if (status.Fail())
    return status.takeError();

  return loop.Run().takeError();
}

Error runServer(const MCPOptions &opts) {
  logger() << "Running starting server\n";

  StringRef socket_path = lldb_protocol::mcp::CommunicationSocketPath();
  Status error;
  auto listener = Socket::Create(Socket::ProtocolUnixDomain, error);
  if (error.Fail())
    return error.takeError();
  error = listener->Listen(socket_path, 5);
  if (error.Fail())
    return error.takeError();

  std::string addresses = join(listener->GetListeningConnectionURI(), ", ");
  logger() << "Listening on " << addresses << "\n";

  MainLoop loop;
  RelayServer server{loop, &logger()};
  Expected<std::vector<MainLoopBase::ReadHandleUP>> accept_handle =
      listener->Accept(loop, std::bind(&RelayServer::OnConnect, &server,
                                       std::ref(loop), std::placeholders::_1));

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

Error runTestClient(const MCPOptions &opts) {
  logger() << "Running stdio <-> lldb-mcp test client\n";

  Expected<MCPTransportUP> maybe_conn = Transport::Connect(&logger());
  if (auto Err = maybe_conn.takeError())
    error(std::move(Err), "Failed to connect to server");
  MCPTransportUP conn = std::move(*maybe_conn);

  RelayClient client(conn.get());
  MainLoop loop;
  auto handle = conn->RegisterMessageHandler(loop, client);
  if (!handle)
    return handle.takeError();

  lldb_protocol::mcp::DebuggersChangedParams changed_event;
  changed_event.debuggers = {lldb_protocol::mcp::DebuggerRef{
      1, {lldb_protocol::mcp::TargetRef{2, "target.exe"}}}};
  client.DebuggersChanged(changed_event);

  const InitializeParams params = {
      /*protocolVersion=*/"2025-06-18",
      /*capabilities=*/{},
      /*clientInfo=*/
      {
          /*name=*/"lldb-mcp",
          /*title=*/"lldb-mcp cli",
          /*version=*/"0.0.1",
      },
  };
  Expected<InitializeResult> init_result =
      AsyncInvoke(loop, client.Initialize, params);
  if (!init_result)
    return init_result.takeError();

  Expected<ToolsListResult> tools_list_result =
      AsyncInvoke(loop, client.ToolsList, Void());
  if (!tools_list_result)
    return tools_list_result.takeError();

  for (const auto &tool : tools_list_result->tools)
    outs() << "Tool: " << tool.name << "\n";

  Expected<lldb_protocol::mcp::ResourcesListResult> resources_list_result =
      AsyncInvoke(loop, client.ResourcesList, Void());
  if (!resources_list_result)
    return resources_list_result.takeError();
  if (resources_list_result->resources.empty())
    return createStringError("Not enough debug sessions returned!!");
  auto debugger_uri = resources_list_result->resources[0].uri;

  for (const auto &resource : resources_list_result->resources)
    outs() << "Resource: " << resource.uri << ":" << resource.name << " - "
           << resource.description << "\n";

  Expected<lldb_protocol::mcp::ResourcesReadResult> resources_read_result =
      AsyncInvoke(loop, client.ResourcesRead, {/*URI=*/debugger_uri});
  if (!resources_read_result)
    return resources_read_result.takeError();

  for (const auto &content : resources_read_result->contents)
    outs() << "Reading resource " << content.uri << ":\n"
           << content.text << "\n";

  Expected<lldb_protocol::mcp::ToolsCallResult> tools_call_launch_result =
      AsyncInvoke(loop, client.ToolsCall,
                  {/*name=*/"launch", /*arguments=*/{}});
  if (!tools_call_launch_result)
    return tools_call_launch_result.takeError();

  if (tools_call_launch_result->isError)
    errs() << "Invoking launch failed: ";
  for (const auto &content : tools_call_launch_result->content)
    outs() << content.text << "\n";

  Expected<lldb_protocol::mcp::ToolsCallResult> tools_call_evaluate_result =
      AsyncInvoke(loop, client.ToolsCall,
                  {/*name=*/"evaluate",
                   /*arguments=*/{
                       json::Object{{"debugger_uri", debugger_uri},
                                    {"arguments", "example arguments..."}}}});
  if (!tools_call_evaluate_result)
    return tools_call_evaluate_result.takeError();
  if (tools_call_evaluate_result->isError)
    errs() << "Invoking launch failed: ";
  for (const auto &content : tools_call_evaluate_result->content)
    outs() << content.text << "\n";

  return Error::success();
}

} // namespace

int main(int argc, char *argv[]) {
  InitLLVM IL(argc, argv, /*InstallPipeSignalExitHandler=*/false);

  SmallString<256> program_path(argv[0]);
  sys::fs::make_absolute(program_path);
  tool_name = program_path.str();

  MCPOptTable T;
  BumpPtrAllocator A;
  StringSaver Saver{A};
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
    if (Error err = runClient(opts))
      error(std::move(err), "lldb-mcp client");
    break;
  case Mode::TestClient:
    if (Error err = runTestClient(opts))
      error(std::move(err), "lldb-mcp client");
    break;
  }

  return EXIT_SUCCESS;
}
