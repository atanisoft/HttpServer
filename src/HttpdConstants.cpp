/*
 * SPDX-FileCopyrightText: 2019 Mike Dunston (atanisoft)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <utils/constants.hxx>

namespace http
{

///////////////////////////////////////////////////////////////////////////////
// Httpd constants
///////////////////////////////////////////////////////////////////////////////

DEFAULT_CONST(httpd_server_stack_size, 6144);
DEFAULT_CONST(httpd_server_priority, 0);
DEFAULT_CONST(httpd_header_chunk_size, 512);
DEFAULT_CONST(httpd_body_chunk_size, 3072);
DEFAULT_CONST(httpd_response_chunk_size, 2048);
DEFAULT_CONST(httpd_max_header_size, 1024);
DEFAULT_CONST(httpd_max_header_count, 20);
DEFAULT_CONST(httpd_max_param_count, 20);
DEFAULT_CONST(httpd_max_req_size, 4194304);
DEFAULT_CONST(httpd_max_req_per_connection, 5);
DEFAULT_CONST(httpd_req_timeout_ms, 5);
DEFAULT_CONST(httpd_socket_timeout_ms, 50);
DEFAULT_CONST(httpd_websocket_timeout_ms, 5);
DEFAULT_CONST(httpd_websocket_max_frame_size, 512);
DEFAULT_CONST(httpd_websocket_max_read_attempts, 2);
DEFAULT_CONST(httpd_websocket_max_uris, 1);
DEFAULT_CONST(httpd_websocket_max_clients, 10);
DEFAULT_CONST(httpd_websocket_max_pending_frames, 20);
DEFAULT_CONST(httpd_cache_max_age_sec, 300);

///////////////////////////////////////////////////////////////////////////////
// Dnsd constants
///////////////////////////////////////////////////////////////////////////////

DEFAULT_CONST(dnsd_priority, 0);
DEFAULT_CONST(dnsd_stack_size, 2048);
DEFAULT_CONST(dnsd_buffer_size, 512);

} // namespace http