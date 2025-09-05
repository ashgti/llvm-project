//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ProtocolMCPTestUtilities.h" // IWYU pragma: keep
#include "TestingSupport/Host/JSONTransportTestUtilities.h"
#include "TestingSupport/SubsystemRAII.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/JSONTransport.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Host/MainLoopBase.h"
#include "lldb/Host/Socket.h"
#include "lldb/Protocol/MCP/MCPError.h"
#include "lldb/Protocol/MCP/Protocol.h"
#include "lldb/Protocol/MCP/Resource.h"
#include "lldb/Protocol/MCP/Server.h"
#include "lldb/Protocol/MCP/Tool.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <future>
#include <memory>

using namespace llvm;
using namespace lldb;
using namespace lldb_private;
using namespace lldb_protocol::mcp;
using namespace testing;

namespace {

/// Test tool that returns it argument as text.
class TestTool : public Tool {
public:
  using Tool::Tool;

  llvm::Expected<CallToolResult> Call(const ToolArguments &args) override {
    std::string argument;
    if (const json::Object *args_obj =
            std::get<json::Value>(args).getAsObject()) {
      if (const json::Value *s = args_obj->get("arguments")) {
        argument = s->getAsString().value_or("");
      }
    }

    CallToolResult text_result;
    text_result.content.emplace_back(TextContent{{argument}});
    return text_result;
  }
};

class TestResourceProvider : public ResourceProvider {
  using ResourceProvider::ResourceProvider;

  std::vector<Resource> GetResources() const override {
    std::vector<Resource> resources;

    Resource resource;
    resource.uri = "lldb://foo/bar";
    resource.name = "name";
    resource.description = "description";
    resource.mimeType = "application/json";

    resources.push_back(resource);
    return resources;
  }

  llvm::Expected<ReadResourceResult>
  ReadResource(llvm::StringRef uri) const override {
    if (uri != "lldb://foo/bar")
      return llvm::make_error<UnsupportedURI>(uri.str());

    TextResourceContents contents;
    contents.uri = "lldb://foo/bar";
    contents.mimeType = "application/json";
    contents.text = "foobar";

    ReadResourceResult result;
    result.contents.push_back(contents);
    return result;
  }
};

/// Test tool that returns an error.
class ErrorTool : public Tool {
public:
  using Tool::Tool;

  llvm::Expected<CallToolResult> Call(const ToolArguments &args) override {
    return llvm::createStringError("error");
  }
};

/// Test tool that fails but doesn't return an error.
class FailTool : public Tool {
public:
  using Tool::Tool;

  llvm::Expected<CallToolResult> Call(const ToolArguments &args) override {
    CallToolResult text_result;
    text_result.content.emplace_back(TextContent{{"failed"}});
    text_result.isError = true;
    return text_result;
  }
};

class TestServer : public Server {
public:
  using Server::m_binder;
  using Server::Server;
};

class ProtocolServerMCPTest : public testing::Test {
public:
  SubsystemRAII<FileSystem, HostInfo, Socket> subsystems;

  MainLoop loop;
  std::unique_ptr<TestTransport<Request, Response, Notification>> conn[2];
  lldb_private::MainLoop::ReadHandleUP handles[2];
  std::unique_ptr<TestTransport<Request, Response, Notification>::Binder<>>
      client_up;
  std::unique_ptr<TestServer> server_up;

  OutgoingRequest<InitializeResult, InitializeParams> Initialize;
  OutgoingRequest<ListToolsResult, VoidT> ToolsList;
  OutgoingRequest<CallToolResult, CallToolParams> ToolsCall;
  OutgoingRequest<ListResourcesResult, VoidT> ResourcesList;
  OutgoingNotification<VoidT> Initialized;

  /// Runs the MainLoop a single time, executing any pending callbacks.
  void Run() {
    loop.AddPendingCallback(
        [](MainLoopBase &loop) { loop.RequestTermination(); });
    EXPECT_THAT_ERROR(loop.Run().takeError(), Succeeded());
  }

  void SetUp() override {
    conn[0] =
        std::make_unique<TestTransport<Request, Response, Notification>>();
    conn[1] =
        std::make_unique<TestTransport<Request, Response, Notification>>();
    server_up =
        std::make_unique<TestServer>("lldb-mcp", "0.1.0", *conn[0], loop);
    client_up = std::make_unique<
        TestTransport<Request, Response, Notification>::Binder<>>(*conn[1],
                                                                  /*seq=*/1);
    auto server_handle = conn[0]->RegisterMessageHandler(loop, *client_up);
    EXPECT_THAT_EXPECTED(server_handle, Succeeded());
    handles[0] = std::move(*server_handle);
    auto client_handle = conn[1]->RegisterMessageHandler(loop, *server_up);
    EXPECT_THAT_EXPECTED(client_handle, Succeeded());
    handles[1] = std::move(*client_handle);
    Initialize =
        client_up->bind<InitializeResult, InitializeParams>("initialize");
    ToolsList = client_up->bind<ListToolsResult, VoidT>("tools/list");
    ToolsCall = client_up->bind<CallToolResult, CallToolParams>("tools/call");
    ResourcesList =
        client_up->bind<ListResourcesResult, VoidT>("resources/list");
    Initialized = client_up->bind<VoidT>("notifications/initialized");
  }

  template <typename Result>
  Expected<json::Value>
  Capture(llvm::unique_function<void(Reply<Result>)> &fn) {
    std::promise<llvm::Expected<Result>> promised_result;
    fn([&promised_result](llvm::Expected<Result> result) {
      promised_result.set_value(std::move(result));
    });
    Run();
    llvm::Expected<Result> result = promised_result.get_future().get();
    if (!result)
      return result.takeError();
    return toJSON(*result);
  }

  template <typename Result, typename Params>
  Expected<json::Value>
  Capture(llvm::unique_function<void(const Params &, Reply<Result>)> &fn,
          const Params &params) {
    std::promise<llvm::Expected<Result>> promised_result;
    fn(params, [&promised_result](llvm::Expected<Result> result) {
      promised_result.set_value(std::move(result));
    });
    Run();
    llvm::Expected<Result> result = promised_result.get_future().get();
    if (!result)
      return result.takeError();
    return toJSON(*result);
  }
};

template <typename T>
Request make_request(StringLiteral method, T &&params, Id id = 1) {
  return Request{id, method.str(), toJSON(std::forward<T>(params))};
}

template <typename T> Response make_response(T &&result, Id id = 1) {
  return Response{id, std::forward<T>(result)};
}

template <typename T>
inline internal::EqMatcher<llvm::json::Value> HasJSON(T x) {
  return internal::EqMatcher<llvm::json::Value>(toJSON(x));
}

testing::Matcher<MCPError> withMessage(llvm::StringRef message) {
  return testing::Property(&MCPError::getMessage, message);
}

} // namespace

TEST_F(ProtocolServerMCPTest, Initialization) {
  EXPECT_THAT_EXPECTED(
      (Capture<InitializeResult, InitializeParams>(
          Initialize, InitializeParams{/*protocolVersion=*/"2024-11-05",
                                       /*capabilities=*/{},
                                       /*clientInfo=*/{"lldb-unit", "0.1.0"}})),
      HasValue(HasJSON(
          InitializeResult{/*protocolVersion=*/"2024-11-05",
                           /*capabilities=*/{/*supportsToolsList=*/true},
                           /*serverInfo=*/{"lldb-mcp", "0.1.0"}})));
}

TEST_F(ProtocolServerMCPTest, ToolsList) {
  server_up->AddTool(std::make_unique<TestTool>("test", "test tool"));

  ToolDefinition test_tool;
  test_tool.name = "test";
  test_tool.description = "test tool";
  test_tool.inputSchema = json::Object{{"type", "object"}};

  EXPECT_THAT_EXPECTED(Capture<ListToolsResult>(ToolsList),
                       HasValue(HasJSON(ListToolsResult{{test_tool}})));
}

TEST_F(ProtocolServerMCPTest, ResourcesList) {
  server_up->AddResourceProvider(std::make_unique<TestResourceProvider>());

  EXPECT_THAT_EXPECTED(Capture<ListResourcesResult>(ResourcesList),
                       HasValue(HasJSON(ListResourcesResult{{
                           {
                               /*uri=*/"lldb://foo/bar",
                               /*name=*/"name",
                               /*description=*/"description",
                               /*mimeType=*/"application/json",
                           },
                       }})));
}

TEST_F(ProtocolServerMCPTest, ToolsCall) {
  server_up->AddTool(std::make_unique<TestTool>("test", "test tool"));

  EXPECT_THAT_EXPECTED((Capture<CallToolResult, CallToolParams>(
                           ToolsCall, CallToolParams{
                                          /*name=*/"test",
                                          /*arguments=*/
                                          json::Object{
                                              {"arguments", "foo"},
                                              {"debugger_id", 0},
                                          },
                                      })),
                       HasValue(HasJSON(CallToolResult{{{/*text=*/"foo"}}})));
}

TEST_F(ProtocolServerMCPTest, ToolsCallError) {
  server_up->AddTool(std::make_unique<ErrorTool>("error", "error tool"));

  EXPECT_THAT_EXPECTED((Capture<CallToolResult, CallToolParams>(
                           ToolsCall, CallToolParams{
                                          /*name=*/"error",
                                          /*arguments=*/
                                          json::Object{
                                              {"arguments", "foo"},
                                              {"debugger_id", 0},
                                          },
                                      })),
                       Failed<MCPError>(withMessage("error")));
}

TEST_F(ProtocolServerMCPTest, ToolsCallFail) {
  server_up->AddTool(std::make_unique<FailTool>("fail", "fail tool"));

  EXPECT_THAT_EXPECTED((Capture<CallToolResult, CallToolParams>(
                           ToolsCall, CallToolParams{
                                          /*name=*/"fail",
                                          /*arguments=*/
                                          json::Object{
                                              {"arguments", "foo"},
                                              {"debugger_id", 0},
                                          },
                                      })),
                       HasValue(HasJSON(CallToolResult{
                           {{/*text=*/"failed"}},
                           /*isError=*/true,
                       })));
}

TEST_F(ProtocolServerMCPTest, NotificationInitialized) {
  bool handler_called = false;

  server_up->m_binder.bind<Void>(
      "notifications/initialized", [&](const Void &) { handler_called = true; },
      std::placeholders::_1);

  Initialized();
  Run();
  EXPECT_TRUE(handler_called);
}
