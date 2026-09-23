'use strict';

/*
 * QuorumESP Phase 1 — protocol constants.
 *
 * Mirror of the reference implementation enums, NOT a new spec:
 * - qdevices/tlv.h  (TLV_OPT_*, TLS, error codes, algorithms, votes, ...)
 * - qdevices/msg.h  (MSG_TYPE_*)
 * - qdevices/qnet-config.h (defaults: port, sizes, timeouts)
 *
 * Sources:
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/tlv.h
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/msg.h
 * https://raw.githubusercontent.com/corosync/corosync-qdevice/master/qdevices/qnet-config.h
 */

const MSG = Object.freeze({
  PREINIT: 0,
  PREINIT_REPLY: 1,
  STARTTLS: 2,
  INIT: 3,
  INIT_REPLY: 4,
  SERVER_ERROR: 5,
  SET_OPTION: 6,
  SET_OPTION_REPLY: 7,
  ECHO_REQUEST: 8,
  ECHO_REPLY: 9,
  NODE_LIST: 10,
  NODE_LIST_REPLY: 11,
  ASK_FOR_VOTE: 12,
  ASK_FOR_VOTE_REPLY: 13,
  VOTE_INFO: 14,
  VOTE_INFO_REPLY: 15,
  HEURISTICS_CHANGE: 16,
  HEURISTICS_CHANGE_REPLY: 17,
});

const TLV = Object.freeze({
  MSG_SEQ_NUMBER: 0,
  CLUSTER_NAME: 1,
  TLS_SUPPORTED: 2,
  TLS_CLIENT_CERT_REQUIRED: 3,
  SUPPORTED_MESSAGES: 4,
  SUPPORTED_OPTIONS: 5,
  REPLY_ERROR_CODE: 6,
  SERVER_MAXIMUM_REQUEST_SIZE: 7,
  SERVER_MAXIMUM_REPLY_SIZE: 8,
  NODE_ID: 9,
  SUPPORTED_DECISION_ALGORITHMS: 10,
  DECISION_ALGORITHM: 11,
  HEARTBEAT_INTERVAL: 12,
  RING_ID: 13,
  CONFIG_VERSION: 14,
  DATA_CENTER_ID: 15,
  NODE_STATE: 16,
  NODE_INFO: 17,
  NODE_LIST_TYPE: 18,
  VOTE: 19,
  QUORATE: 20,
  TIE_BREAKER: 21,
  HEURISTICS: 22,
  KEEP_ACTIVE_PARTITION_TIE_BREAKER: 23,
});

const TLS = Object.freeze({
  UNSUPPORTED: 0,
  SUPPORTED: 1,
  REQUIRED: 2,
});

const REPLY_ERROR = Object.freeze({
  NO_ERROR: 0,
  UNSUPPORTED_NEEDED_MESSAGE: 1,
  UNSUPPORTED_NEEDED_OPTION: 2,
  TLS_REQUIRED: 3,
  UNSUPPORTED_MESSAGE: 4,
  MESSAGE_TOO_LONG: 5,
  PREINIT_REQUIRED: 6,
  DOESNT_CONTAIN_REQUIRED_OPTION: 7,
  UNEXPECTED_MESSAGE: 8,
  ERROR_DECODING_MSG: 9,
  INTERNAL_ERROR: 10,
  INIT_REQUIRED: 11,
  UNSUPPORTED_DECISION_ALGORITHM: 12,
  INVALID_HEARTBEAT_INTERVAL: 13,
  UNSUPPORTED_DECISION_ALGORITHM_MESSAGE: 14,
  TIE_BREAKER_DIFFERS_FROM_OTHER_NODES: 15,
  ALGORITHM_DIFFERS_FROM_OTHER_NODES: 16,
  DUPLICATE_NODE_ID: 17,
  INVALID_CONFIG_NODE_LIST: 18,
  INVALID_MEMBERSHIP_NODE_LIST: 19,
});

const ALGO = Object.freeze({
  TEST: 0,
  FFSPLIT: 1,
  NODE2LMS: 2,
  LMS: 3,
});

const NODE_STATE = Object.freeze({
  NOT_SET: 0,
  MEMBER: 1,
  DEAD: 2,
  LEAVING: 3,
});

const NODE_LIST_TYPE = Object.freeze({
  INITIAL_CONFIG: 0,
  CHANGED_CONFIG: 1,
  MEMBERSHIP: 2,
  QUORUM: 3,
});

const VOTE = Object.freeze({
  UNDEFINED: 0,
  ACK: 1,
  NACK: 2,
  ASK_LATER: 3,
  WAIT_FOR_REPLY: 4,
  NO_CHANGE: 5,
});

const QUORATE = Object.freeze({
  INQUORATE: 0,
  QUORATE: 1,
});

const TIE_BREAKER_MODE = Object.freeze({
  LOWEST: 1,
  HIGHEST: 2,
  NODE_ID: 3,
});

const HEURISTICS = Object.freeze({
  UNDEFINED: 0,
  PASS: 1,
  FAIL: 2,
});

const KAP_TB = Object.freeze({
  DISABLED: 0,
  ENABLED: 1,
});

/* Defaults from qnet-config.h */
const DEFAULTS = Object.freeze({
  PORT: 5403,
  HEADER_LEN: 6,
  INITIAL_MSG_SIZE: 1 << 15, // 32768
  QNETD_MAX_SEND_SIZE: 1 << 15,
  QNETD_MAX_RECEIVE_SIZE: 1 << 15,
  QDEVICE_MAX_RECEIVE_SIZE: 1 << 24, // 16777216
  HEARTBEAT_MIN: 1000,
  HEARTBEAT_MAX: 120000,
  DPD_COEFFICIENT: 1.5,
  CONNECT_TIMEOUT_MIN: 1000,
  CONNECT_TIMEOUT_MAX: 120000,
});

const SUPPORTED_MESSAGES = Object.freeze(
  Object.values(MSG).filter((v) => typeof v === 'number')
);
const SUPPORTED_OPTIONS = Object.freeze(
  Object.values(TLV).filter((v) => typeof v === 'number')
);
const SUPPORTED_ALGORITHMS = Object.freeze([ALGO.FFSPLIT, ALGO.LMS]);

module.exports = {
  MSG,
  TLV,
  TLS,
  REPLY_ERROR,
  ALGO,
  NODE_STATE,
  NODE_LIST_TYPE,
  VOTE,
  QUORATE,
  TIE_BREAKER_MODE,
  HEURISTICS,
  KAP_TB,
  DEFAULTS,
  SUPPORTED_MESSAGES,
  SUPPORTED_OPTIONS,
  SUPPORTED_ALGORITHMS,
};
