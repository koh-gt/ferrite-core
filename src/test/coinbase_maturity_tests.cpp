// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <util/system.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

const int PERIOD = 144; // regtest miner confirmation window
const int THRESHOLD = 108;
const int SIGNAL_VERSION = VERSIONBITS_TOP_BITS | (1 << 7);

/** Regtest chain parameters, with one -vbparams argument unless it is empty. */
std::unique_ptr<const CChainParams> RegtestParams(const std::string& vbparams)
{
    ArgsManager args;
    if (!vbparams.empty()) args.ForceSetArg("-vbparams", vbparams);
    return CreateChainParams(args, CBaseChainParams::REGTEST);
}

/** A chain of block index entries of the given length (heights 0 to size - 1). The blocks of the window
 *  signalling_window signal bit 7 (the coinbasematurity deployment); no other block does. */
std::vector<CBlockIndex> BuildChain(int size, int signalling_window)
{
    std::vector<CBlockIndex> chain(size);
    for (int height = 0; height < size; ++height) {
        chain[height].nHeight = height;
        chain[height].pprev = height > 0 ? &chain[height - 1] : nullptr;
        chain[height].nVersion = height / PERIOD == signalling_window ? SIGNAL_VERSION : VERSIONBITS_TOP_BITS;
        chain[height].BuildSkip();
    }
    return chain;
}

/** The parent of the block at this height in the chain (nullptr for the genesis block). */
const CBlockIndex* Parent(const std::vector<CBlockIndex>& chain, int height)
{
    return height > 0 ? &chain[height - 1] : nullptr;
}

/** A branch of block index entries, by height; its first blocks may belong to another branch. */
struct Branch {
    std::vector<std::unique_ptr<CBlockIndex>> owned;
    std::vector<CBlockIndex*> by_height;
    //! The window from which the branch is LOCKED_IN.
    int lock_in_window{0};

    /** The first blocks of another branch, up to and including the one at this height. */
    Branch(const Branch& other, int height, int lock_in) : by_height(other.by_height.begin(), other.by_height.begin() + height + 1), lock_in_window(lock_in) {}
    explicit Branch(int lock_in) : lock_in_window(lock_in) {}

    /** Append blocks up to the height size - 1; those of the window signalling_window signal bit 7. */
    void Grow(int size, int window, int signalling_window)
    {
        for (int height = by_height.size(); height < size; ++height) {
            owned.push_back(std::make_unique<CBlockIndex>());
            CBlockIndex* block = owned.back().get();
            block->nHeight = height;
            block->pprev = height > 0 ? by_height[height - 1] : nullptr;
            block->nVersion = height / window == signalling_window ? SIGNAL_VERSION : VERSIONBITS_TOP_BITS;
            block->BuildSkip();
            by_height.push_back(block);
        }
    }

    const CBlockIndex* Parent(int height) const { return height > 0 ? by_height[height - 1] : nullptr; }
};

/** The state and since height of the block at this height on a branch whose deployment starts at the height start
 *  and is LOCKED_IN from the window lock_in_window for locked_in_windows windows, computed from these numbers. */
std::pair<ThresholdState, int> ExpectedState(int height, int window, int start, int lock_in_window, int locked_in_windows)
{
    const int activation = (lock_in_window + locked_in_windows) * window;
    if (height < start) return {ThresholdState::DEFINED, 0};
    if (height < lock_in_window * window) return {ThresholdState::STARTED, start};
    if (height < activation) return {ThresholdState::LOCKED_IN, lock_in_window * window};
    return {ThresholdState::ACTIVE, activation};
}

/** Clears the global version bits cache when it goes out of scope, however the scope is left. The cache holds
 *  states by block index, so it must not outlive a chain that a test built for itself. cs_main must be held. */
struct ScopedVersionBitsCacheClear {
    ~ScopedVersionBitsCacheClear() { versionbitscache.Clear(); }
};

/** A transaction that spends this outpoint and pays 1 satoshi less than value. */
CMutableTransaction Spend(const COutPoint& outpoint, CAmount value)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(outpoint);
    tx.vout.emplace_back(value - 1, CScript() << OP_TRUE);
    return tx;
}

/** CheckTxInputs for a spend in the block at spend_height of a coin created in the block at creation_height. */
TxValidationState CheckSpend(int creation_height, bool coinbase, bool pegout, int spend_height, int activation_height)
{
    CCoinsView base;
    CCoinsViewCache view(&base);
    const COutPoint outpoint(uint256S("0x0101"), 0);
    const CAmount value = 50 * COIN;
    view.AddCoin(outpoint, Coin(CTxOut(value, CScript() << OP_TRUE), creation_height, coinbase, pegout), false);
    const CTransaction tx(Spend(outpoint, value));
    TxValidationState state;
    CAmount fee = 0;
    const bool valid = Consensus::CheckTxInputs(tx, state, view, spend_height, activation_height, fee);
    BOOST_CHECK_EQUAL(valid, state.IsValid());
    if (valid) BOOST_CHECK_EQUAL(fee, 1);
    return state;
}

void CheckPrematureSpend(const TxValidationState& state, const std::string& reason, int depth)
{
    BOOST_CHECK(state.IsInvalid());
    BOOST_CHECK(state.GetResult() == TxValidationResult::TX_PREMATURE_SPEND);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), reason);
    BOOST_CHECK_EQUAL(state.GetDebugMessage(), strprintf("tried to spend %s at depth %d", reason == "bad-txns-premature-spend-of-coinbase" ? "coinbase" : "pegout output", depth));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(coinbase_maturity_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(helper)
{
    BOOST_CHECK_EQUAL(COINBASE_MATURITY, 10);
    BOOST_CHECK_EQUAL(COINBASE_MATURITY_EXTENDED, 100);
    const int activation = 432;
    // The creation height decides: below the activation height 10, from it on 100.
    static_assert(GetCoinbaseMaturity(activation - 1, activation) == 10, "");
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(0, activation), 10);
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(activation - 1, activation), 10);
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(activation, activation), 100);
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(activation + 1, activation), 100);
    // Always active: every coinbase, the genesis block's included.
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(0, 0), 100);
    // Not active: every coinbase needs 10, as before the deployment.
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(0, COINBASE_MATURITY_NOT_ACTIVE), 10);
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(1250080, COINBASE_MATURITY_NOT_ACTIVE), 10);
    BOOST_CHECK_EQUAL(GetCoinbaseMaturity(std::numeric_limits<int>::max() - 1, COINBASE_MATURITY_NOT_ACTIVE), 10);
}

BOOST_AUTO_TEST_CASE(check_tx_inputs_boundaries)
{
    const int A = 432;
    const std::string coinbase_reason = "bad-txns-premature-spend-of-coinbase";
    // A coinbase of the block below the activation height keeps maturity 10, also when spent after activation.
    CheckPrematureSpend(CheckSpend(A - 1, true, false, A - 1 + 9, A), coinbase_reason, 9);
    BOOST_CHECK(CheckSpend(A - 1, true, false, A - 1 + 10, A).IsValid());
    // The coinbases of the activation block and of the one after it need 100.
    for (const int creation : {A, A + 1}) {
        CheckPrematureSpend(CheckSpend(creation, true, false, creation + 10, A), coinbase_reason, 10);
        CheckPrematureSpend(CheckSpend(creation, true, false, creation + 99, A), coinbase_reason, 99);
        BOOST_CHECK(CheckSpend(creation, true, false, creation + 100, A).IsValid());
        // Without the deployment, 10 as before.
        CheckPrematureSpend(CheckSpend(creation, true, false, creation + 9, COINBASE_MATURITY_NOT_ACTIVE), coinbase_reason, 9);
        BOOST_CHECK(CheckSpend(creation, true, false, creation + 10, COINBASE_MATURITY_NOT_ACTIVE).IsValid());
    }
    // Always active.
    CheckPrematureSpend(CheckSpend(1, true, false, 100, 0), coinbase_reason, 99);
    BOOST_CHECK(CheckSpend(1, true, false, 101, 0).IsValid());
    // Peg-out outputs keep maturity 6 whatever the deployment.
    for (const int activation : {0, A, COINBASE_MATURITY_NOT_ACTIVE}) {
        CheckPrematureSpend(CheckSpend(A, false, true, A + 5, activation), "bad-txns-premature-spend-of-pegout", 5);
        BOOST_CHECK(CheckSpend(A, false, true, A + 6, activation).IsValid());
        // Other outputs have no maturity.
        BOOST_CHECK(CheckSpend(A, false, false, A, activation).IsValid());
    }
}

BOOST_AUTO_TEST_CASE(activation_height_states)
{
    LOCK(cs_main);
    ScopedVersionBitsCacheClear clear_cache_at_end;

    // Signalling from block 144; the window [144, 287] signals, [288, 431] is LOCKED_IN, the rule applies from 432.
    {
        const auto params = RegtestParams("coinbasematurity:0:0:144:99999999");
        const Consensus::Params& consensus = params->GetConsensus();
        BOOST_REQUIRE_EQUAL((int)consensus.nMinerConfirmationWindow, PERIOD);
        BOOST_REQUIRE_EQUAL((int)consensus.nRuleChangeActivationThreshold, THRESHOLD);
        versionbitscache.Clear();
        const auto chain = BuildChain(5 * PERIOD, 1);
        const auto state = [&](int height) { return VersionBitsState(Parent(chain, height), consensus, Consensus::DEPLOYMENT_COINBASE_MATURITY, versionbitscache); };
        BOOST_CHECK(state(143) == ThresholdState::DEFINED);
        BOOST_CHECK(state(144) == ThresholdState::STARTED);
        BOOST_CHECK(state(288) == ThresholdState::LOCKED_IN);
        BOOST_CHECK(state(431) == ThresholdState::LOCKED_IN);
        BOOST_CHECK(state(432) == ThresholdState::ACTIVE);
        BOOST_CHECK_EQUAL(CoinbaseMaturityActivationHeight(nullptr, consensus), COINBASE_MATURITY_NOT_ACTIVE);
        for (int height = 1; height < (int)chain.size(); ++height) {
            const int expected = height >= 432 ? 432 : COINBASE_MATURITY_NOT_ACTIVE;
            BOOST_CHECK_MESSAGE(CoinbaseMaturityActivationHeight(Parent(chain, height), consensus) == expected,
                                strprintf("block %d: activation height %d, expected %d", height, CoinbaseMaturityActivationHeight(Parent(chain, height), consensus), expected));
        }
    }

    // Never active (the regtest default): not active for any block.
    {
        const auto params = RegtestParams("");
        versionbitscache.Clear();
        const auto chain = BuildChain(5 * PERIOD, 1);
        BOOST_CHECK_EQUAL(CoinbaseMaturityActivationHeight(nullptr, params->GetConsensus()), COINBASE_MATURITY_NOT_ACTIVE);
        for (int height : {1, 432, 5 * PERIOD - 1}) {
            BOOST_CHECK_EQUAL(CoinbaseMaturityActivationHeight(Parent(chain, height), params->GetConsensus()), COINBASE_MATURITY_NOT_ACTIVE);
        }
    }

    // Always active: from block 0, also for the parent of the genesis block.
    {
        const auto params = RegtestParams("coinbasematurity:-1:0");
        versionbitscache.Clear();
        const auto chain = BuildChain(2 * PERIOD, -1);
        BOOST_CHECK_EQUAL(CoinbaseMaturityActivationHeight(nullptr, params->GetConsensus()), 0);
        for (int height : {1, 143, 144, 2 * PERIOD - 1}) {
            BOOST_CHECK_EQUAL(CoinbaseMaturityActivationHeight(Parent(chain, height), params->GetConsensus()), 0);
        }
    }

    // Mainnet, where the deployment is DEFINED until block 1,250,000: a coinbase spend at depth 10 is valid.
    {
        ArgsManager args;
        const auto params = CreateChainParams(args, CBaseChainParams::MAIN);
        versionbitscache.Clear();
        const auto chain = BuildChain(400, -1);
        BOOST_CHECK_EQUAL(CoinbaseMaturityActivationHeight(nullptr, params->GetConsensus()), COINBASE_MATURITY_NOT_ACTIVE);
        const int activation = CoinbaseMaturityActivationHeight(&chain[399], params->GetConsensus());
        BOOST_CHECK_EQUAL(activation, COINBASE_MATURITY_NOT_ACTIVE);
        BOOST_CHECK(CheckSpend(390, true, false, 400, activation).IsValid());
    }
}

BOOST_AUTO_TEST_CASE(activation_height_bisection)
{
    LOCK(cs_main);
    ScopedVersionBitsCacheClear clear_cache_at_end;
    const auto params = RegtestParams("coinbasematurity:0:0:144:99999999");
    const Consensus::Params& consensus = params->GetConsensus();
    const auto pos = Consensus::DEPLOYMENT_COINBASE_MATURITY;
    // The deployment activates two windows after the window that signals.
    for (const int activation_window : {3, 4, 7}) {
        versionbitscache.Clear();
        const auto chain = BuildChain((activation_window + 3) * PERIOD, activation_window - 2);
        int active_blocks = 0;
        for (int height = 0; height < (int)chain.size(); ++height) {
            const CBlockIndex* parent = Parent(chain, height);
            const int got = CoinbaseMaturityActivationHeight(parent, consensus);
            const bool active = VersionBitsState(parent, consensus, pos, versionbitscache) == ThresholdState::ACTIVE;
            const int expected = active ? VersionBitsStateSinceHeight(parent, consensus, pos, versionbitscache) : COINBASE_MATURITY_NOT_ACTIVE;
            BOOST_CHECK_MESSAGE(got == expected, strprintf("activation window %d, block %d: %d, expected %d", activation_window, height, got, expected));
            BOOST_CHECK_EQUAL(active, height >= activation_window * PERIOD);
            if (active) ++active_blocks;
        }
        BOOST_CHECK_EQUAL(active_blocks, 3 * PERIOD);
    }
}

BOOST_AUTO_TEST_CASE(activation_height_order_independence)
{
    // Branches that share one version bits cache and activate at different heights, with one and with several
    // locked-in windows. The activation height of every block of every branch, asked for in ascending, descending
    // and random orders, each on a cleared cache, is the first block after the locked-in windows; between two such
    // queries the state and since height of a random block of a random branch are checked as well. Last, the since
    // heights of the tips are asked for first (they fill the cache window after window) and then every block in a
    // random order.
    LOCK(cs_main);
    ScopedVersionBitsCacheClear clear_cache_at_end;
    const auto pos = Consensus::DEPLOYMENT_COINBASE_MATURITY;
    for (const int window : {10, PERIOD}) {
        for (const int locked_in_windows : {1, 3, 504}) {
            // 504 windows of 144 blocks take long and add nothing to 504 windows of 10 blocks.
            if (window == PERIOD && locked_in_windows == 504) continue;
            const int start = window;
            const int threshold = window * 3 / 4;
            Consensus::Params consensus = RegtestParams(strprintf("coinbasematurity:0:0:%d:99999999:%d", start, locked_in_windows))->GetConsensus();
            consensus.nMinerConfirmationWindow = window;
            consensus.nRuleChangeActivationThreshold = threshold;
            BOOST_REQUIRE_EQUAL(consensus.vDeployments[pos].nLockedInPeriods, locked_in_windows);
            const int size = (8 + locked_in_windows) * window;
            // Branch x signals in window 2, so it is LOCKED_IN from window 3.
            Branch x(3);
            x.Grow(size, window, 2);
            // Branch y leaves x in the middle of window 2, without enough signals in it, and signals in window 4.
            Branch y(x, 2 * window + threshold / 2, 5);
            y.Grow(size + window, window, 4);
            // Branch z leaves x in its first locked-in window and does not signal: it activates where x does.
            Branch z(x, 3 * window + window / 2, 3);
            z.Grow(size - window, window, -1);
            const std::vector<const Branch*> branches{&x, &y, &z};

            std::vector<std::pair<size_t, int>> ascending;
            for (size_t b = 0; b < branches.size(); ++b) {
                for (int height = 0; height < (int)branches[b]->by_height.size(); ++height) ascending.emplace_back(b, height);
            }
            std::stable_sort(ascending.begin(), ascending.end(), [](const std::pair<size_t, int>& l, const std::pair<size_t, int>& r) { return l.second < r.second; });
            const std::vector<std::pair<size_t, int>> descending(ascending.rbegin(), ascending.rend());
            std::vector<std::vector<std::pair<size_t, int>>> orders{ascending, descending};
            for (unsigned int seed = 1; seed <= 3; ++seed) {
                orders.push_back(ascending);
                std::mt19937 rng{seed};
                std::shuffle(orders.back().begin(), orders.back().end(), rng);
            }
            const std::string context = strprintf("window %d, %d locked-in windows", window, locked_in_windows);
            std::mt19937 rng{42};
            int mismatches = 0;
            const auto check_activation = [&](size_t b, int height, const std::string& order) {
                const Branch& branch = *branches[b];
                const int got = CoinbaseMaturityActivationHeight(branch.Parent(height), consensus);
                const int activation = (branch.lock_in_window + locked_in_windows) * window;
                const int expected = height >= activation ? activation : COINBASE_MATURITY_NOT_ACTIVE;
                if (got != expected && ++mismatches <= 10) {
                    BOOST_ERROR(strprintf("%s, %s: branch %d, block %d: activation height %d, expected %d", context, order, b, height, got, expected));
                }
            };
            for (size_t n = 0; n < orders.size(); ++n) {
                versionbitscache.Clear();
                const std::string order = strprintf("order %d", n);
                for (size_t i = 0; i < orders[n].size(); ++i) {
                    check_activation(orders[n][i].first, orders[n][i].second, order);
                    // For the windows of 144 blocks, only after every seventh query, so that the case stays short.
                    if (window == PERIOD && i % 7) continue;
                    const size_t b = rng() % branches.size();
                    const Branch& branch = *branches[b];
                    const int height = rng() % branch.by_height.size();
                    const std::pair<ThresholdState, int> got{VersionBitsState(branch.Parent(height), consensus, pos, versionbitscache),
                                                             VersionBitsStateSinceHeight(branch.Parent(height), consensus, pos, versionbitscache)};
                    if (got != ExpectedState(height, window, start, branch.lock_in_window, locked_in_windows) && ++mismatches <= 10) {
                        BOOST_ERROR(strprintf("%s, %s: branch %d, block %d: state %d since %d", context, order, b, height, (int)got.first, got.second));
                    }
                }
            }
            versionbitscache.Clear();
            for (const Branch* branch : branches) VersionBitsStateSinceHeight(branch->by_height.back(), consensus, pos, versionbitscache);
            for (const auto& query : orders.back()) check_activation(query.first, query.second, "after the since heights of the tips");
            BOOST_CHECK_MESSAGE(mismatches == 0, strprintf("%s: %d mismatches", context, mismatches));
            // The branches go out of scope, and the cache must not keep their blocks.
            versionbitscache.Clear();
        }
    }
}

BOOST_AUTO_TEST_CASE(remove_for_reorg)
{
    // A mempool that spends a coinbase output created at height 300 and is checked for the block at height 350
    // (depth 50): the spend stays while the rule applies from a height above 300 (or not at all), and is removed
    // once it applies from a height at or below 300, as after a reorganisation onto a chain on which the
    // deployment activated earlier. Peg-out outputs keep maturity 6.
    struct Case {
        int creation_height;
        bool coinbase;
        bool pegout;
        unsigned int mempool_height;
        int activation_height;
        bool kept;
    };
    const std::vector<Case> cases{
        {300, true, false, 350, 432, true},
        {300, true, false, 350, COINBASE_MATURITY_NOT_ACTIVE, true},
        {300, true, false, 350, 288, false},
        {300, true, false, 350, 300, false},
        {300, true, false, 400, 288, true},
        {300, true, false, 399, 288, false},
        {300, true, false, 309, COINBASE_MATURITY_NOT_ACTIVE, false},
        {300, true, false, 310, COINBASE_MATURITY_NOT_ACTIVE, true},
        {300, false, true, 305, 0, false},
        {300, false, true, 306, 0, true},
    };
    for (const Case& c : cases) {
        CTxMemPool pool;
        LOCK2(cs_main, pool.cs);
        CCoinsView base;
        CCoinsViewCache view(&base);
        const COutPoint outpoint(uint256S("0x0202"), 0);
        view.AddCoin(outpoint, Coin(CTxOut(50 * COIN, CScript() << OP_TRUE), c.creation_height, c.coinbase, c.pegout), false);
        const CMutableTransaction tx = Spend(outpoint, 50 * COIN);
        TestMemPoolEntryHelper entry;
        pool.addUnchecked(entry.SpendsCoinbase(true).FromTx(tx));
        BOOST_REQUIRE(pool.exists(tx.GetHash()));
        pool.removeForReorg(&view, c.mempool_height, c.activation_height, STANDARD_LOCKTIME_VERIFY_FLAGS);
        BOOST_CHECK_MESSAGE(pool.exists(tx.GetHash()) == c.kept,
                            strprintf("created at %d (coinbase %d, peg-out %d), mempool height %d, activation height %d: expected %s",
                                      c.creation_height, c.coinbase, c.pegout, c.mempool_height, c.activation_height, c.kept ? "kept" : "removed"));
    }
}

BOOST_AUTO_TEST_SUITE_END()
