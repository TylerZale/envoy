#include "extensions/filters/network/thrift_proxy/conn_manager.h"

#include "envoy/common/exception.h"
#include "envoy/event/dispatcher.h"

#include "extensions/filters/network/thrift_proxy/app_exception_impl.h"
#include "extensions/filters/network/thrift_proxy/binary_protocol_impl.h"
#include "extensions/filters/network/thrift_proxy/compact_protocol_impl.h"
#include "extensions/filters/network/thrift_proxy/framed_transport_impl.h"
#include "extensions/filters/network/thrift_proxy/protocol.h"
#include "extensions/filters/network/thrift_proxy/unframed_transport_impl.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace ThriftProxy {

ConnectionManager::ConnectionManager(Config& config, Runtime::RandomGenerator& random_generator,
                                     Event::TimeSystem& time_system)
    : config_(config), stats_(config_.stats()), transport_(config.createTransport()),
      protocol_(config.createProtocol()),
      decoder_(std::make_unique<Decoder>(*transport_, *protocol_, *this)),
      random_generator_(random_generator), time_system_(time_system) {}

ConnectionManager::~ConnectionManager() {}

Network::FilterStatus ConnectionManager::onData(Buffer::Instance& data, bool end_stream) {
  request_buffer_.move(data);
  dispatch();

  if (end_stream) {
    ENVOY_CONN_LOG(trace, "downstream half-closed", read_callbacks_->connection());

    // Downstream has closed. Unless we're waiting for an upstream connection to complete a oneway
    // request, close. The special case for oneway requests allows them to complete before the
    // ConnectionManager is destroyed.
    if (stopped_) {
      ASSERT(!rpcs_.empty());
      MessageMetadata& metadata = *(*rpcs_.begin())->metadata_;
      ASSERT(metadata.hasMessageType());
      if (metadata.messageType() == MessageType::Oneway) {
        ENVOY_CONN_LOG(trace, "waiting for one-way completion", read_callbacks_->connection());
        half_closed_ = true;
        return Network::FilterStatus::StopIteration;
      }
    }

    resetAllRpcs(false);
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
  }

  return Network::FilterStatus::StopIteration;
}

void ConnectionManager::dispatch() {
  if (stopped_) {
    ENVOY_CONN_LOG(debug, "thrift filter stopped", read_callbacks_->connection());
    return;
  }

  try {
    bool underflow = false;
    while (!underflow) {
      FilterStatus status = decoder_->onData(request_buffer_, underflow);
      if (status == FilterStatus::StopIteration) {
        stopped_ = true;
        break;
      }
    }

    return;
  } catch (const AppException& ex) {
    ENVOY_LOG(error, "thrift application exception: {}", ex.what());
    if (rpcs_.empty()) {
      MessageMetadata metadata;
      sendLocalReply(metadata, ex);
    } else {
      sendLocalReply(*(*rpcs_.begin())->metadata_, ex);
    }
  } catch (const EnvoyException& ex) {
    ENVOY_CONN_LOG(error, "thrift error: {}", read_callbacks_->connection(), ex.what());

    // Use the current rpc to send an error downstream, if possible.
    rpcs_.front()->onError(ex.what());
  }

  stats_.request_decoding_error_.inc();
  resetAllRpcs(true);
  read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
}

void ConnectionManager::sendLocalReply(MessageMetadata& metadata, const DirectResponse& response) {
  Buffer::OwnedImpl buffer;

  response.encode(metadata, *protocol_, buffer);

  Buffer::OwnedImpl response_buffer;

  metadata.setProtocol(protocol_->type());
  transport_->encodeFrame(response_buffer, metadata, buffer);

  read_callbacks_->connection().write(response_buffer, false);
}

void ConnectionManager::continueDecoding() {
  ENVOY_CONN_LOG(debug, "thrift filter continued", read_callbacks_->connection());
  stopped_ = false;
  dispatch();

  if (!stopped_ && half_closed_) {
    // If we're half closed, but not stopped waiting for an upstream, reset any pending rpcs and
    // close the connection.
    resetAllRpcs(false);
    read_callbacks_->connection().close(Network::ConnectionCloseType::FlushWrite);
  }
}

void ConnectionManager::doDeferredRpcDestroy(ConnectionManager::ActiveRpc& rpc) {
  read_callbacks_->connection().dispatcher().deferredDelete(rpc.removeFromList(rpcs_));
}

void ConnectionManager::resetAllRpcs(bool local_reset) {
  while (!rpcs_.empty()) {
    if (local_reset) {
      ENVOY_CONN_LOG(debug, "local close with active request", read_callbacks_->connection());
      stats_.cx_destroy_local_with_active_rq_.inc();
    } else {
      ENVOY_CONN_LOG(debug, "remote close with active request", read_callbacks_->connection());
      stats_.cx_destroy_remote_with_active_rq_.inc();
    }

    rpcs_.front()->onReset();
  }
}

void ConnectionManager::initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) {
  read_callbacks_ = &callbacks;

  read_callbacks_->connection().addConnectionCallbacks(*this);
  read_callbacks_->connection().enableHalfClose(true);
}

void ConnectionManager::onEvent(Network::ConnectionEvent event) {
  resetAllRpcs(event == Network::ConnectionEvent::LocalClose);
}

DecoderEventHandler& ConnectionManager::newDecoderEventHandler() {
  ENVOY_LOG(trace, "new decoder filter");

  ActiveRpcPtr new_rpc(new ActiveRpc(*this));
  new_rpc->createFilterChain();
  new_rpc->moveIntoList(std::move(new_rpc), rpcs_);

  return **rpcs_.begin();
}

bool ConnectionManager::ResponseDecoder::onData(Buffer::Instance& data) {
  upstream_buffer_.move(data);

  bool underflow = false;
  decoder_->onData(upstream_buffer_, underflow);
  ASSERT(complete_ || underflow);
  return complete_;
}

FilterStatus ConnectionManager::ResponseDecoder::messageBegin(MessageMetadataSharedPtr metadata) {
  metadata_ = metadata;
  metadata_->setSequenceId(parent_.original_sequence_id_);

  first_reply_field_ =
      (metadata->hasMessageType() && metadata->messageType() == MessageType::Reply);
  return ProtocolConverter::messageBegin(metadata);
}

FilterStatus ConnectionManager::ResponseDecoder::fieldBegin(absl::string_view name,
                                                            FieldType field_type,
                                                            int16_t field_id) {
  if (first_reply_field_) {
    // Reply messages contain a struct where field 0 is the call result and fields 1+ are
    // exceptions, if defined. At most one field may be set. Therefore, the very first field we
    // encounter in a reply is either field 0 (success) or not (IDL exception returned).
    success_ = field_id == 0 && field_type != FieldType::Stop;
    first_reply_field_ = false;
  }

  return ProtocolConverter::fieldBegin(name, field_type, field_id);
}

FilterStatus ConnectionManager::ResponseDecoder::transportEnd() {
  ASSERT(metadata_ != nullptr);

  ConnectionManager& cm = parent_.parent_;

  Buffer::OwnedImpl buffer;

  // Use the factory to get the concrete transport from the decoder transport (as opposed to
  // potentially pre-detection auto transport).
  TransportPtr transport =
      NamedTransportConfigFactory::getFactory(parent_.parent_.decoder_->transportType())
          .createTransport();

  metadata_->setProtocol(parent_.parent_.decoder_->protocolType());
  transport->encodeFrame(buffer, *metadata_, parent_.response_buffer_);
  complete_ = true;

  cm.read_callbacks_->connection().write(buffer, false);

  cm.stats_.response_.inc();

  switch (metadata_->messageType()) {
  case MessageType::Reply:
    cm.stats_.response_reply_.inc();
    if (success_.value_or(false)) {
      cm.stats_.response_success_.inc();
    } else {
      cm.stats_.response_error_.inc();
    }

    break;

  case MessageType::Exception:
    cm.stats_.response_exception_.inc();
    break;

  default:
    cm.stats_.response_invalid_type_.inc();
    break;
  }

  return FilterStatus::Continue;
}

FilterStatus ConnectionManager::ActiveRpc::transportEnd() {
  ASSERT(metadata_ != nullptr);
  ASSERT(metadata_->hasMessageType());

  parent_.stats_.request_.inc();

  switch (metadata_->messageType()) {
  case MessageType::Call:
    parent_.stats_.request_call_.inc();
    break;

  case MessageType::Oneway:
    parent_.stats_.request_oneway_.inc();

    // No response forthcoming, we're done.
    parent_.doDeferredRpcDestroy(*this);
    break;

  default:
    parent_.stats_.request_invalid_type_.inc();
    break;
  }

  FilterStatus status = event_handler_->transportEnd();

  if (metadata_->isProtocolUpgradeMessage()) {
    ENVOY_CONN_LOG(error, "thrift: sending protocol upgrade response",
                   parent_.read_callbacks_->connection());
    sendLocalReply(*parent_.protocol_->upgradeResponse(*upgrade_handler_));
  }

  return status;
}

FilterStatus ConnectionManager::ActiveRpc::messageBegin(MessageMetadataSharedPtr metadata) {
  ASSERT(metadata->hasSequenceId());

  metadata_ = metadata;
  original_sequence_id_ = metadata_->sequenceId();

  if (metadata_->isProtocolUpgradeMessage()) {
    ASSERT(parent_.protocol_->supportsUpgrade());

    ENVOY_CONN_LOG(error, "thrift: decoding protocol upgrade request",
                   parent_.read_callbacks_->connection());
    upgrade_handler_ = parent_.protocol_->upgradeRequestDecoder();
    ASSERT(upgrade_handler_ != nullptr);
    event_handler_ = upgrade_handler_.get();
  }

  return event_handler_->messageBegin(metadata);
}

void ConnectionManager::ActiveRpc::createFilterChain() {
  parent_.config_.filterFactory().createFilterChain(*this);
}

void ConnectionManager::ActiveRpc::onReset() {
  // TODO(zuercher): e.g., parent_.stats_.named_.downstream_rq_rx_reset_.inc();
  parent_.doDeferredRpcDestroy(*this);
}

void ConnectionManager::ActiveRpc::onError(const std::string& what) {
  if (metadata_) {
    sendLocalReply(AppException(AppExceptionType::ProtocolError, what));
    return;
  }

  // Transport or protocol error happened before (or during message begin) parsing. It's not
  // possible to provide a valid response, so don't try.
}

const Network::Connection* ConnectionManager::ActiveRpc::connection() const {
  return &parent_.read_callbacks_->connection();
}

void ConnectionManager::ActiveRpc::continueDecoding() { parent_.continueDecoding(); }

Router::RouteConstSharedPtr ConnectionManager::ActiveRpc::route() {
  if (!cached_route_) {
    if (metadata_ != nullptr) {
      Router::RouteConstSharedPtr route =
          parent_.config_.routerConfig().route(*metadata_, stream_id_);
      cached_route_ = std::move(route);
    } else {
      cached_route_ = nullptr;
    }
  }

  return cached_route_.value();
}

void ConnectionManager::ActiveRpc::sendLocalReply(const DirectResponse& response) {
  metadata_->setSequenceId(original_sequence_id_);

  parent_.sendLocalReply(*metadata_, response);
  parent_.doDeferredRpcDestroy(*this);
}

void ConnectionManager::ActiveRpc::startUpstreamResponse(Transport& transport, Protocol& protocol) {
  ASSERT(response_decoder_ == nullptr);

  response_decoder_ = std::make_unique<ResponseDecoder>(*this, transport, protocol);
}

bool ConnectionManager::ActiveRpc::upstreamData(Buffer::Instance& buffer) {
  ASSERT(response_decoder_ != nullptr);

  try {
    bool complete = response_decoder_->onData(buffer);
    if (complete) {
      parent_.doDeferredRpcDestroy(*this);
    }
    return complete;
  } catch (const AppException& ex) {
    ENVOY_LOG(error, "thrift response application error: {}", ex.what());
    parent_.stats_.response_decoding_error_.inc();

    sendLocalReply(ex);
    decoder_filter_->resetUpstreamConnection();
    return true;
  } catch (const EnvoyException& ex) {
    ENVOY_CONN_LOG(error, "thrift response error: {}", parent_.read_callbacks_->connection(),
                   ex.what());
    parent_.stats_.response_decoding_error_.inc();

    onError(ex.what());
    decoder_filter_->resetUpstreamConnection();
    return true;
  }
}

void ConnectionManager::ActiveRpc::resetDownstreamConnection() {
  parent_.read_callbacks_->connection().close(Network::ConnectionCloseType::NoFlush);
}

} // namespace ThriftProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
