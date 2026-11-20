// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>

namespace {

typedef std::vector<unsigned char> valtype;

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

BOOST_AUTO_TEST_SUITE_END()
