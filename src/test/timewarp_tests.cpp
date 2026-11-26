// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <miner.h>
#include <pow.h>
#include <primitives/block.h>
#include <random.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>
#include <versionbits.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

/** Regtest chain parameters, with one -vbparams argument unless it is empty. */
std::unique_ptr<const CChainParams> RegtestParams(const std::string& vbparams)
{
    ArgsManager args;
    if (!vbparams.empty()) args.ForceSetArg("-vbparams", vbparams);
    return CreateChainParams(args, CBaseChainParams::REGTEST);
}

/** Clears the global version bits cache when it goes out of scope, however the scope is left. cs_main must be held. */
struct ScopedVersionBitsCacheClear {
    ~ScopedVersionBitsCacheClear() { versionbitscache.Clear(); }
};

/** A chain of block index entries, heights 0 to times.size() - 1, with these times. No block signals. */
std::vector<std::unique_ptr<CBlockIndex>> IndexChain(const std::vector<int64_t>& times)
{
    std::vector<std::unique_ptr<CBlockIndex>> chain;
    for (size_t height = 0; height < times.size(); ++height) {
        chain.push_back(std::make_unique<CBlockIndex>());
        CBlockIndex& index = *chain.back();
        index.nHeight = height;
        index.pprev = height > 0 ? chain[height - 1].get() : nullptr;
        index.nVersion = VERSIONBITS_TOP_BITS;
        index.nTime = times[height];
        index.BuildSkip();
    }
    return chain;
}

/** A node on regtest with the given arguments, and a mock clock. */
struct TimewarpChainSetup : public TestingSetup {
    const CScript m_coinbase_script{CScript() << OP_TRUE};

    explicit TimewarpChainSetup(const std::vector<const char*>& extra_args)
        : TestingSetup(CBaseChainParams::REGTEST, extra_args) {}

    ~TimewarpChainSetup() { SetMockTime(0); }

    const Consensus::Params& Consensus() const { return Params().GetConsensus(); }

    const CBlockIndex* Tip()
    {
        LOCK(cs_main);
        return ::ChainActive().Tip();
    }

    int64_t MinimumTime(const CBlockIndex* prev)
    {
        LOCK(cs_main);
        return GetMinimumBlockTime(prev, Consensus());
    }

    /** A block template for the next block, made at the current clock, with its merkle root. Throws if the node
     *  cannot make one. */
    CBlock Template()
    {
        CBlock block = BlockAssembler(*m_node.mempool, Params()).CreateNewBlock(m_coinbase_script)->block;
        block.hashMerkleRoot = BlockMerkleRoot(block);
        return block;
    }

    void Solve(CBlockHeader& header)
    {
        while (!CheckProofOfWork(header.GetPoWHash(), header.nBits, Consensus())) ++header.nNonce;
    }

    /** A block on the tip from a template made at the current clock, stamped with the given time. */
    CBlock BlockAt(int64_t time)
    {
        CBlock block = Template();
        block.nTime = time;
        {
            LOCK(cs_main);
            block.nBits = GetNextWorkRequired(::ChainActive().Tip(), &block, Consensus());
        }
        Solve(block);
        return block;
    }

    /** Whether the block was connected as the new tip. */
    bool Process(const CBlock& block)
    {
        bool new_block{false};
        Assert(m_node.chainman)->ProcessNewBlock(Params(), std::make_shared<const CBlock>(block), true, &new_block);
        return Tip()->GetBlockHash() == block.GetHash();
    }

    /** Mine a block on the tip stamped with the given time; fails the test if it is not connected. */
    void MineAt(int64_t time)
    {
        const CBlock block = BlockAt(time);
        BOOST_REQUIRE_MESSAGE(Process(block), strprintf("block at height %d with time %d not connected", Tip()->nHeight + 1, time));
    }

    /** A solved header on prev with the given time and a random merkle root. */
    CBlockHeader HeaderAt(const CBlockIndex* prev, int64_t time)
    {
        CBlockHeader header;
        header.nVersion = VERSIONBITS_TOP_BITS;
        header.hashPrevBlock = prev->GetBlockHash();
        header.hashMerkleRoot = InsecureRand256();
        header.nTime = time;
        header.nBits = GetNextWorkRequired(prev, &header, Consensus());
        Solve(header);
        return header;
    }

    /** Offer one header; returns the validation state. */
    BlockValidationState OfferHeader(const CBlockHeader& header)
    {
        BlockValidationState state;
        Assert(m_node.chainman)->ProcessNewBlockHeaders({header}, state, Params());
        return state;
    }

    bool Known(const uint256& hash)
    {
        LOCK(cs_main);
        return LookupBlockIndex(hash) != nullptr;
    }
};

struct ActiveSetup : public TimewarpChainSetup {
    ActiveSetup() : TimewarpChainSetup({"-vbparams=timewarp:-1:0"}) {}
};

struct InactiveSetup : public TimewarpChainSetup {
    InactiveSetup() : TimewarpChainSetup({}) {}
};

/** The rules apply from block 432 (see softfork_params_tests/vbparams_forms). MWEB is switched off, because its
 *  first block needs a peg-in transaction. */
struct ActivationSetup : public TimewarpChainSetup {
    ActivationSetup() : TimewarpChainSetup({"-vbparams=timewarp:0:0", "-vbparams=mweb:-2:0"}) {}
};

constexpr int ACTIVATION_HEIGHT{432};

/** Time of the first block the tests mine, well after the regtest genesis block. */
constexpr int64_t BASE_TIME{1700000000};

void CheckRefused(const BlockValidationState& state, BlockValidationResult result, const std::string& reason, const std::string& context)
{
    BOOST_CHECK_MESSAGE(state.IsInvalid(), context + ": not refused");
    BOOST_CHECK_MESSAGE(state.GetResult() == result, strprintf("%s: result %d, expected %d", context, (int)state.GetResult(), (int)result));
    BOOST_CHECK_MESSAGE(state.GetRejectReason() == reason, strprintf("%s: reason %s, expected %s", context, state.GetRejectReason(), reason));
}

/** The consensus parameters of a network, with the timewarp deployment always active. */
Consensus::Params ConsensusWithTimewarp(const std::string& network)
{
    ArgsManager args;
    Consensus::Params consensus = CreateChainParams(args, network)->GetConsensus();
    consensus.vDeployments[Consensus::DEPLOYMENT_TIMEWARP].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
    consensus.vDeployments[Consensus::DEPLOYMENT_TIMEWARP].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
    return consensus;
}

/** Index entries at heights first_height to first_height + times.size() - 1 with these times and one nBits, linked by
 *  pprev only (no skip pointers, so that the chain need not start at the genesis block). */
std::vector<std::unique_ptr<CBlockIndex>> HighChain(int first_height, const std::vector<int64_t>& times, uint32_t bits)
{
    std::vector<std::unique_ptr<CBlockIndex>> chain;
    for (size_t i = 0; i < times.size(); ++i) {
        chain.push_back(std::make_unique<CBlockIndex>());
        CBlockIndex& index = *chain.back();
        index.nHeight = first_height + i;
        index.pprev = i > 0 ? chain[i - 1].get() : nullptr;
        index.nVersion = VERSIONBITS_TOP_BITS;
        index.nTime = times[i];
        index.nBits = bits;
    }
    return chain;
}

/** nBits of the block after the last entry of a chain with these times (mainnet's difficulty does not read the new
 *  block's own time). */
uint32_t NextBits(int first_height, const std::vector<int64_t>& times, uint32_t bits, const Consensus::Params& consensus)
{
    const auto chain = HighChain(first_height, times, bits);
    CBlockHeader header;
    header.nTime = times.back() + 60;
    return GetNextWorkRequired(chain.back().get(), &header, consensus);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(timewarp_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(minimum_time)
{
    // Chains of index entries with times one second apart (the floor binds from height 24) and sixty seconds apart
    // (the median binds).
    std::vector<int64_t> dense, sparse;
    for (int height = 0; height < 3 * 144 + 30; ++height) {
        dense.push_back(BASE_TIME + height);
        sparse.push_back(BASE_TIME + 60 * height);
    }
    const auto dense_chain = IndexChain(dense);
    const auto sparse_chain = IndexChain(sparse);
    const auto mtp_plus_one = [](const CBlockIndex* prev) { return prev->GetMedianTimePast() + 1; };

    LOCK(cs_main);
    ScopedVersionBitsCacheClear clear_cache_at_end;

    // Never active (the default on regtest, and mainnet, where the deployment is not scheduled): the median time
    // past plus one, at every height.
    for (const std::string& vbparams : {std::string{""}, std::string{"timewarp:-2:0"}}) {
        versionbitscache.Clear();
        const auto params = RegtestParams(vbparams);
        for (const auto& prev : dense_chain) {
            BOOST_CHECK_EQUAL(GetMinimumBlockTime(prev.get(), params->GetConsensus()), mtp_plus_one(prev.get()));
        }
    }
    versionbitscache.Clear();
    for (const auto& prev : dense_chain) {
        BOOST_CHECK_EQUAL(GetMinimumBlockTime(prev.get(), Params().GetConsensus()), mtp_plus_one(prev.get()));
    }

    // Always active.
    versionbitscache.Clear();
    const auto active = RegtestParams("timewarp:-1:0");
    const Consensus::Params& consensus = active->GetConsensus();
    // Below height 24 the floor does not apply. Block 23 would otherwise need t(0) + 480, which is above the median.
    for (int height = 1; height <= 23; ++height) {
        const CBlockIndex* prev = dense_chain[height - 1].get();
        BOOST_CHECK_EQUAL(GetMinimumBlockTime(prev, consensus), mtp_plus_one(prev));
    }
    BOOST_CHECK_GT(dense[0] + TIMEWARP_MIN_SPAN, mtp_plus_one(dense_chain[22].get()));
    // From height 24: the later of the median plus one and the time of the block 23 below plus 480 seconds.
    BOOST_CHECK_EQUAL(GetMinimumBlockTime(dense_chain[23].get(), consensus), dense[1] + 480);
    BOOST_CHECK_EQUAL(GetMinimumBlockTime(dense_chain[24].get(), consensus), dense[2] + 480);
    for (size_t height = 24; height < dense.size(); ++height) {
        const CBlockIndex* prev = dense_chain[height - 1].get();
        BOOST_CHECK_EQUAL(GetMinimumBlockTime(prev, consensus), std::max(mtp_plus_one(prev), dense[height - 23] + 480));
        // Here the floor binds.
        BOOST_CHECK_GT(dense[height - 23] + 480, mtp_plus_one(prev));
        // On the sparse chain the median binds: t(h-23) + 480 is 900 seconds below the median, t(h-6).
        const CBlockIndex* sparse_prev = sparse_chain[height - 1].get();
        BOOST_CHECK_EQUAL(GetMinimumBlockTime(sparse_prev, consensus), mtp_plus_one(sparse_prev));
        BOOST_CHECK_EQUAL(mtp_plus_one(sparse_prev), sparse[height - 6] + 1);
    }

    // Activated by signalling at block 432: the median below it, the floor from it on, also where the 24 blocks
    // straddle the activation height.
    versionbitscache.Clear();
    const auto signalled = RegtestParams("timewarp:0:0");
    for (int height = ACTIVATION_HEIGHT - 30; height <= ACTIVATION_HEIGHT + 25; ++height) {
        const CBlockIndex* prev = dense_chain[height - 1].get();
        const int64_t expected = height < ACTIVATION_HEIGHT ? mtp_plus_one(prev) : dense[height - 23] + 480;
        BOOST_CHECK_MESSAGE(GetMinimumBlockTime(prev, signalled->GetConsensus()) == expected, strprintf("height %d", height));
    }
}

BOOST_AUTO_TEST_CASE(floor_and_difficulty)
{
    // Mainnet's difficulty calculation as it is: from nPowDGWHeight on, no minimum-difficulty rule, retargeting on.
    ArgsManager args;
    const auto main_params = CreateChainParams(args, CBaseChainParams::MAIN);
    const Consensus::Params& consensus = main_params->GetConsensus();
    BOOST_REQUIRE(!consensus.fPowAllowMinDifficultyBlocks);
    BOOST_REQUIRE(!consensus.fPowNoRetargeting);
    BOOST_REQUIRE_EQUAL(consensus.nPowKGWHeight, consensus.nPowDGWHeight);
    const int first = consensus.nPowDGWHeight + 1000;
    const uint32_t bits = 0x1c0fffff;
    BOOST_REQUIRE(UintToArith256(consensus.powLimit) > arith_uint256().SetCompact(bits) * 3);

    // 60 entries a minute apart; index h is the last one, pindexLast of the next block's difficulty.
    std::vector<int64_t> minute;
    for (int i = 0; i < 60; ++i) minute.push_back(BASE_TIME + 60 * i);
    const size_t h = minute.size() - 1;
    const auto with_span = [&](int64_t span) {
        std::vector<int64_t> t = minute;
        t[h] = t[h - TIMEWARP_SPAN_DEPTH] + span;
        return t;
    };
    const auto next = [&](const std::vector<int64_t>& t) { return NextBits(first, t, bits, consensus); };

    // The span t(h) - t(h - 23): 479 and 480 seconds give the same target, 481 another; so do 0 and -7200.
    const uint32_t at_479 = next(with_span(TIMEWARP_MIN_SPAN - 1));
    const uint32_t at_480 = next(with_span(TIMEWARP_MIN_SPAN));
    const uint32_t at_481 = next(with_span(TIMEWARP_MIN_SPAN + 1));
    BOOST_CHECK_EQUAL(at_479, at_480);
    BOOST_CHECK(at_481 != at_480);
    BOOST_CHECK(arith_uint256().SetCompact(at_481) > arith_uint256().SetCompact(at_480));
    BOOST_CHECK_EQUAL(next(with_span(0)), at_480);
    BOOST_CHECK_EQUAL(next(with_span(-7200)), at_480);
    // Then the target is the average target times 480 / 1440 (24 blocks of 60 seconds).
    BOOST_CHECK_EQUAL(at_480, arith_uint256(arith_uint256().SetCompact(bits) * TIMEWARP_MIN_SPAN / ((TIMEWARP_SPAN_DEPTH + 1) * consensus.nPowTargetSpacing)).GetCompact());

    // With a span of 600 seconds: moving t(h - 23) changes the target, moving t(h - 24) or t(h - 22) does not.
    const std::vector<int64_t> base = with_span(600);
    const uint32_t at_600 = next(base);
    for (const int64_t delta : {-100, -1, 1, 100}) {
        std::vector<int64_t> t = base;
        t[h - TIMEWARP_SPAN_DEPTH] += delta;
        BOOST_CHECK_MESSAGE(next(t) != at_600, strprintf("t(h-23) moved by %d", delta));
        t = base;
        t[h - TIMEWARP_SPAN_DEPTH - 1] += delta;
        BOOST_CHECK_MESSAGE(next(t) == at_600, strprintf("t(h-24) moved by %d", delta));
        t = base;
        t[h - TIMEWARP_SPAN_DEPTH + 1] += delta;
        BOOST_CHECK_MESSAGE(next(t) == at_600, strprintf("t(h-22) moved by %d", delta));
    }
    // The calculation reads the 24 blocks h - 23 .. h: the bits of block h - 23 count, those of block h - 24 do not.
    {
        auto chain = HighChain(first, base, bits);
        CBlockHeader header;
        header.nTime = base.back() + 60;
        chain[h - TIMEWARP_SPAN_DEPTH]->nBits = 0x1c07ffff;
        BOOST_CHECK(GetNextWorkRequired(chain.back().get(), &header, consensus) != at_600);
        chain[h - TIMEWARP_SPAN_DEPTH]->nBits = bits;
        chain[h - TIMEWARP_SPAN_DEPTH - 1]->nBits = 0x1c07ffff;
        BOOST_CHECK_EQUAL(GetNextWorkRequired(chain.back().get(), &header, consensus), at_600);
    }

    // With the rule active (in a copy of mainnet's parameters) the floor is 480 seconds after that block 23 below, so
    // the rule tightens the block timestamps the difficulty calculation uses: a block stamped at the floor gives the
    // next block the target of a 480-second span, one second later that of a 481-second span.
    const Consensus::Params active = ConsensusWithTimewarp(CBaseChainParams::MAIN);
    std::vector<int64_t> fast = minute;
    for (size_t i = h - TIMEWARP_SPAN_DEPTH; i < h; ++i) fast[i] = fast[h - TIMEWARP_SPAN_DEPTH] + (i - (h - TIMEWARP_SPAN_DEPTH));
    const auto fast_chain = HighChain(first, fast, bits);
    int64_t floor;
    {
        LOCK(cs_main);
        floor = GetMinimumBlockTime(fast_chain[h - 1].get(), active);
        // Mainnet as joined: not scheduled, so the median time past plus one.
        BOOST_CHECK_EQUAL(GetMinimumBlockTime(fast_chain[h - 1].get(), consensus), fast_chain[h - 1]->GetMedianTimePast() + 1);
    }
    BOOST_CHECK_EQUAL(floor, fast[h - TIMEWARP_SPAN_DEPTH] + TIMEWARP_MIN_SPAN);
    BOOST_CHECK_GT(floor, fast_chain[h - 1]->GetMedianTimePast() + 1);
    std::vector<int64_t> stamped = fast;
    stamped[h] = floor;
    BOOST_CHECK_EQUAL(NextBits(first, stamped, bits, consensus), at_480);
    stamped[h] = floor + 1;
    BOOST_CHECK_EQUAL(NextBits(first, stamped, bits, consensus), at_481);
    stamped[h] = floor - 1;
    BOOST_CHECK_EQUAL(NextBits(first, stamped, bits, consensus), at_480);
}

BOOST_AUTO_TEST_CASE(testnet_update_time)
{
    // Testnet's parameters (minimum-difficulty rule; the difficulty calculation above from height 26), with the rule
    // switched on in a copy.
    const Consensus::Params active = ConsensusWithTimewarp(CBaseChainParams::TESTNET);
    BOOST_REQUIRE(active.fPowAllowMinDifficultyBlocks);
    BOOST_REQUIRE(!active.fPowNoRetargeting);
    const uint32_t bits = 0x1e0fffff;
    const arith_uint256 target = arith_uint256().SetCompact(bits);
    const arith_uint256 limit = UintToArith256(active.powLimit);
    BOOST_REQUIRE(target * 10 < limit);

    // Case 1. 100 blocks a minute apart, then 23 blocks one second apart: the floor binds 458 s after the parent.
    {
        std::vector<int64_t> times;
        for (int i = 0; i < 100; ++i) times.push_back(BASE_TIME + 60 * i);
        for (int i = 1; i <= 23; ++i) times.push_back(times[99] + i);
        auto chain = IndexChain(times);
        for (auto& entry : chain) entry->nBits = bits;
        const CBlockIndex* prev = chain.back().get();
        LOCK(cs_main);
        const int64_t floor = GetMinimumBlockTime(prev, active);
        BOOST_REQUIRE_EQUAL(floor, times[times.size() - TIMEWARP_SPAN_DEPTH] + TIMEWARP_MIN_SPAN);
        BOOST_REQUIRE_EQUAL(floor - prev->GetBlockTime(), 458);
        // The clock one second after the parent: CreateNewBlock sets the clock, UpdateTime raises it to the floor.
        SetMockTime(prev->GetBlockTime() + 1);
        CBlockHeader header;
        header.nTime = GetAdjustedTime();
        header.nBits = GetNextWorkRequired(prev, &header, active);
        const uint32_t bits_at_clock = header.nBits;
        // At the clock the difficulty calculation, with a span of 23 s, gives a third of the target.
        BOOST_CHECK_EQUAL(bits_at_clock, arith_uint256(target * 480 / 1440).GetCompact());
        UpdateTime(&header, active, prev);
        BOOST_CHECK_EQUAL(header.GetBlockTime(), floor);
        BOOST_CHECK_EQUAL(header.nBits, GetNextWorkRequired(prev, &header, active));
        // At the floor (more than 4 minutes after the parent): the minimum-difficulty rule, ten times the target.
        BOOST_CHECK_EQUAL(header.nBits, arith_uint256(target * 10).GetCompact());
        BOOST_CHECK(header.nBits != bits_at_clock);
        // CreateNewBlock's order: UpdateTime, then nBits from GetNextWorkRequired with the updated time. Same value.
        CBlockHeader block;
        block.nTime = GetAdjustedTime();
        UpdateTime(&block, active, prev);
        const uint32_t from_update_time = block.nBits;
        block.nBits = GetNextWorkRequired(prev, &block, active);
        BOOST_CHECK_EQUAL(block.nBits, from_update_time);
        // getblocktemplate's reuse of a template: UpdateTime on a header whose nBits is stale recomputes it.
        CBlockHeader cached;
        cached.nTime = prev->GetBlockTime() + 1;
        cached.nBits = bits_at_clock;
        UpdateTime(&cached, active, prev);
        BOOST_CHECK_EQUAL(cached.GetBlockTime(), floor);
        BOOST_CHECK_EQUAL(cached.nBits, GetNextWorkRequired(prev, &cached, active));
        SetMockTime(0);
    }

    // Case 2. The block 23 below was stamped 7,300 s after its own parent, the 22 blocks after it at about the parent's
    // time + 500: the floor is more than two hours after the parent, and a block stamped at it gets the limit.
    {
        std::vector<int64_t> times;
        for (int i = 0; i < 100; ++i) times.push_back(BASE_TIME + 60 * i);
        times.push_back(times[99] + 7300);
        for (int i = 0; i < 22; ++i) times.push_back(times[99] + 500 + i);
        auto chain = IndexChain(times);
        for (auto& entry : chain) entry->nBits = bits;
        const CBlockIndex* prev = chain.back().get();
        LOCK(cs_main);
        const int64_t floor = GetMinimumBlockTime(prev, active);
        BOOST_REQUIRE_EQUAL(floor, times[100] + TIMEWARP_MIN_SPAN);
        BOOST_REQUIRE_GT(floor, prev->GetBlockTime() + 2 * 60 * 60);
        // Every block of the chain keeps the median rule (each later than the median of the 11 before it).
        for (size_t height = 1; height < chain.size(); ++height) {
            BOOST_CHECK_GT(chain[height]->GetBlockTime(), chain[height - 1]->GetMedianTimePast());
        }
        SetMockTime(prev->GetBlockTime() + 600);
        CBlockHeader header;
        header.nTime = GetAdjustedTime();
        UpdateTime(&header, active, prev);
        BOOST_CHECK_EQUAL(header.GetBlockTime(), floor);
        BOOST_CHECK_EQUAL(header.nBits, GetNextWorkRequired(prev, &header, active));
        BOOST_CHECK_EQUAL(header.nBits, limit.GetCompact());
        SetMockTime(0);
    }
}

BOOST_FIXTURE_TEST_CASE(header_rules, ActiveSetup)
{
    // Blocks 1 to 22, one second apart from the genesis block on.
    const int64_t base = Tip()->GetBlockTime();
    SetMockTime(base);
    for (int height = 1; height <= 22; ++height) MineAt(base + height);

    // Below height 24 the rules are the old ones: block 23 can be stamped at the median plus one, although that is
    // less than 480 seconds after block 0.
    const CBlockIndex* tip = Tip();
    const int64_t mtp_23 = tip->GetMedianTimePast();
    BOOST_CHECK_LT(mtp_23 + 1, tip->GetAncestor(0)->GetBlockTime() + 480);
    BOOST_CHECK_EQUAL(MinimumTime(tip), mtp_23 + 1);
    BOOST_CHECK(OfferHeader(HeaderAt(tip, mtp_23 + 1)).IsValid());
    CheckRefused(OfferHeader(HeaderAt(tip, mtp_23)), BlockValidationResult::BLOCK_INVALID_HEADER, "time-too-old", "block 23 at the median");
    MineAt(mtp_23 + 1);

    // Blocks 24 to 30 at their minimum times.
    while (Tip()->nHeight < 30) MineAt(MinimumTime(Tip()));
    tip = Tip();
    const int64_t mtp = tip->GetMedianTimePast();
    const int64_t floor = MinimumTime(tip);
    BOOST_REQUIRE_EQUAL(floor, tip->GetAncestor(tip->nHeight + 1 - 23)->GetBlockTime() + 480);
    BOOST_REQUIRE_GT(floor, mtp + 1);

    // The clock is past the floor.
    const int64_t now = floor + 100;
    SetMockTime(now);
    {
        const CBlockHeader below = HeaderAt(tip, floor - 1);
        CheckRefused(OfferHeader(below), BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "time-timewarp-span", "one second below the floor");
        BOOST_CHECK(!Known(below.GetHash()));
        // Refused again, and still not stored, when offered again.
        CheckRefused(OfferHeader(below), BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "time-timewarp-span", "one second below the floor, again");
        BOOST_CHECK(!Known(below.GetHash()));
    }
    {
        const CBlockHeader at_floor = HeaderAt(tip, floor);
        BOOST_CHECK(OfferHeader(at_floor).IsValid());
        BOOST_CHECK(Known(at_floor.GetHash()));
    }
    // The two-hour limit is unchanged, and lies far above the floor.
    BOOST_CHECK(OfferHeader(HeaderAt(tip, now + MAX_FUTURE_BLOCK_TIME)).IsValid());
    CheckRefused(OfferHeader(HeaderAt(tip, now + MAX_FUTURE_BLOCK_TIME + 1)), BlockValidationResult::BLOCK_TIME_FUTURE, "time-too-new", "past the future limit");
    // A header at the median is refused as too old (and its sender punished), although it is below the floor too.
    {
        const CBlockHeader at_median = HeaderAt(tip, mtp);
        CheckRefused(OfferHeader(at_median), BlockValidationResult::BLOCK_INVALID_HEADER, "time-too-old", "at the median");
        BOOST_CHECK(!Known(at_median.GetHash()));
    }
    // The median plus one is below the floor here.
    CheckRefused(OfferHeader(HeaderAt(tip, mtp + 1)), BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "time-timewarp-span", "at the median plus one");

    // With the clock so far behind that the floor is more than two hours ahead of it, a header at the floor is too
    // new, and one below it is below the floor (the floor is checked first).
    SetMockTime(floor - MAX_FUTURE_BLOCK_TIME - 2);
    CheckRefused(OfferHeader(HeaderAt(tip, floor)), BlockValidationResult::BLOCK_TIME_FUTURE, "time-too-new", "at the floor, clock behind");
    CheckRefused(OfferHeader(HeaderAt(tip, floor - 1)), BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "time-timewarp-span", "below the floor, clock behind");

    // A header received before its block: the block is then accepted, and a block below the floor is refused with
    // the same result.
    SetMockTime(now);
    const CBlock block = BlockAt(floor);
    BOOST_CHECK(OfferHeader(block.GetBlockHeader()).IsValid());
    BOOST_CHECK(Process(block));
    const CBlock below_floor = BlockAt(MinimumTime(Tip()) - 1);
    BOOST_CHECK(!Process(below_floor));
    BOOST_CHECK(!Known(below_floor.GetHash()));
}

BOOST_FIXTURE_TEST_CASE(heights_23_24_25, ActiveSetup)
{
    // Blocks 1 to 22 one second apart: the floor would be above the median at every height from 23 on.
    const int64_t base = Tip()->GetBlockTime();
    SetMockTime(base + 10000);
    for (int height = 1; height <= 22; ++height) MineAt(base + height);
    for (int height = 23; height <= 25; ++height) {
        const CBlockIndex* tip = Tip();
        BOOST_REQUIRE_EQUAL(tip->nHeight + 1, height);
        const int64_t mtp = tip->GetMedianTimePast();
        const int64_t would_be_floor = tip->GetAncestor(height - TIMEWARP_SPAN_DEPTH)->GetBlockTime() + TIMEWARP_MIN_SPAN;
        BOOST_REQUIRE_GT(would_be_floor, mtp + 1);
        const int64_t L = MinimumTime(tip);
        if (height == 23) {
            BOOST_CHECK_EQUAL(L, mtp + 1);
            // One second below what would be the floor, and the median plus one: valid below 24.
            BOOST_CHECK(OfferHeader(HeaderAt(tip, would_be_floor - 1)).IsValid());
            BOOST_CHECK(OfferHeader(HeaderAt(tip, mtp + 1)).IsValid());
            CheckRefused(OfferHeader(HeaderAt(tip, mtp)), BlockValidationResult::BLOCK_INVALID_HEADER, "time-too-old", "height 23 at the median");
        } else {
            BOOST_CHECK_EQUAL(L, would_be_floor);
            const CBlockHeader below = HeaderAt(tip, L - 1);
            CheckRefused(OfferHeader(below), BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "time-timewarp-span", strprintf("height %d at L-1", height));
            BOOST_CHECK(!Known(below.GetHash()));
            CheckRefused(OfferHeader(HeaderAt(tip, mtp)), BlockValidationResult::BLOCK_INVALID_HEADER, "time-too-old", strprintf("height %d at the median", height));
            CheckRefused(OfferHeader(HeaderAt(tip, mtp + 1)), BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE, "time-timewarp-span", strprintf("height %d at the median plus one", height));
            BOOST_CHECK(OfferHeader(HeaderAt(tip, L)).IsValid());
            BOOST_CHECK(OfferHeader(HeaderAt(tip, L + 1)).IsValid());
            // Full blocks too.
            BOOST_CHECK(!Process(BlockAt(L - 1)));
        }
        // The next block of the chain at its minimum time, which is a full block at L.
        MineAt(L);
    }
}

BOOST_FIXTURE_TEST_CASE(header_rules_inactive, InactiveSetup)
{
    // Without the deployment the old rules apply: one second below what would be the floor is valid, and so is the
    // two-hour limit, but not one second more.
    SetMockTime(BASE_TIME);
    for (int height = 1; height <= 30; ++height) MineAt(BASE_TIME + height);
    const CBlockIndex* tip = Tip();
    const int64_t mtp = tip->GetMedianTimePast();
    const int64_t floor = tip->GetAncestor(tip->nHeight + 1 - 23)->GetBlockTime() + 480;
    BOOST_REQUIRE_GT(floor - 1, mtp);
    BOOST_CHECK_EQUAL(MinimumTime(tip), mtp + 1);
    const int64_t now = floor + 100;
    SetMockTime(now);
    BOOST_CHECK(OfferHeader(HeaderAt(tip, floor - 1)).IsValid());
    BOOST_CHECK(OfferHeader(HeaderAt(tip, mtp + 1)).IsValid());
    BOOST_CHECK(OfferHeader(HeaderAt(tip, now + MAX_FUTURE_BLOCK_TIME)).IsValid());
    CheckRefused(OfferHeader(HeaderAt(tip, now + MAX_FUTURE_BLOCK_TIME + 1)), BlockValidationResult::BLOCK_TIME_FUTURE, "time-too-new", "past the future limit");
    CheckRefused(OfferHeader(HeaderAt(tip, mtp)), BlockValidationResult::BLOCK_INVALID_HEADER, "time-too-old", "at the median");
    // The miner stamps the clock, as before.
    BOOST_CHECK_EQUAL(Template().GetBlockTime(), now);
    SetMockTime(mtp - 1000);
    BOOST_CHECK_EQUAL(Template().GetBlockTime(), mtp + 1);
}

BOOST_FIXTURE_TEST_CASE(miner, ActiveSetup)
{
    SetMockTime(BASE_TIME);
    for (int height = 1; height <= 40; ++height) MineAt(std::max(BASE_TIME + height, MinimumTime(Tip())));

    // The clock stays behind the floor: the template's time is the floor, the block passes the node's own checks
    // (CreateNewBlock runs them) and is connected.
    const int64_t clock = Tip()->GetBlockTime();
    SetMockTime(clock);
    for (int i = 0; i < 30; ++i) {
        const CBlockIndex* tip = Tip();
        const int64_t floor = MinimumTime(tip);
        BOOST_REQUIRE_GT(floor, clock);
        BOOST_REQUIRE_GT(floor, tip->GetMedianTimePast() + 1);
        CBlockHeader header;
        header.nTime = 0;
        UpdateTime(&header, Consensus(), tip);
        BOOST_CHECK_EQUAL(header.GetBlockTime(), floor);
        CBlock block = Template();
        BOOST_CHECK_EQUAL(block.GetBlockTime(), floor);
        Solve(block);
        BOOST_CHECK(Process(block));
    }
    // With the clock past the floor, the clock.
    const int64_t floor = MinimumTime(Tip());
    SetMockTime(floor + 10);
    CBlockHeader header;
    header.nTime = 0;
    UpdateTime(&header, Consensus(), Tip());
    BOOST_CHECK_EQUAL(header.GetBlockTime(), floor + 10);
    BOOST_CHECK_EQUAL(Template().GetBlockTime(), floor + 10);
    // UpdateTime never moves a time back.
    header.nTime = floor + 20;
    UpdateTime(&header, Consensus(), Tip());
    BOOST_CHECK_EQUAL(header.GetBlockTime(), floor + 20);

    // With the floor more than two hours ahead of the clock no time is valid, and the template says so.
    SetMockTime(floor - MAX_FUTURE_BLOCK_TIME - 1);
    BOOST_CHECK_EXCEPTION(Template(), std::runtime_error, HasReason("time-too-new"));
    SetMockTime(floor - MAX_FUTURE_BLOCK_TIME);
    BOOST_CHECK_EQUAL(Template().GetBlockTime(), floor);
}

namespace {

/** How a test miner stamps its blocks: the earliest valid time, the latest, alternating, or anywhere between. */
enum class Stamp { EARLIEST, LATEST, ALTERNATE, ALTERNATE_RUNS, RANDOM };

/** How the clock moves between blocks: in bursts of fast blocks, at about one block a minute, or a mix. */
enum class Clock { BURST, MINUTE, MIXED };

struct LivenessRun {
    Stamp stamp;
    Clock clock;
    uint64_t seed;
};

/** Mine `blocks` blocks on the tip with the given stamping and clock, checking for every block that a valid time
 *  exists from the clock value the bound gives, that the node makes a block from that moment and not one second
 *  before, and that the block it makes is valid. `accepted` holds the clock value at which each block of the chain
 *  was accepted and is extended. Returns the longest forced wait. */
int64_t CheckLiveness(TimewarpChainSetup& setup, const LivenessRun& run, int blocks, std::vector<int64_t>& accepted)
{
    FastRandomContext rng(ArithToUint256(arith_uint256(run.seed)));
    int64_t clock = accepted.back();
    int64_t longest_wait = 0;
    SetMockTime(clock);
    for (int i = 0; i < blocks; ++i) {
        const CBlockIndex* tip = setup.Tip();
        const int height = tip->nHeight + 1;
        BOOST_REQUIRE_EQUAL((size_t)height, accepted.size());
        const int64_t floor = setup.MinimumTime(tip);
        const int64_t parent_clock = accepted[height - 1];
        // The first clock value from which a valid time exists, and the bound on it.
        const int64_t first = std::max(parent_clock, floor - MAX_FUTURE_BLOCK_TIME);
        int64_t bound = parent_clock + 1;
        if (height > TIMEWARP_SPAN_DEPTH) bound = std::max(bound, accepted[height - TIMEWARP_SPAN_DEPTH] + TIMEWARP_MIN_SPAN);
        BOOST_REQUIRE_MESSAGE(first <= bound, strprintf("height %d: first valid clock %d, bound %d", height, first, bound));
        BOOST_REQUIRE_LE(first - parent_clock, TIMEWARP_MIN_SPAN);
        longest_wait = std::max(longest_wait, first - parent_clock);
        // The node agrees: no template one second before, a valid one at that moment.
        if (first > clock) {
            SetMockTime(first - 1);
            BOOST_CHECK_EXCEPTION(setup.Template(), std::runtime_error, HasReason("time-too-new"));
            clock = first;
        }
        SetMockTime(clock);
        const CBlock earliest = setup.Template();
        BOOST_REQUIRE_EQUAL(earliest.GetBlockTime(), std::max(floor, clock));

        // The block is found a little later and stamped by the run's policy, anywhere from the earliest valid time
        // to the future limit.
        switch (run.clock) {
        case Clock::BURST: clock += rng.randrange(3); break;
        case Clock::MINUTE: clock += 30 + rng.randrange(61); break;
        case Clock::MIXED: clock += (rng.randrange(4) == 0) ? 60 + rng.randrange(600) : rng.randrange(5); break;
        }
        SetMockTime(clock);
        const int64_t earliest_time = std::max(tip->GetMedianTimePast() + 1, floor);
        const int64_t latest_time = clock + MAX_FUTURE_BLOCK_TIME;
        BOOST_REQUIRE_LE(earliest_time, latest_time);
        int64_t time{0};
        switch (run.stamp) {
        case Stamp::EARLIEST: time = earliest_time; break;
        case Stamp::LATEST: time = latest_time; break;
        case Stamp::ALTERNATE: time = (height % 2) ? latest_time : earliest_time; break;
        case Stamp::ALTERNATE_RUNS: time = ((height / 23) % 2) ? latest_time : earliest_time; break;
        case Stamp::RANDOM: time = earliest_time + rng.randrange(latest_time - earliest_time + 1); break;
        }
        setup.MineAt(time);
        accepted.push_back(clock);
    }
    return longest_wait;
}

} // namespace

BOOST_FIXTURE_TEST_CASE(liveness_bound, ActiveSetup)
{
    // Deterministic case. After 40 blocks a minute apart, block 41 is stamped two hours ahead of the clock c0. The
    // next 22 blocks are stamped at their earliest times while the clock moves one second per block. Block 64 then
    // needs t(41) + 480 = c0 + 7680, which is valid from the clock value c0 + 480 on.
    int64_t clock = BASE_TIME;
    SetMockTime(clock);
    for (int height = 1; height <= 40; ++height) {
        clock += 60;
        SetMockTime(clock);
        MineAt(clock);
    }
    const int64_t c0 = clock + 60;
    SetMockTime(c0);
    MineAt(c0 + MAX_FUTURE_BLOCK_TIME);
    for (int i = 1; i <= 22; ++i) {
        SetMockTime(c0 + i);
        MineAt(std::max(Tip()->GetMedianTimePast() + 1, MinimumTime(Tip())));
    }
    BOOST_REQUIRE_EQUAL(Tip()->nHeight, 63);
    BOOST_CHECK_EQUAL(MinimumTime(Tip()), c0 + MAX_FUTURE_BLOCK_TIME + TIMEWARP_MIN_SPAN);
    SetMockTime(c0 + TIMEWARP_MIN_SPAN - 1);
    BOOST_CHECK_EXCEPTION(Template(), std::runtime_error, HasReason("time-too-new"));
    SetMockTime(c0 + TIMEWARP_MIN_SPAN);
    CBlock block = Template();
    BOOST_CHECK_EQUAL(block.GetBlockTime(), c0 + 7680);
    Solve(block);
    BOOST_CHECK(OfferHeader(block.GetBlockHeader()).IsValid());
    BOOST_CHECK(Process(block));
}

BOOST_FIXTURE_TEST_CASE(liveness_random_chains, ActiveSetup)
{
    // Seeded chains whose stamps lie anywhere between the earliest valid time and the future limit, with the clock
    // moving in bursts of fast blocks or at a block a minute. Each run continues the chain of the run before.
    const std::vector<LivenessRun> runs{
        {Stamp::LATEST, Clock::BURST, 1},
        {Stamp::EARLIEST, Clock::BURST, 2},
        {Stamp::ALTERNATE, Clock::BURST, 3},
        {Stamp::ALTERNATE_RUNS, Clock::BURST, 4},
        {Stamp::RANDOM, Clock::MIXED, 5},
        {Stamp::LATEST, Clock::MINUTE, 6},
        {Stamp::ALTERNATE_RUNS, Clock::MIXED, 7},
        {Stamp::EARLIEST, Clock::MINUTE, 8},
    };
    std::vector<int64_t> accepted{BASE_TIME}; // block 0 counts as accepted at the start
    int64_t longest_wait = 0;
    for (const LivenessRun& run : runs) {
        longest_wait = std::max(longest_wait, CheckLiveness(*this, run, 50, accepted));
    }
    // Some of these chains make the miner wait.
    BOOST_CHECK_GT(longest_wait, 0);
    BOOST_CHECK_LE(longest_wait, TIMEWARP_MIN_SPAN);
}

BOOST_FIXTURE_TEST_CASE(liveness_across_activation, ActivationSetup)
{
    // The same checks on a chain on which the rules apply from block 432, with blocks stamped two hours ahead and at their earliest times in turns of 23 blocks around that height.
    std::vector<int64_t> accepted{BASE_TIME};
    const int64_t longest_wait = CheckLiveness(*this, {Stamp::ALTERNATE_RUNS, Clock::BURST, 9}, ACTIVATION_HEIGHT + 60, accepted);
    BOOST_CHECK_LE(longest_wait, TIMEWARP_MIN_SPAN);
    LOCK(cs_main);
    BOOST_CHECK(!IsTimewarpEnabled(::ChainActive()[ACTIVATION_HEIGHT - 2], Consensus()));
    BOOST_CHECK(IsTimewarpEnabled(::ChainActive()[ACTIVATION_HEIGHT - 1], Consensus()));
}

BOOST_AUTO_TEST_SUITE_END()
