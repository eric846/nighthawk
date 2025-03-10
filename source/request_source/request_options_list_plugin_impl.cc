#include "source/request_source/request_options_list_plugin_impl.h"

#include <memory>

#include "external/envoy/source/common/protobuf/message_validator_impl.h"
#include "external/envoy/source/common/protobuf/protobuf.h"
#include "external/envoy/source/common/protobuf/utility.h"
#include "external/envoy/source/exe/platform_impl.h"

#include "api/client/options.pb.h"

#include "source/common/request_impl.h"
#include "source/common/request_source_impl.h"

namespace Nighthawk {
std::string FileBasedOptionsListRequestSourceFactory::name() const {
  return "nighthawk.file-based-request-source-plugin";
}

Envoy::ProtobufTypes::MessagePtr
FileBasedOptionsListRequestSourceFactory::createEmptyConfigProto() {
  return std::make_unique<nighthawk::request_source::FileBasedOptionsListRequestSourceConfig>();
}

RequestSourcePtr FileBasedOptionsListRequestSourceFactory::createRequestSourcePlugin(
    const Envoy::Protobuf::Message& message, Envoy::Api::Api& api,
    Envoy::Http::RequestHeaderMapPtr header) {
  const auto* any =
      Envoy::Protobuf::DynamicCastToGenerated<const Envoy::ProtobufWkt::Any>(&message);
  nighthawk::request_source::FileBasedOptionsListRequestSourceConfig config;
  Envoy::MessageUtil util;
  THROW_IF_NOT_OK(util.unpackTo(*any, config));
  uint32_t max_file_size = config.has_max_file_size() ? config.max_file_size().value() : 1000000;
  if (api.fileSystem().fileSize(config.file_path()) > max_file_size) {
    throw NighthawkException("file size must be less than max_file_size");
  }
  nighthawk::client::RequestOptionsList loaded_list;
  // Locking to avoid issues with multiple threads reading the same file.
  {
    Envoy::Thread::LockGuard lock_guard(file_lock_);
    THROW_IF_NOT_OK(util.loadFromFile(config.file_path(), loaded_list,
                                      Envoy::ProtobufMessage::getStrictValidationVisitor(), api));
  }
  auto loaded_list_ptr = std::make_unique<const nighthawk::client::RequestOptionsList>(loaded_list);
  return std::make_unique<OptionsListRequestSource>(config.num_requests(), std::move(header),
                                                    std::move(loaded_list_ptr));
}

REGISTER_FACTORY(FileBasedOptionsListRequestSourceFactory, RequestSourcePluginConfigFactory);

std::string InLineOptionsListRequestSourceFactory::name() const {
  return "nighthawk.in-line-options-list-request-source-plugin";
}

Envoy::ProtobufTypes::MessagePtr InLineOptionsListRequestSourceFactory::createEmptyConfigProto() {
  return std::make_unique<nighthawk::request_source::InLineOptionsListRequestSourceConfig>();
}

RequestSourcePtr InLineOptionsListRequestSourceFactory::createRequestSourcePlugin(
    const Envoy::Protobuf::Message& message, Envoy::Api::Api&,
    Envoy::Http::RequestHeaderMapPtr header) {
  const auto* any =
      Envoy::Protobuf::DynamicCastToGenerated<const Envoy::ProtobufWkt::Any>(&message);
  nighthawk::request_source::InLineOptionsListRequestSourceConfig config;
  THROW_IF_NOT_OK(Envoy::MessageUtil::unpackTo(*any, config));
  auto loaded_list_ptr =
      std::make_unique<const nighthawk::client::RequestOptionsList>(config.options_list());
  return std::make_unique<OptionsListRequestSource>(config.num_requests(), std::move(header),
                                                    std::move(loaded_list_ptr));
}

REGISTER_FACTORY(InLineOptionsListRequestSourceFactory, RequestSourcePluginConfigFactory);

OptionsListRequestSource::OptionsListRequestSource(
    const uint32_t total_requests, Envoy::Http::RequestHeaderMapPtr header,
    std::unique_ptr<const nighthawk::client::RequestOptionsList> options_list)
    : header_(std::move(header)), options_list_(std::move(options_list)),
      total_requests_(total_requests) {}

RequestGenerator OptionsListRequestSource::get() {
  request_count_.push_back(0);
  uint32_t& lambda_counter = request_count_.back();
  RequestGenerator request_generator = [this, lambda_counter]() mutable -> RequestPtr {
    // Initialize the header with the values from the default header.
    Envoy::Http::RequestHeaderMapPtr header = Envoy::Http::RequestHeaderMapImpl::create();
    Envoy::Http::HeaderMapImpl::copyFrom(*header, *header_);

    // if request_max is 0, then we never stop generating requests.
    if (lambda_counter >= total_requests_ && total_requests_ != 0) {
      return nullptr;
    }
    // if the options_list_ is empty, we just return the default header.
    if (options_list_->options_size() == 0) {
      return std::make_unique<RequestImpl>(std::move(header));
    }

    // Increment the counter and get the request_option from the list for the current iteration.
    const uint32_t index = lambda_counter % options_list_->options_size();
    nighthawk::client::RequestOptions request_option = options_list_->options().at(index);
    ++lambda_counter;

    // Override the default values with the values from the request_option
    header->setMethod(envoy::config::core::v3::RequestMethod_Name(request_option.request_method()));
    uint32_t request_body_length = 0;
    if (!request_option.json_body().empty()) {
      request_body_length = request_option.json_body().size();
    } else {
      request_body_length = request_option.request_body_size().value();
    }
    const uint32_t content_length = request_body_length;

    if (content_length > 0) {
      header->setContentLength(
          content_length); // Content length is used later in stream_decoder to populate the body
    }
    // If json_body is provided, we should set the ContentType as application/json.
    if (!request_option.json_body().empty()) {
      header->setContentType("application/json");
    }
    for (const envoy::config::core::v3::HeaderValueOption& option_header :
         request_option.request_headers()) {
      auto lower_case_key = Envoy::Http::LowerCaseString(std::string(option_header.header().key()));
      header->setCopy(lower_case_key, std::string(option_header.header().value()));
    }
    return std::make_unique<RequestImpl>(std::move(header), request_option.json_body());
  };
  return request_generator;
}

void OptionsListRequestSource::initOnThread() {}
void OptionsListRequestSource::destroyOnThread() {}

} // namespace Nighthawk
