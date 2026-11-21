// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/p2mr_construction.json.h>
#include <test/data/p2mr_pqc_construction.json.h>
#include <test/data/p2mr_spend.json.h>

#include <amount.h>
#include <bech32.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <crypto/sha256.h>
#include <hash.h>
#include <key.h>
#include <key_io.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/script_error.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <validation.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

// In validation.cpp
bool CheckInputScripts(const CTransaction& tx, TxValidationState& state, const CCoinsViewCache& inputs, unsigned int flags, bool cacheSigStore, bool cacheFullScriptStore, PrecomputedTransactionData& txdata, std::vector<CScriptCheck>* pvChecks);

namespace {

typedef std::vector<unsigned char> valtype;

/** The script flags of a block in which Taproot and P2MR are active. */
const unsigned int CONSENSUS_FLAGS = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY |
                                     SCRIPT_VERIFY_CHECKSEQUENCEVERIFY | SCRIPT_VERIFY_TAPROOT | SCRIPT_VERIFY_WITNESS |
                                     SCRIPT_VERIFY_NULLDUMMY | SCRIPT_VERIFY_P2MR;
/** The script flags transactions are relayed with. */
const unsigned int POLICY_FLAGS = STANDARD_SCRIPT_VERIFY_FLAGS;

const uint8_t LEAF_TAPSCRIPT = 0xc0;

/** A scriptPubKey that pushes a small number and then data_size bytes: a witness program when data_size is 2 to 40. */
CScript ProgramScript(opcodetype version, size_t data_size, unsigned char fill)
{
    return CScript() << version << valtype(data_size, fill);
}

/** A transaction with one input for every spent output; an input has a witness exactly when with_witness says so. */
CMutableTransaction SpendingTransaction(const std::vector<CTxOut>& spent_outputs, const std::vector<bool>& with_witness)
{
    CMutableTransaction tx;
    tx.nVersion = 2;
    for (size_t i = 0; i < spent_outputs.size(); ++i) {
        CTxIn txin(COutPoint(uint256S("11"), i), CScript(), 0xfffffffd - i);
        if (with_witness[i]) txin.scriptWitness.stack = {valtype{0x01}, valtype{0x51}, valtype{0xc1}};
        tx.vin.push_back(txin);
    }
    tx.vout.emplace_back(1000, ProgramScript(OP_0, 20, 0x77));
    return tx;
}

/** The state of the precomputed data after Init, as two flags and what they need. */
struct InitResult {
    bool spent_outputs_ready;
    bool bip143_ready;
    bool bip341_ready;
};

InitResult InitFor(const std::vector<CTxOut>& spent_outputs, const std::vector<bool>& with_witness, bool give_spent_outputs = true)
{
    const CMutableTransaction tx = SpendingTransaction(spent_outputs, with_witness);
    PrecomputedTransactionData txdata;
    txdata.Init(tx, give_spent_outputs ? std::vector<CTxOut>(spent_outputs) : std::vector<CTxOut>());
    return {txdata.m_spent_outputs_ready, txdata.m_bip143_segwit_ready, txdata.m_bip341_taproot_ready};
}

void CheckInit(const InitResult& got, bool spent_outputs_ready, bool bip143_ready, bool bip341_ready, const std::string& context)
{
    BOOST_CHECK_MESSAGE(got.spent_outputs_ready == spent_outputs_ready, context + ": m_spent_outputs_ready");
    BOOST_CHECK_MESSAGE(got.bip143_ready == bip143_ready, context + ": m_bip143_segwit_ready");
    BOOST_CHECK_MESSAGE(got.bip341_ready == bip341_ready, context + ": m_bip341_taproot_ready");
}

// The hashes of BIP 340, 341 and 360, computed here from SHA256 alone so that the tests do not depend on the code
// they check.

uint256 Sha256(const valtype& data)
{
    uint256 result;
    CSHA256().Write(data.data(), data.size()).Finalize(result.begin());
    return result;
}

uint256 TaggedSha256(const std::string& tag, const valtype& data)
{
    const uint256 tag_hash = Sha256(valtype(tag.begin(), tag.end()));
    uint256 result;
    CSHA256().Write(tag_hash.begin(), 32).Write(tag_hash.begin(), 32).Write(data.data(), data.size()).Finalize(result.begin());
    return result;
}

void AppendBytes(valtype& to, const valtype& bytes) { to.insert(to.end(), bytes.begin(), bytes.end()); }
void AppendBytes(valtype& to, const uint256& hash) { to.insert(to.end(), hash.begin(), hash.end()); }
void AppendLE(valtype& to, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i) to.push_back((value >> (8 * i)) & 0xff);
}
void AppendCompactSize(valtype& to, uint64_t size)
{
    if (size < 253) {
        to.push_back(size);
    } else {
        BOOST_REQUIRE(size <= 0xffff);
        to.push_back(253);
        AppendLE(to, size, 2);
    }
}
void AppendWithSize(valtype& to, const valtype& bytes)
{
    AppendCompactSize(to, bytes.size());
    AppendBytes(to, bytes);
}

valtype ScriptBytes(const CScript& script) { return valtype(script.begin(), script.end()); }

/** hash_TapLeaf(leaf version || compact_size(size of script) || script) */
uint256 LeafHash(uint8_t leaf_version, const valtype& script)
{
    valtype data{leaf_version};
    AppendWithSize(data, script);
    return TaggedSha256("TapLeaf", data);
}

/** hash_TapBranch of two nodes, the lexicographically smaller one first. */
uint256 BranchHash(const uint256& a, const uint256& b)
{
    const bool a_first = std::memcmp(a.begin(), b.begin(), 32) < 0;
    valtype data;
    AppendBytes(data, a_first ? a : b);
    AppendBytes(data, a_first ? b : a);
    return TaggedSha256("TapBranch", data);
}

/** The root of the tree in which a leaf has these siblings on its way up, the one next to the leaf first. */
uint256 RootFromPath(const uint256& leaf_hash, const std::vector<uint256>& path)
{
    uint256 node = leaf_hash;
    for (const uint256& sibling : path) node = BranchHash(node, sibling);
    return node;
}

/** A P2MR control block: the leaf version with the lowest bit set, then the path. */
valtype ControlBlock(uint8_t leaf_version, const std::vector<uint256>& path)
{
    valtype control{static_cast<unsigned char>(leaf_version | 1)};
    for (const uint256& sibling : path) AppendBytes(control, sibling);
    return control;
}

/** n different node hashes to use as siblings. */
std::vector<uint256> Siblings(size_t n)
{
    std::vector<uint256> path;
    for (size_t i = 0; i < n; ++i) {
        valtype seed{'s', 'i', 'b', 'l', 'i', 'n', 'g'};
        AppendLE(seed, i, 2);
        path.push_back(Sha256(seed));
    }
    return path;
}

CScript P2MRScript(const uint256& root) { return CScript() << OP_2 << ToByteVector(root); }

/** Whether a script holds one of the OP_SUCCESSx opcodes of BIP 342 before a push that does not parse. */
bool HasOpSuccess(const valtype& script_bytes)
{
    const CScript script(script_bytes.begin(), script_bytes.end());
    CScript::const_iterator pc = script.begin();
    while (pc < script.end()) {
        opcodetype opcode;
        if (!script.GetOp(pc, opcode)) return false;
        const int op = opcode;
        if (op == 80 || op == 98 || (op >= 126 && op <= 129) || (op >= 131 && op <= 134) || (op >= 137 && op <= 138) ||
            (op >= 141 && op <= 142) || (op >= 149 && op <= 153) || (op >= 187 && op <= 254)) {
            return true;
        }
    }
    return false;
}

/** Verify one input of a transaction the way a block's script checks do, apart from the signature cache: the
 *  checker has the precomputed transaction data and stops the program if data it needs is missing. */
ScriptError VerifyInput(const CMutableTransaction& tx, const std::vector<CTxOut>& spent_outputs, unsigned int in_pos, unsigned int flags)
{
    PrecomputedTransactionData txdata;
    txdata.Init(tx, std::vector<CTxOut>(spent_outputs));
    ScriptError error = SCRIPT_ERR_UNKNOWN_ERROR;
    const bool ok = VerifyScript(tx.vin[in_pos].scriptSig, spent_outputs[in_pos].scriptPubKey, &tx.vin[in_pos].scriptWitness, flags,
                                 MutableTransactionSignatureChecker(&tx, in_pos, spent_outputs[in_pos].nValue, txdata, MissingDataBehavior::ASSERT_FAIL), &error);
    BOOST_CHECK_MESSAGE(ok == (error == SCRIPT_ERR_OK), "VerifyScript result and script error disagree");
    return error;
}

/** Verify a spend of script_pubkey with this witness (and scriptSig) in a transaction of its own. */
ScriptError VerifySpend(const CScript& script_pubkey, const std::vector<valtype>& witness, unsigned int flags, const CScript& script_sig = CScript())
{
    CMutableTransaction tx;
    tx.nVersion = 2;
    tx.vin.emplace_back(COutPoint(uint256S("22"), 0), script_sig, 0xfffffffe);
    tx.vin[0].scriptWitness.stack = witness;
    tx.vout.emplace_back(9000, ProgramScript(OP_0, 20, 0x77));
    return VerifyInput(tx, {CTxOut(10000, script_pubkey)}, 0, flags);
}

void CheckError(ScriptError got, ScriptError expected, const std::string& context)
{
    BOOST_CHECK_MESSAGE(got == expected, strprintf("%s: got \"%s\", expected \"%s\"", context, ScriptErrorString(got), ScriptErrorString(expected)));
}

void CheckSpend(const CScript& script_pubkey, const std::vector<valtype>& witness, unsigned int flags, ScriptError expected, const std::string& context)
{
    CheckError(VerifySpend(script_pubkey, witness, flags), expected, context);
}

/** Check a spend with the flags of a block and with the flags of relay. */
void CheckSpendBoth(const CScript& script_pubkey, const std::vector<valtype>& witness, ScriptError expected_consensus, ScriptError expected_policy, const std::string& context)
{
    CheckSpend(script_pubkey, witness, CONSENSUS_FLAGS, expected_consensus, context + " (block flags)");
    CheckSpend(script_pubkey, witness, POLICY_FLAGS, expected_policy, context + " (relay flags)");
}

void CheckSpendBoth(const CScript& script_pubkey, const std::vector<valtype>& witness, ScriptError expected, const std::string& context)
{
    CheckSpendBoth(script_pubkey, witness, expected, expected, context);
}

/** Errors that say the witness does not have the form of a P2MR spend or does not commit to the program, and the
 *  errors of other witness programs: none of them is the result of running a leaf script. */
bool IsCommitmentError(ScriptError error)
{
    return error == SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY || error == SCRIPT_ERR_P2MR_WITNESS_SIZE ||
           error == SCRIPT_ERR_P2MR_WRONG_CONTROL_SIZE || error == SCRIPT_ERR_P2MR_CONTROL_PARITY ||
           error == SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH || error == SCRIPT_ERR_WITNESS_PROGRAM_WRONG_LENGTH ||
           error == SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM || error == SCRIPT_ERR_WITNESS_UNEXPECTED ||
           error == SCRIPT_ERR_TAPROOT_WRONG_CONTROL_SIZE || error == SCRIPT_ERR_UNKNOWN_ERROR;
}

UniValue ReadJsonObject(const unsigned char* data, size_t size)
{
    UniValue json;
    BOOST_REQUIRE(json.read(std::string(data, data + size)));
    BOOST_REQUIRE(json.isObject());
    return json;
}

/** The project's vector of a P2MR spend: a tree with two leaves at depth 1, spent through leaf 0, a key and
 *  OP_CHECKSIG, by input 0 of a transaction whose other input spends a P2WPKH output. */
struct SpendVector {
    struct Spending {
        bool has_annex;
        valtype annex;
        std::string sig_msg;
        std::string sig_hash;
        valtype signature;
        std::vector<valtype> witness;
    };
    std::vector<valtype> scripts;
    std::vector<std::string> leaf_hashes;
    std::string merkle_root;
    CScript script_pubkey;
    std::vector<valtype> control_blocks;
    CMutableTransaction unsigned_tx;
    CMutableTransaction signed_tx;
    std::vector<CTxOut> spent_outputs;
    std::vector<Spending> spendings;
};

SpendVector LoadSpendVector()
{
    const UniValue json = ReadJsonObject(json_tests::p2mr_spend, sizeof(json_tests::p2mr_spend));
    SpendVector vector;
    const UniValue& tree = json["given"]["scriptTree"];
    BOOST_REQUIRE(tree.isArray() && tree.size() == 2);
    for (size_t i = 0; i < tree.size(); ++i) {
        BOOST_REQUIRE_EQUAL(tree[i]["id"].get_int(), (int)i);
        BOOST_REQUIRE_EQUAL(tree[i]["leafVersion"].get_int(), 0xc0);
        vector.scripts.push_back(ParseHex(tree[i]["script"].get_str()));
        vector.leaf_hashes.push_back(json["intermediary"]["leafHashes"][i].get_str());
        vector.control_blocks.push_back(ParseHex(json["expected"]["scriptPathControlBlocks"][i].get_str()));
    }
    vector.merkle_root = json["intermediary"]["merkleRoot"].get_str();
    const valtype script_pubkey = ParseHex(json["expected"]["scriptPubKey"].get_str());
    vector.script_pubkey = CScript(script_pubkey.begin(), script_pubkey.end());
    BOOST_REQUIRE(DecodeHexTx(vector.unsigned_tx, json["given"]["rawUnsignedTx"].get_str(), /* try_no_witness */ true, /* try_witness */ false));
    BOOST_REQUIRE(DecodeHexTx(vector.signed_tx, json["expected"]["rawSignedTx"].get_str(), /* try_no_witness */ false, /* try_witness */ true));
    const UniValue& utxos = json["given"]["utxosSpent"];
    for (size_t i = 0; i < utxos.size(); ++i) {
        const valtype script = ParseHex(utxos[i]["scriptPubKey"].get_str());
        vector.spent_outputs.emplace_back(utxos[i]["amountSats"].get_int64(), CScript(script.begin(), script.end()));
    }
    const UniValue& spendings = json["inputSpending"];
    for (size_t i = 0; i < spendings.size(); ++i) {
        const UniValue& given = spendings[i]["given"];
        // The vector is made for these values; the signature message below is written for them.
        BOOST_REQUIRE_EQUAL(given["txinIndex"].get_int(), 0);
        BOOST_REQUIRE_EQUAL(given["leafId"].get_int(), 0);
        BOOST_REQUIRE_EQUAL(given["hashType"].get_int(), 0);
        SpendVector::Spending spending;
        spending.has_annex = !given["annex"].isNull();
        if (spending.has_annex) spending.annex = ParseHex(given["annex"].get_str());
        spending.sig_msg = spendings[i]["intermediary"]["sigMsg"].get_str();
        spending.sig_hash = spendings[i]["intermediary"]["sigHash"].get_str();
        spending.signature = ParseHex(spendings[i]["expected"]["signature"].get_str());
        const UniValue& witness = spendings[i]["expected"]["witness"];
        for (size_t j = 0; j < witness.size(); ++j) spending.witness.push_back(ParseHex(witness[j].get_str()));
        vector.spendings.push_back(spending);
    }
    BOOST_REQUIRE_EQUAL(vector.unsigned_tx.vin.size(), 2U);
    BOOST_REQUIRE_EQUAL(vector.spent_outputs.size(), 2U);
    BOOST_REQUIRE_EQUAL(vector.spendings.size(), 2U);
    BOOST_REQUIRE(!vector.spendings[0].has_annex && vector.spendings[1].has_annex);
    return vector;
}

/** The bytes that are hashed with the tag TapSighash for a signature in a leaf script (BIP 341 "Common signature
 *  message" with ext_flag 1 and the extension of BIP 342), for input 0 and the default hash type, which covers
 *  every input and output. */
valtype TapscriptSigMsg(const CMutableTransaction& tx, const std::vector<CTxOut>& spent_outputs, const valtype* annex, const uint256& tapleaf_hash)
{
    valtype prevouts, amounts, script_pubkeys, sequences, outputs;
    for (const CTxIn& txin : tx.vin) {
        AppendBytes(prevouts, txin.prevout.hash);
        AppendLE(prevouts, txin.prevout.n, 4);
        AppendLE(sequences, txin.nSequence, 4);
    }
    for (const CTxOut& txout : spent_outputs) {
        AppendLE(amounts, txout.nValue, 8);
        AppendWithSize(script_pubkeys, ScriptBytes(txout.scriptPubKey));
    }
    for (const CTxOut& txout : tx.vout) {
        AppendLE(outputs, txout.nValue, 8);
        AppendWithSize(outputs, ScriptBytes(txout.scriptPubKey));
    }

    valtype msg;
    msg.push_back(0x00); // epoch
    msg.push_back(0x00); // hash_type: SIGHASH_DEFAULT
    AppendLE(msg, static_cast<uint32_t>(tx.nVersion), 4);
    AppendLE(msg, tx.nLockTime, 4);
    AppendBytes(msg, Sha256(prevouts));
    AppendBytes(msg, Sha256(amounts));
    AppendBytes(msg, Sha256(script_pubkeys));
    AppendBytes(msg, Sha256(sequences));
    AppendBytes(msg, Sha256(outputs));
    msg.push_back(2 + (annex ? 1 : 0)); // spend_type: ext_flag 1, and whether there is an annex
    AppendLE(msg, 0, 4);                // input index
    if (annex) {
        valtype annex_with_size;
        AppendWithSize(annex_with_size, *annex);
        AppendBytes(msg, Sha256(annex_with_size));
    }
    AppendBytes(msg, tapleaf_hash);
    msg.push_back(0x00);          // key_version
    AppendLE(msg, 0xffffffff, 4); // position of the last executed OP_CODESEPARATOR: none
    return msg;
}

/** A transaction whose input 0 spends a Taproot output (an internal key without a script tree) by its key path and
 *  whose input 1 spends a P2MR output through the leaf <x-only key> OP_CHECKSIG of a tree with two leaves at depth 1,
 *  and the outputs it spends. Both signatures are BIP 340 signatures with SIGHASH_DEFAULT and all-zero auxiliary
 *  randomness, made with the functional test framework; the test checks them before it uses them. */
const char* const TAPROOT_AND_P2MR_TX =
    "0200000000010213c18766d55a8fc0135dd82709c48ba5403d873605de0c1929fe1b1e730ce3c70000000000fdffffff9a689ddfde7f62e0"
    "96561b30c5fa64ec7ae0624219fc0ce3b028f6cbf58cf08a0100000000fdffffff01e022020000000000160014293d23f5822c5bbd501422"
    "e791c333d1dc23d71901409f056cfbeca724390f8be0f2e04017f24d9caf7778ac2d830553cc2f5643622bc17843abc2598b7fe30f4f14d7"
    "ba5f01415388fa699bae7b17b181fa078aece90340e90258efcb94e968ee8f2c37eb84b350322127f936dc3475846ab7df07a5d34f9c915a"
    "ab26e60c380431ac7153977442fd2774bb74c9966fc3ca7457bba1b6f52220f38daa41ff3895b0a312e3db42444d23527862daa3d2634f9e"
    "73760f274dad81ac21c1a85b2107f791b26a84e7586c28cec7cb61202ed3d01944d832500f363782d67500000000";
const CAmount TAPROOT_AND_P2MR_AMOUNTS[] = {100000, 50000};
const char* const TAPROOT_AND_P2MR_SCRIPTS[] = {
    "5120a7b11c9380c24bb0bba1c6be7c6c3c99b72c5a0528929220508ddbc5853f8a0f",
    "522084bcbd31d754525811394c9fe099aa3b1becdab55fb24aee63e240075d7a94b0",
};

CScript ScriptFromHex(const std::string& hex)
{
    const valtype bytes = ParseHex(hex);
    return CScript(bytes.begin(), bytes.end());
}

std::string ErrorsString(const std::map<int, std::string>& errors)
{
    std::string ret;
    for (const auto& error : errors) ret += strprintf("input %d: \"%s\"; ", error.first, error.second);
    return ret;
}

/** One leaf of a tree in a BIP 360 construction vector. */
struct VectorLeaf {
    int id;
    uint8_t version;
    valtype script;
    size_t depth;
};

/** Collect the leaves of a script tree of a BIP 360 vector (a leaf, or a list of two trees) and return its root. */
uint256 ReadScriptTree(const UniValue& tree, size_t depth, std::vector<VectorLeaf>& leaves)
{
    if (tree.isObject()) {
        VectorLeaf leaf{tree["id"].get_int(), static_cast<uint8_t>(tree["leafVersion"].get_int()), ParseHex(tree["script"].get_str()), depth};
        leaves.push_back(leaf);
        return LeafHash(leaf.version, leaf.script);
    }
    BOOST_REQUIRE(tree.isArray());
    BOOST_REQUIRE_EQUAL(tree.size(), 2U);
    const uint256 left = ReadScriptTree(tree[size_t{0}], depth + 1, leaves);
    const uint256 right = ReadScriptTree(tree[size_t{1}], depth + 1, leaves);
    return BranchHash(left, right);
}

/** What the leaf and control block pairs of one vector file are, counted to see that every kind is checked. */
struct VectorCounts {
    size_t error_vectors = 0;
    size_t error_vectors_with_script = 0;
    size_t spends = 0;
    size_t depth_zero = 0;
    size_t other_leaf_version = 0;
    size_t op_success = 0;
    size_t executed = 0;
};

VectorCounts CheckConstructionVectors(const unsigned char* data, size_t size, const std::string& file)
{
    VectorCounts counts;
    const UniValue json = ReadJsonObject(data, size);
    const UniValue& vectors = json["test_vectors"];
    BOOST_REQUIRE(vectors.isArray());
    for (size_t n = 0; n < vectors.size(); ++n) {
        const UniValue& vector = vectors[n];
        const std::string name = file + " " + vector["id"].get_str();
        const UniValue& given = vector["given"];
        const UniValue& expected = vector["expected"];
        const UniValue& tree = given.exists("scriptTree") ? given["scriptTree"] : given["script_tree"];

        if (expected.exists("error")) {
            // These vectors are about building an output: there is no script tree and so nothing to spend.
            BOOST_CHECK_MESSAGE(!expected["error"].get_str().empty(), name + ": empty error");
            BOOST_CHECK_MESSAGE(tree.isNull() || (tree.isStr() && tree.get_str().empty()), name + ": error vector with a script tree");
            if (expected.exists("scriptPubKey")) {
                // A key given where P2MR has none still gives OP_2 <32 bytes>, which has the P2MR output type.
                const valtype script_bytes = ParseHex(expected["scriptPubKey"].get_str());
                std::vector<valtype> solutions;
                BOOST_CHECK_MESSAGE(Solver(CScript(script_bytes.begin(), script_bytes.end()), solutions) == TxoutType::WITNESS_V2_P2MR, name + ": output type");
                ++counts.error_vectors_with_script;
            }
            ++counts.error_vectors;
            continue;
        }

        // The output: OP_2 and a push of the 32-byte Merkle root, which is also the program of the address.
        const valtype script_bytes = ParseHex(expected["scriptPubKey"].get_str());
        const CScript script_pubkey(script_bytes.begin(), script_bytes.end());
        int version;
        valtype program;
        BOOST_REQUIRE_MESSAGE(script_pubkey.IsWitnessProgram(version, program), name + ": not a witness program");
        BOOST_CHECK_MESSAGE(version == 2 && program.size() == WITNESS_V2_P2MR_SIZE && script_bytes.size() == 34 && script_bytes[0] == OP_2 && script_bytes[1] == 0x20, name + ": not OP_2 <32 bytes>");
        const bech32::DecodeResult address = bech32::Decode(expected["bip350Address"].get_str());
        BOOST_CHECK_MESSAGE(address.encoding == bech32::Encoding::BECH32M && address.hrp == "bc", name + ": address is not bech32m with prefix bc");
        BOOST_REQUIRE_MESSAGE(!address.data.empty() && address.data[0] == 2, name + ": address is not witness version 2");
        valtype address_program;
        const bool converted = ConvertBits<5, 8, false>([&](unsigned char c) { address_program.push_back(c); }, address.data.begin() + 1, address.data.end());
        BOOST_CHECK_MESSAGE(converted, name + ": address program has padding that is not zero");
        BOOST_CHECK_MESSAGE(address_program == program, name + ": address program differs from scriptPubKey");
        BOOST_CHECK_MESSAGE(HexStr(program) == vector["intermediary"]["merkleRoot"].get_str(), name + ": program is not the Merkle root");
        std::vector<valtype> solutions;
        BOOST_CHECK_MESSAGE(Solver(script_pubkey, solutions) == TxoutType::WITNESS_V2_P2MR, name + ": output type");
        BOOST_CHECK_MESSAGE(solutions == std::vector<valtype>({valtype{2}, program}), name + ": solutions of the output");

        // The tree: leaf hashes and root.
        std::vector<VectorLeaf> leaves;
        const uint256 root = ReadScriptTree(tree, 0, leaves);
        BOOST_CHECK_MESSAGE(valtype(root.begin(), root.end()) == program, name + ": root computed from the tree differs");
        const UniValue& leaf_hashes = vector["intermediary"]["leafHashes"];
        const UniValue& control_blocks = expected["scriptPathControlBlocks"];
        BOOST_REQUIRE_EQUAL(leaf_hashes.size(), leaves.size());
        BOOST_REQUIRE_EQUAL(control_blocks.size(), leaves.size());

        for (const VectorLeaf& leaf : leaves) {
            const std::string leaf_name = strprintf("%s leaf %d", name, leaf.id);
            BOOST_REQUIRE(leaf.id >= 0 && (size_t)leaf.id < leaves.size());
            BOOST_CHECK_MESSAGE(HexStr(LeafHash(leaf.version, leaf.script)) == leaf_hashes[static_cast<size_t>(leaf.id)].get_str(), leaf_name + ": leaf hash");
            const valtype control = ParseHex(control_blocks[static_cast<size_t>(leaf.id)].get_str());
            BOOST_REQUIRE_MESSAGE(control.size() == 1 + 32 * leaf.depth, leaf_name + ": control block size");
            BOOST_CHECK_MESSAGE(control[0] == (leaf.version | 1), leaf_name + ": control byte");
            ++counts.spends;

            // Spend the leaf with an empty initial stack. The control block commits to the program, so no result
            // is an error about the form of the witness or about the commitment.
            const std::vector<valtype> witness{leaf.script, control};
            const ScriptError consensus = VerifySpend(script_pubkey, witness, CONSENSUS_FLAGS);
            const ScriptError policy = VerifySpend(script_pubkey, witness, POLICY_FLAGS);
            BOOST_CHECK_MESSAGE(!IsCommitmentError(consensus), strprintf("%s: \"%s\" with block flags", leaf_name, ScriptErrorString(consensus)));
            BOOST_CHECK_MESSAGE(!IsCommitmentError(policy), strprintf("%s: \"%s\" with relay flags", leaf_name, ScriptErrorString(policy)));
            if (leaf.depth == 0) {
                // A leaf at depth zero is not run. The vectors have such leaves with leaf version 0xc0 only.
                BOOST_CHECK_MESSAGE(leaf.version == LEAF_TAPSCRIPT, leaf_name + ": depth-zero leaf with another leaf version");
                CheckError(consensus, SCRIPT_ERR_OK, leaf_name + " at depth zero (block flags)");
                CheckError(policy, SCRIPT_ERR_OK, leaf_name + " at depth zero (relay flags)");
                ++counts.depth_zero;
            } else if (leaf.version != LEAF_TAPSCRIPT) {
                // Another leaf version: valid in a block, not relayed.
                CheckError(consensus, SCRIPT_ERR_OK, leaf_name + " with another leaf version (block flags)");
                CheckError(policy, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION, leaf_name + " with another leaf version (relay flags)");
                ++counts.other_leaf_version;
            } else if (HasOpSuccess(leaf.script)) {
                // A tapscript with an OP_SUCCESSx opcode: valid in a block, not relayed.
                CheckError(consensus, SCRIPT_ERR_OK, leaf_name + " with OP_SUCCESSx (block flags)");
                CheckError(policy, SCRIPT_ERR_DISCOURAGE_OP_SUCCESS, leaf_name + " with OP_SUCCESSx (relay flags)");
                ++counts.op_success;
            } else {
                // Any other tapscript is run, and the result is the script's own.
                BOOST_CHECK_MESSAGE(consensus != SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION && consensus != SCRIPT_ERR_DISCOURAGE_OP_SUCCESS, leaf_name + ": discouraged with block flags");
                BOOST_CHECK_MESSAGE(policy != SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION && policy != SCRIPT_ERR_DISCOURAGE_OP_SUCCESS, leaf_name + ": discouraged with relay flags");
                if (leaf.script.size() == 34 && leaf.script[0] == 0x20 && leaf.script[33] == OP_CHECKSIG) {
                    // <32-byte key> OP_CHECKSIG finds no signature on the empty stack.
                    CheckError(consensus, SCRIPT_ERR_INVALID_STACK_OPERATION, leaf_name + " key and OP_CHECKSIG (block flags)");
                    CheckError(policy, SCRIPT_ERR_INVALID_STACK_OPERATION, leaf_name + " key and OP_CHECKSIG (relay flags)");
                }
                ++counts.executed;
            }

            // The lowest bit of the control byte must be set.
            valtype control_parity = control;
            control_parity[0] &= 0xfe;
            CheckSpendBoth(script_pubkey, {leaf.script, control_parity}, SCRIPT_ERR_P2MR_CONTROL_PARITY, leaf_name + " with the lowest bit of the control byte cleared");

            // A change to the path, or at depth zero to the script, breaks the commitment.
            if (leaf.depth > 0) {
                valtype control_changed = control;
                control_changed[1] ^= 0x01;
                CheckSpendBoth(script_pubkey, {leaf.script, control_changed}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, leaf_name + " with a changed path");
            } else {
                valtype script_changed = leaf.script;
                script_changed[0] ^= 0x01;
                CheckSpendBoth(script_pubkey, {script_changed, control}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, leaf_name + " with a changed script");
            }
        }
    }
    return counts;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(p2mr_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(init_bip341_data)
{
    // Signatures in a P2MR leaf (BIP 360) are checked against the BIP 341 signature message, as in a Taproot script
    // path spend. PrecomputedTransactionData::Init must therefore prepare the BIP 341 data when a witness-bearing
    // input spends OP_2 <32 bytes>, also when the transaction has no Taproot input.
    const CTxOut p2mr(50000, ProgramScript(OP_2, 32, 0xaa));
    const CTxOut taproot(60000, ProgramScript(OP_1, 32, 0xbb));
    const CTxOut p2wpkh(70000, ProgramScript(OP_0, 20, 0xcc));
    const CTxOut p2pkh(80000, CScript() << OP_DUP << OP_HASH160 << valtype(20, 0xdd) << OP_EQUALVERIFY << OP_CHECKSIG);

    // The only witness-bearing input spends a P2MR output: the BIP 341 data is prepared, and the BIP 143 data,
    // which only witness version 0 signatures read, is not.
    CheckInit(InitFor({p2mr}, {true}), true, false, true, "P2MR input");
    CheckInit(InitFor({p2pkh, p2mr}, {false, true}), true, false, true, "P2PKH and P2MR inputs");
    // With a witness version 0 input in the same transaction, both sets of data are prepared, in either order.
    CheckInit(InitFor({p2mr, p2wpkh}, {true, true}), true, true, true, "P2MR and P2WPKH inputs");
    CheckInit(InitFor({p2wpkh, p2pkh, p2mr}, {true, false, true}), true, true, true, "P2WPKH, P2PKH and P2MR inputs");

    // The prepared data is the BIP 341 data of this transaction and of the outputs it spends.
    {
        const std::vector<CTxOut> spent_outputs{p2pkh, p2mr};
        const CMutableTransaction tx = SpendingTransaction(spent_outputs, {false, true});
        PrecomputedTransactionData txdata;
        txdata.Init(tx, std::vector<CTxOut>(spent_outputs));
        BOOST_REQUIRE(txdata.m_bip341_taproot_ready);
        CHashWriter prevouts(SER_GETHASH, 0), sequences(SER_GETHASH, 0), outputs(SER_GETHASH, 0), amounts(SER_GETHASH, 0), scripts(SER_GETHASH, 0);
        for (const CTxIn& txin : tx.vin) {
            prevouts << txin.prevout;
            sequences << txin.nSequence;
        }
        for (const CTxOut& txout : tx.vout) outputs << txout;
        for (const CTxOut& txout : spent_outputs) {
            amounts << txout.nValue;
            scripts << txout.scriptPubKey;
        }
        BOOST_CHECK(txdata.m_prevouts_single_hash == prevouts.GetSHA256());
        BOOST_CHECK(txdata.m_sequences_single_hash == sequences.GetSHA256());
        BOOST_CHECK(txdata.m_outputs_single_hash == outputs.GetSHA256());
        BOOST_CHECK(txdata.m_spent_amounts_single_hash == amounts.GetSHA256());
        BOOST_CHECK(txdata.m_spent_scripts_single_hash == scripts.GetSHA256());
        BOOST_CHECK(txdata.m_spent_outputs == spent_outputs);
    }

    // What does not change. A Taproot input prepares the BIP 341 data.
    CheckInit(InitFor({taproot}, {true}), true, false, true, "Taproot input");
    CheckInit(InitFor({taproot, p2mr, p2wpkh}, {true, true, true}), true, true, true, "Taproot, P2MR and P2WPKH inputs");
    // Without the spent outputs the BIP 341 data cannot be prepared.
    CheckInit(InitFor({p2mr}, {true}, /* give_spent_outputs */ false), false, true, false, "P2MR input, spent outputs not given");
    CheckInit(InitFor({taproot}, {true}, /* give_spent_outputs */ false), false, true, false, "Taproot input, spent outputs not given");
    // An input without a witness prepares nothing.
    CheckInit(InitFor({p2mr}, {false}), true, false, false, "P2MR input without witness");
    CheckInit(InitFor({p2pkh}, {false}), true, false, false, "P2PKH input");
    // Other witness versions and program sizes are treated as before.
    CheckInit(InitFor({p2wpkh}, {true}), true, true, false, "P2WPKH input");
    CheckInit(InitFor({CTxOut(1, ProgramScript(OP_2, 31, 0xaa))}, {true}), true, true, false, "witness version 2, 31-byte program");
    CheckInit(InitFor({CTxOut(1, ProgramScript(OP_2, 33, 0xaa))}, {true}), true, true, false, "witness version 2, 33-byte program");
    CheckInit(InitFor({CTxOut(1, ProgramScript(OP_3, 32, 0xaa))}, {true}), true, true, false, "witness version 3, 32-byte program");
    CheckInit(InitFor({CTxOut(1, ProgramScript(OP_16, 32, 0xaa))}, {true}), true, true, false, "witness version 16, 32-byte program");
}

BOOST_AUTO_TEST_CASE(checksig_without_taproot_input)
{
    // A block's script checks verify a transaction whose witness-bearing inputs are a P2MR input that runs
    // OP_CHECKSIG and a P2WPKH input: the BIP 341 signature message needs data that the transaction data must
    // hold although no input spends a Taproot output.
    const SpendVector vector = LoadSpendVector();
    BOOST_REQUIRE(vector.spent_outputs[0].scriptPubKey == vector.script_pubkey);
    int version;
    valtype program;
    BOOST_REQUIRE(vector.spent_outputs[0].scriptPubKey.IsWitnessProgram(version, program) && version == 2 && program.size() == 32);
    BOOST_REQUIRE(vector.spent_outputs[1].scriptPubKey.IsWitnessProgram(version, program) && version == 0 && program.size() == 20);
    BOOST_REQUIRE(vector.signed_tx.vin[0].scriptWitness.stack.size() == 3 && vector.signed_tx.vin[1].scriptWitness.stack.size() == 2);
    BOOST_REQUIRE(vector.signed_tx.vin[0].scriptWitness.stack[1].back() == OP_CHECKSIG);

    CCoinsView no_coins;
    CCoinsViewCache view(&no_coins);
    for (size_t i = 0; i < vector.signed_tx.vin.size(); ++i) {
        view.AddCoin(vector.signed_tx.vin[i].prevout, Coin(vector.spent_outputs[i], /* nHeight */ 1, /* fCoinBase */ false, /* fPegout */ false), /* possible_overwrite */ false);
    }

    LOCK(cs_main);
    {
        const CTransaction tx(vector.signed_tx);
        // Checked inline.
        TxValidationState state;
        PrecomputedTransactionData txdata;
        BOOST_CHECK_MESSAGE(CheckInputScripts(tx, state, view, CONSENSUS_FLAGS, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr), state.ToString());
        BOOST_CHECK(txdata.m_spent_outputs_ready && txdata.m_bip341_taproot_ready && txdata.m_bip143_segwit_ready);
        // Collected first and run afterwards, as the script check threads do.
        TxValidationState state_queued;
        PrecomputedTransactionData txdata_queued;
        std::vector<CScriptCheck> checks;
        BOOST_CHECK(CheckInputScripts(tx, state_queued, view, CONSENSUS_FLAGS, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata_queued, &checks));
        BOOST_CHECK_EQUAL(checks.size(), 2U);
        for (CScriptCheck& check : checks) {
            BOOST_CHECK_MESSAGE(check(), ScriptErrorString(check.GetScriptError()));
        }
        // The same transaction passes with the flags of relay.
        TxValidationState state_policy;
        PrecomputedTransactionData txdata_policy;
        BOOST_CHECK_MESSAGE(CheckInputScripts(tx, state_policy, view, POLICY_FLAGS, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata_policy, nullptr), state_policy.ToString());
    }
    {
        // The signature is really checked: with one bit of it changed the transaction is refused. The reason
        // names the flag as not mandatory because the check passes again with the P2SH flag alone; a block
        // with such a transaction is invalid all the same.
        CMutableTransaction changed = vector.signed_tx;
        changed.vin[0].scriptWitness.stack[0][0] ^= 0x01;
        const CTransaction tx(changed);
        TxValidationState state;
        PrecomputedTransactionData txdata;
        BOOST_CHECK(!CheckInputScripts(tx, state, view, CONSENSUS_FLAGS, /* cacheSigStore */ false, /* cacheFullScriptStore */ false, txdata, nullptr));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "non-mandatory-script-verify-flag (Invalid Schnorr signature)");
    }
}

BOOST_AUTO_TEST_CASE(verification_steps)
{
    const valtype op_true = ScriptBytes(CScript() << OP_TRUE);
    const valtype op_false = ScriptBytes(CScript() << OP_FALSE);
    const valtype op_return = ScriptBytes(CScript() << OP_RETURN);
    const std::vector<uint256> one_sibling = Siblings(1);
    const uint256 true_hash = LeafHash(LEAF_TAPSCRIPT, op_true);
    // A tree in which OP_TRUE is a leaf at depth 1, and its control block.
    const CScript tree_true = P2MRScript(RootFromPath(true_hash, one_sibling));
    const valtype control_true = ControlBlock(LEAF_TAPSCRIPT, one_sibling);
    CheckSpendBoth(tree_true, {op_true, control_true}, SCRIPT_ERR_OK, "OP_TRUE leaf at depth 1");

    // The witness needs a script and a control block: at least two elements.
    CheckSpendBoth(tree_true, {}, SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY, "empty witness");
    CheckSpendBoth(tree_true, {op_true}, SCRIPT_ERR_P2MR_WITNESS_SIZE, "witness with the script alone");
    CheckSpendBoth(tree_true, {control_true}, SCRIPT_ERR_P2MR_WITNESS_SIZE, "witness with the control block alone");
    CheckSpendBoth(tree_true, {valtype{0x50}}, SCRIPT_ERR_P2MR_WITNESS_SIZE, "witness with one element that starts with 0x50");
    CheckSpendBoth(tree_true, {valtype{}}, SCRIPT_ERR_P2MR_WITNESS_SIZE, "witness with one empty element");
    // With exactly two elements, a last element that starts with 0x50 is not an annex, and it is not accepted as
    // a control block either.
    CheckSpendBoth(tree_true, {op_true, valtype{0x50}}, SCRIPT_ERR_P2MR_WITNESS_SIZE, "two elements, the last one 0x50");
    {
        valtype control = control_true;
        control[0] = 0x50;
        CheckSpendBoth(tree_true, {op_true, control}, SCRIPT_ERR_P2MR_WITNESS_SIZE, "two elements, the last one 33 bytes starting with 0x50");
        // With three elements the same last element is an annex, and the two before it are script and control block.
        CheckSpendBoth(tree_true, {op_true, control_true, control}, SCRIPT_ERR_OK, "three elements, the last one 33 bytes starting with 0x50");
    }

    // An annex is the last of three or more elements, if it starts with 0x50. It is removed: the elements before
    // it are the control block, the script and the initial stack.
    CheckSpendBoth(tree_true, {op_true, control_true, valtype{0x50}}, SCRIPT_ERR_OK, "one-byte annex");
    CheckSpendBoth(tree_true, {op_true, control_true, valtype{0x50, 0x01, 0x02}}, SCRIPT_ERR_OK, "three-byte annex");
    // A last element that does not start with 0x50 is no annex: it is the control block, and the element before it
    // the script.
    CheckSpendBoth(tree_true, {op_true, control_true, valtype{0x51}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "three elements, the last one 0x51");
    CheckSpendBoth(tree_true, {op_true, control_true, valtype{}}, SCRIPT_ERR_P2MR_WRONG_CONTROL_SIZE, "three elements, the last one empty");
    {
        // The script compares the one element of the initial stack with 7.
        const valtype seven_equal = ScriptBytes(CScript() << OP_7 << OP_EQUAL);
        const CScript tree = P2MRScript(RootFromPath(LeafHash(LEAF_TAPSCRIPT, seven_equal), one_sibling));
        CheckSpendBoth(tree, {valtype{0x07}, seven_equal, control_true}, SCRIPT_ERR_OK, "initial stack of one element");
        CheckSpendBoth(tree, {valtype{0x07}, seven_equal, control_true, valtype{0x50, 0xff}}, SCRIPT_ERR_OK, "initial stack of one element, with annex");
        CheckSpendBoth(tree, {valtype{0x08}, seven_equal, control_true, valtype{0x50, 0xff}}, SCRIPT_ERR_EVAL_FALSE, "initial stack with the wrong element, with annex");
        CheckSpendBoth(tree, {seven_equal, control_true, valtype{0x50, 0xff}}, SCRIPT_ERR_INVALID_STACK_OPERATION, "empty initial stack, with annex");
        CheckSpendBoth(tree, {valtype{0x07}, valtype{0x07}, seven_equal, control_true}, SCRIPT_ERR_CLEANSTACK, "initial stack of two elements");
    }
    {
        // The annex is covered by the signature: a signature made for a witness with the annex is valid only
        // with it, and one made for a witness without annex only without it.
        const SpendVector vector = LoadSpendVector();
        const SpendVector::Spending& plain = vector.spendings[0];
        const SpendVector::Spending& annexed = vector.spendings[1];
        BOOST_REQUIRE(plain.witness.size() == 3 && annexed.witness.size() == 4);
        BOOST_REQUIRE(annexed.witness[3] == annexed.annex && annexed.annex[0] == 0x50);
        BOOST_REQUIRE(plain.signature != annexed.signature);
        CMutableTransaction tx = vector.unsigned_tx;
        for (const unsigned int flags : {CONSENSUS_FLAGS, POLICY_FLAGS}) {
            tx.vin[0].scriptWitness.stack = plain.witness;
            CheckError(VerifyInput(tx, vector.spent_outputs, 0, flags), SCRIPT_ERR_OK, "signature without annex, witness without annex");
            tx.vin[0].scriptWitness.stack = annexed.witness;
            CheckError(VerifyInput(tx, vector.spent_outputs, 0, flags), SCRIPT_ERR_OK, "signature with annex, witness with annex");
            tx.vin[0].scriptWitness.stack = {plain.signature, annexed.witness[1], annexed.witness[2], annexed.annex};
            CheckError(VerifyInput(tx, vector.spent_outputs, 0, flags), SCRIPT_ERR_SCHNORR_SIG, "signature without annex, witness with annex");
            tx.vin[0].scriptWitness.stack = {annexed.signature, annexed.witness[1], annexed.witness[2]};
            CheckError(VerifyInput(tx, vector.spent_outputs, 0, flags), SCRIPT_ERR_SCHNORR_SIG, "signature with annex, witness without annex");
            valtype other_annex = annexed.annex;
            other_annex.back() ^= 0x01;
            tx.vin[0].scriptWitness.stack = {annexed.signature, annexed.witness[1], annexed.witness[2], other_annex};
            CheckError(VerifyInput(tx, vector.spent_outputs, 0, flags), SCRIPT_ERR_SCHNORR_SIG, "signature with annex, witness with another annex");
        }
    }

    // The control block has 1 + 32 * m bytes, for m from 0 to 128.
    {
        const std::vector<uint256> long_path = Siblings(129);
        const valtype long_control = ControlBlock(LEAF_TAPSCRIPT, long_path);
        BOOST_REQUIRE_EQUAL(long_control.size(), 4129U);
        for (const size_t m : {1, 2, 3, 127, 128}) {
            const std::vector<uint256> path(long_path.begin(), long_path.begin() + m);
            const valtype control = ControlBlock(LEAF_TAPSCRIPT, path);
            BOOST_REQUIRE_EQUAL(control.size(), 1 + 32 * m);
            CheckSpendBoth(P2MRScript(RootFromPath(true_hash, path)), {op_true, control}, SCRIPT_ERR_OK, strprintf("control block of %u bytes", control.size()));
        }
        for (const size_t size : {0, 2, 32, 34, 64, 66, 4096, 4098, 4128, 4129, 4130, 4161}) {
            // The first bytes of a long control block (or all of it and more), spending the output that commits
            // to the tree of the path they hold, so that the size alone is wrong.
            valtype control(long_control.begin(), long_control.begin() + std::min(size, long_control.size()));
            control.resize(size, 0x00);
            const size_t nodes = std::min<size_t>(size > 0 ? (size - 1) / 32 : 0, long_path.size());
            const std::vector<uint256> path(long_path.begin(), long_path.begin() + nodes);
            CheckSpendBoth(P2MRScript(RootFromPath(true_hash, path)), {op_true, control}, SCRIPT_ERR_P2MR_WRONG_CONTROL_SIZE, strprintf("control block of %u bytes", size));
        }
        BOOST_CHECK_EQUAL(P2MR_CONTROL_MAX_SIZE, 4097U);
    }

    // The lowest bit of the control byte is 1, at every depth.
    {
        valtype control = control_true;
        control[0] = LEAF_TAPSCRIPT;
        CheckSpendBoth(tree_true, {op_true, control}, SCRIPT_ERR_P2MR_CONTROL_PARITY, "depth 1, lowest bit of the control byte 0");
        CheckSpendBoth(P2MRScript(true_hash), {op_true, valtype{LEAF_TAPSCRIPT}}, SCRIPT_ERR_P2MR_CONTROL_PARITY, "depth 0, lowest bit of the control byte 0");
        CheckSpendBoth(P2MRScript(true_hash), {op_true, valtype{LEAF_TAPSCRIPT | 1}}, SCRIPT_ERR_OK, "depth 0, lowest bit of the control byte 1");
        // The size is checked before the bit, and the bit before the commitment.
        CheckSpendBoth(tree_true, {op_true, valtype{LEAF_TAPSCRIPT, 0x00}}, SCRIPT_ERR_P2MR_WRONG_CONTROL_SIZE, "wrong size and lowest bit 0");
        CheckSpendBoth(tree_true, {op_false, control}, SCRIPT_ERR_P2MR_CONTROL_PARITY, "lowest bit 0 and another script");
    }

    // The root computed from the script, the leaf version and the path must be the program.
    {
        CheckSpendBoth(tree_true, {op_false, control_true}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "another script");
        CheckSpendBoth(tree_true, {op_true, ControlBlock(LEAF_TAPSCRIPT, Siblings(2))}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "a longer path");
        CheckSpendBoth(tree_true, {op_true, ControlBlock(LEAF_TAPSCRIPT, {})}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "no path");
        CheckSpendBoth(tree_true, {op_true, ControlBlock(0xc2, one_sibling)}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "another leaf version");
        valtype control = control_true;
        control[32] ^= 0x80;
        CheckSpendBoth(tree_true, {op_true, control}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "one bit of the path changed");
        uint256 other_root = RootFromPath(true_hash, one_sibling);
        *other_root.begin() ^= 0x01;
        CheckSpendBoth(P2MRScript(other_root), {op_true, control_true}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "one bit of the program changed");

        // Two nodes are hashed with the lexicographically smaller one first, whichever of them the path holds.
        const uint256 low = uint256S("0000000000000000000000000000000000000000000000000000000000000000");
        const uint256 high = uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        for (const uint256& sibling : {low, high}) {
            const bool sibling_first = sibling == low;
            valtype sorted, reversed;
            AppendBytes(sorted, sibling_first ? sibling : true_hash);
            AppendBytes(sorted, sibling_first ? true_hash : sibling);
            AppendBytes(reversed, sibling_first ? true_hash : sibling);
            AppendBytes(reversed, sibling_first ? sibling : true_hash);
            const std::string name = sibling_first ? "sibling below the leaf hash" : "sibling above the leaf hash";
            CheckSpendBoth(P2MRScript(TaggedSha256("TapBranch", sorted)), {op_true, ControlBlock(LEAF_TAPSCRIPT, {sibling})}, SCRIPT_ERR_OK, name);
            CheckSpendBoth(P2MRScript(TaggedSha256("TapBranch", reversed)), {op_true, ControlBlock(LEAF_TAPSCRIPT, {sibling})}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, name + ", hashed in the other order");
        }
    }

    // A leaf at depth zero is not run once it commits to the program; at depth 1 or more a leaf with leaf version
    // 0xc0 is run as a tapscript.
    {
        const CScript depth0_return = P2MRScript(LeafHash(LEAF_TAPSCRIPT, op_return));
        const CScript depth1_return = P2MRScript(RootFromPath(LeafHash(LEAF_TAPSCRIPT, op_return), one_sibling));
        CheckSpendBoth(depth0_return, {op_return, valtype{0xc1}}, SCRIPT_ERR_OK, "OP_RETURN leaf at depth 0");
        CheckSpendBoth(depth0_return, {valtype{0x01}, valtype(600, 0x02), op_return, valtype{0xc1}}, SCRIPT_ERR_OK, "OP_RETURN leaf at depth 0 with an initial stack");
        CheckSpendBoth(depth0_return, {op_return, valtype{0xc1}, valtype{0x50}}, SCRIPT_ERR_OK, "OP_RETURN leaf at depth 0 with an annex");
        CheckSpendBoth(depth1_return, {op_return, control_true}, SCRIPT_ERR_OP_RETURN, "OP_RETURN leaf at depth 1");
        const CScript depth1_false = P2MRScript(RootFromPath(LeafHash(LEAF_TAPSCRIPT, op_false), one_sibling));
        CheckSpendBoth(depth1_false, {op_false, control_true}, SCRIPT_ERR_EVAL_FALSE, "OP_FALSE leaf at depth 1");
        CheckSpendBoth(P2MRScript(LeafHash(LEAF_TAPSCRIPT, op_false)), {op_false, valtype{0xc1}}, SCRIPT_ERR_OK, "OP_FALSE leaf at depth 0");
        // The limits of tapscript apply to the initial stack of a leaf that is run.
        CheckSpendBoth(tree_true, {valtype(521, 0x02), op_true, control_true}, SCRIPT_ERR_PUSH_SIZE, "initial stack element of 521 bytes at depth 1");
    }

    // Other leaf versions are not run. They are valid in a block, and not relayed, at every depth. The commitment
    // is checked first.
    for (const uint8_t leaf_version : {0xc2, 0x50, 0x00, 0x66, 0xfe}) {
        const std::string name = strprintf("leaf version 0x%02x", leaf_version);
        const uint256 leaf_hash = LeafHash(leaf_version, op_return);
        const valtype control0 = ControlBlock(leaf_version, {});
        const valtype control1 = ControlBlock(leaf_version, one_sibling);
        const CScript depth0 = P2MRScript(leaf_hash);
        const CScript depth1 = P2MRScript(RootFromPath(leaf_hash, one_sibling));
        CheckSpendBoth(depth1, {op_return, control1}, SCRIPT_ERR_OK, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION, name + " at depth 1");
        CheckSpendBoth(depth0, {op_return, control0}, SCRIPT_ERR_OK, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION, name + " at depth 0");
        CheckSpendBoth(depth0, {valtype(600, 0x02), op_return, control0}, SCRIPT_ERR_OK, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION, name + " at depth 0 with an initial stack");
        CheckSpendBoth(depth1, {valtype(600, 0x02), op_return, control1}, SCRIPT_ERR_OK, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_TAPROOT_VERSION, name + " at depth 1 with an initial stack");
        CheckSpendBoth(depth0, {op_true, control0}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, name + " at depth 0, another script");
        CheckSpendBoth(depth1, {op_true, control1}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, name + " at depth 1, another script");
        CheckSpendBoth(depth1, {op_return, control0}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, name + ", path left out");
        valtype control_parity = control0;
        control_parity[0] &= 0xfe;
        if (control_parity[0] != 0x50) {
            CheckSpendBoth(depth0, {op_return, control_parity}, SCRIPT_ERR_P2MR_CONTROL_PARITY, name + " at depth 0, lowest bit 0");
        } else {
            // 0x50 as the last of two elements is refused before the control block is looked at.
            CheckSpendBoth(depth0, {op_return, control_parity}, SCRIPT_ERR_P2MR_WITNESS_SIZE, name + " at depth 0, lowest bit 0");
        }
    }

    // OP_SUCCESSx opcodes: at depth 1 or more they make a tapscript succeed in a block and are not relayed. At
    // depth zero the script is not looked at.
    {
        const valtype success = ScriptBytes(CScript() << OP_RETURN << OP_SUBSTR);
        const uint256 leaf_hash = LeafHash(LEAF_TAPSCRIPT, success);
        CheckSpendBoth(P2MRScript(RootFromPath(leaf_hash, one_sibling)), {success, control_true}, SCRIPT_ERR_OK, SCRIPT_ERR_DISCOURAGE_OP_SUCCESS, "OP_SUCCESSx at depth 1");
        CheckSpendBoth(P2MRScript(leaf_hash), {success, valtype{0xc1}}, SCRIPT_ERR_OK, "OP_SUCCESSx at depth 0");
        // A script that ends inside a push.
        const valtype truncated{0x4c};
        const uint256 truncated_hash = LeafHash(LEAF_TAPSCRIPT, truncated);
        CheckSpendBoth(P2MRScript(RootFromPath(truncated_hash, one_sibling)), {truncated, control_true}, SCRIPT_ERR_BAD_OPCODE, "script ending inside a push at depth 1");
        CheckSpendBoth(P2MRScript(truncated_hash), {truncated, valtype{0xc1}}, SCRIPT_ERR_OK, "script ending inside a push at depth 0");
    }

    // The validation weight of a tapscript is the serialized size of the whole witness plus 50, and every
    // signature check with a non-empty signature takes 50 of it. The key here has an unknown type (2 bytes), for
    // which a non-empty signature counts as valid in a block.
    {
        const valtype key{0x02, 0x03};
        const valtype two_checks = ScriptBytes(CScript() << OP_DUP << key << OP_CHECKSIGVERIFY << key << OP_CHECKSIG);
        BOOST_REQUIRE_EQUAL(two_checks.size(), 9U);
        const CScript tree = P2MRScript(RootFromPath(LeafHash(LEAF_TAPSCRIPT, two_checks), one_sibling));
        // Witness: count (1), signature (1 + size), script (1 + 9), control block (1 + 33): 46 bytes and the
        // signature. Two checks need a weight of 100.
        CheckSpend(tree, {valtype(4, 0x01), two_checks, control_true}, CONSENSUS_FLAGS, SCRIPT_ERR_OK, "validation weight 100 for two signature checks");
        CheckSpend(tree, {valtype(3, 0x01), two_checks, control_true}, CONSENSUS_FLAGS, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT, "validation weight 99 for two signature checks");
        CheckSpend(tree, {valtype(1, 0x01), two_checks, control_true}, CONSENSUS_FLAGS, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT, "validation weight 97 for two signature checks");
        // The annex counts: one byte of annex adds two bytes to the witness.
        CheckSpend(tree, {valtype(3, 0x01), two_checks, control_true, valtype{0x50}}, CONSENSUS_FLAGS, SCRIPT_ERR_OK, "validation weight 101 with an annex");
        CheckSpend(tree, {valtype(1, 0x01), two_checks, control_true, valtype{0x50}}, CONSENSUS_FLAGS, SCRIPT_ERR_TAPSCRIPT_VALIDATION_WEIGHT, "validation weight 99 with an annex");
        // An empty signature takes no weight, and the check gives false.
        CheckSpend(tree, {valtype{}, two_checks, control_true}, CONSENSUS_FLAGS, SCRIPT_ERR_CHECKSIGVERIFY, "empty signature");
        // Keys of an unknown type are not relayed.
        CheckSpend(tree, {valtype(4, 0x01), two_checks, control_true}, POLICY_FLAGS, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_PUBKEYTYPE, "key of an unknown type with relay flags");
    }
}

BOOST_AUTO_TEST_CASE(depth_zero_needs_root)
{
    // A spend with a control block of one byte succeeds without running the script only if the hash of the
    // revealed script, with the leaf version of the control byte, is the witness program.
    const valtype op_true = ScriptBytes(CScript() << OP_TRUE);
    const valtype op_false = ScriptBytes(CScript() << OP_FALSE);
    const CScript commits_to_true = P2MRScript(LeafHash(LEAF_TAPSCRIPT, op_true));
    CheckSpendBoth(commits_to_true, {op_true, valtype{0xc1}}, SCRIPT_ERR_OK, "the committed script");
    CheckSpendBoth(commits_to_true, {op_false, valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "another script");
    CheckSpendBoth(commits_to_true, {valtype{}, valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "an empty script");
    CheckSpendBoth(commits_to_true, {op_true, op_true, valtype{0xc1}}, SCRIPT_ERR_OK, "the committed script with an initial stack");
    CheckSpendBoth(commits_to_true, {op_true, op_false, valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "another script with the committed script below it");
    // The leaf version is part of the commitment.
    CheckSpendBoth(commits_to_true, {op_true, valtype{0xc3}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "the committed script with another leaf version");
    CheckSpendBoth(P2MRScript(LeafHash(0xc2, op_true)), {op_true, valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "a script committed with another leaf version");
    // Any program that is not the leaf hash fails, whatever the script: the plain and the double SHA256 of the
    // script, the root of a tree that holds the leaf deeper, and fixed values.
    const std::vector<uint256> one_sibling = Siblings(1);
    for (const uint256& program : {Sha256(op_true), Sha256(ToByteVector(Sha256(op_true))), RootFromPath(LeafHash(LEAF_TAPSCRIPT, op_true), one_sibling), uint256S("01"), one_sibling[0]}) {
        CheckSpendBoth(P2MRScript(program), {op_true, valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "program " + HexStr(program));
        CheckSpendBoth(P2MRScript(program), {valtype{}, valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "program " + HexStr(program) + ", empty script");
        CheckSpendBoth(P2MRScript(program), {ToByteVector(program), valtype{0xc1}}, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "program " + HexStr(program) + ", the program as script");
    }
    // A program of 32 zero bytes cannot be spent at all, with or without the P2MR rules: like every witness program
    // that is false as a stack element, it fails before the witness is looked at.
    CheckSpendBoth(P2MRScript(uint256()), {op_true, valtype{0xc1}}, SCRIPT_ERR_EVAL_FALSE, "program of zero bytes");
    CheckSpend(P2MRScript(uint256()), {op_true, valtype{0xc1}}, CONSENSUS_FLAGS & ~SCRIPT_VERIFY_P2MR, SCRIPT_ERR_EVAL_FALSE, "program of zero bytes, block flags without P2MR");
}

BOOST_AUTO_TEST_CASE(flags_and_forms)
{
    const valtype op_true = ScriptBytes(CScript() << OP_TRUE);
    const std::vector<uint256> one_sibling = Siblings(1);
    const uint256 root = RootFromPath(LeafHash(LEAF_TAPSCRIPT, op_true), one_sibling);
    const CScript p2mr = P2MRScript(root);
    const std::vector<valtype> valid_witness{op_true, ControlBlock(LEAF_TAPSCRIPT, one_sibling)};
    // Witnesses that break the rules, each with the rule it breaks first.
    const std::vector<std::vector<valtype>> invalid_witnesses{
        {},
        {valtype{0x01}},
        {valtype{0x01}, valtype{0x02}},
        {valtype{0x01}, valtype{0x02}, valtype{0x03}},
        {op_true, ControlBlock(LEAF_TAPSCRIPT, Siblings(2))},
    };
    const std::vector<ScriptError> invalid_witness_errors{
        SCRIPT_ERR_WITNESS_PROGRAM_WITNESS_EMPTY,
        SCRIPT_ERR_P2MR_WITNESS_SIZE,
        SCRIPT_ERR_P2MR_CONTROL_PARITY,
        SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH,
        SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH,
    };
    BOOST_REQUIRE_EQUAL(invalid_witnesses.size(), invalid_witness_errors.size());
    const unsigned int consensus_without = CONSENSUS_FLAGS & ~SCRIPT_VERIFY_P2MR;
    const unsigned int policy_without = POLICY_FLAGS & ~SCRIPT_VERIFY_P2MR;
    BOOST_REQUIRE(POLICY_FLAGS & SCRIPT_VERIFY_P2MR);
    BOOST_REQUIRE(policy_without & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM);
    BOOST_REQUIRE(!(CONSENSUS_FLAGS & SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM));

    // Without the P2MR flag every spend of OP_2 <32 bytes> succeeds, whatever the witness and the other flags.
    // With it, only a witness that follows the rules does.
    CheckSpend(p2mr, valid_witness, CONSENSUS_FLAGS, SCRIPT_ERR_OK, "valid witness, block flags");
    CheckSpend(p2mr, valid_witness, POLICY_FLAGS, SCRIPT_ERR_OK, "valid witness, relay flags");
    CheckSpend(p2mr, valid_witness, consensus_without, SCRIPT_ERR_OK, "valid witness, block flags without P2MR");
    CheckSpend(p2mr, valid_witness, policy_without, SCRIPT_ERR_OK, "valid witness, relay flags without P2MR");
    for (size_t i = 0; i < invalid_witnesses.size(); ++i) {
        const std::string name = strprintf("invalid witness %u", i);
        CheckSpend(p2mr, invalid_witnesses[i], consensus_without, SCRIPT_ERR_OK, name + ", block flags without P2MR");
        CheckSpend(p2mr, invalid_witnesses[i], policy_without, SCRIPT_ERR_OK, name + ", relay flags without P2MR");
        CheckSpend(p2mr, invalid_witnesses[i], SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM, SCRIPT_ERR_OK, name + ", witness and discourage flags");
        CheckSpend(p2mr, invalid_witnesses[i], SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_TAPROOT, SCRIPT_ERR_OK, name + ", witness and Taproot flags");
        CheckSpend(p2mr, invalid_witnesses[i], CONSENSUS_FLAGS, invalid_witness_errors[i], name + ", block flags");
        CheckSpend(p2mr, invalid_witnesses[i], POLICY_FLAGS, invalid_witness_errors[i], name + ", relay flags");
        CheckSpend(p2mr, invalid_witnesses[i], SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2MR, invalid_witness_errors[i], name + ", witness and P2MR flags");
        // The flag acts on witness programs, which are verified only with the witness flag.
        CheckSpend(p2mr, invalid_witnesses[i], SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_P2MR, SCRIPT_ERR_OK, name + ", P2MR flag without the witness flag");
    }
    // The rules do not depend on the Taproot flag: a tapscript leaf is run without it.
    CheckSpend(p2mr, valid_witness, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2MR, SCRIPT_ERR_OK, "valid witness, witness and P2MR flags");
    {
        const valtype op_false = ScriptBytes(CScript() << OP_FALSE);
        const CScript tree_false = P2MRScript(RootFromPath(LeafHash(LEAF_TAPSCRIPT, op_false), one_sibling));
        CheckSpend(tree_false, {op_false, ControlBlock(LEAF_TAPSCRIPT, one_sibling)}, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2MR, SCRIPT_ERR_EVAL_FALSE, "OP_FALSE leaf, witness and P2MR flags");
    }
    // A scriptSig is not allowed for a native witness program, with or without the flag.
    CheckError(VerifySpend(p2mr, valid_witness, CONSENSUS_FLAGS, CScript() << OP_TRUE), SCRIPT_ERR_WITNESS_MALLEATED, "scriptSig with block flags");
    CheckError(VerifySpend(p2mr, valid_witness, consensus_without, CScript() << OP_TRUE), SCRIPT_ERR_WITNESS_MALLEATED, "scriptSig with block flags without P2MR");

    // The flag does not change the spends of any other output. Other witness programs of version 2 and programs
    // of 32 bytes with a later version stay as they were: valid in a block, not relayed.
    std::vector<CScript> other_programs;
    for (const size_t size : {2, 20, 31, 33, 40}) other_programs.push_back(ProgramScript(OP_2, size, 0x5a));
    other_programs.push_back(CScript() << OP_3 << ToByteVector(root));
    other_programs.push_back(CScript() << OP_16 << ToByteVector(root));
    std::vector<std::vector<valtype>> witnesses = invalid_witnesses;
    witnesses.push_back(valid_witness);
    for (const CScript& program : other_programs) {
        for (size_t i = 0; i < witnesses.size(); ++i) {
            const std::string name = strprintf("program %s, witness %u", HexStr(program), i);
            CheckSpend(program, witnesses[i], CONSENSUS_FLAGS, SCRIPT_ERR_OK, name + ", block flags");
            CheckSpend(program, witnesses[i], consensus_without, SCRIPT_ERR_OK, name + ", block flags without P2MR");
            CheckSpend(program, witnesses[i], POLICY_FLAGS, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM, name + ", relay flags");
            CheckSpend(program, witnesses[i], policy_without, SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM, name + ", relay flags without P2MR");
        }
    }
    // OP_2 <32 bytes> as a P2SH redeem script is not P2MR: valid in a block with any witness, not relayed.
    {
        const CScript redeem_script = p2mr;
        const CScript p2sh = GetScriptForDestination(ScriptHash(redeem_script));
        const CScript script_sig = CScript() << ScriptBytes(redeem_script);
        for (size_t i = 0; i < witnesses.size(); ++i) {
            const std::string name = strprintf("P2SH-wrapped, witness %u", i);
            CheckError(VerifySpend(p2sh, witnesses[i], CONSENSUS_FLAGS, script_sig), SCRIPT_ERR_OK, name + ", block flags");
            CheckError(VerifySpend(p2sh, witnesses[i], consensus_without, script_sig), SCRIPT_ERR_OK, name + ", block flags without P2MR");
            CheckError(VerifySpend(p2sh, witnesses[i], POLICY_FLAGS, script_sig), SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM, name + ", relay flags");
            CheckError(VerifySpend(p2sh, witnesses[i], policy_without, script_sig), SCRIPT_ERR_DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM, name + ", relay flags without P2MR");
        }
        // No signature operations are counted for a P2MR spend or for the wrapped form.
        CScriptWitness witness;
        witness.stack = {valtype(64, 0x01), ScriptBytes(CScript() << valtype(32, 0x02) << OP_CHECKSIG), ControlBlock(LEAF_TAPSCRIPT, one_sibling)};
        BOOST_CHECK_EQUAL(CountWitnessSigOps(CScript(), p2mr, &witness, CONSENSUS_FLAGS), 0U);
        BOOST_CHECK_EQUAL(CountWitnessSigOps(CScript(), p2mr, &witness, POLICY_FLAGS), 0U);
        BOOST_CHECK_EQUAL(CountWitnessSigOps(script_sig, p2sh, &witness, CONSENSUS_FLAGS), 0U);
    }
    // A Taproot output is not touched by the P2MR flag.
    {
        const CScript taproot = CScript() << OP_1 << ToByteVector(root);
        CheckSpend(taproot, invalid_witnesses[2], SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_P2MR, SCRIPT_ERR_OK, "Taproot output with the P2MR flag and without the Taproot flag");
        CheckSpend(taproot, valid_witness, CONSENSUS_FLAGS, SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "Taproot output with a P2MR witness");
    }
}

BOOST_AUTO_TEST_CASE(spend_vector)
{
    const SpendVector vector = LoadSpendVector();

    // The tree: leaf 0 is a 32-byte key and OP_CHECKSIG, leaf 1 is OP_TRUE; both at depth 1.
    BOOST_REQUIRE(vector.scripts[0].size() == 34 && vector.scripts[0][0] == 0x20 && vector.scripts[0][33] == OP_CHECKSIG);
    BOOST_CHECK(vector.scripts[1] == ScriptBytes(CScript() << OP_TRUE));
    const uint256 leaf0 = LeafHash(LEAF_TAPSCRIPT, vector.scripts[0]);
    const uint256 leaf1 = LeafHash(LEAF_TAPSCRIPT, vector.scripts[1]);
    BOOST_CHECK_EQUAL(HexStr(leaf0), vector.leaf_hashes[0]);
    BOOST_CHECK_EQUAL(HexStr(leaf1), vector.leaf_hashes[1]);
    const uint256 root = BranchHash(leaf0, leaf1);
    BOOST_CHECK_EQUAL(HexStr(root), vector.merkle_root);
    BOOST_CHECK(vector.script_pubkey == P2MRScript(root));
    BOOST_CHECK(vector.control_blocks[0] == ControlBlock(LEAF_TAPSCRIPT, {leaf1}));
    BOOST_CHECK(vector.control_blocks[1] == ControlBlock(LEAF_TAPSCRIPT, {leaf0}));

    // The transaction: input 0 spends the P2MR output, and no input spends a Taproot output.
    BOOST_CHECK(vector.spent_outputs[0].scriptPubKey == vector.script_pubkey);
    for (const CTxOut& spent : vector.spent_outputs) {
        BOOST_CHECK(!(spent.scriptPubKey.size() == 34 && spent.scriptPubKey[0] == OP_1));
    }
    BOOST_CHECK(CTransaction(vector.signed_tx).GetHash() == CTransaction(vector.unsigned_tx).GetHash());
    BOOST_CHECK(vector.signed_tx.vin[0].scriptWitness.stack == vector.spendings[0].witness);

    const XOnlyPubKey key{Span<const unsigned char>(vector.scripts[0].data() + 1, 32)};
    for (const SpendVector::Spending& spending : vector.spendings) {
        const std::string name = spending.has_annex ? "with annex" : "without annex";
        // The signature message and its hash.
        const valtype msg = TapscriptSigMsg(vector.unsigned_tx, vector.spent_outputs, spending.has_annex ? &spending.annex : nullptr, leaf0);
        BOOST_CHECK_MESSAGE(HexStr(msg) == spending.sig_msg, name + ": signature message");
        const uint256 sighash = TaggedSha256("TapSighash", msg);
        BOOST_CHECK_MESSAGE(HexStr(sighash) == spending.sig_hash, name + ": signature hash");
        // The signature and the witness.
        BOOST_CHECK_EQUAL(spending.signature.size(), 64U);
        BOOST_CHECK_MESSAGE(key.VerifySchnorr(sighash, spending.signature), name + ": signature does not verify");
        std::vector<valtype> witness{spending.signature, vector.scripts[0], vector.control_blocks[0]};
        if (spending.has_annex) witness.push_back(spending.annex);
        BOOST_CHECK_MESSAGE(witness == spending.witness, name + ": witness");

        // The spend is valid in a block and for relay.
        CMutableTransaction tx = vector.unsigned_tx;
        tx.vin[0].scriptWitness.stack = spending.witness;
        CheckError(VerifyInput(tx, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_OK, name + " (block flags)");
        CheckError(VerifyInput(tx, vector.spent_outputs, 0, POLICY_FLAGS), SCRIPT_ERR_OK, name + " (relay flags)");
        // A changed signature fails, and so does the signature when the transaction or a spent output changes.
        for (const size_t pos : {0, 31, 32, 63}) {
            CMutableTransaction changed = tx;
            changed.vin[0].scriptWitness.stack[0][pos] ^= 0x01;
            CheckError(VerifyInput(changed, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, strprintf("%s, signature byte %u changed (block flags)", name, pos));
            CheckError(VerifyInput(changed, vector.spent_outputs, 0, POLICY_FLAGS), SCRIPT_ERR_SCHNORR_SIG, strprintf("%s, signature byte %u changed (relay flags)", name, pos));
        }
        {
            CMutableTransaction changed = tx;
            changed.vout[0].nValue -= 1;
            CheckError(VerifyInput(changed, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, name + ", output amount changed");
            changed = tx;
            changed.vin[1].nSequence -= 1;
            CheckError(VerifyInput(changed, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, name + ", sequence of the other input changed");
            std::vector<CTxOut> spent_changed = vector.spent_outputs;
            spent_changed[1].nValue += 1;
            CheckError(VerifyInput(tx, spent_changed, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, name + ", amount of the other spent output changed");
            spent_changed = vector.spent_outputs;
            spent_changed[1].scriptPubKey = ProgramScript(OP_0, 20, 0x01);
            CheckError(VerifyInput(tx, spent_changed, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, name + ", scriptPubKey of the other spent output changed");
            spent_changed = vector.spent_outputs;
            spent_changed[0].nValue += 1;
            CheckError(VerifyInput(tx, spent_changed, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, name + ", amount of the spent output changed");
        }
        // A signature with an explicit hash type byte of 0 is not allowed, and the one-byte default cannot be
        // replaced by SIGHASH_ALL without a new signature.
        {
            CMutableTransaction changed = tx;
            changed.vin[0].scriptWitness.stack[0].push_back(0x00);
            CheckError(VerifyInput(changed, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG_HASHTYPE, name + ", hash type byte 0");
            changed.vin[0].scriptWitness.stack[0].back() = 0x01;
            CheckError(VerifyInput(changed, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_SCHNORR_SIG, name + ", hash type byte 1");
        }
    }

    // Leaf 1 spends the same output without a signature.
    {
        CMutableTransaction tx = vector.unsigned_tx;
        tx.vin[0].scriptWitness.stack = {vector.scripts[1], vector.control_blocks[1]};
        CheckError(VerifyInput(tx, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_OK, "leaf 1 (block flags)");
        CheckError(VerifyInput(tx, vector.spent_outputs, 0, POLICY_FLAGS), SCRIPT_ERR_OK, "leaf 1 (relay flags)");
        // Each control block belongs to its own leaf.
        tx.vin[0].scriptWitness.stack = {vector.scripts[1], vector.control_blocks[0]};
        CheckError(VerifyInput(tx, vector.spent_outputs, 0, CONSENSUS_FLAGS), SCRIPT_ERR_WITNESS_PROGRAM_MISMATCH, "leaf 1 with the control block of leaf 0");
    }
}

BOOST_AUTO_TEST_CASE(bip360_construction_vectors)
{
    // The test vectors of BIP 360: every output with its output type, every leaf hash and root, and a spend of every
    // leaf with its control block.
    const VectorCounts construction = CheckConstructionVectors(json_tests::p2mr_construction, sizeof(json_tests::p2mr_construction), "p2mr_construction.json");
    BOOST_CHECK_EQUAL(construction.error_vectors, 2U);
    BOOST_CHECK_EQUAL(construction.error_vectors_with_script, 1U);
    BOOST_CHECK_EQUAL(construction.spends, 16U);
    BOOST_CHECK_EQUAL(construction.depth_zero, 1U);
    BOOST_CHECK_EQUAL(construction.other_leaf_version, 1U);
    BOOST_CHECK_EQUAL(construction.op_success, 0U);
    BOOST_CHECK_EQUAL(construction.executed, 14U);

    const VectorCounts pqc = CheckConstructionVectors(json_tests::p2mr_pqc_construction, sizeof(json_tests::p2mr_pqc_construction), "p2mr_pqc_construction.json");
    BOOST_CHECK_EQUAL(pqc.error_vectors, 1U);
    BOOST_CHECK_EQUAL(pqc.error_vectors_with_script, 0U);
    BOOST_CHECK_EQUAL(pqc.spends, 13U);
    BOOST_CHECK_EQUAL(pqc.depth_zero, 1U);
    BOOST_CHECK_EQUAL(pqc.other_leaf_version, 1U);
    BOOST_CHECK_EQUAL(pqc.op_success, 9U);
    BOOST_CHECK_EQUAL(pqc.executed, 2U);
}

BOOST_AUTO_TEST_CASE(type_and_policy)
{
    // OP_2 <32 bytes> has an output type of its own. Every other witness version 2 program, and every program of a
    // later version, is still an unknown witness program.
    const uint256 root = RootFromPath(LeafHash(LEAF_TAPSCRIPT, ScriptBytes(CScript() << OP_TRUE)), Siblings(1));
    const CScript p2mr = P2MRScript(root);
    std::vector<valtype> solutions;
    BOOST_CHECK(Solver(p2mr, solutions) == TxoutType::WITNESS_V2_P2MR);
    BOOST_CHECK(solutions == std::vector<valtype>({valtype{2}, ToByteVector(root)}));
    BOOST_CHECK_EQUAL(GetTxnOutputType(TxoutType::WITNESS_V2_P2MR), "witness_v2_p2mr");
    BOOST_CHECK_EQUAL(GetTxnOutputType(Solver(p2mr, solutions)), "witness_v2_p2mr");
    for (const size_t size : {2, 20, 31, 33, 40}) {
        BOOST_CHECK_MESSAGE(Solver(ProgramScript(OP_2, size, 0x5a), solutions) == TxoutType::WITNESS_UNKNOWN, strprintf("witness version 2, %u-byte program", size));
    }
    BOOST_CHECK(Solver(CScript() << OP_3 << ToByteVector(root), solutions) == TxoutType::WITNESS_UNKNOWN);
    BOOST_CHECK(Solver(CScript() << OP_16 << ToByteVector(root), solutions) == TxoutType::WITNESS_UNKNOWN);
    BOOST_CHECK(Solver(CScript() << OP_1 << ToByteVector(root), solutions) == TxoutType::WITNESS_V1_TAPROOT);
    const CScript p2sh = GetScriptForDestination(ScriptHash(p2mr));
    BOOST_CHECK(Solver(p2sh, solutions) == TxoutType::SCRIPTHASH);

    // The destination is still a witness program of version 2, so the address is the same bech32m string.
    CTxDestination dest;
    BOOST_REQUIRE(ExtractDestination(p2mr, dest));
    const WitnessUnknown* program = boost::get<WitnessUnknown>(&dest);
    BOOST_REQUIRE(program != nullptr);
    BOOST_CHECK_EQUAL(program->version, 2U);
    BOOST_CHECK_EQUAL(program->length, 32U);
    BOOST_CHECK(std::equal(root.begin(), root.end(), program->program));
    BOOST_CHECK(GetScriptForDestination(dest) == p2mr);
    valtype data{2};
    ConvertBits<8, 5, true>([&](unsigned char c) { data.push_back(c); }, root.begin(), root.end());
    const std::string address = EncodeDestination(dest);
    BOOST_CHECK_EQUAL(address, bech32::Encode(bech32::Encoding::BECH32M, Params().Bech32HRP(), data));
    BOOST_CHECK_EQUAL(address.substr(0, Params().Bech32HRP().size() + 2), Params().Bech32HRP() + "1z");
    BOOST_CHECK(GetScriptForDestination(DecodeDestination(address)) == p2mr);
    TxoutType type;
    std::vector<CTxDestination> addresses;
    int required;
    BOOST_CHECK(ExtractDestinations(p2mr, type, addresses, required));
    BOOST_CHECK(type == TxoutType::WITNESS_V2_P2MR);
    BOOST_CHECK(addresses == std::vector<CTxDestination>{dest});
    BOOST_CHECK_EQUAL(required, 1);

    // It is a segwit output, as it was as an unknown witness program, also nested in P2SH. A native Taproot output
    // is not.
    FillableSigningProvider provider;
    BOOST_CHECK(IsSegWitOutput(provider, p2mr));
    BOOST_CHECK(!IsSegWitOutput(provider, p2sh));
    BOOST_CHECK(provider.AddCScript(p2mr));
    BOOST_CHECK(IsSegWitOutput(provider, p2sh));
    BOOST_CHECK(IsSegWitOutput(provider, ProgramScript(OP_2, 20, 0x5a)));
    BOOST_CHECK(!IsSegWitOutput(provider, CScript() << OP_1 << ToByteVector(root)));

    // A spend of it is a standard input only when the P2MR rules apply to the next block, whatever the state of
    // Taproot. Spends of other witness version 2 programs and of later versions stay non-standard.
    CCoinsView no_coins;
    CCoinsViewCache coins(&no_coins);
    // A transaction whose inputs spend outputs with these scripts, each added to the view under an outpoint of its own.
    const auto spending = [&](const std::vector<CScript>& spent_scripts) {
        CMutableTransaction tx;
        for (const CScript& script : spent_scripts) {
            const COutPoint prevout(Sha256(ScriptBytes(script)), 0);
            if (!coins.HaveCoin(prevout)) coins.AddCoin(prevout, Coin(CTxOut(10000, script), 1, false, false), false);
            tx.vin.emplace_back(prevout);
        }
        tx.vout.emplace_back(1000, ProgramScript(OP_0, 20, 0x77));
        return CTransaction(tx);
    };
    const CScript taproot = CScript() << OP_1 << ToByteVector(root);
    const CTransaction spends_p2mr = spending({p2mr});
    BOOST_CHECK(!AreInputsStandard(spends_p2mr, coins, /* taproot_active */ true, /* p2mr_active */ false));
    BOOST_CHECK(!AreInputsStandard(spends_p2mr, coins, /* taproot_active */ false, /* p2mr_active */ false));
    BOOST_CHECK(AreInputsStandard(spends_p2mr, coins, /* taproot_active */ true, /* p2mr_active */ true));
    BOOST_CHECK(AreInputsStandard(spends_p2mr, coins, /* taproot_active */ false, /* p2mr_active */ true));
    const CTransaction spends_both = spending({taproot, p2mr});
    BOOST_CHECK(AreInputsStandard(spends_both, coins, /* taproot_active */ true, /* p2mr_active */ true));
    BOOST_CHECK(!AreInputsStandard(spends_both, coins, /* taproot_active */ false, /* p2mr_active */ true));
    BOOST_CHECK(!AreInputsStandard(spends_both, coins, /* taproot_active */ true, /* p2mr_active */ false));
    for (const CScript& other : {ProgramScript(OP_2, 31, 0x5a), ProgramScript(OP_2, 33, 0x5a), CScript() << OP_3 << ToByteVector(root)}) {
        BOOST_CHECK_MESSAGE(!AreInputsStandard(spending({other}), coins, /* taproot_active */ true, /* p2mr_active */ true), "spend of " + HexStr(other));
        BOOST_CHECK_MESSAGE(!AreInputsStandard(spending({other, p2mr}), coins, /* taproot_active */ true, /* p2mr_active */ true), "spend of " + HexStr(other) + " and of P2MR");
    }

    // The witness of a spend is standard within the limits of a Taproot script path spend: no annex, and in a leaf
    // with leaf version 0xc0 no element of the initial stack larger than 80 bytes, at any depth. A leaf with another
    // leaf version has no size limit here; the script flags of relay refuse it.
    const auto witness_standard = [&](const CScript& spent_script, const std::vector<valtype>& witness, const CScript& script_sig = CScript()) {
        CMutableTransaction tx(spending({spent_script}));
        tx.vin[0].scriptSig = script_sig;
        tx.vin[0].scriptWitness.stack = witness;
        return IsWitnessStandard(CTransaction(tx), coins);
    };
    const valtype op_true = ScriptBytes(CScript() << OP_TRUE);
    const valtype control = ControlBlock(LEAF_TAPSCRIPT, Siblings(1));
    const valtype control_other_version = ControlBlock(0xc2, Siblings(1));
    const valtype item_80(80, 0x01), item_81(81, 0x01);
    BOOST_CHECK(witness_standard(p2mr, {op_true, control}));
    BOOST_CHECK(witness_standard(p2mr, {item_80, item_80, op_true, control}));
    BOOST_CHECK(!witness_standard(p2mr, {item_81, op_true, control}));
    BOOST_CHECK(!witness_standard(p2mr, {item_80, item_81, op_true, control}));
    BOOST_CHECK(witness_standard(p2mr, {item_81, op_true, control_other_version}));
    BOOST_CHECK(witness_standard(p2mr, {op_true, valtype(1000, 0x51), control}));
    // At depth zero the leaf is not run, but the limit is the same.
    BOOST_CHECK(witness_standard(p2mr, {item_80, op_true, valtype{0xc1}}));
    BOOST_CHECK(!witness_standard(p2mr, {item_81, op_true, valtype{0xc1}}));
    BOOST_CHECK(witness_standard(p2mr, {item_81, op_true, valtype{0xc3}}));
    // An annex, with or without other elements.
    BOOST_CHECK(!witness_standard(p2mr, {op_true, control, valtype{0x50}}));
    BOOST_CHECK(!witness_standard(p2mr, {op_true, control, valtype{0x50, 0x01}}));
    BOOST_CHECK(!witness_standard(p2mr, {op_true, control_other_version, valtype{0x50}}));
    BOOST_CHECK(!witness_standard(p2mr, {op_true, valtype{0x50}}));
    BOOST_CHECK(witness_standard(p2mr, {op_true, control, valtype{0x51}}));
    // Witnesses that are invalid by consensus: fewer than two elements, an empty control block.
    BOOST_CHECK(!witness_standard(p2mr, {op_true}));
    BOOST_CHECK(!witness_standard(p2mr, {valtype{0x50}}));
    BOOST_CHECK(!witness_standard(p2mr, {valtype{}}));
    BOOST_CHECK(!witness_standard(p2mr, {op_true, valtype{}}));
    BOOST_CHECK(!witness_standard(p2mr, {item_81, op_true, valtype{}}));
    // An input without a witness is not looked at here.
    BOOST_CHECK(witness_standard(p2mr, {}));
    // Other witness version 2 programs and OP_2 <32 bytes> nested in P2SH are not P2MR outputs: these limits do not
    // apply to them (the script flags of relay refuse their spends).
    BOOST_CHECK(witness_standard(ProgramScript(OP_2, 31, 0x5a), {item_81, op_true, control, valtype{0x50}}));
    BOOST_CHECK(witness_standard(p2sh, {item_81, op_true, control, valtype{0x50}}, CScript() << ScriptBytes(p2mr)));
}

BOOST_AUTO_TEST_CASE(signing)
{
    // The signing code cannot sign for Taproot and P2MR outputs. It keeps the witnesses of inputs that spend them,
    // and it can check their signatures only if it knows every output the transaction spends.
    CMutableTransaction signed_tx;
    BOOST_REQUIRE(DecodeHexTx(signed_tx, TAPROOT_AND_P2MR_TX, /* try_no_witness */ false, /* try_witness */ true));
    BOOST_REQUIRE_EQUAL(signed_tx.vin.size(), 2U);
    std::vector<CTxOut> spent_outputs;
    for (size_t i = 0; i < 2; ++i) spent_outputs.emplace_back(TAPROOT_AND_P2MR_AMOUNTS[i], ScriptFromHex(TAPROOT_AND_P2MR_SCRIPTS[i]));
    int version;
    valtype program;
    BOOST_REQUIRE(spent_outputs[0].scriptPubKey.IsWitnessProgram(version, program) && version == 1 && program.size() == 32);
    BOOST_REQUIRE(spent_outputs[1].scriptPubKey.IsWitnessProgram(version, program) && version == 2 && program.size() == 32);
    BOOST_REQUIRE_EQUAL(signed_tx.vin[0].scriptWitness.stack.size(), 1U);
    BOOST_REQUIRE_EQUAL(signed_tx.vin[1].scriptWitness.stack.size(), 3U);
    BOOST_REQUIRE_EQUAL(signed_tx.vin[1].scriptWitness.stack[1].back(), OP_CHECKSIG);
    // Both signatures are valid, and they are really checked.
    for (unsigned int i = 0; i < 2; ++i) {
        CheckError(VerifyInput(signed_tx, spent_outputs, i, CONSENSUS_FLAGS), SCRIPT_ERR_OK, strprintf("input %u (block flags)", i));
        CheckError(VerifyInput(signed_tx, spent_outputs, i, POLICY_FLAGS), SCRIPT_ERR_OK, strprintf("input %u (relay flags)", i));
        CMutableTransaction changed = signed_tx;
        changed.vin[i].scriptWitness.stack[0][10] ^= 0x01;
        CheckError(VerifyInput(changed, spent_outputs, i, POLICY_FLAGS), SCRIPT_ERR_SCHNORR_SIG, strprintf("input %u with a changed signature", i));
    }

    std::map<COutPoint, Coin> coins;
    for (size_t i = 0; i < 2; ++i) coins.emplace(signed_tx.vin[i].prevout, Coin(spent_outputs[i], 1, false, false));
    FillableSigningProvider keystore;
    CKey key;
    key.MakeNewKey(true);
    BOOST_REQUIRE(keystore.AddKey(key));
    const CScript p2pkh = GetScriptForDestination(PKHash(key.GetPubKey()));
    const CScript p2wpkh = GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()));
    const auto witnesses = [](const CMutableTransaction& tx) {
        std::vector<std::vector<valtype>> stacks;
        for (const CTxIn& txin : tx.vin) stacks.push_back(txin.scriptWitness.stack);
        return stacks;
    };
    const std::vector<std::vector<valtype>> given_witnesses = witnesses(signed_tx);

    // With every spent output known, the witnesses come back unchanged and the transaction is complete.
    {
        CMutableTransaction tx = signed_tx;
        std::map<int, std::string> errors;
        BOOST_CHECK(SignTransaction(tx, &keystore, coins, SIGHASH_ALL, errors));
        BOOST_CHECK_MESSAGE(errors.empty(), "errors: " + ErrorsString(errors));
        BOOST_CHECK(witnesses(tx) == given_witnesses);
        BOOST_CHECK(tx.vin[0].scriptSig.empty() && tx.vin[1].scriptSig.empty());
    }
    // With a third input whose coin is not known, the signatures cannot be checked: both inputs keep their
    // witnesses and report "unknown error".
    {
        CMutableTransaction tx = signed_tx;
        tx.vin.emplace_back(COutPoint(uint256S("33"), 0));
        std::map<int, std::string> errors;
        BOOST_CHECK(!SignTransaction(tx, &keystore, coins, SIGHASH_ALL, errors));
        const std::map<int, std::string> expected{{0, "unknown error"}, {1, "unknown error"}, {2, "Input not found or already spent"}};
        BOOST_CHECK_MESSAGE(errors == expected, "errors: " + ErrorsString(errors));
        const std::vector<std::vector<valtype>> got = witnesses(tx);
        BOOST_CHECK(std::vector<std::vector<valtype>>(got.begin(), got.begin() + 2) == given_witnesses);
    }
    // The same when the third input's amount is not known, which the signing RPCs give as MAX_MONEY. That input
    // spends a P2PKH output, whose signature does not commit to amounts: it is signed and complete.
    {
        CMutableTransaction tx = signed_tx;
        tx.vin.emplace_back(COutPoint(uint256S("33"), 0));
        std::map<COutPoint, Coin> coins_without_amount = coins;
        coins_without_amount.emplace(tx.vin[2].prevout, Coin(CTxOut(MAX_MONEY, p2pkh), 1, false, false));
        std::map<int, std::string> errors;
        BOOST_CHECK(!SignTransaction(tx, &keystore, coins_without_amount, SIGHASH_ALL, errors));
        const std::map<int, std::string> expected{{0, "unknown error"}, {1, "unknown error"}};
        BOOST_CHECK_MESSAGE(errors == expected, "errors: " + ErrorsString(errors));
        const std::vector<std::vector<valtype>> got = witnesses(tx);
        BOOST_CHECK(std::vector<std::vector<valtype>>(got.begin(), got.begin() + 2) == given_witnesses);
        BOOST_CHECK(!tx.vin[2].scriptSig.empty() && tx.vin[2].scriptWitness.IsNull());
    }

    // ProduceSignature marks both inputs as witness inputs and keeps their witnesses, though it cannot complete them
    // without the spent outputs.
    for (unsigned int i = 0; i < 2; ++i) {
        SignatureData sigdata = DataFromTransaction(signed_tx, i, spent_outputs[i]);
        BOOST_CHECK(!sigdata.complete);
        BOOST_CHECK(!ProduceSignature(keystore, MutableTransactionSignatureCreator(&signed_tx, i, spent_outputs[i].nValue, SIGHASH_ALL), spent_outputs[i].scriptPubKey, sigdata));
        BOOST_CHECK(sigdata.witness);
        BOOST_CHECK(sigdata.scriptWitness.stack == given_witnesses[i]);
        BOOST_CHECK(sigdata.scriptSig.empty());
    }

    // P2PKH and P2WPKH inputs are signed as before: a P2PKH input gets a scriptSig and loses a witness it was given,
    // a P2WPKH input gets a witness of a signature and the key.
    {
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint(uint256S("44"), 0));
        tx.vin.emplace_back(COutPoint(uint256S("44"), 1));
        tx.vin[0].scriptWitness.stack = {valtype{0x01}};
        tx.vout.emplace_back(19000, ProgramScript(OP_0, 20, 0x77));
        const std::vector<CTxOut> spent{CTxOut(10000, p2pkh), CTxOut(10000, p2wpkh)};
        std::map<COutPoint, Coin> legacy_coins;
        for (size_t i = 0; i < 2; ++i) legacy_coins.emplace(tx.vin[i].prevout, Coin(spent[i], 1, false, false));
        std::map<int, std::string> errors;
        BOOST_CHECK(SignTransaction(tx, &keystore, legacy_coins, SIGHASH_ALL, errors));
        BOOST_CHECK(errors.empty());
        BOOST_CHECK(tx.vin[0].scriptWitness.IsNull());
        std::vector<valtype> pushes;
        CScript::const_iterator pc = tx.vin[0].scriptSig.begin();
        opcodetype opcode;
        valtype push;
        while (tx.vin[0].scriptSig.GetOp(pc, opcode, push)) pushes.push_back(push);
        BOOST_CHECK(pushes.size() == 2 && pushes[1] == ToByteVector(key.GetPubKey()));
        BOOST_CHECK(tx.vin[1].scriptSig.empty());
        BOOST_CHECK_EQUAL(tx.vin[1].scriptWitness.stack.size(), 2U);
        BOOST_CHECK(tx.vin[1].scriptWitness.stack.back() == ToByteVector(key.GetPubKey()));
        for (unsigned int i = 0; i < 2; ++i) {
            CheckError(VerifyInput(tx, spent, i, CONSENSUS_FLAGS), SCRIPT_ERR_OK, strprintf("signed legacy input %u", i));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
