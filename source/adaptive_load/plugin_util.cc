#include "nighthawk/adaptive_load/input_variable_setter.h"
#include "nighthawk/adaptive_load/metrics_plugin.h"
#include "nighthawk/adaptive_load/scoring_function.h"
#include "nighthawk/adaptive_load/step_controller.h"

#include "external/envoy/source/common/config/utility.h"

namespace Nighthawk {
namespace AdaptiveLoad {

InputVariableSetterPtr
LoadInputVariableSetterPlugin(const nighthawk::adaptive_load::InputVariableSetterConfig& config) {
  InputVariableSetterConfigFactory& config_factory =
      Envoy::Config::Utility::getAndCheckFactoryByName<InputVariableSetterConfigFactory>(
          config.name());
  return config_factory.createInputVariableSetter(config.typed_config());
}

ScoringFunctionPtr
LoadScoringFunctionPlugin(const nighthawk::adaptive_load::ScoringFunctionConfig& config) {
  ScoringFunctionConfigFactory& config_factory =
      Envoy::Config::Utility::getAndCheckFactoryByName<ScoringFunctionConfigFactory>(config.name());
  return config_factory.createScoringFunction(config.typed_config());
}

MetricsPluginPtr LoadMetricsPlugin(const nighthawk::adaptive_load::MetricsPluginConfig& config) {
  MetricsPluginConfigFactory& config_factory =
      Envoy::Config::Utility::getAndCheckFactoryByName<MetricsPluginConfigFactory>(config.name());
  return config_factory.createMetricsPlugin(config.typed_config());
}

StepControllerPtr LoadStepControllerPlugin(
    const nighthawk::adaptive_load::StepControllerConfig& config,
    const nighthawk::client::CommandLineOptions& command_line_options_template) {
  StepControllerConfigFactory& config_factory =
      Envoy::Config::Utility::getAndCheckFactoryByName<StepControllerConfigFactory>(config.name());
  return config_factory.createStepController(config.typed_config(), command_line_options_template);
}

} // namespace AdaptiveLoad
} // namespace Nighthawk