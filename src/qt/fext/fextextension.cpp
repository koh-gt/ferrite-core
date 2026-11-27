// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/fext/fextextension.h>

#include <logging.h>
#include <opreturn/names.h>
#include <opreturn/node.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>

#include <limits>

#include <QSettings>
#include <QString>
#include <QVariant>

namespace {
//! The hooks' names in the log, in the order of FextExtension::Hook.
const char* const HOOK_NAMES[] = {
    "attachNode",
    "wrapCentralWidget",
    "addTabActions",
    "addToolBarActions",
    "windowTitle",
    "usageHeader",
    "extendSendPage",
    "prepareSend",
    "describeSend",
    "clearSendPage",
};
static_assert(sizeof(HOOK_NAMES) / sizeof(HOOK_NAMES[0]) == static_cast<size_t>(FextExtension::Hook::COUNT),
    "HOOK_NAMES must name each hook of FextExtension::Hook");

//! Writes the one log line of a hook whose body threw. Never throws itself.
void LogHookError(FextExtension::Hook hook) noexcept
{
    try {
        LogPrintf("FEXT: an unexpected error in the %s hook was caught\n", HOOK_NAMES[static_cast<int>(hook)]);
    } catch (...) {
    }
}
} // namespace

template <typename Result, typename Body>
Result FextExtension::guarded(Hook hook, Result fallback, Body body)
{
    try {
        enterHook(hook);
        return body();
    } catch (...) {
        LogHookError(hook);
        return fallback;
    }
}

template <typename Body>
void FextExtension::guarded(Hook hook, Body body)
{
    try {
        enterHook(hook);
        body();
    } catch (...) {
        LogHookError(hook);
    }
}

opreturn::StoredChoices FextExtension::ReadStoredChoices(const QSettings& settings)
{
    opreturn::StoredChoices stored;
    // A settings file in the INI format and the Windows registry give a stored boolean back as its text.
    const QVariant record{settings.value(QStringLiteral("fext/record"))};
    if (record.type() == QVariant::Bool) {
        stored.record = record.toBool();
    } else if (record.type() == QVariant::String) {
        const QString text{record.toString()};
        if (text == QStringLiteral("true")) stored.record = true;
        if (text == QStringLiteral("false")) stored.record = false;
    }
    // ... and a whole number as its decimal text.
    const QVariant max_size{settings.value(QStringLiteral("fext/maxSize"))};
    bool ok{false};
    qlonglong mib{0};
    switch (max_size.type()) {
    case QVariant::Int:
    case QVariant::LongLong:
    case QVariant::String:
        mib = max_size.toLongLong(&ok);
        break;
    case QVariant::UInt:
    case QVariant::ULongLong: {
        const qulonglong value{max_size.toULongLong(&ok)};
        ok = ok && value <= qulonglong(std::numeric_limits<qlonglong>::max());
        mib = qlonglong(value);
        break;
    }
    default:
        break;
    }
    if (ok) stored.max_size_mib = static_cast<int64_t>(mib);
    return stored;
}

void FextExtension::attachNode(NodeContext& node)
{
    guarded(Hook::ATTACH_NODE, [&node] {
        // The record and its chain client, with the tab's stored choices under the options set explicitly. A value of
        // an option that may not be set stops start-up at the client's verify(), which init calls next.
        if (!opreturn::Construct(node, ReadStoredChoices(QSettings{}))) {
            LogPrintf("FEXT: %s could not be made\n", opreturn::RECORD_NOUN);
        }
    });
}

QWidget* FextExtension::wrapCentralWidget(BitcoinGUI& gui, WalletFrame& frame)
{
    QWidget* const bare_frame{&frame};
    return guarded(Hook::WRAP_CENTRAL_WIDGET, bare_frame, [bare_frame] { return bare_frame; });
}

void FextExtension::addTabActions(BitcoinGUI& gui, QActionGroup& tabs)
{
    guarded(Hook::ADD_TAB_ACTIONS, [] {});
}

void FextExtension::addToolBarActions(BitcoinGUI& gui, QToolBar& toolbar)
{
    guarded(Hook::ADD_TOOL_BAR_ACTIONS, [] {});
}

QString FextExtension::windowTitle(const QString& plain_title)
{
    return guarded(Hook::WINDOW_TITLE, plain_title, [&plain_title] {
        // "Ferrite Core - wallet - [testnet]" becomes "Ferrite Core FEXT - wallet - [testnet]".
        const QString name{QStringLiteral(PACKAGE_NAME)};
        if (!plain_title.startsWith(name)) return plain_title;
        return QString{name + QStringLiteral(" FEXT") + plain_title.mid(name.size())};
    });
}

QString FextExtension::usageHeader(const QString& plain_header)
{
    return guarded(Hook::USAGE_HEADER, plain_header, [&plain_header] {
        // "Usage:  ferrite-qt [command-line options]" names this program instead.
        QString header{plain_header};
        header.replace(QStringLiteral("ferrite-qt "), QStringLiteral("ferrite-qt-fext "));
        return header;
    });
}

void FextExtension::extendSendPage(SendCoinsDialog& dialog, QBoxLayout& layout, int index)
{
    guarded(Hook::EXTEND_SEND_PAGE, [] {});
}

WalletModel::SendCoinsReturn FextExtension::prepareSend(SendCoinsDialog& dialog, WalletModel& model,
    WalletModelTransaction& transaction, const CCoinControl& coin_control)
{
    // Exactly the call the Send page makes without an extension. On an error nothing may be sent, so the fallback
    // is a failure, never OK.
    return guarded(Hook::PREPARE_SEND, WalletModel::SendCoinsReturn{WalletModel::TransactionCreationFailed},
        [&model, &transaction, &coin_control] { return model.prepareTransaction(transaction, coin_control); });
}

void FextExtension::describeSend(SendCoinsDialog& dialog, WalletModelTransaction& transaction,
    QString& question, QString& informative, QString& detailed)
{
    guarded(Hook::DESCRIBE_SEND, [] {});
}

void FextExtension::clearSendPage(SendCoinsDialog& dialog)
{
    guarded(Hook::CLEAR_SEND_PAGE, [] {});
}
