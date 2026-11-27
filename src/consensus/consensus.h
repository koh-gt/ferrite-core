// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_CONSENSUS_H
#define BITCOIN_CONSENSUS_CONSENSUS_H

#include <stdlib.h>
#include <stdint.h>
#include <limits>
#include <mw/consensus/Params.h>

/** The maximum allowed size for a serialized block, in bytes (only for buffer size limits) */
static const unsigned int MAX_BLOCK_SERIALIZED_SIZE = 4000000;
/** MWEB: The maximum size of an MWEB block is just over 11 MB, plus 4 MB for a full segwit block. Add a ~1 MB buffer just to be safe. */
static const unsigned int MAX_BLOCK_SERIALIZED_SIZE_WITH_MWEB = MAX_BLOCK_SERIALIZED_SIZE + mw::MAX_BLOCK_BYTES;
/** The maximum allowed weight for a block, see BIP 141 (network rule) */
static const unsigned int MAX_BLOCK_WEIGHT = 4000000;
/** The maximum allowed number of signature check operations in a block (network rule) */
static const int64_t MAX_BLOCK_SIGOPS_COST = 80000;
/** Coinbase transaction outputs can only be spent after this number of new blocks (network rule) */
static const int COINBASE_MATURITY = 10;
/** Coinbase transaction outputs created once the coinbasematurity deployment is active can only be spent after
 *  this number of new blocks (network rule) */
static const int COINBASE_MATURITY_EXTENDED = 100;
/** Activation height of the coinbasematurity deployment on a chain on which it does not apply to the block considered */
static constexpr int COINBASE_MATURITY_NOT_ACTIVE = std::numeric_limits<int>::max();

/** Number of new blocks after which a coinbase output created at creation_height can be spent, on a chain on which the
 *  coinbasematurity deployment applies from block activation_height (COINBASE_MATURITY_NOT_ACTIVE if it does not apply). */
constexpr int GetCoinbaseMaturity(int creation_height, int activation_height)
{
    return creation_height >= activation_height ? COINBASE_MATURITY_EXTENDED : COINBASE_MATURITY;
}
/** MWEB: Pegout transaction outputs can only be spent after this number of new blocks (network rule) */
static const int PEGOUT_MATURITY = 6;

static const int WITNESS_SCALE_FACTOR = 4;

static const size_t MIN_TRANSACTION_WEIGHT = WITNESS_SCALE_FACTOR * 60; // 60 is the lower bound for the size of a valid serialized CTransaction
static const size_t MIN_SERIALIZABLE_TRANSACTION_WEIGHT = WITNESS_SCALE_FACTOR * 10; // 10 is the lower bound for the size of a serialized CTransaction

/** Flags for nSequence and nLockTime locks */
/** Interpret sequence numbers as relative lock-time constraints. */
static constexpr unsigned int LOCKTIME_VERIFY_SEQUENCE = (1 << 0);
/** Use GetMedianTimePast() instead of nTime for end point timestamp. */
static constexpr unsigned int LOCKTIME_MEDIAN_TIME_PAST = (1 << 1);

#endif // BITCOIN_CONSENSUS_CONSENSUS_H
