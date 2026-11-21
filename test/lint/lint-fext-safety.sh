#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that the node code of the OP_RETURN record (src/opreturn/) makes none of the calls it must never make.
# The checks are numbered so that the checks for the code of the FEXT GUI can join them in this script; these are
# the ones that apply to code without a user interface:
#
#   5   runs no command and loads no code: system(, popen, QProcess, QJSEngine, QScriptEngine, QPluginLoader,
#       QLibrary
#   8   writes to the log only through src/opreturn/log.cpp: no LogPrintf, LogPrint, qDebug, qWarning, qInfo,
#       qCritical, std::cout or std::cerr elsewhere
#   9   takes no wallet action: none of the wallet calls listed below
#   10  uses no regular expressions and no thread pools: <regex>, std::regex, QRegExp, QRegularExpression,
#       QtConcurrent, QFuture, QThreadPool
#   11  builds and sends no transaction and includes no wallet or wallet GUI header: createTransaction,
#       BroadcastTransaction, and the headers listed below
#
# Each check first runs on its sample in test/lint/fext-safety-samples/, which it must find, so that a check that
# can no longer find anything fails too. The checks read every line, comments included: a comment that names a
# call they look for fails as the call would.

export LC_ALL=C

cd "$(dirname "$0")/../.." || exit 1

SAMPLES=test/lint/fext-safety-samples
WORD_START='(^|[^A-Za-z0-9_])'
WORD_END='([^A-Za-z0-9_]|$)'

CHECK_5="${WORD_START}(system|popen)[[:space:]]*\\(|${WORD_START}(QProcess|QJSEngine|QScriptEngine|QPluginLoader|QLibrary)${WORD_END}"
CHECK_8="${WORD_START}(LogPrintf|LogPrint|qDebug|qWarning|qInfo|qCritical)${WORD_END}|std::(cout|cerr)${WORD_END}"
WALLET_CALLS="sendCoins|commitTransaction|lockCoin|unlockCoin|setAddressBook|delAddressBook|signMessage|encryptWallet|changeWalletPassphrase|backupWallet|abandonTransaction|createBumpTransaction|signBumpTransaction|commitBumpTransaction|rebroadcastTransaction|getNewDestination|reserveNewDestination|fillPSBT|addDestData|eraseDestData|executeRpc|RPCConsole|setAddress|pasteEntry|handleURIOrFile|receivedURI"
CHECK_9="${WORD_START}(${WALLET_CALLS})${WORD_END}"
CHECK_10="#[[:space:]]*include[[:space:]]*<regex>|std::regex|${WORD_START}(QRegExp|QRegularExpression|QtConcurrent|QFuture|QThreadPool)${WORD_END}"
WALLET_HEADERS='interfaces/wallet\.h|wallet/coincontrol\.h|node/transaction\.h|qt/walletmodeltransaction\.h|qt/walletmodel\.h|qt/transactiontablemodel\.h|qt/bitcoingui\.h|qt/sendcoinsdialog\.h|qt/walletframe\.h|qt/bitcoinunits\.h'
CHECK_11="${WORD_START}(createTransaction|BroadcastTransaction)${WORD_END}|#[[:space:]]*include[[:space:]]*[<\"](${WALLET_HEADERS})[>\"]"

EXIT_CODE=0

# run_check <number> <description> <pattern> <file that may break it, or empty>
run_check() {
    local number=$1 description=$2 pattern=$3 allowed=$4 sample="${SAMPLES}/check$1.txt" files output
    if [ ! -f "${sample}" ] || ! grep -q -E "${pattern}" "${sample}"; then
        echo "Check ${number} (${description}) does not find what it looks for in ${sample}."
        EXIT_CODE=1
    fi
    files=$(git ls-files -- 'src/opreturn/*' | grep -v -x -F "${allowed:-/}")
    [ -z "${files}" ] && return
    # shellcheck disable=SC2086
    output=$(grep -n -E "${pattern}" ${files})
    if [ -n "${output}" ]; then
        echo "Check ${number}: the OP_RETURN record's node code must not ${description}:"
        echo "${output}"
        echo
        EXIT_CODE=1
    fi
}

run_check 5 "run commands or load code" "${CHECK_5}" ""
run_check 8 "write to the log except through src/opreturn/log.cpp" "${CHECK_8}" "src/opreturn/log.cpp"
run_check 9 "call the wallet" "${CHECK_9}" ""
run_check 10 "use regular expressions or thread pools" "${CHECK_10}" ""
run_check 11 "build or send transactions, or include wallet headers" "${CHECK_11}" ""

exit ${EXIT_CODE}
