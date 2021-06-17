/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <glog/logging.h>

#include <fizz/crypto/Utils.h>
#include <folly/init/Init.h>
#include <folly/io/async/HHWheelTimer.h>
#include <folly/portability/GFlags.h>
#include <folly/stats/Histogram.h>

#include <quic/QuicConstants.h>
#include <quic/client/QuicClientTransport.h>
#include <quic/common/test/TestUtils.h>
#include <quic/congestion_control/CongestionControlEnv.h>
#include <quic/congestion_control/CongestionControlEnvFactory.h>
#include <quic/congestion_control/RLCongestionControllerFactory.h>
#include <quic/congestion_control/ServerCongestionControllerFactory.h>
#include <quic/fizz/client/handshake/FizzClientQuicHandshakeContext.h>
#include <quic/server/AcceptObserver.h>
#include <quic/server/QuicServer.h>
#include <quic/server/QuicServerTransport.h>
#include <quic/server/QuicSharedUDPSocketFactory.h>
#include <quic/tools/tperf/PacingObserver.h>
#include <quic/tools/tperf/TperfQLogger.h>

DEFINE_string(host, "::1", "TPerf server hostname/IP");
DEFINE_int32(port, 6666, "TPerf server port");
DEFINE_string(mode, "server", "Mode to run in: 'client' or 'server'");
DEFINE_int32(duration, 10, "Duration of test in seconds");
DEFINE_uint64(
    block_size,
    4096,
    "Amount of data written to stream each iteration");
DEFINE_uint64(writes_per_loop, 5, "Amount of socket writes per event loop");
DEFINE_uint64(window, 64 * 1024, "Flow control window size");
DEFINE_string(congestion, "newreno", "newreno/cubic/bbr/ccp/none/rl");
DEFINE_string(ccp_config, "", "Additional args to pass to ccp");
DEFINE_bool(pacing, false, "Enable pacing");
DEFINE_bool(gso, false, "Enable GSO writes to the socket");
DEFINE_uint32(
    client_transport_timer_resolution_ms,
    1,
    "Timer resolution for Ack and Loss tiemout in client transport");
DEFINE_string(
    server_qlogger_path,
    "",
    "Path to the directory where qlog files will be written. File will be named"
    " as <CID>.qlog where CID is the DCID from client's perspective.");
DEFINE_uint32(
    max_cwnd_mss,
    quic::kLargeMaxCwndInMss,
    "Max cwnd in the unit of mss");
DEFINE_uint32(num_streams, 1, "Number of streams to send on simultaneously");
DEFINE_uint64(
    bytes_per_stream,
    0,
    "Maximum number of bytes per stream. "
    "0 (the default) means the stream lives for the whole duration of the test.");
DEFINE_string(
    pacing_observer,
    "none",
    "none/time/rtt/ack: Pacing observer bucket type: per 3ms, per rtt or per ack");
DEFINE_uint32(
    max_receive_packet_size,
    std::max(
        quic::kDefaultV4UDPSendPacketLen,
        quic::kDefaultV6UDPSendPacketLen),
    "Maximum packet size to advertise to the peer.");
DEFINE_bool(use_inplace_write, false, "Data path type");
DEFINE_double(latency_factor, 0.5, "Latency factor (delta) for Copa");
DEFINE_uint32(
    num_server_worker,
    1,
    "Max number of mvfst server worker threads");
DEFINE_bool(log_rtt_sample, false, "Log rtt sample events");
DEFINE_bool(log_loss, false, "Log packet loss events");
DEFINE_bool(log_app_rate_limited, false, "Log app rate limited events");
DEFINE_bool(log_pmtu_probing_started, false, "Log pmtu probing started events");
DEFINE_bool(log_pmtu_upperbound, false, "Log pmtu upper bound events");
DEFINE_bool(log_pmtu_blackhole, false, "Log pmtu blackbole events");
DEFINE_bool(d6d_enabled, false, "Enable d6d");
DEFINE_uint32(
    d6d_probe_raiser_constant_step_size,
    10,
    "Server only. The constant step size used to increase PMTU, only meaningful to ConstantStep probe size raiser");
DEFINE_uint32(
    d6d_probe_raiser_type,
    0,
    "Server only. The type of probe size raiser. 0: ConstantStep, 1: BinarySearch");
DEFINE_uint32(
    d6d_blackhole_detection_window_secs,
    5,
    "Server only. PMTU blackhole detection window in secs");
DEFINE_uint32(
    d6d_blackhole_detection_threshold,
    5,
    "Server only. PMTU blackhole detection threshold, in # of packets");
DEFINE_uint32(
    d6d_base_pmtu,
    1252,
    "Client only. The base PMTU advertised to server");
DEFINE_uint32(
    d6d_raise_timeout_secs,
    600,
    "Client only. The raise timeout advertised to server");
DEFINE_uint32(
    d6d_probe_timeout_secs,
    600,
    "Client only. The probe timeout advertised to server");
DEFINE_string(
    transport_knob_params,
    "",
    "JSON-serialized dictionary of transport knob params");
// RL-specific arguments.
DEFINE_string(
    cc_env_mode,
    "local",
    "CongestionControlEnv mode for RL cc_algo - [local|remote|random|fixed]. "
    "Note that 'remote' is not currently supported.");
DEFINE_string(
    cc_env_model_file,
    "traced_model.pt",
    "PyTorch traced model file for local mode");
DEFINE_int64(
    cc_env_job_count,
    -1,
    "Job counter during training. -1 if undefined. "
    "In general should be kept to -1, unless 'cheating' on purpose.");
DEFINE_string(cc_env_agg, "time", "State aggregation type for RL cc_algo");
DEFINE_int32(
    cc_env_time_window_ms,
    100,
    "Window duration (ms) for TIME_WINDOW aggregation");
DEFINE_int32(
    cc_env_fixed_window_size,
    10,
    "Window size for FIXED_WINDOW aggregation");
DEFINE_bool(
    cc_env_use_state_summary,
    true,
    "Whether to use state summary instead of raw states in "
    "observation (auto-enabled for TIME_WINDOW)");
DEFINE_int32(
    cc_env_history_size,
    2,
    "Length of history (such as past actions) to include in observation");
DEFINE_double(
    cc_env_norm_ms,
    100.0,
    "Normalization factor for temporal (in ms) fields in observation");
DEFINE_double(
    cc_env_norm_bytes,
    1000.0,
    "Normalization factor for byte fields in observation");
DEFINE_string(
    cc_env_actions,
    "0,/2,-10,+10,*2",
    "List of actions specifying how cwnd should be updated. The "
    "first action is required to be 0 (no-op action).");
DEFINE_double(
    cc_env_uplink_bandwidth,
    0.0,
    "Maximum bandwidth (in MBytes/s) achievable by the uplink");
DEFINE_int32(
    cc_env_uplink_queue_size_bytes,
    1,
    "Size of the uplink queue (in bytes)");
DEFINE_uint32(
    cc_env_base_rtt,
    1,
    "Minimum RTT that can be achieved based on network settings (in ms)");
DEFINE_string(
    cc_env_reward_formula,
    "log_ratio",
    "Which formula to use for the reward, among: "
    "linear, log_ratio, min_throughput "
    "(see pantheon_env.py for details)");
DEFINE_double(
    cc_env_reward_delay_offset,
    0.1,
    "Offset to remove from the delay when computing the reward (o)");
DEFINE_double(
    cc_env_reward_throughput_factor,
    0.1,
    "Throughput multiplier in reward (a)");
DEFINE_double(
    cc_env_reward_throughput_log_offset,
    1.0,
    "Offset to add to throughput in log version (a')");
DEFINE_double(
    cc_env_reward_delay_factor,
    0.01,
    "Delay multiplier in reward (b)");
DEFINE_double(
    cc_env_reward_delay_log_offset,
    1.0,
    "Offset to add to delay in log version (b')");
DEFINE_double(
    cc_env_reward_packet_loss_factor,
    0.0,
    "Packet loss multiplier in reward (c)");
DEFINE_double(
    cc_env_reward_packet_loss_log_offset,
    1.0,
    "Offset to add to packet loss in log version (c')");
DEFINE_double(
    cc_env_reward_min_throughput_ratio,
    0.9,
    "Min ratio of the maximum achievable throughput / target cwnd "
    "that we want to reach (r).");
DEFINE_double(
    cc_env_reward_max_throughput_ratio,
    1.0,
    "Max ratio of the maximum achievable throughput / target cwnd "
    "that we want to reach (r).");
DEFINE_double(
    cc_env_reward_n_packets_offset,
    1.0,
    "Offset to add to the estimated number of packets in the queue (k).");
DEFINE_double(
    cc_env_reward_uplink_queue_max_fill_ratio,
    0.5,
    "We allow the uplink queue to be filled up to this ratio without penalty (f)");
DEFINE_bool(
    cc_env_reward_max_delay,
    true,
    "Whether to take max delay over observations in reward."
    "Otherwise, avg delay is used.");
DEFINE_uint32(
    cc_env_fixed_cwnd,
    10,
    "Target fixed cwnd value (only used in 'fixed' env mode)");
DEFINE_uint64(
    cc_env_min_rtt_window_length_us,
    quic::kMinRTTWindowLength.count(),
    "Window length (in us) of min RTT filter used to estimate delay");
DEFINE_double(
    cc_env_ack_delay_avg_coeff,
    0.1,
    "Moving average coefficient used to compute the average ACK delay "
    "(weight of new observations: higher values update the average faster)");

namespace quic {
namespace tperf {

using Config = quic::CongestionControlEnv::Config;

std::shared_ptr<quic::CongestionControllerFactory>
makeRLCongestionControllerFactory() {
  Config cfg;

  if (FLAGS_cc_env_mode == "local") {
    cfg.mode = Config::Mode::LOCAL;
  } else if (FLAGS_cc_env_mode == "remote") {
    LOG(FATAL) << "Remote RL env is not currently supported";
  } else if (FLAGS_cc_env_mode == "random") {
    cfg.mode = Config::Mode::RANDOM;
  } else if (FLAGS_cc_env_mode == "fixed") {
    cfg.mode = Config::Mode::FIXED;
  } else {
    LOG(FATAL) << "Unknown cc_env_mode: " << FLAGS_cc_env_mode;
  }

  cfg.modelFile = FLAGS_cc_env_model_file;
  cfg.jobCount = FLAGS_cc_env_job_count;

  // These flags are only used in "remote" mode (not supported):
  cfg.rpcAddress = "";
  cfg.actorId = -1;
  /*
  cfg.rpcAddress = FLAGS_cc_env_rpc_address;
  cfg.actorId = FLAGS_cc_env_actor_id;
  */

  if (FLAGS_cc_env_agg == "time") {
    cfg.aggregation = Config::Aggregation::TIME_WINDOW;
  } else if (FLAGS_cc_env_agg == "fixed") {
    cfg.aggregation = Config::Aggregation::FIXED_WINDOW;
  } else {
    LOG(FATAL) << "Unknown cc_env_agg: " << FLAGS_cc_env_agg;
  }
  cfg.windowDuration = std::chrono::milliseconds(FLAGS_cc_env_time_window_ms);
  cfg.windowSize = FLAGS_cc_env_fixed_window_size;
  cfg.useStateSummary = FLAGS_cc_env_use_state_summary;

  cfg.historySize = FLAGS_cc_env_history_size;

  cfg.normMs = FLAGS_cc_env_norm_ms;
  cfg.normBytes = FLAGS_cc_env_norm_bytes;

  cfg.parseActionsFromString(FLAGS_cc_env_actions);

  if (FLAGS_cc_env_reward_formula == "linear") {
    cfg.rewardFormula = Config::RewardFormula::LINEAR;
  } else if (FLAGS_cc_env_reward_formula == "log_ratio") {
    cfg.rewardFormula = Config::RewardFormula::LOG_RATIO;
  } else if (FLAGS_cc_env_reward_formula == "min_throughput") {
    cfg.rewardFormula = Config::RewardFormula::MIN_THROUGHPUT;
  } else if (FLAGS_cc_env_reward_formula == "target_cwnd") {
    cfg.rewardFormula = Config::RewardFormula::TARGET_CWND;
  } else if (FLAGS_cc_env_reward_formula == "target_cwnd_shaped") {
    cfg.rewardFormula = Config::RewardFormula::TARGET_CWND_SHAPED;
  } else if (FLAGS_cc_env_reward_formula == "higher_is_better") {
    cfg.rewardFormula = Config::RewardFormula::HIGHER_IS_BETTER;
  } else if (FLAGS_cc_env_reward_formula == "above_cwnd") {
    cfg.rewardFormula = Config::RewardFormula::ABOVE_CWND;
  } else if (FLAGS_cc_env_reward_formula == "cwnd_range") {
    cfg.rewardFormula = Config::RewardFormula::CWND_RANGE;
  } else if (FLAGS_cc_env_reward_formula == "cwnd_range_soft") {
    cfg.rewardFormula = Config::RewardFormula::CWND_RANGE_SOFT;
  } else if (FLAGS_cc_env_reward_formula == "cwnd_tradeoff") {
    cfg.rewardFormula = Config::RewardFormula::CWND_TRADEOFF;
  } else {
    LOG(FATAL) << "Unknown cc_env_reward_formula: "
               << FLAGS_cc_env_reward_formula;
  }

  cfg.uplinkBandwidth = FLAGS_cc_env_uplink_bandwidth;
  cfg.uplinkQueueSizeBytes = FLAGS_cc_env_uplink_queue_size_bytes;
  cfg.baseRTT = FLAGS_cc_env_base_rtt;
  cfg.delayOffset = FLAGS_cc_env_reward_delay_offset;
  cfg.throughputFactor = FLAGS_cc_env_reward_throughput_factor;
  cfg.throughputLogOffset = FLAGS_cc_env_reward_throughput_log_offset;
  cfg.delayFactor = FLAGS_cc_env_reward_delay_factor;
  cfg.delayLogOffset = FLAGS_cc_env_reward_delay_log_offset;
  cfg.packetLossFactor = FLAGS_cc_env_reward_packet_loss_factor;
  cfg.packetLossLogOffset = FLAGS_cc_env_reward_packet_loss_log_offset;
  cfg.minThroughputRatio = FLAGS_cc_env_reward_min_throughput_ratio;
  cfg.maxThroughputRatio = FLAGS_cc_env_reward_max_throughput_ratio;
  cfg.nPacketsOffset = FLAGS_cc_env_reward_n_packets_offset;
  cfg.uplinkQueueMaxFillRatio = FLAGS_cc_env_reward_uplink_queue_max_fill_ratio;
  cfg.maxDelayInReward = FLAGS_cc_env_reward_max_delay;
  cfg.fixedCwnd = FLAGS_cc_env_fixed_cwnd;
  cfg.minRTTWindowLength =
      std::chrono::microseconds(FLAGS_cc_env_min_rtt_window_length_us);
  cfg.ackDelayAvgCoeff = FLAGS_cc_env_ack_delay_avg_coeff;

  auto envFactory = std::make_shared<quic::CongestionControlEnvFactory>(cfg);
  return std::make_shared<quic::RLCongestionControllerFactory>(envFactory);
}

namespace {

ProbeSizeRaiserType parseRaiserType(uint32_t type) {
  auto maybeRaiserType = static_cast<ProbeSizeRaiserType>(type);
  switch (maybeRaiserType) {
    case ProbeSizeRaiserType::ConstantStep:
    case ProbeSizeRaiserType::BinarySearch:
      return maybeRaiserType;
    default:
      throw std::runtime_error("Invalid raiser type, must be 0 or 1.");
  }
}

class TPerfInstrumentationObserver : public InstrumentationObserver {
 public:
  void observerDetach(QuicSocket* /* socket */) noexcept override {
    // do nothing
  }

  void appRateLimited(QuicSocket* /* socket */) override {
    if (FLAGS_log_app_rate_limited) {
      LOG(INFO) << "appRateLimited detected";
    }
  }

  void packetLossDetected(
      QuicSocket*, /* socket */
      const struct ObserverLossEvent& /* lossEvent */) override {
    if (FLAGS_log_loss) {
      LOG(INFO) << "packetLoss detected";
    }
  }

  void rttSampleGenerated(
      QuicSocket*, /* socket */
      const PacketRTT& /* RTT sample */) override {
    if (FLAGS_log_rtt_sample) {
      LOG(INFO) << "rttSample generated";
    }
  }

  void pmtuProbingStarted(QuicSocket* /* socket */) override {
    if (FLAGS_log_pmtu_probing_started) {
      LOG(INFO) << "pmtu probing started";
    }
  }

  void pmtuBlackholeDetected(
      QuicSocket*, /* socket */
      const PMTUBlackholeEvent& /* Blackhole event */) override {
    if (FLAGS_log_pmtu_blackhole) {
      LOG(INFO) << "pmtuBlackhole detected";
    }
  }

  void pmtuUpperBoundDetected(
      QuicSocket*, /* socket */
      const PMTUUpperBoundEvent& event) override {
    if (FLAGS_log_pmtu_upperbound) {
      LOG(INFO) << "pmtuUpperBound detected after "
                << event.cumulativeProbesSent << " d6d probes\n"
                << "pmtu upperbound is " << event.upperBoundPMTU;
    }
  }
};

/**
 * A helper accpetor observer that installs instrumentation observers to
 * transport upon accpet
 */
class TPerfAcceptObserver : public AcceptObserver {
 public:
  TPerfAcceptObserver()
      : tperfInstObserver_(std::make_unique<TPerfInstrumentationObserver>()) {}

  void accept(QuicTransportBase* transport) noexcept override {
    transport->addInstrumentationObserver(tperfInstObserver_.get());
  }

  void acceptorDestroy(QuicServerWorker* /* worker */) noexcept override {
    LOG(INFO) << "quic server worker destroyed";
  }

  void observerAttach(QuicServerWorker* /* worker */) noexcept override {
    LOG(INFO) << "TPerfAcceptObserver attached";
  }

  void observerDetach(QuicServerWorker* /* worker */) noexcept override {
    LOG(INFO) << "TPerfAcceptObserver detached";
  }

 private:
  std::unique_ptr<TPerfInstrumentationObserver> tperfInstObserver_;
};

} // namespace

class ServerStreamHandler : public quic::QuicSocket::ConnectionCallback,
                            public quic::QuicSocket::ReadCallback,
                            public quic::QuicSocket::WriteCallback {
 public:
  explicit ServerStreamHandler(
      folly::EventBase* evbIn,
      uint64_t blockSize,
      uint32_t numStreams,
      uint64_t maxBytesPerStream)
      : evb_(evbIn),
        blockSize_(blockSize),
        numStreams_(numStreams),
        maxBytesPerStream_(maxBytesPerStream) {}

  void setQuicSocket(std::shared_ptr<quic::QuicSocket> socket) {
    sock_ = socket;
  }

  void onNewBidirectionalStream(quic::StreamId id) noexcept override {
    LOG(INFO) << "Got bidirectional stream id=" << id;
    sock_->setReadCallback(id, this);
  }

  void onNewUnidirectionalStream(quic::StreamId id) noexcept override {
    LOG(INFO) << "Got unidirectional stream id=" << id;
    sock_->setReadCallback(id, this);
  }

  void onStopSending(
      quic::StreamId id,
      quic::ApplicationErrorCode error) noexcept override {
    LOG(INFO) << "Got StopSending stream id=" << id << " error=" << error;
  }

  void onConnectionEnd() noexcept override {
    LOG(INFO) << "Socket closed";
    sock_.reset();
  }

  void onConnectionError(
      std::pair<quic::QuicErrorCode, std::string> error) noexcept override {
    LOG(ERROR) << "Conn errorCoded=" << toString(error.first)
               << ", errorMsg=" << error.second;
  }

  void onTransportReady() noexcept override {
    LOG(INFO) << "Starting sends to client.";
    for (uint32_t i = 0; i < numStreams_; i++) {
      createNewStream();
    }
  }

  void createNewStream() noexcept {
    if (!sock_) {
      VLOG(4) << __func__ << ": socket is closed.";
      return;
    }
    auto stream = sock_->createUnidirectionalStream();
    VLOG(5) << "New Stream with id = " << stream.value();
    CHECK(stream.hasValue());
    bytesPerStream_[stream.value()] = 0;
    notifyDataForStream(stream.value());
  }

  void notifyDataForStream(quic::StreamId id) {
    evb_->runInEventBaseThread([&, id]() {
      if (!sock_) {
        VLOG(5) << "notifyDataForStream(" << id << "): socket is closed.";
        return;
      }
      auto res = sock_->notifyPendingWriteOnStream(id, this);
      if (res.hasError()) {
        LOG(FATAL) << quic::toString(res.error());
      }
    });
  }

  void readAvailable(quic::StreamId id) noexcept override {
    LOG(INFO) << "read available for stream id=" << id;
  }

  void readError(
      quic::StreamId id,
      std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
          error) noexcept override {
    LOG(ERROR) << "Got read error on stream=" << id
               << " error=" << toString(error);
    // A read error only terminates the ingress portion of the stream state.
    // Your application should probably terminate the egress portion via
    // resetStream
  }

  void onStreamWriteReady(quic::StreamId id, uint64_t maxToSend) noexcept
      override {
    bool eof = false;
    uint64_t toSend = maxToSend;
    if (maxBytesPerStream_ > 0) {
      toSend =
          std::min<uint64_t>(toSend, maxBytesPerStream_ - bytesPerStream_[id]);
      bytesPerStream_[id] += toSend;
      if (bytesPerStream_[id] >= maxBytesPerStream_) {
        eof = true;
      }
    }
    auto buf = folly::IOBuf::createChain(toSend, blockSize_);
    auto curBuf = buf.get();
    do {
      curBuf->append(curBuf->capacity());
      curBuf = curBuf->next();
    } while (curBuf != buf.get());
    auto res = sock_->writeChain(id, std::move(buf), eof, true, nullptr);
    if (res.hasError()) {
      LOG(FATAL) << "Got error on write: " << quic::toString(res.error());
    }
    if (!eof) {
      notifyDataForStream(id);
    } else {
      bytesPerStream_.erase(id);
      createNewStream();
    }
  }

  void onStreamWriteError(
      quic::StreamId id,
      std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
          error) noexcept override {
    LOG(ERROR) << "write error with stream=" << id
               << " error=" << toString(error);
  }

  folly::EventBase* getEventBase() {
    return evb_;
  }

 private:
  std::shared_ptr<quic::QuicSocket> sock_;
  folly::EventBase* evb_;
  uint64_t blockSize_;
  uint32_t numStreams_;
  uint64_t maxBytesPerStream_;
  std::unordered_map<quic::StreamId, uint64_t> bytesPerStream_;
};

class TPerfServerTransportFactory : public quic::QuicServerTransportFactory {
 public:
  ~TPerfServerTransportFactory() override = default;

  explicit TPerfServerTransportFactory(
      uint64_t blockSize,
      uint32_t numStreams,
      uint64_t maxBytesPerStream)
      : blockSize_(blockSize),
        numStreams_(numStreams),
        maxBytesPerStream_(maxBytesPerStream) {}

  quic::QuicServerTransport::Ptr make(
      folly::EventBase* evb,
      std::unique_ptr<folly::AsyncUDPSocket> sock,
      const folly::SocketAddress&,
      std::shared_ptr<const fizz::server::FizzServerContext> ctx) noexcept
      override {
    CHECK_EQ(evb, sock->getEventBase());
    auto serverHandler = std::make_unique<ServerStreamHandler>(
        evb, blockSize_, numStreams_, maxBytesPerStream_);
    auto transport = quic::QuicServerTransport::make(
        evb, std::move(sock), *serverHandler, ctx);
    if (!FLAGS_server_qlogger_path.empty()) {
      auto qlogger = std::make_shared<TperfQLogger>(
          VantagePoint::Server, FLAGS_server_qlogger_path);
      setPacingObserver(qlogger, transport.get(), FLAGS_pacing_observer);
      transport->setQLogger(std::move(qlogger));
    }
    serverHandler->setQuicSocket(transport);
    handlers_.push_back(std::move(serverHandler));
    return transport;
  }

 private:
  void setPacingObserver(
      std::shared_ptr<TperfQLogger>& qlogger,
      quic::QuicServerTransport* transport,
      const std::string& pacingObserverType) {
    if (pacingObserverType == "time") {
      qlogger->setPacingObserver(
          std::make_unique<FixedBucketQLogPacingObserver>(qlogger, 3ms));
    } else if (pacingObserverType == "rtt") {
      qlogger->setPacingObserver(std::make_unique<RttBucketQLogPacingObserver>(
          qlogger, *transport->getState()));
    } else if (pacingObserverType == "ack") {
      qlogger->setPacingObserver(std::make_unique<QLogPacingObserver>(qlogger));
    }
  }

  std::vector<std::unique_ptr<ServerStreamHandler>> handlers_;
  uint64_t blockSize_;
  uint32_t numStreams_;
  uint64_t maxBytesPerStream_;
};

class TPerfServer {
 public:
  explicit TPerfServer(
      const std::string& host,
      uint16_t port,
      uint64_t blockSize,
      uint64_t writesPerLoop,
      quic::CongestionControlType congestionControlType,
      bool gso,
      uint32_t maxCwndInMss,
      bool pacing,
      uint32_t numStreams,
      uint64_t maxBytesPerStream,
      uint32_t maxReceivePacketSize,
      bool useInplaceWrite)
      : host_(host),
        port_(port),
        acceptObserver_(std::make_unique<TPerfAcceptObserver>()),
        server_(QuicServer::createQuicServer()) {
    eventBase_.setName("tperf_server");
    server_->setQuicServerTransportFactory(
        std::make_unique<TPerfServerTransportFactory>(
            blockSize, numStreams, maxBytesPerStream));
    auto serverCtx = quic::test::createServerCtx();
    serverCtx->setClock(std::make_shared<fizz::SystemClock>());
    server_->setFizzContext(serverCtx);
    quic::TransportSettings settings;
    if (useInplaceWrite) {
      settings.dataPathType = DataPathType::ContinuousMemory;
    } else {
      settings.dataPathType = DataPathType::ChainedMemory;
    }
    settings.maxCwndInMss = maxCwndInMss;
    settings.writeConnectionDataPacketsLimit = writesPerLoop;
    settings.defaultCongestionController = congestionControlType;
    settings.pacingEnabled = pacing;
    if (pacing) {
      settings.pacingTimerTickInterval = 200us;
    }
    if (gso) {
      settings.batchingMode = QuicBatchingMode::BATCHING_MODE_GSO;
      settings.maxBatchSize = 16;
    }
    settings.maxRecvPacketSize = maxReceivePacketSize;
    settings.canIgnorePathMTU = !FLAGS_d6d_enabled;
    settings.copaDeltaParam = FLAGS_latency_factor;
    settings.d6dConfig.enabled = FLAGS_d6d_enabled;
    settings.d6dConfig.probeRaiserConstantStepSize =
        FLAGS_d6d_probe_raiser_constant_step_size;
    settings.d6dConfig.raiserType =
        parseRaiserType(FLAGS_d6d_probe_raiser_type);
    settings.d6dConfig.blackholeDetectionWindow =
        std::chrono::seconds(FLAGS_d6d_blackhole_detection_window_secs);
    settings.d6dConfig.blackholeDetectionThreshold =
        FLAGS_d6d_blackhole_detection_threshold;

    // RL-based congestion control uses a special factory.
    std::shared_ptr<quic::CongestionControllerFactory> ccFactory =
        std::make_shared<ServerCongestionControllerFactory>();
    if (congestionControlType == quic::CongestionControlType::RL) {
      ccFactory = makeRLCongestionControllerFactory();
    }
    server_->setCongestionControllerFactory(ccFactory);

    server_->setTransportSettings(settings);
    server_->setCcpConfig(FLAGS_ccp_config);
  }

  void start() {
    // Create a SocketAddress and the default or passed in host.
    folly::SocketAddress addr1(host_.c_str(), port_);
    addr1.setFromHostPort(host_, port_);
    server_->start(addr1, FLAGS_num_server_worker);
    auto workerEvbs = server_->getWorkerEvbs();
    for (auto evb : workerEvbs) {
      server_->addAcceptObserver(evb, acceptObserver_.get());
    }
    LOG(INFO) << "tperf server started at: " << addr1.describe();
    eventBase_.loopForever();
  }

 private:
  std::string host_;
  uint16_t port_;
  folly::EventBase eventBase_;
  std::unique_ptr<TPerfAcceptObserver> acceptObserver_;
  std::shared_ptr<quic::QuicServer> server_;
};

class TPerfClient : public quic::QuicSocket::ConnectionCallback,
                    public quic::QuicSocket::ReadCallback,
                    public quic::QuicSocket::WriteCallback,
                    public folly::HHWheelTimer::Callback {
 public:
  TPerfClient(
      const std::string& host,
      uint16_t port,
      std::chrono::milliseconds transportTimerResolution,
      int32_t duration,
      uint64_t window,
      bool gso,
      quic::CongestionControlType congestionControlType,
      uint32_t maxReceivePacketSize)
      : host_(host),
        port_(port),
        eventBase_(transportTimerResolution),
        duration_(duration),
        window_(window),
        gso_(gso),
        congestionControlType_(congestionControlType),
        maxReceivePacketSize_(maxReceivePacketSize) {
    eventBase_.setName("tperf_client");
  }

  void timeoutExpired() noexcept override {
    quicClient_->closeNow(folly::none);
    constexpr double bytesPerMegabit = 131072;
    LOG(INFO) << "Received " << receivedBytes_ << " bytes in "
              << duration_.count() << " seconds.";
    LOG(INFO) << "Overall throughput: "
              << (receivedBytes_ / bytesPerMegabit) / duration_.count()
              << "Mb/s";
    // Per Stream Stats
    LOG(INFO) << "Average per Stream throughput: "
              << ((receivedBytes_ / receivedStreams_) / bytesPerMegabit) /
            duration_.count()
              << "Mb/s over " << receivedStreams_ << " streams";
    if (receivedStreams_ != 1) {
      LOG(INFO) << "Histogram per Stream bytes: " << std::endl;
      LOG(INFO) << "Lo\tHi\tNum\tSum";
      for (const auto bytes : bytesPerStream_) {
        bytesPerStreamHistogram_.addValue(bytes.second);
      }
      std::ostringstream os;
      bytesPerStreamHistogram_.toTSV(os);
      std::vector<std::string> lines;
      folly::split("\n", os.str(), lines);
      for (const auto& line : lines) {
        LOG(INFO) << line;
      }
    }
  }

  virtual void callbackCanceled() noexcept override {}

  void readAvailable(quic::StreamId streamId) noexcept override {
    auto readData = quicClient_->read(streamId, 0);
    if (readData.hasError()) {
      LOG(FATAL) << "TPerfClient failed read from stream=" << streamId
                 << ", error=" << (uint32_t)readData.error();
    }

    auto readBytes = readData->first->computeChainDataLength();
    receivedBytes_ += readBytes;
    bytesPerStream_[streamId] += readBytes;
    if (readData.value().second) {
      bytesPerStreamHistogram_.addValue(bytesPerStream_[streamId]);
      bytesPerStream_.erase(streamId);
    }
  }

  void readError(
      quic::StreamId /*streamId*/,
      std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
      /*error*/) noexcept override {
    // A read error only terminates the ingress portion of the stream state.
    // Your application should probably terminate the egress portion via
    // resetStream
  }

  void onNewBidirectionalStream(quic::StreamId id) noexcept override {
    LOG(INFO) << "TPerfClient: new bidirectional stream=" << id;
    quicClient_->setReadCallback(id, this);
  }

  void onNewUnidirectionalStream(quic::StreamId id) noexcept override {
    VLOG(5) << "TPerfClient: new unidirectional stream=" << id;
    if (!timerScheduled_) {
      timerScheduled_ = true;
      eventBase_.timer().scheduleTimeout(this, duration_);
    }
    quicClient_->setReadCallback(id, this);
    receivedStreams_++;
  }

  void onTransportReady() noexcept override {
    LOG(INFO) << "TPerfClient: onTransportReady";
  }

  void onStopSending(
      quic::StreamId id,
      quic::ApplicationErrorCode /*error*/) noexcept override {
    VLOG(10) << "TPerfClient got StopSending stream id=" << id;
  }

  void onConnectionEnd() noexcept override {
    LOG(INFO) << "TPerfClient connection end";

    eventBase_.terminateLoopSoon();
  }

  void onConnectionError(
      std::pair<quic::QuicErrorCode, std::string> error) noexcept override {
    LOG(ERROR) << "TPerfClient error: " << toString(error.first);
    eventBase_.terminateLoopSoon();
  }

  void onStreamWriteReady(quic::StreamId id, uint64_t maxToSend) noexcept
      override {
    LOG(INFO) << "TPerfClient stream" << id
              << " is write ready with maxToSend=" << maxToSend;
  }

  void onStreamWriteError(
      quic::StreamId id,
      std::pair<quic::QuicErrorCode, folly::Optional<folly::StringPiece>>
          error) noexcept override {
    LOG(ERROR) << "TPerfClient write error with stream=" << id
               << " error=" << toString(error);
  }

  void start() {
    folly::SocketAddress addr(host_.c_str(), port_);

    auto sock = std::make_unique<folly::AsyncUDPSocket>(&eventBase_);
    auto fizzClientContext =
        FizzClientQuicHandshakeContext::Builder()
            .setCertificateVerifier(test::createTestCertificateVerifier())
            .build();
    quicClient_ = std::make_shared<quic::QuicClientTransport>(
        &eventBase_, std::move(sock), std::move(fizzClientContext));
    quicClient_->setHostname("tperf");
    quicClient_->addNewPeerAddress(addr);

    // RL-based congestion control uses a special factory.
    std::shared_ptr<quic::CongestionControllerFactory> ccFactory =
        std::make_shared<DefaultCongestionControllerFactory>();
    if (congestionControlType_ == quic::CongestionControlType::RL) {
      ccFactory = makeRLCongestionControllerFactory();
    }
    quicClient_->setCongestionControllerFactory(ccFactory);

    auto settings = quicClient_->getTransportSettings();
    settings.advertisedInitialUniStreamWindowSize = window_;
    // TODO figure out what actually to do with conn flow control and not sent
    // limit.
    settings.advertisedInitialConnectionWindowSize =
        std::numeric_limits<uint32_t>::max();
    settings.connectUDP = true;
    settings.shouldRecvBatch = true;
    settings.defaultCongestionController = congestionControlType_;
    if (congestionControlType_ == quic::CongestionControlType::BBR) {
      settings.pacingEnabled = true;
      settings.pacingTimerTickInterval = 200us;
    }
    if (gso_) {
      settings.batchingMode = QuicBatchingMode::BATCHING_MODE_GSO;
      settings.maxBatchSize = 16;
    }
    settings.maxRecvPacketSize = maxReceivePacketSize_;
    settings.canIgnorePathMTU = !FLAGS_d6d_enabled;
    settings.d6dConfig.enabled = FLAGS_d6d_enabled;
    settings.d6dConfig.advertisedBasePMTU = FLAGS_d6d_base_pmtu;
    settings.d6dConfig.advertisedRaiseTimeout =
        std::chrono::seconds(FLAGS_d6d_raise_timeout_secs);
    settings.d6dConfig.advertisedProbeTimeout =
        std::chrono::seconds(FLAGS_d6d_probe_timeout_secs);
    if (!FLAGS_transport_knob_params.empty()) {
      settings.knobs.push_back(
          {kDefaultQuicTransportKnobSpace,
           kDefaultQuicTransportKnobId,
           FLAGS_transport_knob_params});
    }
    quicClient_->setTransportSettings(settings);

    LOG(INFO) << "TPerfClient connecting to " << addr.describe();
    quicClient_->start(this);
    eventBase_.loopForever();
  }

  ~TPerfClient() override = default;

 private:
  bool timerScheduled_{false};
  std::string host_;
  uint16_t port_;
  std::shared_ptr<quic::QuicClientTransport> quicClient_;
  folly::EventBase eventBase_;
  uint64_t receivedBytes_{0};
  uint64_t receivedStreams_{0};
  std::map<quic::StreamId, uint64_t> bytesPerStream_;
  folly::Histogram<uint64_t> bytesPerStreamHistogram_{
      1024,
      0,
      1024 * 1024 * 1024};
  std::chrono::seconds duration_;
  uint64_t window_;
  bool gso_;
  quic::CongestionControlType congestionControlType_;
  uint32_t maxReceivePacketSize_;
};

} // namespace tperf
} // namespace quic

using namespace quic::tperf;

quic::CongestionControlType flagsToCongestionControlType(
    const std::string& congestionControlFlag) {
  auto ccType = quic::congestionControlStrToType(congestionControlFlag);
  if (!ccType) {
    throw std::invalid_argument(folly::to<std::string>(
        "Unknown congestion controller ", congestionControlFlag));
  }
  return *ccType;
}

int main(int argc, char* argv[]) {
#if FOLLY_HAVE_LIBGFLAGS
  // Enable glog logging to stderr by default.
  gflags::SetCommandLineOptionWithMode(
      "logtostderr", "1", gflags::SET_FLAGS_DEFAULT);
#endif
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  folly::Init init(&argc, &argv);
  fizz::CryptoUtils::init();

  if (FLAGS_mode == "server") {
    TPerfServer server(
        FLAGS_host,
        FLAGS_port,
        FLAGS_block_size,
        FLAGS_writes_per_loop,
        flagsToCongestionControlType(FLAGS_congestion),
        FLAGS_gso,
        FLAGS_max_cwnd_mss,
        FLAGS_pacing,
        FLAGS_num_streams,
        FLAGS_bytes_per_stream,
        FLAGS_max_receive_packet_size,
        FLAGS_use_inplace_write);
    server.start();
  } else if (FLAGS_mode == "client") {
    if (FLAGS_num_streams != 1) {
      LOG(ERROR) << "num_streams option is server only";
      return 1;
    }
    if (FLAGS_bytes_per_stream != 0) {
      LOG(ERROR) << "bytes_per_stream option is server only";
      return 1;
    }
    TPerfClient client(
        FLAGS_host,
        FLAGS_port,
        std::chrono::milliseconds(FLAGS_client_transport_timer_resolution_ms),
        FLAGS_duration,
        FLAGS_window,
        FLAGS_gso,
        flagsToCongestionControlType(FLAGS_congestion),
        FLAGS_max_receive_packet_size);
    client.start();
  }
  return 0;
}
