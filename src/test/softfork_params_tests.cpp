// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/system.h>
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

BOOST_AUTO_TEST_SUITE_END()
