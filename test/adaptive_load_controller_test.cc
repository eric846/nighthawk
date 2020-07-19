#include <grpc++/grpc++.h>

#include <chrono>
#include <iostream>

#include "envoy/registry/registry.h"

#include "nighthawk/adaptive_load/adaptive_load_controller.h"
#include "nighthawk/adaptive_load/input_variable_setter.h"
#include "nighthawk/adaptive_load/metrics_plugin.h"
#include "nighthawk/adaptive_load/scoring_function.h"
#include "nighthawk/adaptive_load/step_controller.h"

#include "external/envoy/source/common/config/utility.h"
#include "external/envoy/source/common/protobuf/protobuf.h"

#include "api/adaptive_load/adaptive_load.pb.h"
#include "api/adaptive_load/benchmark_result.pb.h"
#include "api/adaptive_load/input_variable_setter_impl.pb.h"
#include "api/adaptive_load/metric_spec.pb.h"
#include "api/adaptive_load/metrics_plugin_impl.pb.h"
#include "api/adaptive_load/scoring_function_impl.pb.h"
#include "api/adaptive_load/step_controller_impl.pb.h"
#include "api/client/options.pb.h"
#include "api/client/output.pb.h"
#include "api/client/service.grpc.pb.h"
#include "api/client/service_mock.grpc.pb.h"

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/strings/str_join.h"
#include "adaptive_load/metrics_plugin_impl.h"
#include "adaptive_load/plugin_util.h"
#include "adaptive_load/step_controller_impl.h"
#include "grpcpp/test/mock_stream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Nighthawk {
namespace AdaptiveLoad {
namespace {

using ::testing::_;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::Return;
using ::testing::SetArgPointee;

class FakeTimeSource : public Envoy::TimeSource {
public:
  Envoy::SystemTime systemTime() override {
    Envoy::SystemTime epoch;
    return epoch + std::chrono::seconds(unix_time_);
  }
  Envoy::MonotonicTime monotonicTime() override {
    AdvanceUnixTime(1);
    Envoy::MonotonicTime epoch;
    return epoch + std::chrono::seconds(unix_time_);
  }
  void AdvanceUnixTime(int increment_seconds) { unix_time_ += increment_seconds; }
  void SetUnixTime(int64_t unix_time) { unix_time_ = unix_time; }

private:
  int64_t unix_time_{0};
};

// MetricsPlugin for testing, supporting a single metric named 'metric1'.
class FakeMetricsPlugin : public MetricsPlugin {
public:
  FakeMetricsPlugin() {}
  double GetMetricByName(const std::string&) override { return metric_value_; }
  const std::vector<std::string> GetAllSupportedMetricNames() const override { return {"metric1"}; }

  // Setters for fake responses.
  void SetMetricValue(double metric_value) { metric_value_ = metric_value; }

private:
  double metric_value_{0.0};
};

// A factory that creates a FakeMetricsPlugin with no config proto, registered under the name
// 'fake-metrics-plugin'.
class FakeMetricsPluginConfigFactory : public MetricsPluginConfigFactory {
public:
  std::string name() const override { return "fake-metrics-plugin"; }
  Envoy::ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<Envoy::ProtobufWkt::Any>();
  }
  MetricsPluginPtr createMetricsPlugin(const Envoy::Protobuf::Message&) override {
    return std::make_unique<FakeMetricsPlugin>();
  }
};

REGISTER_FACTORY(FakeMetricsPluginConfigFactory, MetricsPluginConfigFactory);

static int global_convergence_countdown;
static int global_doom_countdown;

// StepController for testing.
class FakeStepController : public StepController {
public:
  FakeStepController() {}
  bool IsConverged() const override { return --global_convergence_countdown <= 0; }
  bool IsDoomed(std::string* doomed_reason) const override {
    bool doomed = --global_doom_countdown <= 0;
    if (doomed) {
      *doomed_reason = doomed_reason_;
    }
    return doomed;
  }
  nighthawk::client::CommandLineOptions GetCurrentCommandLineOptions() const override {
    return command_line_options_;
  }
  void UpdateAndRecompute(const nighthawk::adaptive_load::BenchmarkResult&) override {}

  // // Setters for fake responses.
  // void SetIsConverged(bool is_converged) { is_converged_ = is_converged; }
  // void SetIsDoomed(bool is_doomed) { is_doomed_ = is_doomed; }
  // void SetCurrentCommandLineOptions(nighthawk::client::CommandLineOptions command_line_options) {
  //   command_line_options_ = command_line_options;
  // }

private:
  // bool is_converged_{false};
  // bool is_doomed_{false};
  std::string doomed_reason_{};
  nighthawk::client::CommandLineOptions command_line_options_{};
};

// A factory that creates a FakeStepController with no config proto.
class FakeStepControllerConfigFactory : public StepControllerConfigFactory {
public:
  std::string name() const override { return "fake-step-controller"; }
  Envoy::ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<Envoy::ProtobufWkt::Any>();
  }
  StepControllerPtr createStepController(const Envoy::Protobuf::Message&,
                                         const nighthawk::client::CommandLineOptions&) override {
    return std::make_unique<FakeStepController>();
  }
};

REGISTER_FACTORY(FakeStepControllerConfigFactory, StepControllerConfigFactory);

// Creates a valid MetricsPluginConfig proto that activates the fake MetricsPlugin defined in this
// file.
nighthawk::adaptive_load::MetricsPluginConfig MakeFakeMetricsPluginConfig() {
  nighthawk::adaptive_load::MetricsPluginConfig config;
  config.set_name("fake-metrics-plugin");
  *config.mutable_typed_config() = Envoy::ProtobufWkt::Any();
  return config;
}

// Creates a valid StepControllerConfig proto that activates the fake StepController defined in this
// file.
nighthawk::adaptive_load::StepControllerConfig MakeFakeStepControllerConfig() {
  nighthawk::adaptive_load::StepControllerConfig config;
  config.set_name("fake-step-controller");
  *config.mutable_typed_config() = Envoy::ProtobufWkt::Any();
  return config;
}

// Creates a valid ScoringFunctionConfig proto selecting the real BinaryScoringFunction plugin
// and configuring it with a threshold.
nighthawk::adaptive_load::ScoringFunctionConfig
MakeBinaryScoringFunctionConfig(double upper_threshold) {
  nighthawk::adaptive_load::ScoringFunctionConfig config;
  config.set_name("binary");

  nighthawk::adaptive_load::BinaryScoringFunctionConfig inner_config;
  inner_config.mutable_upper_threshold()->set_value(upper_threshold);
  Envoy::ProtobufWkt::Any inner_config_any;
  inner_config_any.PackFrom(inner_config);
  *config.mutable_typed_config() = inner_config_any;

  return config;
}

TEST(AdaptiveLoadControllerTest, FailsWithTrafficTemplateDurationSet) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  spec.mutable_nighthawk_traffic_template()->mutable_duration()->set_seconds(1);

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);
  EXPECT_THAT(output.session_status().message(), HasSubstr("should not have |duration| set"));
}

TEST(AdaptiveLoadControllerTest, FailsWithOpenLoopSet) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;
  spec.mutable_nighthawk_traffic_template()->mutable_open_loop()->set_value(false);

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("should not have |open_loop| set"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentMetricsPluginName) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricsPluginConfig* metrics_plugin_config =
      spec.mutable_metrics_plugin_configs()->Add();
  metrics_plugin_config->set_name("nonexistent-plugin");
  *metrics_plugin_config->mutable_typed_config() = Envoy::ProtobufWkt::Any();

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("MetricsPlugin not found"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentStepControllerPluginName) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::StepControllerConfig config;
  config.set_name("nonexistent-plugin");
  *config.mutable_typed_config() = Envoy::ProtobufWkt::Any();
  *spec.mutable_step_controller_config() = config;

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("StepController plugin not found"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentScoringFunctionPluginName) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricSpecWithThreshold* threshold =
      spec.mutable_metric_thresholds()->Add();
  nighthawk::adaptive_load::ScoringFunctionConfig scoring_function_config;
  scoring_function_config.set_name("nonexistent-scoring-function");
  *scoring_function_config.mutable_typed_config() = Envoy::ProtobufWkt::Any();
  *threshold->mutable_threshold_spec()->mutable_scoring_function() = scoring_function_config;

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("ScoringFunction plugin not found"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentMetricsPluginNameInMetricThresholdSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricSpecWithThreshold* threshold =
      spec.mutable_metric_thresholds()->Add();
  *threshold->mutable_threshold_spec()->mutable_scoring_function() =
      MakeBinaryScoringFunctionConfig(0.0);
  threshold->mutable_metric_spec()->set_metric_name("x");
  threshold->mutable_metric_spec()->set_metrics_plugin_name("nonexistent-metrics-plugin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("nonexistent metrics_plugin_name"));
}

TEST(AdaptiveLoadControllerTest, FailsWithUndeclaredMetricsPluginNameInMetricThresholdSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricSpecWithThreshold* threshold =
      spec.mutable_metric_thresholds()->Add();
  *threshold->mutable_threshold_spec()->mutable_scoring_function() =
      MakeBinaryScoringFunctionConfig(0.0);
  threshold->mutable_metric_spec()->set_metric_name("x");
  // Valid plugin name, but plugin not declared in the spec.
  threshold->mutable_metric_spec()->set_metrics_plugin_name("fake-metrics-plugin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("nonexistent metrics_plugin_name"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentMetricsPluginNameInInformationalMetricSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  // spec.mutable_nighthawk_traffic_template();
  // *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

  nighthawk::adaptive_load::MetricSpec* metric_spec =
      spec.mutable_informational_metric_specs()->Add();
  metric_spec->set_metric_name("x");
  metric_spec->set_metrics_plugin_name("nonexistent-metrics-plugin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("nonexistent metrics_plugin_name"));
}

TEST(AdaptiveLoadControllerTest, FailsWithUndeclaredMetricsPluginNameInInformationalMetricSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricSpec* metric_spec =
      spec.mutable_informational_metric_specs()->Add();
  metric_spec->set_metric_name("x");
  // Valid plugin name, but plugin not declared in the spec.
  metric_spec->set_metrics_plugin_name("fake-metrics-plugin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("nonexistent metrics_plugin_name"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentBuiltinMetricNameInMetricThresholdSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricSpecWithThreshold* threshold =
      spec.mutable_metric_thresholds()->Add();
  *threshold->mutable_threshold_spec()->mutable_scoring_function() =
      MakeBinaryScoringFunctionConfig(0.0);
  threshold->mutable_metric_spec()->set_metric_name("nonexistent-metric-name");
  threshold->mutable_metric_spec()->set_metrics_plugin_name("builtin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("not implemented by plugin"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentCustomMetricNameInMetricThresholdSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  *spec.mutable_metrics_plugin_configs()->Add() = MakeFakeMetricsPluginConfig();

  nighthawk::adaptive_load::MetricSpecWithThreshold* threshold =
      spec.mutable_metric_thresholds()->Add();
  *threshold->mutable_threshold_spec()->mutable_scoring_function() =
      MakeBinaryScoringFunctionConfig(0.0);
  threshold->mutable_metric_spec()->set_metric_name("nonexistent-metric-name");
  threshold->mutable_metric_spec()->set_metrics_plugin_name("fake-metrics-plugin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("not implemented by plugin"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentBuiltinMetricNameInInformationalMetricSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  nighthawk::adaptive_load::MetricSpec* metric_spec =
      spec.mutable_informational_metric_specs()->Add();
  metric_spec->set_metric_name("nonexistent-metric-name");
  metric_spec->set_metrics_plugin_name("builtin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("not implemented by plugin"));
}

TEST(AdaptiveLoadControllerTest, FailsWithNonexistentCustomMetricNameInInformationalMetricSpec) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  *spec.mutable_metrics_plugin_configs()->Add() = MakeFakeMetricsPluginConfig();

  nighthawk::adaptive_load::MetricSpec* metric_spec =
      spec.mutable_informational_metric_specs()->Add();
  metric_spec->set_metric_name("nonexistent-metric-name");
  metric_spec->set_metrics_plugin_name("fake-metrics-plugin");

  std::ostringstream diagnostic_ostream;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      /*nighthawk_service_stub=*/nullptr, spec, diagnostic_ostream,
      /*time_source=*/nullptr);

  EXPECT_THAT(output.session_status().message(), HasSubstr("not implemented by plugin"));
}

TEST(AdaptiveLoadControllerTest, TimesOutIfNeverConverged) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  spec.mutable_nighthawk_traffic_template();
  *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

  spec.mutable_convergence_deadline()->set_seconds(5);
  global_convergence_countdown = 100;
  global_doom_countdown = 1000;

  auto mock_reader_writer = std::make_shared<grpc::testing::MockClientReaderWriter<
      nighthawk::client::ExecutionRequest, nighthawk::client::ExecutionResponse>>();
  EXPECT_CALL(*mock_reader_writer.get(), Write(_, _)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), WritesDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), Read(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer, Finish()).WillRepeatedly(Return(::grpc::Status::OK));

  nighthawk::client::MockNighthawkServiceStub mock_nighthawk_service_stub;
  EXPECT_CALL(mock_nighthawk_service_stub, ExecutionStreamRaw(_))
      .WillRepeatedly(Return(mock_reader_writer.get()));

  std::ostringstream diagnostic_ostream;
  FakeTimeSource time_source;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      &mock_nighthawk_service_stub, spec, diagnostic_ostream, &time_source);

  EXPECT_THAT(output.session_status().message(), HasSubstr("Failed to converge before deadline"));
}

TEST(AdaptiveLoadControllerTest, ExitsWhenDoomed) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  spec.mutable_nighthawk_traffic_template();
  *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

  spec.mutable_convergence_deadline()->set_seconds(5);
  global_convergence_countdown = 1000;
  global_doom_countdown = 3;

  auto mock_reader_writer = std::make_shared<grpc::testing::MockClientReaderWriter<
      nighthawk::client::ExecutionRequest, nighthawk::client::ExecutionResponse>>();
  EXPECT_CALL(*mock_reader_writer.get(), Write(_, _)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), WritesDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), Read(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer, Finish()).WillRepeatedly(Return(::grpc::Status::OK));

  nighthawk::client::MockNighthawkServiceStub mock_nighthawk_service_stub;
  EXPECT_CALL(mock_nighthawk_service_stub, ExecutionStreamRaw(_))
      .WillRepeatedly(Return(mock_reader_writer.get()));

  std::ostringstream diagnostic_ostream;
  FakeTimeSource time_source;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      &mock_nighthawk_service_stub, spec, diagnostic_ostream, &time_source);

  EXPECT_THAT(output.session_status().message(),
              HasSubstr("Step controller determined that it can never converge"));
}

TEST(AdaptiveLoadControllerTest, PerformsTestingStageAfterConvergence) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  spec.mutable_nighthawk_traffic_template();
  *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

  spec.mutable_convergence_deadline()->set_seconds(5);
  global_convergence_countdown = 3;
  global_doom_countdown = 1000;

  auto mock_reader_writer = std::make_shared<grpc::testing::MockClientReaderWriter<
      nighthawk::client::ExecutionRequest, nighthawk::client::ExecutionResponse>>();
  EXPECT_CALL(*mock_reader_writer.get(), Write(_, _)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), WritesDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), Read(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer, Finish()).WillRepeatedly(Return(::grpc::Status::OK));

  nighthawk::client::MockNighthawkServiceStub mock_nighthawk_service_stub;
  EXPECT_CALL(mock_nighthawk_service_stub, ExecutionStreamRaw(_))
      .WillRepeatedly(Return(mock_reader_writer.get()));

  std::ostringstream diagnostic_ostream;
  FakeTimeSource time_source;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      &mock_nighthawk_service_stub, spec, diagnostic_ostream, &time_source);

  EXPECT_TRUE(output.has_testing_stage_result());
}

TEST(AdaptiveLoadControllerTest, SetsBenchmarkErrorStatusIfNighthawkServiceDoesNotSendResponse) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  spec.mutable_nighthawk_traffic_template();
  *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

  spec.mutable_convergence_deadline()->set_seconds(5);
  global_convergence_countdown = 2;
  global_doom_countdown = 1000;

  auto mock_reader_writer = std::make_shared<grpc::testing::MockClientReaderWriter<
      nighthawk::client::ExecutionRequest, nighthawk::client::ExecutionResponse>>();
  EXPECT_CALL(*mock_reader_writer.get(), Write(_, _)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), WritesDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), Read(_)).WillRepeatedly(Return(false));
  EXPECT_CALL(*mock_reader_writer, Finish()).WillRepeatedly(Return(::grpc::Status::OK));

  nighthawk::client::MockNighthawkServiceStub mock_nighthawk_service_stub;
  EXPECT_CALL(mock_nighthawk_service_stub, ExecutionStreamRaw(_))
      .WillRepeatedly(Return(mock_reader_writer.get()));

  std::ostringstream diagnostic_ostream;
  FakeTimeSource time_source;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      &mock_nighthawk_service_stub, spec, diagnostic_ostream, &time_source);

  ASSERT_GT(output.adjusting_stage_results_size(), 0);
  EXPECT_EQ(output.adjusting_stage_results()[0].status().code(), ::grpc::UNKNOWN);
  EXPECT_EQ(output.adjusting_stage_results()[0].status().message(),
            "Nighthawk Service did not send a response.");
}

TEST(AdaptiveLoadControllerTest,
     SetsBenchmarkErrorStatusIfNighthawkServiceGrpcStreamClosesAbnormally) {
  nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

  spec.mutable_nighthawk_traffic_template();
  *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

  spec.mutable_convergence_deadline()->set_seconds(5);
  global_convergence_countdown = 2;
  global_doom_countdown = 1000;

  auto mock_reader_writer = std::make_shared<grpc::testing::MockClientReaderWriter<
      nighthawk::client::ExecutionRequest, nighthawk::client::ExecutionResponse>>();
  EXPECT_CALL(*mock_reader_writer.get(), Write(_, _)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), WritesDone()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer.get(), Read(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_reader_writer, Finish())
      .WillRepeatedly(Return(::grpc::Status(::grpc::UNKNOWN, "status message")));

  nighthawk::client::MockNighthawkServiceStub mock_nighthawk_service_stub;
  EXPECT_CALL(mock_nighthawk_service_stub, ExecutionStreamRaw(_))
      .WillRepeatedly(Return(mock_reader_writer.get()));

  std::ostringstream diagnostic_ostream;
  FakeTimeSource time_source;
  nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
      &mock_nighthawk_service_stub, spec, diagnostic_ostream, &time_source);

  ASSERT_GT(output.adjusting_stage_results_size(), 0);
  EXPECT_EQ(output.adjusting_stage_results()[0].status().code(), ::grpc::UNKNOWN);
  EXPECT_EQ(output.adjusting_stage_results()[0].status().message(), "status message");
}
// TEST(AdaptiveLoadControllerTest, EvaluatesBuiltinMetric) {
//   nighthawk::adaptive_load::AdaptiveLoadSessionSpec spec;

//   spec.mutable_nighthawk_traffic_template();
//   *spec.mutable_step_controller_config() = MakeFakeStepControllerConfig();

//   // *spec.mutable_metric_thresholds()->Add() = MakeBinaryScoringFunctionConfig(15.0);

//   spec.mutable_convergence_deadline()->set_seconds(5);
//   global_convergence_countdown = 3;
//   global_doom_countdown = 1000;

//   nighthawk::client::MockNighthawkServiceStub mock_nighthawk_service_stub;

//   auto mock_reader_writer = std::make_shared<grpc::testing::MockClientReaderWriter<
//       nighthawk::client::ExecutionRequest, nighthawk::client::ExecutionResponse>>();

//   EXPECT_CALL(mock_nighthawk_service_stub, ExecutionStreamRaw(_))
//       .WillRepeatedly(Return(mock_reader_writer.get()));

//   nighthawk::client::ExecutionResponse execution_response;

//   EXPECT_CALL(*mock_reader_writer.get(), Write(_, _)).WillRepeatedly(Return(true));
//   EXPECT_CALL(*mock_reader_writer.get(), WritesDone()).WillRepeatedly(Return(true));
//   EXPECT_CALL(*mock_reader_writer.get(),
//   Read(_)).WillRepeatedly(DoAll(SetArgPointee<0>(execution_response), Return(true){});
//   EXPECT_CALL(*mock_reader_writer, Finish()).WillRepeatedly(Return(::grpc::Status::OK));

//   std::ostringstream diagnostic_ostream;
//   FakeTimeSource time_source;
//   nighthawk::adaptive_load::AdaptiveLoadSessionOutput output = PerformAdaptiveLoadSession(
//       &mock_nighthawk_service_stub, spec, diagnostic_ostream, &time_source);

//   EXPECT_TRUE(output.has_testing_stage_result());
// }

} // namespace
} // namespace AdaptiveLoad
} // namespace Nighthawk