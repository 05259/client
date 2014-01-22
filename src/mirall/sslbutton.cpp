/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "mirall/sslbutton.h"
#include "mirall/account.h"
#include "mirall/utility.h"

#include <QMenu>
#include <QUrl>
#include <QtNetwork>
#include <QSslConfiguration>
#include <QWidgetAction>
#include <QLabel>

namespace Mirall {

SslButton::SslButton(QWidget *parent) :
    QToolButton(parent)
{
    setPopupMode(QToolButton::InstantPopup);
}

QString SslButton::protoToString(QSsl::SslProtocol proto)
{
    switch(proto) {
        break;
    case QSsl::SslV2:
        return QLatin1String("SSL v2");
    case QSsl::SslV3:
        return QLatin1String("SSL v3");
    case QSsl::TlsV1:
        return QLatin1String("TLS");
    default:
        return QString();
    }
}

static QString addCertDetailsField(const QString &key, const QString &value, bool tt = false)
{
    if (value.isEmpty())
        return QString();

    QString row = QString::fromLatin1("<tr><td style=\"vertical-align: top;\"><b>%1</b></td><td style=\"vertical-align: bottom;\">%2</td></tr>").arg(key);

    if (tt) {
        row = row.arg(QString::fromLatin1("<tt style=\"font-size: small\">%1</tt>").arg(value));
    } else {
        row = row.arg(value);
    }
    return row;
}

QMenu* SslButton::buildCertMenu(QMenu *parent, const QSslCertificate& cert,
                                const QList<QSslCertificate>& userApproved, int pos)
{
    QString cn = QStringList(cert.subjectInfo(QSslCertificate::CommonName)).join(QChar(';'));
    QString ou = QStringList(cert.subjectInfo(QSslCertificate::OrganizationalUnitName)).join(QChar(';'));
    QString org = QStringList(cert.subjectInfo(QSslCertificate::Organization)).join(QChar(';'));
    QString country = QStringList(cert.subjectInfo(QSslCertificate::CountryName)).join(QChar(';'));
    QString state = QStringList(cert.subjectInfo(QSslCertificate::StateOrProvinceName)).join(QChar(';'));
    QString issuer = QStringList(cert.issuerInfo(QSslCertificate::CommonName)).join(QChar(';'));
    QString md5 = Utility::formatFingerprint(cert.digest(QCryptographicHash::Md5).toHex());
    QString sha1 = Utility::formatFingerprint(cert.digest(QCryptographicHash::Sha1).toHex());
    QString serial = QString::fromUtf8(cert.serialNumber(), true);
    QString effectiveDate = cert.effectiveDate().date().toString();
    QString expiryDate = cert.expiryDate().date().toString();
    QString sna = QStringList(cert.alternateSubjectNames().values()).join(" ");

    QString details;
    QTextStream stream(&details);

    stream << QLatin1String("<html><body>");

    stream << tr("<h3>Certificate Details</h3>");

    stream << QLatin1String("<table>");
    stream << addCertDetailsField(tr("Common Name (CN):"), Utility::escape(cn));
    stream << addCertDetailsField(tr("Subject Alternative Names:"), Utility::escape(sna)
                                  .replace(" ", "<br/>"));
    stream << addCertDetailsField(tr("Organization (O):"), Utility::escape(org));
    stream << addCertDetailsField(tr("Organizational Unit (OU):"), Utility::escape(ou));
    stream << addCertDetailsField(tr("State/Province:"), Utility::escape(state));
    stream << addCertDetailsField(tr("Country:"), Utility::escape(country));
    stream << addCertDetailsField(tr("Serial:"), Utility::escape(serial), true);
    stream << QLatin1String("</table>");

    stream << tr("<h3>Issuer</h3>");

    stream << QLatin1String("<table>");
    stream << addCertDetailsField(tr("Issuer:"), Utility::escape(issuer));
    stream << addCertDetailsField(tr("Issued on:"), Utility::escape(effectiveDate));
    stream << addCertDetailsField(tr("Expires on:"), Utility::escape(expiryDate));
    stream << QLatin1String("</table>");

    stream << tr("<h3>Fingerprints</h3>");

    stream << QLatin1String("<table>");
    stream << addCertDetailsField(tr("MD 5:"), Utility::escape(md5), true);
    stream << addCertDetailsField(tr("SHA-1:"), Utility::escape(sha1), true);
    stream << QLatin1String("</table>");

    if (userApproved.contains(cert)) {
        stream << tr("<p><b>Note:</b> This certificate was manually approved</p>");
    }
    stream << QLatin1String("</body></html>");

    QString txt;
    if (pos > 0) {
        txt += QString(pos, ' ');
        txt += QChar(0x21AA); // nicer '->' symbol
        txt += QChar(' ');
    }

    if (QSslSocket::systemCaCertificates().contains(cert)) {
        txt += tr("%1 (in Root CA store)").arg(cn);
    } else {
        if (cn == issuer) {
            txt += tr("%1 (self-signed)").arg(cn, issuer);
        } else {
            txt += tr("%1").arg(cn);
        }
    }

    // create label first
    QLabel *label = new QLabel(parent);
    label->setStyleSheet(QLatin1String("QLabel { padding: 8px; background-color: #fff; }"));
    label->setText(details);
    // plug label into widget action
    QWidgetAction *action = new QWidgetAction(parent);
    action->setDefaultWidget(label);
    // plug action into menu
    QMenu *menu = new QMenu(parent);
    menu->menuAction()->setText(txt);
    menu->addAction(action);

    return menu;

}

void SslButton::updateAccountInfo(Account *account)
{
    if (!account || account->state() != Account::Connected) {
        setVisible(false);
        return;
    } else {
        setVisible(true);
    }
    if (account->url().scheme() == QLatin1String("https")) {
        setIcon(QIcon(QPixmap(":/mirall/resources/lock-https.png")));
        QSslCipher cipher = account->sslConfiguration().sessionCipher();
        setToolTip(tr("This connection is encrypted using %1 bit %2.\n").arg(cipher.usedBits()).arg(cipher.name()));
        QMenu *menu = new QMenu(this);
        QList<QSslCertificate> chain = account->sslConfiguration().peerCertificateChain();
        menu->addAction(tr("Certificate information:"))->setEnabled(false);

        // find trust anchor (informational only, verification is done by QSslSocket!)
        foreach (const QSslCertificate &rootCA, QSslSocket::systemCaCertificates()) {
            if (rootCA.issuerInfo(QSslCertificate::CommonName) == chain.last().issuerInfo(QSslCertificate::CommonName) &&
                    rootCA.issuerInfo(QSslCertificate::Organization) == chain.last().issuerInfo(QSslCertificate::Organization)) {
                chain << rootCA;
                break;
            }
        }

        QListIterator<QSslCertificate> it(chain);
        it.toBack();
        int i = 0;
        while (it.hasPrevious()) {
            menu->addMenu(buildCertMenu(menu, it.previous(), account->approvedCerts(), i));
            i++;
        }
        setMenu(menu);
    } else {
        setIcon(QIcon(QPixmap(":/mirall/resources/lock-http.png")));
        setToolTip(tr("This connection is NOT secure as it is not encrypted.\n"));
        setMenu(0);
    }
}

} // namespace Mirall
