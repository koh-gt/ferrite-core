// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/fexthooktests.h>

#include <chainparamsbase.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/opreturn.h>
#include <key.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <opreturn/names.h>
#include <opreturn/node.h>
#include <opreturn/options.h>
#include <opreturn/registry.h>
#include <qt/bitcoin.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoingui.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/fext/fextextension.h>
#include <qt/guiconstants.h>
#include <qt/guiextension.h>
#include <qt/networkstyle.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/rpcconsole.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/utilitydialog.h>
#include <qt/walletframe.h>
#include <qt/walletmodel.h>
#include <qt/walletmodeltransaction.h>
#include <scheduler.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/system.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScopedPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidgetAction>

namespace {
using Hook = FextExtension::Hook;

//! The plain main window, as Describe() gives it: the wallet frame in the middle, the four tab actions in their
//! exclusive group and on the toolbar.
const QString PLAIN_WINDOW{"central widget: WalletFrame; exclusive group: &Overview, &Send, &Receive, &Transactions; "
                           "toolbar: &Overview, &Send, &Receive, &Transactions"};

//! The items of the plain Send page's top-level layout, as LayoutItems() gives them.
const QStringList PLAIN_SEND_PAGE{"frameCoinControl", "frameMWEBFeatures", "scrollArea", "frameFee", "horizontalLayout"};

//! Makes a GUI extension the program's for as long as it lives, and then none again.
class ExtensionScope
{
public:
    explicit ExtensionScope(GuiExtension* extension) { g_gui_extension = extension; }
    ~ExtensionScope() { g_gui_extension = nullptr; }
};

//! What a hook of ThrowingExtension throws: not a std::exception, so that only a guard that catches everything
//! stops it.
struct HookTestFailure {
};

//! The extension of ferrite-qt-fext, with one hook armed to throw once from inside its guard.
class ThrowingExtension : public FextExtension
{
public:
    void arm(Hook hook)
    {
        m_armed = true;
        m_hook = hook;
    }
    //! Whether the armed hook has not been called yet.
    bool armed() const { return m_armed; }
    int thrown() const { return m_thrown; }

protected:
    void enterHook(Hook hook) override
    {
        if (m_armed && hook == m_hook) {
            m_armed = false;
            ++m_thrown;
            throw HookTestFailure{};
        }
    }

private:
    bool m_armed{false};
    Hook m_hook{Hook::ATTACH_NODE};
    int m_thrown{0};
};

//! The extension of ferrite-qt-fext, with an attachNode that writes to stdout where GuiMain calls it and the stored
//! choice for the record that it reads there, and then ends the program at once, so that the node is never started.
class AttachNodeReporter : public FextExtension
{
public:
    void attachNode(NodeContext& node) override
    {
        const bool gui_thread{QThread::currentThread() == QCoreApplication::instance()->thread()};
        // The base initialization ends by making the node's interfaces for its chain clients; starting the node
        // makes its scheduler, mempool, connection manager and chainstate manager.
        const bool base_initialized{node.args == &gArgs && node.chain && node.wallet_client};
        const bool not_started{!node.scheduler && !node.mempool && !node.connman && !node.chainman};
        const opreturn::StoredChoices stored{ReadStoredChoices(QSettings{})};
        std::printf("attachNode: %s, %s, %s\nthe stored choice for the record: %s\n", gui_thread ? "on the GUI thread" : "NOT on the GUI thread",
            base_initialized ? "after the base initialization" : "NOT after the base initialization",
            not_started ? "before the node starts" : "NOT before the node starts",
            !stored.record ? "none" : *stored.record ? "on" : "off");
        std::fflush(stdout);
        std::_Exit(EXIT_SUCCESS);
    }
};

//! Sets -disablewallet while it lives and unsets it after, however the scope is left.
class WalletDisabled
{
public:
    WalletDisabled() { gArgs.ForceSetArg("-disablewallet", "1"); }
    ~WalletDisabled() { gArgs.ForceSetArg("-disablewallet", "0"); }
};

//! Keeps the lines written to the log while it lives.
class LogCapture
{
public:
    LogCapture() : m_callback{LogInstance().PushBackCallback([this](const std::string& line) { add(line); })} {}
    ~LogCapture() { LogInstance().DeleteCallback(m_callback); }
    //! How many of the lines contain text.
    int count(const std::string& text)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        return std::count_if(m_lines.begin(), m_lines.end(), [&text](const std::string& line) { return line.find(text) != std::string::npos; });
    }

private:
    void add(const std::string& line)
    {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_lines.push_back(line);
    }
    std::mutex m_mutex;
    std::vector<std::string> m_lines;
    std::list<std::function<void(const std::string&)>>::iterator m_callback;
};

//! While it lives, answers every message box that shows: a send confirmation with Yes, or with Cancel when cancel is
//! true, any other with its default button. The texts of the boxes are kept in the order they showed.
class BoxAnswerer
{
public:
    explicit BoxAnswerer(bool cancel = false)
    {
        QObject::connect(&m_timer, &QTimer::timeout, [this, cancel] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                QMessageBox* box = qobject_cast<QMessageBox*>(widget);
                if (!box || !box->isVisible()) continue;
                texts << box->text();
                QAbstractButton* button{nullptr};
                if (qobject_cast<SendConfirmationDialog*>(box)) {
                    button = box->button(cancel ? QMessageBox::Cancel : QMessageBox::Yes);
                    button->setEnabled(true);
                } else {
                    button = box->defaultButton() ? box->defaultButton() : box->escapeButton();
                }
                button->click();
            }
        });
        m_timer.start(10);
    }
    QStringList texts;

private:
    QTimer m_timer;
};

//! The tests that make windows do not run on macOS with the "minimal" platform, as in the other GUI tests.
bool SkipOnMacMinimal()
{
#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        QWARN("Skipping FextHookTests on mac build with 'minimal' platform set due to Qt bugs (QTBUG-49686).");
        return true;
    }
#endif
    return false;
}

//! A legacy wallet that holds the key the test chain's coinbases pay, scanned up to the tip.
std::shared_ptr<CWallet> MakeWallet(TestChain100Setup& test, const std::string& name)
{
    const int tip_height{::ChainActive().Height()};
    const uint256 tip_hash{::ChainActive().Tip()->GetBlockHash()};
    auto wallet = std::make_shared<CWallet>(test.m_node.chain.get(), name, CreateMockWalletDatabase());
    bool first_run;
    wallet->LoadWallet(first_run);
    {
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type, spk_man->GetScanSecret()), "", "receive");
        spk_man->AddKeyPubKey(test.coinbaseKey, test.coinbaseKey.GetPubKey());
        wallet->SetLastBlockProcessed(tip_height, tip_hash);
    }
    WalletRescanReserver reserver(*wallet);
    reserver.reserve();
    const CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0 /* block height */, {} /* max height */, reserver, true /* fUpdate */);
    assert(result.status == CWallet::ScanResult::SUCCESS);
    wallet->SetBroadcastTransactions(true);
    return wallet;
}

//! A regtest chain of COINBASE_MATURITY + 5 blocks, all paying the coinbase key, so that a wallet with that key can
//! spend the coinbases of the first five blocks; wallets of that key with the given names, a ClientModel, and a
//! WalletModel for each wallet.
struct WalletFixture {
    TestChain100Setup test;
    OptionsModel options_model;
    std::unique_ptr<ClientModel> client_model;
    std::vector<std::shared_ptr<CWallet>> wallets;
    std::vector<std::unique_ptr<WalletModel>> wallet_models;

    WalletFixture(FextHookTests& tests, const std::vector<std::string>& names)
    {
        for (int i = 0; i < 5; ++i) {
            test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
        }
        interfaces::Node& node{tests.node()};
        node.setContext(&test.m_node);
        client_model = std::make_unique<ClientModel>(node, &options_model);
        for (const std::string& name : names) {
            wallets.push_back(MakeWallet(test, name));
            AddWallet(wallets.back());
            wallet_models.push_back(std::make_unique<WalletModel>(interfaces::MakeWallet(wallets.back()), *client_model, tests.platformStyle()));
            RemoveWallet(wallets.back(), nullopt);
        }
    }
};

//! A main window's title on regtest: program, then the current wallet's name if it has one, then the network.
QString Title(const QString& program, const QString& wallet = {})
{
    return program + (wallet.isEmpty() ? QString{} : " - " + wallet) + " - [regtest]";
}

//! What makes a main window ferrite-qt's: its central widget, the actions of its action groups and those on its
//! toolbars (not counting the widgets there), as text.
QString Describe(const BitcoinGUI& window)
{
    QStringList parts{QString{"central widget: "} + (window.centralWidget() ? window.centralWidget()->metaObject()->className() : "none")};
    for (const QActionGroup* group : window.findChildren<QActionGroup*>()) {
        QStringList actions;
        for (const QAction* action : group->actions()) actions << action->text();
        parts << QString{group->isExclusive() ? "exclusive group: " : "group: "} + actions.join(", ");
    }
    for (const QToolBar* toolbar : window.findChildren<QToolBar*>()) {
        QStringList actions;
        for (const QAction* action : toolbar->actions()) {
            if (!qobject_cast<const QWidgetAction*>(action)) actions << action->text();
        }
        parts << "toolbar: " + actions.join(", ");
    }
    return parts.join("; ");
}

//! A main window's title and its make-up (Describe) as made, with the first wallet, after switching to the second,
//! after closing the second and after closing the first.
QStringList WindowStates(FextHookTests& tests, WalletFixture& fixture)
{
    BitcoinGUI& window{tests.makeWindow()};
    QStringList states{window.windowTitle() + " | " + Describe(window)};
    // The ClientModel the window would get when the node has started, as far as the wallet frame needs it.
    WalletFrame* frame{window.findChild<WalletFrame*>()};
    frame->setClientModel(fixture.client_model.get());
    window.addWallet(fixture.wallet_models[0].get());
    states << window.windowTitle() + " | " + Describe(window);
    window.addWallet(fixture.wallet_models[1].get());
    window.setCurrentWallet(fixture.wallet_models[1].get());
    states << window.windowTitle() + " | " + Describe(window);
    window.removeWallet(fixture.wallet_models[1].get());
    states << window.windowTitle() + " | " + Describe(window);
    window.removeWallet(fixture.wallet_models[0].get());
    states << window.windowTitle() + " | " + Describe(window);
    // The window outlives the fixture.
    frame->setClientModel(nullptr);
    return states;
}

//! The command-line help's text, as its dialog shows it.
QString HelpText()
{
    HelpMessageDialog help(nullptr, false);
    return help.findChild<QTextEdit*>("helpMessage")->toPlainText();
}

//! The About box's text.
QString AboutText()
{
    HelpMessageDialog about(nullptr, true);
    return about.findChild<QLabel*>("aboutMessage")->text();
}

//! The object names of the items of the Send page's top-level layout.
QStringList LayoutItems(const SendCoinsDialog& page)
{
    QStringList items;
    for (int i = 0; i < page.layout()->count(); ++i) {
        QLayoutItem* item = page.layout()->itemAt(i);
        items << (item->widget() ? item->widget()->objectName() : item->layout() ? item->layout()->objectName() : QString{"spacer"});
    }
    return items;
}

//! The Send page's entries.
std::vector<SendCoinsEntry*> Entries(const SendCoinsDialog& page)
{
    std::vector<SendCoinsEntry*> entries;
    const QVBoxLayout* layout = page.findChild<QVBoxLayout*>("entries");
    for (int i = 0; i < layout->count(); ++i) {
        if (SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(layout->itemAt(i)->widget())) entries.push_back(entry);
    }
    return entries;
}

//! Types a payment into the Send page's first entry.
void FillPayment(SendCoinsDialog& page, const CTxDestination& address, CAmount amount)
{
    SendCoinsEntry* entry = Entries(page).at(0);
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(amount);
}

//! Sets a custom change address in the Send page's coin control section.
void SetChangeAddress(SendCoinsDialog& page, const CTxDestination& address)
{
    page.findChild<QValidatedLineEdit*>("lineEditCoinControlChange")->setText(QString::fromStdString(EncodeDestination(address)));
    page.findChild<QCheckBox*>("checkBoxCoinControlChange")->setChecked(true);
}

//! Sets the Send page's fee section to the recommended fee for a confirmation within target blocks, whatever
//! earlier runs left in the settings.
void UseRecommendedFee(SendCoinsDialog& page, unsigned int target)
{
    page.findChild<QRadioButton*>("radioSmartFee")->setChecked(true);
    QComboBox* targets{page.findChild<QComboBox*>("confTargetSelector")};
    const int index{targets->findText(QString{"(%1 blocks)"}.arg(target), Qt::MatchEndsWith)};
    assert(index >= 0);
    targets->setCurrentIndex(index);
}

//! Opens the Send page's coin control dialog, ticks the coin outpoint there (the first coin if outpoint is null) and
//! closes the dialog with OK. Returns the coin ticked, null if none was.
COutPoint SelectCoin(SendCoinsDialog& page, const COutPoint& outpoint)
{
    COutPoint ticked;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&outpoint, &ticked] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            CoinControlDialog* dialog = qobject_cast<CoinControlDialog*>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            // An item of a coin holds the coin's txid and output index in the address column (CoinControlDialog's
            // TxHashRole and VOutRole of COLUMN_ADDRESS); its check box is the first column.
            for (QTreeWidgetItemIterator it(dialog->findChild<QTreeWidget*>("treeWidget")); *it && ticked.IsNull(); ++it) {
                const QString txid{(*it)->data(3, Qt::UserRole).toString()};
                if (txid.isEmpty()) continue;
                const COutPoint coin{uint256S(txid.toStdString()), (*it)->data(3, Qt::UserRole + 1).toUInt()};
                if (!outpoint.IsNull() && coin != outpoint) continue;
                (*it)->setCheckState(0, Qt::Checked);
                ticked = coin;
            }
            dialog->findChild<QDialogButtonBox*>("buttonBox")->button(QDialogButtonBox::Ok)->click();
        }
    });
    timer.start(0);
    const bool invoked{QMetaObject::invokeMethod(&page, "coinControlButtonClicked")};
    assert(invoked);
    return ticked;
}

//! Presses Send on the page and answers its boxes as a BoxAnswerer does. Returns the boxes' texts; txid is set to
//! the transaction the wallet took in, null if none.
QStringList PressSend(CWallet& wallet, SendCoinsDialog& page, bool cancel, uint256& txid)
{
    txid.SetNull();
    boost::signals2::scoped_connection connection(wallet.NotifyTransactionChanged.connect([&txid](CWallet*, const uint256& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    BoxAnswerer answerer{cancel};
    const bool invoked{QMetaObject::invokeMethod(&page, "on_sendButton_clicked")};
    assert(invoked);
    return answerer.texts;
}

//! A transaction's outputs as (script, value) pairs, sorted, so that the place of the change, which the wallet
//! chooses at random, does not count.
std::vector<std::pair<CScript, CAmount>> SortedOutputs(const CTransaction& tx)
{
    std::vector<std::pair<CScript, CAmount>> outputs;
    for (const CTxOut& out : tx.vout) outputs.emplace_back(out.scriptPubKey, out.nValue);
    std::sort(outputs.begin(), outputs.end());
    return outputs;
}

//! A send confirmation's text without the size of the transaction, which differs by a byte between two signings
//! when one signature comes out a byte shorter. The fee does not depend on it.
QString WithoutSize(QString text)
{
    const QString start{"</b> ("};
    const int from{text.indexOf(start)};
    const int to{text.indexOf(" kB): ", from)};
    if (from >= 0 && to > from) text.remove(from + start.size(), to - from - start.size());
    return text;
}

//! The transaction of txid in the wallet, or null.
CTransactionRef WalletTx(CWallet& wallet, const uint256& txid)
{
    LOCK(wallet.cs_wallet);
    const CWalletTx* wtx = wallet.GetWalletTx(txid);
    return wtx ? wtx->tx : nullptr;
}

//! Sets the program up as its main does while it lives: the program is the record's GUI host, and the record's options
//! are registered in `args`. The program is no longer the GUI host after. The options stay in `args`, as an
//! ArgsManager cannot drop an option, so a scope on gArgs lives inside a testing setup, which clears gArgs as it ends.
class ProgramScope
{
public:
    explicit ProgramScope(ArgsManager& args)
    {
        opreturn::SetGuiHost();
        opreturn::AddArgs(args);
    }
    ~ProgramScope() { opreturn::SetGuiHost(false); }
};

//! The FEXT tab's choices for the record in the GUI settings while it lives (an invalid QVariant stores none), and
//! none after.
class StoredChoicesScope
{
public:
    StoredChoicesScope(const QVariant& record, const QVariant& max_size)
    {
        QSettings settings;
        Store(settings, "fext/record", record);
        Store(settings, "fext/maxSize", max_size);
    }
    ~StoredChoicesScope()
    {
        QSettings settings;
        settings.remove("fext/record");
        settings.remove("fext/maxSize");
    }

private:
    static void Store(QSettings& settings, const QString& key, const QVariant& value)
    {
        if (value.isValid()) {
            settings.setValue(key, value);
        } else {
            settings.remove(key);
        }
    }
};

/**
 * A node as GuiMain gives it to the extension's attachNode, with arguments of its own: the record's options, registered
 * as the program's main registers them (or, in a program that is not the record's GUI host, only registered), and this
 * command line. The chain clients that the hook adds are driven as init drives them, and are flushed, stopped and let
 * go of when it ends, as Shutdown does.
 */
class HookNode
{
    ArgsManager m_args;
    CScheduler m_scheduler;

public:
    NodeContext node;

    explicit HookNode(const std::vector<std::string>& command_line, bool gui_host = true, bool disable_wallet = false)
    {
        opreturn::SetGuiHost(gui_host);
        opreturn::AddArgs(m_args);
        std::vector<const char*> argv{"ferrite-qt-fext"};
        for (const std::string& arg : command_line) argv.push_back(arg.c_str());
        std::string error;
        const bool parsed{m_args.ParseParameters(int(argv.size()), argv.data(), error)};
        assert(parsed);
        if (disable_wallet) m_args.ForceSetArg("-disablewallet", "1");
        node.args = &m_args;
    }
    ~HookNode()
    {
        Stop();
        opreturn::SetGuiHost(false);
    }
    //! As init: verify() of every client, and when each verifies, load() and start() of each.
    bool Start()
    {
        for (const auto& client : node.chain_clients) {
            if (!client->verify()) return false;
        }
        for (const auto& client : node.chain_clients) {
            client->load();
            client->start(m_scheduler);
        }
        return true;
    }
    //! As Shutdown: flush() and stop() of every client, then let go of them.
    void Stop()
    {
        for (const auto& client : node.chain_clients) {
            client->flush();
            client->stop();
        }
        node.chain_clients.clear();
    }
};

//! Whether the record is kept. It reads only the blocks connected after its start, so it is BACKFILLING then.
bool Kept(const std::shared_ptr<interfaces::OpReturnRecord>& record)
{
    if (!record) return false;
    const opreturn::State state{record->getStatus().state};
    return state == opreturn::State::RUNNING || state == opreturn::State::BACKFILLING;
}

//! Waits until the condition holds, at most 20 seconds; whether it holds.
template <typename Condition>
bool WaitFor(Condition condition)
{
    for (int i = 0; i < 20000 && !condition(); ++i) QThread::msleep(1);
    return condition();
}

//! The record's options as the help dialog shows them: for each, the texts of its two cells, its name and its help
//! text on one line.
std::vector<std::pair<QString, QString>> RecordOptionCells()
{
    ArgsManager args;
    opreturn::AddArgs(args);
    std::vector<std::pair<QString, QString>> cells;
    for (const QString& line : QString::fromStdString(args.GetHelpMessage()).split('\n')) {
        if (line.startsWith("  -")) {
            cells.emplace_back(line.trimmed(), QString{});
        } else if (line.startsWith("   ") && !cells.empty()) {
            cells.back().second += (cells.back().second.isEmpty() ? QString{} : QString{" "}) + line.trimmed();
        }
    }
    return cells;
}
} // namespace

FextHookTests::FextHookTests(interfaces::Node& node)
    : m_node(node),
      m_platform_style(PlatformStyle::instantiate("other")),
      m_network_style(NetworkStyle::instantiate(CBaseChainParams::REGTEST))
{
}

FextHookTests::~FextHookTests() = default;

BitcoinGUI& FextHookTests::makeWindow()
{
    m_windows.push_back(std::make_unique<BitcoinGUI>(m_node, m_platform_style.get(), m_network_style.get()));
    // So that no message of the node reaches a window a test has done with.
    m_windows.back()->unsubscribeFromCoreSignals();
    return *m_windows.back();
}

//! The tests read, write and remove GUI settings, which the test program keeps in a temporary directory of its own and
//! not with the user's settings.
void FextHookTests::initTestCase()
{
    const QSettings settings;
    QCOMPARE(settings.format(), QSettings::IniFormat);
    QVERIFY2(settings.fileName().startsWith(QDir::tempPath() + "/test_ferrite-qt-fext-settings-"), qPrintable(settings.fileName()));
}

void FextHookTests::cleanupTestCase()
{
    m_windows.clear();
}

//! The window title starts "Ferrite Core FEXT" and the usage line of the command-line help names ferrite-qt-fext,
//! with any wallet current and with none; nothing else of the window, the help or the About box changes.
void FextHookTests::titleAndUsage()
{
    if (SkipOnMacMinimal()) return;
    // The program's main registers the record's options in gArgs; the test does so inside the fixture's testing
    // setup, which clears gArgs as it ends.
    {
        WalletFixture fixture(*this, {"w1", "w2"});

        // As ferrite-qt shows them.
        const QString plain_help{HelpText()};
        const QString plain_about{AboutText()};
        const QStringList plain_windows{WindowStates(*this, fixture)};
        const QString plain{PACKAGE_NAME};
        QCOMPARE(plain_windows, (QStringList{
            Title(plain) + " | " + PLAIN_WINDOW,
            Title(plain, "w1") + " | " + PLAIN_WINDOW,
            Title(plain, "w2") + " | " + PLAIN_WINDOW,
            Title(plain, "w1") + " | " + PLAIN_WINDOW,
            Title(plain) + " | " + PLAIN_WINDOW,
        }));

        FextExtension extension;
        ExtensionScope scope{&extension};

        // The help differs in its usage line and in the record's options, which the program's main registers: each option
        // is in the help once, with its help text, and every other line is ferrite-qt's.
        const ProgramScope program{gArgs};
        const QStringList help_lines{HelpText().split('\n')};
        const QStringList plain_help_lines{plain_help.split('\n')};
        const std::vector<std::pair<QString, QString>> options{RecordOptionCells()};
        QCOMPARE(int(options.size()), 3);
        std::vector<int> shown(options.size(), 0);
        int usage{0};
        int i{0};
        int j{0};
        while (i < help_lines.size()) {
            if (j < plain_help_lines.size() && help_lines[i] == plain_help_lines[j]) {
                ++i;
                ++j;
                continue;
            }
            const auto option = std::find_if(options.begin(), options.end(), [&](const std::pair<QString, QString>& cells) { return help_lines[i].trimmed() == cells.first; });
            if (option != options.end()) {
                QVERIFY(i + 1 < help_lines.size());
                QCOMPARE(help_lines[i + 1].trimmed(), option->second);
                ++shown[option - options.begin()];
                i += 2;
                continue;
            }
            QVERIFY(j < plain_help_lines.size());
            QCOMPARE(plain_help_lines[j].trimmed(), QString{"Usage:  ferrite-qt [command-line options]"});
            QCOMPARE(help_lines[i].trimmed(), QString{"Usage:  ferrite-qt-fext [command-line options]"});
            ++usage;
            ++i;
            ++j;
        }
        QCOMPARE(j, plain_help_lines.size());
        QCOMPARE(usage, 1);
        QVERIFY(shown == std::vector<int>(options.size(), 1));
        QCOMPARE(options[0].first, QString{"-opreturnindex"});
        QCOMPARE(options[1].first, QString{"-opreturnmaxsize=<n>"});
        QCOMPARE(options[2].first, QString{"-opreturnreindex"});
        QCOMPARE(AboutText(), plain_about);

        // Each title is ferrite-qt's with "FEXT" after the program's name, and the window is otherwise the same.
        const QString fext{"Ferrite Core FEXT"};
        QCOMPARE(WindowStates(*this, fixture), (QStringList{
            Title(fext) + " | " + PLAIN_WINDOW,
            Title(fext, "w1") + " | " + PLAIN_WINDOW,
            Title(fext, "w2") + " | " + PLAIN_WINDOW,
            Title(fext, "w1") + " | " + PLAIN_WINDOW,
            Title(fext) + " | " + PLAIN_WINDOW,
        }));

        // Under -disablewallet the window has no wallet frame and no toolbar, as ferrite-qt's has not, and the title
        // still names the program. The later tests have the wallet again whether or not these checks pass.
        {
            const WalletDisabled disabled;
            BitcoinGUI& window{makeWindow()};
            QCOMPARE(window.windowTitle(), Title(fext));
            QCOMPARE(Describe(window), QString{"central widget: RPCConsole; exclusive group: &Overview, &Send, &Receive, &Transactions"});
        }
    }
    // So the test leaves none of the options registered, and the process can register them again (AddArg stops at a
    // second registration of an option).
    for (const char* name : {"-opreturnindex", "-opreturnmaxsize", "-opreturnreindex", "-fextindex", "-fextmaxsize", "-fextreindex"}) {
        QVERIFY2(!gArgs.GetArgFlags(name), name);
    }
}

//! With the extension, the Send page is ferrite-qt's: the same form, the same confirmation, and the transaction
//! that WalletModel::prepareTransaction builds, with the coin and the change address chosen in coin control; it is
//! cleared after sending as before, and fee bumping works as before.
void FextHookTests::sendPageHooksInert()
{
    if (SkipOnMacMinimal()) return;
    WalletFixture fixture(*this, {""});
    WalletModel& wallet_model = *fixture.wallet_models[0];
    CWallet& wallet = *fixture.wallets[0];
    CKey payee_key;
    payee_key.MakeNewKey(true);
    const CTxDestination payee{PKHash(payee_key.GetPubKey())};
    const CTxDestination change{PKHash(fixture.test.coinbaseKey.GetPubKey())};
    const unsigned int target{wallet_model.wallet().getConfirmTarget()};

    // The payment as ferrite-qt prepares it: 10 FEC from one coin ticked in coin control, the change to an
    // address of the wallet set there; the confirmation is cancelled.
    SendCoinsDialog plain_page(platformStyle());
    plain_page.setModel(&wallet_model);
    QCOMPARE(LayoutItems(plain_page), PLAIN_SEND_PAGE);
    FillPayment(plain_page, payee, 10 * COIN);
    SetChangeAddress(plain_page, change);
    UseRecommendedFee(plain_page, target);
    const COutPoint coin{SelectCoin(plain_page, {})};
    QVERIFY(!coin.IsNull());
    uint256 txid;
    const QStringList plain_boxes{PressSend(wallet, plain_page, true /* cancel */, txid)};
    QCOMPARE(plain_boxes.size(), 1);
    QVERIFY(txid.IsNull());

    FextExtension extension;
    ExtensionScope scope{&extension};
    SendCoinsDialog page(platformStyle());
    page.setModel(&wallet_model);
    QCOMPARE(LayoutItems(page), PLAIN_SEND_PAGE);
    FillPayment(page, payee, 10 * COIN);
    SetChangeAddress(page, change);
    UseRecommendedFee(page, target);
    QCOMPARE(SelectCoin(page, coin), coin);

    // What WalletModel::prepareTransaction builds for the same payment and the same coin control.
    SendCoinsRecipient recipient{QString::fromStdString(EncodeDestination(payee)), "", 10 * COIN, ""};
    recipient.type = SendCoinsRecipient::REGULAR;
    WalletModelTransaction expected({recipient});
    CCoinControl coin_control;
    coin_control.Select(coin);
    coin_control.destChange = change;
    coin_control.m_confirm_target = target;
    QCOMPARE(wallet_model.prepareTransaction(expected, coin_control).status, WalletModel::OK);

    // The page with the extension shows the same confirmation and sends exactly that transaction.
    const QStringList boxes{PressSend(wallet, page, false /* cancel */, txid)};
    QCOMPARE(boxes.size(), 1);
    QVERIFY(boxes[0].contains("Transaction fee"));
    QCOMPARE(WithoutSize(boxes[0]), WithoutSize(plain_boxes[0]));
    QVERIFY(!txid.IsNull());
    const CTransactionRef sent{WalletTx(wallet, txid)};
    QVERIFY(sent);
    QCOMPARE(int(sent->vin.size()), 1);
    QVERIFY(sent->vin[0].prevout == coin);
    QVERIFY(SortedOutputs(*sent) == SortedOutputs(*expected.getWtx()));
    QCOMPARE(int(expected.getWtx()->vout.size()), 2);
    CAmount sent_out{0};
    for (const CTxOut& out : sent->vout) sent_out += out.nValue;
    QCOMPARE(WalletTx(wallet, coin.hash)->vout[coin.n].nValue - sent_out, expected.getTransactionFee());

    // After sending, the page is cleared as before: one empty entry, no coin control choices.
    QCOMPARE(int(Entries(page).size()), 1);
    QVERIFY(Entries(page)[0]->findChild<QValidatedLineEdit*>("payTo")->text().isEmpty());
    QVERIFY(!page.findChild<QCheckBox*>("checkBoxCoinControlChange")->isChecked());
    QVERIFY(page.findChild<QValidatedLineEdit*>("lineEditCoinControlChange")->text().isEmpty());

    // Fee bumping goes through no hook: a transaction that signals replaceability is bumped as without the
    // extension.
    CCoinControl replaceable;
    replaceable.m_signal_bip125_rbf = true;
    int change_pos{-1};
    CAmount fee{0};
    bilingual_str error;
    const CTransactionRef original{wallet_model.wallet().createTransaction({CRecipient{payee, 3 * COIN, false}}, replaceable, true /* sign */, change_pos, fee, error)};
    QVERIFY(original);
    wallet_model.wallet().commitTransaction(original, {} /* value_map */, {} /* order_form */, {} /* reserved_keys */);
    QVERIFY(wallet_model.wallet().transactionCanBeBumped(original->GetHash()));
    uint256 bumped;
    {
        BoxAnswerer answerer;
        QVERIFY(wallet_model.bumpFee(original->GetHash(), bumped));
        QCOMPARE(answerer.texts.size(), 1);
        QVERIFY(answerer.texts[0].startsWith("Do you want to increase the fee?"));
    }
    const CTransactionRef replacement{WalletTx(wallet, bumped)};
    QVERIFY(replacement);
    QVERIFY(bumped != original->GetHash());
    QVERIFY(replacement->vin.size() == original->vin.size());
    for (size_t i = 0; i < original->vin.size(); ++i) QVERIFY(replacement->vin[i].prevout == original->vin[i].prevout);
}

//! A hook that throws lets nothing out into the plain program: the window, the help, the Send page and its
//! clearing work as in ferrite-qt, and the Send page's transaction fails and nothing is sent. Each such error
//! writes one line to the log.
void FextHookTests::throwingHooks()
{
    if (SkipOnMacMinimal()) return;
    WalletFixture fixture(*this, {""});
    WalletModel& wallet_model = *fixture.wallet_models[0];
    CWallet& wallet = *fixture.wallets[0];
    LogCapture log;
    ThrowingExtension extension;
    ExtensionScope scope{&extension};

    extension.arm(Hook::ATTACH_NODE);
    extension.attachNode(fixture.test.m_node);
    QVERIFY(!extension.armed());

    // A window made while one of its hooks throws is ferrite-qt's: the bare wallet frame in the middle, the four
    // tab actions, and ferrite-qt's title if the title hook threw.
    for (const Hook hook : {Hook::WRAP_CENTRAL_WIDGET, Hook::ADD_TAB_ACTIONS, Hook::ADD_TOOL_BAR_ACTIONS, Hook::WINDOW_TITLE}) {
        extension.arm(hook);
        BitcoinGUI& window{makeWindow()};
        QVERIFY(!extension.armed());
        QCOMPARE(Describe(window), PLAIN_WINDOW);
        QCOMPARE(window.windowTitle(), Title(hook == Hook::WINDOW_TITLE ? PACKAGE_NAME : "Ferrite Core FEXT"));
    }

    // The help shows ferrite-qt's usage line.
    extension.arm(Hook::USAGE_HEADER);
    QVERIFY(HelpText().contains("\nUsage:  ferrite-qt [command-line options] "));
    QVERIFY(!extension.armed());

    // A Send page made while its hook throws is ferrite-qt's.
    extension.arm(Hook::EXTEND_SEND_PAGE);
    SendCoinsDialog page(platformStyle());
    QVERIFY(!extension.armed());
    page.setModel(&wallet_model);
    QCOMPARE(LayoutItems(page), PLAIN_SEND_PAGE);
    QStringList messages;
    QObject::connect(&page, &SendCoinsDialog::message, [&messages](const QString&, const QString& message, unsigned int) { messages << message; });
    CKey payee_key;
    payee_key.MakeNewKey(true);
    const CTxDestination payee{PKHash(payee_key.GetPubKey())};

    // When the transaction cannot be made because the hook threw, the page says so, asks nothing and sends nothing.
    FillPayment(page, payee, 5 * COIN);
    extension.arm(Hook::PREPARE_SEND);
    uint256 txid;
    QCOMPARE(PressSend(wallet, page, false, txid), QStringList{});
    QVERIFY(!extension.armed());
    QVERIFY(txid.IsNull());
    QCOMPARE(messages, QStringList{"Transaction creation failed!"});
    QCOMPARE(int(Entries(page).size()), 1);

    // When the confirmation's hook throws, the confirmation shows what ferrite-qt's does, and the payment is sent.
    extension.arm(Hook::DESCRIBE_SEND);
    const QStringList boxes{PressSend(wallet, page, false, txid)};
    QVERIFY(!extension.armed());
    QCOMPARE(boxes.size(), 1);
    QVERIFY(boxes[0].startsWith("Are you sure you want to send?"));
    QVERIFY(!txid.IsNull());
    QVERIFY(WalletTx(wallet, txid));
    QCOMPARE(messages.size(), 1);

    // When the clearing hook throws, the page is cleared all the same.
    FillPayment(page, payee, 7 * COIN);
    page.addEntry();
    QCOMPARE(int(Entries(page).size()), 2);
    extension.arm(Hook::CLEAR_SEND_PAGE);
    page.clear();
    QVERIFY(!extension.armed());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(int(Entries(page).size()), 1);
    QVERIFY(Entries(page)[0]->findChild<QValidatedLineEdit*>("payTo")->text().isEmpty());

    // One line in the log for each hook that threw.
    QCOMPARE(extension.thrown(), 10);
    for (const char* name : {"attachNode", "wrapCentralWidget", "addTabActions", "addToolBarActions", "windowTitle",
             "usageHeader", "extendSendPage", "prepareSend", "describeSend", "clearSendPage"}) {
        QCOMPARE(log.count(strprintf("FEXT: an unexpected error in the %s hook was caught\n", name)), 1);
    }
}

//! GuiMain, run as a program of its own with a data directory of its own, calls the extension's attachNode on the GUI
//! thread once the node's base initialization has succeeded and before it starts the node. It has made the options
//! model by then, so with -resetguisettings the choice that the FEXT tab stored is gone when attachNode reads it.
void FextHookTests::attachNodeCall()
{
    if (SkipOnMacMinimal()) return;
    struct Case {
        QStringList args;
        QVariant stored_record; //!< the tab's stored choice for the record in the GUI settings; invalid: none
        QString read;           //!< the stored choice that attachNode reads
    };
    for (const Case& c : std::vector<Case>{
             {{}, QVariant{}, "none"},
             {{}, QVariant{false}, "off"},
             {{"-resetguisettings"}, QVariant{false}, "none"},
         }) {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        if (c.stored_record.isValid()) {
            // Where GuiMain keeps its settings on regtest: under its organization, in a file named after the network.
            QSettings settings{dir.filePath("settings/" QAPP_ORG_NAME "/" QAPP_APP_NAME_REGTEST ".ini"), QSettings::IniFormat};
            settings.setValue("fext/record", c.stored_record);
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
        }
        QProcess program;
        QProcessEnvironment environment{QProcessEnvironment::systemEnvironment()};
        environment.insert(GUI_MAIN_SETTINGS_VARIABLE, dir.filePath("settings"));
        program.setProcessEnvironment(environment);
        // Were attachNode not called, the node would start, listening nowhere, and stop again after loading its chain.
        program.start(QCoreApplication::applicationFilePath(), QStringList{"-regtest", "-datadir=" + dir.path(), "-splash=0", "-listen=0", "-stopafterblockimport"} + c.args);
        QVERIFY(program.waitForStarted());
        const bool finished{program.waitForFinished(60 * 1000)};
        if (!finished) {
            program.kill();
            program.waitForFinished();
        }
        const QString output{QString::fromLocal8Bit(program.readAllStandardOutput()).trimmed()};
        QVERIFY2(finished, qPrintable("the program did not end; it wrote: " + output));
        QCOMPARE(program.exitStatus(), QProcess::NormalExit);
        QCOMPARE(output, "attachNode: on the GUI thread, after the base initialization, before the node starts\nthe stored choice for the record: " + c.read);
        QCOMPARE(program.exitCode(), EXIT_SUCCESS);
    }
}

//! The program's main registers the record's options before GuiMain parses the arguments, and makes the program the
//! record's GUI host. The help shows the options under their first names only; the second names work as the first.
void FextHookTests::optionsRegistered()
{
    // A data directory of the test's own for the record that the last check makes.
    const BasicTestingSetup setup{CBaseChainParams::REGTEST};
    ArgsManager args;
    const ProgramScope program{args};
    const std::string help{args.GetHelpMessage()};
    for (const char* name : {"-opreturnindex", "-opreturnmaxsize=<n>", "-opreturnreindex"}) {
        QVERIFY2(help.find(std::string{"\n  "} + name + "\n") != std::string::npos, name);
    }
    QVERIFY(help.find("-fext") == std::string::npos);
    const char* argv[]{"ferrite-qt-fext", "-opreturnindex=0", "-fextmaxsize=2048", "-noopreturnreindex", "-nofextreindex"};
    std::string error;
    QVERIFY2(args.ParseParameters(5, argv, error), error.c_str());
    QVERIFY(args.IsArgSet("-fextmaxsize"));
    // As the GUI host, the program gets the record's client also while the record is off.
    NodeContext node;
    node.args = &args;
    QVERIFY(opreturn::Construct(node, {}));
    QCOMPARE(int(node.chain_clients.size()), 1);
    QVERIFY(opreturn::GetRecord());
    for (const auto& client : node.chain_clients) client->stop();
    node.chain_clients.clear();
    QVERIFY(!opreturn::GetRecord());
}

//! At the hook after the node's base initialization the extension makes the record with the options the user set and,
//! where none is set, the choices the FEXT tab stored. A stored choice of another type counts as none; a stored size
//! limit that may not be set is ignored with a line in the log; an option value that may not be set stops start-up at
//! verify().
void FextHookTests::resolveAtH2()
{
    TestChain100Setup test;
    FextExtension extension;
    const std::string ignored{strprintf("%s the stored choice for -opreturnmaxsize is out of range and is ignored", opreturn::LOG_PREFIX)};
    struct Case {
        std::vector<std::string> command_line;
        QVariant record;   //!< stored as fext/record; invalid: none
        QVariant max_size; //!< stored as fext/maxSize; invalid: none
        bool kept;
        int explicit_options;
        int ignored_lines;
    };
    const QVariant none;
    for (const Case& c : std::vector<Case>{
             {{}, none, none, true, 0, 0},
             {{}, QVariant{false}, none, false, 0, 0},
             {{}, QVariant{QString{"false"}}, none, false, 0, 0},
             {{}, QVariant{true}, none, true, 0, 0},
             {{}, QVariant{QString{"true"}}, none, true, 0, 0},
             // An option set explicitly wins, under either name.
             {{"-opreturnindex=1"}, QVariant{false}, none, true, opreturn::EXPLICIT_RECORD, 0},
             {{"-fextindex=1"}, QVariant{QString{"false"}}, none, true, opreturn::EXPLICIT_RECORD, 0},
             {{"-noopreturnindex"}, QVariant{true}, none, false, opreturn::EXPLICIT_RECORD, 0},
             // A choice of another type, or other text, is none.
             {{}, QVariant{0}, none, true, 0, 0},
             {{}, QVariant{QString{"False"}}, none, true, 0, 0},
             // The size limit: one that may not be set, as a number or as its text, is ignored and logged; with the
             // option set, the stored one is not read; one that is not a whole number is none.
             {{}, none, QVariant{50}, true, 0, 1},
             {{}, none, QVariant{QString{"50"}}, true, 0, 1},
             {{}, none, QVariant{qulonglong{50}}, true, 0, 1},
             {{"-opreturnmaxsize=2048"}, none, QVariant{50}, true, opreturn::EXPLICIT_MAXSIZE, 0},
             {{}, none, QVariant{QString{"2048 MiB"}}, true, 0, 0},
             {{}, none, QVariant{2048.0}, true, 0, 0},
             {{}, none, QVariant{2048}, true, 0, 0},
         }) {
        const StoredChoicesScope stored{c.record, c.max_size};
        HookNode host{c.command_line};
        LogCapture log;
        extension.attachNode(host.node);
        QCOMPARE(int(host.node.chain_clients.size()), 1);
        QVERIFY(host.Start());
        const std::shared_ptr<interfaces::OpReturnRecord> record{opreturn::GetRecord()};
        QVERIFY(record);
        if (c.kept) {
            QVERIFY(WaitFor([&] { return Kept(record); }));
        } else {
            QVERIFY(record->getStatus().state == opreturn::State::OFF);
        }
        QCOMPARE(int(record->getStatus().explicit_options), c.explicit_options);
        QCOMPARE(log.count(ignored), c.ignored_lines);
    }

    // The choices as the extension reads them.
    {
        const StoredChoicesScope stored{QVariant{QString{"false"}}, QVariant{QString{"+4096"}}};
        const opreturn::StoredChoices choices{FextExtension::ReadStoredChoices(QSettings{})};
        QVERIFY(choices.record && !*choices.record);
        QVERIFY(choices.max_size_mib && *choices.max_size_mib == 4096);
    }
    for (const QVariant& max_size : {QVariant{std::numeric_limits<qulonglong>::max()}, QVariant{2048.0}, QVariant{QString{"2048 MiB"}}}) {
        const StoredChoicesScope stored{QVariant{1}, max_size};
        const opreturn::StoredChoices choices{FextExtension::ReadStoredChoices(QSettings{})};
        QVERIFY(!choices.record && !choices.max_size_mib);
    }

    // A value that may not be set stops start-up, with the error, and there is no record, also while the record is off.
    for (const auto& c : std::vector<std::pair<std::vector<std::string>, std::string>>{
             {{"-opreturnmaxsize=50"}, "-opreturnmaxsize must be 0 (no limit) or from 100 to 4194304 MiB"},
             {{"-opreturnindex=0", "-opreturnmaxsize=50"}, "-opreturnmaxsize must be 0 (no limit) or from 100 to 4194304 MiB"},
             {{"-opreturnindex=1", "-fextindex=0"}, "-fextindex is another name for -opreturnindex, and the two are set to different values: remove -fextindex"},
         }) {
        const StoredChoicesScope stored{QVariant{true}, QVariant{2048}};
        HookNode host{c.first};
        LogCapture log;
        extension.attachNode(host.node);
        QCOMPARE(int(host.node.chain_clients.size()), 1);
        QVERIFY(!host.Start());
        QVERIFY(!opreturn::GetRecord());
        QCOMPARE(log.count("Error: " + c.second + "\n"), 1);
    }
}

//! Without a wallet the program has no FEXT tab, so the record is off whatever the tab stored, unless its option is
//! set. (titleAndUsage checks that the window then is ferrite-qt's without a wallet.)
void FextHookTests::noTabWithoutWallet()
{
    TestChain100Setup test;
    FextExtension extension;
    for (const QVariant& stored_record : {QVariant{}, QVariant{true}}) {
        const StoredChoicesScope stored{stored_record, QVariant{}};
        HookNode host{{}, /*gui_host=*/true, /*disable_wallet=*/true};
        extension.attachNode(host.node);
        QVERIFY(host.Start());
        QVERIFY(opreturn::GetRecord());
        QVERIFY(opreturn::GetRecord()->getStatus().state == opreturn::State::OFF);
    }
    const StoredChoicesScope stored{QVariant{false}, QVariant{}};
    HookNode host{{"-opreturnindex=1"}, /*gui_host=*/true, /*disable_wallet=*/true};
    extension.attachNode(host.node);
    QVERIFY(host.Start());
    QVERIFY(WaitFor([] { return Kept(opreturn::GetRecord()); }));
}

//! A window made with the extension and never given the node has no record, and stays ferrite-qt's; when the hook
//! cannot make the record, the program goes on without one, and the log says so.
void FextHookTests::pageWithoutRecord()
{
    if (SkipOnMacMinimal()) return;
    TestChain100Setup test;
    node().setContext(&test.m_node);
    FextExtension extension;
    const ExtensionScope scope{&extension};
    BitcoinGUI& window{makeWindow()};
    QCOMPARE(Describe(window), PLAIN_WINDOW);
    QVERIFY(!opreturn::GetRecord());
    // The node's base initialization gave no arguments: the record cannot be made.
    NodeContext node;
    LogCapture log;
    extension.attachNode(node);
    QVERIFY(node.chain_clients.empty());
    QVERIFY(!opreturn::GetRecord());
    QCOMPARE(log.count("FEXT: the OP_RETURN index could not be made\n"), 1);
    QCOMPARE(log.count("an unexpected error in the attachNode hook"), 0);
    QCOMPARE(Describe(window), PLAIN_WINDOW);
}

//! The record that the hook makes belongs to the node's chain client, not to the extension: when the extension goes
//! without aboutToQuit, as on GuiMain's early exits, the record runs on, and init's shutdown stops it through its
//! client.
void FextHookTests::destroyWithoutAboutToQuit()
{
    TestChain100Setup test;
    HookNode host{{}};
    {
        auto extension = std::make_unique<FextExtension>();
        const ExtensionScope scope{extension.get()};
        extension->attachNode(host.node);
    }
    QVERIFY(host.Start());
    QVERIFY(WaitFor([] { return Kept(opreturn::GetRecord()); }));
    // It still follows the chain.
    const int height{WITH_LOCK(cs_main, return ::ChainActive().Height()) + 1};
    test.CreateAndProcessBlock({}, CScript() << OP_RETURN << std::vector<unsigned char>{'h', 'i'});
    SyncWithValidationInterfaceQueue();
    QVERIFY(WaitFor([height] {
        const std::shared_ptr<interfaces::OpReturnRecord> record{opreturn::GetRecord()};
        return record && record->getVisibility()->live_tip == height;
    }));
    opreturn::RowView row;
    QVERIFY(opreturn::GetRecord()->readRow({height, 0}, row, nullptr) == opreturn::ReadResult::FOUND);
    // Shutdown stops it and lets go of it.
    const std::weak_ptr<interfaces::OpReturnRecord> weak{opreturn::GetRecord()};
    for (const auto& client : host.node.chain_clients) {
        client->flush();
        client->stop();
    }
    QVERIFY(weak.lock()->getStatus().state == opreturn::State::STOPPED);
    host.Stop();
    QVERIFY(weak.expired());
}

//! As the record's GUI host the program gets the record's client also while the record is off, so that the tab can
//! switch it on; a program that is not the GUI host would get nothing at that hook while the record is off.
void FextHookTests::recordConstructedForGuiHost()
{
    TestChain100Setup test;
    FextExtension extension;
    {
        HookNode host{{"-opreturnindex=0"}};
        extension.attachNode(host.node);
        QCOMPARE(int(host.node.chain_clients.size()), 1);
        QVERIFY(host.Start());
        const std::shared_ptr<interfaces::OpReturnRecord> record{opreturn::GetRecord()};
        QVERIFY(record);
        QVERIFY(record->getStatus().state == opreturn::State::OFF);
        record->setRecordEnabled(true);
        QVERIFY(WaitFor([&] { return Kept(record); }));
    }
    {
        HookNode host{{"-opreturnindex=0"}, /*gui_host=*/false};
        extension.attachNode(host.node);
        QVERIFY(host.node.chain_clients.empty());
        QVERIFY(!opreturn::GetRecord());
    }
    HookNode host{{"-opreturnindex=1"}, /*gui_host=*/false};
    extension.attachNode(host.node);
    QCOMPARE(int(host.node.chain_clients.size()), 1);
    QVERIFY(host.Start());
    QVERIFY(WaitFor([] { return Kept(opreturn::GetRecord()); }));
}

int GuiMainReportingAttachNode(int argc, char* argv[], const char* settings_dir)
{
    // GuiMain's settings go to the test's directory, not to the user's settings.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromLocal8Bit(settings_dir));
    static AttachNodeReporter reporter;
    g_gui_extension = &reporter;
    return GuiMain(argc, argv);
}
