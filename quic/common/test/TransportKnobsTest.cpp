/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/QuicConstants.h>
#include <quic/common/TransportKnobs.h>

#include <folly/Format.h>
#include <folly/portability/GTest.h>

using namespace ::testing;

namespace quic {
namespace test {

struct QuicKnobsParsingTestFixture {
  std::string serializedKnobs;
  bool expectError;
  TransportKnobParams expectParams;
};

void run(const QuicKnobsParsingTestFixture& fixture) {
  auto result = parseTransportKnobs(fixture.serializedKnobs);
  if (fixture.expectError) {
    EXPECT_FALSE(result.hasValue());
  } else {
    EXPECT_EQ(result->size(), fixture.expectParams.size());
    for (size_t i = 0; i < result->size(); i++) {
      auto& actualKnob = (*result)[i];
      auto& expectKnob = fixture.expectParams[i];
      EXPECT_EQ(actualKnob.id, expectKnob.id) << "Knob " << i;
      EXPECT_EQ(actualKnob.val, expectKnob.val) << "Knob " << i;
    }
  }
}

TEST(QuicKnobsParsingTest, Simple) {
  QuicKnobsParsingTestFixture fixture = {
      "{ \"0\": 1,"
      "  \"1\": 5,"
      "  \"19\": 6,"
      "  \"2\": 3"
      "  }",
      false,
      {{0, 1}, {1, 5}, {2, 3}, {19, 6}}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, ObjectValue) {
  QuicKnobsParsingTestFixture fixture = {
      "{ \"1\": "
      "  {"
      "  \"0\" : 1"
      "  }"
      "}",
      true,
      {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidJson) {
  QuicKnobsParsingTestFixture fixture = {
      "{\"0\": "
      " \"1\": "
      "  {"
      "  \"0\" : 1"
      "  }"
      "}",
      true,
      {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, Characters) {
  QuicKnobsParsingTestFixture fixture = {"{ \"o\" : 1 }", true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, NegativeNumbers) {
  QuicKnobsParsingTestFixture fixture = {"{ \"1\" : -1 }", true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, ValidCCAlgorithm) {
  auto key = static_cast<uint64_t>(TransportKnobParamId::CC_ALGORITHM_KNOB);
  uint64_t val =
      static_cast<uint64_t>(congestionControlStrToType("cubic").value());
  std::string args = folly::format(R"({{"{}" : "cubic"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {
      args, false, {{.id = key, .val = val}}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidCCAlgorithm) {
  auto key = static_cast<uint64_t>(TransportKnobParamId::CC_ALGORITHM_KNOB);
  std::string args = folly::format(R"({{"{}" : "foo"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidStringParam) {
  auto key = static_cast<uint64_t>(
      TransportKnobParamId::FORCIBLY_SET_UDP_PAYLOAD_SIZE);
  std::string args = folly::format(R"({{"{}" : "foo"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamFormat) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "1"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamFormatDefault) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::DEFAULT_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "1"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamFormat2) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "1,2"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamZeroDenom) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "1/0"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamZeroNum) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "0/2"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamLargeDenom) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "1/1234567"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidFractionParamLargeNum) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "1234567/1"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, ValidFractionParam) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::STARTUP_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "4/5"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {
      args, false, {{.id = key, .val = (4 * 100 + 5)}}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, ValidFractionParamDefault) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::DEFAULT_RTT_FACTOR_KNOB);
  std::string args = folly::format(R"({{"{}" : "4/5"}})", key).str();
  QuicKnobsParsingTestFixture fixture = {
      args, false, {{.id = key, .val = (4 * 100 + 5)}}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, ValidNotSentBufferSize) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::NOTSENT_BUFFER_SIZE_KNOB);
  uint64_t val = 111;
  std::string args = folly::format(R"({{"{}" : {}}})", key, val).str();
  QuicKnobsParsingTestFixture fixture = {
      args, false, {{.id = key, .val = val}}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, InvalidNotSentBufferSizeAsString) {
  auto key =
      static_cast<uint64_t>(TransportKnobParamId::NOTSENT_BUFFER_SIZE_KNOB);
  uint64_t val = 111;
  std::string args = folly::format(R"({{"{}" : "{}"}})", key, val).str();
  QuicKnobsParsingTestFixture fixture = {args, true, {{.id = key, .val = val}}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, NonStringKey) {
  QuicKnobsParsingTestFixture fixture = {"{ 1 : 1 }", true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, DoubleKey) {
  QuicKnobsParsingTestFixture fixture = {"{ \"3.14\" : 1 }", true, {}};
  run(fixture);
}

TEST(QuicKnobsParsingTest, DoubleValue) {
  QuicKnobsParsingTestFixture fixture = {"{  \"10\" : 0.1 }", true, {}};
  run(fixture);
}

} // namespace test
} // namespace quic
