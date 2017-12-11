#include "common/buffer/zero_copy_input_stream_impl.h"
#include "common/common/version.h"
#include "common/grpc/codec.h"
#include "common/grpc/common.h"

#include "test/integration/http_integration.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace {

class AccessLogIntegrationTest : public HttpIntegrationTest,
                                 public testing::TestWithParam<Network::Address::IpVersion> {
public:
  AccessLogIntegrationTest() : HttpIntegrationTest(Http::CodecClient::Type::HTTP1, GetParam()) {}

  void createUpstreams() override {
    HttpIntegrationTest::createUpstreams();
    fake_upstreams_.emplace_back(new FakeUpstream(0, FakeHttpConnection::Type::HTTP2, version_));
    ports_.push_back(fake_upstreams_.back()->localAddress()->ip()->port());
  }

  void initialize() override {
    config_helper_.addConfigModifier([](envoy::api::v2::Bootstrap& bootstrap) {
      bootstrap.mutable_rate_limit_service()->set_cluster_name("accesslog");
      auto* ratelimit_cluster = bootstrap.mutable_static_resources()->add_clusters();
      ratelimit_cluster->MergeFrom(bootstrap.static_resources().clusters()[0]);
      ratelimit_cluster->set_name("accesslog");
      ratelimit_cluster->mutable_http2_protocol_options();
    });

    config_helper_.addConfigModifier(
        [](envoy::api::v2::filter::network::HttpConnectionManager& hcm) {
          auto* access_log = hcm.add_access_log();
          access_log->set_name("envoy.http_grpc_access_log");

          envoy::api::v2::filter::accesslog::HttpGrpcAccessLogConfig config;
          auto* common_config = config.mutable_common_config();
          common_config->set_log_name("foo");
          common_config->set_cluster_name("accesslog");
          MessageUtil::jsonConvert(config, *access_log->mutable_config());
        });

    HttpIntegrationTest::initialize();
  }

  /*void initiateClientConnection() {
    auto conn = makeClientConnection(lookupPort("http"));
    codec_client_ = makeHttpConnection(std::move(conn));
    Http::TestHeaderMapImpl headers{{":method", "POST"},       {":path", "/test/long/url"},
                                    {":scheme", "http"},       {":authority", "host"},
                                    {"x-lyft-user-id", "123"}, {"x-forwarded-for", "10.0.0.1"}};
    codec_client_->makeRequestWithBody(headers, request_size_, *response_);
  }*/

  void waitForAccessLogRequest() {
    fake_access_log_connection_ = fake_upstreams_[1]->waitForHttpConnection(*dispatcher_);
    access_log_request_ = fake_access_log_connection_->waitForNewStream(*dispatcher_);
    envoy::api::v2::filter::accesslog::StreamAccessLogsMessage request_msg;
    access_log_request_->waitForGrpcMessage(*dispatcher_, request_msg);
    EXPECT_STREQ("POST", access_log_request_->headers().Method()->value().c_str());
    EXPECT_STREQ("/envoy.api.v2.filter.accesslog.AccessLogService/StreamAccessLogs",
                 access_log_request_->headers().Path()->value().c_str());
    EXPECT_STREQ("application/grpc", access_log_request_->headers().ContentType()->value().c_str());

    const std::string EXPECTED_ACCESS_LOG =
        R"EOF(
identifier:
  node:
    id: node_name
    cluster: cluster_name
    locality:
      zone: zone_name
    build_version: {}
  log_name: foo
)EOF";

    envoy::api::v2::filter::accesslog::StreamAccessLogsMessage expected_request_msg;
    MessageUtil::loadFromYaml(fmt::format(EXPECTED_ACCESS_LOG, VersionInfo::version()),
                              expected_request_msg);
    EXPECT_EQ(request_msg.DebugString(), expected_request_msg.DebugString());
  }

  void cleanup() {
    if (fake_access_log_connection_ != nullptr) {
      fake_access_log_connection_->close();
      fake_access_log_connection_->waitForDisconnect();
    }
  }

  FakeHttpConnectionPtr fake_access_log_connection_;
  FakeStreamPtr access_log_request_;
};

INSTANTIATE_TEST_CASE_P(IpVersions, AccessLogIntegrationTest,
                        testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));

TEST_P(AccessLogIntegrationTest, Ok) {
  testRouterNotFound();
  waitForAccessLogRequest();
  // sendRateLimitResponse(pb::lyft::ratelimit::RateLimitResponse_Code_OK);
  cleanup();

  // EXPECT_EQ(1, test_server_->counter("cluster.cluster_0.ratelimit.ok")->value());
  // EXPECT_EQ(nullptr, test_server_->counter("cluster.cluster_0.ratelimit.over_limit"));
  // EXPECT_EQ(nullptr, test_server_->counter("cluster.cluster_0.ratelimit.error"));
}

/*TEST_P(AccessLogIntegrationTest, OverLimit) {
  initiateClientConnection();
  waitForRatelimitRequest();
  sendRateLimitResponse(pb::lyft::ratelimit::RateLimitResponse_Code_OVER_LIMIT);
  waitForFailedUpstreamResponse(429);
  cleanup();

  EXPECT_EQ(nullptr, test_server_->counter("cluster.cluster_0.ratelimit.ok"));
  EXPECT_EQ(1, test_server_->counter("cluster.cluster_0.ratelimit.over_limit")->value());
  EXPECT_EQ(nullptr, test_server_->counter("cluster.cluster_0.ratelimit.error"));
}

TEST_P(AccessLogIntegrationTest, Error) {
  initiateClientConnection();
  waitForRatelimitRequest();
  ratelimit_request_->encodeHeaders(Http::TestHeaderMapImpl{{":status", "404"}}, true);
  // Rate limiter fails open
  waitForSuccessfulUpstreamResponse();
  cleanup();

  EXPECT_EQ(nullptr, test_server_->counter("cluster.cluster_0.ratelimit.ok"));
  EXPECT_EQ(nullptr, test_server_->counter("cluster.cluster_0.ratelimit.over_limit"));
  EXPECT_EQ(1, test_server_->counter("cluster.cluster_0.ratelimit.error")->value());
}

TEST_P(AccessLogIntegrationTest, Timeout) {
  initiateClientConnection();
  waitForRatelimitRequest();
  test_server_->waitForCounterGe("cluster.ratelimit.upstream_rq_timeout", 1);
  // Rate limiter fails open
  waitForSuccessfulUpstreamResponse();
  cleanup();

  EXPECT_EQ(1, test_server_->counter("cluster.ratelimit.upstream_rq_timeout")->value());
  EXPECT_EQ(1, test_server_->counter("cluster.ratelimit.upstream_rq_504")->value());
}

TEST_P(AccessLogIntegrationTest, ConnectImmediateDisconnect) {
  initiateClientConnection();
  fake_ratelimit_connection_ = fake_upstreams_[1]->waitForHttpConnection(*dispatcher_);
  fake_ratelimit_connection_->close();
  fake_ratelimit_connection_->waitForDisconnect(true);
  fake_ratelimit_connection_ = nullptr;
  // Rate limiter fails open
  waitForSuccessfulUpstreamResponse();
  cleanup();
}

TEST_P(AccessLogIntegrationTest, FailedConnect) {
  fake_upstreams_[1].reset();
  initiateClientConnection();
  // Rate limiter fails open
  waitForSuccessfulUpstreamResponse();
  cleanup();
}*/

} // namespace
} // namespace Envoy
