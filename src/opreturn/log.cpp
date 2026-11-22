// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/log.h>

#include <interfaces/opreturn.h>
#include <logging.h>
#include <opreturn/names.h>
#include <tinyformat.h>

namespace opreturn {
namespace {

const LogEventFormat FORMATS[LOG_EVENT_COUNT]{
    {LogEvent::OPTION_IGNORED, "off, so %s is ignored", LogArg::OPTION, LogArg::NONE, LogArg::NONE, false},
    {LogEvent::STORED_CHOICE_IGNORED, "the stored choice for %s is out of range and is ignored", LogArg::OPTION, LogArg::NONE, LogArg::NONE, false},
    {LogEvent::STATE, "state %s", LogArg::STATE, LogArg::NONE, LogArg::NONE, false},
    {LogEvent::FAILED, "state FAILED (%s)", LogArg::REASON, LogArg::NONE, LogArg::NONE, false},
    {LogEvent::BLOCK, "block %s, %s rows, %s us", LogArg::INTEGER, LogArg::INTEGER, LogArg::INTEGER, true},
    {LogEvent::CS_MAIN_HELD, "cs_main held %s ms", LogArg::INTEGER, LogArg::NONE, LogArg::NONE, true},
    {LogEvent::COVERAGE_FULL, "the list of covered ranges is full at %s ranges, so %s heights are to be read again", LogArg::INTEGER, LogArg::INTEGER, LogArg::NONE, false},
};

const char* const STATE_NAMES[STATE_COUNT]{"OFF", "OPENING", "RUNNING", "BACKFILLING", "PAUSED_LOW_DISK", "STOPPING", "STOPPED", "FAILED"};
const char* const REASON_NAMES[FAIL_REASON_COUNT]{"NONE", "OPEN_FAILED", "NEWER_FORMAT", "OTHER_CHAIN", "WRITE_FAILED", "READ_FAILED", "INTERNAL"};
static_assert(uint8_t(State::FAILED) + 1 == STATE_COUNT, "a name for every state");
static_assert(uint8_t(FailReason::INTERNAL) + 1 == FAIL_REASON_COUNT, "a name for every fail reason");

} // namespace

std::string FormatLogArgument(LogArg kind, int64_t value)
{
    switch (kind) {
    case LogArg::INTEGER:
        return strprintf("%d", value);
    case LogArg::OPTION:
        if (value >= 0 && value <= int64_t(Option::REINDEX)) return OptionName(Option(value));
        return "?";
    case LogArg::STATE:
        if (value >= 0 && value < STATE_COUNT) return STATE_NAMES[value];
        return "?";
    case LogArg::REASON:
        if (value >= 0 && value < FAIL_REASON_COUNT) return REASON_NAMES[value];
        return "?";
    case LogArg::NONE:
        break;
    }
    return "";
}

const LogEventFormat& GetLogEventFormat(LogEvent event)
{
    return FORMATS[size_t(event) < LOG_EVENT_COUNT ? size_t(event) : 0];
}

std::string FormatLogLine(LogEvent event, int64_t a, int64_t b, int64_t c)
{
    const LogEventFormat& format = GetLogEventFormat(event);
    const std::string arguments[LOG_MAX_ARGUMENTS]{FormatLogArgument(format.a, a), FormatLogArgument(format.b, b),
                                                   FormatLogArgument(format.c, c)};
    // Each "%s" takes the next argument; the texts hold no other directive (a unit test checks every one).
    std::string line{LOG_PREFIX};
    line += ' ';
    size_t next = 0;
    for (const char* p = format.text; *p != '\0'; ++p) {
        if (p[0] == '%' && p[1] == 's') {
            if (next < LOG_MAX_ARGUMENTS) line += arguments[next++];
            ++p;
        } else {
            line += *p;
        }
    }
    return line;
}

void Log(LogEvent event, int64_t a, int64_t b, int64_t c)
{
    if (GetLogEventFormat(event).bench && !LogAcceptCategory(BCLog::BENCH)) return;
    LogPrintf("%s\n", FormatLogLine(event, a, b, c));
}

} // namespace opreturn
