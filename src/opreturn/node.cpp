// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/node.h>

#include <interfaces/chain.h>
#include <node/context.h>
#include <node/ui_interface.h>
#include <opreturn/names.h>
#include <opreturn/record.h>
#include <opreturn/registry.h>
#include <tinyformat.h>
#include <util/memory.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

namespace opreturn {
namespace {

std::atomic<bool> g_gui_host{false};

std::string HelpText(Option option)
{
    switch (option) {
    case Option::INDEX:
        return strprintf("Keep %s: every OP_RETURN output of the chain, for the FEXT tab. It works on pruned nodes and "
                         "keeps outputs after their blocks are deleted, up to %s (default: 1; 0 with -disablewallet)",
                         RECORD_NOUN, OptionName(Option::MAX_SIZE));
    case Option::MAX_SIZE:
        return strprintf("Keep %s under <n> MiB by removing its oldest outputs: 0 for no limit, or from %d to %d "
                         "(default: on a pruned node the larger of the prune target and %u, and %u with manual "
                         "pruning; otherwise 0)",
                         RECORD_NOUN, MIN_MAX_SIZE_MIB, MAX_MAX_SIZE_MIB, PRUNED_MAX_SIZE_MIB, PRUNED_MAX_SIZE_MIB);
    case Option::REINDEX:
        return strprintf("Delete %s at start and build it again from the blocks on disk. On a pruned node the outputs "
                         "of deleted blocks are lost (default: 0)",
                         RECORD_NOUN);
    }
    return "";
}

/**
 * The record's chain client: init calls it as it calls the wallet's, and it passes each call to the record. Without
 * a record it only reports, at verify(), why there is none.
 */
class RecordClient final : public interfaces::ChainClient
{
public:
    RecordClient(std::shared_ptr<Record> record, std::string error) : m_record(std::move(record)), m_error(std::move(error)) {}

    void registerRpcs() override {}
    bool verify() override
    {
        if (m_error.empty()) return true;
        return InitError(Untranslated(m_error));
    }
    bool load() override
    {
        // A record that fails stops alone, never the node.
        if (m_record) m_record->load();
        return true;
    }
    void start(CScheduler&) override
    {
        if (m_record) m_record->start();
    }
    void flush() override
    {
        if (m_record) m_record->flush();
    }
    void stop() override
    {
        if (m_record) m_record->stop();
    }
    void setMockTime(int64_t) override {}

private:
    const std::shared_ptr<Record> m_record;
    const std::string m_error;
};

} // namespace

void AddArgs(ArgsManager& args)
{
    for (const Option option : {Option::INDEX, Option::MAX_SIZE, Option::REINDEX}) {
        const std::string name{OptionName(option)};
        args.AddArg(option == Option::MAX_SIZE ? name + "=<n>" : name, HelpText(option), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg(OptionAlias(option), "", ArgsManager::ALLOW_ANY, OptionsCategory::HIDDEN);
    }
}

void SetGuiHost(bool gui_host)
{
    g_gui_host = gui_host;
}

NodeFacts ReadNodeFacts(const ArgsManager& args)
{
    NodeFacts facts;
    // The wallet's default: a wallet unless -disablewallet is set.
    facts.disable_wallet = args.GetBoolArg("-disablewallet", false);
    // As AppInitParameterInteraction has set them from -prune.
    facts.prune_mode = fPruneMode;
    facts.prune_target_bytes = nPruneTarget;
    return facts;
}

bool Construct(NodeContext& node, const StoredChoices& stored)
{
    if (!node.args) return false;
    try {
        const ArgsManager& args = *node.args;
        const bool gui_host = g_gui_host;
        const NodeFacts facts = ReadNodeFacts(args);
        Options options;
        std::string error;
        if (!ResolveOptions(args, stored, facts, gui_host, options, error)) {
            node.chain_clients.push_back(MakeUnique<RecordClient>(nullptr, std::move(error)));
            return true;
        }
        if (!options.record && !gui_host) return true;
        auto record = std::make_shared<Record>(GetDataDir() / "indexes" / INDEX_DIR_NAME, options);
        node.chain_clients.push_back(MakeUnique<RecordClient>(record, ""));
        SetRecord(record);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace opreturn
