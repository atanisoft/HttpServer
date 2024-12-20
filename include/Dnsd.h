/*
 * SPDX-FileCopyrightText: 2019 Mike Dunston (atanisoft)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DNSD_H_
#define DNSD_H_

#include <stdint.h>
#include <os/OS.hxx>
#include <utils/constants.hxx>
#include <utils/Singleton.hxx>

namespace http
{

/// FreeRTOS task priority for the dnsd task.
DECLARE_CONST(dnsd_priority);

/// FreeRTOS task stack size for the dnsd task.
DECLARE_CONST(dnsd_stack_size);

/// Size of the dnsd work buffer.
DECLARE_CONST(dnsd_buffer_size);

/// Default port to listen on for DNS requests.
static constexpr uint16_t DEFAULT_DNS_PORT = 53;

/// Very basic DNS server that responds to all addresses with a single
/// pre-defined IP address.
class Dnsd : public Singleton<Dnsd>
{
public:
  /// Constructor.
  ///
  /// @param local_ip is the address to respond to all DNS requests with.
  /// @param name is the name of the thread created for the DNS server.
  /// @param port is the UDP port to listen on.
  Dnsd(uint32_t local_ip, std::string name = "dnsd",
       uint16_t port = DEFAULT_DNS_PORT);
  /// Destructor.
  ~Dnsd();

  /// Thread entry point for the DNS server.
  void dns_process_thread();
private:
  /// Cached copy of the IP address to respond to DNS requests with.
  uint32_t local_ip_;

  /// Name of the DNS server.
  std::string name_;

  /// UDP port that is being listened to.
  uint16_t port_;

  /// Thread used for the DNS server.
  OSThread dns_thread_;

  /// Internal flag used to request a shutdown of the DNS thread.
  bool shutdown_{false};

  /// Internal flag to indicate that the DNS thread has shutdown.
  bool shutdownComplete_{false};

  /// DNS request payload header.
  struct DNSHeader
  {
    /// Request identifier.
    uint16_t id;
    union {
      struct {
        /// Flag to indicate that recursion is requested.
        uint16_t rd  : 1;
        /// Flag to indicate that the message is truncated.
        uint16_t tc  : 1;
        /// Flag to indicate that this is an authoritative answer.
        uint16_t aa  : 1;
        /// Message type indicator.
        uint16_t opc : 4;
        /// Flag indicating this is a query (0) or response (1).
        uint16_t qr  : 1;
        /// Response code.
        uint16_t rc  : 4;
        /// Reserved.
        uint16_t z   : 3;
        /// Flag indicating recursion is available.
        uint16_t ra  : 1;
      };
      /// DNS request flags.
      uint16_t flags;
    };
    /// Number of questions in the payload.
    uint16_t questions;
    /// Number of answers in the payload.
    uint16_t answers;
    /// Number of authority entries in the payload.
    uint16_t authorties;
    /// Number of resource entities in the payload.
    uint16_t resources;
  } __attribute__((packed));

  /// DNS response payload.
  struct DNSResponse
  {
    /// Identifier of the response (0xC00C).
    uint16_t id;
    /// Number of answers in the response payload.
    uint16_t answer;
    /// Number of classes in the response payload.
    uint16_t classes;
    /// DNS entry time-to-live.
    uint32_t ttl;
    /// Length of the payload.
    uint16_t length;
    /// Response IP address (payload).
    uint32_t address;
  } __attribute__((packed));

  /// Buffer for the incoming request and the response payload.
  std::vector<uint8_t> buffer_;

  /// Flags to use for ::sendto() when responding to the DNS request.
  static constexpr int DNS_SENDTO_FLAGS = 0;

  /// Flags to use for ::recvfrom() when receiving the DNS request.
  static constexpr int DNS_RECVFROM_FLAGS = 0;
};

} // namespace http

#endif // DNSD_H_
