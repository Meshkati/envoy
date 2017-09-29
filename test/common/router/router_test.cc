#include <chrono>
#include <cstdint>
#include <string>

#include "common/buffer/buffer_impl.h"
#include "common/network/utility.h"
#include "common/router/router.h"
#include "common/upstream/upstream_impl.h"

#include "test/common/http/common.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/router/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/upstream/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AssertionFailure;
using testing::AssertionResult;
using testing::AssertionSuccess;
using testing::AtLeast;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;
using testing::_;

namespace Envoy {
namespace Router {

class TestFilter : public Filter {
public:
  using Filter::Filter;
  // Filter
  RetryStatePtr createRetryState(const RetryPolicy&, Http::HeaderMap&, const Upstream::ClusterInfo&,
                                 Runtime::Loader&, Runtime::RandomGenerator&, Event::Dispatcher&,
                                 Upstream::ResourcePriority) override {
    EXPECT_EQ(nullptr, retry_state_);
    retry_state_ = new NiceMock<MockRetryState>();
    return RetryStatePtr{retry_state_};
  }

  MockRetryState* retry_state_{};
};

class RouterTest : public testing::Test {
public:
  RouterTest()
      : shadow_writer_(new MockShadowWriter()),
        config_("test.", local_info_, stats_store_, cm_, runtime_, random_,
                ShadowWriterPtr{shadow_writer_}, true),
        router_(config_) {
    router_.setDecoderFilterCallbacks(callbacks_);
    upstream_locality_.set_zone("to_az");

    ON_CALL(*cm_.conn_pool_.host_, address()).WillByDefault(Return(host_address_));
    ON_CALL(*cm_.conn_pool_.host_, locality()).WillByDefault(ReturnRef(upstream_locality_));
  }

  void expectResponseTimerCreate() {
    response_timeout_ = new Event::MockTimer(&callbacks_.dispatcher_);
    EXPECT_CALL(*response_timeout_, enableTimer(_));
    EXPECT_CALL(*response_timeout_, disableTimer());
  }

  void expectPerTryTimerCreate() {
    per_try_timeout_ = new Event::MockTimer(&callbacks_.dispatcher_);
    EXPECT_CALL(*per_try_timeout_, enableTimer(_));
    EXPECT_CALL(*per_try_timeout_, disableTimer());
  }

  AssertionResult verifyHostUpstreamStats(uint64_t success, uint64_t error, uint64_t dropped) {
    if (success != cm_.conn_pool_.host_->stats_store_.counter("rq_success").value()) {
      return AssertionFailure() << fmt::format(
                 "rq_success {} does not match expected {}",
                 cm_.conn_pool_.host_->stats_store_.counter("rq_success").value(), success);
    }
    if (error != cm_.conn_pool_.host_->stats_store_.counter("rq_error").value()) {
      return AssertionFailure() << fmt::format(
                 "rq_error {} does not match expected {}",
                 cm_.conn_pool_.host_->stats_store_.counter("rq_error").value(), error);
    }
    if (dropped != cm_.conn_pool_.host_->stats_store_.counter("rq_dropped").value()) {
      return AssertionFailure() << fmt::format(
                 "rq_dropped {} does not match expected {}",
                 cm_.conn_pool_.host_->stats_store_.counter("rq_dropped").value(), dropped);
    }
    return AssertionSuccess();
  }

  std::string upstream_zone_{"to_az"};
  envoy::api::v2::Locality upstream_locality_;
  Stats::IsolatedStoreImpl stats_store_;
  NiceMock<Upstream::MockClusterManager> cm_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Runtime::MockRandomGenerator> random_;
  Http::ConnectionPool::MockCancellable cancellable_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> callbacks_;
  MockShadowWriter* shadow_writer_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  FilterConfig config_;
  TestFilter router_;
  Event::MockTimer* response_timeout_{};
  Event::MockTimer* per_try_timeout_{};
  Network::Address::InstanceConstSharedPtr host_address_{
      Network::Utility::resolveUrl("tcp://10.0.0.5:9211")};
};

TEST_F(RouterTest, RouteNotFound) {
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::NoRouteFound));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  EXPECT_CALL(callbacks_, route()).WillOnce(Return(nullptr));

  router_.decodeHeaders(headers, true);
  EXPECT_EQ(1UL, stats_store_.counter("test.no_route").value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, ClusterNotFound) {
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::NoRouteFound));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  ON_CALL(cm_, get(_)).WillByDefault(Return(nullptr));

  router_.decodeHeaders(headers, true);
  EXPECT_EQ(1UL, stats_store_.counter("test.no_cluster").value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, PoolFailureWithPriority) {
  ON_CALL(callbacks_.route_->route_entry_, priority())
      .WillByDefault(Return(Upstream::ResourcePriority::High));
  EXPECT_CALL(cm_, httpConnPoolForCluster(_, Upstream::ResourcePriority::High, &router_));

  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder&, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        callbacks.onPoolFailure(Http::ConnectionPool::PoolFailureReason::ConnectionFailure,
                                cm_.conn_pool_.host_);
        return nullptr;
      }));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "57"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamConnectionFailure));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

// Validate overfow (circuit breaker) request failure path and host stats.
TEST_F(RouterTest, PoolFailureOverflow) {
  EXPECT_CALL(cm_, httpConnPoolForCluster(_, Upstream::ResourcePriority::Default, &router_));

  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder&, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        callbacks.onPoolFailure(Http::ConnectionPool::PoolFailureReason::Overflow,
                                cm_.conn_pool_.host_);
        return nullptr;
      }));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "57"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamOverflow));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 1));
}

TEST_F(RouterTest, HashPolicy) {
  ON_CALL(callbacks_.route_->route_entry_, hashPolicy())
      .WillByDefault(Return(&callbacks_.route_->route_entry_.hash_policy_));
  EXPECT_CALL(callbacks_.route_->route_entry_.hash_policy_, generateHash(_, _))
      .WillOnce(Return(Optional<uint64_t>(10)));
  EXPECT_CALL(cm_, httpConnPoolForCluster(_, _, _))
      .WillOnce(
          Invoke([&](const std::string&, Upstream::ResourcePriority,
                     Upstream::LoadBalancerContext* context) -> Http::ConnectionPool::Instance* {
            EXPECT_EQ(10UL, context->hashKey().value());
            return &cm_.conn_pool_;
          }));
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _)).WillOnce(Return(&cancellable_));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // When the router filter gets reset we should cancel the pool request.
  EXPECT_CALL(cancellable_, cancel());
  router_.onDestroy();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, HashPolicyNoHash) {
  ON_CALL(callbacks_.route_->route_entry_, hashPolicy())
      .WillByDefault(Return(&callbacks_.route_->route_entry_.hash_policy_));
  EXPECT_CALL(callbacks_.route_->route_entry_.hash_policy_, generateHash(_, _))
      .WillOnce(Return(Optional<uint64_t>()));
  EXPECT_CALL(cm_, httpConnPoolForCluster(_, _, &router_))
      .WillOnce(
          Invoke([&](const std::string&, Upstream::ResourcePriority,
                     Upstream::LoadBalancerContext* context) -> Http::ConnectionPool::Instance* {
            EXPECT_EQ(false, context->hashKey().valid());
            return &cm_.conn_pool_;
          }));
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _)).WillOnce(Return(&cancellable_));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // When the router filter gets reset we should cancel the pool request.
  EXPECT_CALL(cancellable_, cancel());
  router_.onDestroy();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, CancelBeforeBoundToPool) {
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _)).WillOnce(Return(&cancellable_));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // When the router filter gets reset we should cancel the pool request.
  EXPECT_CALL(cancellable_, cancel());
  router_.onDestroy();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, NoHost) {
  EXPECT_CALL(cm_, httpConnPoolForCluster(_, _, _)).WillOnce(Return(nullptr));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "19"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::NoHealthyUpstream));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);
  EXPECT_EQ(0U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_rq_maintenance_mode")
                    .value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, MaintenanceMode) {
  EXPECT_CALL(*cm_.thread_local_cluster_.cluster_.info_, maintenanceMode()).WillOnce(Return(true));

  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "16"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamOverflow));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_rq_maintenance_mode")
                    .value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, NoRetriesOverflow) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // 5xx response.
  router_.retry_state_->expectRetry();
  Http::HeaderMapPtr response_headers1(new Http::TestHeaderMapImpl{{":status", "503"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  response_decoder->decodeHeaders(std::move(response_headers1), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // We expect the 5xx response to kick off a new request.
  EXPECT_CALL(encoder1.stream_, resetStream(_)).Times(0);
  NiceMock<Http::MockStreamEncoder> encoder2;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder2, cm_.conn_pool_.host_);
        return nullptr;
      }));
  router_.retry_state_->callback_();

  // RetryOverflow kicks in.
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamOverflow));
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _))
      .WillOnce(Return(RetryStatus::NoOverflow));
  EXPECT_CALL(cm_.conn_pool_.host_->health_checker_, setUnhealthy()).Times(0);
  Http::HeaderMapPtr response_headers2(new Http::TestHeaderMapImpl{{":status", "503"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  response_decoder->decodeHeaders(std::move(response_headers2), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 2, 0));
}

TEST_F(RouterTest, ResetDuringEncodeHeaders) {
  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));

  EXPECT_CALL(callbacks_, removeDownstreamWatermarkCallbacks(_));
  EXPECT_CALL(callbacks_, addDownstreamWatermarkCallbacks(_));
  EXPECT_CALL(encoder, encodeHeaders(_, true))
      .WillOnce(Invoke([&](const Http::HeaderMap&, bool) -> void {
        encoder.stream_.resetStream(Http::StreamResetReason::RemoteReset);
      }));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  router_.decodeHeaders(headers, true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, UpstreamTimeout) {
  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);
  Buffer::OwnedImpl data;
  router_.decodeData(data, true);

  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout));
  EXPECT_CALL(encoder.stream_, resetStream(Http::StreamResetReason::LocalReset));
  Http::TestHeaderMapImpl response_headers{
      {":status", "504"}, {"content-length", "24"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).Times(0);
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(504));
  response_timeout_->callback_();

  EXPECT_EQ(1U,
            cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("upstream_rq_timeout")
                .value());
  EXPECT_EQ(1UL, cm_.conn_pool_.host_->stats().rq_timeout_.value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

// Validate gRPC OK response stats are sane when response is trailers only.
TEST_F(RouterTest, GrpcOkTrailersOnly) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"}, {"grpc-status", "0"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));
}

// Validate gRPC AlreadyExists response stats are sane when response is trailers only.
TEST_F(RouterTest, GrpcAlreadyExistsTrailersOnly) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"}, {"grpc-status", "6"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));
}

// Validate gRPC Internal response stats are sane when response is trailers only.
TEST_F(RouterTest, GrpcInternalTrailersOnly) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"}, {"grpc-status", "13"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

// Validate gRPC response stats are sane when response is ended in a DATA
// frame.
TEST_F(RouterTest, GrpcDataEndStream) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), false);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
  Buffer::OwnedImpl data;
  response_decoder->decodeData(data, true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

// Validate gRPC response stats are sane when response is reset after initial
// response HEADERS.
TEST_F(RouterTest, GrpcReset) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), false);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  encoder1.stream_.resetStream(Http::StreamResetReason::RemoteReset);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

// Validate gRPC OK response stats are sane when response is not trailers only.
TEST_F(RouterTest, GrpcOk) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), false);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
  Http::HeaderMapPtr response_trailers(new Http::TestHeaderMapImpl{{"grpc-status", "0"}});
  response_decoder->decodeTrailers(std::move(response_trailers));
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));
}

// Validate gRPC Internal response stats are sane when response is not trailers only.
TEST_F(RouterTest, GrpcInternal) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), false);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
  Http::HeaderMapPtr response_trailers(new Http::TestHeaderMapImpl{{"grpc-status", "13"}});
  response_decoder->decodeTrailers(std::move(response_trailers));
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, UpstreamTimeoutWithAltResponse) {
  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-alt-response", "204"},
                                  {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);
  Buffer::OwnedImpl data;
  router_.decodeData(data, true);

  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout));
  EXPECT_CALL(encoder.stream_, resetStream(Http::StreamResetReason::LocalReset));
  Http::TestHeaderMapImpl response_headers{{":status", "204"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).Times(0);
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(204));
  response_timeout_->callback_();

  EXPECT_EQ(1U,
            cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("upstream_rq_timeout")
                .value());
  EXPECT_EQ(1UL, cm_.conn_pool_.host_->stats().rq_timeout_.value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, UpstreamPerTryTimeout) {
  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  expectResponseTimerCreate();
  expectPerTryTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-internal", "true"},
                                  {"x-envoy-upstream-rq-per-try-timeout-ms", "5"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);
  Buffer::OwnedImpl data;
  router_.decodeData(data, true);

  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout));
  EXPECT_CALL(encoder.stream_, resetStream(Http::StreamResetReason::LocalReset));
  Http::TestHeaderMapImpl response_headers{
      {":status", "504"}, {"content-length", "24"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(504));
  per_try_timeout_->callback_();

  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_rq_per_try_timeout")
                    .value());
  EXPECT_EQ(1UL, cm_.conn_pool_.host_->stats().rq_timeout_.value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, PerTryTimeoutWithNoUpstreamHost) {
  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        // simulate connect timeout, do not call callbacks.onPoolReady(...)
        UNREFERENCED_PARAMETER(callbacks);
        return &cancellable_;
      }));

  expectResponseTimerCreate();
  expectPerTryTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-internal", "true"},
                                  {"x-envoy-upstream-rq-per-try-timeout-ms", "5"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);
  Buffer::OwnedImpl data;
  router_.decodeData(data, true);

  EXPECT_CALL(cancellable_, cancel());
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout));
  Http::TestHeaderMapImpl response_headers{
      {":status", "504"}, {"content-length", "24"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  per_try_timeout_->callback_();

  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_rq_per_try_timeout")
                    .value());
  EXPECT_EQ(0UL, cm_.conn_pool_.host_->stats().rq_timeout_.value());
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST_F(RouterTest, RetryRequestNotComplete) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRemoteReset));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);

  router_.retry_state_->expectRetry();
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  encoder1.stream_.resetStream(Http::StreamResetReason::RemoteReset);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, RetryNoneHealthy) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));

  expectResponseTimerCreate();
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  router_.retry_state_->expectRetry();
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  encoder1.stream_.resetStream(Http::StreamResetReason::LocalReset);

  EXPECT_CALL(cm_, httpConnPoolForCluster(_, _, _)).WillOnce(Return(nullptr));
  Http::TestHeaderMapImpl response_headers{
      {":status", "503"}, {"content-length", "19"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::NoHealthyUpstream));
  router_.retry_state_->callback_();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, RetryUpstreamReset) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  router_.retry_state_->expectRetry();
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  encoder1.stream_.resetStream(Http::StreamResetReason::RemoteReset);

  // We expect this reset to kick off a new request.
  NiceMock<Http::MockStreamEncoder> encoder2;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder2, cm_.conn_pool_.host_);
        return nullptr;
      }));
  router_.retry_state_->callback_();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // Normal response.
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).WillOnce(Return(RetryStatus::No));
  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 1, 0));
}

TEST_F(RouterTest, RetryUpstreamPerTryTimeout) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();
  expectPerTryTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"},
                                  {"x-envoy-internal", "true"},
                                  {"x-envoy-upstream-rq-per-try-timeout-ms", "5"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  router_.retry_state_->expectRetry();
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(504));
  per_try_timeout_->callback_();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // We expect this reset to kick off a new request.
  NiceMock<Http::MockStreamEncoder> encoder2;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder2, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectPerTryTimerCreate();
  router_.retry_state_->callback_();

  // Normal response.
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).WillOnce(Return(RetryStatus::No));
  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 1, 0));
}

TEST_F(RouterTest, RetryUpstreamResetResponseStarted) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // Since the response is already started we don't retry.
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).WillOnce(Return(RetryStatus::No));
  EXPECT_CALL(callbacks_, encodeHeaders_(_, false));
  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), false);
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  encoder1.stream_.resetStream(Http::StreamResetReason::RemoteReset);
  // For normal HTTP, once we have a 200 we consider this a success, even if a
  // later reset occurs.
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));
}

TEST_F(RouterTest, RetryUpstream5xx) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // 5xx response.
  router_.retry_state_->expectRetry();
  Http::HeaderMapPtr response_headers1(new Http::TestHeaderMapImpl{{":status", "503"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  response_decoder->decodeHeaders(std::move(response_headers1), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // We expect the 5xx response to kick off a new request.
  EXPECT_CALL(encoder1.stream_, resetStream(_)).Times(0);
  NiceMock<Http::MockStreamEncoder> encoder2;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder2, cm_.conn_pool_.host_);
        return nullptr;
      }));
  router_.retry_state_->callback_();

  // Normal response.
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).WillOnce(Return(RetryStatus::No));
  EXPECT_CALL(cm_.conn_pool_.host_->health_checker_, setUnhealthy()).Times(0);
  Http::HeaderMapPtr response_headers2(new Http::TestHeaderMapImpl{{":status", "200"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers2), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 1, 0));
}

TEST_F(RouterTest, RetryTimeoutDuringRetryDelay) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // 5xx response.
  router_.retry_state_->expectRetry();
  Http::HeaderMapPtr response_headers1(new Http::TestHeaderMapImpl{{":status", "503"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  response_decoder->decodeHeaders(std::move(response_headers1), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // Fire timeout.
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout));

  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putResponseTime(_)).Times(0);
  Http::TestHeaderMapImpl response_headers{
      {":status", "504"}, {"content-length", "24"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  response_timeout_->callback_();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, RetryTimeoutDuringRetryDelayWithUpstreamRequestNoHost) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // 5xx response.
  router_.retry_state_->expectRetry();
  Http::HeaderMapPtr response_headers1(new Http::TestHeaderMapImpl{{":status", "503"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  response_decoder->decodeHeaders(std::move(response_headers1), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  Http::ConnectionPool::MockCancellable cancellable;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder,
                           Http::ConnectionPool::Callbacks&) -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        return &cancellable;
      }));
  router_.retry_state_->callback_();

  // Fire timeout.
  EXPECT_CALL(cancellable, cancel());
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRequestTimeout));

  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putResponseTime(_)).Times(0);
  Http::TestHeaderMapImpl response_headers{
      {":status", "504"}, {"content-length", "24"}, {"content-type", "text/plain"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), false));
  EXPECT_CALL(callbacks_, encodeData(_, true));
  response_timeout_->callback_();
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));
}

TEST_F(RouterTest, RetryUpstream5xxNotComplete) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);

  Buffer::InstancePtr body_data(new Buffer::OwnedImpl("hello"));
  EXPECT_CALL(*router_.retry_state_, enabled()).WillOnce(Return(true));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, router_.decodeData(*body_data, false));

  Http::TestHeaderMapImpl trailers{{"some", "trailer"}};
  router_.decodeTrailers(trailers);

  // 5xx response.
  router_.retry_state_->expectRetry();
  Http::HeaderMapPtr response_headers1(new Http::TestHeaderMapImpl{{":status", "503"}});
  EXPECT_CALL(encoder1.stream_, resetStream(Http::StreamResetReason::LocalReset));
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  response_decoder->decodeHeaders(std::move(response_headers1), false);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // We expect the 5xx response to kick off a new request.
  NiceMock<Http::MockStreamEncoder> encoder2;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder2, cm_.conn_pool_.host_);
        return nullptr;
      }));
  ON_CALL(callbacks_, decodingBuffer()).WillByDefault(Return(body_data.get()));
  EXPECT_CALL(encoder2, encodeHeaders(_, false));
  EXPECT_CALL(encoder2, encodeData(_, false));
  EXPECT_CALL(encoder2, encodeTrailers(_));
  router_.retry_state_->callback_();

  // Normal response.
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).WillOnce(Return(RetryStatus::No));
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putResponseTime(_));
  EXPECT_CALL(cm_.conn_pool_.host_->health_checker_, setUnhealthy());
  Http::HeaderMapPtr response_headers2(new Http::TestHeaderMapImpl{
      {":status", "200"}, {"x-envoy-immediate-health-check-fail", "true"}});
  response_decoder->decodeHeaders(std::move(response_headers2), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 1, 0));

  EXPECT_EQ(1U,
            cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("retry.upstream_rq_503")
                .value());
  EXPECT_EQ(
      1U,
      cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("upstream_rq_200").value());
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("zone.zone_name.to_az.upstream_rq_200")
                    .value());
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("zone.zone_name.to_az.upstream_rq_2xx")
                    .value());
}

TEST_F(RouterTest, RetryUpstreamGrpcCancelled) {
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  Http::TestHeaderMapImpl headers{{"x-envoy-grpc-retry-on", "cancelled"},
                                  {"x-envoy-internal", "true"},
                                  {"content-type", "application/grpc"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  // gRPC with status "cancelled" (1)
  router_.retry_state_->expectRetry();
  Http::HeaderMapPtr response_headers1(
      new Http::TestHeaderMapImpl{{":status", "200"}, {"grpc-status", "1"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers1), true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 1, 0));

  // We expect the grpc-status to result in a retried request.
  EXPECT_CALL(encoder1.stream_, resetStream(_)).Times(0);
  NiceMock<Http::MockStreamEncoder> encoder2;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder2, cm_.conn_pool_.host_);
        return nullptr;
      }));
  router_.retry_state_->callback_();

  // Normal response.
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).WillOnce(Return(RetryStatus::No));
  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"}, {"grpc-status", "0"}});
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 1, 0));
}

TEST_F(RouterTest, Shadow) {
  callbacks_.route_->route_entry_.shadow_policy_.cluster_ = "foo";
  callbacks_.route_->route_entry_.shadow_policy_.runtime_key_ = "bar";
  ON_CALL(callbacks_, streamId()).WillByDefault(Return(43));

  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));
  expectResponseTimerCreate();

  EXPECT_CALL(runtime_.snapshot_, featureEnabled("bar", 0, 43, 10000)).WillOnce(Return(true));

  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);

  Buffer::InstancePtr body_data(new Buffer::OwnedImpl("hello"));
  EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer, router_.decodeData(*body_data, false));

  Http::TestHeaderMapImpl trailers{{"some", "trailer"}};
  EXPECT_CALL(callbacks_, decodingBuffer())
      .Times(AtLeast(1))
      .WillRepeatedly(Return(body_data.get()));
  EXPECT_CALL(*shadow_writer_, shadow_("foo", _, std::chrono::milliseconds(10)))
      .WillOnce(Invoke(
          [](const std::string&, Http::MessagePtr& request, std::chrono::milliseconds) -> void {
            EXPECT_NE(nullptr, request->body());
            EXPECT_NE(nullptr, request->trailers());
          }));
  router_.decodeTrailers(trailers);

  Http::HeaderMapPtr response_headers(new Http::TestHeaderMapImpl{{":status", "200"}});
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));
}

TEST_F(RouterTest, AltStatName) {
  // Also test no upstream timeout here.
  EXPECT_CALL(callbacks_.route_->route_entry_, timeout())
      .WillOnce(Return(std::chrono::milliseconds(0)));
  EXPECT_CALL(callbacks_.dispatcher_, createTimer_(_)).Times(0);

  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));

  Http::TestHeaderMapImpl headers{{"x-envoy-upstream-alt-stat-name", "alt_stat"},
                                  {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(200));
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putResponseTime(_));

  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"},
                                  {"x-envoy-upstream-canary", "true"},
                                  {"x-envoy-virtual-cluster", "hello"}});
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));

  EXPECT_EQ(1U,
            stats_store_.counter("vhost.fake_vhost.vcluster.fake_virtual_cluster.upstream_rq_200")
                .value());
  EXPECT_EQ(1U,
            cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("canary.upstream_rq_200")
                .value());
  EXPECT_EQ(
      1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("alt_stat.upstream_rq_200")
              .value());
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("alt_stat.zone.zone_name.to_az.upstream_rq_200")
                    .value());
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("alt_stat.zone.zone_name.to_az.upstream_rq_200")
                    .value());
}

TEST_F(RouterTest, Redirect) {
  MockRedirectEntry redirect;
  EXPECT_CALL(redirect, newPath(_)).WillOnce(Return("hello"));
  EXPECT_CALL(*callbacks_.route_, redirectEntry()).WillRepeatedly(Return(&redirect));

  Http::TestHeaderMapImpl response_headers{{":status", "301"}, {"location", "hello"}};
  EXPECT_CALL(callbacks_, encodeHeaders_(HeaderMapEqualRef(&response_headers), true));
  Http::TestHeaderMapImpl headers;
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);
  EXPECT_TRUE(verifyHostUpstreamStats(0, 0, 0));
}

TEST(RouterFilterUtilityTest, finalTimeout) {
  {
    NiceMock<MockRouteEntry> route;
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers;
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(10), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(0), timeout.per_try_timeout_);
  }
  {
    NiceMock<MockRouteEntry> route;
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-ms", "15"}};
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(15), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(0), timeout.per_try_timeout_);
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-timeout-ms"));
    EXPECT_EQ("15", headers.get_("x-envoy-expected-rq-timeout-ms"));
  }
  {
    NiceMock<MockRouteEntry> route;
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-ms", "bad"}};
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(10), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(0), timeout.per_try_timeout_);
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-timeout-ms"));
    EXPECT_EQ("10", headers.get_("x-envoy-expected-rq-timeout-ms"));
  }
  {
    NiceMock<MockRouteEntry> route;
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-ms", "15"},
                                    {"x-envoy-upstream-rq-per-try-timeout-ms", "15"}};
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(15), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(0), timeout.per_try_timeout_);
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-timeout-ms"));
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-per-try-timeout-ms"));
    EXPECT_EQ("15", headers.get_("x-envoy-expected-rq-timeout-ms"));
  }
  {
    NiceMock<MockRouteEntry> route;
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-ms", "15"},
                                    {"x-envoy-upstream-rq-per-try-timeout-ms", "5"}};
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(15), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(5), timeout.per_try_timeout_);
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-timeout-ms"));
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-per-try-timeout-ms"));
    EXPECT_EQ("5", headers.get_("x-envoy-expected-rq-timeout-ms"));
  }
  {
    NiceMock<MockRouteEntry> route;
    route.retry_policy_.per_try_timeout_ = std::chrono::milliseconds(7);
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-ms", "15"}};
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(15), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(7), timeout.per_try_timeout_);
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-timeout-ms"));
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-per-try-timeout-ms"));
    EXPECT_EQ("7", headers.get_("x-envoy-expected-rq-timeout-ms"));
  }
  {
    NiceMock<MockRouteEntry> route;
    route.retry_policy_.per_try_timeout_ = std::chrono::milliseconds(7);
    EXPECT_CALL(route, timeout()).WillOnce(Return(std::chrono::milliseconds(10)));
    Http::TestHeaderMapImpl headers{{"x-envoy-upstream-rq-timeout-ms", "15"},
                                    {"x-envoy-upstream-rq-per-try-timeout-ms", "5"}};
    FilterUtility::TimeoutData timeout = FilterUtility::finalTimeout(route, headers);
    EXPECT_EQ(std::chrono::milliseconds(15), timeout.global_timeout_);
    EXPECT_EQ(std::chrono::milliseconds(5), timeout.per_try_timeout_);
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-timeout-ms"));
    EXPECT_FALSE(headers.has("x-envoy-upstream-rq-per-try-timeout-ms"));
    EXPECT_EQ("5", headers.get_("x-envoy-expected-rq-timeout-ms"));
  }
}

TEST(RouterFilterUtilityTest, setUpstreamScheme) {
  {
    Upstream::MockClusterInfo cluster;
    Http::TestHeaderMapImpl headers;
    EXPECT_CALL(cluster, sslContext()).WillOnce(Return(nullptr));
    FilterUtility::setUpstreamScheme(headers, cluster);
    EXPECT_EQ("http", headers.get_(":scheme"));
  }

  {
    Upstream::MockClusterInfo cluster;
    Ssl::MockClientContext context;
    Http::TestHeaderMapImpl headers;
    EXPECT_CALL(cluster, sslContext()).WillOnce(Return(&context));
    FilterUtility::setUpstreamScheme(headers, cluster);
    EXPECT_EQ("https", headers.get_(":scheme"));
  }
}

TEST(RouterFilterUtilityTest, shouldShadow) {
  {
    TestShadowPolicy policy;
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled(_, _, _, _)).Times(0);
    EXPECT_FALSE(FilterUtility::shouldShadow(policy, runtime, 5));
  }
  {
    TestShadowPolicy policy;
    policy.cluster_ = "cluster";
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled(_, _, _, _)).Times(0);
    EXPECT_TRUE(FilterUtility::shouldShadow(policy, runtime, 5));
  }
  {
    TestShadowPolicy policy;
    policy.cluster_ = "cluster";
    policy.runtime_key_ = "foo";
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("foo", 0, 5, 10000)).WillOnce(Return(false));
    EXPECT_FALSE(FilterUtility::shouldShadow(policy, runtime, 5));
  }
  {
    TestShadowPolicy policy;
    policy.cluster_ = "cluster";
    policy.runtime_key_ = "foo";
    NiceMock<Runtime::MockLoader> runtime;
    EXPECT_CALL(runtime.snapshot_, featureEnabled("foo", 0, 5, 10000)).WillOnce(Return(true));
    EXPECT_TRUE(FilterUtility::shouldShadow(policy, runtime, 5));
  }
}

TEST_F(RouterTest, CanaryStatusTrue) {
  EXPECT_CALL(callbacks_.route_->route_entry_, timeout())
      .WillOnce(Return(std::chrono::milliseconds(0)));
  EXPECT_CALL(callbacks_.dispatcher_, createTimer_(_)).Times(0);

  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));

  Http::TestHeaderMapImpl headers{{"x-envoy-upstream-alt-stat-name", "alt_stat"},
                                  {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"},
                                  {"x-envoy-upstream-canary", "false"},
                                  {"x-envoy-virtual-cluster", "hello"}});
  ON_CALL(*cm_.conn_pool_.host_, canary()).WillByDefault(Return(true));
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));

  EXPECT_EQ(1U,
            cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("canary.upstream_rq_200")
                .value());
}

TEST_F(RouterTest, CanaryStatusFalse) {
  EXPECT_CALL(callbacks_.route_->route_entry_, timeout())
      .WillOnce(Return(std::chrono::milliseconds(0)));
  EXPECT_CALL(callbacks_.dispatcher_, createTimer_(_)).Times(0);

  NiceMock<Http::MockStreamEncoder> encoder;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder& decoder, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        response_decoder = &decoder;
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));

  Http::TestHeaderMapImpl headers{{"x-envoy-upstream-alt-stat-name", "alt_stat"},
                                  {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, true);

  Http::HeaderMapPtr response_headers(
      new Http::TestHeaderMapImpl{{":status", "200"},
                                  {"x-envoy-upstream-canary", "false"},
                                  {"x-envoy-virtual-cluster", "hello"}});
  response_decoder->decodeHeaders(std::move(response_headers), true);
  EXPECT_TRUE(verifyHostUpstreamStats(1, 0, 0));

  EXPECT_EQ(0U,
            cm_.thread_local_cluster_.cluster_.info_->stats_store_.counter("canary.upstream_rq_200")
                .value());
}

TEST_F(RouterTest, AutoHostRewriteEnabled) {
  NiceMock<Http::MockStreamEncoder> encoder;
  std::string req_host{"foo.bar.com"};

  Http::TestHeaderMapImpl incoming_headers;
  HttpTestUtility::addDefaultHeaders(incoming_headers);
  incoming_headers.Host()->value(req_host);

  cm_.conn_pool_.host_->hostname_ = "scooby.doo";
  Http::TestHeaderMapImpl outgoing_headers;
  HttpTestUtility::addDefaultHeaders(outgoing_headers);
  outgoing_headers.Host()->value(cm_.conn_pool_.host_->hostname_);

  EXPECT_CALL(callbacks_.route_->route_entry_, timeout())
      .WillOnce(Return(std::chrono::milliseconds(0)));

  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder&, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));

  // :authority header in the outgoing request should match the DNS name of
  // the selected upstream host
  EXPECT_CALL(encoder, encodeHeaders(HeaderMapEqualRef(&outgoing_headers), true))
      .WillOnce(Invoke([&](const Http::HeaderMap&, bool) -> void {
        encoder.stream_.resetStream(Http::StreamResetReason::RemoteReset);
      }));

  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));
  EXPECT_CALL(callbacks_.route_->route_entry_, autoHostRewrite()).WillOnce(Return(true));
  router_.decodeHeaders(incoming_headers, true);
}

TEST_F(RouterTest, AutoHostRewriteDisabled) {
  NiceMock<Http::MockStreamEncoder> encoder;
  std::string req_host{"foo.bar.com"};

  Http::TestHeaderMapImpl incoming_headers;
  HttpTestUtility::addDefaultHeaders(incoming_headers);
  incoming_headers.Host()->value(req_host);

  cm_.conn_pool_.host_->hostname_ = "scooby.doo";

  EXPECT_CALL(callbacks_.route_->route_entry_, timeout())
      .WillOnce(Return(std::chrono::milliseconds(0)));

  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillOnce(Invoke([&](Http::StreamDecoder&, Http::ConnectionPool::Callbacks& callbacks)
                           -> Http::ConnectionPool::Cancellable* {
        callbacks.onPoolReady(encoder, cm_.conn_pool_.host_);
        return nullptr;
      }));

  // :authority header in the outgoing request should match the :authority header of
  // the incoming request
  EXPECT_CALL(encoder, encodeHeaders(HeaderMapEqualRef(&incoming_headers), true))
      .WillOnce(Invoke([&](const Http::HeaderMap&, bool) -> void {
        encoder.stream_.resetStream(Http::StreamResetReason::RemoteReset);
      }));

  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillOnce(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));
  EXPECT_CALL(callbacks_.route_->route_entry_, autoHostRewrite()).WillOnce(Return(false));
  router_.decodeHeaders(incoming_headers, true);
}

class WatermarkTest : public RouterTest {
public:
  void sendRequest(bool header_only_request = true, bool pool_ready = true) {
    EXPECT_CALL(callbacks_.route_->route_entry_, timeout())
        .WillOnce(Return(std::chrono::milliseconds(0)));
    EXPECT_CALL(callbacks_.dispatcher_, createTimer_(_)).Times(0);

    EXPECT_CALL(stream_, addCallbacks(_)).WillOnce(Invoke([&](Http::StreamCallbacks& callbacks) {
      stream_callbacks_ = &callbacks;
    }));
    EXPECT_CALL(encoder_, getStream()).WillOnce(ReturnRef(stream_));
    EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
        .WillOnce(Invoke(
            [&](Http::StreamDecoder& decoder,
                Http::ConnectionPool::Callbacks& callbacks) -> Http::ConnectionPool::Cancellable* {
              response_decoder_ = &decoder;
              pool_callbacks_ = &callbacks;
              if (pool_ready) {
                callbacks.onPoolReady(encoder_, cm_.conn_pool_.host_);
              }
              return nullptr;
            }));
    HttpTestUtility::addDefaultHeaders(headers_);
    router_.decodeHeaders(headers_, header_only_request);
  }
  void sendResponse() {
    response_decoder_->decodeHeaders(
        Http::HeaderMapPtr{new Http::TestHeaderMapImpl{{":status", "200"}}}, true);
  }

  NiceMock<Http::MockStreamEncoder> encoder_;
  NiceMock<Http::MockStream> stream_;
  Http::StreamCallbacks* stream_callbacks_;
  Http::StreamDecoder* response_decoder_ = nullptr;
  Http::TestHeaderMapImpl headers_;
  Http::ConnectionPool::Callbacks* pool_callbacks_{nullptr};
};

TEST_F(WatermarkTest, DownstreamWatermarks) {
  sendRequest();

  stream_callbacks_->onAboveWriteBufferHighWatermark();
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_backed_up_total")
                    .value());
  stream_callbacks_->onBelowWriteBufferLowWatermark();
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_drained_total")
                    .value());

  sendResponse();
}

TEST_F(WatermarkTest, UpstreamWatermarks) {
  sendRequest();

  ASSERT(callbacks_.callbacks_.begin() != callbacks_.callbacks_.end());
  Envoy::Http::DownstreamWatermarkCallbacks* watermark_callbacks = *callbacks_.callbacks_.begin();

  EXPECT_CALL(encoder_, getStream()).WillOnce(ReturnRef(stream_));
  EXPECT_CALL(stream_, readDisable(_));
  watermark_callbacks->onAboveWriteBufferHighWatermark();
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_paused_reading_total")
                    .value());

  EXPECT_CALL(encoder_, getStream()).WillOnce(ReturnRef(stream_));
  EXPECT_CALL(stream_, readDisable(_));
  watermark_callbacks->onBelowWriteBufferLowWatermark();
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_resumed_reading_total")
                    .value());

  sendResponse();
}

TEST_F(WatermarkTest, FilterWatermarks) {
  EXPECT_CALL(callbacks_, decoderBufferLimit()).WillOnce(Return(10));
  router_.setDecoderFilterCallbacks(callbacks_);
  // Send the headers sans-fin, and don't flag the pool as ready.
  sendRequest(false, false);

  // Send 10 bytes of body to fill the 10 byte buffer.
  Buffer::OwnedImpl data("1234567890");
  router_.decodeData(data, false);
  EXPECT_EQ(0u, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_backed_up_total")
                    .value());

  // Send one extra byte.  This should cause the buffer to go over the limit and pause downstream
  // data.
  Buffer::OwnedImpl last_byte("!");
  router_.decodeData(last_byte, true);
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_backed_up_total")
                    .value());

  // Now set up the downstream connection.  The encoder will be given the buffered request body,
  // The mock invocation below drains it, and the buffer will go under the watermark limit again.
  EXPECT_EQ(0U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_drained_total")
                    .value());
  EXPECT_CALL(encoder_, encodeData(_, true))
      .WillOnce(Invoke([&](Buffer::Instance& data, bool) -> void { data.drain(data.length()); }));
  pool_callbacks_->onPoolReady(encoder_, cm_.conn_pool_.host_);
  EXPECT_EQ(1U, cm_.thread_local_cluster_.cluster_.info_->stats_store_
                    .counter("upstream_flow_control_drained_total")
                    .value());

  sendResponse();
} // namespace Router

// Same as RetryRequestNotComplete but with decodeData larger than the buffer
// limit, no retry will occur.
TEST_F(WatermarkTest, RetryRequestNotComplete) {
  EXPECT_CALL(callbacks_, decoderBufferLimit()).WillOnce(Return(10));
  router_.setDecoderFilterCallbacks(callbacks_);
  NiceMock<Http::MockStreamEncoder> encoder1;
  Http::StreamDecoder* response_decoder = nullptr;
  EXPECT_CALL(cm_.conn_pool_, newStream(_, _))
      .WillRepeatedly(Invoke(
          [&](Http::StreamDecoder& decoder,
              Http::ConnectionPool::Callbacks& callbacks) -> Http::ConnectionPool::Cancellable* {
            response_decoder = &decoder;
            callbacks.onPoolReady(encoder1, cm_.conn_pool_.host_);
            return nullptr;
          }));
  EXPECT_CALL(callbacks_.request_info_,
              setResponseFlag(Http::AccessLog::ResponseFlag::UpstreamRemoteReset));
  EXPECT_CALL(callbacks_.request_info_, onUpstreamHostSelected(_))
      .WillRepeatedly(Invoke([&](const Upstream::HostDescriptionConstSharedPtr host) -> void {
        EXPECT_EQ(host_address_, host->address());
      }));

  Http::TestHeaderMapImpl headers{{"x-envoy-retry-on", "5xx"}, {"x-envoy-internal", "true"}};
  HttpTestUtility::addDefaultHeaders(headers);
  router_.decodeHeaders(headers, false);
  Buffer::OwnedImpl data("1234567890123");
  EXPECT_CALL(*router_.retry_state_, enabled()).Times(1).WillOnce(Return(true));
  EXPECT_CALL(*router_.retry_state_, shouldRetry(_, _, _)).Times(0);
  // This will result in retry_state_ being deleted.
  router_.decodeData(data, false);

  // This should not trigger a retry as the retry state has been deleted.
  EXPECT_CALL(cm_.conn_pool_.host_->outlier_detector_, putHttpResponseCode(503));
  encoder1.stream_.resetStream(Http::StreamResetReason::RemoteReset);
}

} // namespace Router
} // namespace Envoy
