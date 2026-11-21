// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_NAMES_H
#define BITCOIN_OPRETURN_NAMES_H

#include <cstdint>

/**
 * Every name of the OP_RETURN record that a user or an operator can see: its options and the second name each of
 * them is also read under, its directory, the first words of its log lines, the noun its messages use for it, and
 * the name of its thread. No other source file spells them, so that renaming them changes this file and nothing
 * else.
 */
namespace opreturn {

//! The record's options. 1 << value is the option's bit in Options::explicit_options; REINDEX is the last.
enum class Option : uint8_t {
    INDEX = 0,    //!< keep the record
    MAX_SIZE = 1, //!< the size limit
    REINDEX = 2,  //!< build the record again at start
};

//! An option's name on the command line and in ferrite.conf, with its leading dash. Help texts, messages and log
//! lines use it.
constexpr const char* OptionName(Option option)
{
    return option == Option::INDEX    ? "-opreturnindex" :
           option == Option::MAX_SIZE ? "-opreturnmaxsize" :
                                        "-opreturnreindex";
}

//! The second name of an option, in the FEXT tab's spelling: the option is read under both, and -help shows only
//! the first.
constexpr const char* OptionAlias(Option option)
{
    return option == Option::INDEX    ? "-fextindex" :
           option == Option::MAX_SIZE ? "-fextmaxsize" :
                                        "-fextreindex";
}

//! The record's directory, inside the "indexes" directory of the network's data directory.
static constexpr const char* INDEX_DIR_NAME{"opreturn"};

//! The first words of every log line of the record.
static constexpr const char* LOG_PREFIX{"OP_RETURN index:"};

//! What help texts and error messages call the record.
static constexpr const char* RECORD_NOUN{"the OP_RETURN index"};

//! The name of the record's thread. The tree puts "b-" before a thread's name, and Linux keeps 15 characters.
static constexpr const char* WORKER_THREAD_NAME{"opret-worker"};

} // namespace opreturn

#endif // BITCOIN_OPRETURN_NAMES_H
