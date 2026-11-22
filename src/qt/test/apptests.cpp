// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <chainparams.h>
#include <key.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/guiextension.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <qt/utilitydialog.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <validation.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#ifdef ENABLE_WALLET
#include <qt/walletframe.h>
#endif

#include <QAction>
#include <QActionGroup>
#include <QEventLoop>
#include <QLineEdit>
#include <QScopedPointer>
#include <QTest>
#include <QTextEdit>
#include <QToolBar>
#include <QWidgetAction>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

namespace {
//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QEventLoop loop;
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QObject::connect(messagesWidget, &QTextEdit::textChanged, &loop, &QEventLoop::quit);
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    loop.exec();
    QString output = messagesWidget->toPlainText();
    UniValue value;
    value.read(output.right(output.size() - output.lastIndexOf(QChar::ObjectReplacementCharacter) - 1).toStdString());
    QCOMPARE(value["chain"].get_str(), std::string("regtest"));
}
} // namespace

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_bitcoin-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif

    fs::create_directories([] {
        BasicTestingSetup test{CBaseChainParams::REGTEST}; // Create a temp data directory to backup the gui settings to
        return GetDataDir() / "blocks";
    }());

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    m_app.createOptionsModel(true /* reset settings */);
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    m_app.setupPlatformStyle();
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
    AbortShutdown();
    {
        LOCK(cs_main);
        UnloadBlockIndex(/* mempool */ nullptr, g_chainman);
        g_chainman.Reset();
    }
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};

    // No GUI extension is set, so the window is the plain one: the four tab
    // actions in their exclusive group, the same four on the toolbar, the wallet
    // frame as the central widget, the plain title and the plain usage line.
    QVERIFY(g_gui_extension == nullptr);
    const QList<QActionGroup*> groups = window->findChildren<QActionGroup*>();
    QCOMPARE(groups.size(), 1);
    QVERIFY(groups[0]->isExclusive());
    QStringList tabs;
    for (const QAction* tab : groups[0]->actions()) tabs << tab->text();
    QCOMPARE(tabs, (QStringList{"&Overview", "&Send", "&Receive", "&Transactions"}));
#ifdef ENABLE_WALLET
    const QToolBar* toolbar = window->findChild<QToolBar*>();
    QVERIFY(toolbar);
    QList<QAction*> buttons;
    for (QAction* button : toolbar->actions()) {
        if (!qobject_cast<QWidgetAction*>(button)) buttons << button;
    }
    QCOMPARE(buttons, groups[0]->actions());
    QVERIFY(qobject_cast<WalletFrame*>(window->centralWidget()));
#else
    QVERIFY(qobject_cast<RPCConsole*>(window->centralWidget()));
#endif
    QScopedPointer<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCOMPARE(window->windowTitle(), QString{PACKAGE_NAME} + " - " + style->getTitleAddText());
    const HelpMessageDialog* help = window->findChild<HelpMessageDialog*>();
    QVERIFY(help);
    QVERIFY(help->findChild<QTextEdit*>("helpMessage")->toPlainText().contains("\nUsage:  ferrite-qt [command-line options] "));

    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.quit();
    }
}
