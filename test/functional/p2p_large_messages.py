#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the length limits for incoming P2P messages.

A message may be up to 4 MB (MAX_PROTOCOL_MESSAGE_LENGTH). Once the version
handshake is complete, block and compact block messages, which carry a whole
MWEB block, may be up to 32 MB (MAX_BLOCK_MESSAGE_LENGTH). Check that:

- before the handshake, a message over 4 MB disconnects the peer, also when
  it is a block or compact block message;
- after the handshake, any other message over 4 MB disconnects the peer, and
  a block or compact block message over 32 MB does too, while one of 32 MB is
  received and processed;
- the messages over 4 MB that inbound peers are still sending count against
  a total of 128 MB: a peer whose message would take the total over it is
  disconnected; a completed message and the message of a peer that has
  disconnected stop counting; messages up to 4 MB and messages from outbound
  peers are not counted.
"""

import struct

from test_framework.messages import (
    MAX_BLOCK_MESSAGE_LENGTH,
    MAX_PROTOCOL_MESSAGE_LENGTH,
)
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

HEADER_SIZE = 24
MAX_INBOUND_LARGE_MESSAGES_TOTAL = 128 * 1000 * 1000


class msg_raw:
    """A message of any type with the given payload."""

    def __init__(self, msgtype, payload):
        self.msgtype = msgtype
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_raw(msgtype={}, {} bytes)".format(self.msgtype, len(self.payload))


class NoVerackPeer(P2PInterface):
    """Sends its version message, but never the verack."""

    def on_version(self, message):
        pass


class QuietPeer(P2PInterface):
    """Sends nothing of its own accord, so that a partly sent message stays intact."""

    def on_ping(self, message):
        pass


def junk_message(conn, msgtype, length):
    """A complete message whose payload does not deserialize as any message."""
    return conn.build_message(msg_raw(msgtype, b'\xff' * length))


def header_only(conn, msgtype, length):
    """The header of a message of the given length, without its payload."""
    return conn.magic_bytes + msgtype.ljust(12, b'\x00') + struct.pack('<I', length) + b'\x00' * 4


class LargeMessagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        self.test_before_handshake()
        self.test_after_handshake()
        self.test_inbound_total()

    def newest_peer_id(self):
        return max(peer['id'] for peer in self.nodes[0].getpeerinfo())

    def peer_bytesrecv(self, peer_id):
        for peer in self.nodes[0].getpeerinfo():
            if peer['id'] == peer_id:
                return peer['bytesrecv']
        return None

    def send_accepted(self, conn, peer_id, data):
        """Send data and wait until the node has read it, without a header error for it."""
        before = self.peer_bytesrecv(peer_id)

        def read_by_node():
            bytesrecv = self.peer_bytesrecv(peer_id)
            assert bytesrecv is not None, "peer={} disconnected".format(peer_id)
            return bytesrecv >= before + len(data)

        with self.nodes[0].assert_debug_log(expected_msgs=[], unexpected_msgs=['HEADER ERROR']):
            conn.send_raw_message(data)
            self.wait_until(read_by_node)
        assert conn.is_connected

    def send_refused(self, conn, data, log):
        """Send data and check that the node logs the error and disconnects the peer."""
        with self.nodes[0].assert_debug_log([log]):
            conn.send_raw_message(data)
            conn.wait_for_disconnect()

    def test_before_handshake(self):
        node = self.nodes[0]
        self.log.info("Before the version handshake, a block message over 4 MB disconnects the peer")
        conn = node.add_p2p_connection(P2PInterface(), send_version=False, wait_for_verack=False)
        self.send_refused(conn, header_only(conn, b'block', MAX_PROTOCOL_MESSAGE_LENGTH + 1),
                          'HEADER ERROR - SIZE (block, {} bytes)'.format(MAX_PROTOCOL_MESSAGE_LENGTH + 1))

        self.log.info("So does a compact block message over 4 MB from a peer that has not sent its verack")
        conn = node.add_p2p_connection(NoVerackPeer(), wait_for_verack=False)
        conn.wait_for_verack()
        self.send_refused(conn, header_only(conn, b'cmpctblock', MAX_PROTOCOL_MESSAGE_LENGTH + 1),
                          'HEADER ERROR - SIZE (cmpctblock, {} bytes)'.format(MAX_PROTOCOL_MESSAGE_LENGTH + 1))
        node.disconnect_p2ps()

    def test_after_handshake(self):
        node = self.nodes[0]
        self.log.info("After the handshake, a message over 4 MB of another type disconnects the peer")
        for msgtype in [b'tx', b'blocktxn', b'headers', b'mwebheader', b'mwebleafset', b'mwebutxos']:
            conn = node.add_p2p_connection(P2PInterface())
            self.send_refused(conn, header_only(conn, msgtype, MAX_PROTOCOL_MESSAGE_LENGTH + 1),
                              'HEADER ERROR - SIZE ({}, {} bytes)'.format(msgtype.decode(), MAX_PROTOCOL_MESSAGE_LENGTH + 1))

        for msgtype in [b'block', b'cmpctblock']:
            self.log.info("A {} message over 32 MB disconnects the peer".format(msgtype.decode()))
            conn = node.add_p2p_connection(P2PInterface())
            self.send_refused(conn, header_only(conn, msgtype, MAX_BLOCK_MESSAGE_LENGTH + 1),
                              'HEADER ERROR - SIZE ({}, {} bytes)'.format(msgtype.decode(), MAX_BLOCK_MESSAGE_LENGTH + 1))

            self.log.info("A {} message of 32 MB is received and processed".format(msgtype.decode()))
            conn = node.add_p2p_connection(P2PInterface())
            with node.assert_debug_log(['ProcessMessages({}, {} bytes): Exception'.format(msgtype.decode(), MAX_BLOCK_MESSAGE_LENGTH)]):
                conn.send_raw_message(junk_message(conn, msgtype, MAX_BLOCK_MESSAGE_LENGTH))
                conn.sync_with_ping()
            assert_equal(node.getpeerinfo()[-1]['bytesrecv_per_msg'][msgtype.decode()], HEADER_SIZE + MAX_BLOCK_MESSAGE_LENGTH)
        node.disconnect_p2ps()

    def test_inbound_total(self):
        node = self.nodes[0]
        refused_log = 'HEADER ERROR - LARGE MESSAGES TOTAL (block, {} bytes)'.format(MAX_PROTOCOL_MESSAGE_LENGTH + 1)
        over_4mb = MAX_PROTOCOL_MESSAGE_LENGTH + 1

        self.log.info("Four inbound peers start messages of 32 MB, which reach the total of 128 MB")
        assert_equal(4 * MAX_BLOCK_MESSAGE_LENGTH, MAX_INBOUND_LARGE_MESSAGES_TOTAL)
        peers = []
        for msgtype in [b'cmpctblock', b'block', b'block', b'block']:
            conn = node.add_p2p_connection(QuietPeer())
            peer_id = self.newest_peer_id()
            message = junk_message(conn, msgtype, MAX_BLOCK_MESSAGE_LENGTH) if not peers else None
            self.send_accepted(conn, peer_id, header_only(conn, msgtype, MAX_BLOCK_MESSAGE_LENGTH) if message is None else message[:HEADER_SIZE])
            peers.append((conn, peer_id, message))

        self.log.info("A fifth inbound peer starting a message over 4 MB is disconnected")
        conn = node.add_p2p_connection(QuietPeer())
        self.send_refused(conn, header_only(conn, b'block', over_4mb), refused_log)

        self.log.info("Messages up to 4 MB are not counted")
        conn = node.add_p2p_connection(P2PInterface())
        with node.assert_debug_log(['ProcessMessages(block, {} bytes): Exception'.format(MAX_PROTOCOL_MESSAGE_LENGTH)]):
            conn.send_raw_message(junk_message(conn, b'block', MAX_PROTOCOL_MESSAGE_LENGTH))
            conn.sync_with_ping()

        self.log.info("Messages from outbound peers are not counted")
        conn = node.add_outbound_p2p_connection(P2PInterface(), p2p_idx=0)
        with node.assert_debug_log(['ProcessMessages(block, {} bytes): Exception'.format(over_4mb)], unexpected_msgs=['HEADER ERROR']):
            conn.send_raw_message(junk_message(conn, b'block', over_4mb))
            conn.sync_with_ping()

        self.log.info("A completed message stops counting")
        conn, peer_id, message = peers.pop(0)
        with node.assert_debug_log(['ProcessMessages(cmpctblock, {} bytes): Exception'.format(MAX_BLOCK_MESSAGE_LENGTH)]):
            conn.send_raw_message(message[HEADER_SIZE:])
            conn.sync_with_ping()
        conn = node.add_p2p_connection(QuietPeer())
        self.send_accepted(conn, self.newest_peer_id(), header_only(conn, b'block', MAX_BLOCK_MESSAGE_LENGTH))
        conn = node.add_p2p_connection(QuietPeer())
        self.send_refused(conn, header_only(conn, b'block', over_4mb), refused_log)

        self.log.info("The message of a peer that disconnects stops counting")
        conn, peer_id, _ = peers.pop(0)
        with node.assert_debug_log(['Cleared nodestate for peer={}'.format(peer_id)]):
            conn.peer_disconnect()
        conn = node.add_p2p_connection(QuietPeer())
        self.send_accepted(conn, self.newest_peer_id(), header_only(conn, b'block', MAX_BLOCK_MESSAGE_LENGTH))
        conn = node.add_p2p_connection(QuietPeer())
        self.send_refused(conn, header_only(conn, b'block', over_4mb), refused_log)
        node.disconnect_p2ps()


if __name__ == '__main__':
    LargeMessagesTest().main()
