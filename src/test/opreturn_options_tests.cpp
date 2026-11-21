// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparamsbase.h>
#include <fs.h>
#include <opreturn/names.h>
#include <opreturn/options.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/settings.h>
#include <util/strencodings.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace opreturn;

namespace {

constexpr uint64_t MIB{1024 * 1024};

//! The options, in the order they are read.
const std::vector<Option> OPTIONS{Option::MAX_SIZE, Option::INDEX, Option::REINDEX};

std::string Set(const char* name, const std::string& value) { return std::string(name) + "=" + value; }
std::string Set(Option option, const std::string& value) { return Set(OptionName(option), value); }
std::string SetAlias(Option option, const std::string& value) { return Set(OptionAlias(option), value); }
//! An option set under its first name, and under its second.
using Setter = std::string (*)(Option, const std::string&);
const std::vector<Setter> SETTERS{static_cast<Setter>(Set), SetAlias};
std::string Negated(const char* name) { return "-no" + std::string(name).substr(1); }
//! A name's key in the settings file and in ferrite.conf.
std::string Key(const char* name) { return std::string(name).substr(1); }

std::string MaxSizeError(const char* name)
{
    return strprintf("%s must be 0 (no limit) or from 100 to 4194304 MiB", name);
}
std::string Unreadable(const char* name) { return std::string(name) + " has a value that cannot be read"; }
std::string Conflict(Option option)
{
    return strprintf("%s is another name for %s, and the two are set to different values: remove %s",
                     OptionAlias(option), OptionName(option), OptionAlias(option));
}

//! An ArgsManager that also reads a text as ferrite.conf is read.
class ConfArgs : public ArgsManager
{
public:
    void ReadConf(const std::string& text)
    {
        std::istringstream stream(text);
        std::string error;
        BOOST_REQUIRE_MESSAGE(ReadConfigStream(stream, "ferrite.conf", error, /*ignore_invalid_keys=*/true), error);
    }
};

/**
 * An ArgsManager that knows the offered options under both of their names, as the node registers them, with a
 * command line, a ferrite.conf and settings file values, for a network.
 */
struct TestArgs {
    ConfArgs args;

    explicit TestArgs(const std::vector<std::string>& command_line, const std::string& conf = "",
                      const std::map<std::string, UniValue>& settings = {}, const std::string& network = CBaseChainParams::MAIN)
    {
        for (const Option option : OPTIONS) {
            args.AddArg(OptionName(option), "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
            args.AddArg(OptionAlias(option), "", ArgsManager::ALLOW_ANY, OptionsCategory::HIDDEN);
        }
        std::vector<const char*> argv{"ferrite-qt-fext"};
        for (const std::string& arg : command_line) argv.push_back(arg.c_str());
        std::string error;
        BOOST_REQUIRE_MESSAGE(args.ParseParameters(int(argv.size()), argv.data(), error), error);
        if (!conf.empty()) args.ReadConf(conf);
        args.LockSettings([&](util::Settings& s) {
            for (const auto& setting : settings) s.rw_settings[setting.first] = setting.second;
        });
        args.SelectConfigNetwork(network);
    }
};

struct Result {
    bool ok{false};
    Options options;
    std::string error;
    std::vector<std::string> log; //!< every line the record logged
};

Result Resolve(TestArgs& test_args, bool gui_host, const NodeFacts& facts = {}, const StoredChoices& stored = {})
{
    Result result;
    const opreturn::test::LogLines lines;
    result.ok = ResolveOptions(test_args.args, stored, facts, gui_host, result.options, result.error);
    result.log = lines.Containing(LOG_PREFIX);
    return result;
}

Result Resolve(const std::vector<std::string>& command_line, bool gui_host, const NodeFacts& facts = {}, const StoredChoices& stored = {})
{
    TestArgs test_args(command_line);
    return Resolve(test_args, gui_host, facts, stored);
}

NodeFacts WithoutWallet()
{
    NodeFacts facts;
    facts.disable_wallet = true;
    return facts;
}

NodeFacts Pruned(uint64_t target_bytes)
{
    NodeFacts facts;
    facts.prune_mode = true;
    facts.prune_target_bytes = target_bytes;
    return facts;
}

StoredChoices Stored(Optional<bool> record, Optional<int64_t> max_size_mib = nullopt)
{
    StoredChoices stored;
    stored.record = record;
    stored.max_size_mib = max_size_mib;
    return stored;
}

bool Ignored(const Result& result, Option option)
{
    const std::string line = strprintf("%s off, so %s is ignored\n", LOG_PREFIX, OptionName(option));
    size_t count = 0;
    for (const std::string& logged : result.log) count += logged.size() >= line.size() && logged.compare(logged.size() - line.size(), line.size(), line) == 0;
    return count == 1;
}

//! Where a value is set: on the command line, in the settings file, at the top of ferrite.conf, or in one of its
//! network sections.
struct Place {
    enum Kind { COMMAND_LINE, SETTINGS, CONF, CONF_SECTION } kind;
    std::string section; //!< for CONF_SECTION

    std::string Name() const
    {
        return kind == COMMAND_LINE ? "command line" : kind == SETTINGS ? "settings file" : kind == CONF ? "ferrite.conf" : "ferrite.conf [" + section + "]";
    }
};

//! Values set under names, each at a place, and the network the node runs on.
struct Inputs {
    std::vector<std::string> command_line;
    std::string conf_top;
    std::map<std::string, std::string> conf_sections; //!< section -> its lines
    std::map<std::string, UniValue> settings;
    std::string network{CBaseChainParams::MAIN};

    //! Sets `name` to `value` at `place`. A switch's value "no" stands for its negated form.
    void Add(const Place& place, const char* name, const std::string& value)
    {
        const bool negated = value == "no";
        switch (place.kind) {
        case Place::COMMAND_LINE:
            command_line.push_back(negated ? Negated(name) : Set(name, value));
            break;
        case Place::SETTINGS:
            settings[Key(name)] = negated ? UniValue(false) : UniValue(value);
            break;
        case Place::CONF:
            conf_top += negated ? "no" + Key(name) + "=1\n" : Key(name) + "=" + value + "\n";
            break;
        case Place::CONF_SECTION:
            conf_sections[place.section] += negated ? "no" + Key(name) + "=1\n" : Key(name) + "=" + value + "\n";
            break;
        }
    }

    Result Resolve(bool gui_host = false, const NodeFacts& facts = {})
    {
        std::string conf = conf_top;
        for (const auto& section : conf_sections) conf += "[" + section.first + "]\n" + section.second;
        TestArgs test_args(command_line, conf, settings, network);
        return ::Resolve(test_args, gui_host, facts);
    }
};

const std::vector<Place> PLACES{{Place::COMMAND_LINE, ""}, {Place::SETTINGS, ""}, {Place::CONF, ""},
                                {Place::CONF_SECTION, CBaseChainParams::MAIN}, {Place::CONF_SECTION, CBaseChainParams::TESTNET},
                                {Place::CONF_SECTION, CBaseChainParams::REGTEST}};

} // namespace

BOOST_FIXTURE_TEST_SUITE(opreturn_options_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(names)
{
    // The names a user sees, as they are spelled: each other test builds its input from these constants.
    BOOST_CHECK_EQUAL(std::string(OptionName(Option::INDEX)), "-opreturnindex");
    BOOST_CHECK_EQUAL(std::string(OptionName(Option::MAX_SIZE)), "-opreturnmaxsize");
    BOOST_CHECK_EQUAL(std::string(OptionName(Option::REINDEX)), "-opreturnreindex");
    BOOST_CHECK_EQUAL(std::string(OptionAlias(Option::INDEX)), "-fextindex");
    BOOST_CHECK_EQUAL(std::string(OptionAlias(Option::MAX_SIZE)), "-fextmaxsize");
    BOOST_CHECK_EQUAL(std::string(OptionAlias(Option::REINDEX)), "-fextreindex");
    BOOST_CHECK_EQUAL(std::string(INDEX_DIR_NAME), "opreturn");
    BOOST_CHECK_EQUAL(std::string(LOG_PREFIX), "OP_RETURN index:");
    BOOST_CHECK_EQUAL(std::string(RECORD_NOUN), "the OP_RETURN index");
    // The thread's name, after the tree's "b-", fits the 15 characters Linux keeps.
    BOOST_CHECK_EQUAL(std::string(WORKER_THREAD_NAME), "opret-worker");
    BOOST_CHECK_LE(std::strlen(WORKER_THREAD_NAME) + 2, 15U);
}

BOOST_AUTO_TEST_CASE(record_node_programs)
{
    // ferrited and ferrite-qt keep no record unless its option is set, with or without a wallet.
    for (const NodeFacts& facts : {NodeFacts{}, WithoutWallet()}) {
        Result r = Resolve({}, false, facts);
        BOOST_CHECK(r.ok && !r.options.record && !r.options.reindex);
        BOOST_CHECK_EQUAL(r.options.max_size_bytes, 0U);
        BOOST_CHECK_EQUAL(r.options.explicit_options, 0);
        BOOST_CHECK(r.log.empty());
        for (const char* name : {OptionName(Option::INDEX), OptionAlias(Option::INDEX)}) {
            BOOST_CHECK(Resolve({Set(name, "1")}, false, facts).options.record);
            BOOST_CHECK(!Resolve({Set(name, "0")}, false, facts).options.record);
            BOOST_CHECK(!Resolve({Negated(name)}, false, facts).options.record);
            // The option is read as every boolean option of the node is.
            BOOST_CHECK(Resolve({name}, false, facts).options.record);
            BOOST_CHECK(Resolve({Set(name, "2")}, false, facts).options.record);
            BOOST_CHECK(!Resolve({Set(name, "yes")}, false, facts).options.record);
        }
    }
    // Choices stored by a GUI are not read.
    Result r = Resolve({}, false, {}, Stored(true, 200));
    BOOST_CHECK(r.ok && !r.options.record && r.options.max_size_bytes == 0);
    r = Resolve({Set(Option::INDEX, "1")}, false, {}, Stored(false, 200));
    BOOST_CHECK(r.options.record && r.options.max_size_bytes == 0);
    BOOST_CHECK(r.log.empty());
}

BOOST_AUTO_TEST_CASE(record_gui_host)
{
    // The table of the record's start value in a GUI program that shows it, under either name of the option.
    struct Row {
        const char* option; // value of the option, or null when it is not set
        bool disable_wallet;
        Optional<bool> stored;
        bool record;
    };
    for (const char* name : {OptionName(Option::INDEX), OptionAlias(Option::INDEX)}) {
        for (const Row& row : std::vector<Row>{
                 {"1", false, nullopt, true}, {"1", true, false, true}, {"1", false, false, true},
                 {"0", false, true, false}, {"0", true, nullopt, false},
                 {nullptr, true, true, false}, {nullptr, true, nullopt, false},
                 {nullptr, false, false, false},
                 {nullptr, false, true, true}, {nullptr, false, nullopt, true}}) {
            std::vector<std::string> command_line;
            if (row.option) command_line.push_back(Set(name, row.option));
            NodeFacts facts;
            facts.disable_wallet = row.disable_wallet;
            const Result r = Resolve(command_line, true, facts, Stored(row.stored));
            BOOST_CHECK(r.ok);
            BOOST_CHECK_EQUAL(r.options.record, row.record);
            BOOST_CHECK_EQUAL(r.options.explicit_options, row.option ? EXPLICIT_RECORD : 0);
        }
    }
}

BOOST_AUTO_TEST_CASE(max_size_values)
{
    struct Case {
        std::string value;
        bool ok;
        uint64_t mib;
    };
    const std::vector<Case> cases{
        {"0", true, 0}, {"1", false, 0}, {"99", false, 0}, {"100", true, 100}, {"101", true, 101},
        {"4194303", true, 4194303}, {"4194304", true, 4194304}, {"4194305", false, 0},
        {"-1", false, 0}, {"-100", false, 0}, {"9223372036854775807", false, 0},
        // Not numbers the tree's integer parser accepts.
        {"", false, 0}, {"abc", false, 0}, {" 100", false, 0}, {"100 ", false, 0}, {"1e3", false, 0},
        {"100.0", false, 0}, {"0x64", false, 0}, {"100MiB", false, 0}, {"1O0", false, 0},
        {"9223372036854775808", false, 0}, {"-9223372036854775809", false, 0},
        // Spellings that parser accepts.
        {"+100", true, 100}, {"00100", true, 100}, {"-0", true, 0},
    };
    for (const char* name : {OptionName(Option::MAX_SIZE), OptionAlias(Option::MAX_SIZE)}) {
        for (const Case& c : cases) {
            for (const bool gui_host : {false, true}) {
                // Checked whether or not the record is on; the message names the option as it was given.
                for (const std::string& index : {Set(Option::INDEX, "1"), Set(Option::INDEX, "0")}) {
                    const Result r = Resolve({index, Set(name, c.value)}, gui_host);
                    BOOST_CHECK_MESSAGE(r.ok == c.ok, std::string(name) + "='" + c.value + "'");
                    if (c.ok) {
                        BOOST_CHECK_EQUAL(r.options.max_size_bytes, c.mib * MIB);
                        BOOST_CHECK(r.error.empty());
                    } else {
                        BOOST_CHECK_EQUAL(r.error, MaxSizeError(name));
                        BOOST_CHECK(r.log.empty());
                    }
                    BOOST_CHECK(r.options.explicit_options & EXPLICIT_MAXSIZE || !r.ok);
                }
            }
        }
        // The option given with no value, negated, and negated twice.
        BOOST_CHECK(!Resolve({name}, false).ok);
        const Result negated = Resolve({Set(Option::INDEX, "1"), Negated(name)}, false, Pruned(4000 * MIB));
        BOOST_CHECK(negated.ok && negated.options.max_size_bytes == 0 && (negated.options.explicit_options & EXPLICIT_MAXSIZE));
        BOOST_CHECK(!Resolve({Negated(name) + "=0"}, false).ok);
        // The last value given counts.
        BOOST_CHECK_EQUAL(Resolve({Set(name, "abc"), Set(name, "200")}, false).options.max_size_bytes, 200 * MIB);
        BOOST_CHECK(!Resolve({Set(name, "200"), Set(name, "50")}, false).ok);
    }
    BOOST_CHECK_EQUAL(MaxSizeError(OptionName(Option::MAX_SIZE)), std::string(OptionName(Option::MAX_SIZE)) + " must be 0 (no limit) or from 100 to 4194304 MiB");
}

BOOST_AUTO_TEST_CASE(max_size_defaults)
{
    // Not pruned: no limit. Pruned: the larger of the target and 1 GiB. Manual pruning: 1 GiB.
    struct Case {
        NodeFacts facts;
        uint64_t mib;
    };
    for (const Case& c : std::vector<Case>{{NodeFacts{}, 0}, {Pruned(550 * MIB), 1024}, {Pruned(1023 * MIB), 1024},
                                           {Pruned(1024 * MIB), 1024}, {Pruned(1025 * MIB), 1025}, {Pruned(4000 * MIB), 4000},
                                           {Pruned(5000000 * MIB), 5000000},
                                           {Pruned(std::numeric_limits<uint64_t>::max()), 1024}}) {
        BOOST_CHECK_EQUAL(DefaultMaxSizeBytes(c.facts), c.mib * MIB);
        for (const bool gui_host : {false, true}) {
            const Result r = Resolve({Set(Option::INDEX, "1")}, gui_host, c.facts);
            BOOST_CHECK(r.ok);
            BOOST_CHECK_EQUAL(r.options.max_size_bytes, c.mib * MIB);
        }
    }
    // A set value counts on a pruned node too, also below the default.
    BOOST_CHECK_EQUAL(Resolve({Set(Option::MAX_SIZE, "100")}, false, Pruned(4000 * MIB)).options.max_size_bytes, 100 * MIB);
    BOOST_CHECK_EQUAL(Resolve({SetAlias(Option::MAX_SIZE, "100")}, false, Pruned(4000 * MIB)).options.max_size_bytes, 100 * MIB);
    BOOST_CHECK_EQUAL(Resolve({Set(Option::MAX_SIZE, "0")}, false, Pruned(4000 * MIB)).options.max_size_bytes, 0U);
}

BOOST_AUTO_TEST_CASE(max_size_stored)
{
    for (const NodeFacts& facts : {NodeFacts{}, WithoutWallet()}) {
        for (const int64_t mib : {int64_t{0}, int64_t{100}, int64_t{4194304}}) {
            const Result r = Resolve({}, true, facts, Stored(nullopt, mib));
            BOOST_CHECK(r.ok && r.log.empty());
            BOOST_CHECK_EQUAL(r.options.max_size_bytes, uint64_t(mib) * MIB);
        }
        // Out of range: the default, and one line.
        NodeFacts pruned = facts;
        pruned.prune_mode = true;
        pruned.prune_target_bytes = 2048 * MIB;
        for (const int64_t mib : {int64_t{1}, int64_t{99}, int64_t{4194305}, int64_t{-1}, std::numeric_limits<int64_t>::min()}) {
            const Result r = Resolve({}, true, pruned, Stored(nullopt, mib));
            BOOST_CHECK(r.ok);
            BOOST_CHECK_EQUAL(r.options.max_size_bytes, 2048 * MIB);
            BOOST_REQUIRE_EQUAL(r.log.size(), 1U);
            BOOST_CHECK(r.log[0].find(strprintf("%s the stored choice for %s is out of range and is ignored\n", LOG_PREFIX, OptionName(Option::MAX_SIZE))) != std::string::npos);
        }
    }
    // The option, under either name, wins over a stored choice, which is then not even looked at.
    for (const char* name : {OptionName(Option::MAX_SIZE), OptionAlias(Option::MAX_SIZE)}) {
        const Result option = Resolve({Set(name, "300")}, true, {}, Stored(nullopt, 99));
        BOOST_CHECK(option.ok && option.options.max_size_bytes == 300 * MIB && option.log.empty());
    }
    // ferrited reads no stored choice.
    const Result node = Resolve({}, false, {}, Stored(nullopt, 99));
    BOOST_CHECK(node.ok && node.options.max_size_bytes == 0 && node.log.empty());
}

BOOST_AUTO_TEST_CASE(reindex)
{
    for (const char* name : {OptionName(Option::REINDEX), OptionAlias(Option::REINDEX)}) {
        BOOST_CHECK(Resolve({Set(Option::INDEX, "1"), Set(name, "1")}, false).options.reindex);
        BOOST_CHECK(!Resolve({Set(Option::INDEX, "1")}, false).options.reindex);
        BOOST_CHECK(!Resolve({Set(Option::INDEX, "1"), Set(name, "0")}, false).options.reindex);
        BOOST_CHECK(!Resolve({Set(Option::INDEX, "1"), Negated(name)}, false).options.reindex);
        // Off: ignored, and the line names the option by its first name.
        const Result off = Resolve({Set(name, "1")}, false);
        BOOST_CHECK(off.ok && !off.options.reindex && Ignored(off, Option::REINDEX));
        const Result gui_off = Resolve({Set(Option::INDEX, "0"), Set(name, "1")}, true);
        BOOST_CHECK(gui_off.ok && !gui_off.options.reindex && Ignored(gui_off, Option::REINDEX));
        BOOST_CHECK(Resolve({Set(name, "1")}, true).options.reindex);
    }
}

BOOST_AUTO_TEST_CASE(explicit_bits)
{
    BOOST_CHECK_EQUAL(EXPLICIT_RECORD, 1 << uint8_t(Option::INDEX));
    BOOST_CHECK_EQUAL(EXPLICIT_MAXSIZE, 1 << uint8_t(Option::MAX_SIZE));
    BOOST_CHECK_EQUAL(EXPLICIT_REINDEX, 1 << uint8_t(Option::REINDEX));
    BOOST_CHECK_EQUAL(Resolve({}, true).options.explicit_options, 0);
    // Either name sets the bit, also to the value 0 or negated.
    for (const Setter set : SETTERS) {
        BOOST_CHECK_EQUAL(Resolve({set(Option::INDEX, "0")}, false).options.explicit_options, EXPLICIT_RECORD);
        BOOST_CHECK_EQUAL(Resolve({set(Option::MAX_SIZE, "0")}, true).options.explicit_options, EXPLICIT_MAXSIZE);
        BOOST_CHECK_EQUAL(Resolve({set(Option::REINDEX, "0")}, true).options.explicit_options, EXPLICIT_REINDEX);
        const Result all = Resolve({set(Option::INDEX, "1"), set(Option::MAX_SIZE, "0"), set(Option::REINDEX, "1")}, false);
        BOOST_CHECK_EQUAL(all.options.explicit_options, EXPLICIT_RECORD | EXPLICIT_MAXSIZE | EXPLICIT_REINDEX);
    }
    for (const Option option : OPTIONS) {
        BOOST_CHECK_EQUAL(Resolve({Negated(OptionAlias(option))}, true).options.explicit_options, 1 << uint8_t(option));
        // Both names set to one value: one bit.
        BOOST_CHECK_EQUAL(Resolve({Set(option, "0"), SetAlias(option, "0")}, true).options.explicit_options, 1 << uint8_t(option));
    }
}

BOOST_AUTO_TEST_CASE(both_names_every_place)
{
    // Each name of an option takes its value with the tree's precedence, wherever it is set. When both names have
    // a value, equal values are taken, and different values stop start-up with one message that names both. -noX
    // is the value 0. A value in the section of another network does not count.
    struct Pair {
        std::string first, second; //!< "no" stands for the negated form
        bool equal;
    };
    const std::map<Option, std::vector<Pair>> pairs{
        {Option::INDEX, {{"1", "1", true}, {"1", "0", false}, {"1", "no", false}, {"0", "no", true}, {"no", "no", true}}},
        {Option::REINDEX, {{"1", "1", true}, {"1", "0", false}, {"no", "1", false}, {"0", "no", true}}},
        {Option::MAX_SIZE, {{"200", "200", true}, {"200", "300", false}, {"100", "+100", true}, {"0", "no", true}, {"no", "200", false}}},
    };
    // What a value means: a switch's, or the size limit in MiB.
    const auto meaning = [](const std::string& value) {
        int32_t number = 0;
        if (value != "no") BOOST_REQUIRE(ParseInt32(value, &number));
        return number;
    };
    for (const Option option : OPTIONS) {
        for (const Pair& pair : pairs.at(option)) {
            for (const Place& first : PLACES) {
                for (const Place& second : PLACES) {
                    Inputs in;
                    // The record is on in every case, so that each option has its effect.
                    if (option != Option::INDEX) in.command_line.push_back(Set(Option::INDEX, "1"));
                    in.network = first.kind == Place::CONF_SECTION ? first.section : second.kind == Place::CONF_SECTION ? second.section : CBaseChainParams::MAIN;
                    in.Add(first, OptionName(option), pair.first);
                    in.Add(second, OptionAlias(option), pair.second);
                    const bool second_counts = second.kind != Place::CONF_SECTION || second.section == in.network;
                    const Result r = in.Resolve();
                    const std::string what = std::string(OptionName(option)) + "=" + pair.first + " at " + first.Name() + ", " +
                                             OptionAlias(option) + "=" + pair.second + " at " + second.Name() + " on " + in.network;
                    const bool conflict = second_counts && !pair.equal;
                    BOOST_CHECK_MESSAGE(r.ok == !conflict, what + ": " + r.error);
                    if (conflict) {
                        BOOST_CHECK_EQUAL(r.error, Conflict(option));
                        continue;
                    }
                    BOOST_CHECK_MESSAGE(r.options.explicit_options & (1 << uint8_t(option)), what);
                    // The first name always counts: the network is that of its section.
                    const int value = meaning(pair.first);
                    if (option == Option::INDEX) BOOST_CHECK_MESSAGE(r.options.record == (value != 0), what);
                    if (option == Option::REINDEX) BOOST_CHECK_MESSAGE(r.options.reindex == (value != 0), what);
                    if (option == Option::MAX_SIZE) BOOST_CHECK_MESSAGE(r.options.max_size_bytes == uint64_t(value) * MIB, what);
                }
            }
        }
    }
    // The second name alone in the section of another network: not set at all.
    Inputs in;
    in.network = CBaseChainParams::REGTEST;
    in.Add({Place::CONF_SECTION, CBaseChainParams::TESTNET}, OptionAlias(Option::INDEX), "1");
    const Result r = in.Resolve();
    BOOST_CHECK(r.ok && !r.options.record && r.options.explicit_options == 0);
}

BOOST_AUTO_TEST_CASE(both_names_compared_as_read)
{
    struct Case {
        std::vector<std::string> command_line;
        std::string conf;
        std::map<std::string, UniValue> settings;
        bool ok;
        bool record;
    };
    const std::string index = OptionName(Option::INDEX);
    const std::string alias = OptionAlias(Option::INDEX);
    const std::vector<Case> cases{
        // -noX is the value 0, under either name and wherever it is given.
        {{Negated(alias.c_str()), Set(index.c_str(), "0")}, "", {}, true, false},
        {{Negated(index.c_str()), Set(alias.c_str(), "0")}, "", {}, true, false},
        {{Negated(alias.c_str()), Set(index.c_str(), "1")}, "", {}, false, false},
        {{Set(index.c_str(), "0")}, "no" + Key(alias.c_str()) + "=1\n", {}, true, false},
        {{Set(index.c_str(), "1")}, "no" + Key(alias.c_str()) + "=1\n", {}, false, false},
        {{Set(alias.c_str(), "0")}, "", {{Key(index.c_str()), UniValue(false)}}, true, false},
        {{Set(alias.c_str(), "1")}, "", {{Key(index.c_str()), UniValue(true)}}, true, true},
        {{Set(alias.c_str(), "0")}, "", {{Key(index.c_str()), UniValue(true)}}, false, false},
        // Values are compared as the switch reads them.
        {{Set(index.c_str(), "2"), Set(alias.c_str(), "1")}, "", {}, true, true},
        {{Set(index.c_str(), "yes"), Set(alias.c_str(), "0")}, "", {}, true, false},
        {{index, Set(alias.c_str(), "1")}, "", {}, true, true},
        {{Set(index.c_str(), "1")}, "", {{Key(alias.c_str()), UniValue("1")}}, true, true},
        // Repeats: the last value of the command line counts, the first of ferrite.conf.
        {{Set(alias.c_str(), "1"), Set(alias.c_str(), "0"), Set(index.c_str(), "0")}, "", {}, true, false},
        {{Set(alias.c_str(), "0"), Set(alias.c_str(), "1"), Set(index.c_str(), "0")}, "", {}, false, false},
        {{Set(index.c_str(), "0")}, Key(alias.c_str()) + "=0\n" + Key(alias.c_str()) + "=1\n", {}, true, false},
        {{Set(index.c_str(), "1")}, Key(alias.c_str()) + "=0\n" + Key(alias.c_str()) + "=1\n", {}, false, false},
        // One name at several places: the tree's precedence decides, and nothing conflicts.
        {{Set(index.c_str(), "1")}, Key(index.c_str()) + "=0\n", {{Key(index.c_str()), UniValue(false)}}, true, true},
        {{}, Key(alias.c_str()) + "=0\n", {{Key(alias.c_str()), UniValue(true)}}, true, true},
        // The command line does not override the other name in ferrite.conf: the two must agree.
        {{Set(alias.c_str(), "0")}, Key(index.c_str()) + "=1\n", {}, false, false},
    };
    for (const Case& c : cases) {
        TestArgs t(c.command_line, c.conf, c.settings);
        const Result r = Resolve(t, false);
        BOOST_CHECK_EQUAL(r.ok, c.ok);
        if (!r.ok) {
            BOOST_CHECK_EQUAL(r.error, Conflict(Option::INDEX));
        } else {
            BOOST_CHECK_EQUAL(r.options.record, c.record);
            BOOST_CHECK_EQUAL(r.options.explicit_options, EXPLICIT_RECORD);
        }
    }

    // The size limit: compared as a number, +100, 00100, a number in the settings file and 100 being one value.
    const char* max_size = OptionName(Option::MAX_SIZE);
    const char* max_alias = OptionAlias(Option::MAX_SIZE);
    for (const std::string& spelling : {std::string{"+100"}, std::string{"00100"}, std::string{"100"}}) {
        const Result r = Resolve({Set(max_size, spelling), Set(max_alias, "100")}, false);
        BOOST_CHECK(r.ok && r.options.max_size_bytes == 100 * MIB);
        TestArgs t({Set(max_alias, spelling)}, "", {{Key(max_size), UniValue(int64_t{100})}});
        const Result s = Resolve(t, false);
        BOOST_CHECK(s.ok && s.options.max_size_bytes == 100 * MIB);
    }
    BOOST_CHECK(Resolve({Set(max_size, "-0"), Set(max_alias, "0")}, false).ok);
    BOOST_CHECK(Resolve({Negated(max_alias), Set(max_size, "0")}, false).ok);
    const Result different = Resolve({Negated(max_alias), Set(max_size, "100")}, false);
    BOOST_CHECK(!different.ok && different.error == Conflict(Option::MAX_SIZE));
    BOOST_CHECK_EQUAL(Resolve({Set(max_size, "100"), Set(max_alias, "101")}, true).error, Conflict(Option::MAX_SIZE));
    // Each value is checked before the two are compared, the first name's first; the message names the name that
    // holds the value.
    BOOST_CHECK_EQUAL(Resolve({Set(max_size, "100"), Set(max_alias, "50")}, false).error, MaxSizeError(max_alias));
    BOOST_CHECK_EQUAL(Resolve({Set(max_size, "50"), Set(max_alias, "100")}, false).error, MaxSizeError(max_size));
    BOOST_CHECK_EQUAL(Resolve({Set(max_size, "50"), Set(max_alias, "60")}, false).error, MaxSizeError(max_size));
    BOOST_CHECK_EQUAL(Resolve({Set(max_size, "50"), Set(max_alias, "50")}, false).error, MaxSizeError(max_size));
    // The conflict of a switch stops start-up also while the record is off, and the size limit is checked first.
    BOOST_CHECK_EQUAL(Resolve({Set(Option::REINDEX, "1"), SetAlias(Option::REINDEX, "0")}, false).error, Conflict(Option::REINDEX));
    BOOST_CHECK_EQUAL(Resolve({Set(Option::INDEX, "1"), SetAlias(Option::INDEX, "0"), Set(max_alias, "1")}, false).error, MaxSizeError(max_alias));
}

BOOST_AUTO_TEST_CASE(both_names_further_forms)
{
    // One name set at several places, or in several forms, while the other is set: each name takes its value with
    // the tree's rules first, and then the two values must be equal.
    struct Case {
        std::string what;
        std::vector<std::string> command_line;
        std::string conf;
        std::map<std::string, UniValue> settings;
        std::string network;
        bool ok;
        bool record;
    };
    const std::string& main = CBaseChainParams::MAIN;
    const std::string& test = CBaseChainParams::TESTNET;
    const std::vector<Case> cases{
        {"the command line over ferrite.conf", {"-noopreturnindex"}, "opreturnindex=1\nfextindex=0\n", {}, main, true, false},
        {"a network section over the top", {}, "opreturnindex=0\nfextindex=1\n[test]\nopreturnindex=1\n", {}, test, true, true},
        {"the section of another network", {}, "opreturnindex=0\nfextindex=1\n[test]\nopreturnindex=1\n", {}, main, false, false},
        {"plain, then negated on the command line", {"-opreturnindex=1", "-noopreturnindex", "-fextindex=0"}, "", {}, main, true, false},
        {"negated, then plain on the command line", {"-noopreturnindex", "-opreturnindex=1", "-fextindex=0"}, "", {}, main, false, false},
        {"plain, then negated in ferrite.conf", {}, "opreturnindex=1\nnoopreturnindex=1\nfextindex=0\n", {}, main, true, false},
        {"-X= is 1", {"-opreturnindex=", "-fextindex=1"}, "", {}, main, true, true},
        {"-X= against 0", {"-opreturnindex=", "-fextindex=0"}, "", {}, main, false, false},
        {"-noX=0 is 1", {"-noopreturnindex=0", "-fextindex=1"}, "", {}, main, true, true},
        {"-noX=0 against 0", {"-nofextindex=0", "-opreturnindex=0"}, "", {}, main, false, false},
        {"a network key on its network", {}, "test.fextindex=1\n", {}, test, true, true},
        {"a network key on another network", {}, "test.fextindex=1\n", {}, main, true, false},
        {"a network key against the top", {}, "opreturnindex=0\ntest.fextindex=1\n", {}, test, false, false},
        {"a negated network key", {}, "opreturnindex=0\ntest.nofextindex=1\n", {}, test, true, false},
        {"ferrite.conf alone", {}, "opreturnindex=1\nfextindex=0\n", {}, main, false, false},
        {"a null in the settings file hides ferrite.conf's value", {}, "opreturnindex=1\nfextindex=0\n", {{"opreturnindex", UniValue()}}, main, true, false},
    };
    for (const Case& c : cases) {
        TestArgs t(c.command_line, c.conf, c.settings, c.network);
        const Result r = Resolve(t, false);
        BOOST_CHECK_MESSAGE(r.ok == c.ok, c.what + ": " + r.error);
        if (c.ok) {
            BOOST_CHECK_MESSAGE(r.options.record == c.record, c.what);
        } else {
            BOOST_CHECK_EQUAL(r.error, Conflict(Option::INDEX));
        }
    }
    // The settings file's true for the size limit is the number 1, as the tree reads it.
    TestArgs t({}, "", {{Key(OptionAlias(Option::MAX_SIZE)), UniValue(true)}});
    BOOST_CHECK_EQUAL(Resolve(t, false).error, MaxSizeError(OptionAlias(Option::MAX_SIZE)));

    // A file that ferrite.conf includes counts as ferrite.conf.
    const fs::path conf = GetDataDir(false) / "options.conf";
    const fs::path included = GetDataDir(false) / "options-included.conf";
    for (const bool equal : {true, false}) {
        {
            fsbridge::ofstream file(conf);
            file << "includeconf=" << included.string() << "\n" << Key(OptionName(Option::INDEX)) << "=1\n";
            fsbridge::ofstream other(included);
            other << Key(OptionAlias(Option::INDEX)) << (equal ? "=1\n" : "=0\n");
        }
        ConfArgs args;
        for (const Option option : OPTIONS) {
            args.AddArg(OptionName(option), "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
            args.AddArg(OptionAlias(option), "", ArgsManager::ALLOW_ANY, OptionsCategory::HIDDEN);
        }
        args.AddArg("-conf=<file>", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        args.AddArg("-includeconf=<file>", "", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
        const std::string arg = "-conf=" + conf.string();
        const char* argv[]{"ferrite-qt-fext", arg.c_str()};
        std::string error;
        BOOST_REQUIRE_MESSAGE(args.ParseParameters(2, argv, error), error);
        BOOST_REQUIRE_MESSAGE(args.ReadConfigFiles(error, /*ignore_invalid_keys=*/true), error);
        args.SelectConfigNetwork(CBaseChainParams::MAIN);
        Options options;
        BOOST_CHECK_EQUAL(ResolveOptions(args, {}, {}, false, options, error), equal);
        if (equal) {
            BOOST_CHECK(options.record);
        } else {
            BOOST_CHECK_EQUAL(error, Conflict(Option::INDEX));
        }
    }
}

BOOST_AUTO_TEST_CASE(ignored_options_logged)
{
    // Nothing is logged when no option is set, or when the record is on.
    BOOST_CHECK(Resolve({}, false).log.empty());
    BOOST_CHECK(Resolve({Set(Option::INDEX, "0")}, false).log.empty());
    for (const Setter set : SETTERS) {
        const std::vector<std::string> two{set(Option::MAX_SIZE, "200"), set(Option::REINDEX, "1")};
        std::vector<std::string> on{set(Option::INDEX, "1")};
        on.insert(on.end(), two.begin(), two.end());
        BOOST_CHECK(Resolve(on, false).log.empty());
        BOOST_CHECK(Resolve(on, true).log.empty());

        // One line per option that does nothing because the record is off, in a fixed order, each naming the
        // option by its first name whichever name was set.
        for (const bool gui_host : {false, true}) {
            const Result r = Resolve(two, gui_host, WithoutWallet());
            BOOST_REQUIRE_EQUAL(r.log.size(), 2U);
            BOOST_CHECK(Ignored(r, Option::MAX_SIZE) && Ignored(r, Option::REINDEX));
            BOOST_CHECK(r.log[0].find(OptionName(Option::MAX_SIZE)) != std::string::npos);
            BOOST_CHECK(r.log[1].find(OptionName(Option::REINDEX)) != std::string::npos);
            for (const std::string& line : r.log) BOOST_CHECK(line.find("-fext") == std::string::npos);
        }
        // With the tab, the record can still be switched on, and then the size limit applies.
        std::vector<std::string> gui_off{set(Option::INDEX, "0")};
        gui_off.insert(gui_off.end(), two.begin(), two.end());
        const Result tab = Resolve(gui_off, true);
        BOOST_REQUIRE_EQUAL(tab.log.size(), 1U);
        BOOST_CHECK(Ignored(tab, Option::REINDEX));
        BOOST_CHECK_EQUAL(tab.options.max_size_bytes, 200 * MIB);
        // An option set to its default value is still reported: it is set, and has no effect.
        BOOST_CHECK(Ignored(Resolve({set(Option::REINDEX, "0")}, false), Option::REINDEX));
    }
    // Both names set to one value: one line.
    const Result both = Resolve({Set(Option::REINDEX, "1"), SetAlias(Option::REINDEX, "1")}, false);
    BOOST_CHECK(both.ok && both.log.size() == 1 && Ignored(both, Option::REINDEX));
}

BOOST_AUTO_TEST_CASE(unreadable_settings)
{
    // The settings file can hold a value that is neither text nor a boolean. The size limit takes a whole number,
    // as the tree's integer options do, within the same bounds as text; any other such value fails start-up with a
    // fixed message that names the option as the file names it, and never throws.
    for (const char* name : {OptionName(Option::MAX_SIZE), OptionAlias(Option::MAX_SIZE)}) {
        const auto max_size = [&](const UniValue& value) {
            TestArgs t({Set(Option::INDEX, "1")}, "", {{Key(name), value}});
            Result r;
            BOOST_CHECK_NO_THROW(r = Resolve(t, false));
            return r;
        };
        for (const int64_t mib : {int64_t{0}, int64_t{100}, int64_t{4194304}}) {
            const Result r = max_size(UniValue(mib));
            BOOST_CHECK(r.ok);
            BOOST_CHECK_EQUAL(r.options.max_size_bytes, uint64_t(mib) * MIB);
        }
        for (const int64_t mib : {int64_t{-1}, int64_t{1}, int64_t{99}, int64_t{4194305}}) {
            const Result r = max_size(UniValue(mib));
            BOOST_CHECK(!r.ok);
            BOOST_CHECK_EQUAL(r.error, MaxSizeError(name));
        }
        UniValue fraction;
        BOOST_REQUIRE(fraction.setNumStr("100.5"));
        for (const UniValue& value : {fraction, UniValue(UniValue::VARR), UniValue(UniValue::VOBJ)}) {
            const Result r = max_size(value);
            BOOST_CHECK(!r.ok);
            BOOST_CHECK_EQUAL(r.error, Unreadable(name));
        }
    }
    // A switch's value of another type fails start-up too, under either name, whether or not the record is on, and
    // also when the switch's other name holds a value that can be read.
    for (const Option option : {Option::INDEX, Option::REINDEX}) {
        for (const char* name : {OptionName(option), OptionAlias(option)}) {
            for (const std::string& other : {std::string{}, std::string{"1"}}) {
                std::vector<std::string> command_line;
                const bool first = std::string(name) == OptionName(option);
                if (!other.empty()) command_line.push_back(Set(first ? OptionAlias(option) : OptionName(option), other));
                for (const UniValue& value : {UniValue(UniValue::VARR), UniValue(int64_t{1})}) {
                    TestArgs t(command_line, "", {{Key(name), value}});
                    Result r;
                    BOOST_CHECK_NO_THROW(r = Resolve(t, false));
                    BOOST_CHECK(!r.ok);
                    BOOST_CHECK_EQUAL(r.error, Unreadable(name));
                }
            }
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
