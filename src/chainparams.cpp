// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>

#include <chainparamsseeds.h>
#include <consensus/merkle.h>
#include <hash.h> // for signet block challenge hash
#include <tinyformat.h>
#include <util/system.h>
#include <util/strencodings.h>
#include <versionbitsinfo.h>

#include <assert.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */

// All blocks were mined after the release of the 22 Nov 2022 news of FTX.
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "ST 22Nov22 Singapore was second biggest user of FTX pre collapse";
    const CScript genesisOutputScript = CScript() << ParseHex("04a7cb90400675f171c818b53ea84938118e5f1c668a36ac4ca4cb2e502ae12cdadd5ab524eb2319d5e68f5433229e8d1dd0bf60e62f6d1ba09a05d48562d757a3") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

/**
 * Main network
 */
class CMainParams : public CChainParams {
public:
    CMainParams() {
        strNetworkID = CBaseChainParams::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 301107;       // Halving occurs every 301107 blocks which happens to be very close to 8 months for timekeeping.
        consensus.BIP16Height = 0;
        consensus.BIP34Height = 0;
        consensus.BIP34Hash = uint256S("0x46ca17415c18e43f5292034ebf9bbd10de80a61fc6dc17180e6609f33d3b48f3");
        consensus.BIP65Height = 0;
        consensus.BIP66Height = 0;
        consensus.CSVHeight = 0;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0; // segwit activation height + miner confirmation window
        consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 10 * 60;                   // A target Timespan of 1h. Not 10 blocks. If blocks take longer than usual, difficulty will adjust earlier than usual.
        consensus.nPowTargetSpacing = 1 * 60;                    // Block confirmation time of 60 seconds.
        consensus.nPowKGWHeight = 250000;
	consensus.nPowDGWHeight = 250000;
        consensus.fPowAllowMinDifficultyBlocks = false;         // All blocks must have minimum difficulty of 1
        consensus.fPowNoRetargeting = false;                   // Difficulty will be dynamically adjusted.
        consensus.nRuleChangeActivationThreshold = 39;        // 97.5% of 40 to count as consensus
        consensus.nMinerConfirmationWindow = 40;             // nPowTargetTimespan / nPowTargetSpacing * 4
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartHeight = 450000; // 2024 (after halving 1)
        // If 39 of 40 blocks in a window are mined with Taproot support after block 450000, then Taproot will be activated
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeoutHeight = 99999999; // never*
        // Taproot will be automatically activated after block 200000.

        // Deployment of MWEB (LIP-0002, LIP-0003, and LIP-0004)
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].nStartHeight = 150000; // 
        // MWEB can be put up for consensus voting in later versions when mining infrastructure is ready and compatible.
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].nTimeoutHeight = 99999999; // never*
        // MWEB will be automatically activated after block 99999999 for now.

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000050c151ee81a40c9");  
        // A total of 363688 892397 404361 hashes (363.7 PH) of work as of block 149000.

	// consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000050c151ee81a40c9");  
        // A total of 363688 892397 404361 hashes (363.7 PH) of work as of block 149000.
        // consensus.nMinimumChainWork = uint256S("0x000000000000000000000000000000000000000000000000009cec62dc44b76d");  
        // A total of  44170 005713 303405 hashes (44.17 PH) of work as of block 100000.
        // consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000025004406a93795");  
        // A total of  10414 866307 823509 hashes (10.41 PH) of work as of block 60000.
        // consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000003f1caaf8e7d1f");  
        // A total of   1110 277761 170719 hashes (1.110 PH) of work as of block 30000.
        // consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000aaf695561da1");  
        // A total of    187 976044 125601 hashes (188.0 TH) of work as of block 10000.
        
        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0xef695bb26b2655308cba06c2dd9b303c833db933d0cd872104f3073e471da2b1");  
        // Block 149000
	    
        // consensus.defaultAssumeValid = uint256S("0xef695bb26b2655308cba06c2dd9b303c833db933d0cd872104f3073e471da2b1");  
        // Block 149000
        // consensus.defaultAssumeValid = uint256S("0x022dc4410add84d46359013d45df952493c53343304296a9066fc3df03dc8297");  
        // Block 100000
        // consensus.defaultAssumeValid = uint256S("0xf38b639a8db731e7dac96eaae8f9ab443eaf85039433197345a72e1961d7f286");  
        // Block 60000
        // consensus.defaultAssumeValid = uint256S("0x64ddec3dde1a4fd6c41d06aacfc27694cfc9c3094574ae83fe51ef4740956a95");  
        // Block 30000
        // consensus.defaultAssumeValid = uint256S("0xf5da0fabe25733a186805366c0fdede73e2454e782083676e7627b8ec991ef9b");  
        // Block 10000

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0x4a;
        pchMessageStart[1] = 0x82;
        pchMessageStart[2] = 0x10;
        pchMessageStart[3] = 0xd9;
        nDefaultPort = 9574;
        nPruneAfterHeight = 500000;
        m_assumed_blockchain_size = 2;
        m_assumed_chain_state_size = 2;

        genesis = CreateGenesisBlock(1669136135, 1766816, 0x1e0ffff0, 1, 100 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x46ca17415c18e43f5292034ebf9bbd10de80a61fc6dc17180e6609f33d3b48f3"));
        assert(genesis.hashMerkleRoot == uint256S("0x3db2b5aa928b56b8f38dc404f5bdb9e76209906b91ba175361acdc2405b19592"));

        // Seed servers to connect to other nodes.
        // Default configuration file contains nodes of popular ferritecoin mining pools.
        vSeeds.emplace_back("node1.ferritecoin.org");  // node1.ferritecoin.org
        vSeeds.emplace_back("node2.ferritecoin.org");  // node2.ferritecoin.org
        vSeeds.emplace_back("node3.ferritecoin.org");  // node3.ferritecoin.org
        vSeeds.emplace_back("node4.ferritecoin.org");  // node4.ferritecoin.org
	
	// CryptoID Chainz explorer
	vSeeds.emplace_back("46.105.34.58");  // https://btc.cryptoid.info/fec/
	    
        // Pool seednodes
        vSeeds.emplace_back("188.165.227.178");  // spools.online     
        vSeeds.emplace_back("144.91.107.170");   // coinxpool.com
        vSeeds.emplace_back("155.138.247.235");  // miningmypool.com 

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,36);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,5);
        base58Prefixes[SCRIPT_ADDRESS2] = std::vector<unsigned char>(1,35);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,163);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "fec";
        mweb_hrp = "fecmweb";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_main), std::end(chainparams_seed_main));

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        m_is_test_chain = false;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {        0, uint256S("0x46ca17415c18e43f5292034ebf9bbd10de80a61fc6dc17180e6609f33d3b48f3")},
                {        1, uint256S("0xc57e131b5a4e037b0ae4f6479464861b55bad6cdc934becd82fd78aa943cb731")},
                {       10, uint256S("0xc11ad445b0d130cf4c76fca76c26fd58d08a0be1993206d1fabdf84da4032ee5")},
                {       16, uint256S("0x85b9d245dc36364e19729250c5dcaa1019941833b4146cb5576d99d028208e48")},
                {       45, uint256S("0x6f89572fd7463f191d90f3a9c47e2e4e6ca6222480ca55f3ba596ce56ee25690")},
                {      101, uint256S("0xbc4383276f530a020085cc2ac9283050e6f681a9164aa9bc3fd2d0e7276b7621")},
                {      578, uint256S("0x0ac60bc57de1e70e9246e43dac68b2e03bb07d8572decce24eb12ba37648cdf8")},
                {     4072, uint256S("0x209f38181db9771939a131651b650451a319566d010a6f82c553b357f42aa6b0")},
                {    10000, uint256S("0xf5da0fabe25733a186805366c0fdede73e2454e782083676e7627b8ec991ef9b")},
                {    30000, uint256S("0x64ddec3dde1a4fd6c41d06aacfc27694cfc9c3094574ae83fe51ef4740956a95")},
                {    60000, uint256S("0xf38b639a8db731e7dac96eaae8f9ab443eaf85039433197345a72e1961d7f286")},
                {   100000, uint256S("0x022dc4410add84d46359013d45df952493c53343304296a9066fc3df03dc8297")},
		{   149000, uint256S("0xef695bb26b2655308cba06c2dd9b303c833db933d0cd872104f3073e471da2b1")},
            }
        };

        chainTxData = ChainTxData{
            /* nTime    */ 1687991792,
            /* nTxCount */ 162810,
            /* dTxRate  */ 0.00492770
        };
    }
};

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        strNetworkID = CBaseChainParams::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 301107;
        consensus.BIP16Height = 0; // always enforce P2SH BIP16 on testnet
        consensus.BIP34Height = 76;
        consensus.BIP34Hash = uint256S("0x46ca17415c18e43f5292034ebf9bbd10de80a61fc6dc17180e6609f33d3b48f3");
        consensus.BIP65Height = 76; // 8075c771ed8b495ffd943980a95f702ab34fce3c8c54e379548bda33cc8c0573
        consensus.BIP66Height = 76; // 8075c771ed8b495ffd943980a95f702ab34fce3c8c54e379548bda33cc8c0573
        consensus.CSVHeight = 0; // 00000000025e930139bac5c6c31a403776da130831ab85be56578f3fa75369bb
        consensus.SegwitHeight = 0; // 00000000002b980fcd729daaa248fd9316a5200e9b367f4ff2c42453e84201ca
        consensus.MinBIP9WarningHeight = 0; // segwit activation height + miner confirmation window
        consensus.powLimit = uint256S("000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 10 * 60; // 1 hour
        consensus.nPowTargetSpacing = 1 * 60;
        consensus.nPowKGWHeight = 26; // DGW uses past 24 blocks
	consensus.nPowDGWHeight = 26; // DGW uses past 24 blocks
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 10; // 50% for testchains
        consensus.nMinerConfirmationWindow = 20; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // Deployment of Taproot (BIPs 340-342)
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartHeight = 10; 
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeoutHeight = 20;

        // Deployment of MWEB (LIP-0002, LIP-0003, and LIP-0004)
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].nStartHeight = 160; 
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].nTimeoutHeight = 99999999;
	    
	
        consensus.nMinimumChainWork = uint256S("0x00000000000000000000000000000000000000000000000000000000019a894b");
	// 26 904907  (26.9 MH) hashes of work since block 250. (testnet)
        consensus.defaultAssumeValid = uint256S("0xd710251db07b4b5ad58ff59edcda83642af83e757fdf791424cf9d85e977bd65");

        pchMessageStart[0] = 0xba;
        pchMessageStart[1] = 0x76;
        pchMessageStart[2] = 0xab;
        pchMessageStart[3] = 0x8a;
        nDefaultPort = 19574;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 2;
        m_assumed_chain_state_size = 1;

        genesis = CreateGenesisBlock(1669136958, 336866, 0x1e0ffff0, 1, 100 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x7a9f43d6e86eefa66e2b79918b2235c9362106f3d9f11f37f7a33450ceae73c1"));
        assert(genesis.hashMerkleRoot == uint256S("0x3db2b5aa928b56b8f38dc404f5bdb9e76209906b91ba175361acdc2405b19592"));

        vFixedSeeds.clear();
        vSeeds.clear();
        // nodes with support for servicebits filtering should be at the top
        vSeeds.emplace_back("test1.ferritecoin.org");  // node1.ferritecoin.org
        vSeeds.emplace_back("test2.ferritecoin.org");  // node1.ferritecoin.org
        vSeeds.emplace_back("test3.ferritecoin.org");  // node1.ferritecoin.org
        

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SCRIPT_ADDRESS2] = std::vector<unsigned char>(1,35);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tfec";
        mweb_hrp = "tfecmweb";

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_test), std::end(chainparams_seed_test));

        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        m_is_test_chain = true;
        m_is_mockable_chain = false;

        checkpointData = {
            {
                {        0, uint256S("0x7a9f43d6e86eefa66e2b79918b2235c9362106f3d9f11f37f7a33450ceae73c1")},
		{        1, uint256S("0x9fe0eff34a1501c47e476c66d9cdca3133aebf3b1d8e2db13be04525f39301aa")},
		{       10, uint256S("0x269cc5bc0f63df93ca6e65a6d39ab4450dcbba5c6f9069bcfe141071e42171b0")},     // taproot compatible
		{       20, uint256S("0xc57940588a334cecf9bbaa1434892c074a6c532ea52c1b0adf9b1a55c07372ff")},     // taproot mandatory
		{       28, uint256S("0xdb2b80687bd0d731e052b2e1370f36409fdbfd1b1852b4e66d2f30df1f3d684e")},     // DarkGravityWave v3 hardfork
		{      100, uint256S("0x6e423dcbe5e9f98776f856cf54eafc00d65f42f4c4718cd6caaf9cd45711c129")},     
		{      160, uint256S("0xbf82199c7f3985ebd673372a328e6a1cf409c46d18c505e0fc2536f6c51ac885")},     // MWEB compatible
		{      250, uint256S("0xd710251db07b4b5ad58ff59edcda83642af83e757fdf791424cf9d85e977bd65")},
		{     1000, uint256S("0xb17ea0a86515e347878e11ccbddc2f9b0769f13418681b6bc95f1384ec58b38f")},
		{     6000, uint256S("0x0e753be814e8c5cb8801ad5a291a9e52c7e06eeb01a8cd3ab92dad1c5c67afe6")},
		    
            }
        };

        chainTxData = ChainTxData{
            // Data from RPC: getchaintxstats 4096 36d8ad003bac090cf7bf4e24fbe1d319554c8933b9314188d6096ac12648764d
            /* nTime    */ 1681409764,
            /* nTxCount */ 334,
            /* dTxRate  */ 0.00382938753883577,
        };
    }
};

/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    explicit CRegTestParams(const ArgsManager& args) {
        strNetworkID =  CBaseChainParams::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        consensus.BIP16Height = 0;
        consensus.BIP34Height = 500; // BIP34 activated on regtest (Used in functional tests)
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1351; // BIP65 activated on regtest (Used in functional tests)
        consensus.BIP66Height = 1251; // BIP66 activated on regtest (Used in functional tests)
        consensus.CSVHeight = 432; // CSV activated on regtest (Used in rpc activation tests)
        consensus.SegwitHeight = 0; // SEGWIT is always activated on regtest unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 10 * 60; // 1 hour
        consensus.nPowTargetSpacing = 1 * 60;
        consensus.nPowKGWHeight = 26;
	consensus.nPowDGWHeight = 26;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.nRuleChangeActivationThreshold = 108; // 75% for testchains
        consensus.nMinerConfirmationWindow = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TAPROOT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // Deployment of MWEB (LIP-0002 and LIP-0003)
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].nStartTime = 1601450001; // September 30, 2020
        consensus.vDeployments[Consensus::DEPLOYMENT_MWEB].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
        nDefaultPort = 19444;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        UpdateActivationParametersFromArgs(args);

        genesis = CreateGenesisBlock(1296688602, 0, 0x207fffff, 1, 100 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        fDefaultConsistencyChecks = true;
        fRequireStandard = true;
        m_is_test_chain = true;
        m_is_mockable_chain = true;

        checkpointData = {
            {
                {0, uint256S("492258df125bae6c6f95eed66a5d6d8be785bcb7d10da025226b57a5c0e42837")},
            }
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SCRIPT_ADDRESS2] = std::vector<unsigned char>(1,58);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "rfec";
        mweb_hrp = "tmweb";
    }

    /**
     * Allows modifying the Version Bits regtest parameters.
     */
    void UpdateVersionBitsParameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout, int64_t nStartHeight, int64_t nTimeoutHeight)
    {
        consensus.vDeployments[d].nStartTime = nStartTime;
        consensus.vDeployments[d].nTimeout = nTimeout;
        consensus.vDeployments[d].nStartHeight = nStartHeight;
        consensus.vDeployments[d].nTimeoutHeight = nTimeoutHeight;
    }
    void UpdateActivationParametersFromArgs(const ArgsManager& args);
};

void CRegTestParams::UpdateActivationParametersFromArgs(const ArgsManager& args)
{
    if (args.IsArgSet("-segwitheight")) {
        int64_t height = args.GetArg("-segwitheight", consensus.SegwitHeight);
        if (height < -1 || height >= std::numeric_limits<int>::max()) {
            throw std::runtime_error(strprintf("Activation height %ld for segwit is out of valid range. Use -1 to disable segwit.", height));
        } else if (height == -1) {
            LogPrintf("Segwit disabled for testing\n");
            height = std::numeric_limits<int>::max();
        }
        consensus.SegwitHeight = static_cast<int>(height);
    }

    if (!args.IsArgSet("-vbparams")) return;

    for (const std::string& strDeployment : args.GetArgs("-vbparams")) {
        std::vector<std::string> vDeploymentParams;
        boost::split(vDeploymentParams, strDeployment, boost::is_any_of(":"));
        if (vDeploymentParams.size() < 3 || 5 < vDeploymentParams.size()) {
            throw std::runtime_error("Version bits parameters malformed, expecting deployment:start:end[:heightstart:heightend]");
        }
        int64_t nStartTime, nTimeout, nStartHeight, nTimeoutHeight;
        if (!ParseInt64(vDeploymentParams[1], &nStartTime)) {
            throw std::runtime_error(strprintf("Invalid nStartTime (%s)", vDeploymentParams[1]));
        }
        if (!ParseInt64(vDeploymentParams[2], &nTimeout)) {
            throw std::runtime_error(strprintf("Invalid nTimeout (%s)", vDeploymentParams[2]));
        }
        if (vDeploymentParams.size() > 3 && !ParseInt64(vDeploymentParams[3], &nStartHeight)) {
            throw std::runtime_error(strprintf("Invalid nStartHeight (%s)", vDeploymentParams[3]));
        }
        if (vDeploymentParams.size() > 4 && !ParseInt64(vDeploymentParams[4], &nTimeoutHeight)) {
            throw std::runtime_error(strprintf("Invalid nTimeoutHeight (%s)", vDeploymentParams[4]));
        }
        bool found = false;
        for (int j=0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
            if (vDeploymentParams[0] == VersionBitsDeploymentInfo[j].name) {
                UpdateVersionBitsParameters(Consensus::DeploymentPos(j), nStartTime, nTimeout, nStartHeight, nTimeoutHeight);
                found = true;
                LogPrintf("Setting version bits activation parameters for %s to start=%ld, timeout=%ld, start_height=%d, timeout_height=%d\n", vDeploymentParams[0], nStartTime, nTimeout, nStartHeight, nTimeoutHeight);
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(strprintf("Invalid deployment (%s)", vDeploymentParams[0]));
        }
    }
}

static std::unique_ptr<const CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<const CChainParams> CreateChainParams(const ArgsManager& args, const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN) {
        return std::unique_ptr<CChainParams>(new CMainParams());
    } else if (chain == CBaseChainParams::TESTNET) {
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    } else if (chain == CBaseChainParams::SIGNET) {
        return std::unique_ptr<CChainParams>(new CTestNetParams()); // TODO: Support SigNet
    } else if (chain == CBaseChainParams::REGTEST) {
        return std::unique_ptr<CChainParams>(new CRegTestParams(args));
    }
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(gArgs, network);
}
