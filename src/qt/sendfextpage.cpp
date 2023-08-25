#include <qt/sendfextpage.h>
#include <qt/forms/ui_sendfextpage.h>

#include <interfaces/node.h>
#include <logging.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <rpc/protocol.h>

#include <univalue.h>
#include <util/strencodings.h>
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
  
    // Qstring
    const std::string strfextdata = fextdata.toStdString();
    std::string hexstrfextdata = HexStr(strfextdata);
    LogPrint(BCLog::QT, "Ferrite Core sending hex:%s\n", hexstrfextdata);

    std::string walletURI = "/wallet/" + walletModel->getWalletName().toStdString();

    UniValue params(UniValue::VOBJ);
    params.pushKV ("data", hexstrfextdata);
    UniValue hexstringout = walletModel->node().executeRpc("createrawtransaction", params, walletURI);
    
    UniValue params2(UniValue::VOBJ);
    params2.pushKV ("hexstring", hexstringout);
    UniValue hexstringout2 = walletModel->node().executeRpc("fundrawtransaction", params2, walletURI);

    UniValue params3(UniValue::VOBJ);
    params3.pushKV ("hexstring", hexstringout2);
    UniValue hexstringout3 = walletModel->node().executeRpc("signrawtransactionwithwallet", params3, walletURI);
    
    UniValue params4(UniValue::VOBJ);
    params4.pushKV ("hexstring", hexstringout3);
    walletModel->node().executeRpc("sendrawtransaction", params4, walletURI);
    
    // reset UI text
    ui->sendFext->setText("");
    ui->sendFextButton->setDefault(true);
}
