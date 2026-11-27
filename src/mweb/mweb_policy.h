#pragma once

#include <mw/consensus/Params.h>

#include <cstddef>
#include <string>

// Forward Declarations
class CTransaction;

namespace MWEB {

// Relay/mempool policy limits for standalone MWEB transactions (non-consensus).
//
// The consensus limits (mw::MAX_BLOCK_WEIGHT / mw::MAX_NUM_INPUTS) permit a
// single transaction to contain an entire block's worth of inputs, outputs, and
// kernels. Because MWEB signature and rangeproof verification runs during
// mempool acceptance before any fee is required, relaying such a transaction
// would let a peer force a full block's worth of crypto verification for free.
// We therefore bound relayed MWEB transactions to ~2% of the consensus limits.
// (Note inputs carry no MWEB weight, so they must be bounded by count.)
// The wallet keeps the transactions it creates within the input limit.
static constexpr size_t MAX_STANDARD_MWEB_TX_WEIGHT = mw::MAX_BLOCK_WEIGHT / 50; // 4,000
static constexpr size_t MAX_STANDARD_MWEB_TX_INPUTS = mw::MAX_NUM_INPUTS / 50;   // 1,000

class Policy
{
public:
    /// <summary>
    /// Checks the transaction for violation of any MWEB-specific standard tx policies.
    /// </summary>
    /// <param name="tx">The transaction to check.</param>
    /// <param name="reason">The reason it's non-standard, if any.</param>
    /// <returns>True if the transaction is standard.</returns>
    static bool IsStandardTx(const CTransaction& tx, std::string& reason);

    /// <summary>
    /// Cheaply checks that an MWEB transaction is within the relay weight/input
    /// limits (much tighter than the consensus limits). Intended to run before
    /// the expensive signature/rangeproof verification in
    /// MWEB::Node::CheckTransaction, to prevent a cheap-to-relay,
    /// expensive-to-verify DoS.
    /// </summary>
    /// <param name="tx">The transaction to check.</param>
    /// <param name="reason">The reason it's non-standard, if any.</param>
    /// <returns>True if the transaction is within the relay limits.</returns>
    static bool CheckWeight(const CTransaction& tx, std::string& reason);
};

}