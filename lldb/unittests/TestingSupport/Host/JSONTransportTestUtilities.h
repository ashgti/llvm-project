//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_UNITTESTS_TESTINGSUPPORT_HOST_NATIVEPROCESSTESTUTILS_H
#define LLDB_UNITTESTS_TESTINGSUPPORT_HOST_NATIVEPROCESSTESTUTILS_H

#include "lldb/Host/FileSystem.h"
#include "lldb/Host/JSONTransport.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Utility/FileSpec.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include <cstddef>

template <typename Req, typename Resp, typename Evt>
class MockMessageHandler final
    : public lldb_private::Transport<Req, Resp, Evt>::MessageHandler {
public:
  MOCK_METHOD(void, Received, (const Evt &), (override));
  MOCK_METHOD(void, Received, (const Req &), (override));
  MOCK_METHOD(void, Received, (const Resp &), (override));
  MOCK_METHOD(void, OnError, (llvm::Error), (override));
  MOCK_METHOD(void, OnClosed, (), (override));
};

template <typename Req, typename Resp, typename Evt>
class TestTransport final : public lldb_private::Transport<Req, Resp, Evt> {
public:
  using MessageHandler =
      typename lldb_private::Transport<Req, Resp, Evt>::MessageHandler;

  TestTransport(lldb_private::MainLoop *loop = nullptr,
                MessageHandler *handler = nullptr)
      : m_loop(loop), m_handler(handler) {
    llvm::Expected<lldb::FileUP> dummy_file =
        lldb_private::FileSystem::Instance().Open(
            lldb_private::FileSpec(lldb_private::FileSystem::DEV_NULL),
            lldb_private::File::eOpenOptionReadWrite);
    EXPECT_THAT_EXPECTED(dummy_file, llvm::Succeeded());
    m_dummy_file = std::move(*dummy_file);
  }

  llvm::Error Send(const Evt &e) override {
    EXPECT_TRUE(m_loop && m_handler)
        << "Send called before RegisterMessageHandler";
    m_loop->AddPendingCallback(
        [this, e](lldb_private::MainLoopBase &) { m_handler->Received(e); });
    return llvm::Error::success();
  }

  llvm::Error Send(const Req &r) override {
    EXPECT_TRUE(m_loop && m_handler)
        << "Send called before RegisterMessageHandler";
    m_loop->AddPendingCallback(
        [this, r](lldb_private::MainLoopBase &) { m_handler->Received(r); });
    return llvm::Error::success();
  }

  llvm::Error Send(const Resp &r) override {
    EXPECT_TRUE(m_loop && m_handler)
        << "Send called before RegisterMessageHandler";
    m_loop->AddPendingCallback(
        [this, r](lldb_private::MainLoopBase &) { m_handler->Received(r); });
    return llvm::Error::success();
  }

  llvm::Expected<lldb_private::MainLoop::ReadHandleUP>
  RegisterMessageHandler(lldb_private::MainLoop &loop,
                         MessageHandler &handler) override {
    if (!m_loop)
      m_loop = &loop;
    if (!m_handler)
      m_handler = &handler;
    lldb_private::Status status;
    auto handle = loop.RegisterReadObject(
        m_dummy_file, [](lldb_private::MainLoopBase &) {}, status);
    if (status.Fail())
      return status.takeError();
    return handle;
  }

protected:
  void Log(llvm::StringRef message) override {};

private:
  lldb_private::MainLoop *m_loop;
  MessageHandler *m_handler;
  // Dummy file for registering with the MainLoop.
  lldb::FileSP m_dummy_file;
};

#endif
