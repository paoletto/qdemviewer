/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** Commercial License Usage
** Licensees holding a valid commercial qdemviewer license may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement with the copyright holder. For licensing terms
** and conditions and further information contact the copyright holder.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3. The licenses are as published by
** the Free Software Foundation at https://www.gnu.org/licenses/gpl-3.0.html,
** with the exception that the use of this work for training artificial intelligence
** is prohibited for both commercial and non-commercial use.
**
****************************************************************************/

#ifndef TILECACHE_P
#define TILECACHE_P

#include <QtSql/QSqlDatabase>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QMutex>
#include <QFileInfo>
#include <QDir>
#include <QSqlError>
#include <QMutexLocker>
#include <QThread>
#include <QBuffer>
#include <QDebug>
#include <QDataStream>
#include <QImage>
#include "utils_p.h"

class CompoundTileCache {
public:

    static CompoundTileCache& instance()
    {
        static thread_local CompoundTileCache instance;
        return instance;
    }

    CompoundTileCache(CompoundTileCache const&) = delete;
    void operator=(CompoundTileCache const&) = delete;

    bool insert(const QString &tileBaseURL,
                int x,
                int y,
                int sourceZoom,
                int destinationZoom,
                const QImage &tile);

    bool insert(const QString &tileBaseURL,
                int x,
                int y,
                int sourceZoom,
                int destinationZoom,
                const QByteArray &md5,
                const QImage &tile);

    QImage tile(const QString &tileBaseURL,
                    int x,
                    int y,
                    int sourceZoom,
                    int destinationZoom);

    QByteArray tileMD5(const QString &tileBaseURL,
                    int x,
                    int y,
                    int sourceZoom,
                    int destinationZoom);

    QPair<QByteArray, QImage> tileRecord(const QString &tileBaseURL,
                                            int x,
                                            int y,
                                            int sourceZoom,
                                            int destinationZoom);

    QString lockStatus();

    quint64 size() const;
    bool initialized() const { return m_initialized; }
    static QString cachePath();
    static quint64 cacheSize();

protected:
    CompoundTileCache();

    QString m_sqlitePath;
    // DBs
    QSqlDatabase m_diskCache;

    // Queries
    QSqlQuery m_queryCreation;
    QSqlQuery m_queryFetchData;
    QSqlQuery m_queryFetchHash;
    QSqlQuery m_queryFetchBoth;
    QSqlQuery m_queryInsertData;
    QSqlQuery m_queryLockStatus;
    bool m_initialized{false};
};

#endif
