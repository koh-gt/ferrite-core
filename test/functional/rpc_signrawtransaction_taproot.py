#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the signing RPCs on transactions that spend a Taproot output.

The signing code cannot sign for a Taproot output, and it can check a
Taproot signature only if it knows every output the transaction spends.
The test funds a Taproot output with a key path and one script leaf, and
builds a valid key-path spend and a valid script-path spend of it. It
checks that signrawtransactionwithkey and signrawtransactionwithwallet
return the witness unchanged, that they report the input as complete
when every spent output is known with its amount and as not complete
when one is not, that they fail when the amount of the Taproot input is
not given, that combinerawtransaction returns the input without a
witness, that walletprocesspsbt and finalizepsbt add the witness UTXO to
a Taproot input that comes with its previous transaction only, and that
the node accepts what the signing RPCs return.
"""

import base64
from decimal import Decimal

from test_framework.address import program_to_witness
from test_framework.key import compute_xonly_pubkey, generate_privkey, sign_schnorr, tweak_add_privkey
from test_framework.messages import COIN, MAX_MONEY, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, ToHex, ser_compact_size
from test_framework.script import (
    CScript,
    LEAF_VERSION_TAPSCRIPT,
    OP_CHECKSIG,
    OP_TRUE,
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    taproot_construct,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, find_vout_for_address
from test_framework.wallet_util import bytes_to_wif

# The error reported for a Taproot input whose signature cannot be checked
# because another spent output or amount is not known.
NOT_CHECKED = {"key": "No error", "script": "unknown error"}


def witness_hex(tx, index):
    return [item.hex() for item in tx.wit.vtxinwit[index].scriptWitness.stack]


def psbt_with_previous_tx(tx, previous_tx):
    """Return a PSBT for the one-input transaction tx whose input has its previous transaction and no witness UTXO."""
    def pair(key, value):
        return ser_compact_size(len(key)) + key + ser_compact_size(len(value)) + value
    data = b"psbt\xff" + pair(b"\x00", tx.serialize_without_witness()) + b"\x00"
    data += pair(b"\x00", previous_tx) + b"\x00"
    data += b"\x00" * len(tx.vout)
    return base64.b64encode(data).decode()


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

    def check_result(self, result, tx, errors):
        """Check that the transaction comes back byte for byte and that the inputs in errors (index: error) fail."""
        assert_equal(result["hex"], ToHex(tx))
        assert_equal(result["complete"], not errors)
        prevouts = [(txin.prevout.hash, txin.prevout.n) for txin in tx.vin]
        found = {}
        for entry in result.get("errors", []):
            index = prevouts.index((int(entry["txid"], 16), entry["vout"]))
            assert_equal(entry["witness"], witness_hex(tx, index))
            found[index] = entry["error"]
        assert_equal(found, errors)

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
        txid = node.sendtoaddress(address, 1)
        vout = find_vout_for_address(node, txid, address)
        node.generate(1)
        outpoint = COutPoint(int(txid, 16), vout)
        spent_output = CTxOut(COIN, tap.scriptPubKey)
        prevtxs = [{"txid": txid, "vout": vout, "scriptPubKey": tap.scriptPubKey.hex(), "amount": 1}]
        self.dest_script = bytes.fromhex(node.validateaddress(node.getnewaddress())["scriptPubKey"])
        privkey = bytes_to_wif(internal_key)

        spends = {path: self.spend([outpoint], [spent_output], Decimal("0.999"), 0, path) for path in ("key", "script")}
        for path, tx in spends.items():
            self.log.info(f"The {path}-path spend is valid")
            assert_equal(node.testmempoolaccept([ToHex(tx)])[0]["allowed"], True)

            self.log.info(f"signrawtransactionwithkey and signrawtransactionwithwallet keep the {path}-path witness and check it")
            self.check_result(node.signrawtransactionwithkey(ToHex(tx), [privkey], prevtxs), tx, {})
            self.check_result(node.signrawtransactionwithwallet(ToHex(tx)), tx, {})

        # A second input, whose output anyone can spend with an empty scriptSig.
        other = COutPoint(0x0123456789abcdef, 0)
        other_output = CTxOut(COIN, CScript([OP_TRUE]))
        other_no_amount = {"txid": f"{other.hash:064x}", "vout": other.n, "scriptPubKey": other_output.scriptPubKey.hex()}
        for path in ("key", "script"):
            tx = self.spend([outpoint, other], [spent_output, other_output], Decimal("1.999"), 0, path)
            self.log.info(f"The {path}-path signature is not checked when the output of another input is not known")
            errors = {0: NOT_CHECKED[path], 1: "Input not found or already spent"}
            self.check_result(node.signrawtransactionwithkey(ToHex(tx), [privkey], prevtxs), tx, errors)
            self.check_result(node.signrawtransactionwithwallet(ToHex(tx)), tx, errors)

            self.log.info(f"The {path}-path signature is not checked when another input is given without its amount")
            self.check_result(node.signrawtransactionwithkey(ToHex(tx), [privkey], prevtxs + [other_no_amount]), tx, {0: NOT_CHECKED[path]})
            self.check_result(node.signrawtransactionwithwallet(ToHex(tx), [other_no_amount]), tx, {0: NOT_CHECKED[path]})

        self.log.info("Without the amount of the Taproot input the signing RPCs fail")
        no_amount = [{key: value for key, value in prevtxs[0].items() if key != "amount"}]
        missing_amount = f"Missing amount for CTxOut(nValue={MAX_MONEY // COIN}.{MAX_MONEY % COIN:08d}, scriptPubKey={tap.scriptPubKey.hex()[:30]})"
        for tx in spends.values():
            assert_raises_rpc_error(-3, missing_amount, node.signrawtransactionwithkey, ToHex(tx), [privkey], no_amount)
            assert_raises_rpc_error(-3, missing_amount, node.signrawtransactionwithwallet, ToHex(tx), no_amount)

        self.log.info("combinerawtransaction returns the input without a witness")
        combined = node.combinerawtransaction([ToHex(spends["key"]), ToHex(spends["script"])])
        vin = node.decoderawtransaction(combined)["vin"]
        assert_equal(len(vin), 1)
        assert_equal((vin[0]["txid"], vin[0]["vout"]), (txid, vout))
        assert "txinwitness" not in vin[0]

        self.log.info("walletprocesspsbt and finalizepsbt add the witness UTXO of a Taproot input")
        witness_utxo = {"amount": Decimal("1"), "scriptPubKey": tap.scriptPubKey.hex()}
        psbt = psbt_with_previous_tx(spends["key"], bytes.fromhex(node.gettransaction(txid)["hex"]))
        assert "witness_utxo" not in node.decodepsbt(psbt)["inputs"][0]
        # The wallet fills in the previous transaction of a PSBT that does not have it.
        created = node.createpsbt([{"txid": txid, "vout": vout}], [{node.getnewaddress(): Decimal("0.999")}])
        for result in (node.walletprocesspsbt(psbt), node.finalizepsbt(psbt), node.walletprocesspsbt(created)):
            assert_equal(result["complete"], False)
            psbt_input = node.decodepsbt(result["psbt"])["inputs"][0]
            assert_equal(psbt_input["non_witness_utxo"]["txid"], txid)
            got = psbt_input["witness_utxo"]
            assert_equal({"amount": got["amount"], "scriptPubKey": got["scriptPubKey"]["hex"]}, witness_utxo)

        self.log.info("With SIGHASH_SINGLE and no output for the Taproot input the witness is kept and checked")
        coin = node.listunspent()[0]
        wallet_outpoint = COutPoint(int(coin["txid"], 16), coin["vout"])
        wallet_output = CTxOut(int(coin["amount"] * COIN), bytes.fromhex(coin["scriptPubKey"]))
        for path in ("key", "script"):
            tx = self.spend([wallet_outpoint, outpoint], [wallet_output, spent_output], coin["amount"] + Decimal("0.999"), 1, path)
            result = node.signrawtransactionwithwallet(ToHex(tx), [], "SINGLE")
            assert_equal(result["complete"], True)
            assert "errors" not in result
            assert_equal(node.decoderawtransaction(result["hex"])["vin"][1]["txinwitness"], witness_hex(tx, 1))
            assert_equal(node.testmempoolaccept([result["hex"]])[0]["allowed"], True)

        self.log.info("The node accepts the key-path spend and mines it")
        spent_txid = node.sendrawtransaction(ToHex(spends["key"]))
        block = node.generate(1)[0]
        assert spent_txid in node.getblock(block)["tx"]
        assert_equal(node.gettxout(txid, vout), None)


if __name__ == '__main__':
    SignRawTransactionTaprootTest().main()
