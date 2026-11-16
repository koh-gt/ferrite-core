// Copyright (c) 2021 The Litecoin Core developers
// Copyright (c) 2023 The Ferrite Core Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mw/crypto/Schnorr.h>
#include <mw/models/tx/Kernel.h>
#include <mw/models/tx/Transaction.h>

#include <mweb/mweb_policy.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <policy/settings.h>
#include <primitives/transaction.h>
#include <script/standard.h>

#include <test_framework/TestMWEB.h>

#include <limits>

BOOST_FIXTURE_TEST_SUITE(TestKernel, MWEBTestingSetup)

// Checks a kernel that sets one feature bit with an empty field and carries no other field. Blocks
// and transactions write the empty field right after the features byte and read the kernel back
// unchanged, while the kernel ID is still computed over the bytes without it.
static void CheckEmptyFlaggedFieldReadsBack(const Kernel& kernel)
{
    CDataStream stream(SER_DISK, PROTOCOL_VERSION);
    stream << kernel;

    std::vector<uint8_t> expected = kernel.Serialized();
    expected.insert(expected.begin() + 1, 0x00);
    BOOST_CHECK(std::vector<uint8_t>(stream.begin(), stream.end()) == expected);

    Kernel reloaded;
    BOOST_REQUIRE_NO_THROW(stream >> reloaded);
    BOOST_CHECK(stream.empty());
    BOOST_CHECK_EQUAL(reloaded.GetFeatures(), kernel.GetFeatures());
    BOOST_CHECK(reloaded.GetKernelID() == kernel.GetKernelID());

    CDataStream rewritten(SER_DISK, PROTOCOL_VERSION);
    rewritten << reloaded;
    BOOST_CHECK(std::vector<uint8_t>(rewritten.begin(), rewritten.end()) == expected);
}

BOOST_AUTO_TEST_CASE(FeatureActivationDeployment_Test)
{
    // Ferrite mainnet signals both rules on bit 5 from block 1,250,000 (v3.3.0, the mwebkernel deployment);
    // Litecoin activates them at heights 3'154'440 and 3'172'640.
    const auto main_params = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& main_consensus = main_params->GetConsensus();
    const Consensus::BIP9Deployment& main_deployment = main_consensus.vDeployments[Consensus::DEPLOYMENT_MWEB_KERNEL];
    BOOST_REQUIRE_EQUAL(main_deployment.bit, 5);
    BOOST_REQUIRE_EQUAL(main_deployment.nStartTime, 0);
    BOOST_REQUIRE_EQUAL(main_deployment.nTimeout, 0);
    BOOST_REQUIRE_EQUAL(main_deployment.nStartHeight, 1'250'000);
    BOOST_REQUIRE_EQUAL(main_deployment.nTimeoutHeight, 99'999'999);
    BOOST_REQUIRE_EQUAL(main_deployment.nStartHeight % main_consensus.nMinerConfirmationWindow, 0);

    for (const std::string& chain : {CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(*m_node.args, chain);
        const Consensus::BIP9Deployment& deployment = params->GetConsensus().vDeployments[Consensus::DEPLOYMENT_MWEB_KERNEL];
        BOOST_REQUIRE_EQUAL(deployment.bit, 5);
        BOOST_REQUIRE(deployment.nStartTime == Consensus::BIP9Deployment::ALWAYS_ACTIVE);
    }
}

BOOST_AUTO_TEST_CASE(PlainKernel_Test)
{
    CAmount fee = 1000;
    BlindingFactor excess_blind = BlindingFactor::Random();
    Kernel kernel = Kernel::Create(excess_blind, boost::none, fee, boost::none, std::vector<PegOutCoin>{}, boost::none);

    //
    // Serialization
    //
    {
        std::vector<uint8_t> serialized = kernel.Serialized();

        Kernel kernel2;
        CDataStream(serialized, SER_DISK, 0) >> kernel2;
        BOOST_REQUIRE(kernel == kernel2);
    }

    //
    // Signature Message
    //
    {
        Hasher hasher;
        hasher << uint8_t(1);
        hasher << Commitment::Blinded(excess_blind, 0);
        ::WriteVarInt<Hasher, VarIntMode::NONNEGATIVE_SIGNED, CAmount>(hasher, fee);
        BOOST_REQUIRE(kernel.BuildSignedMsg().GetMsgHash() == hasher.hash());
    }

    //
    // Getters
    //
    {
        BOOST_REQUIRE(!kernel.HasPegIn());
        BOOST_REQUIRE(!kernel.HasPegOut());
        BOOST_REQUIRE(!kernel.HasStealthExcess());
        BOOST_REQUIRE(kernel.GetPegIn() == 0);
        BOOST_REQUIRE(kernel.GetPegOuts().empty());
        BOOST_REQUIRE(kernel.GetLockHeight() == 0);
        BOOST_REQUIRE(kernel.GetFee() == fee);
        BOOST_REQUIRE(kernel.GetCommitment() == Commitment::Blinded(excess_blind, 0));
        BOOST_REQUIRE(kernel.GetSignature() == Schnorr::Sign(excess_blind.data(), kernel.BuildSignedMsg().GetMsgHash()));
    }
}

BOOST_AUTO_TEST_CASE(NonStandardKernel_Test)
{
    CAmount fee = 1000;
    CAmount pegin = 10000;
    std::vector<uint8_t> rand1 = RandomBytes<30>();
    PegOutCoin pegout(2000, CScript(rand1.data(), rand1.data() + rand1.size()));
    int32_t lock_height = 123456;
    std::vector<uint8_t> rand2 = RandomBytes<30>();
    Kernel standard_kernel(
        Kernel::FEE_FEATURE_BIT | Kernel::PEGIN_FEATURE_BIT | Kernel::PEGOUT_FEATURE_BIT | Kernel::HEIGHT_LOCK_FEATURE_BIT | Kernel::STEALTH_EXCESS_FEATURE_BIT,
        fee,
        pegin,
        std::vector<PegOutCoin>{PegOutCoin(2000, CScript(rand2.data(), rand2.data() + rand2.size()))},
        lock_height,
        PublicKey::Random(),
        std::vector<uint8_t>{},
        Commitment::Random(),
        Signature(RandomBigInt<Signature::SIZE>())
    );
    std::vector<uint8_t> rand3 = RandomBytes<30>();
    Kernel nonstandard_kernel1(
        Kernel::ALL_FEATURE_BITS,
        fee,
        pegin,
        std::vector<PegOutCoin>{PegOutCoin(2000, CScript(rand3.data(), rand3.data() + rand3.size()))},
        lock_height,
        PublicKey::Random(),
        RandomBytes<20>(),
        standard_kernel.GetCommitment(),
        standard_kernel.GetSignature()
    );

    Kernel nonstandard_kernel2(
        0x20,
        boost::none,
        boost::none,
        std::vector<PegOutCoin>{},
        boost::none,
        PublicKey::Random(),
        std::vector<uint8_t>{},
        standard_kernel.GetCommitment(),
        standard_kernel.GetSignature()
    );

    CDataStream serialized(SER_DISK, 0);
    serialized << uint8_t(Kernel::PEGOUT_FEATURE_BIT);
    serialized << std::vector<PegOutCoin>{};
    serialized << standard_kernel.GetCommitment();
    serialized << standard_kernel.GetSignature();
    Kernel nonstandard_kernel3;
    serialized >> nonstandard_kernel3;

    BOOST_REQUIRE(standard_kernel.IsStandard());
    BOOST_REQUIRE(!nonstandard_kernel1.IsStandard());
    BOOST_REQUIRE(!nonstandard_kernel2.IsStandard());
    BOOST_REQUIRE(!nonstandard_kernel3.IsStandard());
    BOOST_REQUIRE(nonstandard_kernel1.HasCanonicalExtraDataFeature());
    BOOST_REQUIRE(!nonstandard_kernel2.HasCanonicalExtraDataFeature());
    BOOST_REQUIRE(!nonstandard_kernel3.HasCanonicalPegOutFeature());

    mw::Transaction::CPtr nonstandard_tx = mw::Transaction::Create(
        BlindingFactor::Random(), BlindingFactor::Random(), {}, {}, { nonstandard_kernel3 });
    CMutableTransaction transaction;
    transaction.mweb_tx = MWEB::Tx{std::move(nonstandard_tx)};
    std::string reason;
    BOOST_REQUIRE(!MWEB::Policy::IsStandardTx(CTransaction{std::move(transaction)}, reason));
    BOOST_REQUIRE_EQUAL(reason, "non-standard-mweb-tx");
}

BOOST_AUTO_TEST_CASE(EmptyPegOutFeature_Test)
{
    const BlindingFactor blind = BlindingFactor::Random();
    const Commitment excess = Commitment::Blinded(blind, 0);
    const uint8_t features = Kernel::PEGOUT_FEATURE_BIT;
    const auto message = Kernel::GetSignatureMessage(
        features,
        excess,
        boost::none,
        boost::none,
        boost::none,
        {},
        boost::none,
        {}
    );
    const Kernel kernel(
        features,
        boost::none,
        boost::none,
        {},
        boost::none,
        boost::none,
        {},
        excess,
        Schnorr::Sign(blind.data(), message)
    );

    BOOST_REQUIRE(!kernel.HasCanonicalPegOutFeature());
    BOOST_REQUIRE_NO_THROW(TxBody({}, {}, {kernel}).Validate());
    CheckEmptyFlaggedFieldReadsBack(kernel);
}

BOOST_AUTO_TEST_CASE(EmptyExtraDataFeature_Test)
{
    const BlindingFactor blind = BlindingFactor::Random();
    const Commitment excess = Commitment::Blinded(blind, 0);
    const uint8_t features = Kernel::EXTRA_DATA_FEATURE_BIT;
    const auto message = Kernel::GetSignatureMessage(
        features,
        excess,
        boost::none,
        boost::none,
        boost::none,
        {},
        boost::none,
        {}
    );
    const Kernel kernel(
        features,
        boost::none,
        boost::none,
        {},
        boost::none,
        boost::none,
        {},
        excess,
        Schnorr::Sign(blind.data(), message)
    );

    BOOST_REQUIRE(!kernel.HasCanonicalExtraDataFeature());
    BOOST_REQUIRE_NO_THROW(TxBody({}, {}, {kernel}).Validate());
    CheckEmptyFlaggedFieldReadsBack(kernel);
}

// Writes the fields of a kernel in the order the deserializer reads them: every field whose feature
// bit is set, except those whose bits are in `omitted`.
static std::vector<uint8_t> KernelBytes(const Kernel& kernel, const uint8_t omitted)
{
    const int written = kernel.GetFeatures() & ~omitted;
    std::vector<uint8_t> bytes;
    CVectorWriter s(SER_NETWORK, PROTOCOL_VERSION, bytes, 0);
    s << kernel.GetFeatures();
    if (written & Kernel::FEE_FEATURE_BIT) ::WriteVarInt<CVectorWriter, VarIntMode::NONNEGATIVE_SIGNED, CAmount>(s, kernel.GetFee());
    if (written & Kernel::PEGIN_FEATURE_BIT) ::WriteVarInt<CVectorWriter, VarIntMode::NONNEGATIVE_SIGNED, CAmount>(s, kernel.GetPegIn());
    if (written & Kernel::PEGOUT_FEATURE_BIT) s << kernel.GetPegOuts();
    if (written & Kernel::HEIGHT_LOCK_FEATURE_BIT) ::WriteVarInt<CVectorWriter, VarIntMode::NONNEGATIVE_SIGNED, int32_t>(s, kernel.GetLockHeight());
    if (written & Kernel::STEALTH_EXCESS_FEATURE_BIT) s << kernel.GetStealthExcess();
    if (written & Kernel::EXTRA_DATA_FEATURE_BIT) s << kernel.GetExtraData();
    s << kernel.GetExcess() << kernel.GetSignature();
    return bytes;
}

BOOST_AUTO_TEST_CASE(FeatureCombinationsReadBack_Test)
{
    // A kernel read from a block or a transaction carries the fields its feature bits name, but its
    // peg-out list or extra data may be empty although the bit is set. For every such kernel, the
    // kernel ID is computed over the bytes that leave out an empty field, while kernels written on
    // their own or inside a transaction carry every field whose bit is set and read back unchanged.
    const std::vector<uint8_t> script_bytes(22, 0x51);
    const std::vector<PegOutCoin> pegouts{PegOutCoin(5000, CScript(script_bytes.begin(), script_bytes.end()))};
    const std::vector<uint8_t> extra_data{0x01, 0x02, 0x03};
    const PublicKey stealth_excess(BigInt<33>::ValueOf(2));
    const Commitment excess(BigInt<Commitment::SIZE>::ValueOf(8));
    const Signature signature(BigInt<Signature::SIZE>::ValueOf(9));
    const uint8_t empty_fields[] = {
        0,
        Kernel::PEGOUT_FEATURE_BIT,
        Kernel::EXTRA_DATA_FEATURE_BIT,
        Kernel::PEGOUT_FEATURE_BIT | Kernel::EXTRA_DATA_FEATURE_BIT,
    };

    for (int features = 0; features <= Kernel::ALL_FEATURE_BITS; ++features) {
        for (const uint8_t empty : empty_fields) {
            if ((features & empty) != empty) continue;
            BOOST_TEST_CONTEXT("features " << features << ", empty fields " << int{empty})
            {
                const int present = features & ~empty;
                const Kernel kernel(
                    features,
                    (present & Kernel::FEE_FEATURE_BIT) ? boost::make_optional<CAmount>(1000) : boost::none,
                    (present & Kernel::PEGIN_FEATURE_BIT) ? boost::make_optional<CAmount>(20000) : boost::none,
                    (present & Kernel::PEGOUT_FEATURE_BIT) ? pegouts : std::vector<PegOutCoin>{},
                    (present & Kernel::HEIGHT_LOCK_FEATURE_BIT) ? boost::make_optional<int32_t>(300) : boost::none,
                    (present & Kernel::STEALTH_EXCESS_FEATURE_BIT) ? boost::make_optional(stealth_excess) : boost::none,
                    (present & Kernel::EXTRA_DATA_FEATURE_BIT) ? extra_data : std::vector<uint8_t>{},
                    excess,
                    signature
                );

                const std::vector<uint8_t> committed = KernelBytes(kernel, empty);
                BOOST_CHECK(kernel.Serialized() == committed);
                BOOST_CHECK(kernel.GetKernelID() == Hashed(committed));

                CDataStream stream(SER_DISK, PROTOCOL_VERSION);
                stream << kernel;
                BOOST_CHECK(std::vector<uint8_t>(stream.begin(), stream.end()) == KernelBytes(kernel, 0));
                Kernel reloaded;
                BOOST_REQUIRE_NO_THROW(stream >> reloaded);
                BOOST_CHECK(stream.empty());
                BOOST_CHECK(reloaded.Serialized() == committed);
                BOOST_CHECK(reloaded.GetKernelID() == kernel.GetKernelID());

                const mw::Transaction tx(BlindingFactor(), BlindingFactor(), TxBody({}, {}, {kernel}));
                CDataStream tx_stream(SER_DISK, PROTOCOL_VERSION);
                tx_stream << tx;
                mw::Transaction tx_reloaded;
                BOOST_REQUIRE_NO_THROW(tx_stream >> tx_reloaded);
                BOOST_CHECK(tx_stream.empty());
                BOOST_CHECK(tx_reloaded.GetHash() == tx.GetHash());
                BOOST_REQUIRE_EQUAL(tx_reloaded.GetKernels().size(), 1U);
                BOOST_CHECK(tx_reloaded.GetKernels().front().GetKernelID() == kernel.GetKernelID());
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(PegOutAmountOutOfRange_Test)
{
    std::vector<uint8_t> script_bytes = RandomBytes<30>();
    CScript script(script_bytes.data(), script_bytes.data() + script_bytes.size());

    Kernel kernel = Kernel::Create(
        BlindingFactor::Random(),
        boost::none,
        boost::none,
        boost::none,
        std::vector<PegOutCoin>{ PegOutCoin(MAX_MONEY, script), PegOutCoin(1, script) },
        boost::none
    );

    BOOST_REQUIRE(!kernel.GetPegOutAmount().has_value());
}

BOOST_AUTO_TEST_CASE(SupplyChangeOutOfRange_Test)
{
    std::vector<uint8_t> script_bytes = RandomBytes<30>();
    CScript script(script_bytes.data(), script_bytes.data() + script_bytes.size());

    Kernel kernel = Kernel::Create(
        BlindingFactor::Random(),
        boost::none,
        std::numeric_limits<CAmount>::max(),
        boost::none,
        std::vector<PegOutCoin>{ PegOutCoin(1, script) },
        boost::none
    );

    BOOST_REQUIRE(!kernel.GetSupplyChange().has_value());
}

BOOST_AUTO_TEST_CASE(PegOutDataCarrier_Test)
{
    // IsStandardTx() checks each peg-out script as it checks an output script,
    // so -datacarriersize applies to OP_RETURN peg-outs too. The standard weight
    // of a transaction with only MWEB data counts its peg-outs and nothing else.
    const auto pegout_tx = [](const CScript& script) {
        const Kernel kernel = Kernel::Create(BlindingFactor::Random(), boost::none, 1000, boost::none, {PegOutCoin(1000, script)}, boost::none);
        CMutableTransaction transaction;
        transaction.mweb_tx = MWEB::Tx{mw::Transaction::Create(BlindingFactor::Random(), BlindingFactor::Random(), {}, {}, {kernel})};
        return CTransaction{std::move(transaction)};
    };
    std::string reason;

    // An 84-byte OP_RETURN peg-out script is standard by default, but not with
    // -datacarriersize=83
    const CScript null_data_84 = CScript() << OP_RETURN << std::vector<uint8_t>(81);
    BOOST_CHECK_EQUAL(null_data_84.size(), 84U);
    BOOST_CHECK(IsStandardTx(pegout_tx(null_data_84), reason));
    nMaxDatacarrierBytes = 83;
    BOOST_CHECK(!IsStandardTx(pegout_tx(null_data_84), reason));
    BOOST_CHECK_EQUAL(reason, "scriptpubkey");
    nMaxDatacarrierBytes = MAX_OP_RETURN_RELAY;

    // The largest OP_RETURN peg-out script within MAX_STANDARD_TX_WEIGHT is
    // 99,987 bytes: the weight counts the peg-out as an output of an 8-byte
    // amount, a 5-byte script length and the script, 100,000 bytes in all.
    // One byte more is too large.
    std::vector<uint8_t> data(99'987 - 6); // 6 bytes for OP_RETURN, OP_PUSHDATA4 and its 4-byte length
    const CTransaction largest = pegout_tx(CScript() << OP_RETURN << data);
    BOOST_CHECK_EQUAL(GetTransactionWeight(largest), MAX_STANDARD_TX_WEIGHT);
    BOOST_CHECK(IsStandardTx(largest, reason));
    BOOST_CHECK(MWEB::Policy::CheckWeight(largest, reason));
    data.push_back(0);
    reason.clear();
    BOOST_CHECK(!IsStandardTx(pegout_tx(CScript() << OP_RETURN << data), reason));
    BOOST_CHECK_EQUAL(reason, "tx-size");
}

BOOST_AUTO_TEST_SUITE_END()
