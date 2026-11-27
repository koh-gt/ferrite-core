// Copyright (c) 2018-2020 The Bitcoin Core developers
// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/node.h>
#include <qt/bitcoin.h>
#include <qt/fext/fextextension.h>
#include <qt/guiextension.h>

#include <util/system.h>
#include <util/translation.h>
#include <util/url.h>

#include <QCoreApplication>
#include <QtGlobal>

#include <functional>
#include <string>

/** Translate string to current locale using Qt. */
extern const std::function<std::string(const char*)> G_TRANSLATION_FUN = [](const char* psz) {
    return QCoreApplication::translate("bitcoin-core", psz).toStdString();
};
UrlDecodeFn* const URL_DECODE = urlDecode;

/** What this program adds to the GUI of ferrite-qt. */
static FextExtension g_fext_extension;

int main(int argc, char* argv[])
{
    g_gui_extension = &g_fext_extension;
    // The program shows the OP_RETURN record. It is the record's GUI host, so that the record is kept by default, and
    // the record's options are registered before GuiMain parses the arguments, so that the command line, the settings
    // file and ferrite.conf can set them and -help shows them.
    opreturn::SetGuiHost();
    opreturn::AddArgs(gArgs);
    Q_INIT_RESOURCE(fext);
    return GuiMain(argc, argv);
}
