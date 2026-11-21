// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_OPTIONS_H
#define BITCOIN_OPRETURN_OPTIONS_H

#include <opreturn/names.h>
#include <optional.h>

#include <cstdint>
#include <string>

class ArgsManager;

/**
 * The record's options and the values they take at start. The rules are in ResolveOptions; the option names are
 * in opreturn/names.h.
 */
namespace opreturn {

//! The bits of Options::explicit_options: which options are set, under either of their names, on the command line,
//! in the settings file or in ferrite.conf.
enum ExplicitOption : uint8_t {
    EXPLICIT_RECORD = 1,  //!< 1 << Option::INDEX
    EXPLICIT_MAXSIZE = 2, //!< 1 << Option::MAX_SIZE
    EXPLICIT_REINDEX = 4, //!< 1 << Option::REINDEX
};

//! The size limit in MiB that may be set, besides 0 (no limit): from MIN_MAX_SIZE_MIB to MAX_MAX_SIZE_MIB.
static constexpr int64_t MIN_MAX_SIZE_MIB{100};
static constexpr int64_t MAX_MAX_SIZE_MIB{4194304};
//! The size limit of a pruned node at least, and the limit with manual pruning.
static constexpr uint64_t PRUNED_MAX_SIZE_MIB{1024};

//! The last choices made in the GUI's OP_RETURN tab, which only a GUI program stores and passes in.
struct StoredChoices {
    Optional<bool> record;
    Optional<int64_t> max_size_mib;
};

//! The facts of the node that the defaults depend on.
struct NodeFacts {
    bool disable_wallet{false};
    bool prune_mode{false};
    //! The prune target in bytes; the largest uint64_t with manual pruning.
    uint64_t prune_target_bytes{0};
};

//! The values the record starts with.
struct Options {
    bool record{false};          //!< keep the record
    uint64_t max_size_bytes{0};  //!< the size limit; 0 means none
    bool reindex{false};         //!< build the record again at start
    uint8_t explicit_options{0}; //!< ExplicitOption bits
};

//! The size limit a node has when none is set: on a pruned node the larger of the prune target and 1 GiB, and
//! 1 GiB with manual pruning; otherwise none.
uint64_t DefaultMaxSizeBytes(const NodeFacts& facts);

/**
 * The values the record starts with, from the options of `args`, the stored choices of the GUI and the facts of
 * the node. `gui_host` is true in a program that shows the record in a GUI tab, where the record is kept by
 * default; in every other program it is off unless its option is set, and the stored choices are not read.
 *
 * Each option is read under both of its names (OptionName and OptionAlias), each name with the tree's
 * precedence: the command line, then the settings file, then ferrite.conf, its network's section before its top
 * level. When both names of an option have a value, the two values must be equal; -noX is the value 0, and values
 * are compared as the option reads them, so that -X=1 equals -X=2 for a switch and +100 equals 100 for the size limit.
 *
 * Never throws. Returns false, with a message for the user in `error`, for a value of the size limit that does not
 * parse or is out of range, whether or not the record is on; for a value that the settings file holds in a form
 * that cannot be read; and for the two names of one option set to different values. Each message names the option
 * under the name it was read, or both names. Logs one line for each option set to no effect because the record is
 * off, and one for a stored size limit out of range, each with the option's first name.
 */
bool ResolveOptions(const ArgsManager& args, const StoredChoices& stored, const NodeFacts& facts, bool gui_host,
                    Options& out, std::string& error);

} // namespace opreturn

#endif // BITCOIN_OPRETURN_OPTIONS_H
