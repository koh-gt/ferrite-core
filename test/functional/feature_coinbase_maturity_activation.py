#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the signalled activation of coinbase maturity 100 (the coinbasematurity deployment).

Node 0 mines with the default block version, so it signals every started deployment. Node 1
mines with -blockversion=0x20000000, like a pool that does not signal. Both use -vbparams to let
the deployment signal from START_HEIGHT (regtest windows are 144 blocks, 108 of them lock in) and
stay locked in for the number of windows that --locked-in-windows gives (1 by default; CI runs
the test with 1 and with 3).
Node 2 runs with the default parameters, on which the deployment is never active: it decides as a
node without the rule does. MWEB is switched off, so that the test can build plain blocks.

The test checks what getblockchaininfo and getblocktemplate show in each state of the deployment,
and the boundaries of the rule around the activation height A: the coinbase of block A-1 can be
spent from block A+9, those of blocks A and A+1 only 100 blocks after their block, through the
mempool, generateblock, submitblock and from a peer, which is disconnected for a block that spends
too early. Node 2 accepts such spends and the block, and follows the longer chain of node 0
afterwards; its wallet counts a coinbase of the new rule as mature after 11 confirmations, where
node 0's counts it at 101. A transaction in the mempool when the rule activates stays valid and is
mined, also across a restart. While the deployment is locked in, a coinbase needs 10 blocks.

Reorganisations: onto a chain on which the deployment activated one window earlier, the mempool
drops a spend of a coinbase that was created between the two activation heights and is no longer
mature (a reorganisation of the same depth that keeps the activation height keeps it), and back
onto the first chain the spend is valid again. The coinbase spent is the same transaction on every
branch, so that only the maturity rule can remove the spend. A restart and a restart with -reindex
keep the activation height and the chain.
"""

from decimal import Decimal

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, add_witness_commitment, create_block
from test_framework.messages import CTransaction, FromHex
from test_framework.p2p import P2PDataStore
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

WINDOW = 144
THRESHOLD = 108
START_HEIGHT = WINDOW
TIMEOUT_HEIGHT = 99999999
# The first window does not lock in, the second does: locked in from this height.
LOCKED_IN_HEIGHT = START_HEIGHT + 2 * WINDOW
DEPLOYMENT_BIT = 7
NOT_SIGNALLING_VERSION = 0x20000000
# The last block of the first window that both branches share.
FORK_HEIGHT = START_HEIGHT + THRESHOLD - 2

PREMATURE = "bad-txns-premature-spend-of-coinbase"
FEE = Decimal("0.001")


class CoinbaseMaturityActivationTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--locked-in-windows", dest="locked_in_windows", type=int, default=1,
                            help="number of windows the deployment stays locked in (default: 1)")

    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        locked_in_windows = self.options.locked_in_windows
        # The rule applies from the activation height A, after the locked-in windows. On the other branch the first
        # window locks in, and the rule applies one window earlier.
        self.activation_height = LOCKED_IN_HEIGHT + locked_in_windows * WINDOW
        self.early_activation_height = self.activation_height - WINDOW
        self.common_args = ["-vbparams=mweb:-2:0"]
        self.signalling_args = self.common_args + ["-vbparams=coinbasematurity:0:0:{}:{}:{}".format(START_HEIGHT, TIMEOUT_HEIGHT, locked_in_windows)]
        self.not_signalling_args = self.signalling_args + ["-blockversion={}".format(NOT_SIGNALLING_VERSION)]
        self.extra_args = [self.signalling_args, self.not_signalling_args, self.common_args]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes(1, 0)
        self.sync_all()

    def sync_all_blocks(self):
        """Sync node 2 with the others. Node 2 stays disconnected otherwise: node 0 announces a block to its peers
        before it has checked the block's transactions, and node 2 would follow the blocks node 0 refuses."""
        self.connect_nodes(2, 0)
        self.sync_blocks()
        self.disconnect_nodes(2, 0)

    def deployment(self, node=None):
        return (node or self.nodes[0]).getblockchaininfo()['softforks']['coinbasematurity']

    def template(self):
        # Segwit-only callers, as pools use (NORMAL_GBT_REQUEST_PARAMS lists only segwit)
        return self.nodes[0].getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)

    def mine_to(self, height, node=None):
        node = node or self.nodes[0]
        node.generatetoaddress(height - node.getblockcount(), self.address)

    def empty_blocks_to(self, height, node, address=None):
        """Mine empty blocks paying to self.address unless another address is given. An empty block that pays there
        has the same coinbase transaction as any other at its height, whichever node builds it on whichever branch."""
        while node.getblockcount() < height:
            node.generateblock(address or self.address, [])

    def coinbase(self, height, node=None):
        node = node or self.nodes[0]
        return node.getblock(node.getblockhash(height), 2)['tx'][0]

    def coinbase_spend(self, height, wallet=None):
        """Return a signed transaction (hex) that spends the coinbase output of the block at this height."""
        wallet = wallet or self.nodes[0]
        coinbase = self.coinbase(height)
        value = coinbase['vout'][0]['value']
        raw = wallet.createrawtransaction([{'txid': coinbase['txid'], 'vout': 0}], {self.payout_address: value - FEE})
        prevtx = [{'txid': coinbase['txid'], 'vout': 0, 'scriptPubKey': coinbase['vout'][0]['scriptPubKey']['hex'], 'amount': value}]
        signed = wallet.signrawtransactionwithwallet(raw, prevtx)
        assert signed['complete']
        return signed['hex']

    def txid(self, tx_hex):
        return self.nodes[0].decoderawtransaction(tx_hex)['txid']

    def block_with(self, tx_hex):
        tmpl = self.template()
        block = create_block(tmpl=tmpl, txlist=[FromHex(CTransaction(), tx_hex)])
        add_witness_commitment(block)
        block.solve()
        return block

    def check_refused(self, tx_hex, depth):
        """The spend is refused by node 0 in the mempool and in blocks, with the texts each RPC gives."""
        node = self.nodes[0]
        tip = node.getbestblockhash()
        assert_equal(node.testmempoolaccept([tx_hex])[0]['reject-reason'], PREMATURE)
        message = "{}, tried to spend coinbase at depth {}".format(PREMATURE, depth)
        assert_raises_rpc_error(-26, message, node.sendrawtransaction, tx_hex)
        assert_raises_rpc_error(-25, "TestBlockValidity failed: " + message, node.generateblock, self.address, [tx_hex])
        block = self.block_with(tx_hex)
        assert_equal(node.getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']}), PREMATURE)
        assert_equal(node.submitblock(block.serialize().hex()), PREMATURE)
        assert_equal(node.getbestblockhash(), tip)

    def check_accepted(self, tx_hex):
        """The spend is accepted by node 0's mempool and mined by node 0 in the next block."""
        node = self.nodes[0]
        result = node.testmempoolaccept([tx_hex])[0]
        assert result['allowed'], result
        node.sendrawtransaction(tx_hex)
        block_hash = node.generatetoaddress(1, self.address)[0]
        assert self.txid(tx_hex) in node.getblock(block_hash)['tx']

    def wallet_category(self, node, txid):
        tx = node.gettransaction(txid)
        assert tx['generated']
        return tx['confirmations'], tx['details'][0]['category']

    def run_test(self):
        node, other_pool, old = self.nodes
        self.address = node.getnewaddress()
        self.payout_address = node.getnewaddress()

        self.log.info("Before signalling: the deployment is defined, and the node without it does not list it")
        self.mine_to(START_HEIGHT - 2)
        self.sync_all_blocks()
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'defined', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': 0},
            'active': False,
        })
        assert 'coinbasematurity' not in old.getblockchaininfo()['softforks']
        tmpl = self.template()
        assert 'coinbasematurity' not in tmpl['vbavailable']
        assert 'coinbasematurity' not in tmpl['rules']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)

        self.log.info("Signalling starts at the start height, and templates for segwit-only callers set bit 7")
        self.mine_to(START_HEIGHT - 1)
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
        assert_equal(tmpl['vbavailable']['coinbasematurity'], DEPLOYMENT_BIT)
        assert 'coinbasematurity' not in tmpl['rules']
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("A window whose last 37 blocks come from a pool that does not signal does not lock in")
        self.mine_to(FORK_HEIGHT)
        self.sync_all_blocks()
        other_pool.generatetoaddress(WINDOW - THRESHOLD + 1, self.address)
        self.sync_all_blocks()
        assert_equal(node.getblockcount(), START_HEIGHT + WINDOW - 1)
        assert_equal(self.deployment()['bip8']['status'], 'started')
        assert_equal(node.getblockheader(node.getblockhash(FORK_HEIGHT + 1))['version'] & (1 << DEPLOYMENT_BIT), 0)
        assert node.getblockheader(node.getblockhash(FORK_HEIGHT))['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("A window in which every block signals locks in; the rule does not apply yet")
        self.mine_to(LOCKED_IN_HEIGHT - 1)
        self.sync_all_blocks()
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'locked_in', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': LOCKED_IN_HEIGHT,
                     'activation_height': self.activation_height},
            'active': False,
        })
        tmpl = self.template()
        assert_equal(tmpl['vbavailable']['coinbasematurity'], DEPLOYMENT_BIT)
        assert 'coinbasematurity' not in tmpl['rules']
        assert tmpl['version'] & (1 << DEPLOYMENT_BIT)

        self.log.info("While the deployment is locked in, a coinbase needs 10 blocks")
        self.mine_to(LOCKED_IN_HEIGHT + 14)
        assert_equal(self.deployment()['bip8']['status'], 'locked_in')
        # In the next block, the coinbase of LOCKED_IN_HEIGHT + 5 is at depth 10, that of LOCKED_IN_HEIGHT + 6 at depth 9.
        assert_equal(node.testmempoolaccept([self.coinbase_spend(LOCKED_IN_HEIGHT + 5)])[0]['allowed'], True)
        assert_equal(node.testmempoolaccept([self.coinbase_spend(LOCKED_IN_HEIGHT + 6)])[0]['reject-reason'], PREMATURE)

        self.log.info("A transaction in the mempool when the rule activates stays valid, also across a restart")
        # Tip A-2: a spend of the coinbase of block A-11 at depth 10 in block A-1.
        self.mine_to(self.activation_height - 2)
        before_activation = self.coinbase_spend(self.activation_height - 11)
        node.sendrawtransaction(before_activation)
        self.sync_mempools([node, other_pool])
        # Block A-1 without it, so that it is still in the mempool when the rule applies to the next block.
        node.generateblock(self.address, [])
        self.sync_all_blocks()
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': self.activation_height},
            'height': self.activation_height,
            'active': True,
        })
        tmpl = self.template()
        assert 'coinbasematurity' in tmpl['rules']
        assert 'coinbasematurity' not in tmpl['vbavailable']
        assert_equal(tmpl['version'] & (1 << DEPLOYMENT_BIT), 0)
        assert_equal(node.getrawmempool(), [self.txid(before_activation)])
        self.restart_node(0, extra_args=self.signalling_args)
        self.connect_nodes(1, 0)
        self.wait_until(lambda: node.getmempoolinfo()['loaded'])
        assert_equal(node.getrawmempool(), [self.txid(before_activation)])
        assert_equal(self.deployment()['bip8']['since'], self.activation_height)
        # A spend of the coinbase of block A-10 at depth 10 in block A: that coinbase was created below A.
        at_activation = self.coinbase_spend(self.activation_height - 10)
        node.sendrawtransaction(at_activation)
        block_hash = node.generatetoaddress(1, self.address)[0]
        assert_equal(sorted(node.getblock(block_hash)['tx'][1:]), sorted([self.txid(before_activation), self.txid(at_activation)]))
        self.sync_all_blocks()
        assert_equal(node.getrawmempool(), [])

        self.log.info("The coinbase of block A-1 keeps maturity 10")
        # Block A+1 pays to node 2's wallet.
        old_address = old.getnewaddress()
        node.generatetoaddress(1, old_address)
        old_coinbase = self.coinbase(self.activation_height + 1)['txid']
        self.mine_to(self.activation_height + 7)
        self.sync_all_blocks()
        # Tip A+7: the coinbase of A-1 is at depth 9 in block A+8.
        assert_equal(node.testmempoolaccept([self.coinbase_spend(self.activation_height - 1)])[0]['reject-reason'], PREMATURE)
        self.mine_to(self.activation_height + 8)
        self.check_accepted(self.coinbase_spend(self.activation_height - 1))
        # Tip A+9: in node 0's wallet the coinbase of A-1 is mature with 11 confirmations, that of A is not.
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height - 2)['txid']), (12, 'generate'))
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height)['txid']), (10, 'immature'))
        self.sync_all_blocks()

        self.log.info("The coinbase of block A needs 100 blocks; the node without the rule accepts it at depth 10")
        # Tip A+9: the coinbase of A at depth 10 in block A+10.
        early_spend = self.coinbase_spend(self.activation_height)
        self.check_refused(early_spend, 10)
        assert_equal(old.testmempoolaccept([early_spend])[0]['allowed'], True)
        node.generatetoaddress(1, self.address)
        self.sync_all_blocks()
        # Tip A+10: with 11 confirmations the coinbase of A is still immature in node 0's wallet.
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height)['txid']), (11, 'immature'))
        assert self.coinbase(self.activation_height)['txid'] not in [u['txid'] for u in node.listunspent()]
        assert self.coinbase(self.activation_height - 2)['txid'] in [u['txid'] for u in node.listunspent()]
        # In the wallet of node 2 the coinbase of A+1 is mature after 11 confirmations, and the wallet spends it.
        assert_equal(self.wallet_category(old, old_coinbase), (10, 'immature'))
        node.generatetoaddress(1, self.address)
        self.sync_all_blocks()
        assert_equal(self.wallet_category(old, old_coinbase), (11, 'generate'))
        assert_equal(old.getbalances()['mine']['immature'], 0)
        old_spend = old.sendtoaddress(old.getnewaddress(), 1)
        old_spend_hex = old.gettransaction(old_spend)['hex']
        assert_equal(old.getrawtransaction(old_spend, True)['vin'][0]['txid'], old_coinbase)
        assert_equal(node.testmempoolaccept([old_spend_hex])[0]['reject-reason'], PREMATURE)
        assert old_spend not in node.getrawmempool()
        # The block that node 2 would build with the spend of the coinbase of A is refused by node 0, also from a peer,
        # which is disconnected.
        tip = node.getbestblockhash()
        block = self.block_with(early_spend)
        peer = node.add_p2p_connection(P2PDataStore())
        peer.send_blocks_and_test([block], node, success=False, reject_reason=PREMATURE, expect_disconnect=True)
        assert_equal(node.getbestblockhash(), tip)
        assert_equal(old.submitblock(block.serialize().hex()), None)
        assert_equal(old.getbestblockhash(), block.hash)
        # Node 2 follows node 0's longer chain.
        self.mine_to(node.getblockcount() + 2)
        self.sync_all_blocks()
        assert_equal(old.getbestblockhash(), node.getbestblockhash())

        self.log.info("A reorganisation that keeps the activation height keeps a spend of a coinbase created after it")
        # Block A-16 is empty on every branch, so its coinbase is the same transaction everywhere. It was created
        # below A and above the activation height of the other branch.
        reorg_coinbase_height = self.activation_height - 16
        assert_equal(len(node.getblock(node.getblockhash(reorg_coinbase_height))['tx']), 1)
        main_tip = node.getblockcount()
        reorg_spend = self.coinbase_spend(reorg_coinbase_height)
        node.sendrawtransaction(reorg_spend)
        self.disconnect_nodes(1, 0)
        # The other pool builds a longer chain from the fork height with empty blocks: without signalling in the
        # rest of the first window, and with signalling from the second, so that the activation height is the same.
        other_pool.invalidateblock(other_pool.getblockhash(FORK_HEIGHT + 1))
        # These blocks pay elsewhere, so that they differ from the blocks of the first chain that were invalidated.
        self.empty_blocks_to(START_HEIGHT + WINDOW - 1, other_pool, self.payout_address)
        self.restart_node(1, extra_args=self.signalling_args)
        self.empty_blocks_to(main_tip + 1, other_pool)
        assert_equal(self.deployment(other_pool)['bip8']['since'], self.activation_height)
        self.connect_nodes(1, 0)
        self.sync_blocks([node, other_pool])
        assert_equal(node.getblockcount(), main_tip + 1)
        assert_equal(self.deployment()['bip8']['since'], self.activation_height)
        assert_equal(self.coinbase(reorg_coinbase_height)['txid'], self.coinbase(reorg_coinbase_height, old)['txid'])
        # The spends of coinbases from below A that the disconnected blocks held are valid on this chain as well,
        # since those coinbases are the same here, and are back in the mempool.
        mempool = node.getrawmempool()
        assert self.txid(reorg_spend) in mempool
        assert self.txid(before_activation) in mempool
        assert self.txid(at_activation) in mempool

        self.log.info("A reorganisation onto a chain with an earlier activation height drops the spend")
        self.disconnect_nodes(1, 0)
        # The same pool, signalling, builds a longer chain on which the first window locks in.
        other_pool.invalidateblock(other_pool.getblockhash(FORK_HEIGHT + 1))
        self.empty_blocks_to(main_tip + 2, other_pool)
        assert_equal(self.deployment(other_pool)['bip8']['since'], self.early_activation_height)
        self.connect_nodes(1, 0)
        self.sync_blocks([node, other_pool])
        assert_equal(node.getblockcount(), main_tip + 2)
        assert_equal(self.deployment(), {
            'type': 'bip8',
            'bip8': {'status': 'active', 'start_height': START_HEIGHT, 'timeout_height': TIMEOUT_HEIGHT, 'since': self.early_activation_height},
            'height': self.early_activation_height,
            'active': True,
        })
        assert_equal(self.coinbase(reorg_coinbase_height)['txid'], self.coinbase(reorg_coinbase_height, old)['txid'])
        # All of them spend coinbases created between the two activation heights, less than 100 blocks deep.
        assert_equal(node.getrawmempool(), [])
        depth = node.getblockcount() + 1 - reorg_coinbase_height
        assert depth < 100
        self.check_refused(reorg_spend, depth)
        # The wallet follows: the coinbase of A-16, mature on the first chain, is immature here.
        assert_equal(self.wallet_category(node, self.coinbase(reorg_coinbase_height)['txid'])[1], 'immature')
        self.restart_node(0, extra_args=self.signalling_args)
        assert_equal(self.deployment()['bip8']['since'], self.early_activation_height)
        assert_equal(node.testmempoolaccept([reorg_spend])[0]['reject-reason'], PREMATURE)

        self.log.info("Back onto the first chain the spend is valid again")
        # Node 2, still on the first chain, makes it the longest.
        self.empty_blocks_to(main_tip + 3, old)
        self.connect_nodes(2, 0)
        self.sync_blocks([node, old])
        self.disconnect_nodes(2, 0)
        assert_equal(self.deployment()['bip8']['since'], self.activation_height)
        assert_equal(self.wallet_category(node, self.coinbase(reorg_coinbase_height)['txid'])[1], 'generate')
        self.restart_node(0, extra_args=self.signalling_args)
        assert_equal(self.deployment()['bip8']['since'], self.activation_height)
        # The wallet of node 0 gives its transaction to the mempool again at start-up, which now accepts it.
        self.wait_until(lambda: self.txid(reorg_spend) in node.getrawmempool())
        block_hash = node.generatetoaddress(1, self.address)[0]
        assert self.txid(reorg_spend) in node.getblock(block_hash)['tx']
        other_pool.reconsiderblock(node.getblockhash(FORK_HEIGHT + 1))
        self.connect_nodes(1, 0)
        self.sync_all_blocks()

        self.log.info("The coinbases of blocks A and A+1 can be spent 100 blocks after their block")
        self.mine_to(self.activation_height + 98)
        # Tip A+98: the coinbase of A at depth 99, that of A+1 at depth 98.
        self.check_refused(self.coinbase_spend(self.activation_height), 99)
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height)['txid']), (99, 'immature'))
        node.generatetoaddress(1, self.address)
        # Tip A+99: the coinbase of A at depth 100; that of A+1 at depth 99, in node 2's wallet.
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height)['txid']), (100, 'immature'))
        old_spend_of_a_plus_1 = self.coinbase_spend(self.activation_height + 1, old)
        self.check_refused(old_spend_of_a_plus_1, 99)
        self.check_accepted(self.coinbase_spend(self.activation_height))
        # Tip A+100.
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height)['txid']), (101, 'generate'))
        assert_equal(self.wallet_category(node, self.coinbase(self.activation_height + 2)['txid']), (99, 'immature'))
        self.sync_all_blocks()
        # Node 2's own spend of that coinbase, from its mempool, is now valid for node 0 too.
        assert_equal(node.testmempoolaccept([old_spend_hex])[0]['allowed'], True)
        if old_spend not in node.getrawmempool():
            node.sendrawtransaction(old_spend_hex)
        block_hash = node.generatetoaddress(1, self.address)[0]
        assert old_spend in node.getblock(block_hash)['tx']
        self.sync_all_blocks()

        self.log.info("A restart with -reindex validates the same chain")
        tip = node.getbestblockhash()
        self.stop_node(2)
        self.restart_node(0, extra_args=self.signalling_args + ["-reindex"])
        self.wait_until(lambda: node.getbestblockhash() == tip)
        assert_equal(self.deployment()['bip8']['since'], self.activation_height)


if __name__ == '__main__':
    CoinbaseMaturityActivationTest().main()
