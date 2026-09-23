#pragma once
/* QuorumESP — portable protocol constants (no IDF dependency by design).
 *
 * Values mirror the reference implementation:
 * - qdevices/tlv.h  (options, enums)
 * - qdevices/msg.h  (message types)
 * - qdevices/qnet-config.h (defaults)
 * Equivalence with the reference is proven by byte vectors in test/
 * (vectors generated from host/ + captured from a real client).
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes. Negative = fail-closed, never use partial output. */
#define QESP_OK 0
#define QESP_ERR_INVAL (-1)   /* bad argument or bad content */
#define QESP_ERR_NOMEM (-2)   /* destination buffer too small */
#define QESP_ERR_TRUNC (-3)   /* input shorter than claimed */
#define QESP_ERR_OVERRUN (-4) /* TLV overruns its message */
#define QESP_ERR_RANGE (-5)   /* enum value out of range */

/* Message types (msg.h). */
#define QESP_MSG_PREINIT 0
#define QESP_MSG_PREINIT_REPLY 1
#define QESP_MSG_STARTTLS 2
#define QESP_MSG_INIT 3
#define QESP_MSG_INIT_REPLY 4
#define QESP_MSG_SERVER_ERROR 5
#define QESP_MSG_SET_OPTION 6
#define QESP_MSG_SET_OPTION_REPLY 7
#define QESP_MSG_ECHO_REQUEST 8
#define QESP_MSG_ECHO_REPLY 9
#define QESP_MSG_NODE_LIST 10
#define QESP_MSG_NODE_LIST_REPLY 11
#define QESP_MSG_ASK_FOR_VOTE 12
#define QESP_MSG_ASK_FOR_VOTE_REPLY 13
#define QESP_MSG_VOTE_INFO 14
#define QESP_MSG_VOTE_INFO_REPLY 15
#define QESP_MSG_HEURISTICS_CHANGE 16
#define QESP_MSG_HEURISTICS_CHANGE_REPLY 17
#define QESP_MSG_TYPE_MAX 17

/* TLV options (tlv.h). */
#define QESP_TLV_MSG_SEQ_NUMBER 0
#define QESP_TLV_CLUSTER_NAME 1
#define QESP_TLV_TLS_SUPPORTED 2
#define QESP_TLV_TLS_CLIENT_CERT_REQUIRED 3
#define QESP_TLV_SUPPORTED_MESSAGES 4
#define QESP_TLV_SUPPORTED_OPTIONS 5
#define QESP_TLV_REPLY_ERROR_CODE 6
#define QESP_TLV_SERVER_MAXIMUM_REQUEST_SIZE 7
#define QESP_TLV_SERVER_MAXIMUM_REPLY_SIZE 8
#define QESP_TLV_NODE_ID 9
#define QESP_TLV_SUPPORTED_DECISION_ALGORITHMS 10
#define QESP_TLV_DECISION_ALGORITHM 11
#define QESP_TLV_HEARTBEAT_INTERVAL 12
#define QESP_TLV_RING_ID 13
#define QESP_TLV_CONFIG_VERSION 14
#define QESP_TLV_DATA_CENTER_ID 15
#define QESP_TLV_NODE_STATE 16
#define QESP_TLV_NODE_INFO 17
#define QESP_TLV_NODE_LIST_TYPE 18
#define QESP_TLV_VOTE 19
#define QESP_TLV_QUORATE 20
#define QESP_TLV_TIE_BREAKER 21
#define QESP_TLV_HEURISTICS 22
#define QESP_TLV_KEEP_ACTIVE_PARTITION_TB 23

/* Enums (tlv.h). */
#define QESP_TLS_UNSUPPORTED 0
#define QESP_TLS_SUPPORTED 1
#define QESP_TLS_REQUIRED 2

#define QESP_ALGO_TEST 0
#define QESP_ALGO_FFSPLIT 1
#define QESP_ALGO_2NODELMS 2
#define QESP_ALGO_LMS 3

#define QESP_NODE_STATE_NOT_SET 0
#define QESP_NODE_STATE_MEMBER 1
#define QESP_NODE_STATE_DEAD 2
#define QESP_NODE_STATE_LEAVING 3

#define QESP_NL_INITIAL_CONFIG 0
#define QESP_NL_CHANGED_CONFIG 1
#define QESP_NL_MEMBERSHIP 2
#define QESP_NL_QUORUM 3

#define QESP_VOTE_UNDEFINED 0
#define QESP_VOTE_ACK 1
#define QESP_VOTE_NACK 2
#define QESP_VOTE_ASK_LATER 3
#define QESP_VOTE_WAIT_FOR_REPLY 4
#define QESP_VOTE_NO_CHANGE 5

#define QESP_QUORATE_INQUORATE 0
#define QESP_QUORATE_QUORATE 1

#define QESP_TB_LOWEST 1
#define QESP_TB_HIGHEST 2
#define QESP_TB_NODE_ID 3

#define QESP_HEUR_UNDEFINED 0
#define QESP_HEUR_PASS 1
#define QESP_HEUR_FAIL 2

#define QESP_KAP_DISABLED 0
#define QESP_KAP_ENABLED 1

/* Error codes (tlv.h TLV_REPLY_ERROR_CODE_*). */
#define QESP_E_NO_ERROR 0
#define QESP_E_TLS_REQUIRED 3
#define QESP_E_UNSUPPORTED_MESSAGE 4
#define QESP_E_MESSAGE_TOO_LONG 5
#define QESP_E_PREINIT_REQUIRED 6
#define QESP_E_DOESNT_CONTAIN_REQUIRED_OPTION 7
#define QESP_E_UNEXPECTED_MESSAGE 8
#define QESP_E_ERROR_DECODING_MSG 9
#define QESP_E_INIT_REQUIRED 11
#define QESP_E_UNSUPPORTED_DECISION_ALGORITHM 12
#define QESP_E_INVALID_HEARTBEAT_INTERVAL 13
#define QESP_E_UNSUPPORTED_DECISION_ALGORITHM_MESSAGE 14
#define QESP_E_INVALID_CONFIG_NODE_LIST 18
#define QESP_E_INVALID_MEMBERSHIP_NODE_LIST 19

/* Defaults (qnet-config.h). */
#define QESP_DEFAULT_PORT 5403
#define QESP_MSG_HEADER_LEN 6
#define QESP_INITIAL_MSG_SIZE 32768u
#define QESP_QDEVICE_MAX_RECEIVE_SIZE 16777216u
#define QESP_HEARTBEAT_MIN 1000u
#define QESP_HEARTBEAT_MAX 120000u

#ifdef __cplusplus
}
#endif
