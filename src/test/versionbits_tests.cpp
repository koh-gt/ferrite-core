// Copyright (c) 2014-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/memory.h>
#include <validation.h>
#include <versionbits.h>

#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

/* Define a virtual block time, one block per 10 minutes after Nov 14 2014, 0:55:36am */
static int32_t TestTime(int nHeight) { return 1415926536 + 600 * nHeight; }

static const std::string StateName(ThresholdState state)
{
    switch (state) {
    case ThresholdState::DEFINED:   return "DEFINED";
    case ThresholdState::STARTED:   return "STARTED";
    case ThresholdState::LOCKED_IN: return "LOCKED_IN";
    case ThresholdState::ACTIVE:    return "ACTIVE";
    case ThresholdState::FAILED:    return "FAILED";
    } // no default case, so the compiler can warn about missing cases
    return "";
}

static const Consensus::Params paramsDummy = Consensus::Params();

class TestConditionChecker : public AbstractThresholdConditionChecker
{
private:
    mutable ThresholdConditionCache cache;
    bool height_based = false;
    int64_t height_start = 0;
    int64_t height_timeout = 0;
    int locked_in_periods = 1;

public:
    int64_t BeginTime(const Consensus::Params& params) const override { return this->height_based ? 0 : TestTime(10000); }
    int64_t EndTime(const Consensus::Params& params) const override { return this->height_based ? 0 : TestTime(20000); }
    int64_t BeginHeight(const Consensus::Params& params) const override { return this->height_start; }
    int64_t EndHeight(const Consensus::Params& params) const override { return this->height_timeout; }
    int Period(const Consensus::Params& params) const override { return 1000; }
    int Threshold(const Consensus::Params& params) const override { return 900; }
    int LockedInPeriods(const Consensus::Params& params) const override { return this->locked_in_periods; }
    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override { return (pindex->nVersion & 0x100); }
    void SetLockedInPeriods(int n) { this->locked_in_periods = n; }
    void SetHeightBased(int64_t flag) { this->height_based = flag; }
    void SetHeightStart(int64_t n) { this->height_start = n; }
    void SetHeightTimeout(int64_t n) { this->height_timeout = n; }

    ThresholdState GetStateFor(const CBlockIndex* pindexPrev) const { return AbstractThresholdConditionChecker::GetStateFor(pindexPrev, paramsDummy, cache); }
    int GetStateSinceHeightFor(const CBlockIndex* pindexPrev) const { return AbstractThresholdConditionChecker::GetStateSinceHeightFor(pindexPrev, paramsDummy, cache); }
};

class TestAlwaysActiveConditionChecker : public TestConditionChecker
{
public:
    int64_t BeginTime(const Consensus::Params& params) const override { return Consensus::BIP9Deployment::ALWAYS_ACTIVE; }
};

class TestNeverActiveConditionChecker : public TestConditionChecker
{
public:
    int64_t BeginTime(const Consensus::Params& params) const override { return Consensus::BIP9Deployment::NEVER_ACTIVE; }
};

#define CHECKERS 6

class VersionBitsTester
{
    // A fake blockchain
    std::vector<CBlockIndex*> vpblock;

    // 6 independent checkers for the same bit.
    // The first one performs all checks, the second only 50%, the third only 25%, etc...
    // This is to test whether lack of cached information leads to the same results.
    TestConditionChecker checker[CHECKERS];
    // Another 6 that assume always active activation
    TestAlwaysActiveConditionChecker checker_always[CHECKERS];
    // Another 6 that assume never active activation
    TestNeverActiveConditionChecker checker_never[CHECKERS];

    // Test counter (to identify failures)
    int num;

public:
    VersionBitsTester() : num(1000) {}

    VersionBitsTester& SetCheckerHeightBasedTest(bool flag, int64_t height_start, int64_t height_timeout)
    {
        for (unsigned int i = 0; i < CHECKERS; i++) {
            checker[i].SetHeightBased(flag);
            checker[i].SetHeightStart(height_start);
            checker[i].SetHeightTimeout(height_timeout);
        }
        return *this;
    }

    VersionBitsTester& SetLockedInPeriods(int locked_in_periods)
    {
        for (unsigned int i = 0; i < CHECKERS; i++) {
            checker[i].SetLockedInPeriods(locked_in_periods);
        }
        return *this;
    }

    VersionBitsTester& Reset() {
        // Have each group of tests be counted by the 1000s part, starting at 1000
        num = num - (num % 1000) + 1000;

        for (unsigned int i = 0; i < vpblock.size(); i++) {
            delete vpblock[i];
        }
        for (unsigned int  i = 0; i < CHECKERS; i++) {
            checker[i] = TestConditionChecker();
            checker_always[i] = TestAlwaysActiveConditionChecker();
            checker_never[i] = TestNeverActiveConditionChecker();
        }
        vpblock.clear();
        return *this;
    }

    ~VersionBitsTester() {
         Reset();
    }

    VersionBitsTester& Mine(unsigned int height, int32_t nTime, int32_t nVersion) {
        while (vpblock.size() < height) {
            CBlockIndex* pindex = new CBlockIndex();
            pindex->nHeight = vpblock.size();
            pindex->pprev = Tip();
            pindex->nTime = nTime;
            pindex->nVersion = nVersion;
            pindex->BuildSkip();
            vpblock.push_back(pindex);
        }
        return *this;
    }

    VersionBitsTester& TestStateSinceHeight(int height)
    {
        return TestStateSinceHeight(height, height);
    }

    VersionBitsTester& TestStateSinceHeight(int height, int height_delayed)
    {
        const CBlockIndex* tip = Tip();
        for (int i = 0; i < CHECKERS; i++) {
            if (InsecureRandBits(i) == 0) {
                BOOST_CHECK_MESSAGE(checker[i].GetStateSinceHeightFor(tip) == height, strprintf("Test %i for StateSinceHeight", num));
                BOOST_CHECK_MESSAGE(checker_always[i].GetStateSinceHeightFor(tip) == 0, strprintf("Test %i for StateSinceHeight (always active)", num));
                BOOST_CHECK_MESSAGE(checker_never[i].GetStateSinceHeightFor(tip) == 0, strprintf("Test %i for StateSinceHeight (never active)", num));
            }
        }
        num++;
        return *this;
    }

    VersionBitsTester& TestState(ThresholdState exp)
    {
        return TestState(exp, exp);
    }

    VersionBitsTester& TestState(ThresholdState exp, ThresholdState exp_delayed)
    {
        if (exp != exp_delayed) {
            // only expected differences are that delayed stays in locked_in longer
            BOOST_CHECK_EQUAL(exp, ThresholdState::ACTIVE);
            BOOST_CHECK_EQUAL(exp_delayed, ThresholdState::LOCKED_IN);
        }

        const CBlockIndex* pindex = Tip();
        for (int i = 0; i < CHECKERS; i++) {
            if (InsecureRandBits(i) == 0) {
                ThresholdState got = checker[i].GetStateFor(pindex);
                ThresholdState got_always = checker_always[i].GetStateFor(pindex);
                ThresholdState got_never = checker_never[i].GetStateFor(pindex);
                // nHeight of the next block. If vpblock is empty, the next (ie first)
                // block should be the genesis block with nHeight == 0.
                int height = pindex == nullptr ? 0 : pindex->nHeight + 1;
                BOOST_CHECK_MESSAGE(got == exp, strprintf("Test %i for %s height %d (got %s)", num, StateName(exp), height, StateName(got)));
                BOOST_CHECK_MESSAGE(got_always == ThresholdState::ACTIVE, strprintf("Test %i for ACTIVE height %d (got %s; always active case)", num, height, StateName(got_always)));
                BOOST_CHECK_MESSAGE(got_never == ThresholdState::FAILED, strprintf("Test %i for FAILED height %d (got %s; never active case)", num, height, StateName(got_never)));
            }
        }
        num++;
        return *this;
    }

    VersionBitsTester& TestDefined() { return TestState(ThresholdState::DEFINED); }
    VersionBitsTester& TestStarted() { return TestState(ThresholdState::STARTED); }
    VersionBitsTester& TestLockedIn() { return TestState(ThresholdState::LOCKED_IN); }
    VersionBitsTester& TestActive() { return TestState(ThresholdState::ACTIVE); }
    VersionBitsTester& TestFailed() { return TestState(ThresholdState::FAILED); }

    CBlockIndex* Tip() { return vpblock.empty() ? nullptr : vpblock.back(); }
};

BOOST_FIXTURE_TEST_SUITE(versionbits_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(versionbits_test)
{
    for (int i = 0; i < 64; i++) {
        // DEFINED -> STARTED after timeout reached -> FAILED
        VersionBitsTester().TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0x100).TestDefined().TestStateSinceHeight(0)
                           .Mine(11, TestTime(11), 0x100).TestDefined().TestStateSinceHeight(0)
                           .Mine(989, TestTime(989), 0x100).TestDefined().TestStateSinceHeight(0)
                           .Mine(999, TestTime(20000), 0x100).TestDefined().TestStateSinceHeight(0) // Timeout and start time reached simultaneously
                           .Mine(1000, TestTime(20000), 0).TestStarted().TestStateSinceHeight(1000) // Hit started, stop signalling
                           .Mine(1999, TestTime(30001), 0).TestStarted().TestStateSinceHeight(1000)
                           .Mine(2000, TestTime(30002), 0x100).TestFailed().TestStateSinceHeight(2000) // Hit failed, start signalling again
                           .Mine(2001, TestTime(30003), 0x100).TestFailed().TestStateSinceHeight(2000)
                           .Mine(2999, TestTime(30004), 0x100).TestFailed().TestStateSinceHeight(2000)
                           .Mine(3000, TestTime(30005), 0x100).TestFailed().TestStateSinceHeight(2000)
                           .Mine(4000, TestTime(30006), 0x100).TestFailed().TestStateSinceHeight(2000)

        // DEFINED -> STARTED -> FAILED
                           .Reset().TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(10000) - 1, 0x100).TestDefined().TestStateSinceHeight(0) // One second more and it would be defined
                           .Mine(2000, TestTime(10000), 0x100).TestStarted().TestStateSinceHeight(2000) // So that's what happens the next period
                           .Mine(2051, TestTime(10010), 0).TestStarted().TestStateSinceHeight(2000) // 51 old blocks
                           .Mine(2950, TestTime(10020), 0x100).TestStarted().TestStateSinceHeight(2000) // 899 new blocks
                           .Mine(3000, TestTime(20000), 0).TestFailed().TestStateSinceHeight(3000) // 50 old blocks (so 899 out of the past 1000)
                           .Mine(4000, TestTime(20010), 0x100).TestFailed().TestStateSinceHeight(3000)

        // DEFINED -> STARTED -> LOCKEDIN after timeout reached -> ACTIVE
                           .Reset().TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(10000) - 1, 0x101).TestDefined().TestStateSinceHeight(0) // One second more and it would be defined
                           .Mine(2000, TestTime(10000), 0x101).TestStarted().TestStateSinceHeight(2000) // So that's what happens the next period
                           .Mine(2999, TestTime(30000), 0x100).TestStarted().TestStateSinceHeight(2000) // 999 new blocks
                           .Mine(3000, TestTime(30000), 0x100).TestLockedIn().TestStateSinceHeight(3000) // 1 new block (so 1000 out of the past 1000 are new)
                           .Mine(3999, TestTime(30001), 0).TestLockedIn().TestStateSinceHeight(3000)
                           .Mine(24000, TestTime(40000), 0).TestActive().TestStateSinceHeight(4000, 15000)

        // DEFINED -> STARTED -> LOCKEDIN before timeout -> ACTIVE
                           .Reset().TestDefined()
                           .Mine(1, TestTime(1), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(10000) - 1, 0x101).TestDefined().TestStateSinceHeight(0) // One second more and it would be defined
                           .Mine(2000, TestTime(10000), 0x101).TestStarted().TestStateSinceHeight(2000) // So that's what happens the next period
                           .Mine(2050, TestTime(10010), 0x200).TestStarted().TestStateSinceHeight(2000) // 50 old blocks
                           .Mine(2950, TestTime(10020), 0x100).TestStarted().TestStateSinceHeight(2000) // 900 new blocks
                           .Mine(2999, TestTime(19999), 0x200).TestStarted().TestStateSinceHeight(2000) // 49 old blocks
                           .Mine(3000, TestTime(29999), 0x200).TestLockedIn().TestStateSinceHeight(3000) // 1 old block (so 900 out of the past 1000)
                           .Mine(3999, TestTime(30001), 0).TestLockedIn().TestStateSinceHeight(3000)
                           .Mine(15000, TestTime(40000), 0).TestActive().TestStateSinceHeight(4000, 15000)
                           .Mine(24000, TestTime(40000), 0).TestActive().TestStateSinceHeight(4000, 15000)

        // DEFINED multiple periods -> STARTED multiple periods -> FAILED
                           .Reset().TestDefined().TestStateSinceHeight(0)
                           .Mine(999, TestTime(999), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(1000), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(2000, TestTime(2000), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(3000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(4000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(5000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(5999, TestTime(20000), 0).TestStarted().TestStateSinceHeight(3000)
                           .Mine(6000, TestTime(20000), 0).TestFailed().TestStateSinceHeight(6000)
                           .Mine(7000, TestTime(20000), 0x100).TestFailed().TestStateSinceHeight(6000)
                           .Mine(24000, TestTime(20000), 0x100).TestFailed().TestStateSinceHeight(6000) // stay in FAILED no matter how much we signal

        // DEFINED -> STARTED -> LOCKEDIN -> ACTIVE (mandatory lockin)
                           .Reset().SetCheckerHeightBasedTest(true, 2000, 4000).TestDefined().TestStateSinceHeight(0)
                           .Mine(999, TestTime(999), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(1000), 0).TestDefined().TestStateSinceHeight(0)
                           .Mine(2000, TestTime(10000), 0).TestStarted().TestStateSinceHeight(2000)
                           .Mine(4000, TestTime(10001), 0).TestLockedIn().TestStateSinceHeight(4000)
                           .Mine(5000, TestTime(10002), 0).TestActive().TestStateSinceHeight(5000)
                           .Mine(7000, TestTime(20000), 0x100).TestActive().TestStateSinceHeight(5000)

        // DEFINED -> STARTED -> LOCKEDIN -> ACTIVE (mandatory lockin but activation by signalling)
                           .Reset().SetCheckerHeightBasedTest(true, 3000, 10000).TestDefined().TestStateSinceHeight(0)
                           .Mine(1, TestTime(1), 0x200).TestDefined().TestStateSinceHeight(0)
                           .Mine(1000, TestTime(9999) - 1, 0x200).TestDefined().TestStateSinceHeight(0)
                           .Mine(2000, TestTime(10000), 0x101).TestDefined().TestStateSinceHeight(0)
                           .Mine(3000, TestTime(10010), 0x200).TestStarted().TestStateSinceHeight(3000)
                           .Mine(3050, TestTime(10020), 0x200).TestStarted().TestStateSinceHeight(3000) // 50 old blocks
                           .Mine(3950, TestTime(11999), 0x100).TestStarted().TestStateSinceHeight(3000) // 900 new blocks
                           .Mine(3999, TestTime(12000), 0x200).TestStarted().TestStateSinceHeight(3000) // 49 old blocks
                           .Mine(4000, TestTime(12500), 0x200).TestLockedIn().TestStateSinceHeight(4000) // 1 old block
                           .Mine(4999, TestTime(13000), 0).TestLockedIn().TestStateSinceHeight(4000)
                           .Mine(5000, TestTime(13001), 0).TestActive().TestStateSinceHeight(5000)
        ;
    }
}

BOOST_AUTO_TEST_CASE(versionbits_locked_in_periods)
{
    // A deployment stays LOCKED_IN for exactly the given number of periods, then is ACTIVE; the since height of
    // LOCKED_IN is the first block of the first locked-in period. Height-based, then time-based.
    for (int i = 0; i < 16; i++) {
        for (const int k : {2, 3, 5}) {
            VersionBitsTester tester;
            tester.SetCheckerHeightBasedTest(true, 2000, 99999999).SetLockedInPeriods(k)
                  .TestDefined().TestStateSinceHeight(0)
                  .Mine(1, TestTime(1), 0x100).TestDefined().TestStateSinceHeight(0)
                  .Mine(1999, TestTime(1999), 0x100).TestDefined().TestStateSinceHeight(0)
                  .Mine(2000, TestTime(2000), 0x100).TestStarted().TestStateSinceHeight(2000)
                  .Mine(2999, TestTime(2999), 0x100).TestStarted().TestStateSinceHeight(2000)
                  .Mine(3000, TestTime(3000), 0x100).TestLockedIn().TestStateSinceHeight(3000);
            for (int period = 1; period < k; period++) {
                // Signalling makes no difference while locked in.
                const int first = 3000 + period * 1000;
                tester.Mine(first - 1, TestTime(first - 1), period % 2 ? 0 : 0x100).TestLockedIn().TestStateSinceHeight(3000)
                      .Mine(first, TestTime(first), 0).TestLockedIn().TestStateSinceHeight(3000);
            }
            const int activation = 3000 + k * 1000;
            tester.Mine(activation - 1, TestTime(activation - 1), 0).TestLockedIn().TestStateSinceHeight(3000)
                  .Mine(activation, TestTime(activation), 0).TestActive().TestStateSinceHeight(activation)
                  .Mine(activation + 1, TestTime(activation + 1), 0).TestActive().TestStateSinceHeight(activation)
                  .Mine(activation + 2000, TestTime(activation + 2000), 0).TestActive().TestStateSinceHeight(activation);

            // Time-based: started from the period after the one whose last block has a median time past of at least
            // TestTime(10000); locked in after a period in which 900 of the 1000 blocks signal.
            tester.Reset().SetLockedInPeriods(k).TestDefined().TestStateSinceHeight(0)
                  .Mine(1000, TestTime(10000) - 1, 0x100).TestDefined().TestStateSinceHeight(0)
                  .Mine(2000, TestTime(10000), 0x100).TestStarted().TestStateSinceHeight(2000)
                  .Mine(2100, TestTime(10001), 0).TestStarted().TestStateSinceHeight(2000)
                  .Mine(3000, TestTime(10002), 0x100).TestLockedIn().TestStateSinceHeight(3000);
            for (int period = 1; period < k; period++) {
                const int first = 3000 + period * 1000;
                // Past the timeout time: a locked-in deployment does not fail.
                tester.Mine(first, TestTime(20000 + period), 0).TestLockedIn().TestStateSinceHeight(3000);
            }
            tester.Mine(activation - 1, TestTime(30000), 0).TestLockedIn().TestStateSinceHeight(3000)
                  .Mine(activation, TestTime(30001), 0).TestActive().TestStateSinceHeight(activation)
                  .Mine(activation + 3000, TestTime(30002), 0).TestActive().TestStateSinceHeight(activation);

            // Locked in at the timeout height without signalling (height-based), then k periods of LOCKED_IN.
            tester.Reset().SetCheckerHeightBasedTest(true, 2000, 4000).SetLockedInPeriods(k)
                  .Mine(2000, TestTime(2000), 0).TestStarted().TestStateSinceHeight(2000)
                  .Mine(3999, TestTime(3999), 0).TestStarted().TestStateSinceHeight(2000)
                  .Mine(4000, TestTime(4000), 0).TestLockedIn().TestStateSinceHeight(4000)
                  .Mine(4000 + k * 1000 - 1, TestTime(4000 + k * 1000 - 1), 0).TestLockedIn().TestStateSinceHeight(4000)
                  .Mine(4000 + k * 1000, TestTime(4000 + k * 1000), 0).TestActive().TestStateSinceHeight(4000 + k * 1000);
        }
    }
}

namespace {

/** A threshold checker with every parameter set by the test, and its own cache. Height-based when height_based,
 *  otherwise based on median time past; start and timeout are heights or times accordingly. */
struct ParamChecker : public AbstractThresholdConditionChecker {
    int period{10};
    int threshold{8};
    int locked_in_periods{1};
    bool height_based{true};
    int64_t start{0};
    int64_t timeout{0};
    mutable ThresholdConditionCache cache;

    int64_t BeginTime(const Consensus::Params& params) const override { return height_based ? 0 : start; }
    int64_t EndTime(const Consensus::Params& params) const override { return height_based ? 0 : timeout; }
    int64_t BeginHeight(const Consensus::Params& params) const override { return height_based ? start : 0; }
    int64_t EndHeight(const Consensus::Params& params) const override { return height_based ? timeout : 0; }
    int Period(const Consensus::Params& params) const override { return period; }
    int Threshold(const Consensus::Params& params) const override { return threshold; }
    int LockedInPeriods(const Consensus::Params& params) const override { return locked_in_periods; }
    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override { return pindex->nVersion & 0x100; }

    ThresholdState State(const CBlockIndex* pindexPrev) const { return GetStateFor(pindexPrev, paramsDummy, cache); }
    int Since(const CBlockIndex* pindexPrev) const { return GetStateSinceHeightFor(pindexPrev, paramsDummy, cache); }
    /** The same parameters with an empty cache. */
    ParamChecker Cold() const
    {
        ParamChecker copy{*this};
        copy.cache.clear();
        return copy;
    }
};

/** Owns block index entries of any number of branches. */
class BlockTree
{
    std::vector<std::unique_ptr<CBlockIndex>> m_blocks;

public:
    /** Append blocks to a branch, given as the block at each height, with these versions and times. */
    void Extend(std::vector<const CBlockIndex*>& branch, const std::vector<int32_t>& versions, const std::vector<uint32_t>& times)
    {
        assert(versions.size() == times.size());
        for (size_t i = 0; i < versions.size(); ++i) {
            m_blocks.push_back(MakeUnique<CBlockIndex>());
            CBlockIndex* block = m_blocks.back().get();
            block->pprev = branch.empty() ? nullptr : const_cast<CBlockIndex*>(branch.back());
            block->nHeight = branch.size();
            block->nVersion = versions[i];
            block->nTime = times[i];
            block->BuildSkip();
            branch.push_back(block);
        }
    }
};

/** The state of each period of a branch (index n: the state of blocks n * period to n * period + period - 1),
 *  computed directly from the rules, one period after another from the genesis block, for every period whose
 *  first block is at most one above the branch's last block. LOCKED_IN turns into ACTIVE once the states
 *  so far end with locked_in_periods LOCKED_IN periods. */
std::vector<ThresholdState> ReferenceStates(const ParamChecker& c, const std::vector<const CBlockIndex*>& branch)
{
    std::vector<ThresholdState> states{ThresholdState::DEFINED};
    for (size_t first = c.period; first <= branch.size(); first += c.period) {
        const CBlockIndex* last = branch[first - 1];
        const int64_t progress = c.height_based ? int64_t(first) : last->GetMedianTimePast();
        ThresholdState state = states.back();
        switch (state) {
        case ThresholdState::DEFINED:
            if (progress >= c.start) state = ThresholdState::STARTED;
            break;
        case ThresholdState::STARTED: {
            int count = 0;
            for (size_t height = first - c.period; height < first; ++height) {
                if (branch[height]->nVersion & 0x100) ++count;
            }
            if (count >= c.threshold) {
                state = ThresholdState::LOCKED_IN;
            } else if (progress >= c.timeout) {
                state = c.height_based ? ThresholdState::LOCKED_IN : ThresholdState::FAILED;
            }
            break;
        }
        case ThresholdState::LOCKED_IN: {
            int locked_in = 0;
            for (auto it = states.rbegin(); it != states.rend() && *it == ThresholdState::LOCKED_IN; ++it) ++locked_in;
            if (locked_in >= c.locked_in_periods) state = ThresholdState::ACTIVE;
            break;
        }
        case ThresholdState::ACTIVE:
        case ThresholdState::FAILED:
            break;
        }
        states.push_back(state);
    }
    return states;
}

/** The since height that GetStateSinceHeightFor gives for period n of states: 0 for DEFINED, otherwise the first
 *  block of the earliest period of the run of equal states that ends at period n. */
int ReferenceSince(const std::vector<ThresholdState>& states, size_t n, int period)
{
    if (states[n] == ThresholdState::DEFINED) return 0;
    while (n > 1 && states[n - 1] == states[n]) --n;
    return n * period;
}

/** Check that every block of a branch, from the genesis block to the one above its last block, gets the reference
 *  state and since height: with a cold cache for each query, and from caches filled in ascending and in random
 *  order. The caches of warm and shuffled are kept, so that several branches can share them. */
void CheckBranch(const ParamChecker& warm, const ParamChecker& shuffled, const std::vector<const CBlockIndex*>& branch, const std::string& context)
{
    const std::vector<ThresholdState> states = ReferenceStates(warm, branch);
    const auto parent = [&](size_t height) { return height == 0 ? nullptr : branch[height - 1]; };
    std::vector<size_t> heights;
    for (size_t height = 0; height <= branch.size(); ++height) heights.push_back(height);
    std::vector<size_t> random_order{heights};
    Shuffle(random_order.begin(), random_order.end(), g_insecure_rand_ctx);
    for (size_t i = 0; i < heights.size(); ++i) {
        for (const auto& query : {std::make_pair(&warm, heights[i]), std::make_pair(&shuffled, random_order[i])}) {
            const size_t height = query.second;
            const size_t n = height / warm.period;
            const ParamChecker cold = query.first->Cold();
            const ThresholdState expected = states[n];
            const int expected_since = ReferenceSince(states, n, warm.period);
            for (const ParamChecker* checker : {query.first, &cold}) {
                const ThresholdState got = checker->State(parent(height));
                BOOST_CHECK_MESSAGE(got == expected, strprintf("%s: block %d is %s, expected %s%s", context, height, StateName(got), StateName(expected), checker == &cold ? " (cold cache)" : ""));
                const int since = checker->Since(parent(height));
                BOOST_CHECK_MESSAGE(since == expected_since, strprintf("%s: block %d since %d, expected %d%s", context, height, since, expected_since, checker == &cold ? " (cold cache)" : ""));
            }
        }
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(versionbits_locked_in_periods_reference)
{
    // Random branches of signalling and non-signalling blocks: GetStateFor and GetStateSinceHeightFor agree with
    // a direct computation of the rules, whatever the cache holds, for one and for several locked-in periods,
    // height-based and time-based (start and timeout times that the median time past passes at random blocks).
    for (const int k : {1, 2, 3, 7}) {
        for (const bool height_based : {true, false}) {
            for (int trial = 0; trial < 40; ++trial) {
                ParamChecker checker;
                checker.locked_in_periods = k;
                checker.height_based = height_based;
                const int periods = 30 + InsecureRandRange(20);
                const int length = periods * checker.period + InsecureRandRange(checker.period);
                // Signalling share between a half and all blocks, so that some periods lock in and some do not.
                const int share = 8 + InsecureRandRange(9);
                std::vector<int32_t> versions;
                std::vector<uint32_t> times;
                uint32_t time = 1000000;
                for (int height = 0; height < length; ++height) {
                    versions.push_back(InsecureRandRange(16) < (uint64_t)share ? 0x20000100 : 0x20000000);
                    time += 1 + InsecureRandRange(1200);
                    times.push_back(time);
                }
                if (height_based) {
                    checker.start = checker.period * InsecureRandRange(10);
                    checker.timeout = InsecureRandBool() ? 99999999 : checker.start + checker.period * InsecureRandRange(20);
                } else {
                    checker.start = times[InsecureRandRange(length / 3)];
                    checker.timeout = InsecureRandBool() ? std::numeric_limits<int64_t>::max() : checker.start + 600 * InsecureRandRange(length);
                }
                BlockTree tree;
                std::vector<const CBlockIndex*> branch;
                tree.Extend(branch, versions, times);
                const ParamChecker shuffled = checker.Cold();
                CheckBranch(checker, shuffled, branch, strprintf("k %d, %s, chain %d", k, height_based ? "heights" : "times", trial));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(versionbits_locked_in_periods_large)
{
    // A number of locked-in periods whose product with the period does not fit in 32 bits: 16 * (2^28 + 2) is
    // 2^32 + 32, which is two periods when the product wraps. The deployment stays locked in for the whole chain.
    ParamChecker checker;
    checker.period = 16;
    checker.threshold = 13;
    checker.locked_in_periods = (1 << 28) + 2;
    checker.start = 0;
    checker.timeout = 99999999;
    const int length = 12 * checker.period;
    BlockTree tree;
    std::vector<const CBlockIndex*> branch;
    tree.Extend(branch, std::vector<int32_t>(length, 0x20000100), std::vector<uint32_t>(length, 1000000));
    const ParamChecker shuffled = checker.Cold();
    CheckBranch(checker, shuffled, branch, "large number of locked-in periods");
    BOOST_CHECK(checker.State(branch.back()) == ThresholdState::LOCKED_IN);
}

BOOST_AUTO_TEST_CASE(versionbits_locked_in_periods_default)
{
    // A checker that does not override LockedInPeriods, like the one behind the warning about unknown new rules,
    // keeps the single locked-in period of BIP 9: ACTIVE from the period after the one that is LOCKED_IN.
    struct DefaultChecker : public AbstractThresholdConditionChecker {
        int64_t BeginTime(const Consensus::Params& params) const override { return 0; }
        int64_t EndTime(const Consensus::Params& params) const override { return 0; }
        int64_t BeginHeight(const Consensus::Params& params) const override { return 10; }
        int64_t EndHeight(const Consensus::Params& params) const override { return 99999999; }
        int Period(const Consensus::Params& params) const override { return 10; }
        int Threshold(const Consensus::Params& params) const override { return 8; }
        bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const override { return pindex->nVersion & 0x100; }
    };
    const DefaultChecker checker;
    BlockTree tree;
    std::vector<const CBlockIndex*> branch;
    tree.Extend(branch, std::vector<int32_t>(40, 0x20000100), std::vector<uint32_t>(40, 1000000));
    ThresholdConditionCache cache;
    // The state of the block at a height, from its parent: started at 10, locked in from 20, active from 30.
    const auto state = [&](int height) { return checker.GetStateFor(branch[height - 1], paramsDummy, cache); };
    BOOST_CHECK(state(9) == ThresholdState::DEFINED);
    BOOST_CHECK(state(10) == ThresholdState::STARTED);
    BOOST_CHECK(state(20) == ThresholdState::LOCKED_IN);
    BOOST_CHECK(state(29) == ThresholdState::LOCKED_IN);
    BOOST_CHECK(state(30) == ThresholdState::ACTIVE);
    BOOST_CHECK(state(39) == ThresholdState::ACTIVE);
    BOOST_CHECK_EQUAL(checker.GetStateSinceHeightFor(branch[38], paramsDummy, cache), 30);
}

BOOST_AUTO_TEST_CASE(versionbits_locked_in_periods_branches)
{
    // Branches that share a cache: a branch that locks in one period later activates one period later, and a branch
    // that leaves the first while it is locked in activates at the same height as the first.
    for (const int k : {1, 2, 3, 5}) {
        ParamChecker warm;
        warm.locked_in_periods = k;
        warm.start = 20;
        warm.timeout = 99999999;
        const ParamChecker shuffled = warm.Cold();
        const int p = warm.period;
        const auto blocks = [](int n, int32_t version, uint32_t time) {
            return std::make_pair(std::vector<int32_t>(n, version), std::vector<uint32_t>(n, time));
        };
        BlockTree tree;
        // Common blocks 0 to 19; period 2 (blocks 20 to 29) is the first STARTED one.
        std::vector<const CBlockIndex*> common;
        auto b = blocks(2 * p, 0x20000100, 1);
        tree.Extend(common, b.first, b.second);
        // Branch a signals in period 2, so periods 3 to 2 + k are LOCKED_IN and 3 + k is ACTIVE.
        std::vector<const CBlockIndex*> a{common};
        b = blocks((4 + k) * p, 0x20000100, 2);
        tree.Extend(a, b.first, b.second);
        // Branch c branches off a after the last block of period 3, during the locked-in periods, and does not signal.
        std::vector<const CBlockIndex*> c(a.begin(), a.begin() + 4 * p);
        b = blocks((1 + k) * p, 0x20000000, 3);
        tree.Extend(c, b.first, b.second);
        // Branch d does not signal in period 2 and signals from period 3 on, so it locks in one period later.
        std::vector<const CBlockIndex*> d{common};
        b = blocks(p, 0x20000000, 4);
        tree.Extend(d, b.first, b.second);
        b = blocks((4 + k) * p, 0x20000100, 4);
        tree.Extend(d, b.first, b.second);

        const std::string context = strprintf("k %d", k);
        // Interleave the branches in the shared caches: a, then d, then c, then a again.
        CheckBranch(warm, shuffled, a, context + ", first branch");
        CheckBranch(warm, shuffled, d, context + ", branch that locks in later");
        CheckBranch(warm, shuffled, c, context + ", branch that leaves the first while locked in");
        CheckBranch(warm, shuffled, a, context + ", first branch again");

        // The states that the reference gives, written out.
        const auto ref_a = ReferenceStates(warm, a);
        const auto ref_c = ReferenceStates(warm, c);
        const auto ref_d = ReferenceStates(warm, d);
        BOOST_CHECK(ref_a[2] == ThresholdState::STARTED && ref_d[2] == ThresholdState::STARTED && ref_d[3] == ThresholdState::STARTED);
        for (int n = 3; n < 3 + k; ++n) {
            BOOST_CHECK(ref_a[n] == ThresholdState::LOCKED_IN);
            BOOST_CHECK(ref_c[n] == ThresholdState::LOCKED_IN);
            BOOST_CHECK(ref_d[n + 1] == ThresholdState::LOCKED_IN);
        }
        BOOST_CHECK(ref_a[3 + k] == ThresholdState::ACTIVE);
        BOOST_CHECK(ref_c[3 + k] == ThresholdState::ACTIVE);
        BOOST_CHECK(ref_d[3 + k] == ThresholdState::LOCKED_IN);
        BOOST_CHECK(ref_d[4 + k] == ThresholdState::ACTIVE);
        BOOST_CHECK(warm.State(a[(3 + k) * p - 1]) == ThresholdState::ACTIVE);
        BOOST_CHECK(warm.State(d[(3 + k) * p - 1]) == ThresholdState::LOCKED_IN);
        BOOST_CHECK_EQUAL(warm.Since(d[(3 + k) * p - 1]), 4 * p);
    }
}

BOOST_AUTO_TEST_CASE(versionbits_locked_in_periods_block_version)
{
    // Regtest with -vbparams=p2mr:0:0:144:99999999:<k>, blocks that signal: ComputeBlockVersion sets the bit for
    // the started period and for all k locked-in periods, and not once the deployment is active.
    for (const int k : {1, 3}) {
        ArgsManager args;
        args.ForceSetArg("-vbparams", strprintf("p2mr:0:0:144:99999999:%d", k));
        const auto chain_params = CreateChainParams(args, CBaseChainParams::REGTEST);
        const Consensus::Params& params = chain_params->GetConsensus();
        const Consensus::DeploymentPos pos = Consensus::DEPLOYMENT_P2MR;
        const int32_t mask = VersionBitsMask(params, pos);
        const int period = params.nMinerConfirmationWindow;
        BOOST_REQUIRE_EQUAL(params.vDeployments[pos].nLockedInPeriods, k);
        VersionBitsTester chain;
        versionbitscache.Clear();
        const CBlockIndex* tip = nullptr;
        for (int height = 0; height <= (3 + k) * period; ++height) {
            // The state and the block version of block height.
            const ThresholdState state = VersionBitsState(tip, params, pos, versionbitscache);
            const ThresholdState expected = height < period ? ThresholdState::DEFINED :
                                            height < 2 * period ? ThresholdState::STARTED :
                                            height < (2 + k) * period ? ThresholdState::LOCKED_IN : ThresholdState::ACTIVE;
            BOOST_CHECK_MESSAGE(state == expected, strprintf("k %d: block %d is %s", k, height, StateName(state)));
            const bool signals = (ComputeBlockVersion(tip, params) & mask) != 0;
            BOOST_CHECK_MESSAGE(signals == (expected == ThresholdState::STARTED || expected == ThresholdState::LOCKED_IN),
                                strprintf("k %d: block %d sets the bit: %d", k, height, signals));
            tip = chain.Mine(height + 1, 100000, VERSIONBITS_TOP_BITS | mask).Tip();
        }
        versionbitscache.Clear();
    }
}

BOOST_AUTO_TEST_CASE(versionbits_sanity)
{
    // Sanity checks of version bit deployments
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params &mainnetParams = chainParams->GetConsensus();
    for (int i=0; i<(int) Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        uint32_t bitmask = VersionBitsMask(mainnetParams, static_cast<Consensus::DeploymentPos>(i));
        // Make sure that no deployment tries to set an invalid bit.
        BOOST_CHECK_EQUAL(bitmask & ~(uint32_t)VERSIONBITS_TOP_MASK, bitmask);

        // Check start height is on a retarget boundary
        BOOST_CHECK_EQUAL(mainnetParams.vDeployments[i].nStartHeight % mainnetParams.nMinerConfirmationWindow, 0U);
        // Check start_height is 0 for ALWAYS_ACTIVE and never active deployments
        if (mainnetParams.vDeployments[i].nStartTime == Consensus::BIP9Deployment::ALWAYS_ACTIVE || mainnetParams.vDeployments[i].nStartTime == Consensus::BIP9Deployment::NEVER_ACTIVE) {
            BOOST_CHECK_EQUAL(mainnetParams.vDeployments[i].nStartHeight, 0);
            BOOST_CHECK_EQUAL(mainnetParams.vDeployments[i].nTimeoutHeight, 0);
        }

        // Verify that the deployment windows of different deployment using the
        // same bit are disjoint.
        // This test may need modification at such time as a new deployment
        // is proposed that reuses the bit of an activated soft fork, before the
        // end time of that soft fork.  (Alternatively, the end time of that
        // activated soft fork could be later changed to be earlier to avoid
        // overlap.)
        for (int j=i+1; j<(int) Consensus::MAX_VERSION_BITS_DEPLOYMENTS; j++) {
            if (VersionBitsMask(mainnetParams, static_cast<Consensus::DeploymentPos>(j)) == bitmask) {
                BOOST_CHECK(mainnetParams.vDeployments[j].nStartTime > mainnetParams.vDeployments[i].nTimeout ||
                        mainnetParams.vDeployments[i].nStartTime > mainnetParams.vDeployments[j].nTimeout);
            }
        }
    }
}

/** Check that ComputeBlockVersion will set the appropriate bit correctly for BIP8 (height-based) activation */
static void check_computeblockversion_bip8(const Consensus::Params& params, Consensus::DeploymentPos dep)
{
    // nStartTime and nTimeout must be set to 0 to use BIP8
    BOOST_CHECK(params.vDeployments[dep].nStartTime == 0 && params.vDeployments[dep].nTimeout == 0);

    int64_t bit = params.vDeployments[dep].bit;
    int32_t bit_mask = (1 << bit);
    int64_t nStartHeight = params.vDeployments[dep].nStartHeight;
    int64_t nTimeoutHeight = params.vDeployments[dep].nTimeoutHeight;
    uint32_t nMinerConfirmationWindow = params.nMinerConfirmationWindow;
    // The deployment stays locked in for this many blocks.
    const uint32_t locked_in_blocks = nMinerConfirmationWindow * params.vDeployments[dep].nLockedInPeriods;

    // should not be any signalling for first block
    BOOST_CHECK_EQUAL(ComputeBlockVersion(nullptr, params), VERSIONBITS_TOP_BITS);

    BOOST_CHECK(nStartHeight >= 0);
    BOOST_CHECK_EQUAL(nStartHeight % nMinerConfirmationWindow, 0U);

    BOOST_CHECK(nTimeoutHeight <= std::numeric_limits<uint32_t>::max());
    BOOST_CHECK_EQUAL(nTimeoutHeight % nMinerConfirmationWindow, 0U);

    BOOST_CHECK(0 <= bit && bit < 32);
    BOOST_CHECK((bit_mask & VERSIONBITS_TOP_MASK) == 0);

    int64_t nTime = 100000;

    const CBlockIndex* lastBlock = nullptr;

    {
        // In the first chain, we don't set the bit, and wait for forced activation via timeout.
        // The bit should be set by CBV during range [nStartHeight, nTimeoutHeight + nMinerConfirmationWindow)
        VersionBitsTester chain;
        versionbitscache.Clear();

        // Bit should not be set before nStartHeight
        lastBlock = chain.Mine(nStartHeight - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & bit_mask, 0);

        // Once we hit nStartHeight, the feature switches to STARTED, we should be able to signal for activation.
        for (uint32_t i = nStartHeight; i < nTimeoutHeight; i++) {
            lastBlock = chain.Mine(i, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & bit_mask) != 0);
            BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);
            BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::STARTED);
        }

        // Once the timeout height is hit, the feature switches to LOCKED_IN for its locked-in periods
        for (uint32_t i = 0; i < locked_in_blocks; i++) {
            lastBlock = chain.Mine(nTimeoutHeight + i, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & bit_mask) != 0);
            BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);
            BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::LOCKED_IN);
        }

        // After the locked-in periods, the feature should be ACTIVE, so we should stop setting the bit.
        lastBlock = chain.Mine(nTimeoutHeight + locked_in_blocks, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & bit_mask, 0);
        BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::ACTIVE);
    }

    {
        // In the second chain, we set the bit to activate before forced activation at nTimeoutHeight.
        // We will signal for the feature during the first confirmation window.
        // It should be LOCKED-IN for the 2nd window, after which it switches to ACTIVE.
        // The bit should be set by CBV while STARTED and LOCKED-IN, and then no longer set while ACTIVE.
        // That is, it should be set by CBV during range [nStartHeight, nStartHeight + 2 * nMinerConfirmationWindow)
        VersionBitsTester chain;
        versionbitscache.Clear();

        // Bit should not be set before nStartHeight
        lastBlock = chain.Mine(nStartHeight - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & bit_mask, 0);
        BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::DEFINED);

        // Once we hit nStartHeight, the feature switches to STARTED, and we start signaling for activation.
        for (uint32_t i = 0; i < nMinerConfirmationWindow; i++) {
            lastBlock = chain.Mine(nStartHeight + i, nTime, VERSIONBITS_TOP_BITS | bit_mask).Tip();
            BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & bit_mask) != 0);
            BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);
            BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::STARTED);
        }

        // After the first confirmation window, the feature switches to LOCKED-IN for its locked-in periods.
        for (uint32_t i = 0; i < locked_in_blocks; i++) {
            lastBlock = chain.Mine(nStartHeight + nMinerConfirmationWindow + i, nTime, VERSIONBITS_TOP_BITS | bit_mask).Tip();
            BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & bit_mask) != 0);
            BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);
            BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::LOCKED_IN);
        }

        // After the LOCKED-IN periods, the feature should be ACTIVE, so we should stop setting the bit.
        lastBlock = chain.Mine(nStartHeight + nMinerConfirmationWindow + locked_in_blocks, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & bit_mask, 0);
        BOOST_CHECK_EQUAL(VersionBitsState(lastBlock, params, dep, versionbitscache), ThresholdState::ACTIVE);
    }
}

/** Check that ComputeBlockVersion will set the appropriate bit correctly */
static void check_computeblockversion(const Consensus::Params& params, Consensus::DeploymentPos dep)
{
    // This implicitly uses versionbitscache, so clear it every time
    versionbitscache.Clear();

    int64_t bit = params.vDeployments[dep].bit;
    int64_t nStartTime = params.vDeployments[dep].nStartTime;
    int64_t nTimeout = params.vDeployments[dep].nTimeout;

    if (nStartTime == 0 && nTimeout == 0) {
        return check_computeblockversion_bip8(params, dep);
    }

    // should not be any signalling for first block
    BOOST_CHECK_EQUAL(ComputeBlockVersion(nullptr, params), VERSIONBITS_TOP_BITS);

    // always/never active deployments shouldn't need to be tested further
    if (nStartTime == Consensus::BIP9Deployment::ALWAYS_ACTIVE) return;
    if (nStartTime == Consensus::BIP9Deployment::NEVER_ACTIVE) return;

    BOOST_CHECK(nStartTime < nTimeout);
    BOOST_CHECK(nStartTime >= 0);
    BOOST_CHECK(nTimeout <= std::numeric_limits<uint32_t>::max() || nTimeout == Consensus::BIP9Deployment::NO_TIMEOUT);
    BOOST_CHECK(0 <= bit && bit < 32);
    BOOST_CHECK(((1 << bit) & VERSIONBITS_TOP_MASK) == 0);

    // In the first chain, test that the bit is set by CBV until it has failed.
    // In the second chain, test the bit is set by CBV while STARTED and
    // LOCKED-IN, and then no longer set while ACTIVE.
    VersionBitsTester firstChain, secondChain;

    int64_t nTime = nStartTime;

    const CBlockIndex *lastBlock = nullptr;

    // Before MedianTimePast of the chain has crossed nStartTime, the bit
    // should not be set.
    if (nTime == 0) {
        // since CBlockIndex::nTime is uint32_t we can't represent any
        // earlier time, so will transition from DEFINED to STARTED at the
        // end of the first period by mining blocks at nTime == 0
        lastBlock = firstChain.Mine(params.nMinerConfirmationWindow - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & (1<<bit), 0);
        lastBlock = firstChain.Mine(params.nMinerConfirmationWindow, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);
        // then we'll keep mining at nStartTime...
    } else {
        // use a time 1s earlier than start time to check we stay DEFINED
        --nTime;

        // Start generating blocks before nStartTime
        lastBlock = firstChain.Mine(params.nMinerConfirmationWindow, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & (1<<bit), 0);

        // Mine more blocks (4 less than the adjustment period) at the old time, and check that CBV isn't setting the bit yet.
        for (uint32_t i = 1; i < params.nMinerConfirmationWindow - 4; i++) {
            lastBlock = firstChain.Mine(params.nMinerConfirmationWindow + i, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & (1<<bit), 0);
        }
        // Now mine 5 more blocks at the start time -- MTP should not have passed yet, so
        // CBV should still not yet set the bit.
        nTime = nStartTime;
        for (uint32_t i = params.nMinerConfirmationWindow - 4; i <= params.nMinerConfirmationWindow; i++) {
            lastBlock = firstChain.Mine(params.nMinerConfirmationWindow + i, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & (1<<bit), 0);
        }
        // Next we will advance to the next period and transition to STARTED,
    }

    lastBlock = firstChain.Mine(params.nMinerConfirmationWindow * 3, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    // so ComputeBlockVersion should now set the bit,
    BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);
    // and should also be using the VERSIONBITS_TOP_BITS.
    BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);

    // Check that ComputeBlockVersion will set the bit until nTimeout
    nTime += 600;
    uint32_t blocksToMine = params.nMinerConfirmationWindow * 2; // test blocks for up to 2 time periods
    uint32_t nHeight = params.nMinerConfirmationWindow * 3;
    // These blocks are all before nTimeout is reached.
    while (nTime < nTimeout && blocksToMine > 0) {
        lastBlock = firstChain.Mine(nHeight+1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & VERSIONBITS_TOP_MASK, VERSIONBITS_TOP_BITS);
        blocksToMine--;
        nTime += 600;
        nHeight += 1;
    }

    if (nTimeout != Consensus::BIP9Deployment::NO_TIMEOUT) {
        // can reach any nTimeout other than NO_TIMEOUT due to earlier BOOST_REQUIRE

        nTime = nTimeout;

        // finish the last period before we start timing out
        while (nHeight % params.nMinerConfirmationWindow != 0) {
            lastBlock = firstChain.Mine(nHeight+1, nTime - 1, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);
            nHeight += 1;
        }

        // FAILED is only triggered at the end of a period, so CBV should be setting
        // the bit until the period transition.
        for (uint32_t i = 0; i < params.nMinerConfirmationWindow - 1; i++) {
            lastBlock = firstChain.Mine(nHeight+1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
            BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);
            nHeight += 1;
        }
        // The next block should trigger no longer setting the bit.
        lastBlock = firstChain.Mine(nHeight+1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
        BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & (1<<bit), 0);
    }

    // On a new chain:
    // verify that the bit will be set after lock-in, and then stop being set
    // after activation.
    nTime = nStartTime;

    // Mine one period worth of blocks, and check that the bit will be on for the
    // next period.
    lastBlock = secondChain.Mine(params.nMinerConfirmationWindow, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);

    // Mine another period worth of blocks, signaling the new bit.
    lastBlock = secondChain.Mine(params.nMinerConfirmationWindow * 2, nTime, VERSIONBITS_TOP_BITS | (1<<bit)).Tip();
    // After one period of setting the bit on each block, it should have locked in.
    // We keep setting the bit for one more period though, until activation.
    BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1<<bit)) != 0);

    // Now check that we keep mining the block until the end of this period, and
    // then stop at the beginning of the next period.
    lastBlock = secondChain.Mine((params.nMinerConfirmationWindow * 3) - 1, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();
    BOOST_CHECK((ComputeBlockVersion(lastBlock, params) & (1 << bit)) != 0);
    lastBlock = secondChain.Mine(params.nMinerConfirmationWindow * 3, nTime, VERSIONBITS_LAST_OLD_BLOCK_VERSION).Tip();

    // Check that we don't signal after activation
    BOOST_CHECK_EQUAL(ComputeBlockVersion(lastBlock, params) & (1<<bit), 0);
}

BOOST_AUTO_TEST_CASE(versionbits_computeblockversion)
{
    // check that any deployment on any chain can conceivably reach both
    // ACTIVE and FAILED states in roughly the way we expect
    for (const auto& chain_name : { CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto chainParams = CreateChainParams(*m_node.args, chain_name);
        for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++i) {
            check_computeblockversion(chainParams->GetConsensus(), static_cast<Consensus::DeploymentPos>(i));
        }
    }

    {
        // Use regtest/testdummy to ensure we always exercise some
        // deployment that's not always/never active
        ArgsManager args;
        args.ForceSetArg("-vbparams", "testdummy:1199145601:1230767999"); // January 1, 2008 - December 31, 2008
        const auto chainParams = CreateChainParams(args, CBaseChainParams::REGTEST);
        check_computeblockversion(chainParams->GetConsensus(), Consensus::DEPLOYMENT_TESTDUMMY);
    }

}

BOOST_AUTO_TEST_SUITE_END()