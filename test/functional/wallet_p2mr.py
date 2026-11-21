#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the wallet and the signing RPCs with P2MR outputs (BIP 360).

The node signals the p2mr deployment from block 288 (-vbparams), so the P2MR rules apply from block
576, after MWEB, which is active from block 432.

Until the rules apply to the next block, the wallet refuses to create an output of witness
version 2: sendtoaddress and sendmany fail with -6; send, fundrawtransaction and
walletcreatefundedpsbt with -4, also for such a change address chosen by the caller when no
change output would be needed; bumpfee and psbtbumpfee with -4, and a peg-out from MWEB coins to
such an address fails like any other payment. createrawtransaction, sendrawtransaction and
generatetoaddress pay such addresses. Once the rules apply, the wallet pays them again.

The signing RPCs keep the witness of a P2MR input, as they do for a native Taproot input, and check
it when every spent output is known with its amount; combinerawtransaction keeps the first witness
given for such an input; the PSBT RPCs add its witness UTXO; and a witness that checks a signature
without the data to check it stops neither the node nor ferrite-tx. getaddressinfo describes a
known script OP_2 <32 bytes> with the type witness_v2_p2mr.
"""

import base64
import json
import os
import subprocess
from decimal import Decimal

from test_framework.address import program_to_witness, script_to_p2sh, script_to_p2wsh
from test_framework.fec_util import setup_mweb_chain
from test_framework.key import compute_xonly_pubkey, sign_schnorr, tweak_add_privkey
from test_framework.messages import COIN, MAX_MONEY, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, FromHex, ToHex, ser_compact_size, sha256
from test_framework.script import CScript, OP_CHECKSIG, OP_TRUE, SIGHASH_DEFAULT, TaprootSignatureHash, p2mr_construct, taproot_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet_util import bytes_to_wif

REFUSED = "Witness version 2 outputs can be spent by anyone until the p2mr soft fork is active"
START_HEIGHT = 288
ACTIVATION_HEIGHT = START_HEIGHT + 2 * 144
NOT_CHECKED = "unknown error"

# The keys of this test. They are used by this test only.
LEAF_KEY = sha256(b"wallet_p2mr.py leaf key")
INTERNAL_KEY = sha256(b"wallet_p2mr.py internal key")


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


class WalletP2MRTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Replacements are relayed only with -mempoolreplacement.
        self.extra_args = [["-vbparams=p2mr:0:0:{}:99999999".format(START_HEIGHT), "-mempoolreplacement=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def p2mr_state(self):
        return self.nodes[0].getblockchaininfo()['softforks']['p2mr']

    def coinbase_coins(self):
        """The wallet's spendable coinbase outputs, which are not MWEB outputs."""
        address = self.nodes[0].get_deterministic_priv_key().address
        return [u for u in self.wallet.listunspent() if u.get('address') == address and u['spendable']]

    def pay_raw(self, outputs, replaceable=False):
        """Pay these outputs ({address: amount}) through createrawtransaction, signrawtransactionwithwallet and
        sendrawtransaction, with the change to getrawchangeaddress, and return the transaction id."""
        node = self.nodes[0]
        total = sum(outputs.values())
        coin = next(u for u in self.coinbase_coins() if u['amount'] > total + 1)
        outputs = dict(outputs)
        outputs[self.wallet.getrawchangeaddress()] = coin['amount'] - total - Decimal("0.001")
        inputs = [{'txid': coin['txid'], 'vout': coin['vout'], 'sequence': 0xfffffffd if replaceable else 0xffffffff}]
        signed = self.wallet.signrawtransactionwithwallet(node.createrawtransaction(inputs, outputs))
        assert signed['complete']
        return node.sendrawtransaction(signed['hex'])

    def outpoint(self, txid, script_pubkey):
        """The outpoint and output of the first output of the wallet transaction with this scriptPubKey."""
        tx = FromHex(CTransaction(), self.wallet.gettransaction(txid)['hex'])
        n = next(i for i, out in enumerate(tx.vout) if out.scriptPubKey == bytes(script_pubkey))
        return COutPoint(int(txid, 16), n), tx.vout[n]

    def run_test(self):
        node = self.nodes[0]
        self.wallet = node.get_wallet_rpc(self.default_wallet_name)
        self.payout_script = bytes.fromhex(self.wallet.getaddressinfo(self.wallet.getnewaddress())['scriptPubKey'])
        key_script = CScript([compute_xonly_pubkey(LEAF_KEY)[0], OP_CHECKSIG])
        self.tree = p2mr_construct([("key", key_script), ("true", CScript([OP_TRUE]))])
        self.p2mr_address = program_to_witness(2, self.tree.merkle_root)
        self.v2_20_address = program_to_witness(2, bytes([0x5a] * 20))
        assert self.p2mr_address.startswith("rfec1z") and self.v2_20_address.startswith("rfec1z")

        self.log.info("Activate MWEB; the p2mr deployment is locked in, its rules apply from block {}".format(ACTIVATION_HEIGHT))
        setup_mweb_chain(node)
        assert_equal(self.p2mr_state()['bip8']['status'], 'locked_in')
        node.createwallet(wallet_name="mweb_only")
        self.mweb_only = node.get_wallet_rpc("mweb_only")
        self.wallet.sendtoaddress(self.mweb_only.getnewaddress(address_type='mweb'), 10)
        node.generate(1)
        assert_equal(self.mweb_only.getbalance(), 10)

        self.test_addresses()
        self.test_before_activation()
        self.test_signing()
        self.test_combine()
        self.test_psbt()
        self.test_address_info()

        node.generate(ACTIVATION_HEIGHT - 1 - node.getblockcount())
        assert_equal(self.p2mr_state()['active'], True)
        self.test_after_activation()

    def test_addresses(self):
        self.log.info("validateaddress and getaddressinfo describe a P2MR address as a witness program of version 2")
        for address, program in [(self.p2mr_address, self.tree.merkle_root), (self.v2_20_address, bytes([0x5a] * 20))]:
            info = self.wallet.getaddressinfo(address)
            assert_equal((info['iswitness'], info['witness_version'], info['witness_program']), (True, 2, program.hex()))
            assert_equal((info['ismine'], info['solvable']), (False, False))
            assert_equal(self.nodes[0].validateaddress(address)['witness_version'], 2)

    def test_before_activation(self):
        node = self.nodes[0]
        wallet = self.wallet
        assert_equal(self.p2mr_state()['active'], False)
        normal = wallet.getnewaddress()

        self.log.info("Before activation the wallet refuses to pay witness version 2 addresses")
        for address in [self.p2mr_address, self.v2_20_address]:
            assert_raises_rpc_error(-6, REFUSED, wallet.sendtoaddress, address, 1)
            assert_raises_rpc_error(-6, REFUSED, wallet.sendmany, "", {address: 1})
            assert_raises_rpc_error(-6, REFUSED, wallet.sendmany, "", {normal: 1, address: 1})
            assert_raises_rpc_error(-4, REFUSED, wallet.send, {address: 1})
            assert_raises_rpc_error(-4, REFUSED, wallet.fundrawtransaction, node.createrawtransaction([], {address: 1}))
            assert_raises_rpc_error(-4, REFUSED, wallet.walletcreatefundedpsbt, [], {address: 1})

        self.log.info("... and change addresses of witness version 2, also when no change output is needed")
        raw = node.createrawtransaction([], {normal: 1})
        for option in ["changeAddress", "change_address"]:
            assert_raises_rpc_error(-4, REFUSED, wallet.fundrawtransaction, raw, {option: self.p2mr_address})
            assert_raises_rpc_error(-4, REFUSED, wallet.walletcreatefundedpsbt, [], {normal: 1}, 0, {option: self.p2mr_address})
        assert_raises_rpc_error(-4, REFUSED, wallet.send, outputs={normal: 1}, options={"change_address": self.p2mr_address})
        assert_raises_rpc_error(-8, "Use change_address", wallet.send, outputs={normal: 1}, options={"changeAddress": self.p2mr_address})
        coin = self.coinbase_coins()[0]
        inputs = [{'txid': coin['txid'], 'vout': coin['vout']}]
        exact = node.createrawtransaction(inputs, {normal: coin['amount']})
        no_change = {"add_inputs": False, "subtractFeeFromOutputs": [0]}
        assert_equal(wallet.fundrawtransaction(exact, no_change)['changepos'], -1)
        assert_raises_rpc_error(-4, REFUSED, wallet.fundrawtransaction, exact, dict(no_change, changeAddress=self.p2mr_address))
        assert_raises_rpc_error(-4, REFUSED, wallet.walletcreatefundedpsbt, inputs, {normal: coin['amount']}, 0, dict(no_change, change_address=self.p2mr_address))
        assert_raises_rpc_error(-4, REFUSED, wallet.send, outputs={normal: coin['amount']},
                                options={"inputs": inputs, "add_inputs": False, "subtract_fee_from_outputs": [0], "change_address": self.p2mr_address})

        self.log.info("A peg-out from MWEB coins to such an address is refused like any other payment")
        assert_raises_rpc_error(-6, REFUSED, self.mweb_only.sendtoaddress, self.p2mr_address, 1)
        pegout = self.mweb_only.sendtoaddress(normal, 1)
        node.generate(1)
        assert_equal(self.mweb_only.gettransaction(pegout)['confirmations'], 1)

        self.log.info("createrawtransaction, sendrawtransaction and generatetoaddress pay such addresses")
        paid = self.pay_raw({self.p2mr_address: 1, self.v2_20_address: 1}, replaceable=True)
        block = node.generatetoaddress(1, self.p2mr_address)[0]
        coinbase = node.getblock(block, 2)['tx'][0]
        assert_equal(coinbase['vout'][0]['scriptPubKey']['addresses'], [self.p2mr_address])
        assert_equal(coinbase['vout'][0]['scriptPubKey']['type'], 'witness_v2_p2mr')
        assert paid in node.getblock(block)['tx']

        self.log.info("bumpfee and psbtbumpfee refuse a transaction that pays such an address")
        txid = self.pay_raw({self.p2mr_address: 1}, replaceable=True)
        assert_raises_rpc_error(-4, "Unable to create transaction. " + REFUSED, wallet.bumpfee, txid)
        assert_raises_rpc_error(-4, "Unable to create transaction. " + REFUSED, wallet.psbtbumpfee, txid)
        node.generate(1)

    def test_after_activation(self):
        node = self.nodes[0]
        wallet = self.wallet
        normal = wallet.getnewaddress()

        self.log.info("Once the rules apply to the next block, the wallet pays witness version 2 addresses")
        txids = []
        for address in [self.p2mr_address, self.v2_20_address]:
            txids.append(wallet.sendtoaddress(address, 1))
            txids.append(wallet.sendmany("", {normal: 1, address: 1}))
            txids.append(wallet.send({address: 1})['txid'])
            funded = wallet.fundrawtransaction(node.createrawtransaction([], {address: 1}))
            assert_equal(wallet.signrawtransactionwithwallet(funded['hex'])['complete'], True)
            assert wallet.walletcreatefundedpsbt([], {address: 1})['psbt']
        tx = node.decoderawtransaction(node.getrawtransaction(txids[0]))
        assert 'witness_v2_p2mr' in [out['scriptPubKey']['type'] for out in tx['vout']]
        for option in ["changeAddress", "change_address"]:
            funded = wallet.fundrawtransaction(node.createrawtransaction([], {normal: 1}), {option: self.p2mr_address})
            change = FromHex(CTransaction(), funded['hex']).vout[funded['changepos']]
            assert_equal(change.scriptPubKey, bytes(self.tree.scriptPubKey))
            assert wallet.walletcreatefundedpsbt([], {normal: 1}, 0, {option: self.p2mr_address})['psbt']
        txids.append(wallet.send(outputs={normal: 1}, options={"change_address": self.p2mr_address})['txid'])
        pegout = self.mweb_only.sendtoaddress(self.p2mr_address, 1)
        bumped = self.pay_raw({self.p2mr_address: 1}, replaceable=True)
        txids.append(wallet.bumpfee(bumped)['txid'])
        bumped = self.pay_raw({self.p2mr_address: 1}, replaceable=True)
        assert wallet.psbtbumpfee(bumped)['psbt']
        node.generate(1)
        for txid in txids:
            assert_equal(wallet.gettransaction(txid)['confirmations'], 1)
        assert_equal(self.mweb_only.gettransaction(pegout)['confirmations'], 1)

    def fund_spend_outputs(self):
        """Fund a Taproot output with a key path and P2MR outputs through the open paths; return their outpoints."""
        tap = taproot_construct(compute_xonly_pubkey(INTERNAL_KEY)[0])
        self.output_key = tweak_add_privkey(INTERNAL_KEY, tap.tweak)
        self.taproot_spk = tap.scriptPubKey
        taproot_address = program_to_witness(1, tap.scriptPubKey[2:])
        txid = self.pay_raw({taproot_address: 1})
        p2mr_txid = self.pay_raw({self.p2mr_address: 1})
        self.nodes[0].generate(1)
        return self.outpoint(txid, tap.scriptPubKey), self.outpoint(p2mr_txid, self.tree.scriptPubKey), p2mr_txid

    def spend(self, utxos, value, witnesses):
        """A transaction that spends the outputs; witnesses[i](tx, spent outputs) gives the witness of input i, or None."""
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint) for outpoint, _ in utxos]
        tx.vout = [CTxOut(int(value * COIN), self.payout_script)]
        tx.wit.vtxinwit = [CTxInWitness() for _ in utxos]
        spent = [out for _, out in utxos]
        for i, make_witness in enumerate(witnesses):
            if make_witness:
                tx.wit.vtxinwit[i].scriptWitness.stack = make_witness(tx, spent)
        tx.rehash()
        return tx

    def key_path_witness(self, index):
        return lambda tx, spent: [sign_schnorr(self.output_key, TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, index))]

    def key_leaf_witness(self, index):
        leaf = self.tree.leaves["key"]
        return lambda tx, spent: [sign_schnorr(LEAF_KEY, TaprootSignatureHash(tx, spent, SIGHASH_DEFAULT, index, scriptpath=True, script=leaf.script)),
                                  leaf.script, leaf.control_block]

    def check_signed(self, result, tx, errors):
        """The transaction comes back byte for byte, and exactly the inputs in errors (index: error) fail."""
        assert_equal(result['hex'], ToHex(tx))
        assert_equal(result['complete'], not errors)
        prevouts = [(txin.prevout.hash, txin.prevout.n) for txin in tx.vin]
        found = {}
        for entry in result.get('errors', []):
            index = prevouts.index((int(entry['txid'], 16), entry['vout']))
            assert_equal(entry['witness'], witness_hex(tx, index))
            found[index] = entry['error']
        assert_equal(found, errors)

    def test_signing(self):
        node = self.nodes[0]
        wallet = self.wallet
        taproot, p2mr, self.p2mr_txid = self.fund_spend_outputs()
        self.p2mr_utxos = [self.outpoint(self.pay_raw({self.p2mr_address: 1}), self.tree.scriptPubKey) for _ in range(3)]
        node.generate(1)
        prevtxs = [{'txid': '{:064x}'.format(o.hash), 'vout': o.n, 'scriptPubKey': out.scriptPubKey.hex(), 'amount': Decimal(out.nValue) / COIN}
                   for o, out in [taproot, p2mr]]
        privkeys = [bytes_to_wif(LEAF_KEY)]

        self.log.info("The signing RPCs keep the witnesses of a Taproot key-path input and a P2MR input and check them")
        tx = self.spend([taproot, p2mr], Decimal("1.999"), [self.key_path_witness(0), self.key_leaf_witness(1)])
        assert_equal(node.testmempoolaccept([ToHex(tx)])[0]['reject-reason'], 'bad-txns-nonstandard-inputs')
        self.check_signed(wallet.signrawtransactionwithwallet(ToHex(tx)), tx, {})
        self.check_signed(node.signrawtransactionwithkey(ToHex(tx), privkeys, prevtxs), tx, {})

        self.log.info("When another spent output or its amount is not known, the signatures are not checked")
        other = (COutPoint(0x0123456789abcdef, 0), CTxOut(COIN, CScript([OP_TRUE])))
        other_no_amount = {'txid': '{:064x}'.format(other[0].hash), 'vout': 0, 'scriptPubKey': other[1].scriptPubKey.hex()}
        tx = self.spend([taproot, p2mr, other], Decimal("2.999"), [self.key_path_witness(0), self.key_leaf_witness(1), None])
        unknown = {0: NOT_CHECKED, 1: NOT_CHECKED, 2: "Input not found or already spent"}
        self.check_signed(wallet.signrawtransactionwithwallet(ToHex(tx)), tx, unknown)
        self.check_signed(node.signrawtransactionwithkey(ToHex(tx), privkeys, prevtxs), tx, unknown)
        self.check_signed(wallet.signrawtransactionwithwallet(ToHex(tx), [other_no_amount]), tx, {0: NOT_CHECKED, 1: NOT_CHECKED})
        self.check_signed(node.signrawtransactionwithkey(ToHex(tx), privkeys, prevtxs + [other_no_amount]), tx, {0: NOT_CHECKED, 1: NOT_CHECKED})

        self.log.info("Without the amount of the P2MR input the signing RPCs fail")
        tx = self.spend([taproot, p2mr], Decimal("1.999"), [self.key_path_witness(0), self.key_leaf_witness(1)])
        no_amount = [prevtxs[0], {key: value for key, value in prevtxs[1].items() if key != 'amount'}]
        missing = "Missing amount for CTxOut(nValue={}.{:08d}, scriptPubKey={})".format(MAX_MONEY // COIN, MAX_MONEY % COIN, self.tree.scriptPubKey.hex()[:30])
        assert_raises_rpc_error(-3, missing, node.signrawtransactionwithkey, ToHex(tx), privkeys, no_amount)
        assert_raises_rpc_error(-3, missing, wallet.signrawtransactionwithwallet, ToHex(tx), no_amount)

        self.log.info("A P2MR witness with a signature that cannot be checked stops neither the node nor ferrite-tx")
        leaf = self.tree.leaves["key"]
        crafted = self.spend([p2mr], Decimal("0.999"), [lambda tx, spent: [bytes(range(64)), leaf.script, leaf.control_block]])
        result = node.signrawtransactionwithkey(ToHex(crafted), privkeys, prevtxs[1:])
        self.check_signed(result, crafted, {0: "Invalid Schnorr signature"})
        crafted_two = self.spend([p2mr, other], Decimal("1.999"), [lambda tx, spent: [bytes(range(64)), leaf.script, leaf.control_block], None])
        self.check_signed(node.signrawtransactionwithkey(ToHex(crafted_two), privkeys, prevtxs[1:]), crafted_two, {0: NOT_CHECKED, 1: "Input not found or already spent"})
        combined = FromHex(CTransaction(), node.combinerawtransaction([ToHex(crafted), ToHex(crafted)]))
        assert_equal(witness_hex(combined, 0), witness_hex(crafted, 0))
        tool = os.path.join(self.config["environment"]["BUILDDIR"], "src", "ferrite-tx" + self.config["environment"]["EXEEXT"])
        if os.path.isfile(tool):
            tool_prevtxs = [dict(prevtxs[1], amount=str(prevtxs[1]['amount']))]
            run = subprocess.run([tool, "-regtest", ToHex(crafted), "set=privatekeys:" + json.dumps(privkeys), "set=prevtxs:" + json.dumps(tool_prevtxs), "sign=ALL"],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=60)
            assert_equal((run.returncode, run.stderr), (0, ""))
            assert_equal(witness_hex(FromHex(CTransaction(), run.stdout.strip()), 0), witness_hex(crafted, 0))
        else:
            self.log.info("ferrite-tx is not built: its case is skipped")
        assert_equal(node.getblockcount(), node.getblockchaininfo()['blocks'])

    def test_combine(self):
        self.log.info("combinerawtransaction keeps the first witness given for a P2MR input")
        node = self.nodes[0]
        utxo = self.p2mr_utxos.pop()
        true_leaf = self.tree.leaves["true"]
        copies = {
            "key": self.spend([utxo], Decimal("0.999"), [self.key_leaf_witness(0)]),
            # The witness of the leaf OP_TRUE is complete without the spent outputs, the one with a signature is not.
            "true": self.spend([utxo], Decimal("0.999"), [lambda tx, spent: [true_leaf.script, true_leaf.control_block]]),
            "none": self.spend([utxo], Decimal("0.999"), [None]),
        }
        assert_equal(copies["key"].hash, copies["true"].hash)
        for order in (("key", "true"), ("true", "key"), ("none", "key"), ("none", "true"), ("key", "none")):
            combined = FromHex(CTransaction(), node.combinerawtransaction([ToHex(copies[name]) for name in order]))
            first = order[1] if order[0] == "none" else order[0]
            assert_equal(witness_hex(combined, 0), witness_hex(copies[first], 0))
            assert_equal(node.testmempoolaccept([ToHex(combined)])[0]['reject-reason'], 'bad-txns-nonstandard-inputs')

    def test_psbt(self):
        self.log.info("The PSBT RPCs add the witness UTXO of a P2MR input")
        node = self.nodes[0]
        wallet = self.wallet
        utxo = self.p2mr_utxos.pop()
        witness_utxo = {'amount': Decimal(utxo[1].nValue) / COIN, 'scriptPubKey': utxo[1].scriptPubKey.hex()}

        def check_witness_utxo(psbt):
            psbt_input = node.decodepsbt(psbt)['inputs'][0]
            got = psbt_input['witness_utxo']
            assert_equal({'amount': got['amount'], 'scriptPubKey': got['scriptPubKey']['hex']}, witness_utxo)
            assert_equal(got['scriptPubKey']['type'], 'witness_v2_p2mr')
            return psbt_input

        tx = self.spend([utxo], Decimal("0.999"), [None])
        txid = '{:064x}'.format(utxo[0].hash)
        # With the previous transaction only.
        psbt = psbt_with_previous_tx(tx, bytes.fromhex(wallet.gettransaction(txid)['hex']))
        assert 'witness_utxo' not in node.decodepsbt(psbt)['inputs'][0]
        for result in (wallet.walletprocesspsbt(psbt), node.finalizepsbt(psbt)):
            assert_equal(result['complete'], False)
            assert_equal(check_witness_utxo(result['psbt'])['non_witness_utxo']['txid'], txid)
        # With the previous transaction in the wallet.
        created = node.createpsbt([{'txid': txid, 'vout': utxo[0].n}], [{wallet.getnewaddress(): Decimal("0.999")}])
        assert_equal(check_witness_utxo(wallet.walletprocesspsbt(created)['psbt'])['non_witness_utxo']['txid'], txid)
        # utxoupdatepsbt finds the output in the UTXO set and adds it as a witness UTXO.
        assert 'non_witness_utxo' not in check_witness_utxo(node.utxoupdatepsbt(created))

    def test_address_info(self):
        self.log.info("getaddressinfo describes a known script OP_2 <32 bytes> as witness_v2_p2mr")
        wallet = self.wallet
        script = bytes(self.tree.scriptPubKey)
        p2sh, p2wsh = script_to_p2sh(script), script_to_p2wsh(script)
        # getaddressinfo describes the script of a P2WSH address only when the wallet watches that address: importaddress
        # stores the script, importmulti watches the P2WSH address.
        wallet.importaddress(script.hex(), "", False, True)
        assert 'script' not in wallet.getaddressinfo(p2wsh)
        result = wallet.importmulti([{"scriptPubKey": {"address": p2wsh}, "watchonly": True, "timestamp": "now"}])[0]
        assert_equal(result, {'success': True})
        for address in [p2sh, p2wsh]:
            info = wallet.getaddressinfo(address)
            assert_equal((info['script'], info['hex']), ('witness_v2_p2mr', script.hex()))
            assert 'script' not in info['embedded']
            assert_equal((info['embedded']['witness_version'], info['embedded']['witness_program']), (2, self.tree.merkle_root.hex()))
            assert_equal(info['embedded']['address'], self.p2mr_address)


if __name__ == '__main__':
    WalletP2MRTest().main()
