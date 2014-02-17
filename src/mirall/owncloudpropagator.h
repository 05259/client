/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#ifndef OWNCLOUDPROPAGATOR_H
#define OWNCLOUDPROPAGATOR_H

#include <neon/ne_request.h>
#include <QHash>
#include <QObject>
#include <qelapsedtimer.h>

#include "syncfileitem.h"
#include "progressdispatcher.h"

struct hbf_transfer_s;
struct ne_session_s;
struct ne_decompress_s;

namespace Mirall {

class SyncJournalDb;
class OwncloudPropagator;

class PropagatorJob : public QObject {
    Q_OBJECT
protected:
    OwncloudPropagator *_propagator;
    void emitReady() {
        bool wasReady = _readySent;
        _readySent = true;
        if (!wasReady)
            emit ready();
    };
public:
    bool _readySent;
    explicit PropagatorJob(OwncloudPropagator* propagator) : _propagator(propagator), _readySent(false) {}

public slots:
    virtual void start() = 0;
    virtual void abort() {}
signals:
    /**
     * Emitted when the job is fully finished
     */
    void finished(SyncFileItem::Status);

    /**
     * Emitted when one item has been completed within a job.
     */
    void completed(const SyncFileItem &);

    /**
     * Emitted when all the sub-jobs have been scheduled and
     * we are ready and more jobs might be started
     * This signal is not always emitted.
     */
    void ready();

    void progress(Progress::Kind, const SyncFileItem& item, quint64 bytes, quint64 total);

};

/*
 * Propagate a directory, and all its sub entries.
 */
class PropagateDirectory : public PropagatorJob {
    Q_OBJECT
public:
    // e.g: create the directory
    QScopedPointer<PropagatorJob>_firstJob;

    // all the sub files or sub directories.
    //TODO:  in the future, all sub job can be run in parallel
    QVector<PropagatorJob *> _subJobs;

    SyncFileItem _item;

    int _current; // index of the current running job
    int _runningNow; // number of subJob running now
    SyncFileItem::Status _hasError;  // NoStatus,  or NormalError / SoftError if there was an error


    explicit PropagateDirectory(OwncloudPropagator *propagator, const SyncFileItem &item = SyncFileItem())
        : PropagatorJob(propagator)
        , _firstJob(0), _item(item),  _current(-1), _runningNow(0), _hasError(SyncFileItem::NoStatus) { }

    virtual ~PropagateDirectory() {
        qDeleteAll(_subJobs);
    }

    void append(PropagatorJob *subJob) {
        _subJobs.append(subJob);
    }

    virtual void start();
    virtual void abort() {
        if (_firstJob)
            _firstJob->abort();
        foreach (PropagatorJob *j, _subJobs)
            j->abort();
    }

private slots:
    void startJob(PropagatorJob *next) {
        connect(next, SIGNAL(finished(SyncFileItem::Status)), this, SLOT(slotSubJobFinished(SyncFileItem::Status)), Qt::QueuedConnection);
        connect(next, SIGNAL(completed(SyncFileItem)), this, SIGNAL(completed(SyncFileItem)));
        connect(next, SIGNAL(progress(Progress::Kind,SyncFileItem,quint64,quint64)), this, SIGNAL(progress(Progress::Kind,SyncFileItem,quint64,quint64)));
        connect(next, SIGNAL(ready()), this, SLOT(slotSubJobReady()));
        _runningNow++;
        QMetaObject::invokeMethod(next, "start");
    }

    void slotSubJobFinished(SyncFileItem::Status status);
    void slotSubJobReady();
};


/*
 * Abstract class to propagate a single item
 * (Only used for neon job)
 */
class PropagateItemJob : public PropagatorJob {
    Q_OBJECT
protected:
    void done(SyncFileItem::Status status, const QString &errorString = QString());

    SyncFileItem  _item;

private:
    QScopedPointer<PropagateItemJob> _restoreJob;

public:
    PropagateItemJob(OwncloudPropagator* propagator, const SyncFileItem &item)
        : PropagatorJob(propagator), _item(item) {}

};

// Dummy job that just mark it as completed and ignored.
class PropagateIgnoreJob : public PropagateItemJob {
    Q_OBJECT
public:
    PropagateIgnoreJob(OwncloudPropagator* propagator,const SyncFileItem& item)
        : PropagateItemJob(propagator, item) {}
    void start() {
        done(SyncFileItem::FileIgnored);
    }
};


class OwncloudPropagator : public QObject {
    Q_OBJECT

    PropagateItemJob *createJob(const SyncFileItem& item);
    QScopedPointer<PropagateDirectory> _rootJob;

public:
    /* 'const' because they are accessed by the thread */

    QThread* _neonThread;
    ne_session_s * const _session;

    const QString _localDir; // absolute path to the local directory. ends with '/'
    const QString _remoteDir; // path to the root of the remote. ends with '/'  (include remote.php/webdav)
    const QString _remoteFolder; // folder. (same as remoteDir but without remote.php/webdav)

    SyncJournalDb * const _journal;

public:
    OwncloudPropagator(ne_session_s *session, const QString &localDir, const QString &remoteDir, const QString &remoteFolder,
                       SyncJournalDb *progressDb, QThread *neonThread)
            : _neonThread(neonThread)
            , _session(session)
            , _localDir((localDir.endsWith(QChar('/'))) ? localDir : localDir+'/' )
            , _remoteDir((remoteDir.endsWith(QChar('/'))) ? remoteDir : remoteDir+'/' )
            , _remoteFolder((remoteFolder.endsWith(QChar('/'))) ? remoteFolder : remoteFolder+'/' )
            , _journal(progressDb)
            , _activeJobs(0)
    { }

    void start(const SyncFileItemVector &_syncedItems);

    QAtomicInt _downloadLimit;
    QAtomicInt _uploadLimit;

    QAtomicInt _abortRequested; // boolean set by the main thread to abort.

    /* The number of currently active jobs */
    int _activeJobs;

    void overallTransmissionSizeChanged( qint64 change );

    bool isInSharedDirectory(const QString& file);


    void abort() {
        _abortRequested.fetchAndStoreOrdered(true);
        if (_rootJob)
            _rootJob->abort();
        emit finished();
    }


signals:
    void completed(const SyncFileItem &);
    void progress(Progress::Kind kind, const SyncFileItem&, quint64 bytes, quint64 total);
    void progressChanged(qint64 change);
    void finished();

};

}

#endif
