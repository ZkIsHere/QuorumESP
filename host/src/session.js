'use strict';

/*
 * QuorumESP Phase 1 — qnetd-side session state machine (test harness).
 *
 * Models the server side of the handshake so the harness can verify
 * client behavior and its own fail-closed handling:
 *
 *   CONNECTED -> PREINIT_DONE -> [TLS_UPGRADED] -> ACTIVE -> CLOSED
 *
 * Fail-closed rules (AGENTS.md §8): any protocol violation, malformed
 * message, unexpected message, or TLS policy breach closes the session
 * without entering ACTIVE. The fixed reply vote is a test stub — it is
 * NOT a quorum algorithm (FFSplit/LMS need Phase 4 interop proof).
 *
 * Reference: qdevices/qnetd-client-msg-received.c (server message dispatch),
 * qdevices/qdevice-net-msg-received.c (client expectations), msg.h/tlv.h.
 */

const {
  MSG,
  TLS,
  REPLY_ERROR,
  ALGO,
  VOTE,
  HEURISTICS,
  DEFAULTS,
} = require('./consts');
const msg = require('./msg');
const tlv = require('./tlv');
const { TLV } = require('./consts');

const STATE = Object.freeze({
  CONNECTED: 'CONNECTED',
  PREINIT_DONE: 'PREINIT_DONE',
  TLS_UPGRADED: 'TLS_UPGRADED',
  ACTIVE: 'ACTIVE',
  CLOSED: 'CLOSED',
});

class SessionError extends Error {
  constructor(message, errorCode = REPLY_ERROR.INTERNAL_ERROR) {
    super(message);
    this.name = 'SessionError';
    this.errorCode = errorCode;
  }
}

/* Client tls= on/off/required semantics (man corosync-qdevice(8)). */
function clientShouldStartTls(clientMode, serverTls) {
  if (clientMode === 'off') {
    return serverTls === TLS.REQUIRED ? 'abort' : 'plaintext';
  }
  if (clientMode === 'on') {
    return serverTls === TLS.UNSUPPORTED ? 'plaintext' : 'starttls';
  }
  // clientMode === 'required'
  return serverTls === TLS.UNSUPPORTED ? 'abort' : 'starttls';
}

class QnetSession {
  constructor({
    tlsMode = 'on', // qnetd -s on|off|req  → advertised TLS.SUPPORTED/UNSUPPORTED/REQUIRED
    clientCertRequired = true,
    fixedVote = VOTE.ACK, // TEST STUB, not a quorum decision
    maxReceiveSize = DEFAULTS.QNETD_MAX_RECEIVE_SIZE,
    maxSendSize = DEFAULTS.QNETD_MAX_SEND_SIZE,
    nowMs = () => Date.now(),
  } = {}) {
    if (!['on', 'off', 'req'].includes(tlsMode)) {
      throw new SessionError(`invalid tlsMode ${tlsMode}`);
    }
    this.tlsMode = tlsMode;
    this.advertisedTls =
      tlsMode === 'off' ? TLS.UNSUPPORTED : tlsMode === 'req' ? TLS.REQUIRED : TLS.SUPPORTED;
    this.clientCertRequired = clientCertRequired;
    this.fixedVote = fixedVote;
    this.maxReceiveSize = maxReceiveSize;
    this.maxSendSize = maxSendSize;
    this.nowMs = nowMs;
    this.state = STATE.CONNECTED;
    this.clusterName = null;
    this.nodeId = null;
    this.algorithm = null;
    this.heartbeatInterval = null;
    this.lastActivityMs = this.nowMs();
    this.tlsUpgraded = false;
    this.closedReason = null;
  }

  get isActive() {
    return this.state === STATE.ACTIVE;
  }

  get isClosed() {
    return this.state === STATE.CLOSED;
  }

  /* Dead-peer detection: alive while last activity within interval * coeff. */
  isAlive(atMs = this.nowMs(), coeff = DEFAULTS.DPD_COEFFICIENT) {
    if (this.heartbeatInterval === null) {
      return this.state !== STATE.CLOSED;
    }
    return atMs - this.lastActivityMs <= this.heartbeatInterval * coeff;
  }

  close(reason) {
    this.state = STATE.CLOSED;
    this.closedReason = reason;
    return null; // no further replies after close
  }

  serverErrorReply(code, seq) {
    return msg.serverError(code, seq);
  }

  /*
   * Handle one complete frame. Returns a reply Buffer, null (no reply:
   * STARTTLS ack happens at transport level / session closed).
   * Throws SessionError on violation; caller must close the transport.
   */
  handle(frame) {
    if (this.state === STATE.CLOSED) {
      throw new SessionError('message on closed session', REPLY_ERROR.UNEXPECTED_MESSAGE);
    }
    let m;
    try {
      m = msg.decodeMessage(frame, this.maxReceiveSize);
    } catch (err) {
      throw new SessionError(`decode failed: ${err.message}`, REPLY_ERROR.ERROR_DECODING_MSG);
    }
    this.lastActivityMs = this.nowMs();

    switch (this.state) {
      case STATE.CONNECTED:
        return this.onConnected(m);
      case STATE.PREINIT_DONE:
      case STATE.TLS_UPGRADED:
        return this.onPreinitDone(m);
      case STATE.ACTIVE:
        return this.onActive(m, frame);
      default:
        throw new SessionError(`invalid state ${this.state}`, REPLY_ERROR.INTERNAL_ERROR);
    }
  }

  requirePreinit(m) {
    if (m.type !== MSG.PREINIT || typeof m.clusterName !== 'string' || m.clusterName.length === 0) {
      throw new SessionError('PREINIT with cluster_name required', REPLY_ERROR.PREINIT_REQUIRED);
    }
  }

  onConnected(m) {
    this.requirePreinit(m);
    this.clusterName = m.clusterName;
    this.state = STATE.PREINIT_DONE;
    return msg.preinitReply(this.advertisedTls, this.clientCertRequired, m.seq);
  }

  onPreinitDone(m) {
    if (m.type === MSG.STARTTLS) {
      if (this.tlsMode === 'off') {
        throw new SessionError('STARTTLS when TLS disabled', REPLY_ERROR.UNSUPPORTED_MESSAGE);
      }
      // No STARTTLS reply exists in the protocol; TLS handshake follows on
      // the transport, then the client sends INIT. Harness marks the intent.
      this.tlsUpgraded = true;
      this.state = STATE.TLS_UPGRADED;
      return null;
    }
    if (m.type !== MSG.INIT) {
      throw new SessionError(
        `expected INIT, got type ${m.type}`,
        this.state === STATE.CONNECTED ? REPLY_ERROR.PREINIT_REQUIRED : REPLY_ERROR.INIT_REQUIRED
      );
    }
    if (this.tlsMode === 'req' && !this.tlsUpgraded) {
      throw new SessionError('TLS required but STARTTLS missing', REPLY_ERROR.TLS_REQUIRED);
    }
    this.validateInit(m);
    this.nodeId = m.nodeId;
    this.algorithm = m.algorithm;
    this.heartbeatInterval = m.heartbeatInterval;
    this.state = STATE.ACTIVE;
    return msg.initReply({
      seq: m.seq,
      errorCode: REPLY_ERROR.NO_ERROR,
      maxRequest: this.maxReceiveSize,
      maxReply: this.maxSendSize,
      algorithms: [ALGO.FFSPLIT, ALGO.LMS],
    });
  }

  validateInit(m) {
    if (m.nodeId === undefined || m.nodeId === 0) {
      throw new SessionError('INIT without node_id', REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION);
    }
    if (![ALGO.FFSPLIT, ALGO.LMS].includes(m.algorithm)) {
      throw new SessionError(
        `unsupported algorithm ${m.algorithm}`,
        REPLY_ERROR.UNSUPPORTED_DECISION_ALGORITHM
      );
    }
    if (
      m.heartbeatInterval === undefined ||
      m.heartbeatInterval < DEFAULTS.HEARTBEAT_MIN ||
      m.heartbeatInterval > DEFAULTS.HEARTBEAT_MAX
    ) {
      throw new SessionError(
        `bad heartbeat ${m.heartbeatInterval}`,
        REPLY_ERROR.INVALID_HEARTBEAT_INTERVAL
      );
    }
    if (!m.tieBreaker || !m.ringId) {
      throw new SessionError('INIT without tie_breaker/ring_id', REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION);
    }
  }

  onActive(m, frame) {
    switch (m.type) {
      case MSG.ECHO_REQUEST: {
        // Reference msg_create_echo_reply: byte copy, type overwritten.
        const out = Buffer.from(frame);
        out.writeUInt16BE(MSG.ECHO_REPLY, 0);
        return out;
      }
      case MSG.NODE_LIST:
        if (m.seq === undefined || m.listType === undefined || !m.ringId) {
          throw new SessionError('bad NODE_LIST', REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION);
        }
        return msg.nodeListReply(m.seq, m.listType, m.ringId, this.fixedVote);
      case MSG.ASK_FOR_VOTE:
        if (m.seq === undefined) {
          throw new SessionError('bad ASK_FOR_VOTE', REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION);
        }
        return msg.askForVoteReply(m.seq, { nodeId: this.nodeId, seq: 0n }, this.fixedVote);
      case MSG.HEURISTICS_CHANGE:
        if (m.seq === undefined || m.heuristics === undefined) {
          throw new SessionError('bad HEURISTICS_CHANGE', REPLY_ERROR.DOESNT_CONTAIN_REQUIRED_OPTION);
        }
        if (![HEURISTICS.PASS, HEURISTICS.FAIL].includes(m.heuristics)) {
          throw new SessionError('bad heuristics value', REPLY_ERROR.ERROR_DECODING_MSG);
        }
        return msg.heuristicsChangeReply(
          m.seq,
          { nodeId: this.nodeId, seq: 0n },
          m.heuristics,
          this.fixedVote
        );
      case MSG.SET_OPTION: {
        const parts = [];
        if (m.seq !== undefined) {
          parts.push(tlv.encodeU32(TLV.MSG_SEQ_NUMBER, m.seq));
        }
        if (m.heartbeatInterval !== undefined) {
          parts.push(tlv.encodeU32(TLV.HEARTBEAT_INTERVAL, m.heartbeatInterval));
        }
        if (m.kapTb !== undefined) {
          parts.push(tlv.encodeU8(TLV.KEEP_ACTIVE_PARTITION_TIE_BREAKER, m.kapTb));
        }
        return msg.encodeMsg(MSG.SET_OPTION_REPLY, parts);
      }
      case MSG.VOTE_INFO_REPLY:
        return null; // ack of our VOTE_INFO, nothing to send back
      case MSG.PREINIT:
      case MSG.INIT:
      case MSG.STARTTLS:
        throw new SessionError(`unexpected type ${m.type} in ACTIVE`, REPLY_ERROR.UNEXPECTED_MESSAGE);
      default:
        throw new SessionError(`unsupported type ${m.type}`, REPLY_ERROR.UNSUPPORTED_MESSAGE);
    }
  }
}

module.exports = { STATE, SessionError, QnetSession, clientShouldStartTls };
