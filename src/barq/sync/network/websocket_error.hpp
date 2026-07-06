/*************************************************************************
 *
 * Copyright 2023 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/


#pragma once

#include "barq/error_codes.h"

#include <ostream>

namespace barq::sync::websocket {

enum class WebSocketError {
    websocket_ok = BARQ_ERR_WEBSOCKET_OK,
    websocket_going_away = BARQ_ERR_WEBSOCKET_GOINGAWAY,
    websocket_protocol_error = BARQ_ERR_WEBSOCKET_PROTOCOLERROR,
    websocket_unsupported_data = BARQ_ERR_WEBSOCKET_UNSUPPORTEDDATA,
    websocket_reserved = BARQ_ERR_WEBSOCKET_RESERVED,
    websocket_no_status_received = BARQ_ERR_WEBSOCKET_NOSTATUSRECEIVED,
    websocket_abnormal_closure = BARQ_ERR_WEBSOCKET_ABNORMALCLOSURE,
    websocket_invalid_payload_data = BARQ_ERR_WEBSOCKET_INVALIDPAYLOADDATA,
    websocket_policy_violation = BARQ_ERR_WEBSOCKET_POLICYVIOLATION,
    websocket_message_too_big = BARQ_ERR_WEBSOCKET_MESSAGETOOBIG,
    websocket_invalid_extension = BARQ_ERR_WEBSOCKET_INAVALIDEXTENSION,
    websocket_internal_server_error = BARQ_ERR_WEBSOCKET_INTERNALSERVERERROR,
    websocket_tls_handshake_failed = BARQ_ERR_WEBSOCKET_TLSHANDSHAKEFAILED, // Used by default WebSocket

    // WebSocket Errors - reported by server
    websocket_unauthorized = BARQ_ERR_WEBSOCKET_UNAUTHORIZED,
    websocket_forbidden = BARQ_ERR_WEBSOCKET_FORBIDDEN,
    websocket_moved_permanently = BARQ_ERR_WEBSOCKET_MOVEDPERMANENTLY,

    websocket_resolve_failed = BARQ_ERR_WEBSOCKET_RESOLVE_FAILED,
    websocket_connection_failed = BARQ_ERR_WEBSOCKET_CONNECTION_FAILED,
    websocket_read_error = BARQ_ERR_WEBSOCKET_READ_ERROR,
    websocket_write_error = BARQ_ERR_WEBSOCKET_WRITE_ERROR,
    websocket_retry_error = BARQ_ERR_WEBSOCKET_RETRY_ERROR,
    websocket_fatal_error = BARQ_ERR_WEBSOCKET_FATAL_ERROR,
};

std::ostream& operator<<(std::ostream& os, WebSocketError code);

} // namespace barq::sync::websocket
