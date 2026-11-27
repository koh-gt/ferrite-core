// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/node.h>

#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/opreturn.h>
#include <key.h>
#include <node/context.h>
#include <node/ui_interface.h>
#include <opreturn/names.h>
#include <opreturn/options.h>
#include <opreturn/registry.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace opreturn;
using opreturn::test::LogLines;

namespace {

using Clock = std::chrono::steady_clock;

template <typename F>
bool WaitUntil(F condition, std::chrono::milliseconds timeout = std::chrono::milliseconds{20000})
{
    const auto end = Clock::now() + timeout;
    while (!condition()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

//! The options of a program that shows the record, as AddArgs registers them, with a command line.
struct TestArgs {
    ArgsManager args;

    explicit TestArgs(const std::vector<std::string>& command_line = {})
    {
        AddArgs(args);
        std::vector<const char*> argv{"ferrite-qt-fext"};
        for (const std::string& arg : command_line) argv.push_back(arg.c_str());
        std::string error;
        BOOST_REQUIRE_MESSAGE(args.ParseParameters(int(argv.size()), argv.data(), error), error);
    }
};

//! A program's node context with its own options, as the GUI host's hook gives it to Construct.
struct Host {
    TestArgs test_args;
    NodeContext node;

    explicit Host(const std::vector<std::string>& command_line = {}, bool disable_wallet = false) : test_args(command_line)
    {
        if (disable_wallet) test_args.args.ForceSetArg("-disablewallet", "1");
        node.args = &test_args.args;
    }
    ~Host()
    {
        // As Shutdown does: flush and stop every client, then let go of them.
        for (const auto& client : node.chain_clients) {
            client->flush();
            client->stop();
        }
        node.chain_clients.clear();
        node.args = nullptr;
    }
    interfaces::ChainClient& Client()
    {
        BOOST_REQUIRE_EQUAL(node.chain_clients.size(), 1U);
        return *node.chain_clients.back();
    }
};

//! Sets the program a GUI host for one test.
struct GuiHost {
    explicit GuiHost(bool gui_host = true) { SetGuiHost(gui_host); }
    ~GuiHost() { SetGuiHost(false); }
};

std::string Set(Option option, const std::string& value) { return std::string(OptionName(option)) + "=" + value; }
std::string SetAlias(Option option, const std::string& value) { return std::string(OptionAlias(option)) + "=" + value; }

//! Whether the record is kept: until it reads older blocks, a new record covers only the blocks connected after its
//! start, so it is BACKFILLING rather than RUNNING.
bool Kept(const std::shared_ptr<interfaces::OpReturnRecord>& record)
{
    if (!record) return false;
    const State state = record->getStatus().state;
    return state == State::RUNNING || state == State::BACKFILLING;
}

CScript OpReturn(const std::string& text)
{
    return CScript() << OP_RETURN << std::vector<unsigned char>(text.begin(), text.end());
}

constexpr uint64_t MIB{1024 * 1024};

//! The texts InitError shows, as the GUI's message box gets them, while it exists.
class MessageBoxes
{
    boost::signals2::connection m_connection;

public:
    std::vector<std::string> texts;

    MessageBoxes()
    {
        m_connection = uiInterface.ThreadSafeMessageBox_connect([this](const bilingual_str& message, const std::string&, unsigned int) {
            texts.push_back(message.original);
            return false;
        });
    }
    ~MessageBoxes() { m_connection.disconnect(); }
};

/**
 * The prune mode and target as the node's base initialization sets them from -prune=<mib> (AppInitParameterInteraction
 * in src/init.cpp), before the GUI host's hook, until the end of the scope. This test program cannot start its node
 * with -prune: its earlier set-ups have enabled the compact filters of the -blockfilterindex default, which init
 * then finds enabled and does not allow with -prune.
 */
class Pruning
{
    const bool m_mode{fPruneMode};
    const uint64_t m_target{nPruneTarget};

public:
    explicit Pruning(int64_t mib)
    {
        nPruneTarget = mib == 1 ? std::numeric_limits<uint64_t>::max() : uint64_t(mib) * MIB;
        fPruneMode = true;
    }
    ~Pruning()
    {
        fPruneMode = m_mode;
        nPruneTarget = m_target;
    }
};

//! The facts Construct reads on a pruned node, the default size limit they give, and the record's pruned flag.
void CheckPruned(int64_t mib, uint64_t target_bytes, uint64_t default_bytes)
{
    const Pruning pruning(mib);
    const GuiHost gui_host;
    Host host;
    const NodeFacts facts = ReadNodeFacts(*host.node.args);
    BOOST_CHECK(facts.prune_mode);
    BOOST_CHECK_EQUAL(facts.prune_target_bytes, target_bytes);
    BOOST_CHECK_EQUAL(DefaultMaxSizeBytes(facts), default_bytes);
    BOOST_CHECK(Construct(host.node, {}));
    BOOST_REQUIRE(GetRecord());
    BOOST_CHECK(GetRecord()->getStatus().pruned);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(opreturn_node_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(options_registered)
{
    // The options, in the help's first group "Options:", with help texts that speak of the tab only; their second
    // names work, and -help does not show them, also not with -help-debug.
    ArgsManager args;
    AddArgs(args);
    args.AddArg("-help-debug", "", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);
    std::string error;
    const char* debug[]{"ferrite-qt-fext", "-help-debug"};
    BOOST_REQUIRE(args.ParseParameters(2, debug, error));
    const std::string help = args.GetHelpMessage();
    BOOST_REQUIRE(help.compare(0, 10, "Options:\n\n") == 0);
    // The group ends where the next one begins: at an empty line followed by a line that is not indented.
    size_t end = 0;
    while ((end = help.find("\n\n", end + 1)) != std::string::npos && end + 2 < help.size() && help[end + 2] == ' ') {}
    const std::string group = help.substr(0, end);
    for (const std::string& name : {std::string(OptionName(Option::INDEX)), std::string(OptionName(Option::MAX_SIZE)) + "=<n>",
                                    std::string(OptionName(Option::REINDEX))}) {
        BOOST_CHECK_MESSAGE(group.find("\n  " + name + "\n") != std::string::npos, name);
    }
    // The texts, with the help's line breaks and indents made single spaces.
    std::string text;
    for (const char c : help) {
        if (c != ' ' && c != '\n') {
            text += c;
        } else if (!text.empty() && text.back() != ' ') {
            text += ' ';
        }
    }
    BOOST_CHECK(text.find("-fext") == std::string::npos);
    BOOST_CHECK(text.find(std::string("Keep ") + RECORD_NOUN + ": every OP_RETURN output of the chain, for the FEXT tab.") != std::string::npos);
    BOOST_CHECK(text.find(std::string("Keep ") + RECORD_NOUN + " under <n> MiB") != std::string::npos);
    BOOST_CHECK(text.find(std::string("Delete ") + RECORD_NOUN + " at start") != std::string::npos);
    BOOST_CHECK(text.find("(default: 1; 0 with -disablewallet)") != std::string::npos);
    BOOST_CHECK(text.find("from 100 to 4194304") != std::string::npos);
    for (const char* word : {"rpc", "RPC", "getopreturn", "listopreturns"}) BOOST_CHECK(text.find(word) == std::string::npos);

    // Every name of every option is accepted.
    for (const Option option : {Option::INDEX, Option::MAX_SIZE, Option::REINDEX}) {
        for (const char* name : {OptionName(option), OptionAlias(option)}) {
            ArgsManager a;
            AddArgs(a);
            const std::string arg = std::string(name) + (option == Option::MAX_SIZE ? "=100" : "=1");
            const char* argv[]{"ferrite-qt-fext", arg.c_str()};
            BOOST_CHECK_MESSAGE(a.ParseParameters(2, argv, error), arg + ": " + error);
        }
    }
}

BOOST_AUTO_TEST_CASE(gui_host_always_constructs)
{
    // A GUI host gets the chain client also while the record is off, so that the tab can switch it on; the record is
    // on by default, also when only the second name of a switch is set.
    const GuiHost gui_host;
    {
        Host host({Set(Option::INDEX, "0")});
        BOOST_CHECK(Construct(host.node, {}));
        interfaces::ChainClient& client = host.Client();
        const std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
        BOOST_REQUIRE(record);
        BOOST_CHECK(client.verify());
        BOOST_CHECK(client.load());
        client.start(*m_node.scheduler);
        BOOST_CHECK(record->getStatus().state == State::OFF);
        BOOST_CHECK_EQUAL(record->getStatus().explicit_options, EXPLICIT_RECORD);
        // Switched on by the tab.
        record->setRecordEnabled(true);
        BOOST_CHECK(WaitUntil([&] { return Kept(record); }));
    }
    {
        // The default, and the tab's stored choice.
        Host host;
        BOOST_CHECK(Construct(host.node, {}));
        host.Client().load();
        BOOST_CHECK(WaitUntil([&] { return Kept(GetRecord()); }));
    }
    {
        Host host;
        StoredChoices stored;
        stored.record = false;
        BOOST_CHECK(Construct(host.node, stored));
        host.Client().load();
        BOOST_CHECK(GetRecord()->getStatus().state == State::OFF);
    }
    {
        Host host({SetAlias(Option::INDEX, "0")});
        BOOST_CHECK(Construct(host.node, {}));
        host.Client().load();
        BOOST_CHECK(GetRecord()->getStatus().state == State::OFF);
        BOOST_CHECK_EQUAL(GetRecord()->getStatus().explicit_options, EXPLICIT_RECORD);
    }
    BOOST_CHECK(!GetRecord());
}

BOOST_AUTO_TEST_CASE(node_program_constructs_when_on)
{
    // Without SetGuiHost the record is off unless its option is set, and then nothing is made at all.
    {
        Host host;
        BOOST_CHECK(Construct(host.node, {}));
        BOOST_CHECK(host.node.chain_clients.empty());
        BOOST_CHECK(!GetRecord());
    }
    for (const std::string& arg : {Set(Option::INDEX, "1"), SetAlias(Option::INDEX, "1")}) {
        Host host({arg});
        BOOST_CHECK(Construct(host.node, {}));
        BOOST_REQUIRE(GetRecord());
        BOOST_CHECK(host.Client().verify());
        host.Client().load();
        BOOST_CHECK(WaitUntil([&] { return Kept(GetRecord()); }));
    }
}

BOOST_AUTO_TEST_CASE(bad_values_stop_start)
{
    // A value that may not be set stops start-up at verify(), with one error that names the option as it was given;
    // such a client holds no record, and flush() and stop() still work, as Shutdown calls them.
    const GuiHost gui_host;
    struct Case {
        std::vector<std::string> command_line;
        std::string error;
    };
    const std::string conflict = strprintf("%s is another name for %s, and the two are set to different values: remove %s",
                                           OptionAlias(Option::INDEX), OptionName(Option::INDEX), OptionAlias(Option::INDEX));
    for (const Case& c : std::vector<Case>{
             {{Set(Option::MAX_SIZE, "50")}, strprintf("%s must be 0 (no limit) or from 100 to 4194304 MiB", OptionName(Option::MAX_SIZE))},
             {{SetAlias(Option::MAX_SIZE, "4194305")}, strprintf("%s must be 0 (no limit) or from 100 to 4194304 MiB", OptionAlias(Option::MAX_SIZE))},
             {{Set(Option::INDEX, "0"), Set(Option::MAX_SIZE, "1")}, strprintf("%s must be 0 (no limit) or from 100 to 4194304 MiB", OptionName(Option::MAX_SIZE))},
             {{Set(Option::INDEX, "1"), SetAlias(Option::INDEX, "0")}, conflict},
         }) {
        Host host(c.command_line);
        BOOST_CHECK(Construct(host.node, {}));
        BOOST_CHECK(!GetRecord());
        const LogLines lines;
        const MessageBoxes boxes;
        BOOST_CHECK(!host.Client().verify());
        // The message box shows the message as it is, and the log line is "Error: " and the message.
        BOOST_REQUIRE_EQUAL(boxes.texts.size(), 1U);
        BOOST_CHECK_EQUAL(boxes.texts[0], c.error);
        const std::vector<std::string> logged = lines.Containing(c.error);
        BOOST_REQUIRE_EQUAL(logged.size(), 1U);
        const std::string tail = "] Error: " + c.error + "\n";
        BOOST_CHECK(logged[0].size() >= tail.size() && logged[0].compare(logged[0].size() - tail.size(), tail.size(), tail) == 0);
    }
    // Good values pass verify().
    Host host({Set(Option::MAX_SIZE, "100"), SetAlias(Option::MAX_SIZE, "+100")});
    BOOST_CHECK(Construct(host.node, {}));
    BOOST_CHECK(host.Client().verify());
}

BOOST_AUTO_TEST_CASE(init_order)
{
    // Driven as init drives it (verify, load, start, then flush and stop), the record follows the chain in its own
    // directory, indexes/opreturn, and stops.
    const GuiHost gui_host;
    Host host({Set(Option::INDEX, "1")});
    const fs::path dir = GetDataDir() / "indexes" / INDEX_DIR_NAME;
    BOOST_CHECK(Construct(host.node, {}));
    BOOST_CHECK(!fs::exists(dir));
    interfaces::ChainClient& client = host.Client();
    const std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
    BOOST_REQUIRE(record);
    BOOST_CHECK(client.verify());
    BOOST_CHECK(client.load());
    BOOST_CHECK(fs::is_directory(dir));
    client.start(*m_node.scheduler);
    BOOST_CHECK(record->getStatus().node_known);
    const CBlock block = CreateAndProcessBlock({}, OpReturn("init order"));
    const int height = WITH_LOCK(cs_main, return ::ChainActive().Height());
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK(WaitUntil([&] { return record->getVisibility()->live_tip == height; }));
    RowView row;
    BOOST_CHECK(record->readRow({height, 0}, row, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(row.tx.txid == block.vtx[0]->GetHash());
    client.flush();
    client.stop();
    BOOST_CHECK(record->getStatus().state == State::STOPPED);
    BOOST_CHECK(record->readRow({height, 0}, row, nullptr) == ReadResult::NOT_FOUND);
    // Nothing of the record lies elsewhere in the data directory.
    for (const char* other : {"fext", "record"}) BOOST_CHECK(!fs::exists(GetDataDir() / "indexes" / other));
}

BOOST_AUTO_TEST_CASE(disablewallet)
{
    // Without a wallet a GUI host still gets the client, but the record is off unless its option is set, under
    // either name; a stored choice is not read.
    const GuiHost gui_host;
    for (const bool stored_on : {false, true}) {
        Host host({}, /*disable_wallet=*/true);
        StoredChoices stored;
        stored.record = stored_on;
        BOOST_CHECK(Construct(host.node, stored));
        host.Client().load();
        BOOST_REQUIRE(GetRecord());
        BOOST_CHECK(GetRecord()->getStatus().state == State::OFF);
    }
    for (const std::string& arg : {Set(Option::INDEX, "1"), SetAlias(Option::INDEX, "1")}) {
        Host host({arg}, /*disable_wallet=*/true);
        BOOST_CHECK(Construct(host.node, {}));
        host.Client().load();
        BOOST_CHECK(WaitUntil([&] { return Kept(GetRecord()); }));
    }
    // An option set to no effect is logged by its first name, whichever name was set.
    const LogLines lines;
    Host host({SetAlias(Option::REINDEX, "1")}, /*disable_wallet=*/true);
    BOOST_CHECK(Construct(host.node, {}));
    BOOST_CHECK_EQUAL(lines.Containing(strprintf("%s off, so %s is ignored", LOG_PREFIX, OptionName(Option::REINDEX))).size(), 1U);
}

BOOST_AUTO_TEST_CASE(construct_needs_arguments)
{
    // Without the node's arguments Construct makes nothing and says so; it never throws. (The hook that calls it, and
    // a failure of that hook, are tested with the program that has the hook.)
    const GuiHost gui_host;
    Host host;
    host.node.args = nullptr;
    BOOST_CHECK(!Construct(host.node, {}));
    BOOST_CHECK(host.node.chain_clients.empty());
    BOOST_CHECK(!GetRecord());
}

BOOST_AUTO_TEST_CASE(release_under_cs_main)
{
    // Letting go of the client neither stops the record nor waits for its worker, here on a thread that holds
    // cs_main: stopping waits for the worker, which may be waiting for cs_main, so only Shutdown stops the record,
    // outside cs_main. The registry holds the record only weakly: once nothing else holds it, it is gone.
    const GuiHost gui_host;
    std::weak_ptr<interfaces::OpReturnRecord> weak;
    {
        // A reader holds the record too.
        Host host({Set(Option::INDEX, "1")});
        BOOST_CHECK(Construct(host.node, {}));
        host.Client().load();
        host.Client().start(*m_node.scheduler);
        std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
        BOOST_REQUIRE(record);
        BOOST_CHECK(WaitUntil([&] { return Kept(record); }));
        weak = record;
        {
            LOCK(cs_main);
            const auto begun = Clock::now();
            host.node.chain_clients.clear();
            BOOST_CHECK(Clock::now() - begun < std::chrono::milliseconds{500});
            BOOST_CHECK(Kept(record));
        }
        // This test holds it now; let go, its worker ends it.
        record.reset();
    }
    BOOST_CHECK(WaitUntil([&] { return weak.expired(); }));
    BOOST_CHECK(!GetRecord());
    for (const bool on : {true, false}) {
        // The client is the only owner: of a record that runs, its worker then ends it; of one that is off, which no
        // worker holds, it goes at once.
        Host host({Set(Option::INDEX, on ? "1" : "0")});
        BOOST_CHECK(Construct(host.node, {}));
        host.Client().load();
        host.Client().start(*m_node.scheduler);
        weak = GetRecord();
        BOOST_CHECK(WaitUntil([&] {
            const std::shared_ptr<interfaces::OpReturnRecord> record = weak.lock();
            return record && (on ? Kept(record) : record->getStatus().state == State::OFF);
        }));
        {
            LOCK(cs_main);
            const auto begun = Clock::now();
            host.node.chain_clients.clear();
            BOOST_CHECK(Clock::now() - begun < std::chrono::milliseconds{500});
            if (!on) BOOST_CHECK(weak.expired());
        }
        BOOST_CHECK(WaitUntil([&] { return weak.expired(); }));
    }
}

BOOST_AUTO_TEST_CASE(switch_before_load)
{
    // The tab holds the record from the hook on, before init has loaded it: a switch or a rebuild asked then does
    // nothing, and load() starts the record as its options say; from then on the switches work.
    const GuiHost gui_host;
    {
        Host host({Set(Option::INDEX, "0")});
        BOOST_CHECK(Construct(host.node, {}));
        const std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
        BOOST_REQUIRE(record);
        record->setRecordEnabled(true);
        record->requestRebuild();
        // Nothing has started: no worker opens the record's files.
        std::this_thread::sleep_for(std::chrono::milliseconds{500});
        BOOST_CHECK(record->getStatus().state == State::OFF);
        BOOST_CHECK(!fs::exists(GetDataDir() / "indexes" / INDEX_DIR_NAME));
        interfaces::ChainClient& client = host.Client();
        BOOST_CHECK(client.verify());
        BOOST_CHECK(client.load());
        client.start(*m_node.scheduler);
        BOOST_CHECK(record->getStatus().state == State::OFF);
        record->setRecordEnabled(true);
        BOOST_CHECK(WaitUntil([&] { return Kept(record); }, std::chrono::milliseconds{5000}));
    }
    {
        Host host({Set(Option::INDEX, "1")});
        BOOST_CHECK(Construct(host.node, {}));
        const std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
        BOOST_REQUIRE(record);
        record->requestRebuild();
        record->setRecordEnabled(false);
        interfaces::ChainClient& client = host.Client();
        BOOST_CHECK(client.load());
        client.start(*m_node.scheduler);
        BOOST_CHECK(WaitUntil([&] { return Kept(record); }, std::chrono::milliseconds{5000}));
        CreateAndProcessBlock({}, OpReturn("switch before load"));
        const int height = WITH_LOCK(cs_main, return ::ChainActive().Height());
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK(WaitUntil([&] { return record->getVisibility()->live_tip == height; }, std::chrono::milliseconds{5000}));
        BOOST_CHECK(record->getStatus().state != State::FAILED);
    }
}

BOOST_AUTO_TEST_CASE(flush_reaches_the_record)
{
    // The client's flush() reaches the record, which then reads no more undo data: the coins that a transaction of a
    // later block spends stay to be looked up, while blocks are still written.
    const GuiHost gui_host;
    Host host({Set(Option::INDEX, "1")});
    BOOST_CHECK(Construct(host.node, {}));
    interfaces::ChainClient& client = host.Client();
    const std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
    BOOST_REQUIRE(record);
    BOOST_CHECK(client.verify());
    BOOST_CHECK(client.load());
    client.start(*m_node.scheduler);
    const CScript coinbase_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    // A transaction spending a mature coinbase, with an OP_RETURN output.
    const auto message = [&](size_t coin, const std::string& text) {
        const CTransactionRef& spent = m_coinbase_txns[coin];
        CMutableTransaction tx;
        tx.vin.emplace_back(COutPoint(spent->GetHash(), 0));
        tx.vout.emplace_back(0, OpReturn(text));
        tx.vout.emplace_back(spent->vout[0].nValue - 10000, coinbase_script);
        const uint256 hash = SignatureHash(spent->vout[0].scriptPubKey, tx, 0, SIGHASH_ALL, spent->vout[0].nValue, SigVersion::BASE);
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(coinbaseKey.Sign(hash, signature));
        signature.push_back(uint8_t(SIGHASH_ALL));
        tx.vin[0].scriptSig = CScript() << signature;
        return tx;
    };
    const auto mine = [&](const CMutableTransaction& tx) {
        CreateAndProcessBlock({tx}, coinbase_script);
        const int height = WITH_LOCK(cs_main, return ::ChainActive().Height());
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE(WaitUntil([&] { return record->getVisibility()->live_tip == height; }));
        return height;
    };
    const auto paid_by = [&](int height) {
        RowView row;
        BOOST_REQUIRE(record->readRow({height, 0}, row, nullptr) == ReadResult::FOUND);
        return row.tx.paid_by_kind;
    };
    // Before the flush the worker looks the coin up.
    const int before = mine(message(0, "before the flush"));
    BOOST_CHECK(WaitUntil([&] { return paid_by(before) == PaidByKind::SCRIPT; }));
    client.flush();
    const int after = mine(message(1, "after the flush"));
    std::this_thread::sleep_for(std::chrono::milliseconds{1500});
    BOOST_CHECK(paid_by(after) == PaidByKind::PENDING);
}

BOOST_AUTO_TEST_CASE(partial_start)
{
    // Shutdown flushes and stops every client also when start-up ended early: before load(), or between load() and
    // start(); and stop() may come twice. The record then ends stopped, with no worker left and nothing opened
    // before load().
    const GuiHost gui_host;
    const fs::path dir = GetDataDir() / "indexes" / INDEX_DIR_NAME;
    std::weak_ptr<interfaces::OpReturnRecord> weak;
    {
        Host host({Set(Option::INDEX, "1")});
        BOOST_CHECK(Construct(host.node, {}));
        weak = GetRecord();
        host.Client().flush();
        host.Client().stop();
        BOOST_CHECK(weak.lock()->getStatus().state == State::STOPPED);
        BOOST_CHECK(!fs::exists(dir));
    }
    BOOST_CHECK(weak.expired());
    {
        Host host({Set(Option::INDEX, "1")});
        BOOST_CHECK(Construct(host.node, {}));
        weak = GetRecord();
        BOOST_CHECK(host.Client().verify());
        BOOST_CHECK(host.Client().load());
        BOOST_CHECK(WaitUntil([&] { return Kept(weak.lock()); }));
        host.Client().flush();
        host.Client().stop();
        BOOST_CHECK(weak.lock()->getStatus().state == State::STOPPED);
    }
    BOOST_CHECK(weak.expired());
    for (const bool on : {true, false}) {
        Host host({Set(Option::INDEX, on ? "1" : "0")});
        BOOST_CHECK(Construct(host.node, {}));
        weak = GetRecord();
        BOOST_CHECK(host.Client().verify());
        BOOST_CHECK(host.Client().load());
        host.Client().start(*m_node.scheduler);
        host.Client().stop();
        host.Client().stop();
        BOOST_CHECK(weak.lock()->getStatus().state == State::STOPPED);
        // The Host flushes and stops once more, as Shutdown would.
    }
    BOOST_CHECK(weak.expired());
}

BOOST_AUTO_TEST_CASE(reader_outlives_client)
{
    // A reader that holds the record across flush(), stop() and the end of its client can still call every method:
    // the record is stopped, reads find nothing, switches do nothing, and it goes when the reader lets go of it.
    const GuiHost gui_host;
    Host host({Set(Option::INDEX, "1")});
    BOOST_CHECK(Construct(host.node, {}));
    std::shared_ptr<interfaces::OpReturnRecord> record = GetRecord();
    BOOST_REQUIRE(record);
    host.Client().load();
    host.Client().start(*m_node.scheduler);
    const CBlock block = CreateAndProcessBlock({}, OpReturn("read after stop"));
    const int height = WITH_LOCK(cs_main, return ::ChainActive().Height());
    SyncWithValidationInterfaceQueue();
    BOOST_REQUIRE(WaitUntil([&] { return record->getVisibility()->live_tip == height; }));
    RowView row;
    BOOST_REQUIRE(record->readRow({height, 0}, row, nullptr) == ReadResult::FOUND);
    host.Client().flush();
    host.Client().stop();
    host.node.chain_clients.clear();

    BOOST_CHECK(record->getStatus().state == State::STOPPED);
    const VisibilityRef v = record->getVisibility();
    BOOST_REQUIRE(v && v->coverage);
    BOOST_CHECK(v->coverage->empty());
    record->getRemovals();
    RowRequest request;
    const RowBatch batch = record->readRows(request);
    BOOST_CHECK(batch.rows.empty() && batch.end && !batch.failed);
    BOOST_CHECK(record->readRow({height, 0}, row, nullptr) == ReadResult::NOT_FOUND);
    std::vector<unsigned char> bytes;
    BOOST_CHECK(record->readScript({height, 0}, 0, 100, bytes, nullptr) == ReadResult::NOT_FOUND);
    TxView tx;
    BOOST_CHECK(record->readTransaction({height, 0}, 100, 100, tx, nullptr) == ReadResult::NOT_FOUND);
    BlockView block_view;
    BOOST_CHECK(record->readBlock(height, block_view, nullptr) == ReadResult::NOT_FOUND);
    TxKey key;
    BOOST_CHECK(record->findTransaction(block.vtx[0]->GetHash(), key, nullptr) == ReadResult::NOT_FOUND);
    int32_t found;
    BOOST_CHECK(record->findHeightForTime(0, found, nullptr) == ReadResult::NOT_FOUND);
    BOOST_CHECK(!record->waitForTip(100));
    record->setRecordEnabled(true);
    record->requestRebuild();
    BOOST_CHECK(record->getStatus().state == State::STOPPED);
    std::unique_ptr<interfaces::Handler> handler = record->handleChanged([](const RecordChange&) {});
    BOOST_CHECK(handler);
    handler.reset();
    BOOST_CHECK(record->readRows(request).rows.empty());
    const std::weak_ptr<interfaces::OpReturnRecord> weak = record;
    record.reset();
    BOOST_CHECK(weak.expired());
}

BOOST_AUTO_TEST_CASE(facts_of_the_node)
{
    // The facts Construct reads on a node that does not prune: no default size limit, and -disablewallet.
    Host host;
    NodeFacts facts = ReadNodeFacts(*host.node.args);
    BOOST_CHECK(!facts.prune_mode && facts.prune_target_bytes == 0 && !facts.disable_wallet);
    BOOST_CHECK_EQUAL(DefaultMaxSizeBytes(facts), 0U);
    Host without_wallet({}, /*disable_wallet=*/true);
    facts = ReadNodeFacts(*without_wallet.node.args);
    BOOST_CHECK(facts.disable_wallet);
    const GuiHost gui_host;
    BOOST_CHECK(Construct(host.node, {}));
    BOOST_REQUIRE(GetRecord());
    BOOST_CHECK(!GetRecord()->getStatus().pruned);
}

BOOST_AUTO_TEST_CASE(facts_of_a_pruned_node)
{
    // -prune=550: the default size limit is 1 GiB; -prune=1 (pruning by hand): 1 GiB; -prune=2048: the target.
    CheckPruned(550, 550 * MIB, 1024 * MIB);
    CheckPruned(1, std::numeric_limits<uint64_t>::max(), 1024 * MIB);
    CheckPruned(2048, 2048 * MIB, 2048 * MIB);
    BOOST_CHECK(!fPruneMode);
}

BOOST_AUTO_TEST_SUITE_END()
