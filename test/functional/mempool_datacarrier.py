#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test -datacarrier and -datacarriersize.

Check that:

- with the default settings, a transaction with an OP_RETURN output of a
  few thousand bytes is accepted and relayed to a second node, and the
  largest OP_RETURN output that keeps a transaction within the standard
  weight limit is accepted, while one byte more makes it too large;
- -datacarrier=0 refuses OP_RETURN outputs;
- -datacarriersize=83, the default of earlier versions, accepts 80 bytes of
  data and refuses 81.
"""

from test_framework.address import script_to_p2wsh
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
)
from test_framework.script import (
    CScript,
    OP_0,
    OP_RETURN,
    OP_TRUE,
    sha256,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

MAX_STANDARD_TX_WEIGHT = 400000
OLD_DEFAULT_DATACARRIER_SIZE = 83
# Coins sent to this script are spent with a witness stack of just OP_TRUE
P2WSH_OP_TRUE = CScript([OP_0, sha256(CScript([OP_TRUE]))])


def tx_weight(tx):
    return 3 * len(tx.serialize_without_witness()) + len(tx.serialize_with_witness())


class DataCarrierTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.extra_args = [
            [],
            [],
            ["-datacarrier=0"],
            [f"-datacarriersize={OLD_DEFAULT_DATACARRIER_SIZE}"],
        ]

    def null_data_transaction(self, data):
        """Return a transaction that spends the next coin to the same script, with an OP_RETURN output for data."""
        utxo = self.utxos[-1]
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(utxo['txid'], 16), utxo['vout']))]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [CScript([OP_TRUE])]
        tx.vout = [
            CTxOut(int(utxo['value'] * COIN), P2WSH_OP_TRUE),
            CTxOut(0, CScript([OP_RETURN, data])),
        ]
        tx.vout[0].nValue -= 10 * tx.get_vsize()  # fee of 10 atoms per virtual byte
        tx.rehash()
        return tx

    def test_null_data_transaction(self, node, data, success, reason="scriptpubkey"):
        tx = self.null_data_transaction(data)
        tx_hex = tx.serialize().hex()
        if success:
            assert_equal(node.sendrawtransaction(tx_hex), tx.hash)
            assert tx.hash in node.getrawmempool()
            self.utxos.pop()
        else:
            assert_raises_rpc_error(-26, reason, node.sendrawtransaction, tx_hex)
        return tx.hash

    def run_test(self):
        self.utxos = []
        for blockhash in self.nodes[0].generatetoaddress(4, script_to_p2wsh(CScript([OP_TRUE]))):
            coinbase = self.nodes[0].getblock(blockhash, 2)['tx'][0]
            assert_equal(coinbase['vout'][0]['scriptPubKey']['hex'], P2WSH_OP_TRUE.hex())
            self.utxos.append({'txid': coinbase['txid'], 'vout': 0, 'value': coinbase['vout'][0]['value']})
        self.nodes[0].generate(100)
        self.sync_blocks()

        self.log.info("With the default settings, an OP_RETURN output of 4,000 bytes is accepted and relayed")
        txid = self.test_null_data_transaction(self.nodes[0], bytes(4000), success=True)
        self.wait_until(lambda: txid in self.nodes[1].getrawmempool())

        self.log.info("With the default settings, the standard weight limit is what limits an OP_RETURN output")
        # With more than 65,535 bytes of data, each further byte of data adds
        # one byte to the output script and four to the transaction weight.
        size = 99000
        size += (MAX_STANDARD_TX_WEIGHT - tx_weight(self.null_data_transaction(bytes(size)))) // 4
        largest = self.null_data_transaction(bytes(size))
        assert tx_weight(largest) <= MAX_STANDARD_TX_WEIGHT
        assert_greater_than(tx_weight(self.null_data_transaction(bytes(size + 1))), MAX_STANDARD_TX_WEIGHT)
        self.log.info(f"Largest OP_RETURN output script here: {len(largest.vout[1].scriptPubKey)} bytes, {size} bytes of data")
        self.test_null_data_transaction(self.nodes[0], bytes(size + 1), success=False, reason="tx-size")
        self.test_null_data_transaction(self.nodes[0], bytes(size), success=True)

        self.log.info("-datacarrier=0 refuses OP_RETURN outputs")
        self.test_null_data_transaction(self.nodes[2], bytes(80), success=False)
        self.test_null_data_transaction(self.nodes[2], b'', success=False)

        self.log.info("-datacarriersize=83 accepts 80 bytes of data and refuses 81")
        self.test_null_data_transaction(self.nodes[3], bytes(81), success=False)
        self.test_null_data_transaction(self.nodes[3], bytes(80), success=True)


if __name__ == '__main__':
    DataCarrierTest().main()
