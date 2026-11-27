// Copyright (c) 2009-2019 The Bitcoin Core developers
// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <interfaces/node.h>
#include <qt/bitcoin.h>
#include <qt/test/fexthooktests.h>
#include <test/util/setup_common.h>

#include <cstdlib>

#include <QApplication>
#include <QDir>
#include <QObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_MINIMAL)
Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin);
#endif
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};

// The tests of ferrite-qt-fext, run as test_main.cpp runs those of ferrite-qt.
int main(int argc, char* argv[])
{
    // Started by FextHookTests::attachNodeCall: run GuiMain instead of the tests.
    if (const char* settings_dir = std::getenv(GUI_MAIN_SETTINGS_VARIABLE)) {
        return GuiMainReportingAttachNode(argc, argv, settings_dir);
    }

    // Initialize persistent globals with the testing setup state for sanity, as test_main.cpp does. All tests must
    // use their own testing setup (if needed).
    {
        BasicTestingSetup dummy{CBaseChainParams::REGTEST};
    }

    NodeContext node_context;
    std::unique_ptr<interfaces::Node> node = interfaces::MakeNode(&node_context);

    bool fInvalid = false;

    // Prefer the "minimal" platform, as test_main.cpp does, but with the system's fonts, so that text is laid out
    // and drawn as on a screen.
    #if defined(WIN32)
        if (getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "minimal:enable_fonts");
    #else
        setenv("QT_QPA_PLATFORM", "minimal:enable_fonts", /* overwrite */ 0);
    #endif

    // The GUI settings that the tests read, write and remove are kept in a directory of their own, which outlives the
    // application and is removed at the end, so that the tests never read or change the user's settings.
    const QTemporaryDir settings_dir{QDir::tempPath() + "/test_ferrite-qt-fext-settings-XXXXXX"};
    if (!settings_dir.isValid()) {
        qCritical("No directory for the GUI settings: %s", qPrintable(settings_dir.errorString()));
        return EXIT_FAILURE;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());

    // Don't remove this, it's needed to access
    // QApplication:: and QCoreApplication:: in the tests
    BitcoinApplication app;
    app.setNode(*node);
    app.setApplicationName("Ferrite-Qt-fext-test");

    app.node().context()->args = &gArgs;     // Make gArgs available in the NodeContext
    FextHookTests hook_tests(app.node());
    if (QTest::qExec(&hook_tests) != 0) {
        fInvalid = true;
    }

    return fInvalid;
}
