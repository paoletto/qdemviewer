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

#ifndef NETWORKSQLITECACHE_P_H
#define NETWORKSQLITECACHE_P_H

#include <QtNetwork/QAbstractNetworkCache>
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

class NetworkInMemoryCache : public QAbstractNetworkCache
{
    Q_OBJECT
public:
    NetworkInMemoryCache(QObject *parent = nullptr);
    virtual ~NetworkInMemoryCache();

    QNetworkCacheMetaData metaData(const QUrl &url) override;
    void updateMetaData(const QNetworkCacheMetaData &metaData) override;
    QIODevice *data(const QUrl &url) override;
    bool remove(const QUrl &url) override;
    qint64 cacheSize() const override;
    QIODevice *prepare(const QNetworkCacheMetaData &metaData) override;
    void insert(QIODevice *device) override;

public Q_SLOTS:
    void clear() override;

protected:
    QMap<QUrl,QNetworkCacheMetaData> m_metadata;
    std::map<QUrl, QScopedPointer<QIODevice>> m_content;
    QMap<QIODevice*, QUrl> m_inserting;
    std::map<QUrl, QScopedPointer<QIODevice>> m_insertingData;
    std::map<QUrl, QNetworkCacheMetaData> m_insertingMetadata;

private:
    Q_DISABLE_COPY(NetworkInMemoryCache)
};

class NetworkSqliteCache : public NetworkInMemoryCache
{
    Q_OBJECT
public:
    NetworkSqliteCache(const QString &sqlitePath, QObject *parent = nullptr);
    ~NetworkSqliteCache() override;

    QNetworkCacheMetaData metaData(const QUrl &url) override;
    void updateMetaData(const QNetworkCacheMetaData &metaData) override;
    QIODevice *data(const QUrl &url) override;
    bool remove(const QUrl &url) override;
    qint64 cacheSize() const override;
    void insert(QIODevice *device) override;

public Q_SLOTS:
    void clear() override;

protected:
    bool contains(const QUrl &url);

protected:
    QString m_sqlitePath;
    // DBs
    QSqlDatabase m_diskCache;

    // Queries
    QSqlQuery m_queryCreation; // Used for creation. can't be prepared, since QtSql does not allow multiple statements with sqlite3
    QSqlQuery m_queryFetchData;
    QSqlQuery m_queryUpdateTs;
    QSqlQuery m_queryUpdateMetadata;
    QSqlQuery m_queryUpdateData;
    QSqlQuery m_queryInsertData;
    QSqlQuery m_queryCheckUrl;
    bool m_initialized{false};

private:
    Q_DISABLE_COPY(NetworkSqliteCache)
};

#endif // #define NETWORKSQLITECACHE_H
