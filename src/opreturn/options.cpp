// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/options.h>

#include <opreturn/log.h>
#include <opreturn/names.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <algorithm>
#include <exception>
#include <limits>

namespace opreturn {
namespace {

constexpr uint64_t MIB{1024 * 1024};

uint8_t Bit(Option option)
{
    return uint8_t(1 << uint8_t(option));
}

//! The size limit in bytes for a value in MiB, if it is one that may be set.
bool MaxSizeFromMiB(int64_t mib, uint64_t& bytes)
{
    if (mib != 0 && (mib < MIN_MAX_SIZE_MIB || mib > MAX_MAX_SIZE_MIB)) return false;
    bytes = uint64_t(mib) * MIB;
    return true;
}

//! Reads the size limit in MiB under one name: text that is a whole number, or a number from the settings file,
//! which is read as the tree reads the numbers of its integer options. False for other text; throws for a value of
//! another type.
bool ReadMaxSizeMiB(const ArgsManager& args, const char* name, int64_t& mib)
{
    std::string text;
    try {
        text = args.GetArg(name, "");
    } catch (const std::exception&) {
        // Reading the value as text fails only for a value that is neither text nor a boolean.
        mib = args.GetArg(name, int64_t{0});
        return true;
    }
    return ParseInt64(text, &mib);
}

/**
 * Reads an option under both of its names. `read(name, value)` reads it under one name, which is set; it returns
 * false, with `error` set, for a value that may not be set, and throws for a value that cannot be read. Each name's
 * value is the one the tree's precedence gives it; when both names have one, the two must be equal. `set` tells
 * whether either has one. This is the only place that decides between the two names.
 */
template <typename T, typename Read>
bool ReadBothNames(const ArgsManager& args, Option option, Read read, bool& set, T& value, std::string& error)
{
    set = false;
    for (const char* name : {OptionName(option), OptionAlias(option)}) {
        if (!args.IsArgSet(name)) continue;
        T read_value{};
        try {
            if (!read(name, read_value)) return false;
        } catch (const std::exception&) {
            // Only a value in the settings file of a type the option does not take gets here.
            error = strprintf("%s has a value that cannot be read", name);
            return false;
        }
        if (set && read_value != value) {
            error = strprintf("%s is another name for %s, and the two are set to different values: remove %s",
                              OptionAlias(option), OptionName(option), OptionAlias(option));
            return false;
        }
        value = read_value;
        set = true;
    }
    return true;
}

} // namespace

uint64_t DefaultMaxSizeBytes(const NodeFacts& facts)
{
    if (!facts.prune_mode) return 0;
    const uint64_t least = PRUNED_MAX_SIZE_MIB * MIB;
    // Manual pruning has no target; it gets the least limit.
    if (facts.prune_target_bytes == std::numeric_limits<uint64_t>::max()) return least;
    return std::max(facts.prune_target_bytes, least);
}

bool ResolveOptions(const ArgsManager& args, const StoredChoices& stored, const NodeFacts& facts, bool gui_host,
                    Options& out, std::string& error)
{
    // In a GUI program with a wallet the tab can switch the record on and off while the program runs, and it
    // remembers that choice.
    const bool tab = gui_host && !facts.disable_wallet;
    const auto read_switch = [&](const char* name, bool& value) {
        value = args.GetBoolArg(name, false);
        return true;
    };
    const auto read_max_size = [&](const char* name, int64_t& mib) {
        uint64_t bytes;
        if (ReadMaxSizeMiB(args, name, mib) && MaxSizeFromMiB(mib, bytes)) return true;
        error = strprintf("%s must be 0 (no limit) or from %d to %d MiB", name, MIN_MAX_SIZE_MIB, MAX_MAX_SIZE_MIB);
        return false;
    };

    // Every option is read and checked whether or not the record is on, the size limit first.
    bool max_size_set{false}, index_set{false}, reindex_set{false};
    int64_t max_size_mib{0};
    bool index{false}, reindex{false};
    if (!ReadBothNames(args, Option::MAX_SIZE, read_max_size, max_size_set, max_size_mib, error) ||
        !ReadBothNames(args, Option::INDEX, read_switch, index_set, index, error) ||
        !ReadBothNames(args, Option::REINDEX, read_switch, reindex_set, reindex, error)) {
        return false;
    }

    Options result;
    if (index_set) result.explicit_options |= EXPLICIT_RECORD;
    if (max_size_set) result.explicit_options |= EXPLICIT_MAXSIZE;
    if (reindex_set) result.explicit_options |= EXPLICIT_REINDEX;

    if (max_size_set) {
        MaxSizeFromMiB(max_size_mib, result.max_size_bytes);
    } else if (gui_host && stored.max_size_mib) {
        if (!MaxSizeFromMiB(*stored.max_size_mib, result.max_size_bytes)) {
            Log(LogEvent::STORED_CHOICE_IGNORED, int64_t(Option::MAX_SIZE));
            result.max_size_bytes = DefaultMaxSizeBytes(facts);
        }
    } else {
        result.max_size_bytes = DefaultMaxSizeBytes(facts);
    }

    if (index_set) {
        result.record = index;
    } else {
        // Without a wallet the GUI shows no tab, so a stored choice could neither be seen nor changed.
        result.record = tab && (stored.record ? *stored.record : true);
    }

    result.reindex = result.record && reindex_set && reindex;

    if (!result.record) {
        for (const Option option : {Option::MAX_SIZE, Option::REINDEX}) {
            if (!(result.explicit_options & Bit(option))) continue;
            // With the tab, the record can still be switched on, and then the size limit applies.
            if (tab && option == Option::MAX_SIZE) continue;
            Log(LogEvent::OPTION_IGNORED, int64_t(option));
        }
    }
    out = result;
    return true;
}

} // namespace opreturn
