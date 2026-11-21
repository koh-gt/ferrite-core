// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <policy/policy.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/system.h>
#include <validation.h>
#include <versionbits.h>
#include <versionbitsinfo.h>

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

void CheckDeployment(const Consensus::BIP9Deployment& got, const Consensus::BIP9Deployment& expected, const std::string& context)
{
    BOOST_CHECK_MESSAGE(got.bit == expected.bit, strprintf("%s: bit %d, expected %d", context, got.bit, expected.bit));
    BOOST_CHECK_MESSAGE(got.nStartTime == expected.nStartTime, strprintf("%s: nStartTime %d, expected %d", context, got.nStartTime, expected.nStartTime));
    BOOST_CHECK_MESSAGE(got.nTimeout == expected.nTimeout, strprintf("%s: nTimeout %d, expected %d", context, got.nTimeout, expected.nTimeout));
    BOOST_CHECK_MESSAGE(got.nStartHeight == expected.nStartHeight, strprintf("%s: nStartHeight %d, expected %d", context, got.nStartHeight, expected.nStartHeight));
    BOOST_CHECK_MESSAGE(got.nTimeoutHeight == expected.nTimeoutHeight, strprintf("%s: nTimeoutHeight %d, expected %d", context, got.nTimeoutHeight, expected.nTimeoutHeight));
}

/** The deployments of p2mr, coinbasematurity and timewarp, with the bit each one signals on every network. */
struct NewDeployment {
    Consensus::DeploymentPos pos;
    std::string name;
    int bit;
};
const std::vector<NewDeployment> NEW_DEPLOYMENTS{
    {Consensus::DEPLOYMENT_P2MR, "p2mr", 6},
    {Consensus::DEPLOYMENT_COINBASE_MATURITY, "coinbasematurity", 7},
    {Consensus::DEPLOYMENT_TIMEWARP, "timewarp", 8},
};

std::string DescribeDeployment(const Consensus::BIP9Deployment& d)
{
    return strprintf("bit %d, nStartTime %d, nTimeout %d, nStartHeight %d, nTimeoutHeight %d", d.bit, d.nStartTime, d.nTimeout, d.nStartHeight, d.nTimeoutHeight);
}

/** Clears the global version bits cache when it goes out of scope, however the scope is left. The cache holds
 *  states by block index, so it must not outlive a chain that a test built for itself. cs_main must be held. */
struct ScopedVersionBitsCacheClear {
    ~ScopedVersionBitsCacheClear() { versionbitscache.Clear(); }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(softfork_params_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(vbparams_forms)
{
    const auto defaults = RegtestParams("");

    struct Form {
        std::string fields;
        int64_t start_time;
        int64_t timeout;
        int64_t start_height;
        int64_t timeout_height;
    };
    const std::vector<Form> forms{
        // Height-based (start and end 0): a height that is not given is 0.
        {"0:0", 0, 0, 0, 0},
        {"0:0:288", 0, 0, 288, 0},
        {"0:0:288:99999999", 0, 0, 288, 99999999},
        // Time-based, always active and never active: the heights are not used and are 0.
        {"1199145601:1230767999", 1199145601, 1230767999, 0, 0},
        {"-1:0", Consensus::BIP9Deployment::ALWAYS_ACTIVE, 0, 0, 0},
        {"-2:0", Consensus::BIP9Deployment::NEVER_ACTIVE, 0, 0, 0},
    };

    // Every deployment is found by its name, and only that deployment changes.
    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++i) {
        const std::string name = VersionBitsDeploymentInfo[i].name;
        for (const Form& form : forms) {
            const std::string arg = name + ":" + form.fields;
            const auto params = RegtestParams(arg);
            for (int j = 0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
                Consensus::BIP9Deployment expected = defaults->GetConsensus().vDeployments[j];
                if (j == i) {
                    expected.nStartTime = form.start_time;
                    expected.nTimeout = form.timeout;
                    expected.nStartHeight = form.start_height;
                    expected.nTimeoutHeight = form.timeout_height;
                }
                CheckDeployment(params->GetConsensus().vDeployments[j], expected, strprintf("-vbparams=%s, deployment %s", arg, VersionBitsDeploymentInfo[j].name));
            }
        }
    }
    BOOST_CHECK_EXCEPTION(RegtestParams("nosuchdeployment:0:0"), std::runtime_error, HasReason("Invalid deployment (nosuchdeployment)"));

    // With start and end 0 (start height 0, timeout height 0), the window [0, 143] is DEFINED, as the genesis
    // block's window always is; [144, 287] is STARTED and locks in at its end whatever the blocks signal, because
    // it ends past the timeout height; [288, 431] is LOCKED_IN and the deployment is ACTIVE from block 432.
    const int period = defaults->GetConsensus().nMinerConfirmationWindow;
    BOOST_REQUIRE_EQUAL(period, 144);
    std::vector<CBlockIndex> chain(3 * period); // heights 0 to 431, none signalling
    for (size_t height = 0; height < chain.size(); ++height) {
        chain[height].nHeight = height;
        chain[height].pprev = height > 0 ? &chain[height - 1] : nullptr;
        chain[height].nVersion = VERSIONBITS_TOP_BITS;
        chain[height].BuildSkip();
    }
    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++i) {
        const auto pos = static_cast<Consensus::DeploymentPos>(i);
        const std::string arg = std::string(VersionBitsDeploymentInfo[i].name) + ":0:0";
        const auto params = RegtestParams(arg);
        const Consensus::Params& consensus = params->GetConsensus();
        VersionBitsCache cache;
        // The state of the block at this height, i.e. the state read with its parent.
        const auto state_of_block = [&](int height) { return VersionBitsState(&chain[height - 1], consensus, pos, cache); };
        BOOST_CHECK_MESSAGE(state_of_block(143) == ThresholdState::DEFINED, arg + ": block 143 not DEFINED");
        BOOST_CHECK_MESSAGE(state_of_block(144) == ThresholdState::STARTED, arg + ": block 144 not STARTED");
        BOOST_CHECK_MESSAGE(state_of_block(287) == ThresholdState::STARTED, arg + ": block 287 not STARTED");
        BOOST_CHECK_MESSAGE(state_of_block(288) == ThresholdState::LOCKED_IN, arg + ": block 288 not LOCKED_IN");
        BOOST_CHECK_MESSAGE(state_of_block(431) == ThresholdState::LOCKED_IN, arg + ": block 431 not LOCKED_IN");
        BOOST_CHECK_MESSAGE(state_of_block(432) == ThresholdState::ACTIVE, arg + ": block 432 not ACTIVE");
        BOOST_CHECK_EQUAL(VersionBitsStateSinceHeight(&chain[431], consensus, pos, cache), 432);
    }
}

BOOST_AUTO_TEST_CASE(mainnet)
{
    ArgsManager args;
    const auto params = CreateChainParams(args, CBaseChainParams::MAIN);
    const Consensus::Params& consensus = params->GetConsensus();
    BOOST_REQUIRE_EQUAL(consensus.nMinerConfirmationWindow, 40U);
    BOOST_REQUIRE_EQUAL(consensus.nRuleChangeActivationThreshold, 39U);

    // Height-based (start and timeout times 0), signalling from the start height, with no deadline.
    const std::vector<int64_t> start_heights{1300000, 1250000, 1250000};
    BOOST_REQUIRE_EQUAL(start_heights.size(), NEW_DEPLOYMENTS.size());
    for (size_t i = 0; i < NEW_DEPLOYMENTS.size(); ++i) {
        const NewDeployment& deployment = NEW_DEPLOYMENTS[i];
        Consensus::BIP9Deployment expected;
        expected.bit = deployment.bit;
        expected.nStartHeight = start_heights[i];
        expected.nTimeoutHeight = 99999999;
        CheckDeployment(consensus.vDeployments[deployment.pos], expected, "mainnet " + deployment.name);

        // The name used by getblockchaininfo, getblocktemplate and -vbparams; getblocktemplate sets the bit for
        // callers that do not list the rule, and does not refuse them once it is active.
        BOOST_CHECK_EQUAL(VersionBitsDeploymentInfo[deployment.pos].name, deployment.name);
        BOOST_CHECK(VersionBitsDeploymentInfo[deployment.pos].gbt_force);
    }
}

BOOST_AUTO_TEST_CASE(testnet_starts)
{
    // Regtest: never active unless -vbparams sets them.
    const auto regtest = RegtestParams("");
    for (const NewDeployment& deployment : NEW_DEPLOYMENTS) {
        Consensus::BIP9Deployment expected;
        expected.bit = deployment.bit;
        expected.nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        expected.nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        CheckDeployment(regtest->GetConsensus().vDeployments[deployment.pos], expected, "regtest " + deployment.name);
    }

    // Testnet: either none of the three is scheduled, or all three are height-based with no deadline,
    // coinbasematurity and timewarp from the same start height and p2mr from 1,000 blocks later.
    ArgsManager args;
    const auto testnet = CreateChainParams(args, CBaseChainParams::TESTNET);
    const Consensus::Params& consensus = testnet->GetConsensus();
    for (const NewDeployment& deployment : NEW_DEPLOYMENTS) {
        BOOST_CHECK_MESSAGE(consensus.vDeployments[deployment.pos].bit == deployment.bit, "testnet " + deployment.name + ": wrong bit");
    }
    const Consensus::BIP9Deployment& p2mr = consensus.vDeployments[Consensus::DEPLOYMENT_P2MR];
    const Consensus::BIP9Deployment& maturity = consensus.vDeployments[Consensus::DEPLOYMENT_COINBASE_MATURITY];
    const Consensus::BIP9Deployment& timewarp = consensus.vDeployments[Consensus::DEPLOYMENT_TIMEWARP];
    const auto never_active = [](const Consensus::BIP9Deployment& d) {
        return d.nStartTime == Consensus::BIP9Deployment::NEVER_ACTIVE;
    };
    const auto starts_at = [](const Consensus::BIP9Deployment& d, int64_t start_height) {
        return d.nStartTime == 0 && d.nTimeout == 0 && d.nStartHeight == start_height && d.nTimeoutHeight == 99999999;
    };
    const bool not_scheduled = never_active(p2mr) && never_active(maturity) && never_active(timewarp);
    const int64_t start_height = maturity.nStartHeight;
    const bool scheduled = start_height > 0 && start_height % consensus.nMinerConfirmationWindow == 0 &&
                           starts_at(maturity, start_height) && starts_at(timewarp, start_height) &&
                           starts_at(p2mr, start_height + 1000);
    BOOST_CHECK_MESSAGE(not_scheduled || scheduled,
                        strprintf("testnet: p2mr %s; coinbasematurity %s; timewarp %s",
                                  DescribeDeployment(p2mr), DescribeDeployment(maturity), DescribeDeployment(timewarp)));
}

BOOST_AUTO_TEST_CASE(predicates)
{
    // IsP2MREnabled and IsTimewarpEnabled give the state of their own deployment for the block after the one
    // they are given: on a regtest chain whose blocks do not signal, with -vbparams=<name>:0:0, ACTIVE from
    // block 432 (see vbparams_forms).
    std::vector<CBlockIndex> chain(432); // heights 0 to 431
    for (size_t height = 0; height < chain.size(); ++height) {
        chain[height].nHeight = height;
        chain[height].pprev = height > 0 ? &chain[height - 1] : nullptr;
        chain[height].nVersion = VERSIONBITS_TOP_BITS;
        chain[height].BuildSkip();
    }
    struct Case {
        std::string vbparams;
        bool p2mr;
        bool timewarp;
    };
    const std::vector<Case> cases{
        {"", false, false},
        {"p2mr:0:0", true, false},
        {"timewarp:0:0", false, true},
        {"coinbasematurity:0:0", false, false},
    };
    LOCK(cs_main);
    // Declared after the chain and the lock, so the cache is cleared before either goes away, also if a check throws.
    ScopedVersionBitsCacheClear clear_cache_at_end;
    for (const Case& c : cases) {
        const auto params = RegtestParams(c.vbparams);
        const Consensus::Params& consensus = params->GetConsensus();
        // The global cache holds states by block index; this chain is local and the parameters change.
        versionbitscache.Clear();
        const std::string context = "-vbparams=" + c.vbparams;
        BOOST_CHECK_MESSAGE(!IsP2MREnabled(nullptr, consensus), context + ": p2mr active for the genesis block");
        BOOST_CHECK_MESSAGE(!IsP2MREnabled(&chain[430], consensus), context + ": p2mr active for block 431");
        BOOST_CHECK_MESSAGE(IsP2MREnabled(&chain[431], consensus) == c.p2mr, context + ": p2mr state for block 432");
        BOOST_CHECK_MESSAGE(!IsTimewarpEnabled(nullptr, consensus), context + ": timewarp active for the genesis block");
        BOOST_CHECK_MESSAGE(!IsTimewarpEnabled(&chain[430], consensus), context + ": timewarp active for block 431");
        BOOST_CHECK_MESSAGE(IsTimewarpEnabled(&chain[431], consensus) == c.timewarp, context + ": timewarp state for block 432");
    }
}

BOOST_AUTO_TEST_CASE(constants)
{
    // P2MR (BIP 360). The script flag has a bit of its own. It is one of the flags transactions are relayed with,
    // whether or not blocks enforce the rules yet, so the mempool never holds a spend that breaks them. It is not
    // one of the mandatory flags.
    BOOST_CHECK_EQUAL(static_cast<unsigned int>(SCRIPT_VERIFY_P2MR), 1U << 21);
    BOOST_CHECK(STANDARD_SCRIPT_VERIFY_FLAGS & SCRIPT_VERIFY_P2MR);
    BOOST_CHECK(STANDARD_NOT_MANDATORY_VERIFY_FLAGS & SCRIPT_VERIFY_P2MR);
    BOOST_CHECK(!(MANDATORY_SCRIPT_VERIFY_FLAGS & SCRIPT_VERIFY_P2MR));
    // A program of 32 bytes, and a control block of one byte and up to 128 nodes of 32 bytes.
    BOOST_CHECK_EQUAL(WITNESS_V2_P2MR_SIZE, 32U);
    BOOST_CHECK_EQUAL(P2MR_CONTROL_BASE_SIZE, 1U);
    BOOST_CHECK_EQUAL(P2MR_CONTROL_MAX_SIZE, 4097U);
}

BOOST_AUTO_TEST_SUITE_END()
