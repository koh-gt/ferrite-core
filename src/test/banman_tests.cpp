// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addrdb.h>
#include <banman.h>
#include <fs.h>
#include <netaddress.h>
#include <node/ui_interface.h>
#include <util/system.h>
#include <util/time.h>

#include <test/util/setup_common.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

namespace {

CSubNet TestSubNet(uint32_t n)
{
    struct in_addr s;
    s.s_addr = htonl(0x0a000000 + n); // 10.0.0.0/8
    return CSubNet(CNetAddr(s));
}

//! Whether a write of the ban list is under way: the list is written to a
//! temporary file "banlist.<4 hex digits>" that is then renamed into place.
bool BanListWriteInProgress()
{
    for (fs::directory_iterator it(GetDataDir()); it != fs::directory_iterator(); ++it) {
        const std::string name = it->path().filename().string();
        if (name.size() == 12 && name.compare(0, 8, "banlist.") == 0) return true;
    }
    return false;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(banman_tests, BasicTestingSetup)

// A ban that is added while another thread is writing the ban list to disk
// must be on disk when the call that added it returns.
BOOST_AUTO_TEST_CASE(ban_added_during_dump_is_written)
{
    const fs::path ban_path = GetDataDir() / "banlist.dat";

    // Start from a long ban list, so that writing it takes a while.
    const uint32_t INITIAL_BANS = 50000;
    {
        banmap_t initial;
        CBanEntry entry(GetTime());
        entry.nBanUntil = GetTime() + DEFAULT_MISBEHAVING_BANTIME;
        for (uint32_t i = 0; i < INITIAL_BANS; ++i) {
            initial[TestSubNet(i)] = entry;
        }
        BOOST_REQUIRE(CBanDB(ban_path).Write(initial));
    }
    BOOST_REQUIRE(!BanListWriteInProgress());

    BanMan banman(ban_path, &uiInterface, DEFAULT_MISBEHAVING_BANTIME);

    // BannedListChanged is signalled after a ban is added to the list in
    // memory and before the list is written. The handler holds back this
    // thread at that point until the other thread has finished its write.
    const std::thread::id main_thread = std::this_thread::get_id();
    std::thread writer;
    bool added_during_write = false;
    boost::signals2::scoped_connection handler = uiInterface.BannedListChanged_connect([&] {
        if (std::this_thread::get_id() != main_thread) return;
        added_during_write = BanListWriteInProgress();
        // The bans in this test must not expire while it runs: an expired
        // ban is signalled from inside the write of the ban list, and
        // waiting for the writer there would never return.
        writer.join();
    });

    // The temporary file exists from after the writer has copied the list
    // until its write is complete. If it is there both before and after this
    // thread adds its ban, the ban was added during the writer's write, after
    // the writer copied the list. Try again in the unlikely case that the
    // write was over too soon.
    uint32_t next = INITIAL_BANS;
    for (int attempt = 0; attempt < 10 && !added_during_write; ++attempt) {
        std::atomic<bool> writer_done{false};
        const CSubNet writer_ban = TestSubNet(next++);
        writer = std::thread([&] {
            banman.Ban(writer_ban);
            writer_done = true;
        });
        while (!BanListWriteInProgress() && !writer_done) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        banman.Ban(TestSubNet(next++));
        // The handler has waited for the writer.
        const bool writer_joined = !writer.joinable();
        if (!writer_joined) writer.join();
        BOOST_REQUIRE(writer_joined);
    }
    handler.disconnect();
    BOOST_REQUIRE(added_during_write);

    // Every ban, including the last one, is in the file.
    banmap_t on_disk;
    BOOST_REQUIRE(CBanDB(ban_path).Read(on_disk));
    BOOST_CHECK_EQUAL(on_disk.size(), next);
    for (uint32_t i = INITIAL_BANS; i < next; ++i) {
        BOOST_CHECK_MESSAGE(on_disk.count(TestSubNet(i)) == 1, "ban " << i - INITIAL_BANS << " is missing from the ban list file");
    }
}

BOOST_AUTO_TEST_SUITE_END()
