// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/registry.h>

#include <interfaces/opreturn.h>
#include <sync.h>

namespace opreturn {
namespace {

Mutex g_registry_mutex;
std::weak_ptr<interfaces::OpReturnRecord> g_record GUARDED_BY(g_registry_mutex);

} // namespace

std::shared_ptr<interfaces::OpReturnRecord> GetRecord()
{
    LOCK(g_registry_mutex);
    return g_record.lock();
}

void SetRecord(const std::shared_ptr<interfaces::OpReturnRecord>& record)
{
    LOCK(g_registry_mutex);
    g_record = record;
}

} // namespace opreturn
