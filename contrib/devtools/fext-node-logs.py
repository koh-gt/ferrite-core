#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check the OP_RETURN index in the debug logs of regtest nodes that ran ferrite-qt-fext.

Usage: contrib/devtools/fext-node-logs.py <directory>

Reads every regtest/debug.log below the directory, such as the data directories that the functional tests kept with
--nocleanup. A node's log holds one part for each time the node started, from the version line that each start logs
first. From the options that a start logged, the check works out whether the index was on in it, as the program
decides: the value of -opreturnindex, or of its second name -fextindex, where one is set; otherwise on unless
-disablewallet is set. (The choices that the FEXT tab stores in the GUI settings are not in the log, so the nodes
checked must have none stored.) The check fails, naming the log and the start, when

  - it finds no log, or a log without a start;
  - a start logged no options;
  - a log shows that the index failed (a "state FAILED" line), that the GUI could not make the index, or that one of
    the GUI's hooks caught an error;
  - in a start with the index on, the index never ran (no RUNNING or BACKFILLING state line), or in a start with the
    index off, it ran.

Other lines of the index and of the FEXT GUI do not count. Exit status 0 when every start passed, 1 when one did not,
2 on wrong usage.
"""

import json
import os
import re
import sys

# A line's time and thread, before its message.
PREFIX = re.compile(r'^(?:\d{4}-\d\d-\d\dT[0-9:.]+Z )?(?:\[[^\] ]*\] )?')
START = 'Ferrite Core version '
COMMAND_LINE_ARG = re.compile(r'^Command-line arg: ([^=]+)=(.*)$')
CONFIG_FILE_ARG = re.compile(r'^Config file arg: (?:\[([^\]]*)\] )?([^=]+)=(.*)$')
SETTINGS_FILE_ARG = re.compile(r'^Setting file arg: (\S+) = (.*)$')
RUNNING = ('OP_RETURN index: state RUNNING', 'OP_RETURN index: state BACKFILLING')
FAILURES = (
    ('OP_RETURN index: state FAILED', 'the OP_RETURN index failed'),
    ('FEXT: the OP_RETURN index could not be made', 'the GUI could not make the OP_RETURN index'),
    ('FEXT: an unexpected error in the ', 'a hook of the GUI caught an error'),
)
# Where a start's options come from, in the order the node reads them: the command line, the settings file, then
# ferrite.conf's [regtest] section and its lines before any section.
SOURCES = ('command line', 'settings file', 'regtest section', 'top of ferrite.conf')


class Start:
    def __init__(self, line_number):
        self.line_number = line_number
        self.options = {source: {} for source in SOURCES}
        self.logged_options = False
        self.running = False

    def add_option(self, source, name, value):
        self.logged_options = True
        try:
            value = json.loads(value)
        except ValueError:
            pass  # a sensitive value, logged as ****
        self.options[source].setdefault(name, []).append(value)

    def setting(self, name):
        """The option's value in this start, as the node reads it, or None when it is not set: the first source that
        holds it decides; in it a negation (false) that comes last wins, and otherwise the last value on the command
        line and the first value after the last negation in ferrite.conf."""
        for source in SOURCES:
            values = self.options[source].get(name)
            if not values:
                continue
            if values[-1] is False:
                return False
            after = values[max([i + 1 for i, v in enumerate(values) if v is False], default=0):]
            return after[0] if source in SOURCES[2:] else after[-1]
        return None

    def index_on(self):
        index = self.setting('opreturnindex')
        if index is None:
            index = self.setting('fextindex')
        if index is not None:
            return as_bool(index)
        disable_wallet = self.setting('disablewallet')
        return not (disable_wallet is not None and as_bool(disable_wallet))


def as_bool(value):
    """A value as the node reads a switch: a text is true when empty or when its number is not 0."""
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    number = re.match(r'\s*([+-]?\d+)', str(value))
    return str(value) == '' or (number is not None and int(number.group(1)) != 0)


def check_log(path, errors):
    """Checks one node's log, adding what fails to errors; the number of starts and of those with the index on."""
    starts = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line_number, line in enumerate(f, start=1):
            line = line.rstrip('\n')
            message = line[PREFIX.match(line).end():]
            if message.startswith(START):
                starts.append(Start(line_number))
                continue
            for text, failure in FAILURES:
                if message.startswith(text):
                    errors.append('{}:{}: {}: {}'.format(path, line_number, failure, message))
            if not starts:
                continue
            start = starts[-1]
            if message in RUNNING:
                start.running = True
                continue
            match = COMMAND_LINE_ARG.match(message)
            if match:
                start.add_option(SOURCES[0], match.group(1), match.group(2))
                continue
            match = SETTINGS_FILE_ARG.match(message)
            if match:
                start.add_option(SOURCES[1], match.group(1), match.group(2))
                continue
            match = CONFIG_FILE_ARG.match(message)
            if match:
                section = match.group(1) or ''
                if section in ('', 'regtest'):
                    start.add_option(SOURCES[2] if section else SOURCES[3], match.group(2), match.group(3))
                else:
                    start.logged_options = True
    if not starts:
        errors.append('{}: no start of a node'.format(path))
    on = 0
    for number, start in enumerate(starts, start=1):
        where = '{}:{}: start {}'.format(path, start.line_number, number)
        if not start.logged_options:
            errors.append('{}: logged no options'.format(where))
        elif start.index_on():
            on += 1
            if not start.running:
                errors.append('{}: the OP_RETURN index is on, but it never ran'.format(where))
        elif start.running:
            errors.append('{}: the OP_RETURN index is off, but it ran'.format(where))
    return len(starts), on


def main():
    if len(sys.argv) != 2 or not os.path.isdir(sys.argv[1]):
        print(__doc__, file=sys.stderr)
        return 2
    logs = []
    for directory, _, files in os.walk(sys.argv[1]):
        if os.path.basename(directory) == 'regtest' and 'debug.log' in files:
            logs.append(os.path.join(directory, 'debug.log'))
    errors = []
    starts = on = 0
    for path in sorted(logs):
        log_starts, log_on = check_log(path, errors)
        starts += log_starts
        on += log_on
    if not logs:
        errors.append('{}: no regtest/debug.log below it'.format(sys.argv[1]))
    for error in errors:
        print('::error::{}'.format(error))
    print('{} logs, {} starts of a node, {} of them with the OP_RETURN index on: {}'.format(
        len(logs), starts, on, '{} failures'.format(len(errors)) if errors else 'as expected'))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
