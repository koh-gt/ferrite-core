// Copyright (c) 2015-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/wallettests.h>
#include <qt/test/util.h>

#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <qt/bitcoinamountfield.h>
#include <qt/clientmodel.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/qvalidatedlineedit.h>
#include <qt/sendcoinsdialog.h>
#include <qt/sendcoinsentry.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <key_io.h>
#include <script/standard.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/txrecord.h>
#include <wallet/wallet.h>
#include <qt/overviewpage.h>
#include <qt/receivecoinsdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/receiverequestdialog.h>

#include <algorithm>
#include <memory>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QPushButton>
#include <QTimer>
#include <QTranslator>
#include <QVBoxLayout>
#include <QTextEdit>
#include <QListView>
#include <QDialogButtonBox>

namespace
{
//! Press "Yes" or "Cancel" buttons in modal send confirmation dialog.
void ConfirmSend(QString* text = nullptr, bool cancel = false)
{
    QTimer::singleShot(0, [text, cancel]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget->inherits("SendConfirmationDialog")) {
                SendConfirmationDialog* dialog = qobject_cast<SendConfirmationDialog*>(widget);
                if (text) *text = dialog->text();
                QAbstractButton* button = dialog->button(cancel ? QMessageBox::Cancel : QMessageBox::Yes);
                button->setEnabled(true);
                button->click();
            }
        }
    });
}

//! Send coins to address and return txid.
uint256 SendCoins(CWallet& wallet, SendCoinsDialog& sendCoinsDialog, const CTxDestination& address, CAmount amount, bool rbf)
{
    QVBoxLayout* entries = sendCoinsDialog.findChild<QVBoxLayout*>("entries");
    SendCoinsEntry* entry = qobject_cast<SendCoinsEntry*>(entries->itemAt(0)->widget());
    entry->findChild<QValidatedLineEdit*>("payTo")->setText(QString::fromStdString(EncodeDestination(address)));
    entry->findChild<BitcoinAmountField*>("payAmount")->setValue(amount);
    /* Litecon: Disabled RBF UI
    sendCoinsDialog.findChild<QFrame*>("frameFee")
        ->findChild<QFrame*>("frameFeeSelection")
        ->findChild<QCheckBox*>("optInRBF")
        ->setCheckState(rbf ? Qt::Checked : Qt::Unchecked);
    */
    uint256 txid;
    boost::signals2::scoped_connection c(wallet.NotifyTransactionChanged.connect([&txid](CWallet*, const uint256& hash, ChangeType status) {
        if (status == CT_NEW) txid = hash;
    }));
    ConfirmSend();
    bool invoked = QMetaObject::invokeMethod(&sendCoinsDialog, "on_sendButton_clicked");
    assert(invoked);
    return txid;
}

//! Find index of txid in transaction list.
QModelIndex FindTx(const QAbstractItemModel& model, const uint256& txid)
{
    QString hash = QString::fromStdString(txid.ToString());
    int rows = model.rowCount({});
    for (int row = 0; row < rows; ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            return index;
        }
    }
    return {};
}

//! Find every row of txid in transaction list, in the list's order.
std::vector<QModelIndex> FindTxRows(const QAbstractItemModel& model, const uint256& txid)
{
    std::vector<QModelIndex> found;
    QString hash = QString::fromStdString(txid.ToString());
    for (int row = 0; row < model.rowCount({}); ++row) {
        QModelIndex index = model.index(row, 0, {});
        if (model.data(index, TransactionTableModel::TxHashRole) == hash) {
            found.push_back(index);
        }
    }
    return found;
}

//! Pay amount (by default nothing) to each of the scripts, the fee coming from
//! the wallet, with the transaction's comments in value_map, and let the
//! transaction list take in the transaction.
CTransactionRef PayToScripts(WalletModel& walletModel, const std::vector<CScript>& scripts, CAmount amount = 0, const interfaces::WalletValueMap& value_map = {})
{
    std::vector<CRecipient> recipients;
    for (const CScript& script : scripts) {
        recipients.push_back({DestinationAddr{script}, amount, false /* subtract fee from amount */});
    }
    CCoinControl coin_control;
    int change_pos = -1;
    CAmount fee;
    bilingual_str error;
    CTransactionRef tx = walletModel.wallet().createTransaction(recipients, coin_control, true /* sign */, change_pos, fee, error);
    if (tx) {
        walletModel.wallet().commitTransaction(tx, value_map, {} /* order_form */, {} /* reserved_keys */);
        // The list learns of the transaction through a queued call.
        QCoreApplication::processEvents();
    }
    return tx;
}

//! Invoke bumpfee on txid and check results.
/* Ferrite: Disable RBF
void BumpFee(TransactionView& view, const uint256& txid, bool expectDisabled, std::string expectError, bool cancel)
{
    QTableView* table = view.findChild<QTableView*>("transactionView");
    QModelIndex index = FindTx(*table->selectionModel()->model(), txid);
    QVERIFY2(index.isValid(), "Could not find BumpFee txid");

    // Select row in table, invoke context menu, and make sure bumpfee action is
    // enabled or disabled as expected.
    QAction* action = view.findChild<QAction*>("bumpFeeAction");
    table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    action->setEnabled(expectDisabled);
    table->customContextMenuRequested({});
    QCOMPARE(action->isEnabled(), !expectDisabled);

    action->setEnabled(true);
    QString text;
    if (expectError.empty()) {
        ConfirmSend(&text, cancel);
    } else {
        ConfirmMessage(&text);
    }
    action->trigger();
    QVERIFY(text.indexOf(QString::fromStdString(expectError)) != -1);
}
*/

//! Simple qt wallet tests.
//
// Test widgets can be debugged interactively calling show() on them and
// manually running the event loop, e.g.:
//
//     sendCoinsDialog.show();
//     QEventLoop().exec();
//
// This also requires overriding the default minimal Qt platform:
//
//     QT_QPA_PLATFORM=xcb     src/qt/test/test_bitcoin-qt  # Linux
//     QT_QPA_PLATFORM=windows src/qt/test/test_bitcoin-qt  # Windows
//     QT_QPA_PLATFORM=cocoa   src/qt/test/test_bitcoin-qt  # macOS
void TestGUI(interfaces::Node& node)
{
    // Set up wallet and chain with 15 blocks (5 mature blocks for spending):
    // TestChain100Setup makes COINBASE_MATURITY (10) blocks.
    TestChain100Setup test;
    for (int i = 0; i < 5; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    bool firstRun;
    wallet->LoadWallet(firstRun);
    {
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type, spk_man->GetScanSecret()), "", "receive");
        spk_man->AddKeyPubKey(test.coinbaseKey, test.coinbaseKey.GetPubKey());
        wallet->SetLastBlockProcessed(15, ::ChainActive().Tip()->GetBlockHash());
    }
    {
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0 /* block height */, {} /* max height */, reserver, true /* fUpdate */);
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
        QCOMPARE(result.last_scanned_block, ::ChainActive().Tip()->GetBlockHash());
        QVERIFY(result.last_failed_block.IsNull());
    }
    wallet->SetBroadcastTransactions(true);

    // Create widgets for sending coins and listing transactions.
    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    SendCoinsDialog sendCoinsDialog(platformStyle.get());
    TransactionView transactionView(platformStyle.get());
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, nullopt);
    sendCoinsDialog.setModel(&walletModel);
    transactionView.setModel(&walletModel);

    {
        // Check balance in send dialog
        QLabel* balanceLabel = sendCoinsDialog.findChild<QLabel*>("labelBalance");
        QString balanceText = balanceLabel->text();
        int unit = walletModel.getOptionsModel()->getDisplayUnit();
        CAmount balance = walletModel.wallet().getBalance();
        QString balanceComparison = BitcoinUnits::formatWithUnit(unit, balance, false, BitcoinUnits::SeparatorStyle::ALWAYS);
        QCOMPARE(balanceText, balanceComparison);
    }

    // Send two transactions, and verify they are added to transaction list.
    TransactionTableModel* transactionTableModel = walletModel.getTransactionTableModel();
    QCOMPARE(transactionTableModel->rowCount({}), 15);
    uint256 txid1 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 5 * COIN, false /* rbf */);
    uint256 txid2 = SendCoins(*wallet.get(), sendCoinsDialog, PKHash(), 10 * COIN, true /* rbf */);
    QCOMPARE(transactionTableModel->rowCount({}), 17);
    QVERIFY(FindTx(*transactionTableModel, txid1).isValid());
    QVERIFY(FindTx(*transactionTableModel, txid2).isValid());

    // Call bumpfee. Test disabled, canceled, enabled, then failing cases.
    // Ferrite: Disable BumpFee tests
    // BumpFee(transactionView, txid1, true /* expect disabled */, "not BIP 125 replaceable" /* expected error */, false /* cancel */);
    // BumpFee(transactionView, txid2, false /* expect disabled */, {} /* expected error */, true /* cancel */);
    // BumpFee(transactionView, txid2, false /* expect disabled */, {} /* expected error */, false /* cancel */);
    // BumpFee(transactionView, txid2, true /* expect disabled */, "already bumped" /* expected error */, false /* cancel */);

    // Check current balance on OverviewPage
    OverviewPage overviewPage(platformStyle.get());
    overviewPage.setWalletModel(&walletModel);
    QLabel* balanceLabel = overviewPage.findChild<QLabel*>("labelBalance");
    QString balanceText = balanceLabel->text().trimmed();
    int unit = walletModel.getOptionsModel()->getDisplayUnit();
    CAmount balance = walletModel.wallet().getBalance();
    QString balanceComparison = BitcoinUnits::formatWithUnit(unit, balance, false, BitcoinUnits::SeparatorStyle::ALWAYS);
    QCOMPARE(balanceText, balanceComparison);

    // Check Request Payment button
    ReceiveCoinsDialog receiveCoinsDialog(platformStyle.get());
    receiveCoinsDialog.setModel(&walletModel);
    RecentRequestsTableModel* requestTableModel = walletModel.getRecentRequestsTableModel();

    // Label input
    QLineEdit* labelInput = receiveCoinsDialog.findChild<QLineEdit*>("reqLabel");
    labelInput->setText("TEST_LABEL_1");

    // Amount input
    BitcoinAmountField* amountInput = receiveCoinsDialog.findChild<BitcoinAmountField*>("reqAmount");
    amountInput->setValue(1);

    // Message input
    QLineEdit* messageInput = receiveCoinsDialog.findChild<QLineEdit*>("reqMessage");
    messageInput->setText("TEST_MESSAGE_1");
    int initialRowCount = requestTableModel->rowCount({});
    QPushButton* requestPaymentButton = receiveCoinsDialog.findChild<QPushButton*>("receiveButton");
    requestPaymentButton->click();
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("ReceiveRequestDialog")) {
            ReceiveRequestDialog* receiveRequestDialog = qobject_cast<ReceiveRequestDialog*>(widget);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("payment_header")->text(), QString("Payment information"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("uri_tag")->text(), QString("URI:"));
            QString uri = receiveRequestDialog->QObject::findChild<QLabel*>("uri_content")->text();
            QCOMPARE(uri.count("ferrite:"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("address_tag")->text(), QString("Address:"));

            QCOMPARE(uri.count("amount=0.00000001"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_tag")->text(), QString("Amount:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("amount_content")->text(), QString("0.00000001 ") + QString::fromStdString(CURRENCY_UNIT));

            QCOMPARE(uri.count("label=TEST_LABEL_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_tag")->text(), QString("Label:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("label_content")->text(), QString("TEST_LABEL_1"));

            QCOMPARE(uri.count("message=TEST_MESSAGE_1"), 2);
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_tag")->text(), QString("Message:"));
            QCOMPARE(receiveRequestDialog->QObject::findChild<QLabel*>("message_content")->text(), QString("TEST_MESSAGE_1"));
        }
    }

    // Clear button
    QPushButton* clearButton = receiveCoinsDialog.findChild<QPushButton*>("clearButton");
    clearButton->click();
    QCOMPARE(labelInput->text(), QString(""));
    QCOMPARE(amountInput->value(), CAmount(0));
    QCOMPARE(messageInput->text(), QString(""));

    // Check addition to history
    int currentRowCount = requestTableModel->rowCount({});
    QCOMPARE(currentRowCount, initialRowCount+1);

    // Check Remove button
    QTableView* table = receiveCoinsDialog.findChild<QTableView*>("recentRequestsView");
    table->selectRow(currentRowCount-1);
    QPushButton* removeRequestButton = receiveCoinsDialog.findChild<QPushButton*>("removeRequestButton");
    removeRequestButton->click();
    QCOMPARE(requestTableModel->rowCount({}), currentRowCount-1);
}

//! The transaction list, its tooltips and the new-transaction notification
//! name an OP_RETURN output "OP_RETURN data (N bytes)", N being the size of
//! its script ("1 byte" in English); the address that is copied and searched
//! for stays the hex of the script. A transaction's "to" comment that looks
//! like such hex is shown as it is.
void TestOpReturnOutputs(interfaces::Node& node)
{
    // The texts as an English-speaking user reads them: the English catalogue
    // holds the singular and the plural of "%n byte(s)".
    QTranslator english;
    QVERIFY(english.load(":/translations/en"));
    QCoreApplication::installTranslator(&english);

    // A chain of 15 blocks, of which the wallet can spend the first five coinbases.
    TestChain100Setup test;
    const int extra_blocks{5};
    for (int i = 0; i < extra_blocks; ++i) {
        test.CreateAndProcessBlock({}, GetScriptForRawPubKey(test.coinbaseKey.GetPubKey()));
    }
    node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(node.context()->chain.get(), "", CreateMockWalletDatabase());
    bool firstRun;
    wallet->LoadWallet(firstRun);
    {
        auto spk_man = wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(wallet->cs_wallet, spk_man->cs_KeyStore);
        wallet->SetAddressBook(GetDestinationForKey(test.coinbaseKey.GetPubKey(), wallet->m_default_address_type, spk_man->GetScanSecret()), "", "receive");
        spk_man->AddKeyPubKey(test.coinbaseKey, test.coinbaseKey.GetPubKey());
        wallet->SetLastBlockProcessed(::ChainActive().Height(), ::ChainActive().Tip()->GetBlockHash());
    }
    {
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0 /* block height */, {} /* max height */, reserver, true /* fUpdate */);
        QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
    }
    wallet->SetBroadcastTransactions(true);

    // Only the lowercase hex of a script that starts with OP_RETURN is named,
    // and only in the rows that hold the encoding of an output's script: not
    // in those that hold the transaction's "to" or "from" comment.
    const int sent{WalletTxRecord::SendToAddress};
    const CScript message = CScript() << OP_RETURN << std::vector<unsigned char>(44, 'm');
    QCOMPARE(TransactionTableModel::opReturnSummary(sent, HexStr(message)), QString("OP_RETURN data (46 bytes)"));
    QCOMPARE(TransactionTableModel::opReturnSummary(sent, "6a"), QString("OP_RETURN data (1 byte)"));
    QCOMPARE(TransactionTableModel::opReturnSummary(sent, HexStr(CScript() << OP_RETURN << OP_1)), QString("OP_RETURN data (2 bytes)"));
    QCOMPARE(TransactionTableModel::opReturnSummary(sent, "6a" + std::string(2 * 99999, 'f')), QString("OP_RETURN data (100000 bytes)"));
    QCOMPARE(TransactionTableModel::opReturnSummary(sent, "6a" + std::string(2 * 998999, '0')), QString("OP_RETURN data (999000 bytes)"));
    for (const int type : {WalletTxRecord::RecvWithAddress, WalletTxRecord::SendToSelf}) {
        QCOMPARE(TransactionTableModel::opReturnSummary(type, HexStr(message)), QString("OP_RETURN data (46 bytes)"));
    }
    for (const int type : {WalletTxRecord::Other, WalletTxRecord::Generated, WalletTxRecord::SendToOther, WalletTxRecord::RecvFromOther}) {
        QVERIFY2(TransactionTableModel::opReturnSummary(type, HexStr(message)).isEmpty(), std::to_string(type).c_str());
    }
    const CPubKey pubkey = test.coinbaseKey.GetPubKey();
    CTxDestination taproot;
    QVERIFY(ExtractDestination(CScript() << OP_1 << std::vector<unsigned char>(32, 0x6a), taproot));
    for (const std::string& other : {std::string{}, std::string{"6"}, std::string{"6a0"}, std::string{"6A00"},
             std::string{"6a0A"}, std::string{"6a0g"}, std::string{"6a00 "}, std::string{" 6a00"}, std::string{"6a00, 6a00"},
             HexStr(GetScriptForMultisig(1, {pubkey})), HexStr(GetScriptForPegin(mw::Hash())),
             EncodeDestination(PKHash(pubkey)), EncodeDestination(ScriptHash(message)),
             EncodeDestination(WitnessV0KeyHash(pubkey)), EncodeDestination(taproot),
             EncodeDestination(StealthAddress::Random())}) {
        QVERIFY2(TransactionTableModel::opReturnSummary(sent, other).isEmpty(), other.c_str());
    }

    std::unique_ptr<const PlatformStyle> platformStyle(PlatformStyle::instantiate("other"));
    OptionsModel optionsModel;
    ClientModel clientModel(node, &optionsModel);
    AddWallet(wallet);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel, platformStyle.get());
    RemoveWallet(wallet, nullopt);
    WalletView walletView(platformStyle.get(), nullptr);
    walletView.setClientModel(&clientModel);
    walletView.setWalletModel(&walletModel);
    QStringList notifications;
    QObject::connect(&walletView, &WalletView::incomingTransaction,
        [&notifications](const QString&, int, const CAmount&, const QString& type, const QString& address, const QString& label, const QString&) {
            notifications << type + ": " + address + (label.isEmpty() ? QString() : " [" + label + "]");
        });
    TransactionTableModel* model = walletModel.getTransactionTableModel();

    // Every coinbase holds a witness commitment, an OP_RETURN output that is
    // not the wallet's: the list has a row for the wallet's output only.
    QVERIFY(std::all_of(test.m_coinbase_txns.begin(), test.m_coinbase_txns.end(), [](const CTransactionRef& coinbase) {
        return std::any_of(coinbase->vout.begin(), coinbase->vout.end(), [](const CTxOut& out) {
            return !out.scriptPubKey.empty() && out.scriptPubKey[0] == OP_RETURN;
        });
    }));
    QCOMPARE(model->rowCount({}), int(test.m_coinbase_txns.size()) + extra_blocks);
    for (int row = 0; row < model->rowCount({}); ++row) {
        QCOMPARE(model->index(row, TransactionTableModel::Type).data().toString(), QString("Mined"));
        QVERIFY(!model->index(row, TransactionTableModel::ToAddress).data().toString().contains("OP_RETURN"));
    }

    // A message of 44 bytes, its script of 46; a bare OP_RETURN; two OP_RETURN
    // outputs in one transaction; a script near the size limit of a standard
    // transaction. The change of each goes back to the wallet and is not listed.
    struct Payment {
        std::vector<CScript> scripts;
        QStringList shown;
    };
    const std::vector<Payment> payments{
        {{message}, {"OP_RETURN data (46 bytes)"}},
        {{CScript() << OP_RETURN}, {"OP_RETURN data (1 byte)"}},
        {{CScript() << OP_RETURN << std::vector<unsigned char>{1, 2, 3}, CScript() << OP_RETURN << std::vector<unsigned char>(80, 'n')},
            {"OP_RETURN data (5 bytes)", "OP_RETURN data (83 bytes)"}},
        {{CScript() << OP_RETURN << std::vector<unsigned char>(99000, 'l')}, {"OP_RETURN data (99006 bytes)"}},
    };
    std::vector<CTransactionRef> txs;
    QStringList expected_notifications;
    for (const Payment& payment : payments) {
        CTransactionRef tx = PayToScripts(walletModel, payment.scripts);
        QVERIFY(tx);
        txs.push_back(tx);
        std::vector<QModelIndex> rows = FindTxRows(*model, tx->GetHash());
        QCOMPARE(int(rows.size()), payment.shown.size());
        QStringList shown, addresses, expected_addresses;
        for (const QModelIndex& row : rows) {
            const QString text = model->index(row.row(), TransactionTableModel::ToAddress).data().toString();
            shown << text;
            // The list sorts by the text it shows.
            QCOMPARE(model->index(row.row(), TransactionTableModel::ToAddress).data(Qt::EditRole).toString(), text);
            QCOMPARE(row.data(Qt::ToolTipRole).toString().section('\n', 1), "Sent to " + text);
            addresses << row.data(TransactionTableModel::AddressRole).toString();
        }
        for (const CScript& script : payment.scripts) expected_addresses << QString::fromStdString(HexStr(script));
        shown.sort();
        addresses.sort();
        expected_addresses.sort();
        QCOMPARE(shown, payment.shown);
        QCOMPARE(addresses, expected_addresses);
        // One notification for each new transaction, about its first row.
        expected_notifications << "Sent to: " + model->index(rows[0].row(), TransactionTableModel::ToAddress).data().toString();
    }
    QCOMPARE(notifications, expected_notifications);

    // An output the wallet cannot name, here one with an empty script, is
    // listed with the transaction's "to" comment, which is shown as it is even
    // when it looks like the hex of an OP_RETURN script.
    const std::string comment{HexStr(message)};
    CTransactionRef commented = PayToScripts(walletModel, {CScript()}, COIN, {{"to", comment}});
    QVERIFY(commented);
    std::vector<QModelIndex> commented_rows = FindTxRows(*model, commented->GetHash());
    QCOMPARE(int(commented_rows.size()), 1);
    const QModelIndex commented_row = commented_rows[0];
    QCOMPARE(commented_row.data(TransactionTableModel::TypeRole).toInt(), int(WalletTxRecord::SendToOther));
    QCOMPARE(model->index(commented_row.row(), TransactionTableModel::ToAddress).data().toString(), QString::fromStdString(comment));
    QCOMPARE(commented_row.data(Qt::ToolTipRole).toString().section('\n', 1), "Sent to " + QString::fromStdString(comment));
    QCOMPARE(notifications.size(), expected_notifications.size() + 1);
    QCOMPARE(notifications.back(), "Sent to: " + QString::fromStdString(comment));

    // "Copy address" and the search use the hex of the script.
    TransactionView* transactionView = walletView.findChild<TransactionView*>();
    QTableView* table = transactionView->findChild<QTableView*>("transactionView");
    TransactionFilterProxy* proxy = qobject_cast<TransactionFilterProxy*>(table->model());
    QVERIFY(proxy);
    const QString hex = QString::fromStdString(HexStr(message));
    const QModelIndex message_index = FindTx(*proxy, txs[0]->GetHash());
    QVERIFY(message_index.isValid());
    table->selectionModel()->select(message_index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(QMetaObject::invokeMethod(transactionView, "copyAddress"));
    QCOMPARE(QApplication::clipboard()->text(), hex);
    proxy->setSearchString(hex.mid(4, 16));
    QVERIFY(FindTx(*proxy, txs[0]->GetHash()).isValid());
    proxy->setSearchString("OP_RETURN");
    QVERIFY(!FindTx(*proxy, txs[0]->GetHash()).isValid());
    proxy->setSearchString(QString());
}

} // namespace

void WalletTests::walletTests()
{
#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping WalletTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_bitcoin-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif
    TestGUI(m_node);
}

void WalletTests::opReturnOutputs()
{
#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        // As in walletTests above.
        QWARN("Skipping WalletTests on mac build with 'minimal' platform set due to Qt bugs.");
        return;
    }
#endif
    TestOpReturnOutputs(m_node);
}
