// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_LOG_H
#define BITCOIN_OPRETURN_LOG_H

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * The only way the OP_RETURN record writes to the debug log. A line is the record's prefix, the fixed text of an
 * event, and arguments that are numbers or names from fixed tables. So no script byte, address or other value a
 * stranger can choose, and nothing a caller typed, reaches the log.
 */
namespace opreturn {

//! What an argument of a log line is.
enum class LogArg : uint8_t {
    NONE,    //!< no argument
    INTEGER, //!< a number
    OPTION,  //!< an Option, written as its name
    STATE,   //!< a State of the record, written as its name
    REASON,  //!< a FailReason of the record, written as its name
};

enum class LogEvent : uint8_t {
    OPTION_IGNORED = 0,        //!< a: an Option that has no effect while the record is off
    STORED_CHOICE_IGNORED = 1, //!< a: an Option whose choice stored by the GUI is out of range
    STATE = 2,                 //!< a: the State the record has entered (not FAILED)
    FAILED = 3,                //!< a: the FailReason with which the record failed
    BLOCK = 4,                 //!< a: the height of a block written, b: its rows, c: the microseconds it took
    CS_MAIN_HELD = 5,          //!< a: the milliseconds the record held cs_main, when that was long
    COVERAGE_FULL = 6,         //!< a: the most ranges the record covers, b: the heights it takes out of coverage
};

//! The number of events; every value below it is a LogEvent.
static constexpr size_t LOG_EVENT_COUNT{7};

//! The most arguments a line takes.
static constexpr size_t LOG_MAX_ARGUMENTS{3};

struct LogEventFormat {
    LogEvent event;
    //! One line, without the prefix and the newline, with one "%s" for each argument.
    const char* text;
    LogArg a;
    LogArg b;
    LogArg c;
    //! Whether the line is written only with -debug=bench.
    bool bench;
};

const LogEventFormat& GetLogEventFormat(LogEvent event);

//! How an argument appears in a line: a number in decimal; an option, a state or a fail reason by its name ("?"
//! for a value that is none).
std::string FormatLogArgument(LogArg kind, int64_t value);

//! The line Log writes for an event, without its newline.
std::string FormatLogLine(LogEvent event, int64_t a = 0, int64_t b = 0, int64_t c = 0);

//! Writes the line of an event to the debug log.
void Log(LogEvent event, int64_t a = 0, int64_t b = 0, int64_t c = 0);

} // namespace opreturn

#endif // BITCOIN_OPRETURN_LOG_H
