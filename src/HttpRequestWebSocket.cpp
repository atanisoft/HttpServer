/*
 * SPDX-FileCopyrightText: 2019 Mike Dunston (atanisoft)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Httpd.h"

#if defined(CONFIG_IDF_TARGET)
#include <mbedtls/sha1.h>
#endif // CONFIG_IDF_TARGET

#ifndef CONFIG_HTTP_WS_LOG_LEVEL
#define CONFIG_HTTP_WS_LOG_LEVEL VERBOSE
#endif

namespace http
{

typedef enum
{
  OP_CONTINUATION = 0x0, // Continuation Frame
  OP_TEXT         = 0x1, // Text Frame
  OP_BINARY       = 0x2, // Binary Frame
  OP_CLOSE        = 0x8, // Connection Close Frame
  OP_PING         = 0x9, // Ping Frame
  OP_PONG         = 0xA, // Pong Frame
} WebSocketOpcode;

static constexpr uint8_t WEBSOCKET_FINAL_FRAME = 0x80;
static constexpr uint8_t WEBSOCKET_COMPRESSED_FRAME = 0x40;
static constexpr uint8_t WEBSOCKET_FRAME_IS_MASKED = 0x80;
static constexpr uint8_t WEBSOCKET_FRAME_LEN_SINGLE = 126;
static constexpr uint8_t WEBSOCKET_FRAME_LEN_UINT16 = 126;
static constexpr uint8_t WEBSOCKET_FRAME_LEN_UINT64 = 127;

// This is the WebSocket UUID it is used as part of the handshake process.
static constexpr const char * WEBSOCKET_UUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

WebSocketFlow::WebSocketFlow(Httpd *server, int fd, uint32_t remote_ip
                           , const string &ws_key, WebSocketHandler handler)
                           : StateFlowBase(server)
                           , server_(server)
                           , fd_(fd)
                           , remote_ip_(remote_ip)
                           , timeout_(MSEC_TO_NSEC(config_httpd_websocket_timeout_ms()))
                           , max_frame_size_(config_httpd_websocket_max_frame_size())
                           , handler_(handler)
{
  if(server_->add_websocket(fd, this))
  {
    string key_data = ws_key + WEBSOCKET_UUID;

    // replace spaces with + which might have been removed by url_decode
    std::replace(key_data.begin(), key_data.end(), ' ', '+');

    unsigned char key_sha1[20];
    LOG(CONFIG_HTTP_WS_LOG_LEVEL,
        "[WebSocket fd:%d] Connected, starting handshake", fd_);
#if defined(CONFIG_IDF_TARGET)
    // SHA1 encode the ws_key plus the websocket UUID, if this fails close the
    // socket immediately.
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
    if (!mbedtls_sha1_ret((unsigned char *)key_data.c_str(),
                          key_data.length(), key_sha1))
#else // IDF v5.0+
    if (!mbedtls_sha1((unsigned char *)key_data.c_str(),
                      key_data.length(), key_sha1))
#endif // IDF < v5.0
    {
      AbstractHttpResponse resp(STATUS_SWITCH_PROTOCOL);
      resp.header(HttpHeader::CONNECTION, HTTP_CONNECTION_UPGRADE);
      resp.header(HttpHeader::UPGRADE, HTTP_UPGRADE_HEADER_WEBSOCKET);
      resp.header(HttpHeader::WS_VERSION, HTTP_WEBSOCKET_VERSION);
      resp.header(HttpHeader::WS_ACCEPT
                , base64_encode(string((char *)key_sha1, 20)));

      // Disable usage of all WS extensions by default since they are not
      // implemented by this server.
      //resp.header(HttpHeader::WS_SEC_EXTENSIONS, "");

      handshake_.assign(std::move(resp.to_string()));

      // Allocate buffer for frame data.
      data_ = (uint8_t *)malloc(max_frame_size_);
      if (data_)
      {
        // prepare to send the handshake to the WS client
        start_flow(STATE(send_handshake));

        return;
      }
    }
    LOG_ERROR("[WebSocket fd:%d] Error establishing connection, aborting", fd_);
#else // NOT CONFIG_IDF_TARGET
    LOG_ERROR("[WebSocket fd:%d] No SHA1 library available, aborting", fd_);
#endif // CONFIG_IDF_TARGET
  }
  else
  {
    LOG_ERROR("[WebSocket fd:%d] Failed to allocate websocket, aborting!", fd_);
  }
  server_->schedule_cleanup(this);
}

WebSocketFlow::~WebSocketFlow()
{
  // remove ourselves from the server so we don't get called again
  server_->remove_websocket(fd_);

  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] Disconnected", fd_);
  ::close(fd_);

  if (data_)
  {
    free(data_);
  }
  textFrames_.clear();
}

void WebSocketFlow::send_text(string &text)
{
  OSMutexLock h(&textLock_);
  if (textFrames_.size() < config_httpd_websocket_max_pending_frames())
  {
    textFrames_.push_back(text);
  }
  else
  {
    LOG_ERROR("[WebSocket fd:%d] Discarding text frame", fd_);
  }
}

int WebSocketFlow::id()
{
  return fd_;
}

uint32_t WebSocketFlow::ip()
{
  return remote_ip_;
}

void WebSocketFlow::request_close()
{
  close_requested_ = true;
}

StateFlowBase::Action WebSocketFlow::read_fully_with_timeout(void *buf
                                                           , size_t size
                                                           , size_t attempts
                                                           , StateFlowBase::Callback success
                                                           , StateFlowBase::Callback timeout)
{
  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] requesting %zu bytes", fd_,
      size);
  buf_ = (uint8_t *)buf;
  buf_offs_ = 0;
  buf_remain_ = size;
  buf_next_ = success;
  buf_next_timeout_ = timeout;
  buf_attempts_ = attempts;
  return read_repeated_with_timeout(&helper_, timeout_, fd_, buf_, size
                                  , STATE(data_received));
}

StateFlowBase::Action WebSocketFlow::data_received()
{
  HASSERT(buf_next_);
  size_t received = (buf_remain_ - helper_.remaining_);
  LOG(CONFIG_HTTP_WS_LOG_LEVEL,
      "[WebSocket fd:%d] hasError:%d, readFully:%d, readNonblocking:%d, "
      "readWithTimeout: %d, remaining:%d, received:%zu",
      fd_, helper_.hasError_, helper_.readFully_, helper_.readNonblocking_,
      helper_.readWithTimeout_, helper_.remaining_, received);
  if (helper_.hasError_)
  {
    LOG_ERROR("[WebSocket fd:%d] read-error (%d: %s), disconnecting (recv)"
            , fd_, errno, strerror(errno));
    return yield_and_call(STATE(shutdown_connection));
  }
  buf_remain_ -= received;
  buf_offs_ += received;
  LOG(CONFIG_HTTP_WS_LOG_LEVEL,
      "[WebSocket fd:%d] Received %zu bytes, %zu bytes remain", fd_,
      received, buf_remain_);
  if (buf_remain_ && buf_attempts_ > 0)
  {
    buf_attempts_--;
    return read_repeated_with_timeout(&helper_, timeout_, fd_,
                                      buf_ + buf_offs_, buf_remain_,
                                      STATE(data_received));
  }
  else if (buf_remain_)
  {
    return yield_and_call(buf_next_timeout_);
  }
  return yield_and_call(buf_next_);
}

StateFlowBase::Action WebSocketFlow::send_handshake()
{
  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] Sending handshake:\n%s",
      fd_, handshake_.c_str());
  return write_repeated(&helper_, fd_, handshake_.c_str(), handshake_.length(),
                        STATE(handshake_sent));
}

StateFlowBase::Action WebSocketFlow::handshake_sent()
{
  handshake_.clear();
  if (helper_.hasError_)
  {
    LOG_ERROR(
      "[WebSocket fd:%d] read-error (%d: %s), disconnecting (handshake)", fd_,
      errno, strerror(errno));
    return yield_and_call(STATE(shutdown_connection));
  }
  handler_(this, WebSocketEvent::WS_EVENT_CONNECT, nullptr, 0);
  connect_event_sent_ = true;
  return yield_and_call(STATE(read_frame_header));
}

StateFlowBase::Action WebSocketFlow::read_frame_header()
{
  // Check if there has been a request to shutdown the connection
  if (close_requested_)
  {
    return yield_and_call(STATE(shutdown_connection));
  }
  // reset frame state to defaults
  header_ = 0;
  opcode_ = 0;
  frameLenType_ = 0;
  frameLength_ = 0;
  maskingKey_ = 0;
  bzero(data_, max_frame_size_);
  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] Reading WS packet", fd_);
  return read_fully_with_timeout(&header_, sizeof(uint16_t),
                                 config_httpd_websocket_max_read_attempts(),
                                 STATE(frame_header_received),
                                 STATE(send_frame_header));
}

StateFlowBase::Action WebSocketFlow::frame_header_received()
{
  opcode_ = static_cast<WebSocketOpcode>(header_ & 0x0F);
  masked_ = ((header_ >> 8) & WEBSOCKET_FRAME_IS_MASKED);
  compressed_ = ((header_ >> 8) & WEBSOCKET_COMPRESSED_FRAME);
  uint8_t len = ((header_ >> 8) & 0x7F);
  LOG(CONFIG_HTTP_WS_LOG_LEVEL,
      "[WebSocket fd:%d] opc: %d, masked: %d, len: %d", fd_, opcode_, masked_,
      len);
  if (len < WEBSOCKET_FRAME_LEN_SINGLE)
  {
    frameLenType_ = 0;
    frameLength_ = len;
    return call_immediately(STATE(frame_data_len_received));
  }
  else if (len == WEBSOCKET_FRAME_LEN_UINT16)
  {
    frameLenType_ = 1;
    frameLength_ = 0;
    // retrieve the payload length as a 16 bit number
    return read_fully_with_timeout(&frameLength16_, sizeof(uint16_t),
                                   config_httpd_websocket_max_read_attempts(),
                                   STATE(frame_data_len_received),
                                   STATE(shutdown_connection));
  }
  else if (len == WEBSOCKET_FRAME_LEN_UINT64)
  {
    frameLenType_ = 2;
    // retrieve the payload length as a 64 bit number
    return read_fully_with_timeout(&frameLength_, sizeof(uint64_t),
                                   config_httpd_websocket_max_read_attempts(),
                                   STATE(frame_data_len_received),
                                   STATE(shutdown_connection));
  }

  LOG_ERROR("[WebSocket fd:%d] Invalid frame received, disconnecting", fd_);
  // if we get here the frame is malformed, shutdown the connection
  return yield_and_call(STATE(shutdown_connection));
}

StateFlowBase::Action WebSocketFlow::frame_data_len_received()
{
  if (frameLenType_ == 1)
  {
    LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] orig len: %d", fd_,
        (int)frameLength16_);
    // byte swap frameLength16_ into frameLength_
    frameLength_ = ((frameLength16_ & 0xFF) << 8) +
                   ((frameLength16_ >> 8) & 0xFF);
  }
  else if (frameLenType_ == 2)
  {
  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] orig len: %" PRIu64, fd_,
      frameLength_);
    // byte swap frameLength_ (64 bit)
    uint8_t *p = (uint8_t *)frameLength_;
    uint64_t temp =              p[7] +
                    (((uint16_t)(p[6]) << 8))  +
                    (((uint32_t)(p[5]) << 16)) +
                    (((uint32_t)(p[4]) << 24)) +
                    (((uint64_t)(p[3]) << 32)) +
                    (((uint64_t)(p[2]) << 40)) +
                    (((uint64_t)(p[1]) << 48)) +
                    (((uint64_t)(p[0]) << 56));
    frameLength_ = temp;
  }

  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] frame len: %" PRIu64, fd_,
      frameLength_);

  if (masked_)
  {
    // frame uses data masking, read the mask and then start reading the
    // frame payload
    return read_fully_with_timeout(&maskingKey_, sizeof(uint32_t)
                                 , config_httpd_websocket_max_read_attempts()
                                 , STATE(start_recv_frame_data)
                                 , STATE(shutdown_connection));
  }
  // no masking, start reading the frame data
  return yield_and_call(STATE(start_recv_frame_data));
}

StateFlowBase::Action WebSocketFlow::start_recv_frame_data()
{
  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] Reading WS packet (%d len)",
      fd_, (int)frameLength_);
  // restrict the size of the fame buffer so we don't use all of the ram for
  // one frame.
  data_size_ = std::min(frameLength_, max_frame_size_);
  return read_fully_with_timeout(data_, data_size_
                               , config_httpd_websocket_max_read_attempts()
                               , STATE(recv_frame_data)
                               , STATE(send_frame_header));
}

StateFlowBase::Action WebSocketFlow::recv_frame_data()
{
  size_t received_len = data_size_ - helper_.remaining_;
  if (received_len)
  {
    LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] Received %zu bytes", fd_,
        received_len);
    if (masked_)
    {
      uint8_t *mask = reinterpret_cast<uint8_t *>(&maskingKey_);
      char buf[10];
      LOG(CONFIG_HTTP_WS_LOG_LEVEL,
          "[WebSocket fd:%d] Demasking %zu bytes (mask: %s)", fd_, received_len,
          unsigned_integer_to_buffer_hex(maskingKey_, buf));
      for (size_t idx = 0; idx < received_len; idx++)
      {
        data_[idx] ^= mask[idx % 4];
      }
    }
    if (opcode_ == OP_PING)
    {
      // send PONG
    }
    else if (opcode_ == OP_TEXT)
    {
      handler_(this, WebSocketEvent::WS_EVENT_TEXT, data_, received_len);
    }
    else if (opcode_ == OP_BINARY)
    {
      handler_(this, WebSocketEvent::WS_EVENT_BINARY, data_, received_len);
    }
    if (opcode_ == OP_CLOSE || close_requested_)
    {
      return yield_and_call(STATE(shutdown_connection));
    }
  }
  frameLength_ -= received_len;
  if (frameLength_)
  {
    data_size_ = std::min(frameLength_, max_frame_size_);
    return read_fully_with_timeout(data_, data_size_,
                                   config_httpd_websocket_max_read_attempts(),
                                   STATE(recv_frame_data),
                                   STATE(shutdown_connection));
  }
  return yield_and_call(STATE(send_frame_header));
}

StateFlowBase::Action WebSocketFlow::shutdown_connection()
{
  if (connect_event_sent_)
  {
    handler_(this, WebSocketEvent::WS_EVENT_DISCONNECT, nullptr, 0);
  }
  server_->schedule_cleanup(this);
  return exit();
}

StateFlowBase::Action WebSocketFlow::send_frame_header()
{
  // TODO: add binary message sending support
  OSMutexLock h(&textLock_);
  if (textFrames_.empty())
  {
    return yield_and_call(STATE(read_frame_header));
  }
  bzero(data_, max_frame_size_);
  size_t send_size = 0;
  if (textFrames_.front().length() < WEBSOCKET_FRAME_LEN_SINGLE)
  {
    data_[0] = WEBSOCKET_FINAL_FRAME;
    data_[1] = textFrames_.front().length();
    memcpy(data_ + 2, textFrames_.front().data(),
           textFrames_.front().length());
    data_size_ = textFrames_.front().length();
    send_size = data_size_ + 2;
  }
  else if (textFrames_.front().length() < max_frame_size_ - 4)
  {
    data_size_ = textFrames_.front().length();
    data_[0] = WEBSOCKET_FINAL_FRAME;
    data_[1] = WEBSOCKET_FRAME_LEN_UINT16;
    data_[2] = (data_size_ >> 8) & 0xFF;
    data_[3] = data_size_ & 0xFF;
    memcpy(data_+ 4, textFrames_.front().data(), textFrames_.front().length());
    send_size = data_size_ + 4;
  }
  else
  {
    // size is greater than our max frame, send it as fragments
    data_size_ = max_frame_size_ - 4;
    data_[0] = OP_CONTINUATION;
    data_[1] = WEBSOCKET_FRAME_LEN_UINT16;
    data_[2] = (data_size_ >> 8) & 0xFF;
    data_[3] = data_size_ & 0xFF;
    memcpy(data_+ 4, textFrames_.front().data(), data_size_);
    send_size = data_size_ + 4;
  }
  data_[0] |= OP_TEXT;
  LOG(CONFIG_HTTP_WS_LOG_LEVEL, "[WebSocket fd:%d] send:%zu, text:%zu", fd_,
      send_size, textFrames_.front().length());
  return write_repeated(&helper_, fd_, data_, send_size, STATE(frame_sent));
}

StateFlowBase::Action WebSocketFlow::frame_sent()
{
  if (helper_.hasError_)
  {
    LOG_ERROR("[WebSocket fd:%d] read-error (%d: %s), disconnecting (frame)",
              fd_, errno, strerror(errno));
    return yield_and_call(STATE(shutdown_connection));
  }
  OSMutexLock h(&textLock_);
  textFrames_.front().erase(0, data_size_);

  // check if the frame being sent has been sent completely, if so drop it from
  // the pending frames.
  if (textFrames_.front().empty())
  {
    textFrames_.erase(textFrames_.begin());
  }

  // if we still have frame data to send, send another frame header now.
  if (!textFrames_.empty())
  {
    return yield_and_call(STATE(send_frame_header));
  }

  // out of frames to send, try and receive one.
  return yield_and_call(STATE(read_frame_header));
}

} // namespace http
