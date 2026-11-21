// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/log.h>

#include <logging.h>
#include <opreturn/names.h>
#include <tinyformat.h>

namespace opreturn {
namespace {

const LogEventFormat FORMATS[LOG_EVENT_COUNT]{
    {LogEvent::OPTION_IGNORED, "off, so %s is ignored", LogArg::OPTION, LogArg::NONE, false},
    {LogEvent::STORED_CHOICE_IGNORED, "the stored choice for %s is out of range and is ignored", LogArg::OPTION, LogArg::NONE, false},
};

} // namespace

std::string FormatLogArgument(LogArg kind, int64_t value)
{
    switch (kind) {
    case LogArg::INTEGER:
        return strprintf("%d", value);
    case LogArg::OPTION:
        if (value >= 0 && value <= int64_t(Option::REINDEX)) return OptionName(Option(value));
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

std::string FormatLogLine(LogEvent event, int64_t a, int64_t b)
{
    const LogEventFormat& format = GetLogEventFormat(event);
    const std::string arguments[2]{FormatLogArgument(format.a, a), FormatLogArgument(format.b, b)};
    // Each "%s" takes the next argument; the texts hold no other directive (a unit test checks every one).
    std::string line{LOG_PREFIX};
    line += ' ';
    size_t next = 0;
    for (const char* c = format.text; *c != '\0'; ++c) {
        if (c[0] == '%' && c[1] == 's') {
            if (next < 2) line += arguments[next++];
            ++c;
        } else {
            line += *c;
        }
    }
    return line;
}

void Log(LogEvent event, int64_t a, int64_t b)
{
    if (GetLogEventFormat(event).bench && !LogAcceptCategory(BCLog::BENCH)) return;
    LogPrintf("%s\n", FormatLogLine(event, a, b));
}

} // namespace opreturn
