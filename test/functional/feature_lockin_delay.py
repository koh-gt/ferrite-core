#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test a deployment that stays locked in for more than one window before its rules apply.

The node lets the p2mr deployment signal from START_HEIGHT and stay locked in for three windows
(-vbparams=p2mr:0:0:<start height>:<timeout height>:3; regtest windows are 144 blocks, 108 of them
lock in). MWEB is switched off, so that the test can build plain blocks.

The test checks:
- getblockchaininfo: started, then locked in for exactly three windows with the same since height
  and activation height at every block, then active from the activation height on;
- the block version carries the bit of the deployment for every block until activation, and
  getblocktemplate lists the deployment under rules only from the activation height on;
- a block that spends a P2MR output against the rules is valid one block below the activation
  height and refused at it;
- the mempool and the wallet look at the next block: one window after lock-in and at the tip two
  below the activation height a valid P2MR spend is not standard and the wallet refuses to pay a
  P2MR address; at the tip one below the activation height both are accepted;
- a restart during the locked-in windows gives the same state, since height and activation
  height;
- invalidating a block of the window that locked in returns the deployment to started, and
  reconsidering it restores the locked-in state;
- a number of locked-in windows whose product with the window length does not fit in 32 bits
  keeps the deployment locked in, and getblockchaininfo gives its activation height.
feature_lockin_delay_args.py checks that a malformed number of locked-in windows stops the node.
"""

from test_framework.address import program_to_witness
from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, add_witness_commitment, create_block
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut, ToHex, sha256
from test_framework.script import CScript, OP_0, OP_TRUE, p2mr_construct
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

WINDOW = 144
START_HEIGHT = WINDOW
TIMEOUT_HEIGHT = 99999999
LOCKED_IN_WINDOWS = 3
# Every block signals: the window [START_HEIGHT, START_HEIGHT + WINDOW - 1] locks in, the next three are locked in.
LOCKED_IN_HEIGHT = START_HEIGHT + WINDOW
ACTIVATION_HEIGHT = LOCKED_IN_HEIGHT + LOCKED_IN_WINDOWS * WINDOW
# WINDOW times this number is 9 * 2**32 + 2 * WINDOW: two windows if the product were taken in 32 bits.
LARGE_LOCKED_IN_WINDOWS = 2**28 + 2
DEPLOYMENT_BIT = 6
FEE = 10000
# A spend of a P2MR output whose witness has no control block: valid before the rules apply, invalid afterwards.
INVALID_SPEND_ERROR = "non-mandatory-script-verify-flag (P2MR witness must contain a script and a control block)"
WALLET_REFUSES = "Witness version 2 outputs can be spent by anyone until the p2mr soft fork is active"


class LockInDelayTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.common_args = ["-vbparams=mweb:-2:0", "-par=1"]
        self.extra_args = [self.common_args + ["-vbparams=p2mr:0:0:{}:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT, LOCKED_IN_WINDOWS)]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def deployment(self):
        return self.nodes[0].getblockchaininfo()['softforks']['p2mr']

    def bip8(self, status, since, **fields):
        info = {'status': status, 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': since}
        info.update(fields)
        return info

    def check_locked_in(self):
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': self.bip8('locked_in', LOCKED_IN_HEIGHT, activation_height=ACTIVATION_HEIGHT),
            'active': False,
        })

    def check_template(self, active):
        tmpl = self.nodes[0].getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        assert_equal('p2mr' in tmpl['rules'], active)
        assert_equal('p2mr' in tmpl['vbavailable'], not active)
        assert_equal(bool(tmpl['version'] & (1 << DEPLOYMENT_BIT)), not active)

    def invalid_spend(self, utxo):
        """A transaction that spends the P2MR output with a witness of the leaf script alone."""
        outpoint, spent = utxo
        tx = CTransaction()
        tx.nVersion = 2
        tx.vin = [CTxIn(outpoint, b"", 0xfffffffd)]
        tx.vout = [CTxOut(spent.nValue - FEE, CScript([OP_0, sha256(b"feature_lockin_delay.py payout")]))]
        tx.wit.vtxinwit = [CTxInWitness()]
        tx.wit.vtxinwit[0].scriptWitness.stack = [CScript([OP_TRUE])]
        tx.rehash()
        return tx

    def valid_spend(self, utxo):
        """A transaction that spends the P2MR output by the rules, with the leaf OP_TRUE at depth 1."""
        tx = self.invalid_spend(utxo)
        leaf = self.tree.leaves["true"]
        tx.wit.vtxinwit[0].scriptWitness.stack = [leaf.script, leaf.control_block]
        tx.rehash()
        return tx

    def check_next_block_rules(self, active):
        """The mempool and the wallet apply the rules of the next block."""
        node = self.nodes[0]
        result = node.testmempoolaccept([ToHex(self.valid_spend(self.probe))])[0]
        raw = node.createrawtransaction([], {self.p2mr_address: 1})
        if active:
            assert_equal(result['allowed'], True)
            node.fundrawtransaction(raw)
        else:
            assert_equal(result['reject-reason'], 'bad-txns-nonstandard-inputs')
            assert_raises_rpc_error(-4, WALLET_REFUSES, node.fundrawtransaction, raw)

    def block(self, txs):
        tmpl = self.nodes[0].getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        block = create_block(tmpl=tmpl, txlist=txs)
        add_witness_commitment(block)
        block.solve()
        return block

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Pay to three P2MR outputs before signalling starts")
        node.generate(20)
        # Two leaves at depth 1 (a spend at depth zero is never standard).
        self.tree = tree = p2mr_construct([("true", CScript([OP_TRUE])), ("other", CScript([OP_TRUE, OP_TRUE]))])
        self.p2mr_address = program_to_witness(2, tree.merkle_root)
        # The wallet refuses to pay to P2MR outputs until the rules apply, so the transaction is built here.
        coin = next(u for u in node.listunspent() if u['amount'] * COIN > 3 * COIN)
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(coin['txid'], 16), coin['vout']))]
        tx.vout = [CTxOut(COIN, tree.scriptPubKey), CTxOut(COIN, tree.scriptPubKey), CTxOut(COIN, tree.scriptPubKey),
                   CTxOut(int(coin['amount'] * COIN) - 3 * COIN - FEE, bytes.fromhex(node.getaddressinfo(node.getnewaddress())['scriptPubKey']))]
        signed = node.signrawtransactionwithwallet(ToHex(tx))
        assert signed['complete']
        txid = int(node.sendrawtransaction(signed['hex']), 16)
        utxos = [(COutPoint(txid, n), tx.vout[n]) for n in range(2)]
        self.probe = (COutPoint(txid, 2), tx.vout[2])
        node.generate(START_HEIGHT - 1 - node.getblockcount())
        assert_equal(node.getblockcount(), START_HEIGHT - 1)
        assert_equal(node.getrawmempool(), [])

        self.log.info("Started from the start height; the window locks in")
        assert_equal(self.deployment()['bip8']['status'], 'started')
        assert_equal(self.deployment()['bip8']['since'], START_HEIGHT)
        self.check_template(active=False)
        node.generate(WINDOW - 1)
        assert_equal(self.deployment()['bip8']['status'], 'started')

        self.log.info("Locked in for three windows, with the same since height and activation height at every block")
        node.generate(1)
        restart_height = LOCKED_IN_HEIGHT + WINDOW + 10
        while node.getblockcount() < ACTIVATION_HEIGHT - 2:
            height = node.getblockcount()
            self.check_locked_in()
            self.check_template(active=False)
            if height == LOCKED_IN_HEIGHT + WINDOW - 1:
                self.log.info("One window after lock-in the mempool and the wallet do not apply the rules yet")
                self.check_next_block_rules(active=False)
            if height == restart_height:
                self.log.info("A restart during the locked-in windows gives the same state")
                self.restart_node(0)
                self.check_locked_in()
                self.check_template(active=False)
            node.generate(1)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 2)
        self.check_locked_in()
        self.log.info("Nor at the tip two below the activation height")
        self.check_next_block_rules(active=False)
        # Every block from the start height on signals, the locked-in windows included.
        for height in range(START_HEIGHT, ACTIVATION_HEIGHT - 1):
            assert node.getblockheader(node.getblockhash(height))['version'] & (1 << DEPLOYMENT_BIT), height
        assert_equal(node.getblockheader(node.getblockhash(START_HEIGHT - 1))['version'] & (1 << DEPLOYMENT_BIT), 0)

        self.log.info("Invalidating a block of the window that locked in returns the deployment to started")
        tip = node.getbestblockhash()
        last_signalling = node.getblockhash(LOCKED_IN_HEIGHT - 1)
        first_locked_in = node.getblockhash(LOCKED_IN_HEIGHT)
        node.invalidateblock(last_signalling)
        assert_equal(node.getblockcount(), LOCKED_IN_HEIGHT - 2)
        info = self.deployment()['bip8']
        assert_equal((info['status'], info['since']), ('started', START_HEIGHT))
        assert 'activation_height' not in info
        node.reconsiderblock(last_signalling)
        assert_equal(node.getbestblockhash(), tip)
        self.check_locked_in()
        # Invalidating a block of the locked-in windows changes nothing: the lock-in depends on earlier blocks only.
        node.invalidateblock(first_locked_in)
        assert_equal(node.getblockcount(), LOCKED_IN_HEIGHT - 1)
        self.check_locked_in()
        node.reconsiderblock(first_locked_in)
        assert_equal(node.getbestblockhash(), tip)

        self.log.info("A block one below the activation height may spend a P2MR output against the rules")
        block = self.block([self.invalid_spend(utxos[0])])
        assert_equal(node.submitblock(block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), block.hash)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 1)
        assert node.getblockheader(block.hash)['version'] & (1 << DEPLOYMENT_BIT)
        self.log.info("At the tip one below the activation height the mempool and the wallet apply the rules")
        self.check_next_block_rules(active=True)

        self.log.info("The rules apply from the activation height on")
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': self.bip8('active', ACTIVATION_HEIGHT),
            'height': ACTIVATION_HEIGHT,
            'active': True,
        })
        self.check_template(active=True)
        block = self.block([self.invalid_spend(utxos[1])])
        assert_equal(node.submitblock(block.serialize().hex()), INVALID_SPEND_ERROR)
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT - 1)
        node.generate(1)
        assert_equal(node.getblockheader(node.getbestblockhash())['version'] & (1 << DEPLOYMENT_BIT), 0)
        assert_equal(self.deployment()['bip8']['since'], ACTIVATION_HEIGHT)

        self.stop_node(0)
        self.log.info("A number of locked-in windows whose product with the window length does not fit in 32 bits")
        self.start_node(0, extra_args=self.common_args + ["-vbparams=p2mr:0:0:{}:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT, LARGE_LOCKED_IN_WINDOWS)])
        assert_equal(node.getblockcount(), ACTIVATION_HEIGHT)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': self.bip8('locked_in', LOCKED_IN_HEIGHT, activation_height=LOCKED_IN_HEIGHT + LARGE_LOCKED_IN_WINDOWS * WINDOW),
            'active': False,
        })
        self.stop_node(0)
        self.start_node(0)
        assert_equal(self.deployment()['bip8']['status'], 'active')


if __name__ == '__main__':
    LockInDelayTest().main()
