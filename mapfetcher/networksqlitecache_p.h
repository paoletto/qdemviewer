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
#include <unordered_map>
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

    void addEquivalenceClass(const QString &urlTemplate);
    QString hostWildcard(const QString &host);

public Q_SLOTS:
    void clear() override;

protected:
    QMap<QUrl,QNetworkCacheMetaData> m_metadata;
    std::map<QUrl, QScopedPointer<QIODevice>> m_content;
    QMap<QIODevice*, QUrl> m_inserting;
    std::map<QUrl, QScopedPointer<QIODevice>> m_insertingData;
    std::map<QUrl, QNetworkCacheMetaData> m_insertingMetadata;
    std::unordered_map<QString, QString> m_host2wildcard;

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
    bool contains(QUrl url);

protected:
    QString m_sqlitePath;
    QSqlDatabase m_diskCache;

    QSqlQuery m_queryCreation;
    QSqlQuery m_queryIdx;
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
