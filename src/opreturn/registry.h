// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_REGISTRY_H
#define BITCOIN_OPRETURN_REGISTRY_H

#include <memory>

namespace interfaces {
class OpReturnRecord;
} // namespace interfaces

namespace opreturn {

/**
 * The OP_RETURN record of this process: the one that opreturn::Construct made last, while its chain client or a
 * reader still holds it; null when the process keeps none. Any thread; takes no node lock. The registry holds the
 * record only weakly, so that letting go of it never happens here.
 */
std::shared_ptr<interfaces::OpReturnRecord> GetRecord();

//! Makes `record` the record GetRecord returns. Only Construct calls it.
void SetRecord(const std::shared_ptr<interfaces::OpReturnRecord>& record);

} // namespace opreturn

#endif // BITCOIN_OPRETURN_REGISTRY_H
