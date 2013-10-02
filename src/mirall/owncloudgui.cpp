/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "mirall/application.h"
#include "mirall/owncloudgui.h"
#include "mirall/theme.h"
#include "mirall/folderman.h"
#include "mirall/owncloudinfo.h"
#include "mirall/mirallconfigfile.h"
#include "mirall/utility.h"
#include "mirall/progressdispatcher.h"
#include "mirall/itemprogressdialog.h"
#include "mirall/owncloudsetupwizard.h"
#include "mirall/settingsdialog.h"
#include "mirall/logger.h"
#include "mirall/logbrowser.h"

#include <QDesktopServices>
#include <QMessageBox>

namespace Mirall {

ownCloudGui::ownCloudGui(Application *parent) :
    QObject(parent),
    _tray(0),
    _settingsDialog(0),
    _progressDialog(0),
    _logBrowser(0),
    _contextMenu(0),
    _recentActionsMenu(0),
    _app(parent)
{
    _tray = new Systray();
    _tray->setIcon( Theme::instance()->syncStateIcon( SyncResult::NotYetStarted, true ) );

    connect(_tray.data(), SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            SLOT(slotTrayClicked(QSystemTrayIcon::ActivationReason)));

    setupActions();
    setupContextMenu();

    _tray->show();

    /* use a signal mapper to map the open requests to the alias names */
    _folderOpenActionMapper = new QSignalMapper(this);
    connect(_folderOpenActionMapper, SIGNAL(mapped(const QString &)),
            this, SLOT(slotFolderOpenAction(const QString &)));

    ProgressDispatcher *pd = ProgressDispatcher::instance();
    connect( pd, SIGNAL(progressInfo(QString,Progress::Info)), this,
             SLOT(slotUpdateProgress(QString,Progress::Info)) );
    connect( pd, SIGNAL(progressSyncProblem(QString,Progress::SyncProblem)),
             SLOT(slotProgressSyncProblem(QString,Progress::SyncProblem)));

    FolderMan *folderMan = FolderMan::instance();
    connect( folderMan, SIGNAL(folderSyncStateChange(QString)),
             this,SLOT(slotSyncStateChange(QString)));

    connect( Logger::instance(), SIGNAL(guiLog(QString,QString)),
             SLOT(slotShowTrayMessage(QString,QString)));
    connect( Logger::instance(), SIGNAL(optionalGuiLog(QString,QString)),
             SLOT(slotShowOptionalTrayMessage(QString,QString)));
    connect( Logger::instance(), SIGNAL(guiMessage(QString,QString)),
             SLOT(slotShowGuiMessage(QString,QString)));

}

// This should rather be in application.... or rather in MirallConfigFile?
bool ownCloudGui::checkConfigExists(bool openSettings)
{
    // if no config file is there, start the configuration wizard.
    MirallConfigFile cfgFile;

    if( cfgFile.exists() && !cfgFile.ownCloudUrl().isEmpty() ) {
        if( openSettings ) {
            this->slotSettings();
        }
        return true;
    } else {
        qDebug() << "No configured folders yet, starting setup wizard";
        OwncloudSetupWizard::runWizard(this, SLOT(slotownCloudWizardDone(int)));
        return false;
    }
}

void ownCloudGui::slotTrayClicked( QSystemTrayIcon::ActivationReason reason )
{
    // A click on the tray icon should only open the status window on Win and
    // Linux, not on Mac. They want a menu entry.
#if !defined Q_OS_MAC
    if( reason == QSystemTrayIcon::Trigger ) {
        checkConfigExists(true); // start settings if config is existing.
    }
#endif
}

void ownCloudGui::slotSyncStateChange( const QString& alias )
{
    FolderMan *folderMan = FolderMan::instance();
    const SyncResult& result = folderMan->syncResult( alias );

    slotComputeOverallSyncStatus();

    qDebug() << "Sync state changed for folder " << alias << ": "  << result.statusString();

    if( _progressDialog ) {
        _progressDialog->setSyncResult(result);
    }
    if (result.status() == SyncResult::Success || result.status() == SyncResult::Error) {
        Logger::instance()->enterNextLogFile();
    }
}

void ownCloudGui::slotFoldersChanged()
{
    slotComputeOverallSyncStatus();
    setupContextMenu();
}

void ownCloudGui::startupConnected( bool connected, const QStringList& fails )
{
    FolderMan *folderMan = FolderMan::instance();

    if( connected ) {
        qDebug() << "######## connected to ownCloud Server!";
        folderMan->setSyncEnabled(true);
        _tray->setIcon( Theme::instance()->syncStateIcon( SyncResult::NotYetStarted, true ) );
        _tray->show();
    } else {
        int cnt = folderMan->map().size();
        slotShowOptionalTrayMessage(tr("%1 Sync Started").arg(Theme::instance()->appNameGUI()),
                                    tr("Sync started for %n configured sync folder(s).","", cnt));
    }

    _startupFails = fails; // store that for the settings dialog once it appears.

}

void ownCloudGui::slotComputeOverallSyncStatus()
{
    // display the info of the least successful sync (eg. not just display the result of the latest sync
    QString trayMessage;
    FolderMan *folderMan = FolderMan::instance();
    Folder::Map map = folderMan->map();
    SyncResult overallResult = FolderMan::accountStatus(map.values());

    // if there have been startup problems, show an error message.
    if( !_settingsDialog.isNull() )
        _settingsDialog->setGeneralErrors( _startupFails );

    if( !_startupFails.isEmpty() ) {
        trayMessage = _startupFails.join(QLatin1String("\n"));
        QIcon statusIcon = Theme::instance()->syncStateIcon( SyncResult::Error, true );
        _tray->setIcon( statusIcon );
        _tray->setToolTip(trayMessage);
    } else {
        // create the tray blob message, check if we have an defined state
        if( overallResult.status() != SyncResult::Undefined ) {
            QStringList allStatusStrings;
            foreach(Folder* folder, map.values()) {
                qDebug() << "Folder in overallStatus Message: " << folder << " with name " << folder->alias();
                QString folderMessage = folderMan->statusToString(folder->syncResult().status(), folder->syncEnabled());
                allStatusStrings += tr("Folder %1: %2").arg(folder->alias(), folderMessage);
            }

            if( ! allStatusStrings.isEmpty() )
                trayMessage = allStatusStrings.join(QLatin1String("\n"));
            else
                trayMessage = tr("No sync folders configured.");

            QIcon statusIcon = Theme::instance()->syncStateIcon( overallResult.status(), true);
            _tray->setIcon( statusIcon );
            _tray->setToolTip(trayMessage);
        }
    }
}

void ownCloudGui::setupContextMenu()
{
    bool isConfigured = ownCloudInfo::instance()->isConfigured();
    FolderMan *folderMan = FolderMan::instance();

    _actionOpenoC->setEnabled(isConfigured);

    if( _contextMenu ) {
        _contextMenu->clear();
        _recentActionsMenu->clear();
        _recentActionsMenu->addAction(tr("None."));
        _recentActionsMenu->addAction(_actionRecent);
    } else {
        _contextMenu = new QMenu();
        _recentActionsMenu = _contextMenu->addMenu(tr("Recent Changes"));
        // this must be called only once after creating the context menu, or
        // it will trigger a bug in Ubuntu's SNI bridge patch (11.10, 12.04).
        _tray->setContextMenu(_contextMenu);
    }
    _contextMenu->setTitle(Theme::instance()->appNameGUI() );
    _contextMenu->addAction(_actionOpenoC);

    int folderCnt = folderMan->map().size();
    // add open actions for all sync folders to the tray menu
    if( Theme::instance()->singleSyncFolder() ) {
        // there should be exactly one folder. No sync-folder add action will be shown.
        QStringList li = folderMan->map().keys();
        if( li.size() == 1 ) {
            Folder *folder = folderMan->map().value(li.first());
            if( folder ) {
                // if there is singleFolder mode, a generic open action is displayed.
                QAction *action = new QAction( tr("Open %1 folder").arg(Theme::instance()->appNameGUI()), this);
                connect( action, SIGNAL(triggered()),_folderOpenActionMapper,SLOT(map()));
                _folderOpenActionMapper->setMapping( action, folder->alias() );

                _contextMenu->addAction(action);
            }
        }
    } else {
        // show a grouping with more than one folder.
        if ( folderCnt > 1) {
            _contextMenu->addAction(tr("Managed Folders:"))->setDisabled(true);
        }
        foreach (Folder *folder, folderMan->map() ) {
            QAction *action = new QAction( tr("Open folder '%1'").arg(folder->alias()), this );
            connect( action, SIGNAL(triggered()),_folderOpenActionMapper,SLOT(map()));
            _folderOpenActionMapper->setMapping( action, folder->alias() );

            _contextMenu->addAction(action);
        }
    }

    _contextMenu->addSeparator();
    _contextMenu->addAction(_actionQuota);
    _contextMenu->addSeparator();
    _contextMenu->addAction(_actionStatus);
    _contextMenu->addMenu(_recentActionsMenu);
    _contextMenu->addSeparator();
    _contextMenu->addAction(_actionSettings);
    if (!Theme::instance()->helpUrl().isEmpty()) {
        _contextMenu->addAction(_actionHelp);
    }
    _contextMenu->addSeparator();

    _contextMenu->addAction(_actionQuit);
}


void ownCloudGui::slotShowTrayMessage(const QString &title, const QString &msg)
{
    if( _tray )
        _tray->showMessage(title, msg);
    else
        qDebug() << "Tray not ready: " << msg;
}

void ownCloudGui::slotShowOptionalTrayMessage(const QString &title, const QString &msg)
{
    MirallConfigFile cfg;
    if (cfg.optionalDesktopNotifications())
        slotShowTrayMessage(title, msg);
}


/*
 * open the folder with the given Alais
 */
void ownCloudGui::slotFolderOpenAction( const QString& alias )
{
    Folder *f = FolderMan::instance()->folder(alias);
    qDebug() << "opening local url " << f->path();
    if( f ) {
        QUrl url(f->path(), QUrl::TolerantMode);
        url.setScheme( QLatin1String("file") );

#ifdef Q_OS_WIN32
        // work around a bug in QDesktopServices on Win32, see i-net
        QString filePath = f->path();

        if (filePath.startsWith(QLatin1String("\\\\")) || filePath.startsWith(QLatin1String("//")))
            url.setUrl(QDir::toNativeSeparators(filePath));
        else
            url = QUrl::fromLocalFile(filePath);
#endif
        QDesktopServices::openUrl(url);
    }
}

void ownCloudGui::setupActions()
{
    _actionOpenoC = new QAction(tr("Open %1 in browser").arg(Theme::instance()->appNameGUI()), this);
    QObject::connect(_actionOpenoC, SIGNAL(triggered(bool)), SLOT(slotOpenOwnCloud()));
    _actionQuota = new QAction(tr("Calculating quota..."), this);
    _actionQuota->setEnabled( false );
    _actionStatus = new QAction(tr("Unknown status"), this);
    _actionStatus->setEnabled( false );
    _actionSettings = new QAction(tr("Settings..."), this);
    _actionRecent = new QAction(tr("Details..."), this);
    _actionRecent->setEnabled( true );

    QObject::connect(_actionRecent, SIGNAL(triggered(bool)), SLOT(slotItemProgressDialog()));
    QObject::connect(_actionSettings, SIGNAL(triggered(bool)), SLOT(slotSettings()));
    _actionHelp = new QAction(tr("Help"), this);
    QObject::connect(_actionHelp, SIGNAL(triggered(bool)), SLOT(slotHelp()));
    _actionQuit = new QAction(tr("Quit %1").arg(Theme::instance()->appNameGUI()), this);
    QObject::connect(_actionQuit, SIGNAL(triggered(bool)), _app, SLOT(quit()));

    connect( ownCloudInfo::instance(), SIGNAL(quotaUpdated(qint64,qint64)),
             SLOT(slotRefreshQuotaDisplay(qint64, qint64)));
}

void ownCloudGui::slotRefreshQuotaDisplay( qint64 total, qint64 used )
{
    if (total == 0) {
        _actionQuota->setText(tr("Quota n/a"));
        return;
    }

    double percent = used/(double)total*100;
    QString percentFormatted = Utility::compactFormatDouble(percent, 1);
    QString totalFormatted = Utility::octetsToString(total);
    _actionQuota->setText(tr("%1% of %2 in use").arg(percentFormatted).arg(totalFormatted));
}

void ownCloudGui::slotProgressSyncProblem(const QString& folder, const Progress::SyncProblem& problem)
{
    Q_UNUSED(folder);
    Q_UNUSED(problem);

    // display a warn icon if warnings happend.
    QIcon warnIcon(":/mirall/resources/warning-16");
    _actionRecent->setIcon(warnIcon);

    slotRebuildRecentMenus();
}

void ownCloudGui::slotRebuildRecentMenus()
{
    _recentActionsMenu->clear();
    const QList<Progress::Info>& progressInfoList = ProgressDispatcher::instance()->recentChangedItems(5);

    if( progressInfoList.size() == 0 ) {
        _recentActionsMenu->addAction(tr("No items synced recently"));
    } else {
        QListIterator<Progress::Info> i(progressInfoList);

        while(i.hasNext()) {
            Progress::Info info = i.next();
            QString kindStr = Progress::asResultString(info.kind);
            QString timeStr = info.timestamp.toString("hh:mm");

            QString actionText = tr("%1 (%2, %3)").arg(info.current_file).arg(kindStr).arg(timeStr);
            _recentActionsMenu->addAction( actionText );
        }
    }
    // add a more... entry.
    _recentActionsMenu->addAction(_actionRecent);
}


void ownCloudGui::slotUpdateProgress(const QString &folder, const Progress::Info& progress)
{
    Q_UNUSED(folder);

    // shows an entry in the context menu.
    QString curAmount = Utility::octetsToString(progress.overall_current_bytes);
    QString totalAmount = Utility::octetsToString(progress.overall_transmission_size);
    _actionStatus->setText(tr("Syncing %1 of %2 (%3 of %4) ").arg(progress.current_file_no)
                           .arg(progress.overall_file_count).arg(curAmount, totalAmount));

    // wipe the problem list at start of sync.
    if( progress.kind == Progress::StartSync ) {
        _actionRecent->setIcon( QIcon() ); // Fixme: Set a "in-progress"-item eventually.
    }

    // If there was a change in the file list, redo the progress menu.
    if( progress.kind == Progress::EndDownload || progress.kind == Progress::EndUpload ||
            progress.kind == Progress::EndDelete ) {
        slotRebuildRecentMenus();
    }

    if (progress.kind == Progress::EndSync) {
        slotRebuildRecentMenus();  // show errors.
        QTimer::singleShot(2000, this, SLOT(slotDisplayIdle()));
    }
}

void ownCloudGui::slotDisplayIdle()
{
    _actionStatus->setText(tr("Up to date"));
}

void ownCloudGui::slotShowGuiMessage(const QString &title, const QString &message)
{
    QMessageBox *msgBox = new QMessageBox;
    msgBox->setAttribute(Qt::WA_DeleteOnClose);
    msgBox->setText(message);
    msgBox->setWindowTitle(title);
    msgBox->setIcon(QMessageBox::Information);
    msgBox->open();
}

void ownCloudGui::slotSettings()
{
    if (_settingsDialog.isNull()) {
        _settingsDialog = new SettingsDialog(this);
        _settingsDialog->setAttribute( Qt::WA_DeleteOnClose, true );
        _settingsDialog->show();
    }

    _settingsDialog->setGeneralErrors( _startupFails );
    Utility::raiseDialog(_settingsDialog.data());
}

void ownCloudGui::slotItemProgressDialog()
{
    if (_progressDialog.isNull()) {
        _progressDialog = new ItemProgressDialog(_app);
        _progressDialog->setAttribute( Qt::WA_DeleteOnClose, true );
        _progressDialog->setupList();
        _progressDialog->show();
    }
    Utility::raiseDialog(_progressDialog.data());
}

void ownCloudGui::slotShutdown()
{
    // those do delete on close
    if (!_settingsDialog.isNull()) _settingsDialog->close();
    if (!_progressDialog.isNull()) _progressDialog->close();
    if (!_logBrowser.isNull())     _logBrowser->deleteLater();
}

void ownCloudGui::slotToggleLogBrowser()
{
    if (_logBrowser.isNull()) {
        // init the log browser.
        _logBrowser = new LogBrowser;
        // ## TODO: allow new log name maybe?
    }

    if (_logBrowser->isVisible() ) {
        _logBrowser->hide();
    } else {
        Utility::raiseDialog(_logBrowser);
    }
}

void ownCloudGui::slotOpenOwnCloud()
{
  MirallConfigFile cfgFile;

  QString url = cfgFile.ownCloudUrl();
  QDesktopServices::openUrl( url );
}

void ownCloudGui::slotHelp()
{
    QDesktopServices::openUrl(QUrl(Theme::instance()->helpUrl()));
}


} // end namespace
