// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_FEXTHOOKTESTS_H
#define BITCOIN_QT_TEST_FEXTHOOKTESTS_H

#include <QObject>

#include <memory>
#include <vector>

class BitcoinGUI;
class NetworkStyle;
class PlatformStyle;

namespace interfaces {
class Node;
} // namespace interfaces

/** The variable that FextHookTests::attachNodeCall sets in the environment of the program it starts, to the
 *  directory for that program's GUI settings. With it set, the test program runs GuiMainReportingAttachNode instead
 *  of the tests. */
constexpr char GUI_MAIN_SETTINGS_VARIABLE[] = "FEXT_HOOK_TESTS_GUI_MAIN_SETTINGS";

/** GuiMain with an extension whose attachNode writes to stdout whether GuiMain calls it on the GUI thread, after the
 *  node's base initialization and before the node is started, and the FEXT tab's stored choice for the record that it
 *  reads there, and then ends the program at once. The GUI settings go to settings_dir. */
int GuiMainReportingAttachNode(int argc, char* argv[], const char* settings_dir);

/** How ferrite-qt-fext's GUI extension changes the GUI of ferrite-qt. */
class FextHookTests : public QObject
{
    Q_OBJECT

public:
    explicit FextHookTests(interfaces::Node& node);
    ~FextHookTests();

    interfaces::Node& node() { return m_node; }
    const PlatformStyle* platformStyle() const { return m_platform_style.get(); }
    const NetworkStyle* networkStyle() const { return m_network_style.get(); }
    //! A new main window, kept until all the tests have run.
    BitcoinGUI& makeWindow();

private Q_SLOTS:
    void initTestCase();
    void titleAndUsage();
    void sendPageHooksInert();
    void throwingHooks();
    void attachNodeCall();
    void optionsRegistered();
    void resolveAtH2();
    void noTabWithoutWallet();
    void pageWithoutRecord();
    void destroyWithoutAboutToQuit();
    void recordConstructedForGuiHost();
    void cleanupTestCase();

private:
    interfaces::Node& m_node;
    std::unique_ptr<const PlatformStyle> m_platform_style;
    std::unique_ptr<const NetworkStyle> m_network_style;
    //! The main windows the tests made. A main window connects lambdas without a context object to
    //! QApplication::focusWindowChanged; they stay connected when the window is destroyed and would use freed memory
    //! at the next change of focus. So the windows are destroyed only after the last test.
    std::vector<std::unique_ptr<BitcoinGUI>> m_windows;
};

#endif // BITCOIN_QT_TEST_FEXTHOOKTESTS_H
