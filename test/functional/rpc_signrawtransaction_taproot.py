#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the signing RPCs on transactions that spend a Taproot output.

A Taproot signature hash commits to every output the transaction spends.
The signing code checks the witnesses a transaction already has without
those outputs, so it cannot check a Taproot signature. The test gives
signrawtransactionwithkey, signrawtransactionwithwallet and
combinerawtransaction a valid key-path witness and a valid script-path
witness. It checks that signrawtransactionwithkey and
signrawtransactionwithwallet report the input as not complete, that
combinerawtransaction returns it without a witness, that the node keeps
running, and that with SIGHASH_SINGLE and no matching output
signrawtransactionwithwallet returns the witness unchanged. Mempool and
block validation have the spent outputs: the node accepts both witnesses
and mines one.
"""

from decimal import Decimal

from test_framework.address import program_to_witness
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr, tweak_add_privkey
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, ToHex
from test_framework.script import (
    CScript,
    LEAF_VERSION_TAPSCRIPT,
    OP_CHECKSIG,
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    taproot_construct,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, find_vout_for_address
from test_framework.wallet_util import bytes_to_wif

# The error for an input whose witness was dropped because it could not be checked.
EMPTY_WITNESS = "Witness program was passed an empty witness"


class SignRawTransactionTaprootTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [['-vbparams=mweb:-2:0']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def taproot_witness(self, tx, spent_outputs, index, path):
        """Return a valid witness for input index, spending the Taproot output by key or by script."""
        if path == "key":
            sighash = TaprootSignatureHash(tx, spent_outputs, SIGHASH_DEFAULT, index)
            return [sign_schnorr(self.output_key, sighash)]
        sighash = TaprootSignatureHash(tx, spent_outputs, SIGHASH_DEFAULT, index, scriptpath=True, script=self.leaf_script)
        return [sign_schnorr(self.leaf_key, sighash), self.leaf_script, self.control_block]

    def spend(self, inputs, spent_outputs, value, taproot_index, path):
        """Build a transaction with the given inputs and one output, with a witness for the Taproot input."""
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint) for outpoint in inputs]
        tx.vout = [CTxOut(int(value * COIN), self.dest_script)]
        tx.wit.vtxinwit = [CTxInWitness() for _ in inputs]
        tx.wit.vtxinwit[taproot_index].scriptWitness.stack = self.taproot_witness(tx, spent_outputs, taproot_index, path)
        return tx

    def check_not_complete(self, result, index, error, witness):
        assert_equal(result["complete"], False)
        assert_equal(len(result["errors"]), 1)
        entry = result["errors"][0]
        assert_equal((entry["txid"], entry["vout"]), (self.txid, self.vout))
        assert_equal(entry["error"], error)
        assert_equal(entry["witness"], witness)
        vin = self.nodes[0].decoderawtransaction(result["hex"])["vin"][index]
        assert_equal(vin.get("txinwitness", []), witness)

    def run_test(self):
        node = self.nodes[0]
        node.generate(101)

        self.log.info("Fund a Taproot output with a key path and one script leaf")
        internal_key = generate_privkey()
        internal_pubkey, _ = compute_xonly_pubkey(internal_key)
        self.leaf_key = generate_privkey()
        self.leaf_script = CScript([compute_xonly_pubkey(self.leaf_key)[0], OP_CHECKSIG])
        tap = taproot_construct(internal_pubkey, [("leaf", self.leaf_script)])
        self.output_key = tweak_add_privkey(internal_key, tap.tweak)
        self.control_block = bytes([LEAF_VERSION_TAPSCRIPT + tap.negflag]) + internal_pubkey + tap.leaves["leaf"].merklebranch
        address = program_to_witness(1, tap.scriptPubKey[2:])
        self.txid = node.sendtoaddress(address, 1)
        self.vout = find_vout_for_address(node, self.txid, address)
        node.generate(1)
        outpoint = COutPoint(int(self.txid, 16), self.vout)
        spent_output = CTxOut(COIN, tap.scriptPubKey)
        prevtxs = [{"txid": self.txid, "vout": self.vout, "scriptPubKey": tap.scriptPubKey.hex(), "amount": 1}]
        self.dest_script = bytes.fromhex(node.validateaddress(node.getnewaddress())["scriptPubKey"])
        privkey = bytes_to_wif(internal_key)

        spends = {path: self.spend([outpoint], [spent_output], Decimal("0.999"), 0, path) for path in ("key", "script")}
        for path, tx in spends.items():
            self.log.info(f"The {path}-path spend is valid")
            assert_equal(node.testmempoolaccept([ToHex(tx)])[0]["allowed"], True)

            self.log.info(f"signrawtransactionwithkey reports the {path}-path input as not complete")
            result = node.signrawtransactionwithkey(ToHex(tx), [privkey], prevtxs)
            self.check_not_complete(result, 0, EMPTY_WITNESS, [])

            self.log.info(f"signrawtransactionwithwallet reports the {path}-path input as not complete")
            result = node.signrawtransactionwithwallet(ToHex(tx))
            self.check_not_complete(result, 0, EMPTY_WITNESS, [])

        self.log.info("combinerawtransaction returns the input without a witness")
        combined = node.combinerawtransaction([ToHex(spends["key"]), ToHex(spends["script"])])
        vin = node.decoderawtransaction(combined)["vin"]
        assert_equal(len(vin), 1)
        assert_equal((vin[0]["txid"], vin[0]["vout"]), (self.txid, self.vout))
        assert "txinwitness" not in vin[0]

        self.log.info("With SIGHASH_SINGLE and no matching output the Taproot witness is returned unchanged")
        coin = node.listunspent()[0]
        wallet_outpoint = COutPoint(int(coin["txid"], 16), coin["vout"])
        wallet_output = CTxOut(int(coin["amount"] * COIN), bytes.fromhex(coin["scriptPubKey"]))
        for path, error in (("key", "No error"), ("script", "unknown error")):
            tx = self.spend([wallet_outpoint, outpoint], [wallet_output, spent_output], coin["amount"] + Decimal("0.999"), 1, path)
            witness = [item.hex() for item in tx.wit.vtxinwit[1].scriptWitness.stack]
            result = node.signrawtransactionwithwallet(ToHex(tx), [], "SINGLE")
            self.check_not_complete(result, 1, error, witness)

        self.log.info("The node accepts the key-path spend and mines it")
        txid = node.sendrawtransaction(ToHex(spends["key"]))
        block = node.generate(1)[0]
        assert txid in node.getblock(block)["tx"]
        assert_equal(node.gettxout(self.txid, self.vout), None)


if __name__ == '__main__':
    SignRawTransactionTaprootTest().main()
