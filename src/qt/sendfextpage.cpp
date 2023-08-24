#include <qt/sendfextpage.h>
#include <qt/forms/ui_sendfextpage.h>

#include <interfaces/node.h>
#include <logging.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/protocol.h>

#include <univalue.h>
#include <utilstrencodings.h>
#include <QMessageBox>

SendFextPage::SendFextPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    platformStyle(platformStyle),
    ui(new Ui::SendFextPage),
    walletModel(nullptr)
{
    ui->setupUi(this);

    ui->sendFextButton->hide();
  
    connect(ui->sendFextButton, &QPushButton::clicked, this, &SendFextPage::onSendFextAction);

    ui->sendFext->installEventFilter(this);
}

SendFextPage::~SendFextPage()
{
    delete ui;
}

void SendFextPage::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
}

bool SendFextPage::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FocusIn)
    {
        if (object == ui->sendFext)
        {
            ui->sendFextButton->setDefault(true);
        }
    }
    return QWidget::eventFilter(object, event);
}

void SendFextPage::onSendFextAction()
{
  
    if (!walletModel)
        return;

    QString fextdata = ui->sendFext->text();

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;
  
    //////////////////////////////////////////////////////////////////////
    // Qstring
    const std::string strName = fextdata.toStdString();
    LogPrint(BCLog::QT, "Ferrite Core sending text:%s\n", strName);
    // Convert to Hex
    std::string hexStrName = HexStr(strName);
    LogPrint(BCLog::QT, "Ferrite Core sending hex:%s\n", hexStrName);

    // Create empty UniValue inputs array
    UniValue inputs(UniValue::VARR);
    
    // Construct the outputs array with data output
    UniValue outputs(UniValue::VARR);
    UniValue dataOutput(UniValue::VOBJ);
    dataOutput.pushKV("data", hexStrName);  // Replace with your desired data
    outputs.push_back(dataOutput);
    
    // for the RPC call
    UniValue params(UniValue::VARR);
    params.push_back(inputs);
    params.push_back(outputs);
    
    // Make the RPC call using executeRpc method
    std::string walletURI = "/wallet/" + walletModel->getWalletName().toStdString();
    UniValue result;
    bool success = walletModel->node().executeRpc("createrawtransaction", params, result, walletURI);
    
    // Check if the RPC call was successful
    if (success) {
        std::string rawTransaction = result.get_str();
    } else {
        std::string error = result.write();
        return;
    }
    //////////////////////////////////////////////////////////////////////
    // Construct the parameters array for the RPC call
    UniValue params(UniValue::VARR);
    params.push_back(rawTransaction);
    
    // Make the RPC call using executeRpc method
    UniValue result;
    bool success = walletModel->node().executeRpc("fundrawtransaction", params, result);
    
    // Check if the RPC call was successful
    if (success) {
        std::string fundedRawTransaction = result["hex"].get_str();
    } else {
        std::string error = result.write();
        return;
    }
    //////////////////////////////////////////////////////////////////////    
    // Construct the parameters array for the RPC call
    UniValue params(UniValue::VARR);
    params.push_back(fundedRawTransaction);
    
    // Make the RPC call using executeRpc method
    UniValue result;
    bool success = walletModel->node().executeRpc("signrawtransactionwithwallet", params, result);
    
    // Check if the RPC call was successful
    if (success) {
        std::string signedRawTransaction = result["hex"].get_str();
        bool complete = result["complete"].get_bool();     // You can also check the 'complete' flag to see if signing is complete       
        // hex-encoded signed raw transaction   
    } else {
        std::string error = result.write();
    }
    //////////////////////////////////////////////////////////////////////
    // Construct the parameters array for the RPC call
    UniValue params(UniValue::VARR);
    params.push_back(signedRawTransaction);
    
    // Make the RPC call using executeRpc method
    UniValue result;
    bool success = walletModel->node().executeRpc("sendrawtransaction", params, result);
    
    // Check if the RPC call was successful
    if (success) {
        std::string transactionHash = result.get_str();
        // transaction hash of the broadcasted transaction
    } else {
        std::string error = result.write();
        return;
    }
    //////////////////////////////////////////////////////////////////////
  
    // reset UI text
    ui->sendFext->setText("");
    ui->sendFextButton->setDefault(true);
}
