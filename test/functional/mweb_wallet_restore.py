#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a wallet restored from its seed knows which of its MWEB coins were spent.

A wallet restored from its seed and rescanned finds its MWEB coins in blocks, without the transactions that
created them, and it finds their spends in blocks as well. It must show the balances and unspent coins of the
original wallet, keep that view when it is loaded again, and spend its coins in transactions that nodes
accept. The same holds for a wallet that sees a coin and its spend only in blocks while it follows the chain.
Both unencrypted and encrypted wallets are tested. The wallet file must keep such a spend in a record of its
own type, next to the receipt of the coin; removeprunedfunds removes the spend with its record, and a rescan
records it again. Only legacy wallets hold MWEB coins: createwallet refuses descriptor wallets.
"""

from decimal import Decimal
import os

from test_framework.bdb import dump_bdb_kv
from test_framework.test_framework import BitcoinTestFramework
from test_framework.fec_util import setup_mweb_chain
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet_util import get_generate_key

PASSPHRASE = 'passphrase'
WALLETS = ('restored', 'encrypted', 'watcher')


class MWEBWalletRestoreTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # Enough addresses ahead for a restored wallet to find every coin of the original one.
        self.extra_args = [['-keypool=100'], ['-keypool=100']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def seeded_wallet(self, node, name, seed, passphrase=''):
        """A legacy wallet with the given HD seed, loaded once more to load its MWEB keys."""
        node.createwallet(wallet_name=name, descriptors=False, blank=True, passphrase=passphrase)
        wallet = node.get_wallet_rpc(name)
        if passphrase:
            wallet.walletpassphrase(passphrase, 600)
        wallet.sethdseed(True, seed.privkey)
        wallet.unloadwallet()
        node.loadwallet(name)
        return node.get_wallet_rpc(name)

    def state(self, wallet):
        """Balances and unspent coins."""
        unspent = sorted((coin['amount'], coin['confirmations'], coin['address']) for coin in wallet.listunspent(minconf=0))
        return {
            'balance': wallet.getbalance(),
            'balances': wallet.getbalances()['mine'],
            'unspent': unspent,
        }

    def check_copies(self, wallets):
        """Every copy of the wallet shows the balances and coins of the original."""
        expected = self.state(self.original)
        assert len(expected['unspent']) > 0
        for wallet in wallets:
            assert_equal(self.state(wallet), expected)

    def check_records(self, name):
        """The entry for a spend that the wallet found only in a block, of a coin whose receipt it also knows only
        from a block, has a hash of its own and is kept in a record of its own type. Every other entry is kept in
        a transaction record. The file is read directly, which only works while it holds no large transaction.
        Returns the number of such spends."""
        node = self.nodes[1]
        wallet = node.get_wallet_rpc(name)
        entries = {entry['txid']: entry for entry in wallet.listwallettransactions()}
        assert_equal(len(entries), wallet.getwalletinfo()['txcount'])
        receipts = {entry['partial_mweb']['received']['output_id'] for entry in entries.values()
                    if 'received' in entry.get('partial_mweb', {})}
        expected = {}
        for txid, entry in entries.items():
            spent = entry.get('partial_mweb', {}).get('spent')
            if spent is not None and spent['output_id'] in receipts:
                assert txid != spent['output_id']
                expected[txid] = 'mweb_spend'
            else:
                expected[txid] = 'tx'

        wallet.unloadwallet()
        records = {}
        for key in dump_bdb_kv(os.path.join(node.datadir, self.chain, 'wallets', name, self.wallet_data_filename)):
            kind = key[1:1 + key[0]].decode()
            if kind in ('tx', 'mweb_spend'):
                records[key[1 + key[0]:][::-1].hex()] = kind
        node.loadwallet(name)
        assert_equal(records, expected)
        return list(expected.values()).count('mweb_spend')

    def mweb_inputs(self, wallet, txid):
        return sum(1 for txin in wallet.gettransaction(txid=txid, verbose=True)['decoded']['vin'] if txin['ismweb'])

    def send(self, wallet, amount):
        """Send from the wallet, see the transaction reach the other node and get into a block. Returns the
        number of MWEB inputs."""
        node = self.nodes[0]
        txid = wallet.sendtoaddress(self.funder.getnewaddress(address_type='mweb'), amount)
        self.sync_mempools()
        assert txid in node.getrawmempool()
        node.generate(1)
        self.sync_all()
        assert_equal(wallet.gettransaction(txid)['confirmations'], 1)
        return self.mweb_inputs(wallet, txid)

    def restart_restore_node(self):
        """Restart the node of the restored wallets and load them. The encrypted wallet stays locked."""
        node = self.nodes[1]
        self.restart_node(1)
        self.connect_nodes(0, 1)
        for name in WALLETS:
            if os.path.isdir(os.path.join(node.datadir, self.chain, 'wallets', name)):
                node.loadwallet(name)
        return [node.get_wallet_rpc(name) for name in WALLETS]

    def run_test(self):
        node, restore_node = self.nodes
        setup_mweb_chain(node)
        self.sync_all()
        self.funder = node.get_wallet_rpc(self.default_wallet_name)
        seed = get_generate_key()

        self.log.info("Fund a wallet with MWEB coins and spend some of them")
        self.original = self.seeded_wallet(node, 'original', seed)
        self.funder.sendtoaddress(self.original.getnewaddress(address_type='mweb'), 10)
        self.funder.sendtoaddress(self.original.getnewaddress(address_type='mweb'), 3)
        node.generate(1)
        self.original.sendtoaddress(self.funder.getnewaddress(address_type='mweb'), 4)
        node.generate(1)
        # A peg-out spends MWEB coins as well.
        self.original.sendtoaddress(self.funder.getnewaddress(address_type='bech32'), 7)
        node.generate(1)
        self.funder.sendtoaddress(self.original.getnewaddress(address_type='mweb'), 2)
        node.generate(1)
        self.sync_all()
        # Left: the change of the peg-out and the last coin.
        assert_equal(len(self.original.listunspent()), 2)

        self.log.info("Restore it from its seed, unencrypted and encrypted, and rescan")
        restored = self.seeded_wallet(restore_node, 'restored', seed)
        encrypted = self.seeded_wallet(restore_node, 'encrypted', seed, PASSPHRASE)
        for wallet in (restored, encrypted):
            assert_equal(self.state(wallet), {
                'balance': 0,
                'balances': {'trusted': 0, 'untrusted_pending': 0, 'immature': 0},
                'unspent': [],
            })
        restored.rescanblockchain()
        self.check_copies([restored])
        # The encrypted wallet is locked: it rescans with the keys it has without the passphrase.
        assert_equal(encrypted.getwalletinfo()['unlocked_until'], 0)
        encrypted.rescanblockchain()
        self.check_copies([encrypted])
        encrypted.walletpassphrase(PASSPHRASE, 600)
        self.check_copies([restored, encrypted])

        self.log.info("The wallet file keeps the spends that the rescan found next to the receipts")
        # The coins of 10 and 3 and the change of 5.999961 were spent, and the wallet knows them from blocks only.
        assert_equal(self.check_records('restored'), 3)
        assert_equal(self.check_records('encrypted'), 3)

        self.log.info("removeprunedfunds removes such a spend with its record, and a rescan records it again")
        spend = next(entry for entry in restored.listwallettransactions()
                     if 'spent' in entry.get('partial_mweb', {}) and entry['amount'] == -10)
        restored.removeprunedfunds(spend['txid'])
        assert_equal(self.check_records('restored'), 2)
        assert spend['txid'] not in [entry['txid'] for entry in restored.listwallettransactions()]
        # The coin of 10 shows as unspent again.
        assert_equal(restored.getbalance(), self.original.getbalance() + 10)
        assert Decimal('10') in [coin['amount'] for coin in restored.listunspent()]
        restored.rescanblockchain()
        assert_equal(self.check_records('restored'), 3)
        assert_equal(restored.gettransaction(spend['txid'])['partial_mweb'], spend['partial_mweb'])
        self.check_copies([restored])

        self.log.info("The restored wallets keep that view when they are loaded again and after a restart")
        restored, encrypted, _ = self.restart_restore_node()
        assert_equal(encrypted.getwalletinfo()['unlocked_until'], 0)
        self.check_copies([restored, encrypted])
        encrypted.walletpassphrase(PASSPHRASE, 600)

        self.log.info("The restored wallets spend their coins in transactions that nodes accept")
        # More than either coin, so the transaction spends both.
        assert_equal(self.send(restored, Decimal('2.5')), 2)
        self.check_copies([restored, encrypted])
        self.send(encrypted, Decimal('1'))
        self.check_copies([restored, encrypted])

        self.log.info("A wallet that sees a coin and its spend only in blocks while it follows the chain")
        # Restored only now, this wallet knows every coin from blocks only, so its file can be read below.
        watcher = self.seeded_wallet(restore_node, 'watcher', seed)
        watcher.rescanblockchain()
        wallets = [restored, encrypted, watcher]
        self.check_copies(wallets)
        spends = self.check_records('watcher')
        self.disconnect_nodes(0, 1)
        self.funder.sendtoaddress(self.original.getnewaddress(address_type='mweb'), 5)
        node.generate(1)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        # Reading the balances makes the wallets cache them in the entries that received the coins; the
        # block with the spend must update them.
        self.check_copies(wallets)
        self.disconnect_nodes(0, 1)
        txid = self.original.sendtoaddress(self.funder.getnewaddress(address_type='mweb'), Decimal('5.2'))
        node.generate(1)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        self.check_copies(wallets)
        # The coin of 5 and at least one more, which the watcher also knows from a block only.
        inputs = self.mweb_inputs(self.original, txid)
        assert inputs > 1
        assert_equal(self.check_records('watcher'), spends + inputs)

        self.log.info("All of them keep that view after a restart and can spend")
        wallets = self.restart_restore_node()
        restored, encrypted, watcher = wallets
        encrypted.walletpassphrase(PASSPHRASE, 600)
        self.check_copies(wallets)
        self.send(watcher, Decimal('0.2'))
        self.check_copies(wallets)

        self.log.info("Descriptor wallets cannot be created, so they hold no MWEB coins")
        assert_raises_rpc_error(-4, "Descriptor wallets not supported.", restore_node.createwallet, wallet_name='descriptors', descriptors=True)


if __name__ == '__main__':
    MWEBWalletRestoreTest().main()
