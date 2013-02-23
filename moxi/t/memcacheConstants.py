#!/usr/bin/env python
"""

Copyright (c) 2007  Dustin Sallings <dustin@spy.net>
"""

import struct

# Command constants
CMD_GET = 0
CMD_SET = 1
CMD_ADD = 2
CMD_REPLACE = 3
CMD_DELETE = 4
CMD_INCR = 5
CMD_DECR = 6
CMD_QUIT = 7
CMD_FLUSH = 8
CMD_GETQ = 9
CMD_NOOP = 0x0a
CMD_VERSION = 0x0b
CMD_GETK = 0x0c
CMD_GETKQ = 0x0d
CMD_APPEND = 0x0e
CMD_PREPEND = 0x0f
CMD_STAT = 0x10

# SASL stuff
CMD_SASL_LIST_MECHS = 0x20
CMD_SASL_AUTH = 0x21
CMD_SASL_STEP = 0x22

# Bucket extension
CMD_CREATE_BUCKET = 0x25
CMD_DELETE_BUCKET = 0x26
CMD_LIST_BUCKETS = 0x27
CMD_EXPAND_BUCKET = 0x28

CMD_STOP_PERSISTENCE = 0x80
CMD_START_PERSISTENCE = 0x81
CMD_SET_FLUSH_PARAM = 0x82

CMD_START_REPLICATION = 0x90
CMD_STOP_REPLICATION = 0x91
CMD_SET_TAP_PARAM = 0x92
CMD_EVICT_KEY = 0x93

# Replication
CMD_TAP_CONNECT = 0x40
CMD_TAP_MUTATION = 0x41
CMD_TAP_DELETE = 0x42
CMD_TAP_FLUSH = 0x43
CMD_TAP_OPAQUE = 0x44

# vbucket stuff
CMD_SET_VBUCKET_STATE = 0x83
CMD_GET_VBUCKET_STATE = 0x84
CMD_DELETE_VBUCKET = 0x85

COMMAND_NAMES = dict(((globals()[k], k) for k in globals() if k.startswith("CMD_")))

# TAP flags
TAP_FLAG_BACKFILL = 0x01
TAP_FLAG_DUMP = 0x02

TAP_FLAG_TYPES = {TAP_FLAG_BACKFILL: ">Q",
                  TAP_FLAG_DUMP: ""}

# Flags, expiration
SET_PKT_FMT=">II"

# flags
GET_RES_FMT=">I"

# How long until the deletion takes effect.
DEL_PKT_FMT=""

## TAP stuff
# eng-specific length, flags, ttl, [res, res, res]; item flags, exp
TAP_MUTATION_PKT_FMT = "HHbxxxII"
TAP_GENERAL_PKT_FMT = "HHbxxx"

# amount, initial value, expiration
INCRDECR_PKT_FMT=">QQI"
# Special incr expiration that means do not store
INCRDECR_SPECIAL=0xffffffff
INCRDECR_RES_FMT=">Q"

# Time bomb
FLUSH_PKT_FMT=">I"

MAGIC_BYTE = 0x80
REQ_MAGIC_BYTE = 0x80
RES_MAGIC_BYTE = 0x81

# magic, opcode, keylen, extralen, datatype, vbucket, bodylen, opaque, cas
REQ_PKT_FMT=">BBHBBHIIQ"
# magic, opcode, keylen, extralen, datatype, status, bodylen, opaque, cas
RES_PKT_FMT=">BBHBBHIIQ"
# min recv packet size
MIN_RECV_PACKET = struct.calcsize(REQ_PKT_FMT)
# The header sizes don't deviate
assert struct.calcsize(REQ_PKT_FMT) == struct.calcsize(RES_PKT_FMT)

EXTRA_HDR_FMTS={
    CMD_SET: SET_PKT_FMT,
    CMD_ADD: SET_PKT_FMT,
    CMD_REPLACE: SET_PKT_FMT,
    CMD_INCR: INCRDECR_PKT_FMT,
    CMD_DECR: INCRDECR_PKT_FMT,
    CMD_DELETE: DEL_PKT_FMT,
    CMD_FLUSH: FLUSH_PKT_FMT,
    CMD_TAP_MUTATION: TAP_MUTATION_PKT_FMT,
    CMD_TAP_DELETE: TAP_GENERAL_PKT_FMT,
    CMD_TAP_FLUSH: TAP_GENERAL_PKT_FMT,
    CMD_TAP_OPAQUE: TAP_GENERAL_PKT_FMT,
}

EXTRA_HDR_SIZES=dict(
    [(k, struct.calcsize(v)) for (k,v) in EXTRA_HDR_FMTS.items()])

ERR_UNKNOWN_CMD = 0x81
ERR_NOT_FOUND = 0x1
ERR_EXISTS = 0x2
ERR_AUTH = 0x20
ERR_AUTH_CONTINUE = 0x21

ERR_SUCCESS = 0x00
ERR_KEY_ENOENT = 0x01
ERR_KEY_EEXISTS = 0x02
ERR_E2BIG = 0x03
ERR_EINVAL = 0x04
ERR_NOT_STORED = 0x05
ERR_DELTA_BADVAL = 0x06
ERR_NOT_MY_VBUCKET = 0x07
ERR_AUTH_ERROR = 0x20
ERR_AUTH_CONTINUE = 0x21
ERR_UNKNOWN_COMMAND = 0x81
ERR_ENOMEM = 0x82
ERR_NOT_SUPPORTED = 0x83
ERR_EINTERNAL = 0x84
ERR_EBUSY = 0x85
