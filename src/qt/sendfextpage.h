#ifndef SENDFEXTPAGE_H
#define SENDFEXTPAGE_H

#include <qt/platformstyle.h>

#include <QWidget>
#include <optional>

class WalletModel;

namespace Ui {
    class SendFextPage;
}

QT_BEGIN_NAMESPACE
QT_END_NAMESPACE

/** Page for sending ferritext messages */
class SendFextPage : public QWidget
{
    Q_OBJECT

public:
    explicit SendFextPage(const PlatformStyle *platformStyle, QWidget *parent = nullptr);
    ~SendFextPage();

    void setModel(WalletModel *walletModel);

private:
    const PlatformStyle *platformStyle;
    Ui::SendFextPage *ui;
    WalletModel *walletModel;

private Q_SLOTS:
    bool eventFilter(QObject *object, QEvent *event);
    void onSendFextAction();
};

#endif
