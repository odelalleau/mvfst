/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/api/IoBufQuicBatch.h>

#include <gtest/gtest.h>
#include <quic/client/state/ClientStateMachine.h>
#include <quic/common/test/TestUtils.h>
#include <quic/fizz/client/handshake/FizzClientQuicHandshakeContext.h>
#include <quic/state/StateData.h>

constexpr const auto kNumLoops = 64;
constexpr const auto kMaxBufs = 10;

namespace quic {
namespace testing {
void RunTest(int numBatch) {
  folly::EventBase evb;
  folly::AsyncUDPSocket sock(&evb);

  auto batchWriter = BatchWriterPtr(new test::TestPacketBatchWriter(numBatch));
  folly::SocketAddress peerAddress{"127.0.0.1", 1234};
  QuicClientConnectionState conn(
      FizzClientQuicHandshakeContext::Builder().build());
  QuicConnectionStateBase::HappyEyeballsState happyEyeballsState;

  IOBufQuicBatch ioBufBatch(
      std::move(batchWriter),
      false,
      sock,
      peerAddress,
      conn.statsCallback,
      happyEyeballsState);

  std::string strTest("Test");

  for (size_t i = 0; i < kNumLoops; i++) {
    auto buf = folly::IOBuf::copyBuffer(strTest.c_str(), strTest.length());
    CHECK(ioBufBatch.write(std::move(buf), strTest.length()));
  }
  // check flush is successful
  CHECK(ioBufBatch.flush());
  // check we sent all the packets
  CHECK_EQ(ioBufBatch.getPktSent(), kNumLoops);
}

TEST(QuicBatch, TestBatchingNone) {
  RunTest(1);
}

TEST(QuicBatch, TestBatchingNoFlush) {
  RunTest(-1);
}

TEST(QuicBatch, TestBatching) {
  RunTest(kMaxBufs);
}
} // namespace testing
} // namespace quic
