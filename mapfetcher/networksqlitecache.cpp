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

#include "networksqlitecache_p.h"
#include "utils_p.h"
#include <QFileInfo>
#include <QRandomGenerator>
#include <QDateTime>

namespace {
class ScopeExit {
public:
    ScopeExit(std::function<void()> callback)
        : callback_{ callback }
    {}
    ~ScopeExit()
    {
        callback_();
    }
private:
    std::function<void()> callback_;
};
static QString randomString(int length)
{
   const QString possibleCharacters("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
   const int randomStringLength = length; // assuming you want random strings of 12 characters

   QString randomString;
   for(int i=0; i<randomStringLength; ++i)
   {
       int index = QRandomGenerator::global()->generate() % possibleCharacters.length();
       QChar nextChar = possibleCharacters.at(index);
       randomString.append(nextChar);
   }
   return randomString;
}
}

NetworkSqliteCache::NetworkSqliteCache(const QString &sqlitePath, QObject *parent)
: NetworkInMemoryCache(parent), m_sqlitePath(sqlitePath)
{
    {
        QFileInfo fi(m_sqlitePath);
        if (!fi.dir().exists() && !QDir::root().mkpath(fi.dir().path())) {
            qWarning() << "NetworkSqliteCache QDir::root().mkpath " << fi.dir().path() << " Failed";
            return;
        }
    }

// Somehow this block always returns not writable
//    {
//        QFileInfo fi(m_sqlitePath);
//        const bool writable = fi.isWritable();
//        if (!writable) {
//            qWarning() << "NetworkSqliteCache " << fi.dir().path() << " not writable.";
//            return;
//        }
//    }

    // Create the database if not present and open
    QString connectionName = randomString(6);
    m_diskCache = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    m_diskCache.setDatabaseName(sqlitePath);
    if (!m_diskCache.open()) {
        qWarning("Impossible to create the SQLITE database for the cache");
        return;
    }

    qDebug () << "NetworkSqliteCache: Opened "<<m_diskCache.databaseName() << m_diskCache.isOpen() << m_diskCache.lastError();

    static constexpr char schema[] = R"(
CREATE TABLE IF NOT EXISTS Document (
      url  TEXT PRIMARY KEY
    , metadata BLOB
    , data BLOB
    , lastAccess DATETIME DEFAULT CURRENT_TIMESTAMP
)
    )";

    static constexpr char tsindex[] = R"(
CREATE INDEX IF NOT EXISTS idxLastAccess ON Document(lastAccess);
    )";

    m_queryCreation = QSqlQuery(m_diskCache);
    m_queryCreation.setForwardOnly(true);
    bool res =   m_queryCreation.exec(QLatin1String(schema));
    if (!res)
        qWarning() << "Failed to create Document table"  << m_queryCreation.lastError() <<  __FILE__ << __LINE__;
    m_queryCreation.finish();
    m_diskCache.commit();

    m_queryIdx = QSqlQuery(m_diskCache);
    m_queryIdx.setForwardOnly(true);
    res =   m_queryIdx.exec(QLatin1String(tsindex));
    if (!res)
        qWarning() << "Failed to create idxLastAccess"  << m_queryIdx.lastError() <<  __FILE__ << __LINE__;
    m_queryIdx.finish();
    m_diskCache.commit();

    m_queryFetchData = QSqlQuery(m_diskCache);
    m_queryFetchData.setForwardOnly(true);
    m_queryFetchData.prepare(QStringLiteral("SELECT metadata, data, lastAccess FROM Document WHERE url = :url"));

    m_queryUpdateTs = QSqlQuery(m_diskCache);
    m_queryUpdateTs.setForwardOnly(true);
    m_queryUpdateTs.prepare(QStringLiteral("UPDATE Document SET lastAccess = :ts WHERE url = :url"));

    m_queryUpdateMetadata = QSqlQuery(m_diskCache);
    m_queryUpdateMetadata.setForwardOnly(true);
    m_queryUpdateMetadata.prepare(QStringLiteral("UPDATE Document SET metadata = :metadata WHERE url = :url"));

    m_queryInsertData = QSqlQuery(m_diskCache);
    m_queryInsertData.setForwardOnly(true);
    m_queryInsertData.prepare(QStringLiteral("INSERT INTO Document(metadata, data, url) VALUES (:metadata, :data, :url)"));

    m_queryUpdateData = QSqlQuery(m_diskCache);
    m_queryUpdateData.setForwardOnly(true);
    m_queryUpdateData.prepare(QStringLiteral("UPDATE Document SET metadata = :metadata, data = :data WHERE url = :url"));

    m_queryCheckUrl = QSqlQuery(m_diskCache);
    m_queryCheckUrl.setForwardOnly(true);
    m_queryCheckUrl.prepare(QStringLiteral("SELECT url FROM Document WHERE url = :url"));


    m_initialized = true;
}

NetworkSqliteCache::~NetworkSqliteCache() {}

QNetworkCacheMetaData NetworkSqliteCache::metaData(const QUrl &url) {
    ScopeExit releaser([this]() {m_queryFetchData.finish();});
    QUrl u = url;
    u.setHost(hostWildcard(url.host()));

    m_queryFetchData.bindValue(0, u);
    if (!m_queryFetchData.exec()) {
        qDebug() << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    if (m_queryFetchData.first()) {
        QBuffer data;
        data.open(QBuffer::ReadWrite);
        data.buffer() = m_queryFetchData.value(0).toByteArray();
        QDataStream in(&data);
        QNetworkCacheMetaData res;
        in >> res;

        //TODO: enable this conditionally
        res.setExpirationDate(QDateTime::currentDateTime().addDays(365));
        // mangle URL as well, give requestor what they asked for
        res.setUrl(url);
        return res;
    }
    return {};
}

void NetworkSqliteCache::updateMetaData(const QNetworkCacheMetaData &metaData) {
    if (!contains(metaData.url()))
        return;
    ScopeExit releaser([this]() {m_queryUpdateMetadata.finish();});
    QBuffer metadata;
    metadata.open(QBuffer::ReadWrite);
    QDataStream out(&metadata);
    out << metaData;
    metadata.close();

    QUrl u = metaData.url();
    u.setHost(hostWildcard(u.host()));

    m_queryUpdateMetadata.bindValue(0, /* metadata */ metadata.buffer());
    m_queryUpdateMetadata.bindValue(1, /* url */ u);

    if (!m_queryUpdateMetadata.exec()) {
        qDebug() << m_queryUpdateMetadata.lastError() <<  __FILE__ << __LINE__;
        return;
    }
}

QIODevice *NetworkSqliteCache::data(const QUrl &url) {
    ScopeExit releaser([this]() {m_queryFetchData.finish();});
    QUrl u = url;
    u.setHost(hostWildcard(u.host()));

    m_queryFetchData.bindValue(0, u);
    if (!m_queryFetchData.exec()) {
        qDebug() << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;
        return nullptr;
    }

    if (m_queryFetchData.first()) {
        QBuffer *res = new QBuffer();
        res->open(QBuffer::ReadWrite);
        res->buffer() = m_queryFetchData.value(1).toByteArray();

        return res;
    }
    return nullptr;
}

bool NetworkSqliteCache::remove(const QUrl &/*url*/) {
    //TODO: fixme, Currently removing from cache not supported.
    return false;
}

qint64 NetworkSqliteCache::cacheSize() const {
    return QFileInfo(m_sqlitePath).size();
}

void NetworkSqliteCache::insert(QIODevice *device) {
    if (!m_inserting.contains(device))
        return;
    QUrl url = std::move(m_inserting[device]);
    m_inserting.remove(device);
    QScopedPointer<QIODevice> data;
    data.swap(m_insertingData[url]);
    m_insertingData.erase(url);
    auto meta = m_insertingMetadata.at(url);
    m_insertingMetadata.erase(url);
    QBuffer *buffer = qobject_cast<QBuffer *>(data.data());
    QBuffer metadata;
    metadata.open(QBuffer::ReadWrite);
    QDataStream out(&metadata);
    out << meta;
    metadata.close();


    QSqlQuery *q = (contains(url)) ? &m_queryUpdateData : &m_queryInsertData;

    ScopeExit releaser([q]() {q->finish();});
    // Fire insert query
    url.setHost(hostWildcard(url.host()));
    q->bindValue(0, /* metadata */ metadata.buffer());
    q->bindValue(1, /* data */ buffer->buffer());
    q->bindValue(2, /* url */ url);

    if (!q->exec())
        qDebug() << "Insert query failed!" << q->lastError() << url << __FILE__ << __LINE__;
}

void NetworkInMemoryCache::addEquivalenceClass(const QString &urlTemplate)
{
    const auto res = extractTemplates(urlTemplate);
    if (res.alternatives.size() <= 1
            || res.hostAlternatives.size() <= 1
            || res.hostWildcarded.isEmpty())
        return;

    for (const auto &h: res.hostAlternatives)
        m_host2wildcard[h] = res.hostWildcarded;
}

QString NetworkInMemoryCache::hostWildcard(const QString &host)
{
    const auto it = m_host2wildcard.find(host);
    if (it == m_host2wildcard.end())
        return host;
    return it->second;
}

void NetworkSqliteCache::clear() {
    // TODO: Implement
}

bool NetworkSqliteCache::contains(QUrl url) {
    ScopeExit releaser([this]() {m_queryCheckUrl.finish();});
    url.setHost(hostWildcard(url.host()));
    m_queryCheckUrl.bindValue(0, url);
    if (!m_queryCheckUrl.exec()) {
        qDebug() << m_queryCheckUrl.lastError() <<  __FILE__ << __LINE__;
        return false;
    }
    return m_queryCheckUrl.first();
}

NetworkInMemoryCache::NetworkInMemoryCache(QObject *parent) : QAbstractNetworkCache(parent) {}

NetworkInMemoryCache::~NetworkInMemoryCache() {}

QNetworkCacheMetaData NetworkInMemoryCache::metaData(const QUrl &url) {
    QUrl u = url;
    u.setHost(hostWildcard(url.host()));

    if (m_metadata.contains(u)) {
        auto res = m_metadata.value(u);
        res.setUrl(url);
    }
    return {};
}

void NetworkInMemoryCache::updateMetaData(const QNetworkCacheMetaData &metaData) {
    QUrl url = metaData.url();
    QIODevice *oldDevice = data(url);
    if (!oldDevice) {
        return;
    }
    QIODevice *newDevice = prepare(metaData);
    if (!newDevice) {
        return;
    }
    char data_[1024];
    while (!oldDevice->atEnd()) {
        qint64 s = oldDevice->read(data_, 1024);
        newDevice->write(data_, s);
    }
    delete oldDevice;
    insert(newDevice);
}

QIODevice *NetworkInMemoryCache::data(const QUrl &url) {
    QUrl u = url;
    u.setHost(hostWildcard(url.host()));

    if (m_content.find(u) != m_content.end()) {
        const QBuffer *b = qobject_cast<const QBuffer *>(m_content[u].data());
        QBuffer *res = new QBuffer();
        res->setData(b->buffer());
        res->open(QBuffer::ReadOnly);
        return res;
    }
    return nullptr;
}

bool NetworkInMemoryCache::remove(const QUrl &url) {
    if (!m_metadata.contains(url))
        return false;
    m_metadata.remove(url);
    m_content.erase(url);
    if (m_insertingData.find(url) != m_insertingData.end()) {
        m_inserting.remove(m_insertingData[url].data());
        m_insertingData.erase(url);
    }
    return true;
}

qint64 NetworkInMemoryCache::cacheSize() const {
    quint64 size{0};
    for (const auto &i: m_content) {
        size += i.second->size();
    }
    return size;
}

QIODevice *NetworkInMemoryCache::prepare(const QNetworkCacheMetaData &metaData) {
    if (!metaData.isValid() || !metaData.url().isValid() /*|| !metaData.saveToDisk()*/)
        return nullptr;

    QIODevice *device = new QBuffer;

    const auto &url = metaData.url();
    if (m_insertingData.find(url) != m_insertingData.end())
        return m_insertingData[url].data();

    m_inserting[device] = metaData.url();
    m_insertingData[url].reset(device);
    m_insertingMetadata[url] = metaData;
    device->open(QBuffer::ReadWrite);
    return device;
}

void NetworkInMemoryCache::insert(QIODevice *device) {
    if (!m_inserting.contains(device))
        return;
    QUrl url = std::move(m_inserting[device]);
    m_inserting.remove(device);
    QScopedPointer<QIODevice> data;
    data.swap(m_insertingData[url]);
    m_insertingData.erase(url);

    QNetworkCacheMetaData meta = std::move(m_insertingMetadata[url]);
    m_insertingMetadata.erase(url);

    url.setHost(hostWildcard(url.host()));

    m_content[url].swap(data);
    m_metadata[url] = std::move(meta);
}

void NetworkInMemoryCache::clear() {
    m_metadata.clear();
    m_content.clear();
}
