// Copyright (c) 2015-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

/** The time and the compact target of a block: all that the difficulty rules read from the chain. */
struct BlockData {
    uint32_t nTime;
    uint32_t nBits;
};

/** A block header without the hash of the previous block, which the chain it is put in supplies. */
struct HeaderData {
    int32_t nVersion;
    const char* hashMerkleRoot;
    uint32_t nTime;
    uint32_t nBits;
    uint32_t nNonce;
};

/** Mainnet blocks 1 to 10: the blocks after the genesis block, up to the first retarget. */
const std::vector<HeaderData> MAIN_HEADERS_FROM_1{
    {0x20000000, "60c4a9a1e675ac43ff31d25bfd48920f1dae89b70b0930bcebb9c3321ba54745", 1669139525, 0x1e0ffff0, 13848}, // 1
    {0x20000000, "42ea8d34d3bbf887061a0e9229af4f365d4f539da79ee844651a1e1901c33ca8", 1669139786, 0x1e0ffff0, 9745}, // 2
    {0x20000000, "4f3a8f825652d0e0f8262db49070eb119846f3d8ec6d93ad1ce98140b26e0129", 1669139925, 0x1e0ffff0, 31278}, // 3
    {0x20000000, "d2a3fd6f5d4adb31d8482798cab03c2ff18398f276e3ebf2af5d2e05b0966efe", 1669139962, 0x1e0ffff0, 22467}, // 4
    {0x20000000, "7d6977669d6a2aba5666e6aa323af725ada8abe9753a55e0359f48b50a00403b", 1669139999, 0x1e0ffff0, 52584}, // 5
    {0x20000000, "1f5b35bc20a6262b46b64f8fdb149f214e38b03fe9493edc905eddf43abbca67", 1669140118, 0x1e0ffff0, 54155}, // 6
    {0x20000000, "551d67cf62c06e144d5acb2ffef66b0ff3f51bcc3b3ca417ad686609e779c357", 1669140373, 0x1e0ffff0, 9159}, // 7
    {0x20000000, "72abda613ab2c1d7a0ad970ec2bc73e512249570dfdb41ab3f424d08d8341d7d", 1669140437, 0x1e0ffff0, 40303}, // 8
    {0x20000000, "a02adae5111f3d1b4f321f1706ad714c15a58839e0b02459991fb392c9bd22eb", 1669140578, 0x1e0ffff0, 52257}, // 9
    {0x20000000, "a8ad3bbd3c9c86411230df5472c61bd911ca7c03cf0b4ce437b1677e035d2879", 1669140667, 0x1e0fffff, 159}, // 10
};

/** Mainnet blocks 1,088,969 to 1,089,000. The first builds on block 1,088,968; the last is a checkpoint. */
const char* const MAIN_HASH_1088968 = "17a6e279e228ab8fd5ac155f8c909f058742eb3b69c4258278a3b69d1db541c6";
const std::vector<HeaderData> MAIN_HEADERS_FROM_1088969{
    {0x20000000, "d4a9448bb992ba6ecaa3f17f72e5a2e7b9bd8551ac81e2f4eab00c863b21f5f8", 1794303387, 0x1c1d3a03, 2939125874}, // 1,088,969
    {0x20000000, "37c3721aa7e8c7474e115792338c7469e551a8c8cb704d442ca2830f2b38e1c2", 1794303464, 0x1c18cfeb, 1370149713}, // 1,088,970
    {0x20000000, "bac4f7fa91f2ffeaa8aa202d12ae6789deca79326015b634d9f45d63279ab8ea", 1794303471, 0x1c1a30d0, 3143314263}, // 1,088,971
    {0x20000000, "d242d19e33fa0bf3f3e4de130a910c4d14831afd8103db7679f610ee43a9905f", 1794303544, 0x1c18b3d6, 2738464823}, // 1,088,972
    {0x20000000, "620f640e71a550fdd2cc8c3ad6584ca74d21400526f73361013f3fedf133b719", 1794303584, 0x1c176aec, 1695055376}, // 1,088,973
    {0x20000000, "3ccd1fcdf3c135daee599d715ebb33fb1b50551544209b4870cce800f82302ab", 1794303664, 0x1c179d72, 1503658044}, // 1,088,974
    {0x20000000, "fd14f184796128c7ac8130c41bdca241d8c1e7e2100e6a757baf77050c52c800", 1794303684, 0x1c185d42, 1702428699}, // 1,088,975
    {0x20000000, "5edf122ade5d5872c485fbe35b726ffaf99381176b54a97051b0a1cff5024200", 1794303724, 0x1c17516d, 2874802251}, // 1,088,976
    {0x20000000, "6629e79a9083c85b18db5fe2b32d3749b11bb8596bc5cf030ba82c211458a6c5", 1794303784, 0x1c15cc02, 2841905713}, // 1,088,977
    {0x20000000, "11d7b3f345576610aacc3f11d3ab9c33c60d6f94ec105f23349684ac1bf68638", 1794303919, 0x1c15e319, 4214063128}, // 1,088,978
    {0x20000000, "62d4595376c8e4deed7fcb6750b5f0ef970ff979d2192a915020b60c6a778b62", 1794304104, 0x1c180aec, 824770586}, // 1,088,979
    {0x20000000, "d0ba8f8ea8f585aaa98fd29b9c063f206320a3fc753623a3dbe3460fecedd9f4", 1794304164, 0x1c1abdf6, 2159804466}, // 1,088,980
    {0x20000000, "6011b79d23c12623fd7c1bfca4b35639adbb2630c8a7b27f9d87939f18e300e8", 1794304218, 0x1c1b63e5, 3327721589}, // 1,088,981
    {0x20000000, "a63fc6fe5b8923064d77abe203246a0c08a453c30390221d03fde0a2c508ab28", 1794304260, 0x1c1b5887, 3911122992}, // 1,088,982
    {0x20000000, "482f7744c6f0a54a3c359d990faed6cce54c43da4b21419680a046836f44db24", 1794304263, 0x1c1a38eb, 4221042731}, // 1,088,983
    {0x20000000, "8e56e93834ad9af534dc2b53cd641ef9f3953f55fcaffeeb0ade64bc222952a3", 1794304431, 0x1c1802e5, 1413316661}, // 1,088,984
    {0x20000000, "0943f1f4766284abec88c34a24f70f12f0cc55d59307def7ff1bc02cff54fc65", 1794304435, 0x1c18c61a, 318865442}, // 1,088,985
    {0x20000000, "a471a5337292b6e54e9b40739e82f6d045d120da2f3d0822f591a84dceb2e799", 1794304464, 0x1c172510, 3045523578}, // 1,088,986
    {0x20000000, "e7f47571c16914e3adc3ef8cf037b41828df2b7d4f26430f420ff7d45fcd5a09", 1794304531, 0x1c16e3ef, 572850274}, // 1,088,987
    {0x20000000, "834a7253b8cd0871a81e77010db91156ad70913673cf2033f961adfda71883f4", 1794304589, 0x1c16625a, 1588134245}, // 1,088,988
    {0x20000000, "898b4d0718b25134996767ff13156c5eba4535fdc0d6a7e76336a9996ddcb56a", 1794304684, 0x1c16dd83, 3545464906}, // 1,088,989
    {0x20000000, "9611cb392f4ba6871787cf9e4ca63225c6d85b4f32eddf0b4ec6dab3367f49a1", 1794304894, 0x1c17aa1d, 3840300111}, // 1,088,990
    {0x20000000, "80a3362bca27f676d17cca6ee38fd89b6463a91f7677d1a26c2a1433faa4209a", 1794304984, 0x1c1ae206, 2270789640}, // 1,088,991
    {0x20000000, "7ef80ebfae3fb524e6ae44719d7fc47f05b008692b9011b5d54fd0f0888091d2", 1794304994, 0x1c1ba831, 3476717654}, // 1,088,992
    {0x20000000, "4b062540de31345c890db88bdb84d7053d7ba4b3d87f8629a9a6576f0fd2e4d3", 1794305144, 0x1c1bb892, 440631165}, // 1,088,993
    {0x20000000, "21d2acc0738732d0ec1493e52732310543f6c7066a33432cad78df0a91ad61a8", 1794305164, 0x1c1ce9b8, 3774688058}, // 1,088,994
    {0x20000000, "232cea87b08d97e944aa88661cf5d1f1f2ff7e7967487fda2eb99b811d804344", 1794305183, 0x1c1d62b6, 3780323853}, // 1,088,995
    {0x20000000, "d90e927203df99d96ba83a2f8c7322f3991a2d9edebebbacb95cbe79168ef538", 1794305364, 0x1c1c9d83, 1025179725}, // 1,088,996
    {0x20000000, "6bae812ac526497ded2e4954dbae1d4a01660afea08cfb6f7400848d2581abe6", 1794305381, 0x1c1f3b7e, 2774073419}, // 1,088,997
    {0x20000000, "ece09ae860af931c4d9b231ddd78929f796a2c61bc95e42ef582d700c9eab5fe", 1794305424, 0x1c1e9fe0, 1813315691}, // 1,088,998
    {0x20000000, "2a84532f5e5a144d49a483c5906c83d9a39bc24096da31b668ba857e2b0d23a7", 1794305429, 0x1c1f581a, 4218912283}, // 1,088,999
    {0x20000000, "5972546e684c4c344537c49d11377e281771bbdd21ecbbfd64dd39b00b932b68", 1794305624, 0x1c1f140d, 4226711646}, // 1,089,000
};

/** Time and target of mainnet blocks 249,976 to 250,028: the last blocks of the first difficulty rule and the
 *  first blocks of Dark Gravity Wave, which starts at height 250,000. No checkpoint covers these blocks, so
 *  unlike the other two ranges they are not rebuilt from full headers and hashed here; the values were taken
 *  from a block explorer. */
const std::vector<BlockData> MAIN_BLOCKS_FROM_249976{
    {1736569122, 0x1b08002c}, {1736569125, 0x1b08002c}, {1736569126, 0x1b08002c}, {1736569127, 0x1b08002c}, // 249,976 to 249,979
    {1736569133, 0x1b02000b}, {1736569224, 0x1b02000b}, {1736569245, 0x1b02000b}, {1736569246, 0x1b02000b}, // 249,980 to 249,983
    {1736569247, 0x1b02000b}, {1736569263, 0x1b02000b}, {1736569268, 0x1b02000b}, {1736569271, 0x1b02000b}, // 249,984 to 249,987
    {1736569273, 0x1b02000b}, {1736570097, 0x1b02000b}, {1736570103, 0x1b033bcd}, {1736570112, 0x1b033bcd}, // 249,988 to 249,991
    {1736570116, 0x1b033bcd}, {1736570120, 0x1b033bcd}, {1736570122, 0x1b033bcd}, {1736570127, 0x1b033bcd}, // 249,992 to 249,995
    {1736570130, 0x1b033bcd}, {1736570132, 0x1b033bcd}, {1736570134, 0x1b033bcd}, {1736570138, 0x1b033bcd}, // 249,996 to 249,999
    {1736570139, 0x1b0278b6}, {1736570141, 0x1b024a1a}, {1736570143, 0x1b022024}, {1736570150, 0x1b01f50c}, // 250,000 to 250,003
    {1736571760, 0x1b01c89e}, {1736571965, 0x1b046b97}, {1736571981, 0x1b051f81}, {1736571983, 0x1b057124}, // 250,004 to 250,007
    {1736571987, 0x1b05bad1}, {1736571990, 0x1b060234}, {1736571991, 0x1b065410}, {1736571995, 0x1b06acc6}, // 250,008 to 250,011
    {1736571999, 0x1b070f39}, {1736572000, 0x1b05386e}, {1736572002, 0x1b05478c}, {1736572006, 0x1b055eda}, // 250,012 to 250,015
    {1736572007, 0x1b057ccb}, {1736572014, 0x1b059a61}, {1736572020, 0x1b05bf9d}, {1736572026, 0x1b05e434}, // 250,016 to 250,019
    {1736572029, 0x1b060c5a}, {1736572031, 0x1b063541}, {1736572032, 0x1b065f89}, {1736572034, 0x1b068977}, // 250,020 to 250,023
    {1736572037, 0x1b06b912}, {1736572038, 0x1b06f5ce}, {1736572041, 0x1b073701}, {1736572044, 0x1b077af7}, // 250,024 to 250,027
    {1736575151, 0x1b01f9d7}, // 250,028
};

/** Rebuild headers that follow the block with the hash prev_hash. Returns their times and targets and leaves the
 *  hash of the last header in prev_hash. Every header names the hash of the one before it, so that last hash is
 *  right only if every field of every header is. */
std::vector<BlockData> RebuildHeaders(uint256& prev_hash, const std::vector<HeaderData>& headers)
{
    std::vector<BlockData> blocks;
    for (const HeaderData& data : headers) {
        CBlockHeader header;
        header.nVersion = data.nVersion;
        header.hashPrevBlock = prev_hash;
        header.hashMerkleRoot = uint256S(data.hashMerkleRoot);
        header.nTime = data.nTime;
        header.nBits = data.nBits;
        header.nNonce = data.nNonce;
        prev_hash = header.GetHash();
        blocks.push_back({data.nTime, data.nBits});
    }
    return blocks;
}

/** count blocks with the target bits, spacing seconds apart. */
std::vector<BlockData> EvenBlocks(size_t count, uint32_t bits, uint32_t spacing = 60)
{
    std::vector<BlockData> blocks;
    for (size_t i = 0; i < count; ++i) {
        blocks.push_back({uint32_t(1700000000 + i * spacing), bits});
    }
    return blocks;
}

/** Block index entries at consecutive heights from first_height on, linked by pprev. The entries point at each
 *  other, so the vector must not be copied or resized. */
std::vector<CBlockIndex> BuildChain(int first_height, const std::vector<BlockData>& blocks)
{
    std::vector<CBlockIndex> chain(blocks.size());
    for (size_t i = 0; i < chain.size(); ++i) {
        chain[i].pprev = i ? &chain[i - 1] : nullptr;
        chain[i].nHeight = first_height + int(i);
        chain[i].nTime = blocks[i].nTime;
        chain[i].nBits = blocks[i].nBits;
    }
    return chain;
}

/** The nBits required of a block that is built on tip and whose time is seconds_after_tip later than the tip's. */
unsigned int NextWork(const CBlockIndex& tip, const Consensus::Params& params, int64_t seconds_after_tip = 60)
{
    CBlockHeader next;
    next.nTime = uint32_t(tip.GetBlockTime() + seconds_after_tip);
    return GetNextWorkRequired(&tip, &next, params);
}

/** The nBits required of chain[i], given the blocks before it and its own time. */
unsigned int WorkRequiredOf(const std::vector<CBlockIndex>& chain, size_t i, const Consensus::Params& params)
{
    return NextWork(chain[i - 1], params, chain[i].GetBlockTime() - chain[i - 1].GetBlockTime());
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1358118740; // Block #30240
    CBlockIndex pindexLast;
    pindexLast.nHeight = 280223;
    pindexLast.nTime = 1358378777;  // Block #280223
    pindexLast.nBits = 0x1c0ac141;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1c093f8dU);
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1317972665; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1318480354;  // Block #2015
    pindexLast.nBits = 0x1e0ffff0;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1e0fffffU);
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1401682934; // Block #66528
    CBlockIndex pindexLast;
    pindexLast.nHeight = 578591;
    pindexLast.nTime = 1401757934;  // Block #578591
    pindexLast.nBits = 0x1b075cf1;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1b01d73cU);
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    int64_t nLastRetargetTime = 1463690315; // NOTE: Not an actual block time
    CBlockIndex pindexLast;
    pindexLast.nHeight = 1001951;
    pindexLast.nTime = 1464900315;  // Block #46367
    pindexLast.nBits = 0x1b015318;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, chainParams->GetConsensus()), 0x1b054c60U);
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits = ~0x00800000;
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash.SetHex("0x1");
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p2 = &blocks[InsecureRandRange(10000)];
        CBlockIndex *p3 = &blocks[InsecureRandRange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, std::string chainName)
{
    const auto chainParams = CreateChainParams(args, chainName);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg && pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    /* Ferrite: we allow overflowing by 1 bit
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
        targ_max /= consensus.nPowTargetTimespan*4;
        BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
    }
    */
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_last_checkpoint)
{
    // The header of mainnet block 1,089,000, rebuilt from its fields. It must
    // hash to the last checkpoint and to the assumed-valid block, and its
    // scrypt hash must meet its own nBits. When either of them moves, put the
    // header of the new block here.
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& consensus = chainParams->GetConsensus();

    CBlockHeader header;
    header.nVersion = 0x20000000;
    header.hashPrevBlock = uint256S("0xc3c10b9686cf046ab9e682f90ada29ac2f888b2aae94b9488ff99d5765815c7a");
    header.hashMerkleRoot = uint256S("0x5972546e684c4c344537c49d11377e281771bbdd21ecbbfd64dd39b00b932b68");
    header.nTime = 1794305624;
    header.nBits = 0x1c1f140d;
    header.nNonce = 4226711646;

    const MapCheckpoints& checkpoints = chainParams->Checkpoints().mapCheckpoints;
    BOOST_REQUIRE(!checkpoints.empty());
    BOOST_CHECK_EQUAL(checkpoints.rbegin()->first, 1089000);
    BOOST_CHECK_EQUAL(checkpoints.rbegin()->second, header.GetHash());
    BOOST_CHECK_EQUAL(consensus.defaultAssumeValid, header.GetHash());
    BOOST_CHECK(CheckProofOfWork(header.GetPoWHash(), header.nBits, consensus));
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, CBaseChainParams::SIGNET);
}

/*
 * GetNextWorkRequired on chains of block index entries. Below nPowKGWHeight it uses the first difficulty rule (a
 * retarget every nPowTargetTimespan / nPowTargetSpacing blocks); from nPowDGWHeight on it uses Dark Gravity Wave,
 * which derives every block's target from the 24 blocks before it. The two heights are the same on every network.
 *
 * The expected values are either the nBits of real mainnet blocks, or were calculated apart from this code with
 * integer arithmetic that models the 256-bit wrap: every sum and product is reduced modulo 2^256, which is what
 * this code's 256-bit integers do. The 24 targets are combined newest first by avg = (avg * n + target) / (n + 1),
 * then avg * timespan / 1440, each division rounding down, the timespan being the time from the oldest of the 24
 * blocks to the newest, kept within 480 to 4320 seconds. The Kimoto Gravity Well values were calculated in the same
 * way, with double-precision floating point where that rule decides how many blocks it reads.
 */

/* The first rule on the real blocks that follow the mainnet genesis block. */
BOOST_AUTO_TEST_CASE(GetNextWorkRequired_main_first_blocks)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& consensus = chainParams->GetConsensus();
    const MapCheckpoints& checkpoints = chainParams->Checkpoints().mapCheckpoints;

    // The headers are those of the real blocks: they lead from the genesis block to the checkpoint at height 10.
    const CBlock& genesis = chainParams->GenesisBlock();
    uint256 hash = genesis.GetHash();
    std::vector<BlockData> blocks{{genesis.nTime, genesis.nBits}};
    for (const BlockData& block : RebuildHeaders(hash, MAIN_HEADERS_FROM_1)) blocks.push_back(block);
    BOOST_REQUIRE(checkpoints.count(10) == 1);
    BOOST_REQUIRE_EQUAL(hash, checkpoints.at(10));

    const std::vector<CBlockIndex> chain = BuildChain(0, blocks);
    for (size_t i = 1; i < chain.size(); ++i) {
        BOOST_CHECK_EQUAL(WorkRequiredOf(chain, i, consensus), chain[i].nBits);
    }
    // Blocks 1 to 9 keep the target of the genesis block. Block 10 is the first retarget: the blocks before it took
    // more than four times the target timespan, which quadruples the target, and the limit caps the result.
    BOOST_CHECK_EQUAL(chain[9].nBits, 0x1e0ffff0U);
    BOOST_CHECK_EQUAL(chain[10].nBits, 0x1e0fffffU);
    BOOST_CHECK_EQUAL(chain[10].nBits, UintToArith256(consensus.powLimit).GetCompact());
}

/* The real mainnet blocks around the height at which Dark Gravity Wave starts. */
BOOST_AUTO_TEST_CASE(GetNextWorkRequired_main_dgw_start)
{
    const Consensus::Params consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    BOOST_CHECK_EQUAL(consensus.nPowDGWHeight, 250000);

    const int first_height = 249976;
    const std::vector<CBlockIndex> chain = BuildChain(first_height, MAIN_BLOCKS_FROM_249976);
    BOOST_CHECK_EQUAL(chain.back().nHeight, 250028);
    // From block 249,990 on, every block that its rule reads is in the chain: ten blocks back for the retarget of
    // the first rule at 249,990, and 24 blocks back for Dark Gravity Wave from 250,000 on.
    for (size_t i = 249990 - first_height; i < chain.size(); ++i) {
        BOOST_CHECK_EQUAL(WorkRequiredOf(chain, i, consensus), chain[i].nBits);
    }
    // 249,990 was a retarget of the first rule, and the nine blocks after it kept its target.
    BOOST_CHECK(chain[249990 - first_height].nBits != chain[249989 - first_height].nBits);
    BOOST_CHECK_EQUAL(chain[249999 - first_height].nBits, chain[249990 - first_height].nBits);
    // From 250,000 on the target changes with every block.
    BOOST_CHECK(chain[250000 - first_height].nBits != chain[249999 - first_height].nBits);
    BOOST_CHECK(chain[250001 - first_height].nBits != chain[250000 - first_height].nBits);
    // The 24 blocks before 250,028 span 284 seconds, less than a third of 24 block intervals: the real chain met
    // the lower bound of the timespan there.
    BOOST_CHECK_EQUAL(chain[250027 - first_height].GetBlockTime() - chain[250004 - first_height].GetBlockTime(), 284);
}

/* Dark Gravity Wave on the real mainnet blocks before the checkpoint at height 1,089,000. */
BOOST_AUTO_TEST_CASE(GetNextWorkRequired_main_checkpoint_headers)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const Consensus::Params& consensus = chainParams->GetConsensus();
    const MapCheckpoints& checkpoints = chainParams->Checkpoints().mapCheckpoints;

    // The headers are those of the real blocks: the hash of the last one is the checkpoint.
    uint256 hash = uint256S(MAIN_HASH_1088968);
    const std::vector<BlockData> blocks = RebuildHeaders(hash, MAIN_HEADERS_FROM_1088969);
    BOOST_REQUIRE(checkpoints.count(1089000) == 1);
    BOOST_REQUIRE_EQUAL(hash, checkpoints.at(1089000));

    const std::vector<CBlockIndex> chain = BuildChain(1088969, blocks);
    BOOST_CHECK_EQUAL(chain.back().nHeight, 1089000);
    // The last eight blocks each have the 24 blocks that decide their target before them.
    BOOST_CHECK_EQUAL(chain.size() - 24, 8U);
    for (size_t i = 24; i < chain.size(); ++i) {
        BOOST_CHECK_EQUAL(WorkRequiredOf(chain, i, consensus), chain[i].nBits);
    }
    BOOST_CHECK_EQUAL(WorkRequiredOf(chain, chain.size() - 1, consensus), 0x1c1f140dU);
}

/* The bounds of the timespan that Dark Gravity Wave multiplies the average target by. */
BOOST_AUTO_TEST_CASE(DarkGravityWave_timespan_bounds)
{
    const Consensus::Params consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();

    // The targets of the real blocks 1,088,976 to 1,088,999, which decide the target of block 1,089,000. Only the
    // time of the newest block is changed, to set the time from the oldest of the 24 blocks to the newest.
    uint256 hash = uint256S(MAIN_HASH_1088968);
    std::vector<BlockData> blocks = RebuildHeaders(hash, MAIN_HEADERS_FROM_1088969);
    blocks.pop_back();
    std::vector<CBlockIndex> chain = BuildChain(1088969, blocks);
    CBlockIndex& tip = chain.back();
    const int64_t oldest_time = chain[chain.size() - 24].GetBlockTime();
    const auto next_work_for_span = [&](int64_t span) {
        tip.nTime = uint32_t(oldest_time + span);
        return NextWork(tip, consensus);
    };

    // The real span and the span of 24 block intervals, which leaves the average as it is.
    BOOST_CHECK_EQUAL(next_work_for_span(1705), 0x1c1f140dU);
    BOOST_CHECK_EQUAL(next_work_for_span(1440), 0x1c1a3f7bU);
    // A third of 24 block intervals is the shortest span that counts. A shorter one, none at all and a newest block
    // that is earlier than the oldest all count as 480 seconds.
    BOOST_CHECK_EQUAL(next_work_for_span(481), 0x1c08c47eU);
    BOOST_CHECK_EQUAL(next_work_for_span(480), 0x1c08bfd3U);
    BOOST_CHECK_EQUAL(next_work_for_span(479), 0x1c08bfd3U);
    BOOST_CHECK_EQUAL(next_work_for_span(0), 0x1c08bfd3U);
    BOOST_CHECK_EQUAL(next_work_for_span(-3600), 0x1c08bfd3U);
    // Three times 24 block intervals is the longest span that counts.
    BOOST_CHECK_EQUAL(next_work_for_span(4319), 0x1c4eb9c6U);
    BOOST_CHECK_EQUAL(next_work_for_span(4320), 0x1c4ebe71U);
    BOOST_CHECK_EQUAL(next_work_for_span(4321), 0x1c4ebe71U);
    BOOST_CHECK_EQUAL(next_work_for_span(1000000), 0x1c4ebe71U);
}

/* A Dark Gravity Wave result above the proof-of-work limit is replaced by the limit. */
BOOST_AUTO_TEST_CASE(DarkGravityWave_pow_limit)
{
    const Consensus::Params consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    const unsigned int limit = UintToArith256(consensus.powLimit).GetCompact();
    BOOST_CHECK_EQUAL(limit, 0x1e0fffffU);

    // 24 blocks with the target of the mainnet genesis block, which is just below the limit.
    std::vector<CBlockIndex> chain = BuildChain(300000, EvenBlocks(24, 0x1e0ffff0));
    CBlockIndex& tip = chain.back();
    const int64_t oldest_time = chain.front().GetBlockTime();
    const auto next_work_for_span = [&](int64_t span) {
        tip.nTime = uint32_t(oldest_time + span);
        return NextWork(tip, consensus);
    };

    // 24 blocks that are 60 seconds apart span 23 intervals, so blocks that are exactly on time lower the target.
    BOOST_CHECK_EQUAL(next_work_for_span(23 * 60), 0x1e0f5546U);
    BOOST_CHECK_EQUAL(next_work_for_span(24 * 60), 0x1e0ffff0U);
    // One second more gives a target above the limit (0x1e1002c8), and three times the span one far above it.
    BOOST_CHECK_EQUAL(next_work_for_span(24 * 60 + 1), limit);
    BOOST_CHECK_EQUAL(next_work_for_span(3 * 24 * 60), limit);
}

/* Dark Gravity Wave reads the 24 blocks up to the tip and needs a tip at height 24 or above. */
BOOST_AUTO_TEST_CASE(DarkGravityWave_window)
{
    const Consensus::Params main_consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();

    // No network starts Dark Gravity Wave below height 25, so the parameters are changed to start it at once.
    for (const std::string& chain_name : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        BOOST_CHECK(CreateChainParams(*m_node.args, chain_name)->GetConsensus().nPowDGWHeight > 24);
    }
    Consensus::Params params = main_consensus;
    params.nPowKGWHeight = 0;
    params.nPowDGWHeight = 0;

    // Times and targets of the real blocks 1,088,976 to 1,089,000, used as heights 0 to 24.
    uint256 hash = uint256S(MAIN_HASH_1088968);
    std::vector<BlockData> blocks = RebuildHeaders(hash, MAIN_HEADERS_FROM_1088969);
    blocks.erase(blocks.begin(), blocks.end() - 25);
    std::vector<CBlockIndex> chain = BuildChain(0, blocks);
    BOOST_CHECK_EQUAL(chain.back().nHeight, 24);

    // A tip at height 23 has 24 blocks up to and including it, but the limit is returned up to that height.
    BOOST_CHECK_EQUAL(NextWork(chain[23], params), 0x1e0fffffU);
    // A tip at height 24: the blocks at heights 1 to 24 decide.
    BOOST_CHECK_EQUAL(NextWork(chain[24], params), 0x1c21ec10U);
    // The block at height 0 is not read.
    chain[0].nTime -= 100000;
    chain[0].nBits = 0x1e0fffff;
    BOOST_CHECK_EQUAL(NextWork(chain[24], params), 0x1c21ec10U);
    // The block at height 1 is the oldest of the 24: its time starts the timespan.
    chain[1].nTime -= 300;
    BOOST_CHECK_EQUAL(NextWork(chain[24], params), 0x1c2773f1U);
}

/* The minimum-difficulty rules of testnet, and their absence on mainnet. */
BOOST_AUTO_TEST_CASE(GetNextWorkRequired_testnet_min_difficulty)
{
    const Consensus::Params consensus = CreateChainParams(*m_node.args, CBaseChainParams::TESTNET)->GetConsensus();
    const unsigned int limit = UintToArith256(consensus.powLimit).GetCompact();
    BOOST_CHECK_EQUAL(limit, 0x1f0fffffU);
    BOOST_CHECK(consensus.fPowAllowMinDifficultyBlocks);
    BOOST_CHECK(!consensus.fPowNoRetargeting);
    BOOST_CHECK_EQUAL(consensus.nPowDGWHeight, 26);

    // Heights 0 to 25, 60 seconds apart, all with the same target.
    std::vector<CBlockIndex> chain = BuildChain(0, EvenBlocks(26, 0x1e0ffff0));

    // Block 26 is the first that Dark Gravity Wave decides. A block up to four block intervals after the tip gets
    // the Dark Gravity Wave target.
    BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, 60), 0x1e0f5546U);
    BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, 240), 0x1e0f5546U);
    // A later block gets ten times the target of the tip.
    BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, 241), 0x1f009fffU);
    BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, 2 * 60 * 60), 0x1f009fffU);
    // A block more than two hours after the tip gets the limit.
    BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, 2 * 60 * 60 + 1), limit);
    // Ten times the target of the tip is capped at the limit.
    chain[25].nBits = limit;
    BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, 241), limit);

    // Below height 26 the first rule decides. Between its retargets, a block more than two block intervals after
    // the tip gets the limit, and any other block the target of the last block that was not mined at the limit.
    BOOST_CHECK_EQUAL(NextWork(chain[24], consensus, 120), 0x1e0ffff0U);
    BOOST_CHECK_EQUAL(NextWork(chain[24], consensus, 121), limit);
    chain[24].nBits = limit;
    BOOST_CHECK_EQUAL(NextWork(chain[24], consensus, 60), 0x1e0ffff0U);

    // Mainnet has no such rules: the time of the new block is not read.
    const Consensus::Params main_consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    BOOST_CHECK(!main_consensus.fPowAllowMinDifficultyBlocks);
    const std::vector<CBlockIndex> main_chain = BuildChain(300000, EvenBlocks(24, 0x1e0ffff0));
    for (const int64_t seconds : {1, 60, 241, 2 * 60 * 60 + 1}) {
        BOOST_CHECK_EQUAL(NextWork(main_chain.back(), main_consensus, seconds), 0x1e0f5546U);
    }
}

/* Regtest does not retarget: from the height at which Dark Gravity Wave would start, every block gets the limit. */
BOOST_AUTO_TEST_CASE(GetNextWorkRequired_regtest)
{
    const Consensus::Params consensus = CreateChainParams(*m_node.args, CBaseChainParams::REGTEST)->GetConsensus();
    const unsigned int limit = UintToArith256(consensus.powLimit).GetCompact();
    BOOST_CHECK_EQUAL(limit, 0x207fffffU);
    BOOST_CHECK(consensus.fPowAllowMinDifficultyBlocks);
    BOOST_CHECK(consensus.fPowNoRetargeting);
    BOOST_CHECK_EQUAL(consensus.nPowDGWHeight, 26);

    // Heights 0 to 25 with a target that is not the limit.
    const std::vector<CBlockIndex> chain = BuildChain(0, EvenBlocks(26, 0x1e0ffff0));
    for (const int64_t seconds : {1, 60, 241, 2 * 60 * 60 + 1}) {
        BOOST_CHECK_EQUAL(NextWork(chain[25], consensus, seconds), limit);
    }
    // Below height 26 the first rule decides, as on testnet between retargets...
    BOOST_CHECK_EQUAL(NextWork(chain[24], consensus, 120), 0x1e0ffff0U);
    BOOST_CHECK_EQUAL(NextWork(chain[24], consensus, 121), limit);
    // ...and at a retarget (block 20) the target of the tip is kept, however late the block is.
    BOOST_CHECK_EQUAL(NextWork(chain[19], consensus, 1000), 0x1e0ffff0U);
}

/*
 * On testnet the average target can be so close to the limit that its product with the timespan does not fit in
 * 256 bits. The calculation keeps the low 256 bits of the product, so the next target then comes out below the
 * limit instead of at it. The values below record what the calculation does today on testnet; they change when
 * testnet's difficulty rules do. On mainnet the product always fits.
 */
BOOST_AUTO_TEST_CASE(DarkGravityWave_testnet_timespan_product)
{
    const Consensus::Params consensus = CreateChainParams(*m_node.args, CBaseChainParams::TESTNET)->GetConsensus();
    const arith_uint256 limit = UintToArith256(consensus.powLimit);
    BOOST_CHECK_EQUAL(limit.GetCompact(), 0x1f0fffffU);

    // Heights 0 to 25, all mined at the limit; the blocks at heights 2 to 25 decide the target of block 26.
    std::vector<CBlockIndex> chain = BuildChain(0, EvenBlocks(26, limit.GetCompact()));
    CBlockIndex& tip = chain.back();
    const int64_t oldest_time = chain[2].GetBlockTime();
    const auto next_work_for_span = [&](int64_t span) {
        tip.nTime = uint32_t(oldest_time + span);
        return NextWork(tip, consensus);
    };

    // Up to 4096 seconds the product fits; the result is above the limit and is replaced by it.
    BOOST_CHECK_EQUAL(next_work_for_span(4096), 0x1f0fffffU);
    // From 4097 seconds up to the longest span that counts, it does not fit.
    BOOST_CHECK_EQUAL(next_work_for_span(4097), 0x1e02d555U);
    BOOST_CHECK_EQUAL(next_work_for_span(4319), 0x1f027a4cU);
    BOOST_CHECK_EQUAL(next_work_for_span(4320), 0x1f027d24U);
    BOOST_CHECK_EQUAL(next_work_for_span(5000), 0x1f027d24U);

    // The largest product of an average target and a timespan: it fits for mainnet's limit, not for testnet's.
    BOOST_CHECK_EQUAL(3 * 24 * consensus.nPowTargetSpacing, 4320);
    const arith_uint256 longest_span{4320};
    BOOST_CHECK(limit * longest_span / longest_span != limit);
    const Consensus::Params main_consensus = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    const arith_uint256 main_limit = UintToArith256(main_consensus.powLimit);
    BOOST_CHECK_EQUAL(3 * 24 * main_consensus.nPowTargetSpacing, 4320);
    BOOST_CHECK(main_limit * longest_span / longest_span == main_limit);
}

/* Kimoto Gravity Well would decide the blocks from nPowKGWHeight up to nPowDGWHeight. No network has such blocks. */
BOOST_AUTO_TEST_CASE(KimotoGravityWell_range)
{
    for (const std::string& chain_name : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const Consensus::Params consensus = CreateChainParams(*m_node.args, chain_name)->GetConsensus();
        BOOST_CHECK_EQUAL(consensus.nPowKGWHeight, consensus.nPowDGWHeight);
    }

    // With mainnet's parameters changed to give it a range, it reads up to 70 blocks back from the tip.
    Consensus::Params params = CreateChainParams(*m_node.args, CBaseChainParams::MAIN)->GetConsensus();
    params.nPowKGWHeight = 0;
    params.nPowDGWHeight = 1000;

    // 100 blocks with the target of mainnet block 1,089,000. On time, all 70 blocks are read; blocks that are much
    // faster or slower than that end the reading after 9 and after 47 blocks.
    const std::vector<CBlockIndex> on_time = BuildChain(0, EvenBlocks(100, 0x1c1f140d, 60));
    BOOST_CHECK_EQUAL(NextWork(on_time[99], params), 0x1c1ea264U);
    const std::vector<CBlockIndex> fast = BuildChain(0, EvenBlocks(100, 0x1c1f140d, 1));
    BOOST_CHECK_EQUAL(NextWork(fast[99], params), 0x1b75de0fU);
    const std::vector<CBlockIndex> slow = BuildChain(0, EvenBlocks(100, 0x1c1f140d, 600));
    BOOST_CHECK_EQUAL(NextWork(slow[99], params), 0x1d01302bU);
    // A tip at height 0 gets the limit.
    BOOST_CHECK_EQUAL(NextWork(on_time[0], params), 0x1e0fffffU);
}

BOOST_AUTO_TEST_SUITE_END()
