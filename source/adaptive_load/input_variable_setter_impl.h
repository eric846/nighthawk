#pragma once

#include "nighthawk/adaptive_load/input_variable_setter.h"

#include "api/adaptive_load/input_variable_setter_impl.pb.h"

namespace Nighthawk {
namespace AdaptiveLoad {

// An InputVariableSetter that sets the |requests_per_second| field in the CommandLineOptions proto.
class RequestsPerSecondInputVariableSetter : public InputVariableSetter {
public:
  RequestsPerSecondInputVariableSetter();
  void SetInputVariable(nighthawk::client::CommandLineOptions* command_line_options,
                        double input_value) override;
};

// A factory that creates an RequestsPerSecondInputVariableSetter from an
// RequestsPerSecondInputVariableSetterConfig proto.
class RequestsPerSecondInputVariableSetterConfigFactory : public InputVariableSetterConfigFactory {
public:
  std::string name() const override;
  Envoy::ProtobufTypes::MessagePtr createEmptyConfigProto() override;
  // Ignores |message| because RequestsPerSecondInputVariableSetter does not use a config proto.
  InputVariableSetterPtr createInputVariableSetter(const Envoy::Protobuf::Message&) override;
};

} // namespace AdaptiveLoad
} // namespace Nighthawk