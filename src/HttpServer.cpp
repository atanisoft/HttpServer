/*
 * SPDX-FileCopyrightText: 2019 Mike Dunston (atanisoft)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Httpd.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/fcntl.h>
#include <sys/socket.h>

#ifndef CONFIG_HTTP_SERVER_LOG_LEVEL
#define CONFIG_HTTP_SERVER_LOG_LEVEL VERBOSE
#endif


#if defined(ESP_PLATFORM) && !CONFIG_IDF_TARGET_LINUX

#include <freertos_drivers/esp32/Esp32WiFiManager.hxx>

#include <esp_system.h>

#endif // ESP_PLATFORM && !CONFIG_IDF_TARGET_LINUX

namespace http
{

string HTTP_BUILD_TIME = __DATE__ " " __TIME__;

/// Callback for a newly accepted socket connection.
///
/// @param fd is the socket handle.
static void incoming_http_connection(int fd)
{
  Singleton<Httpd>::instance()->new_connection(fd);
}

Httpd::Httpd(MDNS *mdns, uint16_t port, const string &name,
             const string service_name): Service(&executor_), name_(name),
             mdns_(mdns), mdns_service_(service_name),
             executor_(name.c_str(), config_httpd_server_priority(),
                       config_httpd_server_stack_size()),
             port_(port)
{
  init_server();
}

Httpd::Httpd(ExecutorBase *executor, MDNS *mdns, uint16_t port,
             const string &name, const string service_name): Service(executor),
             name_(name), mdns_(mdns), mdns_service_(service_name),
             executor_(NO_THREAD()), externalExecutor_(true), port_(port)
{
  init_server();
}

#if defined(ESP_PLATFORM) && !CONFIG_IDF_TARGET_LINUX

Httpd::Httpd(openmrn_arduino::Esp32WiFiManager *wifi, MDNS *mdns,
             uint16_t port, const string &name, const string service_name)
  : Service(wifi->executor()), name_(name), mdns_(mdns),
  mdns_service_(service_name), executor_(NO_THREAD()), externalExecutor_(true),
  port_(port)
{
  init_server();

  // Hook into the Esp32WiFiManager to start/stop the listener automatically
  // based on the AP/Station interface status.
  wifi->register_network_up_callback(
    [&](esp_network_interface_t interface, uint32_t ip)
    {
      if (interface == esp_network_interface_t::SOFTAP_INTERFACE)
      {
        start_dns_listener(ntohl(ip));
      }
      start_http_listener();
    });
  wifi->register_network_down_callback(
    [&](esp_network_interface_t interface)
    {
      stop_server();
    });
}
#endif // ESP_PLATFORM && !CONFIG_IDF_TARGET_LINUX

Httpd::~Httpd()
{
  stop_http_listener();
  stop_dns_listener();
  if (!externalExecutor_)
  {
    executor_.shutdown();
  }
  handlers_.clear();
  static_uris_.clear();
  redirect_uris_.clear();
  websocket_uris_.clear();
  websockets_.clear();
}

void Httpd::uri(const std::string &uri, const size_t method_mask,
                RequestProcessor handler, StreamProcessor stream_handler)
{
  handlers_.insert(
    std::make_pair(uri, std::make_pair(method_mask, std::move(handler))));
  if (stream_handler)
  {
    stream_handlers_.insert(std::make_pair(uri, std::move(stream_handler)));
  }
}

void Httpd::uri(const std::string &uri, RequestProcessor handler)
{
  this->uri(uri, 0xFFFF, handler, nullptr);
}

void Httpd::redirect_uri(const string &source, const string &target)
{
  redirect_uris_.insert(
    std::make_pair(std::move(source),
                   std::make_shared<RedirectResponse>(target)));
}

void Httpd::static_uri(const string &uri, const uint8_t *payload,
                       const size_t length, const string &mime_type,
                       const string &encoding, bool cached)
{
  static_uris_.insert(
    std::make_pair(uri, std::make_shared<StaticResponse>(payload, length,
                                                         mime_type, encoding,
                                                         cached)));
  if (cached)
  {
    static_cached_.insert(
      std::make_pair(uri, std::make_shared<AbstractHttpResponse>(
                      HttpStatusCode::STATUS_NOT_MODIFIED)));
  }
}

void Httpd::websocket_uri(const string &uri, WebSocketHandler handler)
{
  if (websocket_uris_.size() < config_httpd_websocket_max_uris())
  {
    websocket_uris_[uri] = handler;
  }
  else
  {
    LOG(FATAL, "[Httpd] Discarding WebSocket URI '%s' as max URI limit has "
               "been reached!", uri.c_str());
  }
}

void Httpd::send_websocket_binary(int id, uint8_t *data, size_t len)
{
  const std::lock_guard<std::mutex> lock(websocketsLock_);
  if (websockets_.find(id) == websockets_.end())
  {
    LOG_ERROR("[Httpd] Attempt to send data to unknown websocket:%d, "
              "discarding.", id);
    return;
  }
  //websockets_[id]->send_binary(data, len);
}

void Httpd::send_websocket_text(int id, std::string &text)
{
  const std::lock_guard<std::mutex> lock(websocketsLock_);
  if (websockets_.find(id) == websockets_.end())
  {
    LOG_ERROR("[Httpd] Attempt to send text to unknown websocket:%d, "
              "discarding text.", id);
    return;
  }
  websockets_[id]->send_text(text);
}

void Httpd::broadcast_websocket_text(std::string &text)
{
  const std::lock_guard<std::mutex> lock(websocketsLock_);
  for (auto &client : websockets_)
  {
    client.second->send_text(text);
  }
}

void Httpd::new_connection(int fd)
{
  struct sockaddr_in source;
  socklen_t source_len = sizeof(sockaddr_in);
  if (getpeername(fd, (sockaddr *)&source, &source_len))
  {
    source.sin_addr.s_addr = 0;
  }
  LOG(CONFIG_HTTP_SERVER_LOG_LEVEL, "[%s fd:%d/%s] Connected", name_.c_str(),
      fd, ipv4_to_string(ntohl(source.sin_addr.s_addr)).c_str());
  // Set socket receive timeout
  ERRNOCHECK("setsockopt_recv_timeout",
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout_,
               sizeof(struct timeval)));

  // Set socket send timeout
  ERRNOCHECK("setsockopt_send_timeout",
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout_,
               sizeof(struct timeval)));

  // Reconfigure the socket for non-blocking operations
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

  // Start the HTTP processing flow or close the socket if we fail to allocate
  // the request handler.
  auto *req =
    new(std::nothrow) HttpRequestFlow(this, fd, ntohl(source.sin_addr.s_addr));
  if (req == nullptr)
  {
    LOG_ERROR("[%s fd:%d/%s] Failed to allocate request handler, closing!",
              name_.c_str(), fd,
              ipv4_to_string(ntohl(source.sin_addr.s_addr)).c_str());
    ::close(fd);
  }
}

void Httpd::captive_portal(string first_access_response,
                           string auth_uri, uint64_t auth_timeout)
{
  captive_response_ =
    std::make_shared<StringResponse>(first_access_response,
                                     MIME_TYPE_TEXT_HTML);
  captive_no_content_ = 
    std::make_shared<AbstractHttpResponse>(HttpStatusCode::STATUS_NO_CONTENT);
  captive_ok_ = 
    std::make_shared<AbstractHttpResponse>(HttpStatusCode::STATUS_OK);
  captive_msft_ncsi_ =
    std::make_shared<StringResponse>("Microsoft NCSI", MIME_TYPE_TEXT_PLAIN);
  captive_success_ = 
    std::make_shared<StringResponse>("success", MIME_TYPE_TEXT_PLAIN);
  captive_success_ios_= 
    std::make_shared<StringResponse>("<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                                     "<BODY>Success</BODY></HTML>",
                                     MIME_TYPE_TEXT_HTML);
  captive_auth_uri_.assign(std::move(auth_uri));
  captive_timeout_ = auth_timeout;
  captive_active_ = true;
}

void Httpd::start_server(in_addr_t dns_ip_address)
{
  if (dns_ip_address != INADDR_ANY && captive_active_)
  {
    start_dns_listener(ntohl(dns_ip_address));
  }
  start_http_listener();
}

void Httpd::stop_server()
{
  stop_http_listener();
  stop_dns_listener();
}

bool Httpd::has_websocket_connections()
{
  const std::lock_guard<std::mutex> lock(websocketsLock_);
  return websockets_.size();
}

void Httpd::init_server()
{
  // pre-initialize the timeout parameters for all sockets that are accepted
  socket_timeout_.tv_sec = 0;
  socket_timeout_.tv_usec = MSEC_TO_USEC(config_httpd_socket_timeout_ms());
}

void Httpd::schedule_cleanup(Executable *flow)
{
  Executable *target_flow = flow;
  // TODO: should this use (std::nothrow) and validate that the executor was
  // created?
  executor()->add(new CallbackExecutable([target_flow]()
  {
    delete target_flow;
  }));
}

void Httpd::start_http_listener()
{
  if (http_active_)
  {
    return;
  }
  LOG(INFO, "[%s] Starting HTTP listener on port %" PRIu16, name_.c_str(), port_);
  listener_.emplace(port_, incoming_http_connection, "HttpSocket");
  http_active_ = true;
  if (mdns_)
  {
    mdns_->publish(name_.c_str(), mdns_service_.c_str(), port_);
  }
}

void Httpd::start_dns_listener(uint32_t ip)
{
  if (dns_active_)
  {
    return;
  }
  LOG(INFO, "[%s] Starting DNS listener", name_.c_str());
  dns_.emplace(ip);
  dns_active_ = true;
}

void Httpd::stop_http_listener()
{
  if (http_active_)
  {
    LOG(INFO, "[%s] Shutting down HTTP listener", name_.c_str());
    listener_.reset();
    http_active_ = false;
    if (mdns_)
    {
      mdns_->unpublish(name_.c_str(), mdns_service_.c_str());
    }
    const std::lock_guard<std::mutex> lock(websocketsLock_);
    for (auto &client : websockets_)
    {
      client.second->request_close();
    }
  }
}

void Httpd::stop_dns_listener()
{
  if (dns_active_)
  {
    LOG(INFO, "[%s] Shutting down HTTP listener", name_.c_str());
    dns_.reset();
    dns_active_ = false;
  }
}

bool Httpd::add_websocket(int id, WebSocketFlow *ws)
{
  const std::lock_guard<std::mutex> lock(websocketsLock_);
  if (websockets_.size() < config_httpd_websocket_max_clients())
  {
    websockets_[id] = ws;
    return true;
  }
  LOG_ERROR("[%s] Rejecting WebSocket client as maximum concurrent clients "
            "has been reached.", name_.c_str());
  return false;
}

void Httpd::remove_websocket(int id)
{
  const std::lock_guard<std::mutex> lock(websocketsLock_);
  websockets_.erase(id);
}

bool Httpd::have_known_response(const string &uri)
{
  return static_uris_.count(uri) || redirect_uris_.count(uri);
}

std::shared_ptr<AbstractHttpResponse> Httpd::response(HttpRequest *request)
{
  if (static_uris_.count(request->uri()))
  {
    if (request->has_header(HttpHeader::IF_MODIFIED_SINCE) &&
       !request->header(HttpHeader::IF_MODIFIED_SINCE).compare(HTTP_BUILD_TIME))
    {
      return static_cached_[request->uri()];
    }
    return static_uris_[request->uri()];
  }
  else if (redirect_uris_.count(request->uri()))
  {
    return redirect_uris_[request->uri()];
  }
  return nullptr;
}

bool Httpd::is_request_too_large(HttpRequest *req)
{
  HASSERT(req);

  // if the request doesn't have the content-length header we can try to
  // process it but it likely will fail.
  if (req->has_header(HttpHeader::CONTENT_LENGTH))
  {
    uint32_t len = std::stoul(req->header(HttpHeader::CONTENT_LENGTH));
    if (len > config_httpd_max_req_size())
    {
      LOG_ERROR("[Httpd uri:%s] Request body too large %" PRIu32 " > %d!",
                req->uri().c_str(), len, config_httpd_max_req_size());
      // request size too big
      return true;
    }
  }
#if CONFIG_HTTP_SERVER_LOG_LEVEL == VERBOSE
  else
  {
    LOG(CONFIG_HTTP_SERVER_LOG_LEVEL,
        "[Httpd] Request does not have Content-Length header:\n%s",
        req->to_string().c_str());
  }
#endif

  return false;
}

bool Httpd::is_servicable_uri(HttpRequest *req)
{
  HASSERT(req);

  // check if it is a GET of a known URI or if it is a Websocket URI
  if ((req->method() == HttpMethod::GET && have_known_response(req->uri())) ||
      websocket_uris_.find(req->uri()) != websocket_uris_.end())
  {
    LOG(CONFIG_HTTP_SERVER_LOG_LEVEL, "[Httpd uri:%s] Known GET URI",
        req->uri().c_str());
    return true;
  }

  // check if it is a POST/PUT and there is a body payload
  if (req->has_header(HttpHeader::CONTENT_LENGTH) &&
      std::stoi(req->header(HttpHeader::CONTENT_LENGTH)) &&
     (req->method() == HttpMethod::POST ||
      req->method() == HttpMethod::PUT) &&
      req->content_type() == ContentType::MULTIPART_FORMDATA)
  {
    auto processor = stream_handler(req->uri());
    LOG(CONFIG_HTTP_SERVER_LOG_LEVEL,
        "[Httpd uri:%s] POST/PUT request, streamproc found: %d",
        req->uri().c_str(), processor != nullptr);
    return processor != nullptr;
  }

  // Check if we have a handler for the provided URI
  auto processor = handler(req->method(), req->uri());
  LOG(CONFIG_HTTP_SERVER_LOG_LEVEL, "[Httpd uri:%s] method: %s, proc: %d"
    , req->raw_method().c_str(), req->uri().c_str(), processor != nullptr);
  return processor != nullptr;
}

bool Httpd::is_captive_auth_required(const uint32_t remote_ip)
{
  if (captive_timeout_ == UINT32_MAX ||
      captive_auth_[remote_ip] < captive_timeout_)
  {
    return false;
  }

  return true;
}

void Httpd::refresh_captive_auth_timeout(const uint32_t remote_ip)
{
  if (captive_timeout_ == UINT32_MAX)
  {
    return;
  }
  captive_auth_[remote_ip] = os_get_time_monotonic() + captive_timeout_;
}

RequestProcessor Httpd::handler(HttpMethod method, const std::string &uri)
{
  LOG(CONFIG_HTTP_SERVER_LOG_LEVEL, "[Httpd uri:%s] Searching for URI handler",
      uri.c_str());
  if (handlers_.count(uri))
  {
    auto handler = handlers_[uri];
    if (handler.first & method)
    {
      return handler.second;
    }
  }
  LOG(CONFIG_HTTP_SERVER_LOG_LEVEL, "[Httpd uri:%s] No suitable handler found",
      uri.c_str());
  return nullptr;
}

StreamProcessor Httpd::stream_handler(const std::string &uri)
{
  LOG(CONFIG_HTTP_SERVER_LOG_LEVEL,
      "[Httpd uri:%s] Searching for URI stream handler", uri.c_str());
  if (stream_handlers_.count(uri))
  {
    return stream_handlers_[uri];
  }
  LOG(CONFIG_HTTP_SERVER_LOG_LEVEL,
      "[Httpd uri:%s] No suitable stream handler found", uri.c_str());
  return nullptr;
}

WebSocketHandler Httpd::ws_handler(const string &uri)
{
  if (websocket_uris_.find(uri) != websocket_uris_.end())
  {
    return websocket_uris_[uri];
  }
  return nullptr;
}

} // namespace http
