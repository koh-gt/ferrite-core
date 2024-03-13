#!/usr/bin/env python3
# Copyright (c) 2010 ArtForz -- public domain half-a-node
# Copyright (c) 2012 Jeff Garzik
# Copyright (c) 2010-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bitcoin test framework primitive and message structures

CBlock, CTransaction, CBlockHeader, CTxIn, CTxOut, etc....:
    data structures that should map to corresponding structures in
    bitcoin/primitives

msg_block, msg_tx, msg_headers, etc.:
    data structures that represent network messages

ser_*, deser_*: functions that handle serialization/deserialization.

Classes use __slots__ to ensure extraneous attributes aren't accidentally added
by tests, compromising their intended effect.
"""
import binascii
from codecs import encode
import copy
import hashlib
from io import BytesIO
import math
import random
import socket
import struct
import time

import ferrite_scrypt
from test_framework.siphash import siphash256
from test_framework.util import hex_str_to_bytes, assert_equal

MIN_VERSION_SUPPORTED = 60001
MY_VERSION = 70017  # past wtxid relay
MY_SUBVERSION = b"/python-p2p-tester:0.0.3/"
MY_RELAY = 1 # from version 70001 onwards, fRelay should be appended to version messages (BIP37)

MAX_LOCATOR_SZ = 101
MAX_BLOCK_BASE_SIZE = 1000000
MAX_BLOOM_FILTER_SIZE = 36000
MAX_BLOOM_HASH_FUNCS = 50

COIN = 100000000  # 1 btc in satoshis
MAX_MONEY = 60221400 * COIN

BIP125_SEQUENCE_NUMBER = 0xfffffffd  # Sequence number that is BIP 125 opt-in and BIP 68-opt-out

MAX_PROTOCOL_MESSAGE_LENGTH = 4000000  # Maximum length of incoming protocol messages
MAX_HEADERS_RESULTS = 2000  # Number of headers sent in one getheaders result
MAX_INV_SIZE = 50000  # Maximum number of entries in an 'inv' protocol message

NODE_NETWORK = (1 << 0)
NODE_GETUTXO = (1 << 1)
NODE_BLOOM = (1 << 2)
NODE_WITNESS = (1 << 3)
NODE_COMPACT_FILTERS = (1 << 6)
NODE_NETWORK_LIMITED = (1 << 10)
NODE_MWEB = (1 << 24)

MSG_TX = 1
MSG_BLOCK = 2
MSG_FILTERED_BLOCK = 3
MSG_CMPCT_BLOCK = 4
MSG_WTX = 5
MSG_WITNESS_FLAG = 1 << 30
MSG_MWEB_FLAG = 1 << 29
MSG_TYPE_MASK = 0xffffffff >> 3
MSG_WITNESS_TX = MSG_TX | MSG_WITNESS_FLAG
MSG_MWEB_BLOCK = MSG_BLOCK | MSG_WITNESS_FLAG | MSG_MWEB_FLAG
MSG_MWEB_TX = MSG_WITNESS_TX | MSG_MWEB_FLAG
MSG_MWEB_HEADER = 8 | MSG_MWEB_FLAG
MSG_MWEB_LEAFSET = 9 | MSG_MWEB_FLAG

FILTER_TYPE_BASIC = 0

WITNESS_SCALE_FACTOR = 4

# Serialization/deserialization tools
def sha256(s):
    return hashlib.new('sha256', s).digest()

def hash256(s):
    return sha256(sha256(s))

def ser_compact_size(l):
    r = b""
    if l < 253:
        r = struct.pack("B", l)
    elif l < 0x10000:
        r = struct.pack("<BH", 253, l)
    elif l < 0x100000000:
        r = struct.pack("<BI", 254, l)
    else:
        r = struct.pack("<BQ", 255, l)
    return r

def deser_compact_size(f):
    nit = struct.unpack("<B", f.read(1))[0]
    if nit == 253:
        nit = struct.unpack("<H", f.read(2))[0]
    elif nit == 254:
        nit = struct.unpack("<I", f.read(4))[0]
    elif nit == 255:
        nit = struct.unpack("<Q", f.read(8))[0]
    return nit

def deser_string(f):
    nit = deser_compact_size(f)
    return f.read(nit)

def ser_string(s):
    return ser_compact_size(len(s)) + s

def deser_uint256(f):
    r = 0
    for i in range(8):
        t = struct.unpack("<I", f.read(4))[0]
        r += t << (i * 32)
    return r


def ser_uint256(u):
    rs = b""
    for _ in range(8):
        rs += struct.pack("<I", u & 0xFFFFFFFF)
        u >>= 32
    return rs


def uint256_from_str(s):
    r = 0
    t = struct.unpack("<IIIIIIII", s[:32])
    for i in range(8):
        r += t[i] << (i * 32)
    return r


def uint256_from_compact(c):
    nbytes = (c >> 24) & 0xFF
    v = (c & 0xFFFFFF) << (8 * (nbytes - 3))
    return v


def deser_fixed_bytes(f, size):
    r = []
    for i in range(size):
        r.append(struct.unpack("B", f.read(1))[0])
    return r

def ser_fixed_bytes(u, size):
    rs = b""
    for i in range(size):
        rs += struct.pack("B", u[i])
        #rs += struct.pack("B", u & 0xFF)
        #u >>= 8
    return rs


def deser_pubkey(f):
    r = 0
    for i in range(33):
        t = struct.unpack("B", f.read(1))[0]
        r += t << (i * 8)
    return r

def ser_pubkey(u):
    rs = b""
    for _ in range(33):
        rs += struct.pack("B", u & 0xFF)
        u >>= 8
    return rs


def deser_signature(f):
    r = 0
    for i in range(64):
        t = struct.unpack("B", f.read(1))[0]
        r += t << (i * 8)
    return r

def ser_signature(u):
    rs = b""
    for _ in range(64):
        rs += struct.pack("B", u & 0xFF)
        u >>= 8
    return rs



# deser_function_name: Allow for an alternate deserialization function on the
# entries in the vector.
def deser_vector(f, c, deser_function_name=None):
    nit = deser_compact_size(f)
    r = []
    for _ in range(nit):
        t = c()
        if deser_function_name:
            getattr(t, deser_function_name)(f)
        else:
            t.deserialize(f)
        r.append(t)
    return r


# ser_function_name: Allow for an alternate serialization function on the
# entries in the vector (we use this for serializing the vector of transactions
# for a witness block).
def ser_vector(l, ser_function_name=None):
    r = ser_compact_size(len(l))
    for i in l:
        if ser_function_name:
            r += getattr(i, ser_function_name)()
        else:
            r += i.serialize()
    return r


def deser_uint256_vector(f):
    nit = deser_compact_size(f)
    r = []
    for _ in range(nit):
        t = deser_uint256(f)
        r.append(t)
    return r


def ser_uint256_vector(l):
    r = ser_compact_size(len(l))
    for i in l:
        r += ser_uint256(i)
    return r


def deser_string_vector(f):
    nit = deser_compact_size(f)
    r = []
    for _ in range(nit):
        t = deser_string(f)
        r.append(t)
    return r


def ser_string_vector(l):
    r = ser_compact_size(len(l))
    for sv in l:
        r += ser_string(sv)
    return r


# Deserialize from a hex string representation (eg from RPC)
def FromHex(obj, hex_string):
    obj.deserialize(BytesIO(hex_str_to_bytes(hex_string)))
    return obj

# Convert a binary-serializable object to hex (eg for submission via RPC)
def ToHex(obj):
    return obj.serialize().hex()

# Objects that map to bitcoind objects, which can be serialized/deserialized


class CAddress:
    __slots__ = ("net", "ip", "nServices", "port", "time")

    # see https://github.com/bitcoin/bips/blob/master/bip-0155.mediawiki
    NET_IPV4 = 1

    ADDRV2_NET_NAME = {
        NET_IPV4: "IPv4"
    }

    ADDRV2_ADDRESS_LENGTH = {
        NET_IPV4: 4
    }

    def __init__(self):
        self.time = 0
        self.nServices = 1
        self.net = self.NET_IPV4
        self.ip = "0.0.0.0"
        self.port = 0

    def deserialize(self, f, *, with_time=True):
        """Deserialize from addrv1 format (pre-BIP155)"""
        if with_time:
            # VERSION messages serialize CAddress objects without time
            self.time = struct.unpack("<I", f.read(4))[0]
        self.nServices = struct.unpack("<Q", f.read(8))[0]
        # We only support IPv4 which means skip 12 bytes and read the next 4 as IPv4 address.
        f.read(12)
        self.net = self.NET_IPV4
        self.ip = socket.inet_ntoa(f.read(4))
        self.port = struct.unpack(">H", f.read(2))[0]

    def serialize(self, *, with_time=True):
        """Serialize in addrv1 format (pre-BIP155)"""
        assert self.net == self.NET_IPV4
        r = b""
        if with_time:
            # VERSION messages serialize CAddress objects without time
            r += struct.pack("<I", self.time)
        r += struct.pack("<Q", self.nServices)
        r += b"\x00" * 10 + b"\xff" * 2
        r += socket.inet_aton(self.ip)
        r += struct.pack(">H", self.port)
        return r

    def deserialize_v2(self, f):
        """Deserialize from addrv2 format (BIP155)"""
        self.time = struct.unpack("<I", f.read(4))[0]

        self.nServices = deser_compact_size(f)

        self.net = struct.unpack("B", f.read(1))[0]
        assert self.net == self.NET_IPV4

        address_length = deser_compact_size(f)
        assert address_length == self.ADDRV2_ADDRESS_LENGTH[self.net]

        self.ip = socket.inet_ntoa(f.read(4))

        self.port = struct.unpack(">H", f.read(2))[0]

    def serialize_v2(self):
        """Serialize in addrv2 format (BIP155)"""
        assert self.net == self.NET_IPV4
        r = b""
        r += struct.pack("<I", self.time)
        r += ser_compact_size(self.nServices)
        r += struct.pack("B", self.net)
        r += ser_compact_size(self.ADDRV2_ADDRESS_LENGTH[self.net])
        r += socket.inet_aton(self.ip)
        r += struct.pack(">H", self.port)
        return r

    def __repr__(self):
        return ("CAddress(nServices=%i net=%s addr=%s port=%i)"
                % (self.nServices, self.ADDRV2_NET_NAME[self.net], self.ip, self.port))


class CInv:
    __slots__ = ("hash", "type")

    typemap = {
        0: "Error",
        MSG_TX: "TX",
        MSG_BLOCK: "Block",
        MSG_TX | MSG_WITNESS_FLAG: "WitnessTx",
        MSG_BLOCK | MSG_WITNESS_FLAG: "WitnessBlock",
        MSG_TX | MSG_WITNESS_FLAG | MSG_MWEB_FLAG: "MWEB Tx",
        MSG_BLOCK | MSG_WITNESS_FLAG | MSG_MWEB_FLAG: "MWEB Block",
        MSG_FILTERED_BLOCK: "filtered Block",
        MSG_CMPCT_BLOCK: "CompactBlock",
        MSG_WTX: "WTX",
        MSG_MWEB_HEADER: "MWEB Header",
        MSG_MWEB_LEAFSET: "MWEB Leafset"
    }

    def __init__(self, t=0, h=0):
        self.type = t
        self.hash = h

    def deserialize(self, f):
        self.type = struct.unpack("<I", f.read(4))[0]
        self.hash = deser_uint256(f)

    def serialize(self):
        r = b""
        r += struct.pack("<I", self.type)
        r += ser_uint256(self.hash)
        return r

    def __repr__(self):
        return "CInv(type=%s hash=%064x)" \
            % (self.typemap[self.type], self.hash)

    def __eq__(self, other):
        return isinstance(other, CInv) and self.hash == other.hash and self.type == other.type


class CBlockLocator:
    __slots__ = ("nVersion", "vHave")

    def __init__(self):
        self.nVersion = MY_VERSION
        self.vHave = []

    def deserialize(self, f):
        self.nVersion = struct.unpack("<i", f.read(4))[0]
        self.vHave = deser_uint256_vector(f)

    def serialize(self):
        r = b""
        r += struct.pack("<i", self.nVersion)
        r += ser_uint256_vector(self.vHave)
        return r

    def __repr__(self):
        return "CBlockLocator(nVersion=%i vHave=%s)" \
            % (self.nVersion, repr(self.vHave))


class COutPoint:
    __slots__ = ("hash", "n")

    def __init__(self, hash=0, n=0):
        self.hash = hash
        self.n = n

    def deserialize(self, f):
        self.hash = deser_uint256(f)
        self.n = struct.unpack("<I", f.read(4))[0]

    def serialize(self):
        r = b""
        r += ser_uint256(self.hash)
        r += struct.pack("<I", self.n)
        return r

    def __repr__(self):
        return "COutPoint(hash=%064x n=%i)" % (self.hash, self.n)


class CTxIn:
    __slots__ = ("nSequence", "prevout", "scriptSig")

    def __init__(self, outpoint=None, scriptSig=b"", nSequence=0):
        if outpoint is None:
            self.prevout = COutPoint()
        else:
            self.prevout = outpoint
        self.scriptSig = scriptSig
        self.nSequence = nSequence

    def deserialize(self, f):
        self.prevout = COutPoint()
        self.prevout.deserialize(f)
        self.scriptSig = deser_string(f)
        self.nSequence = struct.unpack("<I", f.read(4))[0]

    def serialize(self):
        r = b""
        r += self.prevout.serialize()
        r += ser_string(self.scriptSig)
        r += struct.pack("<I", self.nSequence)
        return r

    def __repr__(self):
        return "CTxIn(prevout=%s scriptSig=%s nSequence=%i)" \
            % (repr(self.prevout), self.scriptSig.hex(),
               self.nSequence)


class CTxOut:
    __slots__ = ("nValue", "scriptPubKey")

    def __init__(self, nValue=0, scriptPubKey=b""):
        self.nValue = nValue
        self.scriptPubKey = scriptPubKey

    def deserialize(self, f):
        self.nValue = struct.unpack("<q", f.read(8))[0]
        self.scriptPubKey = deser_string(f)

    def serialize(self):
        r = b""
        r += struct.pack("<q", self.nValue)
        r += ser_string(self.scriptPubKey)
        return r

    def __repr__(self):
        return "CTxOut(nValue=%i.%08i scriptPubKey=%s)" \
            % (self.nValue // COIN, self.nValue % COIN,
               self.scriptPubKey.hex())


class CScriptWitness:
    __slots__ = ("stack",)

    def __init__(self):
        # stack is a vector of strings
        self.stack = []

    def __repr__(self):
        return "CScriptWitness(%s)" % \
               (",".join([x.hex() for x in self.stack]))

    def is_null(self):
        if self.stack:
            return False
        return True


class CTxInWitness:
    __slots__ = ("scriptWitness",)

    def __init__(self):
        self.scriptWitness = CScriptWitness()

    def deserialize(self, f):
        self.scriptWitness.stack = deser_string_vector(f)

    def serialize(self):
        return ser_string_vector(self.scriptWitness.stack)

    def __repr__(self):
        return repr(self.scriptWitness)

    def is_null(self):
        return self.scriptWitness.is_null()


class CTxWitness:
    __slots__ = ("vtxinwit",)

    def __init__(self):
        self.vtxinwit = []

    def deserialize(self, f):
        for i in range(len(self.vtxinwit)):
            self.vtxinwit[i].deserialize(f)

    def serialize(self):
        r = b""
        # This is different than the usual vector serialization --
        # we omit the length of the vector, which is required to be
        # the same length as the transaction's vin vector.
        for x in self.vtxinwit:
            r += x.serialize()
        return r

    def __repr__(self):
        return "CTxWitness(%s)" % \
               (';'.join([repr(x) for x in self.vtxinwit]))

    def is_null(self):
        for x in self.vtxinwit:
            if not x.is_null():
                return False
        return True


class CTransaction:
    __slots__ = ("hash", "nLockTime", "nVersion", "sha256", "vin", "vout",
                 "wit", "mweb_tx", "hogex")

    def __init__(self, tx=None):
        if tx is None:
            self.nVersion = 1
            self.vin = []
            self.vout = []
            self.wit = CTxWitness()
            self.nLockTime = 0
            self.sha256 = None
            self.hash = None
            self.mweb_tx = None
            self.hogex = False
        else:
            self.nVersion = tx.nVersion
            self.vin = copy.deepcopy(tx.vin)
            self.vout = copy.deepcopy(tx.vout)
            self.nLockTime = tx.nLockTime
            self.sha256 = tx.sha256
            self.hash = tx.hash
            self.wit = copy.deepcopy(tx.wit)
            self.mweb_tx = tx.mweb_tx
            self.hogex = tx.hogex

    def deserialize(self, f):
        self.nVersion = struct.unpack("<i", f.read(4))[0]
        self.vin = deser_vector(f, CTxIn)
        flags = 0
        if len(self.vin) == 0:
            flags = struct.unpack("<B", f.read(1))[0]
            # Not sure why flags can't be zero, but this
            # matches the implementation in bitcoind
            if (flags != 0):
                self.vin = deser_vector(f, CTxIn)
                self.vout = deser_vector(f, CTxOut)
        else:
            self.vout = deser_vector(f, CTxOut)
        if flags & 1:
            self.wit.vtxinwit = [CTxInWitness() for _ in range(len(self.vin))]
            self.wit.deserialize(f)
        else:
            self.wit = CTxWitness()

        if flags & 8:
            self.mweb_tx = deser_mweb_tx(f)
            if self.mweb_tx == None:
                self.hogex = True

        self.nLockTime = struct.unpack("<I", f.read(4))[0]
        self.sha256 = None
        self.hash = None

    def serialize_without_witness(self):
        r = b""
        r += struct.pack("<i", self.nVersion)
        r += ser_vector(self.vin)
        r += ser_vector(self.vout)
        r += struct.pack("<I", self.nLockTime)
        return r

    # Only serialize with witness when explicitly called for
    def serialize_with_witness(self):
        flags = 0
        if not self.wit.is_null():
            flags |= 1
        r = b""
        r += struct.pack("<i", self.nVersion)
        if flags:
            dummy = []
            r += ser_vector(dummy)
            r += struct.pack("<B", flags)
        r += ser_vector(self.vin)
        r += ser_vector(self.vout)
        if flags & 1:
            if (len(self.wit.vtxinwit) != len(self.vin)):
                # vtxinwit must have the same length as vin
                self.wit.vtxinwit = self.wit.vtxinwit[:len(self.vin)]
                for _ in range(len(self.wit.vtxinwit), len(self.vin)):
                    self.wit.vtxinwit.append(CTxInWitness())
            r += self.wit.serialize()
        r += struct.pack("<I", self.nLockTime)
        return r

    
    # Only serialize with mweb when explicitly called for
    def serialize_with_mweb(self):
        flags = 0
        if not self.wit.is_null():
            flags |= 1
        if self.hogex or self.mweb_tx != None:
            flags |= 8
        r = b""
        r += struct.pack("<i", self.nVersion)
        if flags:
            dummy = []
            r += ser_vector(dummy)
            r += struct.pack("<B", flags)
        r += ser_vector(self.vin)
        r += ser_vector(self.vout)
        if flags & 1:
            if (len(self.wit.vtxinwit) != len(self.vin)):
                # vtxinwit must have the same length as vin
                self.wit.vtxinwit = self.wit.vtxinwit[:len(self.vin)]
                for _ in range(len(self.wit.vtxinwit), len(self.vin)):
                    self.wit.vtxinwit.append(CTxInWitness())
            r += self.wit.serialize()
        if flags & 8:
            r += ser_mweb_tx(self.mweb_tx)
        r += struct.pack("<I", self.nLockTime)
        return r

    # Regular serialization is with mweb -- must explicitly
    # call serialize_with_witness to exclude mweb data or
    # serialize_without_witness to exclude witness & mweb data.
    def serialize(self):
        return self.serialize_with_mweb()

    # Recalculate the txid (transaction hash without witness)
    def rehash(self):
        self.sha256 = None
        self.calc_sha256()
        return self.hash

    # We will only cache the serialization without witness in
    # self.sha256 and self.hash -- those are expected to be the txid.
    def calc_sha256(self, with_witness=False):
        if with_witness:
            # Don't cache the result, just return it
            return uint256_from_str(hash256(self.serialize_with_witness()))

        if self.sha256 is None:
            self.sha256 = uint256_from_str(hash256(self.serialize_without_witness()))
        self.hash = encode(hash256(self.serialize_without_witness())[::-1], 'hex_codec').decode('ascii')

    def is_valid(self):
        self.calc_sha256()
        for tout in self.vout:
            if tout.nValue < 0 or tout.nValue > 84000000 * COIN:
                return False
        return True

    # Calculate the virtual transaction size using witness and non-witness
    # serialization size (does NOT use sigops).
    def get_vsize(self):
        with_witness_size = len(self.serialize_with_witness())
        without_witness_size = len(self.serialize_without_witness())
        return math.ceil(((WITNESS_SCALE_FACTOR - 1) * without_witness_size + with_witness_size) / WITNESS_SCALE_FACTOR)

    def __repr__(self):
        return "CTransaction(nVersion=%i vin=%s vout=%s wit=%s nLockTime=%i)" \
            % (self.nVersion, repr(self.vin), repr(self.vout), repr(self.wit), self.nLockTime)

    def __eq__(self, other):
            return isinstance(other, CTransaction) and repr(self) == repr(other)

class CBlockHeader:
    __slots__ = ("hash", "hashMerkleRoot", "hashPrevBlock", "nBits", "nNonce",
                 "nTime", "nVersion", "sha256", "scrypt256")

    def __init__(self, header=None):
        if header is None:
            self.set_null()
        else:
            self.nVersion = header.nVersion
            self.hashPrevBlock = header.hashPrevBlock
            self.hashMerkleRoot = header.hashMerkleRoot
            self.nTime = header.nTime
            self.nBits = header.nBits
            self.nNonce = header.nNonce
            self.sha256 = header.sha256
            self.hash = header.hash
            self.scrypt256 = header.scrypt256
            self.calc_sha256()

    def set_null(self):
        self.nVersion = 1
        self.hashPrevBlock = 0
        self.hashMerkleRoot = 0
        self.nTime = 0
        self.nBits = 0
        self.nNonce = 0
        self.sha256 = None
        self.hash = None
        self.scrypt256 = None

    def deserialize(self, f):
        self.nVersion = struct.unpack("<i", f.read(4))[0]
        self.hashPrevBlock = deser_uint256(f)
        self.hashMerkleRoot = deser_uint256(f)
        self.nTime = struct.unpack("<I", f.read(4))[0]
        self.nBits = struct.unpack("<I", f.read(4))[0]
        self.nNonce = struct.unpack("<I", f.read(4))[0]
        self.sha256 = None
        self.hash = None
        self.scrypt256 = None

    def serialize(self):
        r = b""
        r += struct.pack("<i", self.nVersion)
        r += ser_uint256(self.hashPrevBlock)
        r += ser_uint256(self.hashMerkleRoot)
        r += struct.pack("<I", self.nTime)
        r += struct.pack("<I", self.nBits)
        r += struct.pack("<I", self.nNonce)
        return r

    def calc_sha256(self):
        if self.sha256 is None:
            r = b""
            r += struct.pack("<i", self.nVersion)
            r += ser_uint256(self.hashPrevBlock)
            r += ser_uint256(self.hashMerkleRoot)
            r += struct.pack("<I", self.nTime)
            r += struct.pack("<I", self.nBits)
            r += struct.pack("<I", self.nNonce)
            self.sha256 = uint256_from_str(hash256(r))
            self.hash = encode(hash256(r)[::-1], 'hex_codec').decode('ascii')
            self.scrypt256 = uint256_from_str(ferrite_scrypt.getPoWHash(r))

    def rehash(self):
        self.sha256 = None
        self.scrypt256 = None
        self.calc_sha256()
        return self.sha256

    def __repr__(self):
        return "CBlockHeader(nVersion=%i hashPrevBlock=%064x hashMerkleRoot=%064x nTime=%s nBits=%08x nNonce=%08x)" \
            % (self.nVersion, self.hashPrevBlock, self.hashMerkleRoot,
               time.ctime(self.nTime), self.nBits, self.nNonce)

    def __eq__(self, other):
        return isinstance(other, CBlockHeader) and repr(self) == repr(other)

BLOCK_HEADER_SIZE = len(CBlockHeader().serialize())
assert_equal(BLOCK_HEADER_SIZE, 80)

class CBlock(CBlockHeader):
    __slots__ = ("vtx", "mweb_block")

    def __init__(self, header=None):
        super().__init__(header)
        self.vtx = []
        self.mweb_block = None

    def deserialize(self, f):
        super().deserialize(f)
        self.vtx = deser_vector(f, CTransaction)
        if len(self.vtx) > 0 and self.vtx[-1].hogex:
            self.mweb_block = deser_mweb_block(f)

    def serialize(self, with_witness=True, with_mweb=True):
        r = b""
        r += super().serialize()
        if with_mweb and with_witness:
            r += ser_vector(self.vtx, "serialize_with_mweb")
            if len(self.vtx) > 0 and self.vtx[-1].hogex:
                r += ser_mweb_block(self.mweb_block)
        elif with_witness:
            r += ser_vector(self.vtx, "serialize_with_witness")
        else:
            r += ser_vector(self.vtx, "serialize_without_witness")
        
        return r

    # Calculate the merkle root given a vector of transaction hashes
    @classmethod
    def get_merkle_root(cls, hashes):
        while len(hashes) > 1:
            newhashes = []
            for i in range(0, len(hashes), 2):
                i2 = min(i+1, len(hashes)-1)
                newhashes.append(hash256(hashes[i] + hashes[i2]))
            hashes = newhashes
        return uint256_from_str(hashes[0])

    def calc_merkle_root(self):
        hashes = []
        for tx in self.vtx:
            tx.calc_sha256()
            hashes.append(ser_uint256(tx.sha256))
        return self.get_merkle_root(hashes)

    def calc_witness_merkle_root(self):
        # For witness root purposes, the hash of the
        # coinbase, with witness, is defined to be 0...0
        hashes = [ser_uint256(0)]

        for tx in self.vtx[1:]:
            # Calculate the hashes with witness data
            hashes.append(ser_uint256(tx.calc_sha256(True)))

        return self.get_merkle_root(hashes)

    def is_valid(self):
        self.calc_sha256()
        target = uint256_from_compact(self.nBits)
        if self.scrypt256 > target:
            return False
        for tx in self.vtx:
            if not tx.is_valid():
                return False
        if self.calc_merkle_root() != self.hashMerkleRoot:
            return False
        return True

    def solve(self):
        self.rehash()
        target = uint256_from_compact(self.nBits)
        while self.scrypt256 > target:
            self.nNonce += 1
            self.rehash()

    def __repr__(self):
        return "CBlock(nVersion=%i hashPrevBlock=%064x hashMerkleRoot=%064x nTime=%s nBits=%08x nNonce=%08x vtx=%s)" \
            % (self.nVersion, self.hashPrevBlock, self.hashMerkleRoot,
               time.ctime(self.nTime), self.nBits, self.nNonce, repr(self.vtx))


class PrefilledTransaction:
    __slots__ = ("index", "tx")

    def __init__(self, index=0, tx = None):
        self.index = index
        self.tx = tx

    def deserialize(self, f):
        self.index = deser_compact_size(f)
        self.tx = CTransaction()
        self.tx.deserialize(f)

    def serialize(self, with_witness=True, with_mweb=True):
        r = b""
        r += ser_compact_size(self.index)
        if with_witness and with_mweb:
            r += self.tx.serialize_with_mweb()
        elif with_witness:
            r += self.tx.serialize_with_witness()
        else:
            r += self.tx.serialize_without_witness()
        return r

    def serialize_without_witness(self):
        return self.serialize(with_witness=False, with_mweb=False)

    def serialize_with_witness(self):
        return self.serialize(with_witness=True, with_mweb=False)

    def serialize_with_mweb(self):
        return self.serialize(with_witness=True, with_mweb=True)

    def __repr__(self):
        return "PrefilledTransaction(index=%d, tx=%s)" % (self.index, repr(self.tx))


# This is what we send on the wire, in a cmpctblock message.
class P2PHeaderAndShortIDs:
    __slots__ = ("header", "nonce", "prefilled_txn", "prefilled_txn_length",
                 "shortids", "shortids_length", "mweb_block")

    def __init__(self):
        self.header = CBlockHeader()
        self.nonce = 0
        self.shortids_length = 0
        self.shortids = []
        self.prefilled_txn_length = 0
        self.prefilled_txn = []
        self.mweb_block = None

    def deserialize(self, f):
        self.header.deserialize(f)
        self.nonce = struct.unpack("<Q", f.read(8))[0]
        self.shortids_length = deser_compact_size(f)
        for _ in range(self.shortids_length):
            # shortids are defined to be 6 bytes in the spec, so append
            # two zero bytes and read it in as an 8-byte number
            self.shortids.append(struct.unpack("<Q", f.read(6) + b'\x00\x00')[0])
        self.prefilled_txn = deser_vector(f, PrefilledTransaction)
        self.prefilled_txn_length = len(self.prefilled_txn)
        
        if len(self.prefilled_txn) > 0 and self.prefilled_txn[-1].tx.hogex:
            self.mweb_block = deser_mweb_block(f)
                
    # When using version 2 compact blocks, we must serialize with_witness.
    # When using version 3 compact blocks, we must serialize with_mweb.
    def serialize(self, version=1):
        r = b""
        r += self.header.serialize()
        r += struct.pack("<Q", self.nonce)
        r += ser_compact_size(self.shortids_length)
        for x in self.shortids:
            # We only want the first 6 bytes
            r += struct.pack("<Q", x)[0:6]
        if version >= 3:
            r += ser_vector(self.prefilled_txn, "serialize_with_mweb")
            r += ser_mweb_block(self.mweb_block)
        elif version == 2:
            r += ser_vector(self.prefilled_txn, "serialize_with_witness")
        else:
            r += ser_vector(self.prefilled_txn, "serialize_without_witness")
        return r

    def __repr__(self):
        return "P2PHeaderAndShortIDs(header=%s, nonce=%d, shortids_length=%d, shortids=%s, prefilled_txn_length=%d, prefilledtxn=%s" % (repr(self.header), self.nonce, self.shortids_length, repr(self.shortids), self.prefilled_txn_length, repr(self.prefilled_txn))

# Calculate the BIP 152-compact blocks shortid for a given transaction hash
def calculate_shortid(k0, k1, tx_hash):
    expected_shortid = siphash256(k0, k1, tx_hash)
    expected_shortid &= 0x0000ffffffffffff
    return expected_shortid


# This version gets rid of the array lengths, and reinterprets the differential
# encoding into indices that can be used for lookup.
class HeaderAndShortIDs:
    __slots__ = ("header", "nonce", "prefilled_txn", "shortids", "mweb_block")

    def __init__(self, p2pheaders_and_shortids = None):
        self.header = CBlockHeader()
        self.nonce = 0
        self.shortids = []
        self.prefilled_txn = []
        self.mweb_block = None

        if p2pheaders_and_shortids is not None:
            self.header = p2pheaders_and_shortids.header
            self.nonce = p2pheaders_and_shortids.nonce
            self.shortids = p2pheaders_and_shortids.shortids
            last_index = -1
            for x in p2pheaders_and_shortids.prefilled_txn:
                self.prefilled_txn.append(PrefilledTransaction(x.index + last_index + 1, x.tx))
                last_index = self.prefilled_txn[-1].index

    def to_p2p(self):
        ret = P2PHeaderAndShortIDs()
        ret.header = self.header
        ret.nonce = self.nonce
        ret.shortids_length = len(self.shortids)
        ret.shortids = self.shortids
        ret.mweb_block = self.mweb_block
        ret.prefilled_txn_length = len(self.prefilled_txn)
        ret.prefilled_txn = []
        last_index = -1
        for x in self.prefilled_txn:
            ret.prefilled_txn.append(PrefilledTransaction(x.index - last_index - 1, x.tx))
            last_index = x.index
        return ret

    def get_siphash_keys(self):
        header_nonce = self.header.serialize()
        header_nonce += struct.pack("<Q", self.nonce)
        hash_header_nonce_as_str = sha256(header_nonce)
        key0 = struct.unpack("<Q", hash_header_nonce_as_str[0:8])[0]
        key1 = struct.unpack("<Q", hash_header_nonce_as_str[8:16])[0]
        return [ key0, key1 ]

    # Version 2 compact blocks use wtxid in shortids (rather than txid)
    # Version 3 compact blocks include an optional mweb block
    def initialize_from_block(self, block, nonce=0, prefill_list=None, version=1):
        if prefill_list is None:
            prefill_list = [0]
        self.header = CBlockHeader(block)
        self.nonce = nonce
        self.prefilled_txn = [ PrefilledTransaction(i, block.vtx[i]) for i in prefill_list ]
        self.shortids = []
        self.mweb_block = block.mweb_block
        [k0, k1] = self.get_siphash_keys()
        for i in range(len(block.vtx)):
            if i not in prefill_list:
                tx_hash = block.vtx[i].sha256
                if version >= 2:
                    tx_hash = block.vtx[i].calc_sha256(with_witness=True)
                self.shortids.append(calculate_shortid(k0, k1, tx_hash))

    def __repr__(self):
        return "HeaderAndShortIDs(header=%s, nonce=%d, shortids=%s, prefilledtxn=%s" % (repr(self.header), self.nonce, repr(self.shortids), repr(self.prefilled_txn))


class BlockTransactionsRequest:
    __slots__ = ("blockhash", "indexes")

    def __init__(self, blockhash=0, indexes = None):
        self.blockhash = blockhash
        self.indexes = indexes if indexes is not None else []

    def deserialize(self, f):
        self.blockhash = deser_uint256(f)
        indexes_length = deser_compact_size(f)
        for _ in range(indexes_length):
            self.indexes.append(deser_compact_size(f))

    def serialize(self):
        r = b""
        r += ser_uint256(self.blockhash)
        r += ser_compact_size(len(self.indexes))
        for x in self.indexes:
            r += ser_compact_size(x)
        return r

    # helper to set the differentially encoded indexes from absolute ones
    def from_absolute(self, absolute_indexes):
        self.indexes = []
        last_index = -1
        for x in absolute_indexes:
            self.indexes.append(x-last_index-1)
            last_index = x

    def to_absolute(self):
        absolute_indexes = []
        last_index = -1
        for x in self.indexes:
            absolute_indexes.append(x+last_index+1)
            last_index = absolute_indexes[-1]
        return absolute_indexes

    def __repr__(self):
        return "BlockTransactionsRequest(hash=%064x indexes=%s)" % (self.blockhash, repr(self.indexes))


class BlockTransactions:
    __slots__ = ("blockhash", "transactions")

    def __init__(self, blockhash=0, transactions = None):
        self.blockhash = blockhash
        self.transactions = transactions if transactions is not None else []

    def deserialize(self, f):
        self.blockhash = deser_uint256(f)
        self.transactions = deser_vector(f, CTransaction)

    def serialize(self, with_witness=True, with_mweb=True):
        r = b""
        r += ser_uint256(self.blockhash)
        if with_mweb and with_witness:
            r += ser_vector(self.transactions, "serialize_with_mweb")
        elif with_witness:
            r += ser_vector(self.transactions, "serialize_with_witness")
        else:
            r += ser_vector(self.transactions, "serialize_without_witness")
        return r

    def __repr__(self):
        return "BlockTransactions(hash=%064x transactions=%s)" % (self.blockhash, repr(self.transactions))


class CPartialMerkleTree:
    __slots__ = ("nTransactions", "vBits", "vHash")

    def __init__(self):
        self.nTransactions = 0
        self.vHash = []
        self.vBits = []

    def deserialize(self, f):
        self.nTransactions = struct.unpack("<i", f.read(4))[0]
        self.vHash = deser_uint256_vector(f)
        vBytes = deser_string(f)
        self.vBits = []
        for i in range(len(vBytes) * 8):
            self.vBits.append(vBytes[i//8] & (1 << (i % 8)) != 0)

    def serialize(self):
        r = b""
        r += struct.pack("<i", self.nTransactions)
        r += ser_uint256_vector(self.vHash)
        vBytesArray = bytearray([0x00] * ((len(self.vBits) + 7)//8))
        for i in range(len(self.vBits)):
            vBytesArray[i // 8] |= self.vBits[i] << (i % 8)
        r += ser_string(bytes(vBytesArray))
        return r

    def __repr__(self):
        return "CPartialMerkleTree(nTransactions=%d, vHash=%s, vBits=%s)" % (self.nTransactions, repr(self.vHash), repr(self.vBits))


class CMerkleBlock:
    __slots__ = ("header", "txn")

    def __init__(self):
        self.header = CBlockHeader()
        self.txn = CPartialMerkleTree()

    def deserialize(self, f):
        self.header.deserialize(f)
        self.txn.deserialize(f)

    def serialize(self):
        r = b""
        r += self.header.serialize()
        r += self.txn.serialize()
        return r

    def __repr__(self):
        return "CMerkleBlock(header=%s, txn=%s)" % (repr(self.header), repr(self.txn))


# Objects that correspond to messages on the wire
class msg_version:
    __slots__ = ("addrFrom", "addrTo", "nNonce", "nRelay", "nServices",
                 "nStartingHeight", "nTime", "nVersion", "strSubVer")
    msgtype = b"version"

    def __init__(self):
        self.nVersion = MY_VERSION
        self.nServices = NODE_NETWORK | NODE_WITNESS | NODE_MWEB
        self.nTime = int(time.time())
        self.addrTo = CAddress()
        self.addrFrom = CAddress()
        self.nNonce = random.getrandbits(64)
        self.strSubVer = MY_SUBVERSION
        self.nStartingHeight = -1
        self.nRelay = MY_RELAY

    def deserialize(self, f):
        self.nVersion = struct.unpack("<i", f.read(4))[0]
        self.nServices = struct.unpack("<Q", f.read(8))[0]
        self.nTime = struct.unpack("<q", f.read(8))[0]
        self.addrTo = CAddress()
        self.addrTo.deserialize(f, with_time=False)

        self.addrFrom = CAddress()
        self.addrFrom.deserialize(f, with_time=False)
        self.nNonce = struct.unpack("<Q", f.read(8))[0]
        self.strSubVer = deser_string(f)

        self.nStartingHeight = struct.unpack("<i", f.read(4))[0]

        if self.nVersion >= 70001:
            # Relay field is optional for version 70001 onwards
            try:
                self.nRelay = struct.unpack("<b", f.read(1))[0]
            except:
                self.nRelay = 0
        else:
            self.nRelay = 0

    def serialize(self):
        r = b""
        r += struct.pack("<i", self.nVersion)
        r += struct.pack("<Q", self.nServices)
        r += struct.pack("<q", self.nTime)
        r += self.addrTo.serialize(with_time=False)
        r += self.addrFrom.serialize(with_time=False)
        r += struct.pack("<Q", self.nNonce)
        r += ser_string(self.strSubVer)
        r += struct.pack("<i", self.nStartingHeight)
        r += struct.pack("<b", self.nRelay)
        return r

    def __repr__(self):
        return 'msg_version(nVersion=%i nServices=%i nTime=%s addrTo=%s addrFrom=%s nNonce=0x%016X strSubVer=%s nStartingHeight=%i nRelay=%i)' \
            % (self.nVersion, self.nServices, time.ctime(self.nTime),
               repr(self.addrTo), repr(self.addrFrom), self.nNonce,
               self.strSubVer, self.nStartingHeight, self.nRelay)


class msg_verack:
    __slots__ = ()
    msgtype = b"verack"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_verack()"


class msg_addr:
    __slots__ = ("addrs",)
    msgtype = b"addr"

    def __init__(self):
        self.addrs = []

    def deserialize(self, f):
        self.addrs = deser_vector(f, CAddress)

    def serialize(self):
        return ser_vector(self.addrs)

    def __repr__(self):
        return "msg_addr(addrs=%s)" % (repr(self.addrs))


class msg_addrv2:
    __slots__ = ("addrs",)
    msgtype = b"addrv2"

    def __init__(self):
        self.addrs = []

    def deserialize(self, f):
        self.addrs = deser_vector(f, CAddress, "deserialize_v2")

    def serialize(self):
        return ser_vector(self.addrs, "serialize_v2")

    def __repr__(self):
        return "msg_addrv2(addrs=%s)" % (repr(self.addrs))


class msg_sendaddrv2:
    __slots__ = ()
    msgtype = b"sendaddrv2"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_sendaddrv2()"


class msg_inv:
    __slots__ = ("inv",)
    msgtype = b"inv"

    def __init__(self, inv=None):
        if inv is None:
            self.inv = []
        else:
            self.inv = inv

    def deserialize(self, f):
        self.inv = deser_vector(f, CInv)

    def serialize(self):
        return ser_vector(self.inv)

    def __repr__(self):
        return "msg_inv(inv=%s)" % (repr(self.inv))


class msg_getdata:
    __slots__ = ("inv",)
    msgtype = b"getdata"

    def __init__(self, inv=None):
        self.inv = inv if inv is not None else []

    def deserialize(self, f):
        self.inv = deser_vector(f, CInv)

    def serialize(self):
        return ser_vector(self.inv)

    def __repr__(self):
        return "msg_getdata(inv=%s)" % (repr(self.inv))


class msg_getblocks:
    __slots__ = ("locator", "hashstop")
    msgtype = b"getblocks"

    def __init__(self):
        self.locator = CBlockLocator()
        self.hashstop = 0

    def deserialize(self, f):
        self.locator = CBlockLocator()
        self.locator.deserialize(f)
        self.hashstop = deser_uint256(f)

    def serialize(self):
        r = b""
        r += self.locator.serialize()
        r += ser_uint256(self.hashstop)
        return r

    def __repr__(self):
        return "msg_getblocks(locator=%s hashstop=%064x)" \
            % (repr(self.locator), self.hashstop)


class msg_tx:
    __slots__ = ("tx",)
    msgtype = b"tx"

    def __init__(self, tx=CTransaction()):
        self.tx = tx

    def deserialize(self, f):
        self.tx.deserialize(f)

    def serialize(self):
        return self.tx.serialize_with_mweb()

    def __repr__(self):
        return "msg_tx(tx=%s)" % (repr(self.tx))

class msg_wtxidrelay:
    __slots__ = ()
    msgtype = b"wtxidrelay"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_wtxidrelay()"


class msg_no_witness_tx(msg_tx):
    __slots__ = ()

    def serialize(self):
        return self.tx.serialize_without_witness()

class msg_no_mweb_tx(msg_tx):
    __slots__ = ()

    def serialize(self):
        return self.tx.serialize_with_witness()

class msg_block:
    __slots__ = ("block",)
    msgtype = b"block"

    def __init__(self, block=None):
        if block is None:
            self.block = CBlock()
        else:
            self.block = block

    def deserialize(self, f):
        self.block.deserialize(f)

    def serialize(self):
        return self.block.serialize()

    def __repr__(self):
        return "msg_block(block=%s)" % (repr(self.block))


# for cases where a user needs tighter control over what is sent over the wire
# note that the user must supply the name of the msgtype, and the data
class msg_generic:
    __slots__ = ("msgtype", "data")

    def __init__(self, msgtype, data=None):
        self.msgtype = msgtype
        self.data = data

    def serialize(self):
        return self.data

    def __repr__(self):
        return "msg_generic()"


class msg_no_witness_block(msg_block):
    __slots__ = ()
    def serialize(self):
        return self.block.serialize(with_witness=False, with_mweb=False)
    
class msg_no_mweb_block(msg_block):
    __slots__ = ()
    def serialize(self):
        return self.block.serialize(with_witness=True, with_mweb=False)

class msg_getaddr:
    __slots__ = ()
    msgtype = b"getaddr"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_getaddr()"


class msg_ping:
    __slots__ = ("nonce",)
    msgtype = b"ping"

    def __init__(self, nonce=0):
        self.nonce = nonce

    def deserialize(self, f):
        self.nonce = struct.unpack("<Q", f.read(8))[0]

    def serialize(self):
        r = b""
        r += struct.pack("<Q", self.nonce)
        return r

    def __repr__(self):
        return "msg_ping(nonce=%08x)" % self.nonce


class msg_pong:
    __slots__ = ("nonce",)
    msgtype = b"pong"

    def __init__(self, nonce=0):
        self.nonce = nonce

    def deserialize(self, f):
        self.nonce = struct.unpack("<Q", f.read(8))[0]

    def serialize(self):
        r = b""
        r += struct.pack("<Q", self.nonce)
        return r

    def __repr__(self):
        return "msg_pong(nonce=%08x)" % self.nonce


class msg_mempool:
    __slots__ = ()
    msgtype = b"mempool"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_mempool()"


class msg_notfound:
    __slots__ = ("vec", )
    msgtype = b"notfound"

    def __init__(self, vec=None):
        self.vec = vec or []

    def deserialize(self, f):
        self.vec = deser_vector(f, CInv)

    def serialize(self):
        return ser_vector(self.vec)

    def __repr__(self):
        return "msg_notfound(vec=%s)" % (repr(self.vec))


class msg_sendheaders:
    __slots__ = ()
    msgtype = b"sendheaders"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_sendheaders()"


# getheaders message has
# number of entries
# vector of hashes
# hash_stop (hash of last desired block header, 0 to get as many as possible)
class msg_getheaders:
    __slots__ = ("hashstop", "locator",)
    msgtype = b"getheaders"

    def __init__(self):
        self.locator = CBlockLocator()
        self.hashstop = 0

    def deserialize(self, f):
        self.locator = CBlockLocator()
        self.locator.deserialize(f)
        self.hashstop = deser_uint256(f)

    def serialize(self):
        r = b""
        r += self.locator.serialize()
        r += ser_uint256(self.hashstop)
        return r

    def __repr__(self):
        return "msg_getheaders(locator=%s, stop=%064x)" \
            % (repr(self.locator), self.hashstop)


# headers message has
# <count> <vector of block headers>
class msg_headers:
    __slots__ = ("headers",)
    msgtype = b"headers"

    def __init__(self, headers=None):
        self.headers = headers if headers is not None else []

    def deserialize(self, f):
        # comment in bitcoind indicates these should be deserialized as blocks
        blocks = deser_vector(f, CBlock)
        for x in blocks:
            self.headers.append(CBlockHeader(x))

    def serialize(self):
        blocks = [CBlock(x) for x in self.headers]
        return ser_vector(blocks)

    def __repr__(self):
        return "msg_headers(headers=%s)" % repr(self.headers)


class msg_merkleblock:
    __slots__ = ("merkleblock",)
    msgtype = b"merkleblock"

    def __init__(self, merkleblock=None):
        if merkleblock is None:
            self.merkleblock = CMerkleBlock()
        else:
            self.merkleblock = merkleblock

    def deserialize(self, f):
        self.merkleblock.deserialize(f)

    def serialize(self):
        return self.merkleblock.serialize()

    def __repr__(self):
        return "msg_merkleblock(merkleblock=%s)" % (repr(self.merkleblock))


class msg_filterload:
    __slots__ = ("data", "nHashFuncs", "nTweak", "nFlags")
    msgtype = b"filterload"

    def __init__(self, data=b'00', nHashFuncs=0, nTweak=0, nFlags=0):
        self.data = data
        self.nHashFuncs = nHashFuncs
        self.nTweak = nTweak
        self.nFlags = nFlags

    def deserialize(self, f):
        self.data = deser_string(f)
        self.nHashFuncs = struct.unpack("<I", f.read(4))[0]
        self.nTweak = struct.unpack("<I", f.read(4))[0]
        self.nFlags = struct.unpack("<B", f.read(1))[0]

    def serialize(self):
        r = b""
        r += ser_string(self.data)
        r += struct.pack("<I", self.nHashFuncs)
        r += struct.pack("<I", self.nTweak)
        r += struct.pack("<B", self.nFlags)
        return r

    def __repr__(self):
        return "msg_filterload(data={}, nHashFuncs={}, nTweak={}, nFlags={})".format(
            self.data, self.nHashFuncs, self.nTweak, self.nFlags)


class msg_filteradd:
    __slots__ = ("data")
    msgtype = b"filteradd"

    def __init__(self, data):
        self.data = data

    def deserialize(self, f):
        self.data = deser_string(f)

    def serialize(self):
        r = b""
        r += ser_string(self.data)
        return r

    def __repr__(self):
        return "msg_filteradd(data={})".format(self.data)


class msg_filterclear:
    __slots__ = ()
    msgtype = b"filterclear"

    def __init__(self):
        pass

    def deserialize(self, f):
        pass

    def serialize(self):
        return b""

    def __repr__(self):
        return "msg_filterclear()"


class msg_feefilter:
    __slots__ = ("feerate",)
    msgtype = b"feefilter"

    def __init__(self, feerate=0):
        self.feerate = feerate

    def deserialize(self, f):
        self.feerate = struct.unpack("<Q", f.read(8))[0]

    def serialize(self):
        r = b""
        r += struct.pack("<Q", self.feerate)
        return r

    def __repr__(self):
        return "msg_feefilter(feerate=%08x)" % self.feerate


class msg_sendcmpct:
    __slots__ = ("announce", "version")
    msgtype = b"sendcmpct"

    def __init__(self, announce=False, version=1):
        self.announce = announce
        self.version = version

    def deserialize(self, f):
        self.announce = struct.unpack("<?", f.read(1))[0]
        self.version = struct.unpack("<Q", f.read(8))[0]

    def serialize(self):
        r = b""
        r += struct.pack("<?", self.announce)
        r += struct.pack("<Q", self.version)
        return r

    def __repr__(self):
        return "msg_sendcmpct(announce=%s, version=%lu)" % (self.announce, self.version)


class msg_cmpctblock:
    __slots__ = ("header_and_shortids", "version")
    msgtype = b"cmpctblock"

    def __init__(self, header_and_shortids=None, version=1):
        self.header_and_shortids = header_and_shortids
        self.version = version

    def deserialize(self, f):
        self.header_and_shortids = P2PHeaderAndShortIDs()
        self.header_and_shortids.deserialize(f)

    def serialize(self):
        r = b""
        r += self.header_and_shortids.serialize(version=self.version)
        return r

    def __repr__(self):
        return "msg_cmpctblock(HeaderAndShortIDs=%s)" % repr(self.header_and_shortids)


class msg_getblocktxn:
    __slots__ = ("block_txn_request",)
    msgtype = b"getblocktxn"

    def __init__(self):
        self.block_txn_request = None

    def deserialize(self, f):
        self.block_txn_request = BlockTransactionsRequest()
        self.block_txn_request.deserialize(f)

    def serialize(self):
        r = b""
        r += self.block_txn_request.serialize()
        return r

    def __repr__(self):
        return "msg_getblocktxn(block_txn_request=%s)" % (repr(self.block_txn_request))


class msg_blocktxn:
    __slots__ = ("block_transactions",)
    msgtype = b"blocktxn"

    def __init__(self):
        self.block_transactions = BlockTransactions()

    def deserialize(self, f):
        self.block_transactions.deserialize(f)

    def serialize(self):
        r = b""
        r += self.block_transactions.serialize()
        return r

    def __repr__(self):
        return "msg_blocktxn(block_transactions=%s)" % (repr(self.block_transactions))


class msg_no_witness_blocktxn(msg_blocktxn):
    __slots__ = ()

    def serialize(self):
        return self.block_transactions.serialize(with_witness=False, with_mweb=False)


class msg_getcfilters:
    __slots__ = ("filter_type", "start_height", "stop_hash")
    msgtype =  b"getcfilters"

    def __init__(self, filter_type, start_height, stop_hash):
        self.filter_type = filter_type
        self.start_height = start_height
        self.stop_hash = stop_hash

    def deserialize(self, f):
        self.filter_type = struct.unpack("<B", f.read(1))[0]
        self.start_height = struct.unpack("<I", f.read(4))[0]
        self.stop_hash = deser_uint256(f)

    def serialize(self):
        r = b""
        r += struct.pack("<B", self.filter_type)
        r += struct.pack("<I", self.start_height)
        r += ser_uint256(self.stop_hash)
        return r

    def __repr__(self):
        return "msg_getcfilters(filter_type={:#x}, start_height={}, stop_hash={:x})".format(
            self.filter_type, self.start_height, self.stop_hash)

class msg_cfilter:
    __slots__ = ("filter_type", "block_hash", "filter_data")
    msgtype =  b"cfilter"

    def __init__(self, filter_type=None, block_hash=None, filter_data=None):
        self.filter_type = filter_type
        self.block_hash = block_hash
        self.filter_data = filter_data

    def deserialize(self, f):
        self.filter_type = struct.unpack("<B", f.read(1))[0]
        self.block_hash = deser_uint256(f)
        self.filter_data = deser_string(f)

    def serialize(self):
        r = b""
        r += struct.pack("<B", self.filter_type)
        r += ser_uint256(self.block_hash)
        r += ser_string(self.filter_data)
        return r

    def __repr__(self):
        return "msg_cfilter(filter_type={:#x}, block_hash={:x})".format(
            self.filter_type, self.block_hash)

class msg_getcfheaders:
    __slots__ = ("filter_type", "start_height", "stop_hash")
    msgtype =  b"getcfheaders"

    def __init__(self, filter_type, start_height, stop_hash):
        self.filter_type = filter_type
        self.start_height = start_height
        self.stop_hash = stop_hash

    def deserialize(self, f):
        self.filter_type = struct.unpack("<B", f.read(1))[0]
        self.start_height = struct.unpack("<I", f.read(4))[0]
        self.stop_hash = deser_uint256(f)

    def serialize(self):
        r = b""
        r += struct.pack("<B", self.filter_type)
        r += struct.pack("<I", self.start_height)
        r += ser_uint256(self.stop_hash)
        return r

    def __repr__(self):
        return "msg_getcfheaders(filter_type={:#x}, start_height={}, stop_hash={:x})".format(
            self.filter_type, self.start_height, self.stop_hash)

class msg_cfheaders:
    __slots__ = ("filter_type", "stop_hash", "prev_header", "hashes")
    msgtype =  b"cfheaders"

    def __init__(self, filter_type=None, stop_hash=None, prev_header=None, hashes=None):
        self.filter_type = filter_type
        self.stop_hash = stop_hash
        self.prev_header = prev_header
        self.hashes = hashes

    def deserialize(self, f):
        self.filter_type = struct.unpack("<B", f.read(1))[0]
        self.stop_hash = deser_uint256(f)
        self.prev_header = deser_uint256(f)
        self.hashes = deser_uint256_vector(f)

    def serialize(self):
        r = b""
        r += struct.pack("<B", self.filter_type)
        r += ser_uint256(self.stop_hash)
        r += ser_uint256(self.prev_header)
        r += ser_uint256_vector(self.hashes)
        return r

    def __repr__(self):
        return "msg_cfheaders(filter_type={:#x}, stop_hash={:x})".format(
            self.filter_type, self.stop_hash)

class msg_getcfcheckpt:
    __slots__ = ("filter_type", "stop_hash")
    msgtype =  b"getcfcheckpt"

    def __init__(self, filter_type, stop_hash):
        self.filter_type = filter_type
        self.stop_hash = stop_hash

    def deserialize(self, f):
        self.filter_type = struct.unpack("<B", f.read(1))[0]
        self.stop_hash = deser_uint256(f)

    def serialize(self):
        r = b""
        r += struct.pack("<B", self.filter_type)
        r += ser_uint256(self.stop_hash)
        return r

    def __repr__(self):
        return "msg_getcfcheckpt(filter_type={:#x}, stop_hash={:x})".format(
            self.filter_type, self.stop_hash)

class msg_cfcheckpt:
    __slots__ = ("filter_type", "stop_hash", "headers")
    msgtype =  b"cfcheckpt"

    def __init__(self, filter_type=None, stop_hash=None, headers=None):
        self.filter_type = filter_type
        self.stop_hash = stop_hash
        self.headers = headers

    def deserialize(self, f):
        self.filter_type = struct.unpack("<B", f.read(1))[0]
        self.stop_hash = deser_uint256(f)
        self.headers = deser_uint256_vector(f)

    def serialize(self):
        r = b""
        r += struct.pack("<B", self.filter_type)
        r += ser_uint256(self.stop_hash)
        r += ser_uint256_vector(self.headers)
        return r

    def __repr__(self):
        return "msg_cfcheckpt(filter_type={:#x}, stop_hash={:x})".format(
            self.filter_type, self.stop_hash)



"""------------MWEB------------"""

import blake3 as BLAKE3

def hex_reverse(h):
    return "".join(reversed([h[i:i+2] for i in range(0, len(h), 2)]))

class Hash:
    __slots__ = ("val")

    def __init__(self, val = 0):
        self.val = val

    @classmethod
    def from_hex(cls, hex_str):
        return cls(int(hex_str, 16))

    @classmethod
    def from_rev_hex(cls, hex_str):
        return cls(int(hex_reverse(hex_str), 16))

    @classmethod
    def from_byte_arr(cls, b):
        return cls(deser_uint256(BytesIO(b)))

    def to_hex(self):
        return self.serialize().hex()

    def to_byte_arr(self):
        return self.serialize()

    @classmethod
    def deserialize(cls, f):
        return cls(deser_uint256(f))

    def serialize(self):
        return ser_uint256(self.val)

    def __hash__(self):
        return hash(self.val)

    def __str__(self):
        return repr(self)

    def __repr__(self):
        return "Hash(val=0x{:x})".format(self.val)

    def __eq__(self, other):
        return isinstance(other, Hash) and self.val == other.val


def blake3(s):
    return Hash.from_rev_hex(BLAKE3.blake3(s).hexdigest())

def ser_varint(n):
    r = b""

    l=0;
    while True:
        t = (n & 0x7F) | (0x80, 0x00)[l == 0]
        r = struct.pack("B", t) + r
        if n <= 0x7F:
            break
        n = (n >> 7) - 1;
        l = l + 1

    return r

def deser_varint(f):
    n = 0;
    while True: 
        chData = struct.unpack("B", f.read(1))[0]
        n = (n << 7) | (chData & 0x7F)
        if chData & 0x80:
            n = n + 1
        else:
            break

    return n

def ser_mweb_block(b):
    if b == None:
        return struct.pack("B", 0)
    else:
        return struct.pack("B", 1) + b.serialize()

def deser_mweb_block(f):
    has_mweb = struct.unpack("B", f.read(1))[0]
    if has_mweb == 1:
        mweb_block = MWEBBlock()
        mweb_block.deserialize(f)
        return mweb_block
    else:
        return None

def ser_mweb_tx(t):
    if t == None:
        return struct.pack("B", 0)
    else:
        return struct.pack("B", 1) + t.serialize()

def deser_mweb_tx(f):
    has_mweb = struct.unpack("B", f.read(1))[0]
    if has_mweb == 1:
        mweb_tx = MWEBTransaction()
        return mweb_tx.deserialize(f)
    else:
        return None


class MWEBInput:
    __slots__ = ("features", "output_id", "commitment", "input_pubkey",
                "output_pubkey", "extradata", "signature", "hash")

    def __init__(self):
        self.features = 0
        self.output_id = Hash()
        self.commitment = None
        self.input_pubkey = None
        self.output_pubkey = None
        self.extradata = None
        self.signature = None
        self.hash = None

    def deserialize(self, f):
        self.features = struct.unpack("B", f.read(1))[0]
        self.output_id = Hash.deserialize(f)
        self.commitment = deser_pubkey(f)
        self.output_pubkey = deser_pubkey(f)
        if self.features & 1:
            self.input_pubkey = deser_pubkey(f)
        self.extradata = None
        if self.features & 2:
            self.extradata = deser_fixed_bytes(f, deser_compact_size(f))
        self.signature = deser_signature(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += struct.pack("B", self.features)
        r += self.output_id.serialize()
        r += ser_pubkey(self.commitment)
        r += ser_pubkey(self.output_pubkey)
        if self.features & 1:
            r += ser_pubkey(self.input_pubkey)
        if self.features & 2:
            r += ser_compact_size(len(self.extradata))
            r += ser_fixed_bytes(self.extradata, len(self.extradata))
        r += ser_signature(self.signature)
        return r

    def rehash(self):
        self.hash = blake3(self.serialize())
        return self.hash.to_hex()

class MWEBOutputMessage:
    __slots__ = ("features", "key_exchange_pubkey", "view_tag", "masked_value",
                "masked_nonce", "extradata", "hash")

    def __init__(self):
        self.features = 0
        self.key_exchange_pubkey = None
        self.view_tag = 0
        self.masked_value = None
        self.masked_nonce = None
        self.extradata = None
        self.hash = None

    def deserialize(self, f):
        self.features = struct.unpack("B", f.read(1))[0]
        if self.features & 1:
            self.key_exchange_pubkey = deser_pubkey(f)
            self.view_tag = struct.unpack("B", f.read(1))[0]
            self.masked_value = struct.unpack("<q", f.read(8))[0]
            self.masked_nonce = deser_fixed_bytes(f, 16)
        self.extradata = None
        if self.features & 2:
            self.extradata = deser_fixed_bytes(f, deser_compact_size(f))
        self.rehash()

    def serialize(self):
        r = b""
        r += struct.pack("B", self.features)
        if self.features & 1:
            r += ser_pubkey(self.key_exchange_pubkey)
            r += struct.pack("B", self.view_tag)
            r += struct.pack("<q", self.masked_value)
            r += ser_fixed_bytes(self.masked_nonce, 16)
        if self.features & 2:
            r += ser_compact_size(len(self.extradata))
            r += ser_fixed_bytes(self.extradata, len(self.extradata))
        return r

    def rehash(self):
        self.hash = blake3(self.serialize())
        return self.hash.to_hex()

class MWEBOutput:
    __slots__ = ("commitment", "sender_pubkey", "receiver_pubkey", "message",
                "proof", "signature", "hash")

    def __init__(self):
        self.commitment = None
        self.sender_pubkey = None
        self.receiver_pubkey = None
        self.message = MWEBOutputMessage()
        self.proof = None
        self.signature = None
        self.hash = None

    def deserialize(self, f):
        self.commitment = deser_pubkey(f)
        self.sender_pubkey = deser_pubkey(f)
        self.receiver_pubkey = deser_pubkey(f)
        self.message.deserialize(f)
        self.proof = deser_fixed_bytes(f, 675)
        self.signature = deser_signature(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += ser_pubkey(self.commitment)
        r += ser_pubkey(self.sender_pubkey)
        r += ser_pubkey(self.receiver_pubkey)
        r += self.message.serialize()
        r += ser_fixed_bytes(self.proof, 675)
        r += ser_signature(self.signature)
        return r

    def rehash(self):
        self.hash = blake3(self.serialize())
        return self.hash.to_hex()

class MWEBCompactOutput:
    __slots__ = ("commitment", "sender_pubkey", "receiver_pubkey", "message",
                "proof_hash", "signature", "hash")

    def __init__(self):
        self.commitment = None
        self.sender_pubkey = None
        self.receiver_pubkey = None
        self.message = MWEBOutputMessage()
        self.proof_hash = None
        self.signature = None
        self.hash = None

    def deserialize(self, f):
        self.commitment = deser_pubkey(f)
        self.sender_pubkey = deser_pubkey(f)
        self.receiver_pubkey = deser_pubkey(f)
        self.message.deserialize(f)
        self.proof_hash = Hash.deserialize(f)
        self.signature = deser_signature(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += ser_pubkey(self.commitment)
        r += ser_pubkey(self.sender_pubkey)
        r += ser_pubkey(self.receiver_pubkey)
        r += self.message.serialize()
        r += self.proof_hash.serialize()
        r += ser_signature(self.signature)
        return r

    def __repr__(self):
        return "MWEBCompactOutput(commitment=%s)" % (repr(self.commitment))

class MWEBKernel:
    __slots__ = ("features", "fee", "pegin", "pegouts", "lock_height",
                "stealth_excess", "extradata", "excess", "signature", "hash")

    def __init__(self):
        self.features = 0
        self.fee = None
        self.pegin = None
        self.pegouts = None
        self.lock_height = None
        self.stealth_excess = None
        self.extradata = None
        self.excess = None
        self.signature = None
        self.hash = None

    def deserialize(self, f):
        self.features = struct.unpack("B", f.read(1))[0]
        self.fee = None
        if self.features & 1:
            self.fee = deser_varint(f)
        self.pegin = None
        if self.features & 2:
            self.pegin = deser_varint(f)
        self.pegouts = None
        if self.features & 4:
            self.pegouts =  []# TODO: deser_vector(f, CPegout)
        self.lock_height = None
        if self.features & 8:
            self.lock_height = deser_varint(f)
        self.stealth_excess = None
        if self.features & 16:
            self.stealth_excess = Hash.deserialize(f)
        self.extradata = None
        if self.features & 32:
            self.extradata = deser_fixed_bytes(f, deser_compact_size(f))
        self.excess = deser_pubkey(f)
        self.signature = deser_signature(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += struct.pack("B", self.features)
        if self.features & 1:
            r += ser_varint(self.fee)
        if self.features & 2:
            r += ser_varint(self.pegin)
        if self.features & 4:
            r += ser_vector(self.pegouts)
        if self.features & 8:
            r += ser_varint(self.lock_height)
        if self.features & 16:
            r += self.stealth_excess.serialize()
        if self.features & 32:
            r += ser_compact_size(len(self.extradata))
            r += ser_fixed_bytes(self.extradata, len(self.extradata))
        r += ser_pubkey(self.excess)
        r += ser_signature(self.signature)
        return r

    def rehash(self):
        self.hash = blake3(self.serialize())
        return self.hash.to_hex()

    def __repr__(self):
        return "MWEBKernel(features=%d, excess=%s)" % (self.features, repr(self.excess))

class MWEBTxBody:
    __slots__ = ("inputs", "outputs", "kernels")

    def __init__(self):
        self.inputs = []
        self.outputs = []
        self.kernels = []

    def deserialize(self, f):
        self.inputs = deser_vector(f, MWEBInput)
        self.outputs = deser_vector(f, MWEBOutput)
        self.kernels = deser_vector(f, MWEBKernel)

    def serialize(self):
        r = b""
        r += ser_vector(self.inputs)
        r += ser_vector(self.outputs)
        r += ser_vector(self.kernels)
        return r

    def __repr__(self):
        return "MWEBTxBody(inputs=%s, outputs=%s, kernels=%s)" % (repr(self.inputs), repr(self.outputs), repr(self.kernels))


class MWEBTransaction:
    __slots__ = ("kernel_offset", "stealth_offset", "body", "hash")

    def __init__(self):
        self.kernel_offset = Hash()
        self.stealth_offset = Hash()
        self.body = MWEBTxBody()
        self.hash = None

    def deserialize(self, f):
        self.kernel_offset = Hash.deserialize(f)
        self.stealth_offset = Hash.deserialize(f)
        self.body.deserialize(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += self.kernel_offset.serialize()
        r += self.stealth_offset.serialize()
        r += self.body.serialize()
        return r

    def rehash(self):
        self.hash = blake3(self.serialize())
        return self.hash.to_hex()

    def __repr__(self):
        return "MWEBTransaction(kernel_offset=%s, stealth_offset=%s, body=%s, hash=%s)" % (repr(self.kernel_offset), repr(self.stealth_offset), repr(self.body), repr(self.hash))

class MWEBHeader:
    __slots__ = ("height", "output_root", "kernel_root", "leafset_root",
                "kernel_offset", "stealth_offset", "num_txos", "num_kernels", "hash")

    def __init__(self):
        self.height = 0
        self.output_root = Hash()
        self.kernel_root = Hash()
        self.leafset_root = Hash()
        self.kernel_offset = Hash()
        self.stealth_offset = Hash()
        self.num_txos = 0
        self.num_kernels = 0
        self.hash = None

    def from_json(self, mweb_json):
        self.height = mweb_json['height']
        self.output_root = Hash.from_rev_hex(mweb_json['output_root'])
        self.kernel_root = Hash.from_rev_hex(mweb_json['kernel_root'])
        self.leafset_root = Hash.from_rev_hex(mweb_json['leaf_root'])
        self.kernel_offset = Hash.from_rev_hex(mweb_json['kernel_offset'])
        self.stealth_offset = Hash.from_rev_hex(mweb_json['stealth_offset'])
        self.num_txos = mweb_json['num_txos']
        self.num_kernels = mweb_json['num_kernels']
        self.rehash()

    def deserialize(self, f):
        self.height = deser_varint(f)
        self.output_root = Hash.deserialize(f)
        self.kernel_root = Hash.deserialize(f)
        self.leafset_root = Hash.deserialize(f)
        self.kernel_offset = Hash.deserialize(f)
        self.stealth_offset = Hash.deserialize(f)
        self.num_txos = deser_varint(f)
        self.num_kernels = deser_varint(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += ser_varint(self.height)
        r += self.output_root.serialize()
        r += self.kernel_root.serialize()
        r += self.leafset_root.serialize()
        r += self.kernel_offset.serialize()
        r += self.stealth_offset.serialize()
        r += ser_varint(self.num_txos)
        r += ser_varint(self.num_kernels)
        return r
    
    def rehash(self):
        self.hash = blake3(self.serialize())
        return self.hash.to_hex()

    def __repr__(self):
        return ("MWEBHeader(height=%d, output_root=%s, kernel_root=%s, leafset_root=%s, kernel_offset=%s, stealth_offset=%s, num_txos=%d, num_kernels=%d, hash=%s)" %
            (self.height, repr(self.output_root), repr(self.kernel_root), repr(self.leafset_root), repr(self.kernel_offset), repr(self.stealth_offset), self.num_txos, self.num_kernels, repr(self.hash)))

    def __eq__(self, other):
        return isinstance(other, MWEBHeader) and self.hash == other.hash

class MWEBBlock:
    __slots__ = ("header", "body")

    def __init__(self, header = MWEBHeader()):
        self.header = copy.deepcopy(header)
        self.body = MWEBTxBody()

    def deserialize(self, f):
        self.header.deserialize(f)
        self.body.deserialize(f)
        self.rehash()

    def serialize(self):
        r = b""
        r += self.header.serialize()
        r += self.body.serialize()
        return r
    
    def rehash(self):
        return self.header.rehash()

    def __repr__(self):
        return "MWEBBlock(header=%s, body=%s)" % (repr(self.header), repr(self.body))


class CMerkleBlockWithMWEB:
    __slots__ = ("merkle", "hogex", "mweb_header")

    def __init__(self):
        self.merkle = CMerkleBlock()
        self.hogex = CTransaction()
        self.mweb_header = MWEBHeader()

    def deserialize(self, f):
        self.merkle.deserialize(f)
        self.hogex.deserialize(f)
        self.mweb_header.deserialize(f)

    def serialize(self):
        r = b""
        r += self.merkle.serialize()
        r += self.hogex.serialize_with_mweb()
        r += self.mweb_header.serialize()
        return r

    def __repr__(self):
        return "CMerkleBlockWithMWEB(merkle=%s, hogex=%s, mweb_header=%s)" % (repr(self.merkle), repr(self.hogex), repr(self.mweb_header))


class msg_mwebheader:
    __slots__ = ("merkleblockwithmweb",)
    msgtype = b"mwebheader"

    def __init__(self, merkleblockwithmweb=CMerkleBlockWithMWEB()):
        self.merkleblockwithmweb = merkleblockwithmweb

    def deserialize(self, f):
        self.merkleblockwithmweb.deserialize(f)

    def serialize(self):
        return self.merkleblockwithmweb.serialize()

    def header_hash(self):
        self.merkleblockwithmweb.merkle.header.rehash()
        return self.merkleblockwithmweb.merkle.header.hash

    def __repr__(self):
        return "msg_mwebheader(merkleblockwithmweb=%s)" % (repr(self.merkleblockwithmweb))

class msg_mwebleafset:
    __slots__ = ("block_hash", "leafset")
    msgtype = b"mwebleafset"

    def __init__(self, block_hash=None, leafset=None):
        self.block_hash = block_hash
        self.leafset = leafset

    def deserialize(self, f):
        self.block_hash = Hash.deserialize(f)
        self.leafset = deser_fixed_bytes(f, deser_compact_size(f))

    def serialize(self):
        r = b""
        r += self.block_hash.serialize()
        r += ser_compact_size(len(self.leafset))
        r += ser_fixed_bytes(self.leafset, len(self.leafset))
        return r

    def __repr__(self):
        leafset_hex = ser_fixed_bytes(self.leafset, len(self.leafset)).hex() #encode(self.leafset, 'hex_codec').decode('ascii')
        return "msg_mwebleafset(block_hash=%s, leafset=%s%s)" % (repr(self.block_hash), repr(leafset_hex)[:50], "..." if len(leafset_hex) > 50 else "")

class msg_getmwebutxos:
    __slots__ = ("block_hash", "start_index", "num_requested", "output_format")
    msgtype = b"getmwebutxos"

    def __init__(self, block_hash=None, start_index=0, num_requested=0, output_format=0):
        self.block_hash = block_hash
        self.start_index = start_index
        self.num_requested = num_requested
        self.output_format = output_format

    def deserialize(self, f):
        self.block_hash = Hash.deserialize(f)
        self.start_index = deser_varint(f)
        self.num_requested = (struct.unpack("<B", f.read(1))[0] << 8) + struct.unpack("<B", f.read(1))[0]
        self.output_format = struct.unpack("B", f.read(1))[0]

    def serialize(self):
        r = b""
        r += self.block_hash.serialize()
        r += ser_varint(self.start_index)
        r += struct.pack("B", self.num_requested >> 8) + struct.pack("B", self.num_requested & 0xFF)
        r += struct.pack("B", self.output_format)
        return r

    def __repr__(self):
        return ("msg_getmwebutxos(block_hash=%s, start_index=%d, num_requested=%d, output_format=%d)" %
            (repr(self.block_hash), self.start_index, self.num_requested, self.output_format))


class msg_mwebutxos:
    __slots__ = ("block_hash", "start_index", "output_format", "utxos", "proof_hashes")
    msgtype = b"mwebutxos"

    def __init__(self, block_hash=None, start_index=0, output_format=0, utxos=None, proof_hashes=None):
        self.block_hash = block_hash
        self.start_index = start_index
        self.output_format = output_format
        self.utxos = utxos
        self.proof_hashes = proof_hashes

    def deserialize(self, f):
        self.block_hash = Hash.deserialize(f)
        self.start_index = deser_varint(f)
        self.output_format = struct.unpack("B", f.read(1))[0]

        if self.output_format == 0:
            self.utxos = deser_vector(f, Hash)
        elif self.output_format == 1:
            self.utxos = deser_vector(f, MWEBOutput)
        else:
            self.utxos = deser_vector(f, MWEBCompactOutput)

        self.proof_hashes = deser_vector(f, Hash)

    def serialize(self):
        r = b""
        r += self.block_hash.serialize()
        r += ser_varint(self.start_index)
        r += struct.pack("B", self.output_format)
        r += ser_vector(self.utxos)
        r += ser_vector(self.proof_hashes)
        return r

    def __repr__(self):
        return ("msg_mwebutxos(block_hash=%s, start_index=%d, output_format=%d, utxos=%s, proof_hashes=%s)" %
            (repr(self.block_hash), self.start_index, self.output_format, repr(self.utxos), repr(self.proof_hashes)))
