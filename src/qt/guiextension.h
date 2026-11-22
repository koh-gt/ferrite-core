// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_GUIEXTENSION_H
#define BITCOIN_QT_GUIEXTENSION_H

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#ifdef ENABLE_WALLET
#include <qt/walletmodel.h>
#endif

#include <QtGlobal>

class BitcoinGUI;
class CCoinControl;
class SendCoinsDialog;
class WalletFrame;
class WalletModelTransaction;
struct NodeContext;

QT_BEGIN_NAMESPACE
class QActionGroup;
class QBoxLayout;
class QString;
class QToolBar;
class QWidget;
QT_END_NAMESPACE

/**
 * Optional additions to the GUI, for a program built from this tree that adds to it. No program sets one by
 * default, so g_gui_extension is null and each place that would call it runs exactly as it does without it. A
 * program's main() may set it before calling GuiMain(). Every method is called on the GUI thread, and none may
 * let an exception out.
 */
class GuiExtension
{
public:
    virtual ~GuiExtension() {}

    //! After the node's base initialization and before the node is started.
    virtual void attachNode(NodeContext& node) = 0;
    //! The main window's central widget, given the wallet frame that would be: the widget returned is shown
    //! there instead, and must hold the frame.
    virtual QWidget* wrapCentralWidget(BitcoinGUI& gui, WalletFrame& frame) = 0;
    //! After the main window's tab actions are put in their exclusive group.
    virtual void addTabActions(BitcoinGUI& gui, QActionGroup& tabs) = 0;
    //! After the tab actions are put on the main window's toolbar.
    virtual void addToolBarActions(BitcoinGUI& gui, QToolBar& toolbar) = 0;
    //! The main window's title, given the title the window would show.
    virtual QString windowTitle(const QString& plain_title) = 0;
    //! The usage line of the command-line help, given the line the help would show.
    virtual QString usageHeader(const QString& plain_header) = 0;
#ifdef ENABLE_WALLET
    //! After the Send page's form is set up: layout is the page's top-level layout and index the place of its
    //! fee section in it.
    virtual void extendSendPage(SendCoinsDialog& dialog, QBoxLayout& layout, int index) = 0;
    //! Builds the Send page's transaction, in place of WalletModel::prepareTransaction.
    virtual WalletModel::SendCoinsReturn prepareSend(SendCoinsDialog& dialog, WalletModel& model,
        WalletModelTransaction& transaction, const CCoinControl& coin_control) = 0;
    //! After the texts of the Send page's confirmation are written, before it is shown.
    virtual void describeSend(SendCoinsDialog& dialog, WalletModelTransaction& transaction,
        QString& question, QString& informative, QString& detailed) = 0;
    //! First thing when the Send page is cleared.
    virtual void clearSendPage(SendCoinsDialog& dialog) = 0;
#endif // ENABLE_WALLET
};

/** The program's GUI extension: null unless the program's main() sets one. */
extern GuiExtension* g_gui_extension;

#endif // BITCOIN_QT_GUIEXTENSION_H
