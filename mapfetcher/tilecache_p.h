/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** This file is part of qdemviewer.
**
** This work is licensed under the terms of the Creative Commons
** Attribution-NonCommercial-ShareAlike 4.0 International License,
** with additional clauses. See the LICENSE file in the project root
** for full terms.
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

QByteArray md5QImage(const QImage &i);

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
    QSqlDatabase m_diskCache;

    QSqlQuery m_queryCreation;
    QSqlQuery m_queryFetchData;
    QSqlQuery m_queryFetchHash;
    QSqlQuery m_queryFetchBoth;
    QSqlQuery m_queryInsertData;
    QSqlQuery m_queryLockStatus;
    bool m_initialized{false};
};

#endif
