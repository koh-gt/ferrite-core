#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the signalled activation of the P2MR rules (BIP 360, the p2mr deployment).

Node 0 mines with the default block version, so it signals every started deployment. Node 1
mines with -blockversion=0x20000000, like a pool that does not signal. Both nodes use -vbparams
to let the deployment signal from START_HEIGHT (regtest windows are 144 blocks, 108 of them
lock in). MWEB is switched off, so that the test can build plain blocks.

The test checks what getblockchaininfo and getblocktemplate show in each state of the
deployment, and that the rules apply to exactly the blocks from the activation height on: a
block that spends a P2MR output against the rules is valid below that height and refused from
it on, as a block proposal, through generateblock, through submitblock and from a peer, which
is disconnected. The refused transactions were checked in a block proposal below the activation
height first, and they are refused again in later blocks. Blocks whose P2MR spends follow the
rules stay valid.

The mempool and the wallet look at the next block: a valid P2MR spend is refused as a
non-standard input, and the wallet refuses to pay a P2MR address, until the tip is the last
block below the activation height. After a reorganisation to a chain on which the rules do not
apply to the next block, the mempool keeps a P2MR spend and a payment to a P2MR address that it
already had, drops a P2MR spend from the disconnected blocks, and gives valid blocks.

The P2MR outputs and every spend of them are built here. Node 0 checks scripts without the
script check threads, so that it names the script error of a refused block; node 1 checks them
with a thread and gives the general reason.
"""

from test_framework.address import program_to_witness
from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, add_witness_commitment, create_block
from test_framework.key import compute_xonly_pubkey, sign_schnorr
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, ToHex, sha256
from test_framework.p2p import P2PDataStore
from test_framework.script import (
    CScript,
    OP_0,
    OP_CHECKSIG,
    OP_RETURN,
    OP_TRUE,
    SIGHASH_DEFAULT,
    TaprootSignatureHash,
    p2mr_construct,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

WINDOW = 144
THRESHOLD = 108
START_HEIGHT = WINDOW
TIMEOUT_HEIGHT = 99999999
# The first window does not lock in, the second does, the third is locked in.
ACTIVATION_HEIGHT = START_HEIGHT + 3 * WINDOW
DEPLOYMENT_BIT = 6
NOT_SIGNALLING_VERSION = 0x20000000

OUTPUT_AMOUNT = COIN
FUNDING_FEE = COIN // 100
FEE = 10000

# The key of the leaf that checks a signature. It is used by this test only.
LEAF_KEY = sha256(b"feature_p2mr_activation.py leaf key")

WALLET_REFUSES = "Witness version 2 outputs can be spent by anyone until the p2mr soft fork is active"


class P2MRActivationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.common_args = ["-vbparams=mweb:-2:0"]
        self.signalling_args = self.common_args + ["-vbparams=p2mr:0:0:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT)]
        self.extra_args = [
            self.signalling_args + ["-par=1"],
            self.signalling_args + ["-par=2", "-blockversion={}".format(NOT_SIGNALLING_VERSION)],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def deployment(self):
        return self.nodes[0].getblockchaininfo()['softforks']['p2mr']

    def template(self):
        # Segwit-only callers, as pools use (NORMAL_GBT_REQUEST_PARAMS lists only segwit)
        return self.nodes[0].getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)

    def fund_outputs(self, script_pubkeys):
        """Pay OUTPUT_AMOUNT to each scriptPubKey in one transaction and return the outputs as (outpoint, CTxOut)."""
        node = self.nodes[0]
        needed = len(script_pubkeys) * OUTPUT_AMOUNT + FUNDING_FEE
        coin = next(u for u in node.listunspent() if u['amount'] * COIN > needed)
        change = int(coin['amount'] * COIN) - needed
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(coin['txid'], 16), coin['vout']))]
        tx.vout = [CTxOut(OUTPUT_AMOUNT, script_pubkey) for script_pubkey in script_pubkeys]
        tx.vout.append(CTxOut(change, bytes.fromhex(node.getaddressinfo(node.getrawchangeaddress())['scriptPubKey'])))
        signed = node.signrawtransactionwithwallet(ToHex(tx))
        assert signed['complete']
        txid = int(node.sendrawtransaction(signed['hex']), 16)
        return [(COutPoint(txid, n), tx.vout[n]) for n in range(len(script_pubkeys))]

    def spend(self, utxo, make_witness):
        """Return a transaction that spends the output with the witness make_witness(tx, spent output) gives."""
        outpoint, spent = utxo
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(outpoint, b"", 0xfffffffd)]
        tx.vout = [CTxOut(spent.nValue - FEE, self.payout_script)]
        witness = make_witness(tx, spent)
        if witness:
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = witness
        tx.rehash()
        return tx

    def key_leaf_witness(self, tx, spent):
        leaf = self.tree.leaves["key"]
        sighash = TaprootSignatureHash(tx, [spent], SIGHASH_DEFAULT, 0, scriptpath=True, script=leaf.script)
        return [sign_schnorr(LEAF_KEY, sighash), leaf.script, leaf.control_block]

    def wrong_signature_witness(self, tx, spent):
        witness = self.key_leaf_witness(tx, spent)
        witness[0] = witness[0][:-1] + bytes([witness[0][-1] ^ 1])
        return witness

    def spends_against_the_rules(self):
        """Return (script error, transaction) pairs: spends that break the P2MR rules, each of an output of its own."""
        true_leaf = self.tree.leaves["true"]
        key_leaf = self.tree.leaves["key"]
        root_leaf = self.single_leaf_tree.leaves["only"]
        cases = [
            ("Witness program was passed an empty witness", self.tree_utxos,
             lambda tx, spent: []),
            ("P2MR witness must contain a script and a control block", self.tree_utxos,
             lambda tx, spent: [true_leaf.script]),
            ("Invalid P2MR control block size", self.tree_utxos,
             lambda tx, spent: [true_leaf.script, true_leaf.control_block + b"\x00"]),
            ("P2MR control block must have its lowest bit set", self.tree_utxos,
             lambda tx, spent: [true_leaf.script, bytes([true_leaf.control_block[0] & 0xfe]) + true_leaf.control_block[1:]]),
            ("Invalid Schnorr signature", self.tree_utxos,
             self.wrong_signature_witness),
            # A script that the output does not commit to: with the control block of a leaf at depth 1, and with
            # the one-byte control block of a tree whose only leaf is its root.
            ("Witness program hash mismatch", self.tree_utxos,
             lambda tx, spent: [key_leaf.script, true_leaf.control_block]),
            ("Witness program hash mismatch", self.single_leaf_utxos,
             lambda tx, spent: [CScript([OP_TRUE]), root_leaf.control_block]),
        ]
        return [(error, self.spend(utxos.pop(), make_witness)) for error, utxos, make_witness in cases]

    def spends_by_the_rules(self):
        """Return spends that follow the P2MR rules: a leaf without a signature, a leaf with one, and a tree whose
        only leaf is its root."""
        true_leaf = self.tree.leaves["true"]
        root_leaf = self.single_leaf_tree.leaves["only"]
        return [
            self.spend(self.tree_utxos.pop(), lambda tx, spent: [true_leaf.script, true_leaf.control_block]),
            self.spend(self.tree_utxos.pop(), self.key_leaf_witness),
            self.spend(self.single_leaf_utxos.pop(), lambda tx, spent: [root_leaf.script, root_leaf.control_block]),
        ]

    def block(self, txs, time_offset=0):
        """Build a block on the tip of node 0 with these transactions. Blocks with different time_offset differ."""
        tmpl = self.template()
        block = create_block(tmpl=tmpl, ntime=tmpl['curtime'] + time_offset, txlist=txs)
        add_witness_commitment(block)
        block.solve()
        return block

    def propose(self, block, node=None):
        return (node or self.nodes[0]).getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']})

    def run_test(self):
        node, other_pool = self.nodes
        # The spends pay to a script outside the wallet, so that the wallet's coins are not spent by blocks that a
        # reorganisation disconnects.
        self.payout_script = bytes(CScript([OP_0, sha256(b"feature_p2mr_activation.py payout")]))
        key_script = CScript([compute_xonly_pubkey(LEAF_KEY)[0], OP_CHECKSIG])
        # A tree with two leaves at depth 1, and a tree whose only leaf is its root. The script of that leaf
        # would fail if it were run.
        self.tree = p2mr_construct([("true", CScript([OP_TRUE])), ("key", key_script)])
        self.single_leaf_tree = p2mr_construct([("only", CScript([OP_RETURN]))])
        p2mr_address = program_to_witness(2, self.tree.merkle_root)
        true_leaf = self.tree.leaves["true"]

        self.log.info("Pay to P2MR outputs, then stop two blocks before signalling starts")
        node.generate(20)
        self.tree_utxos = self.fund_outputs([self.tree.scriptPubKey] * 24)
        self.single_leaf_utxos = self.fund_outputs([self.single_leaf_tree.scriptPubKey] * 4)
        node.generate(START_HEIGHT - 2 - node.getblockcount())
        self.sync_blocks()
        assert_equal(node.getrawmempool(), [])
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'defined', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': 0},
            'active': False,
        })
        tmpl = self.template()
        assert 'p2mr' not in tmpl['vbavailable']
        assert 'p2mr' not in tmpl['rules']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)

        self.log.info("Signalling starts at the start height, and templates for segwit-only callers set bit 6")
        node.generate(1)
        self.sync_blocks()
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {
                'status': 'started',
                'bit': DEPLOYMENT_BIT,
                'start_height': START_HEIGHT,
                'timeout_height': TIMEOUT_HEIGHT,
                'since': START_HEIGHT,
                'statistics': {'period': WINDOW, 'threshold': THRESHOLD, 'elapsed': 0, 'count': 0, 'possible': True},
            },
            'active': False,
        })
        tmpl = self.template()
        assert_equal(tmpl['vbavailable']['p2mr'], DEPLOYMENT_BIT)
        assert 'p2mr' not in tmpl['rules']
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("A window with 37 blocks from a pool that does not signal does not lock in")
        other_pool.generate(WINDOW - THRESHOLD + 1)
        self.sync_blocks()
        assert_equal(self.deployment()['bip8']['statistics'],
                     {'period': WINDOW, 'threshold': THRESHOLD, 'elapsed': WINDOW - THRESHOLD + 1, 'count': 0, 'possible': False})
        node.generate(THRESHOLD - 1)
        self.sync_blocks()
        assert_equal(node.getblockcount(), START_HEIGHT + WINDOW - 1)
        info = self.deployment()['bip8']
        assert_equal((info['status'], info['since']), ('started', START_HEIGHT))

        self.log.info("A window in which every block signals locks in; the rules do not apply yet")
        node.generate(WINDOW)
        self.sync_blocks()
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'locked_in', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': START_HEIGHT + 2 * WINDOW,
                     'activation_height': ACTIVATION_HEIGHT},
            'active': False,
        })
        tmpl = self.template()
        assert_equal(tmpl['vbavailable']['p2mr'], DEPLOYMENT_BIT)
        assert 'p2mr' not in tmpl['rules']
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("With start and timeout heights of 0 the same chain is active from block 432")
        assert_equal(node.getblockcount(), 3 * WINDOW - 1)
        self.restart_node(0, extra_args=self.common_args + ["-vbparams=p2mr:0:0", "-par=1"])
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': 0, 'timeout_height': 0, 'since': 3 * WINDOW},
            'height': 3 * WINDOW,
            'active': True,
        })
        self.restart_node(0, extra_args=self.extra_args[0])
        self.connect_nodes(0, 1)
        assert_equal(self.deployment()['bip8']['status'], 'locked_in')

        self.log.info("Blocks below the activation height may spend P2MR outputs against the rules")
        node.generate(ACTIVATION_HEIGHT - 3 - node.getblockcount())
        self.sync_blocks()
        # The block two below the activation height, through generateblock.
        early_spend = self.spend(self.tree_utxos.pop(), lambda tx, spent: [CScript([OP_TRUE])])
        block_hash = node.generateblock(node.getnewaddress(), [ToHex(early_spend)])['hash']
        self.sync_blocks()
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 2)
        assert early_spend.hash in other_pool.getblock(block_hash)['tx']
        # The mempool and the wallet look at the next block, which is still below the activation height.
        valid_spend = self.spend(self.tree_utxos.pop(), lambda tx, spent: [true_leaf.script, true_leaf.control_block])
        for n in self.nodes:
            assert_equal(n.testmempoolaccept([ToHex(valid_spend)])[0]['reject-reason'], 'bad-txns-nonstandard-inputs')
        assert_raises_rpc_error(-4, WALLET_REFUSES, node.fundrawtransaction, node.createrawtransaction([], {p2mr_address: 1}))
        # The last block below the activation height: each spend as a block proposal on both nodes, then all of
        # them in one block.
        early_spends = self.spends_against_the_rules()
        for _, tx in early_spends:
            block = self.block([tx])
            assert_equal(self.propose(block), None)
            assert_equal(self.propose(block, other_pool), None)
        # The spends that are refused from the activation height on are first checked here, in a proposal that is
        # not mined. Node 0 keeps the result of a script check it makes for a block proposal; under the rules that
        # apply from the next block the same transactions must be checked again.
        late_spends = self.spends_against_the_rules()
        assert_equal(self.propose(self.block([tx for _, tx in late_spends])), None)
        block = self.block([tx for _, tx in early_spends])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        self.sync_blocks()
        assert_equal(node.getbestblockhash(), block.hash)
        assert_equal(other_pool.getbestblockhash(), block.hash)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 1)

        self.log.info("The rules apply from the first block after the locked-in window")
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': ACTIVATION_HEIGHT},
            'height': ACTIVATION_HEIGHT,
            'active': True,
        })
        tmpl = self.template()
        assert 'p2mr' in tmpl['rules']
        assert 'p2mr' not in tmpl['vbavailable']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)
        for n in self.nodes:
            assert_equal(n.testmempoolaccept([ToHex(valid_spend)])[0]['allowed'], True)
        node.fundrawtransaction(node.createrawtransaction([], {p2mr_address: 1}))

        self.log.info("A block at the activation height that spends a P2MR output against the rules is refused")
        tip = node.getbestblockhash()
        for error, tx in late_spends:
            reason = "non-mandatory-script-verify-flag ({})".format(error)
            # As a block proposal: node 0 names the script error, node 1 checks scripts in a thread.
            assert_equal(self.propose(self.block([tx])), reason)
            assert_equal(self.propose(self.block([tx]), other_pool), "block-validation-failed")
            assert_raises_rpc_error(-25, "TestBlockValidity failed: " + reason, node.generateblock, node.getnewaddress(), [ToHex(tx)])
            # From a peer, which is disconnected for it.
            peer = node.add_p2p_connection(P2PDataStore())
            peer.send_blocks_and_test([self.block([tx], time_offset=1)], node, success=False, reject_reason=reason, expect_disconnect=True)
            # Through submitblock.
            block = self.block([tx], time_offset=2)
            assert_equal(node.submitblock(block.serialize().hex()), reason)
            assert_equal(node.getbestblockhash(), tip)
            assert_equal([t['status'] for t in node.getchaintips() if t['hash'] == block.hash], ['invalid'])
        # One spend against the rules among spends that follow them is enough.
        mixed = self.spends_by_the_rules()
        block = self.block(mixed + [late_spends[-1][1]])
        assert_equal(self.propose(block), "non-mandatory-script-verify-flag ({})".format(late_spends[-1][0]))
        assert_equal(self.propose(block, other_pool), "block-validation-failed")
        assert_equal(node.getbestblockhash(), tip)
        assert_equal(other_pool.getbestblockhash(), tip)

        self.log.info("Blocks whose P2MR spends follow the rules are valid, with and without the script check threads")
        block = self.block(mixed)
        assert_equal(self.propose(block), None)
        assert_equal(self.propose(block, other_pool), None)
        assert_equal(node.submitblock(block.serialize().hex()), None)
        self.sync_blocks()
        assert_equal(other_pool.getbestblockhash(), block.hash)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT)
        # A block whose only spend with a witness is a P2MR spend that checks a signature, through generateblock.
        with_signature = self.spend(self.tree_utxos.pop(), self.key_leaf_witness)
        block_hash = node.generateblock(node.getnewaddress(), [ToHex(with_signature)])['hash']
        self.sync_blocks()
        assert_equal(other_pool.getblock(block_hash)['tx'][1:], [with_signature.hash])

        self.log.info("The pool that never signalled still mines valid blocks after activation")
        other_pool.generate(2)
        self.sync_blocks()
        assert_equal(node.getbestblockhash(), other_pool.getbestblockhash())
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT + 3)
        assert_equal(self.deployment()['bip8']['status'], 'active')

        self.log.info("Blocks above the activation height that spend P2MR outputs against the rules are refused as well")
        tip = node.getbestblockhash()
        for error, tx in late_spends:
            reason = "non-mandatory-script-verify-flag ({})".format(error)
            block = self.block([tx])
            assert_equal(self.propose(block), reason)
            assert_equal(self.propose(block, other_pool), "block-validation-failed")
            assert_equal(node.submitblock(block.serialize().hex()), reason)
            assert_equal(node.getbestblockhash(), tip)
        assert_equal(other_pool.getbestblockhash(), tip)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT + 3)

        self.log.info("After a reorganisation to a chain without the rules the mempool keeps its P2MR spends and drops those of the disconnected blocks")
        mined_spend = self.spend(self.tree_utxos.pop(), lambda tx, spent: [true_leaf.script, true_leaf.control_block])
        node.sendrawtransaction(ToHex(mined_spend))
        block_hash = node.generate(1)[0]
        assert_equal(node.getblock(block_hash)['tx'][1:], [mined_spend.hash])
        self.sync_blocks()
        tip_height = node.getblockcount()
        self.disconnect_nodes(0, 1)
        unmined_spend = self.spend(self.tree_utxos.pop(), self.key_leaf_witness)
        node.sendrawtransaction(ToHex(unmined_spend))
        # A payment to a P2MR address from a coin that the other chain has as well.
        coin = next(u for u in node.listunspent() if node.getblockcount() + 1 - u['confirmations'] < START_HEIGHT + WINDOW)
        payment = node.send(outputs={p2mr_address: 1}, options={"inputs": [{"txid": coin['txid'], "vout": coin['vout']}], "add_inputs": False})['txid']
        kept = sorted([unmined_spend.hash, payment])
        assert_equal(sorted(node.getrawmempool()), kept)
        # The pool that does not signal builds a longer chain from the end of the first window, on which the
        # deployment does not lock in, with blocks that hold no transactions.
        other_pool.invalidateblock(other_pool.getblockhash(START_HEIGHT + WINDOW))
        address = other_pool.getnewaddress()
        while other_pool.getblockcount() <= tip_height:
            other_pool.generateblock(address, [])
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(self.deployment()['bip8']['status'], 'started')
        assert_equal(sorted(node.getrawmempool()), kept)
        for tx in [mined_spend, late_spends[0][1]]:
            assert_equal(node.testmempoolaccept([ToHex(tx)])[0]['reject-reason'], 'bad-txns-nonstandard-inputs')
        # Without the rules, a block with these transactions is valid.
        block_hash = node.generate(1)[0]
        assert_equal(sorted(node.getblock(block_hash)['tx'][1:]), kept)
        self.sync_blocks()


if __name__ == '__main__':
    P2MRActivationTest().main()
