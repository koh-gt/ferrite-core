// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_FEXT_FEXTEXTENSION_H
#define BITCOIN_QT_FEXT_FEXTEXTENSION_H

#include <qt/guiextension.h>

#ifndef ENABLE_WALLET
#error "ferrite-qt-fext is built only with the wallet"
#endif

/**
 * What ferrite-qt-fext adds to the GUI of ferrite-qt. For now that is its name, in the window title and in the usage
 * line of the command-line help; every other hook leaves the program as ferrite-qt is, and the Send page builds its
 * transaction with WalletModel::prepareTransaction, as ferrite-qt does.
 *
 * No exception leaves a hook: each body runs inside a guard that catches every exception, writes one line to the
 * log and then gives what ferrite-qt would have: the bare wallet frame, the title or usage line it was given,
 * TransactionCreationFailed for the Send page's transaction, and nothing for the other hooks.
 */
class FextExtension : public GuiExtension
{
public:
    //! The hooks, in the order of GuiExtension. A hook's name in the log comes from this order.
    enum class Hook {
        ATTACH_NODE,
        WRAP_CENTRAL_WIDGET,
        ADD_TAB_ACTIONS,
        ADD_TOOL_BAR_ACTIONS,
        WINDOW_TITLE,
        USAGE_HEADER,
        EXTEND_SEND_PAGE,
        PREPARE_SEND,
        DESCRIBE_SEND,
        CLEAR_SEND_PAGE,
        //! Not a hook: the number of hooks. It stays last.
        COUNT,
    };

    void attachNode(NodeContext& node) override;
    QWidget* wrapCentralWidget(BitcoinGUI& gui, WalletFrame& frame) override;
    void addTabActions(BitcoinGUI& gui, QActionGroup& tabs) override;
    void addToolBarActions(BitcoinGUI& gui, QToolBar& toolbar) override;
    QString windowTitle(const QString& plain_title) override;
    QString usageHeader(const QString& plain_header) override;
    void extendSendPage(SendCoinsDialog& dialog, QBoxLayout& layout, int index) override;
    WalletModel::SendCoinsReturn prepareSend(SendCoinsDialog& dialog, WalletModel& model,
        WalletModelTransaction& transaction, const CCoinControl& coin_control) override;
    void describeSend(SendCoinsDialog& dialog, WalletModelTransaction& transaction,
        QString& question, QString& informative, QString& detailed) override;
    void clearSendPage(SendCoinsDialog& dialog) override;

protected:
    //! Called first inside each hook's guard. It does nothing; a test overrides it to make a hook throw.
    virtual void enterHook(Hook hook) {}

private:
    template <typename Result, typename Body>
    Result guarded(Hook hook, Result fallback, Body body);
    template <typename Body>
    void guarded(Hook hook, Body body);
};

#endif // BITCOIN_QT_FEXT_FEXTEXTENSION_H
