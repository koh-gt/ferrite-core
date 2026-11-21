#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Test Inactive HD Chains.

A legacy test wallet is given an HD seed and then moved to a new seed, which
makes the first seed an inactive HD chain of the test wallet. A base wallet
with the first seed hands out its addresses. A payment to a key of the
inactive chain must be seen, and must make the test wallet derive the keys
that follow it:
- while the test wallet is unlocked;
- while it is encrypted and locked, once it is unlocked;
- after it is loaded again, so that the chain is read from the wallet file.
A payment to the key of the inactive seed itself must be seen as well.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet_util import (
    get_generate_key,
)


class InactiveHDChainsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-keypool=10"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine_and_sync(self):
        self.nodes[0].generate(1)
        self.nodes[0].syncwithvalidationinterfacequeue()

    def prepare_wallets(self, wallet_basename, encrypt=False):
        self.nodes[0].createwallet(wallet_name=f"{wallet_basename}_base", descriptors=False, blank=True)
        self.nodes[0].createwallet(wallet_name=f"{wallet_basename}_test", descriptors=False, blank=True)
        base_wallet = self.nodes[0].get_wallet_rpc(f"{wallet_basename}_base")
        test_wallet = self.nodes[0].get_wallet_rpc(f"{wallet_basename}_test")

        # Setup both wallets with the same HD seed
        seed = get_generate_key()
        base_wallet.sethdseed(True, seed.privkey)
        test_wallet.sethdseed(True, seed.privkey)

        # Generate a new HD seed on the test wallet
        test_wallet.sethdseed()

        if encrypt:
            # Encrypting keeps the HD seed and leaves the wallet locked
            test_wallet.encryptwallet("pass")

        return base_wallet, test_wallet

    def do_inactive_test(self, base_wallet, test_wallet, encrypt=False):
        default = self.nodes[0].get_wallet_rpc(self.default_wallet_name)

        # The first address should be known by both wallets.
        addr1 = base_wallet.getnewaddress()
        assert test_wallet.getaddressinfo(addr1)["ismine"]
        # The address at index 10 is the first address that the test wallet will not know initially
        for _ in range(0, 9):
            base_wallet.getnewaddress()
        addr2 = base_wallet.getnewaddress()
        assert not test_wallet.getaddressinfo(addr2)["ismine"]

        # Send to first address on the old seed
        txid = default.sendtoaddress(addr1, 10)
        self.mine_and_sync()
        test_wallet.gettransaction(txid)

        if encrypt:
            # The test wallet will not be able to generate the topped up keypool
            # until it is unlocked. So it still should not know about the second address
            assert not test_wallet.getaddressinfo(addr2)["ismine"]
            test_wallet.walletpassphrase("pass", 1)

        # The test wallet should now know about the second address as it
        # should have generated it in the inactive chain's keypool
        addr2_info = test_wallet.getaddressinfo(addr2)
        assert addr2_info["ismine"]
        assert_equal(addr2_info["hdkeypath"], "m/0'/0'/10'")

        # Send to second address on the old seed
        txid = default.sendtoaddress(addr2, 10)
        self.mine_and_sync()
        test_wallet.gettransaction(txid)

    def test_basic(self):
        self.log.info("Test basic case for inactive HD chains")
        self.do_inactive_test(*self.prepare_wallets("basic"))

    def test_encrypted_wallet(self):
        self.log.info("Test inactive HD chains when wallet is encrypted")
        self.do_inactive_test(*self.prepare_wallets("enc", encrypt=True), encrypt=True)

    def test_reloaded_wallet(self):
        self.log.info("Test inactive HD chains read from the wallet file")
        base_wallet, test_wallet = self.prepare_wallets("reload")
        test_wallet.unloadwallet()
        self.nodes[0].loadwallet("reload_test")
        test_wallet = self.nodes[0].get_wallet_rpc("reload_test")
        self.do_inactive_test(base_wallet, test_wallet)

    def test_inactive_seed_key(self):
        # The key of an HD seed has no derivation path, so there is nothing to
        # top up when it is paid.
        self.log.info("Test a payment to the key of an inactive HD seed")
        default = self.nodes[0].get_wallet_rpc(self.default_wallet_name)

        self.nodes[0].createwallet(wallet_name="seedkey_test", descriptors=False, blank=True)
        test_wallet = self.nodes[0].get_wallet_rpc("seedkey_test")
        seed = get_generate_key()
        test_wallet.sethdseed(True, seed.privkey)
        test_wallet.sethdseed()
        assert test_wallet.getaddressinfo(seed.p2pkh_addr)["ismine"]

        txid = default.sendtoaddress(seed.p2pkh_addr, 10)
        self.mine_and_sync()
        assert_equal(test_wallet.gettransaction(txid)["amount"], 10)

    def run_test(self):
        self.nodes[0].generate(101)

        self.test_basic()
        self.test_encrypted_wallet()
        self.test_reloaded_wallet()
        self.test_inactive_seed_key()


if __name__ == '__main__':
    InactiveHDChainsTest().main()
