#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test coinbase maturity 100 (the coinbasematurity deployment) while it is always active.

Node 0 runs with -vbparams=coinbasematurity:-1:0, so every coinbase output needs 100 blocks. Node 1
runs with the default parameters, on which the deployment is never active and every coinbase output
needs 10 blocks, as before the deployment.

The test checks the results of the mempool and of blocks for spends of a coinbase output at depths
99 and 100, through testmempoolaccept, sendrawtransaction, generateblock, block proposals and
submitblock; that node 1 accepts the same spends from depth 10; and that node 0 refuses a block from
node 1 that spends a coinbase output at depth 50. It checks the wallet of node 0: balances, listunspent,
coin selection and the "immature" and "generate" categories, until 101 confirmations. Once MWEB is
active, peg-out outputs keep their maturity of 6 blocks, while the coinbase of the block that holds
them needs 100.
"""

from decimal import Decimal

from test_framework.blocktools import NORMAL_GBT_REQUEST_PARAMS, add_witness_commitment, create_block
from test_framework.fec_util import get_hogex_tx
from test_framework.messages import COIN, CTransaction, FromHex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

PREMATURE = "bad-txns-premature-spend-of-coinbase"
FEE = Decimal("0.001")


class CoinbaseMaturityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-vbparams=coinbasematurity:-1:0"], []]

    def setup_network(self):
        self.setup_nodes()

    def sync_old(self):
        """Sync node 1 with node 0. Node 1 stays disconnected otherwise: node 0 announces a block to its peers before
        it has checked the block's transactions, and node 1 would follow the blocks node 0 refuses."""
        self.connect_nodes(1, 0)
        self.sync_blocks()
        self.disconnect_nodes(1, 0)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def coinbase_spend(self, height):
        """Return a signed transaction (hex) that spends the coinbase output of the block at this height."""
        node = self.nodes[0]
        coinbase = node.getblock(node.getblockhash(height), 2)['tx'][0]
        value = coinbase['vout'][0]['value']
        raw = node.createrawtransaction([{'txid': coinbase['txid'], 'vout': 0}], {self.payout_address: value - FEE})
        prevtx = [{'txid': coinbase['txid'], 'vout': 0, 'scriptPubKey': coinbase['vout'][0]['scriptPubKey']['hex'], 'amount': value}]
        signed = self.wallet.signrawtransactionwithwallet(raw, prevtx)
        assert signed['complete']
        return signed['hex']

    def block_with(self, node, tx_hex):
        tmpl = node.getblocktemplate(NORMAL_GBT_REQUEST_PARAMS)
        tx = FromHex(CTransaction(), tx_hex)
        block = create_block(tmpl=tmpl, txlist=[tx])
        add_witness_commitment(block)
        block.solve()
        return block

    def check_premature(self, tx_hex, depth, blocks=True):
        """The spend is refused in the mempool and, unless blocks is False, in blocks (the test builds blocks only
        before MWEB is active)."""
        node = self.nodes[0]
        tip = node.getbestblockhash()
        assert_equal(node.testmempoolaccept([tx_hex])[0]['reject-reason'], PREMATURE)
        message = "{}, tried to spend coinbase at depth {}".format(PREMATURE, depth)
        assert_raises_rpc_error(-26, message, node.sendrawtransaction, tx_hex)
        if not blocks:
            return
        assert_raises_rpc_error(-25, "TestBlockValidity failed: " + message, node.generateblock, self.payout_address, [tx_hex])
        block = self.block_with(node, tx_hex)
        assert_equal(node.getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']}), PREMATURE)
        assert_equal(node.submitblock(block.serialize().hex()), PREMATURE)
        assert_equal(node.getbestblockhash(), tip)

    def balances(self):
        return self.nodes[0].getbalances()['mine']

    def run_test(self):
        node, old = self.nodes
        self.wallet = node
        self.payout_address = node.getnewaddress()
        mining_address = node.getnewaddress()

        assert_equal(node.getblockchaininfo()['softforks']['coinbasematurity'], {
            'type': 'bip9',
            'bip9': {'status': 'active', 'start_time': -1, 'timeout': 0, 'since': 0},
            'height': 0,
            'active': True,
        })
        assert 'coinbasematurity' not in old.getblockchaininfo()['softforks']

        self.log.info("The wallet counts a coinbase as immature until it has 101 confirmations")
        node.generatetoaddress(1, mining_address)
        reward = node.getblock(node.getblockhash(1), 2)['tx'][0]['vout'][0]['value']
        coinbase_txid = node.getblock(node.getblockhash(1))['tx'][0]
        node.generatetoaddress(99, mining_address)
        self.sync_old()
        # Tip 100: the coinbase of block 1 has 100 confirmations.
        assert_equal(node.getblockcount(), 100)
        tx = node.gettransaction(coinbase_txid)
        assert_equal((tx['confirmations'], tx['generated'], tx['details'][0]['category']), (100, True, 'immature'))
        assert_equal(self.balances()['trusted'], 0)
        assert_equal(self.balances()['immature'], 100 * reward)
        assert_equal(node.getbalance(), 0)
        assert_equal(node.getwalletinfo()['immature_balance'], 100 * reward)
        assert_equal(node.listunspent(), [])
        assert_raises_rpc_error(-6, "Insufficient funds", node.sendtoaddress, self.payout_address, 1)
        assert_equal([t['category'] for t in node.listtransactions("*", 1000) if t['txid'] == coinbase_txid], ['immature'])
        # With the old rule a coinbase can be spent at depth 10: node 1 accepts such a spend.
        assert_equal(old.testmempoolaccept([self.coinbase_spend(91)])[0]['allowed'], True)

        node.generatetoaddress(1, mining_address)
        tx = node.gettransaction(coinbase_txid)
        assert_equal((tx['confirmations'], tx['generated'], tx['details'][0]['category']), (101, True, 'generate'))
        assert_equal(self.balances()['trusted'], reward)
        assert_equal(self.balances()['immature'], 100 * reward)
        assert_equal(node.getbalance(), reward)
        assert_equal([(u['txid'], u['confirmations']) for u in node.listunspent()], [(coinbase_txid, 101)])
        assert_equal([t['category'] for t in node.listtransactions("*", 1000) if t['txid'] == coinbase_txid], ['generate'])
        # Coin selection uses the mature coinbase only.
        assert_raises_rpc_error(-6, "Insufficient funds", node.sendtoaddress, self.payout_address, reward + 1)
        sent = node.sendtoaddress(self.payout_address, 1)
        assert_equal([i['txid'] for i in node.getrawtransaction(sent, True)['vin']], [coinbase_txid])
        node.generatetoaddress(1, mining_address)
        self.sync_old()

        self.log.info("A coinbase output can be spent at depth 100 and not at depth 99")
        # Tip 102: the next block is 103. The coinbase of block 4 is at depth 99 there, that of block 3 at depth 100.
        self.check_premature(self.coinbase_spend(4), 99)
        at_100 = self.coinbase_spend(3)
        assert_equal(node.testmempoolaccept([at_100])[0]['allowed'], True)
        block = self.block_with(node, at_100)
        assert_equal(node.getblocktemplate({'data': block.serialize().hex(), 'mode': 'proposal', 'rules': ['segwit']}), None)
        node.sendrawtransaction(at_100)
        # The miner's block includes it.
        block_hash = node.generatetoaddress(1, mining_address)[0]
        assert_equal(node.getblock(block_hash)['tx'][1:], [node.decoderawtransaction(at_100)['txid']])
        self.sync_old()
        assert_equal(node.getrawmempool(), [])
        # At depth 10, with the old rule, node 1 accepts the spend that node 0 refuses.
        at_10 = self.coinbase_spend(node.getblockcount() + 1 - 10)
        self.check_premature(at_10, 10)
        assert_equal(old.testmempoolaccept([at_10])[0]['allowed'], True)

        self.log.info("A block that spends a coinbase output at depth 50 is valid with the old rule only")
        tip = node.getbestblockhash()
        early = self.coinbase_spend(node.getblockcount() + 1 - 50)
        old_block = old.generateblock(old.getnewaddress(), [early])['hash']
        assert_equal(old.getbestblockhash(), old_block)
        assert_equal(node.submitblock(old.getblock(old_block, 0)), PREMATURE)
        assert_equal(node.getbestblockhash(), tip)
        assert_equal([t['status'] for t in node.getchaintips() if t['hash'] == old_block], ['invalid'])
        # The old node follows the longer chain of node 0 again.
        node.generatetoaddress(2, mining_address)
        self.sync_old()

        self.log.info("Peg-out outputs keep maturity 6")
        # The first MWEB block (432) needs a peg-in.
        node.generatetoaddress(431 - node.getblockcount(), mining_address)
        node.createwallet("mweb")
        mweb_wallet = node.get_wallet_rpc("mweb")
        default_wallet = node.get_wallet_rpc(self.default_wallet_name)
        self.wallet = default_wallet
        default_wallet.sendtoaddress(mweb_wallet.getnewaddress(address_type='mweb'), 10)
        node.generatetoaddress(1, mining_address)
        pegout_address = default_wallet.getnewaddress()
        mweb_wallet.sendtoaddress(pegout_address, 5)
        pegout_block = node.generatetoaddress(1, mining_address)[0]
        pegout_height = node.getblockcount()
        hogex = get_hogex_tx(node, pegout_block)
        pegout_script = default_wallet.getaddressinfo(pegout_address)['scriptPubKey']
        n = next(i for i, out in enumerate(hogex.vout) if i > 0 and out.scriptPubKey.hex() == pegout_script)
        value = Decimal(hogex.vout[n].nValue) / COIN
        raw = node.createrawtransaction([{'txid': hogex.hash, 'vout': n}], {self.payout_address: value - FEE})
        prevtx = [{'txid': hogex.hash, 'vout': n, 'scriptPubKey': pegout_script, 'amount': value}]
        pegout_spend = default_wallet.signrawtransactionwithwallet(raw, prevtx)
        assert pegout_spend['complete']
        node.generatetoaddress(4, mining_address)
        # Depth 5 at the next block.
        assert_equal(node.testmempoolaccept([pegout_spend['hex']])[0]['reject-reason'], "bad-txns-premature-spend-of-pegout")
        node.generatetoaddress(1, mining_address)
        assert_equal(node.getblockcount(), pegout_height + 5)
        assert_equal(node.testmempoolaccept([pegout_spend['hex']])[0]['allowed'], True)
        node.sendrawtransaction(pegout_spend['hex'])
        node.generatetoaddress(1, mining_address)
        assert_equal(node.getrawmempool(), [])
        # The coinbase of the block that holds the peg-out still needs 100 blocks.
        self.check_premature(self.coinbase_spend(pegout_height), 7, blocks=False)
        self.sync_old()


if __name__ == '__main__':
    CoinbaseMaturityTest().main()
