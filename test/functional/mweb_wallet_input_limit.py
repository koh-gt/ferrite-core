#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the wallet keeps to the number of MWEB inputs that nodes relay.

Nodes do not relay a transaction with more than 1,000 MWEB inputs. A wallet that holds more MWEB coins than
that must create transactions that can be sent, by spending its larger coins, or refuse with an error where
no 1,000 of its coins pay the amount. A send that is refused leaves the wallet as it was.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.fec_util import setup_mweb_chain
from test_framework.util import assert_equal, assert_raises_rpc_error

# MAX_STANDARD_MWEB_TX_INPUTS in src/mweb/mweb_policy.h
MAX_MWEB_INPUTS = 1000
SMALL = Decimal('0.001')
SMALL_COINS = MAX_MWEB_INPUTS + 10
LARGE = Decimal('5')
TOO_MANY_INPUTS = "Transaction would have more than 1000 MWEB inputs, the most that nodes relay."


class MWEBWalletInputLimitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        # With this option a wallet selects coins once for each transaction. That halves the time it takes to
        # create the small coins, which is most of the test's time. By default a wallet also makes a second
        # selection, which keeps the coins of an address together, and uses it when it costs no more. The
        # last part of the test runs with the default.
        self.extra_args = [['-maxapsfee=-1']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def wallet_state(self, wallet):
        return {
            'unspent': len(wallet.listunspent(minconf=0)),
            'balances': wallet.getbalances()['mine'],
            'txcount': wallet.getwalletinfo()['txcount'],
            'locked': wallet.listlockunspent(),
            'mempool': self.nodes[0].getrawmempool(),
        }

    def mweb_input_count(self, wallet, txid):
        decoded = wallet.gettransaction(txid=txid, verbose=True)['decoded']
        return sum(1 for txin in decoded['vin'] if txin['ismweb'])

    def check_refused(self, wallet, address, amount, subtract_fee=False):
        before = self.wallet_state(wallet)
        assert_raises_rpc_error(-6, TOO_MANY_INPUTS, wallet.sendtoaddress, address=address, amount=amount, subtractfeefromamount=subtract_fee)
        assert_equal(self.wallet_state(wallet), before)

    def check_sent(self, wallet, txid):
        """The transaction is in the mempool and gets into the next block."""
        node = self.nodes[0]
        assert txid in node.getrawmempool()
        inputs = self.mweb_input_count(wallet, txid)
        assert inputs <= MAX_MWEB_INPUTS, "%d MWEB inputs" % inputs
        node.generate(1)
        assert_equal(wallet.gettransaction(txid)['confirmations'], 1)
        return inputs

    def run_test(self):
        node = self.nodes[0]
        setup_mweb_chain(node)
        node.createwallet(wallet_name='many')
        funder = node.get_wallet_rpc(self.default_wallet_name)
        wallet = node.get_wallet_rpc('many')

        self.log.info("Fund a wallet with %d small MWEB coins" % SMALL_COINS)
        small_address = wallet.getnewaddress(address_type='mweb')
        for i in range(SMALL_COINS):
            funder.sendtoaddress(small_address, SMALL)
            if i % 20 == 19:
                # Keeps the chains of unconfirmed transactions short.
                node.generate(1)
        node.generate(1)
        assert_equal(len(wallet.listunspent()), SMALL_COINS)
        assert_equal(wallet.getbalance(), SMALL * SMALL_COINS)
        mweb_address = funder.getnewaddress(address_type='mweb')

        self.log.info("An amount that no %d of the coins pay is refused, and the wallet stays as it was" % MAX_MWEB_INPUTS)
        self.check_refused(wallet, mweb_address, SMALL * (MAX_MWEB_INPUTS + 5))
        self.check_refused(wallet, funder.getnewaddress(address_type='bech32'), SMALL * (MAX_MWEB_INPUTS + 5))
        self.check_refused(wallet, mweb_address, SMALL * SMALL_COINS, subtract_fee=True)

        self.log.info("The same amount is paid from a larger coin once the wallet has one")
        funder.sendtoaddress(wallet.getnewaddress(address_type='mweb'), LARGE)
        node.generate(1)
        assert_equal(len(wallet.listunspent()), SMALL_COINS + 1)
        # The small coins add up to exactly this amount, and the fee comes out of it.
        txid = wallet.sendtoaddress(address=mweb_address, amount=SMALL * SMALL_COINS, subtractfeefromamount=True)
        self.check_sent(wallet, txid)
        change = LARGE - SMALL * SMALL_COINS
        assert_equal(wallet.getbalance(), SMALL * SMALL_COINS + change)
        assert_equal(len(wallet.listunspent()), SMALL_COINS + 1)

        self.log.info("With the default options, a transaction may spend exactly %d MWEB coins, and not one more" % MAX_MWEB_INPUTS)
        self.restart_node(0, extra_args=[])
        node.loadwallet('many')
        wallet = node.get_wallet_rpc('many')
        largest = change + SMALL * (MAX_MWEB_INPUTS - 1)
        self.check_refused(wallet, mweb_address, largest + SMALL, subtract_fee=True)
        txid = wallet.sendtoaddress(address=mweb_address, amount=largest, subtractfeefromamount=True)
        assert_equal(self.check_sent(wallet, txid), MAX_MWEB_INPUTS)
        # The inputs paid the amount exactly, so the transaction's change output has no value.
        unspent = [coin['amount'] for coin in wallet.listunspent() if coin['amount'] > 0]
        assert_equal(unspent, [SMALL] * (SMALL_COINS + 1 - MAX_MWEB_INPUTS))
        assert_equal(wallet.getbalance(), SMALL * (SMALL_COINS + 1 - MAX_MWEB_INPUTS))


if __name__ == '__main__':
    MWEBWalletInputLimitTest().main()
