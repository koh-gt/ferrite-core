#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that the node code of the OP_RETURN record (src/opreturn/) and the code of the FEXT GUI make none of the
# calls they must never make. The FEXT GUI shows text that anyone can write into an OP_RETURN output, so its code
# must never hand that text to anything that interprets it, act on it, or let it reach a place where Qt shows or
# logs text of its own choice. The checks, by number:
#
#   1   no rich text: QTextBrowser, QTextEdit (other than in a comment), setHtml(, appendHtml(, insertHtml(,
#       fromHtml(, toHtml(, setMarkdown(, Qt::RichText, Qt::AutoText, Qt::MarkdownText, and the properties html
#       and markdown of a form
#   2   in the forms: every QLabel has the text format Qt::PlainText, and no label opens or offers links
#       (openExternalLinks true, LinksAccessibleByMouse or LinksAccessibleByKeyboard)
#   3   no HTML: toHtmlEscaped, HtmlEscape, and the tags <b>, <br>, <span>, <p>, <hr>, <img> and <a>, in upper or
#       lower case, also as a form writes them (&lt;b&gt;)
#   4   opens nothing: QDesktopServices, openUrl, QUrl::fromUserInput, PaymentServer, receivedURI,
#       handleURIOrFile
#   5   runs no command and loads no code: system(, popen, QProcess, QJSEngine, QScriptEngine, QPluginLoader,
#       QLibrary
#   6   opens no connection: QNetworkAccessManager, QTcpSocket, QUdpSocket, QHostInfo, QDnsLookup,
#       QNetworkRequest
#   7   makes text of bytes only in src/qt/fext/displaytext.cpp: QString::fromUtf8(, QString::fromStdString(,
#       QString::fromLatin1(, fromLocal8Bit(, QTextCodec, and a QByteArray put into a QVariant
#   8   writes to the log only through its log file (src/opreturn/log.cpp, src/fext/log.cpp): no LogPrintf,
#       LogPrint, qDebug, qWarning, qInfo, qCritical, std::cout or std::cerr elsewhere
#   9   takes no wallet action: none of the wallet calls listed below, and no unlock( with an argument (a wallet
#       is unlocked with its passphrase; a lock's unlock() takes none)
#   10  uses no regular expressions and no thread pools: <regex>, std::regex, QRegExp, QRegularExpression,
#       QtConcurrent, QFuture, QThreadPool
#   11  builds and sends no transaction and includes no wallet or wallet GUI header: createTransaction,
#       BroadcastTransaction, and the headers listed below, also as the header or include of a form
#   12  puts text into no tooltip, status tip, What's This, window title, message box detail, status bar
#       message or accessible name unless it is a translation, tr(...): setToolTip(, setStatusTip(,
#       setWhatsThis(, setWindowTitle(, setTabToolTip(, setInformativeText(, setDetailedText(, showMessage(,
#       setAccessibleName(, setAccessibleDescription(, QToolTip::showText(, QWhatsThis::showText(, and
#       setData( or setItemData( with the tooltip, status tip or What's This role; and shows no QMessageBox
#       outside src/qt/fext/displaytext.cpp
#   13  translates only through tr(): no QObject::tr( or QCoreApplication::translate( outside
#       src/qt/fext/main.cpp, which copies the GUI's translation function; and every Q_OBJECT class is named
#       Fext...
#   14  no chained .arg( calls, which would substitute a placeholder inside a substituted name again
#   15  no functor form of QMetaObject::invokeMethod (a second argument that starts with [ or &), which the Qt of
#       the release builds lacks
#
# The record's node code is checked by 5, 8, 9, 10 and 11, which apply to code without a user interface; the FEXT
# GUI's code by all of them. Its files are those of src/fext/ (the code without Qt) and its forms,
# src/qt/forms/fext*.ui, which every check reads and check 2 reads as forms; the directory of its Qt code joins
# them when that code is checked.
#
# The checks read statements, not lines: the lines of a statement written over several lines are joined into one,
# so that a call is found however it is laid out, and a finding names the statement's first line. A line that is
# blank, starts with #, < or * (a preprocessor line, a line of a form, a line of a comment) or holds a comment
# (//, /* or */) stands alone; the other lines are joined up to one that ends with ;, { or }.
#
# Each check first runs on its sample in test/lint/fext-safety-samples/, and must find every statement of it but
# the first line, which says what the sample is, so that a check that can no longer find a call fails too; no
# check may find anything in good.txt there. The checks read comments too (but for QTextEdit): a comment that
# names a call they look for fails as the call would.

export LC_ALL=C

cd "$(dirname "$0")/../.." || exit 1

SAMPLES=test/lint/fext-safety-samples
WORD_START='(^|[^A-Za-z0-9_])'
WORD_END='([^A-Za-z0-9_]|$)'

CHECK_1="${WORD_START}(QTextBrowser|Qt::RichText|Qt::AutoText|Qt::MarkdownText)${WORD_END}|${WORD_START}(setHtml|appendHtml|insertHtml|fromHtml|toHtml|setMarkdown)[[:space:]]*\\(|<property name=\"(html|markdown)\""
CHECK_1_EDIT="${WORD_START}QTextEdit${WORD_END}"
# Tag names in either case; a form writes < and > as &lt; and &gt;.
HTML_TAGS='[bB]|[bB][rR]|[sS][pP][aA][nN]|[pP]|[hH][rR]|[iI][mM][gG]|[aA]'
CHECK_3="${WORD_START}(toHtmlEscaped|HtmlEscape)${WORD_END}|(<|&lt;)(${HTML_TAGS})([[:space:]/>]|&gt;|$)"
CHECK_4="${WORD_START}(QDesktopServices|openUrl|PaymentServer|receivedURI|handleURIOrFile)${WORD_END}|QUrl::fromUserInput"
CHECK_5="${WORD_START}(system|popen)[[:space:]]*\\(|${WORD_START}(QProcess|QJSEngine|QScriptEngine|QPluginLoader|QLibrary)${WORD_END}"
CHECK_6="${WORD_START}(QNetworkAccessManager|QTcpSocket|QUdpSocket|QHostInfo|QDnsLookup|QNetworkRequest)${WORD_END}"
CHECK_7="QString::(fromUtf8|fromStdString|fromLatin1)[[:space:]]*\\(|${WORD_START}fromLocal8Bit[[:space:]]*\\(|${WORD_START}QTextCodec${WORD_END}|QVariant[[:space:]]*(\\(|\\{)[[:space:]]*QByteArray|fromValue[[:space:]]*(<[[:space:]]*QByteArray[[:space:]]*>)?[[:space:]]*\\([[:space:]]*QByteArray"
CHECK_8="${WORD_START}(LogPrintf|LogPrint|qDebug|qWarning|qInfo|qCritical)${WORD_END}|std::(cout|cerr)${WORD_END}"
WALLET_CALLS="sendCoins|commitTransaction|lockCoin|unlockCoin|setAddressBook|delAddressBook|signMessage|encryptWallet|changeWalletPassphrase|backupWallet|abandonTransaction|createBumpTransaction|signBumpTransaction|commitBumpTransaction|rebroadcastTransaction|getNewDestination|reserveNewDestination|fillPSBT|addDestData|eraseDestData|executeRpc|RPCConsole|setAddress|pasteEntry|handleURIOrFile|receivedURI|setWalletLocked"
CHECK_9="${WORD_START}(${WALLET_CALLS})${WORD_END}|${WORD_START}unlock[[:space:]]*\\([[:space:]]*[^)[:space:]]"
CHECK_10="#[[:space:]]*include[[:space:]]*<regex>|std::regex|${WORD_START}(QRegExp|QRegularExpression|QtConcurrent|QFuture|QThreadPool)${WORD_END}"
WALLET_HEADERS='interfaces/wallet\.h|wallet/wallet\.h|wallet/coincontrol\.h|node/transaction\.h|qt/walletmodeltransaction\.h|qt/walletmodel\.h|qt/transactiontablemodel\.h|qt/bitcoingui\.h|qt/sendcoinsdialog\.h|qt/walletframe\.h|qt/bitcoinunits\.h'
CHECK_11="${WORD_START}(createTransaction|BroadcastTransaction)${WORD_END}|#[[:space:]]*include[[:space:]]*[<\"](${WALLET_HEADERS})[>\"]|<(header|include)([[:space:]][^>]*)?>(${WALLET_HEADERS})<"
TEXT_SETTERS='setToolTip|setStatusTip|setWhatsThis|setWindowTitle|setInformativeText|setDetailedText|showMessage|setAccessibleName|setAccessibleDescription'
CHECK_12_SET="${WORD_START}(${TEXT_SETTERS})[[:space:]]*\\("
CHECK_12_TR="${WORD_START}(${TEXT_SETTERS})[[:space:]]*\\([[:space:]]*tr[[:space:]]*\\("
CHECK_12_TAB="${WORD_START}setTabToolTip[[:space:]]*\\("
CHECK_12_TAB_TR="${WORD_START}setTabToolTip[[:space:]]*\\([^,]*,[[:space:]]*tr[[:space:]]*\\("
CHECK_12_SHOW="(QToolTip|QWhatsThis)::showText[[:space:]]*\\(|${WORD_START}(setData|setItemData)[[:space:]]*\\(.*Qt::(ToolTipRole|StatusTipRole|WhatsThisRole)"
CHECK_12_BOX="${WORD_START}QMessageBox${WORD_END}"
CHECK_13="(QObject::tr|QCoreApplication::translate)[[:space:]]*\\("
# The first argument may hold parentheses one level deep, as in .arg(QString::number(n)).arg(b).
CHECK_14="\\.arg[[:space:]]*\\(([^()]|\\([^()]*\\))*\\)[[:space:]]*\\.arg[[:space:]]*\\("
CHECK_15="invokeMethod[[:space:]]*\\([^,]*,[[:space:]]*[[&]"

# statements <file>...: the statements of the files, as "file:line:text", line being the statement's first line.
statements() {
    awk '
        function flush() { if (n) print file ":" first ":" text; n = 0 }
        FNR == 1 { flush() }
        /^[ \t]*[#<*]/ || /^[ \t]*$/ || index($0, "//") || index($0, "/*") || index($0, "*/") {
            flush()
            print FILENAME ":" FNR ":" $0
            next
        }
        { if (n++) text = text " " $0; else { file = FILENAME; first = FNR; text = $0 } }
        /[;{}][ \t]*$/ { flush() }
        END { flush() }' "$@"
}

# grep_lines <pattern> <file>...: the statements of the files that match, as "file:line:text".
grep_lines() {
    local pattern=$1
    shift
    [ $# -eq 0 ] && return
    # The pattern may match from the colon before the text on, which is a word's start.
    statements "$@" | grep -E -- "^[^:]*:[0-9]+.*(${pattern})"
}

# The statements of grep_lines output whose text, once a // comment is cut off and unless it is a line of a block
# comment, still matches the pattern.
outside_comments() {
    local pattern=$1 line text
    while IFS= read -r line; do
        text=${line#*:}
        text=${text#*:}
        text=${text%%//*}
        grep -q -E '^[[:space:]]*(\*|/\*)' <<< "${text}" && continue
        grep -q -E -- "${pattern}" <<< "${text}" && echo "${line}"
    done
}

# find_<number> <file>...: the lines of the files that break the check.
find_1() { grep_lines "${CHECK_1}" "$@"; grep_lines "${CHECK_1_EDIT}" "$@" | outside_comments "${CHECK_1_EDIT}"; }
find_2() {
    [ $# -eq 0 ] && return
    grep -n -E 'openExternalLinks|LinksAccessibleBy' "$@" /dev/null | grep -v -E 'openExternalLinks.*<bool>false</bool>'
    grep -n -A3 -E '<property name="openExternalLinks">' "$@" /dev/null | grep -E '<bool>true</bool>'
    # Each QLabel needs a textFormat property of Qt::PlainText before its widget ends.
    awk '
        /<widget class="QLabel"/ { label = 1; plain = 0; start = FNR; next }
        label && /<property name="textFormat">/ { format = 1; next }
        label && format && /<enum>/ { plain = ($0 ~ /<enum>Qt::PlainText<\/enum>/); format = 0; next }
        label && /<\/widget>/ {
            if (!plain) print FILENAME ":" start ": a QLabel without the text format Qt::PlainText"
            label = 0
        }' "$@"
}
find_3() { grep_lines "${CHECK_3}" "$@"; }
find_4() { grep_lines "${CHECK_4}" "$@"; }
find_5() { grep_lines "${CHECK_5}" "$@"; }
find_6() { grep_lines "${CHECK_6}" "$@"; }
find_7() { grep_lines "${CHECK_7}" "$@"; }
find_8() { grep_lines "${CHECK_8}" "$@"; }
find_9() { grep_lines "${CHECK_9}" "$@"; }
find_10() { grep_lines "${CHECK_10}" "$@"; }
find_11() { grep_lines "${CHECK_11}" "$@"; }
find_12() {
    grep_lines "${CHECK_12_SET}" "$@" | grep -v -E -- "${CHECK_12_TR}"
    grep_lines "${CHECK_12_TAB}" "$@" | grep -v -E -- "${CHECK_12_TAB_TR}"
    grep_lines "${CHECK_12_SHOW}" "$@" | grep -v -E -- "${WORD_START}tr[[:space:]]*\\("
}
find_12_box() { grep_lines "${CHECK_12_BOX}" "$@"; }
find_13() {
    grep_lines "${CHECK_13}" "$@"
    [ $# -eq 0 ] && return
    # The class that a Q_OBJECT line belongs to is the last one declared above it.
    awk '
        FNR == 1 { name = "" }
        match($0, /^[ \t]*class[ \t]+[A-Za-z_][A-Za-z0-9_]*/) { name = substr($0, RSTART, RLENGTH); sub(/^[ \t]*class[ \t]+/, "", name) }
        /Q_OBJECT/ && name !~ /^Fext/ { print FILENAME ":" FNR ": a Q_OBJECT class named " name }' "$@"
}
find_14() { grep_lines "${CHECK_14}" "$@"; }
find_15() { grep_lines "${CHECK_15}" "$@"; }

EXIT_CODE=0

# check_sample <finder> <number> <description>: the finder must report every statement of the sample but its first
# line.
check_sample() {
    local finder=$1 number=$2 description=$3 sample="${SAMPLES}/check$2.txt" found expected line
    if [ ! -f "${sample}" ]; then
        echo "Check ${number} (${description}) has no sample ${sample}."
        EXIT_CODE=1
        return
    fi
    found=$(${finder} "${sample}" | cut -d: -f2 | sort -u)
    if [ "${number}" = 2 ]; then
        # A form: the check must find something in it.
        expected=$([ -n "${found}" ] && echo "${found}")
    else
        expected=$(statements "${sample}" | awk -F: '$2 > 1 && $0 !~ /^[^:]*:[0-9]+:[ \t]*$/ { print $2 }' | sort -u)
    fi
    if [ -z "${found}" ] || [ "${found}" != "${expected}" ]; then
        echo "Check ${number} (${description}) does not find everything it looks for in ${sample}; it finds lines: ${found//$'\n'/ }"
        EXIT_CODE=1
    fi
}

# run_check <finder> <number> <what the code must not do> <name of the code> <files, one per line> [<file that may break it>...]
run_check() {
    local finder=$1 number=$2 description=$3 code=$4 files=$5 output allowed
    shift 5
    for allowed in "$@"; do
        files=$(grep -v -x -F -- "${allowed}" <<< "${files}")
    done
    [ -z "${files}" ] && return
    # shellcheck disable=SC2086
    output=$(${finder} ${files})
    if [ -n "${output}" ]; then
        echo "Check ${number}: ${code} must not ${description}:"
        echo "${output}"
        echo
        EXIT_CODE=1
    fi
}

RECORD_FILES=$(git ls-files -- 'src/opreturn/*')
FEXT_FILES=$(git ls-files -- 'src/fext/*' 'src/qt/forms/fext*.ui')
FEXT_FORMS=$(git ls-files -- 'src/qt/forms/fext*.ui')
RECORD="the OP_RETURN record's node code"
FEXT="the FEXT GUI's code"

check_sample find_1 1 "rich text"
check_sample find_2 2 "labels in forms"
check_sample find_3 3 "HTML"
check_sample find_4 4 "opening URLs"
check_sample find_5 5 "commands and code"
check_sample find_6 6 "connections"
check_sample find_7 7 "text from bytes"
check_sample find_8 8 "logging"
check_sample find_9 9 "wallet calls"
check_sample find_10 10 "regular expressions and thread pools"
check_sample find_11 11 "transactions and wallet headers"
check_sample find_12 12 "text in tooltips and titles"
check_sample find_13 13 "translation"
check_sample find_14 14 "chained arg"
check_sample find_15 15 "functor invokeMethod"
if [ ! -f "${SAMPLES}/check12box.txt" ] || [ -z "$(find_12_box "${SAMPLES}/check12box.txt")" ]; then
    echo "Check 12 (message boxes) does not find what it looks for in ${SAMPLES}/check12box.txt."
    EXIT_CODE=1
fi
for finder in find_1 find_3 find_4 find_5 find_6 find_7 find_8 find_9 find_10 find_11 find_12 find_12_box find_13 find_14 find_15; do
    OUTPUT=$(${finder} "${SAMPLES}/good.txt")
    if [ -n "${OUTPUT}" ]; then
        echo "${finder} finds lines in ${SAMPLES}/good.txt, which breaks no check:"
        echo "${OUTPUT}"
        EXIT_CODE=1
    fi
done

run_check find_5 5 "run commands or load code" "${RECORD}" "${RECORD_FILES}"
run_check find_8 8 "write to the log except through src/opreturn/log.cpp" "${RECORD}" "${RECORD_FILES}" src/opreturn/log.cpp
run_check find_9 9 "call the wallet" "${RECORD}" "${RECORD_FILES}"
run_check find_10 10 "use regular expressions or thread pools" "${RECORD}" "${RECORD_FILES}"
run_check find_11 11 "build or send transactions, or include wallet headers" "${RECORD}" "${RECORD_FILES}"

run_check find_1 1 "use rich text" "${FEXT}" "${FEXT_FILES}"
run_check find_2 2 "have a QLabel that is not plain text or that opens links" "${FEXT}" "${FEXT_FORMS}"
run_check find_3 3 "use HTML" "${FEXT}" "${FEXT_FILES}"
run_check find_4 4 "open URLs or payment requests" "${FEXT}" "${FEXT_FILES}"
run_check find_5 5 "run commands or load code" "${FEXT}" "${FEXT_FILES}"
run_check find_6 6 "open network connections" "${FEXT}" "${FEXT_FILES}"
run_check find_7 7 "make text of bytes except in src/qt/fext/displaytext.cpp" "${FEXT}" "${FEXT_FILES}" src/qt/fext/displaytext.cpp
run_check find_8 8 "write to the log except through src/fext/log.cpp" "${FEXT}" "${FEXT_FILES}" src/fext/log.cpp
run_check find_9 9 "call the wallet" "${FEXT}" "${FEXT_FILES}"
run_check find_10 10 "use regular expressions or thread pools" "${FEXT}" "${FEXT_FILES}"
run_check find_11 11 "build or send transactions, or include wallet headers" "${FEXT}" "${FEXT_FILES}"
run_check find_12 12 "put text other than translations into tooltips, titles or status lines" "${FEXT}" "${FEXT_FILES}"
run_check find_12_box 12 "show a QMessageBox except in src/qt/fext/displaytext.cpp" "${FEXT}" "${FEXT_FILES}" src/qt/fext/displaytext.cpp
run_check find_13 13 "translate except through tr(), or name a Q_OBJECT class without Fext" "${FEXT}" "${FEXT_FILES}" src/qt/fext/main.cpp
run_check find_14 14 "chain .arg( calls" "${FEXT}" "${FEXT_FILES}"
run_check find_15 15 "use the functor form of QMetaObject::invokeMethod" "${FEXT}" "${FEXT_FILES}"

exit ${EXIT_CODE}
