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

#include "tilecache_p.h"
#include <QFileInfo>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QStandardPaths>

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

ASTCCache::ASTCCache(const QString &sqlitePath) : m_sqlitePath(sqlitePath) {
    QFileInfo fi(m_sqlitePath);
    if (!fi.dir().exists() && !QDir::root().mkpath(fi.dir().path())) {
        qWarning() << "ASTCCache QDir::root().mkpath " << fi.dir().path() << " Failed";
        return;
    }

    // Create the database if not present and open
    QString connectionName = randomString(6);
    m_diskCache = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    m_diskCache.setDatabaseName(sqlitePath);
    if (!m_diskCache.open()) {
        qWarning("Impossible to create the SQLITE database for the cache");
        return;
    }

    qDebug () << "ASTCCache: Opened "<<m_diskCache.databaseName() << m_diskCache.isOpen() << m_diskCache.lastError();

    static constexpr char schema[] = R"(
    CREATE TABLE IF NOT EXISTS Tile (
          tileHash BLOB
        , blockX INTEGER
        , blockY INTEGER
        , quality REAL
        , width INTEGER
        , height INTEGER
        , tile BLOB
        , PRIMARY KEY (tileHash, blockX, blockY, quality, width, height)
    )
    )";

    m_queryCreation = QSqlQuery(m_diskCache);
    m_queryCreation.setForwardOnly(true);
    bool res =   m_queryCreation.exec(QLatin1String(schema));
    if (!res)
        qWarning() << "Failed to create Tile table"  << m_queryCreation.lastError() <<  __FILE__ << __LINE__;

    m_queryFetchData = QSqlQuery(m_diskCache);
    m_queryFetchData.setForwardOnly(true);
    res = m_queryFetchData.prepare(QStringLiteral(
        "SELECT tile FROM Tile WHERE tileHash = :hash AND blockX = :blockX AND blockY = :blockY AND quality = :quality AND width = :width AND height = :height"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryFetchData"  << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;


    m_queryInsertData = QSqlQuery(m_diskCache);
    m_queryInsertData.setForwardOnly(true);
    res = m_queryInsertData.prepare(QStringLiteral(
        "INSERT INTO Tile(tileHash, blockX, blockY, quality, width, height, tile) VALUES (:hash, :blockX, :blockY, :quality, :width, :height, :tile)"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryInsertData"  << m_queryInsertData.lastError() <<  __FILE__ << __LINE__;

    m_queryHasData = QSqlQuery(m_diskCache);
    m_queryHasData.setForwardOnly(true);
    res = m_queryHasData.prepare(QStringLiteral(
        "SELECT count(*) FROM Tile WHERE tileHash = :hash AND blockX = :blockX AND blockY = :blockY AND quality = :quality"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryInsertData"  << m_queryHasData.lastError() <<  __FILE__ << __LINE__;


    m_initialized = true;
}

bool ASTCCache::insert(const QByteArray &tileHash,
                       int blockX,
                       int blockY,
                       float quality,
                       int width,
                       int height,
                       const QByteArray &tile)
{
    ScopeExit releaser([this]() {m_queryInsertData.finish();});
    m_queryInsertData.bindValue(0, tileHash);
    m_queryInsertData.bindValue(1, blockX);
    m_queryInsertData.bindValue(2, blockY);
    m_queryInsertData.bindValue(3, quality);
    m_queryInsertData.bindValue(4, width);
    m_queryInsertData.bindValue(5, height);
    m_queryInsertData.bindValue(6, tile);

    if (!m_queryInsertData.exec()) {
        qDebug() << m_queryInsertData.lastError() <<  __FILE__ << __LINE__ << "for "<< width << ","<<height;
        return false;
    }
    m_diskCache.commit();
    return true;
}

QByteArray ASTCCache::tile(const QByteArray &tileHash,
                           int blockX,
                           int blockY,
                           float quality,
                           int width,
                           int height)
{
    ScopeExit releaser([this]() {m_queryFetchData.finish();});
    m_queryFetchData.bindValue(0, tileHash);
    m_queryFetchData.bindValue(1, blockX);
    m_queryFetchData.bindValue(2, blockY);
    m_queryFetchData.bindValue(3, quality);
    m_queryFetchData.bindValue(4, width);
    m_queryFetchData.bindValue(5, height);

    if (!m_queryFetchData.exec()) {
        qDebug() << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    if (m_queryFetchData.first())
        return m_queryFetchData.value(0).toByteArray();
    return {};
}

bool ASTCCache::contains(const QByteArray &tileHash,
                         int blockX,
                         int blockY,
                         float quality)
{
    ScopeExit releaser([this]() {m_queryHasData.finish();});
    m_queryHasData.bindValue(0, tileHash);
    m_queryHasData.bindValue(1, blockX);
    m_queryHasData.bindValue(2, blockY);
    m_queryHasData.bindValue(3, quality);
    if (!m_queryHasData.exec()) {
        qDebug() << m_queryHasData.lastError() <<  __FILE__ << __LINE__;
        return {};
    }
    return bool(m_queryHasData.value(0).toInt());
}

quint64 ASTCCache::size() const
{
    QFileInfo fi(m_sqlitePath);
    return fi.size();
}

CompoundTileCache::CompoundTileCache()
    : m_sqlitePath(cachePath()) {
    if (m_sqlitePath.isEmpty())
        return;

    QFileInfo fi(m_sqlitePath);
    if (!fi.dir().exists() && !QDir::root().mkpath(fi.dir().path())) {
        qWarning() << "ASTCCache QDir::root().mkpath " << fi.dir().path() << " Failed";
        return;
    }

    // Create the database if not present and open
    QString connectionName = randomString(6);
    m_diskCache = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    m_diskCache.setDatabaseName(m_sqlitePath);
    if (!m_diskCache.open()) {
        qWarning("Impossible to create the SQLITE database for the cache");
        return;
    }

    qDebug () << "CompoundTileCache("<< QThread::currentThread()->objectName() <<"): Opened "<<m_diskCache.databaseName() << m_diskCache.isOpen() << m_diskCache.lastError();

    static constexpr char schema[] = R"(
    CREATE TABLE IF NOT EXISTS Tile (
          baseURL TEXT
        , x INTEGER
        , y INTEGER
        , z INTEGER
        , dz INTEGER
        , md5 BLOB
        , tile BLOB
        , PRIMARY KEY (baseURL, x, y, z, dz)
    )
    )";

    m_queryCreation = QSqlQuery(m_diskCache);
    m_queryCreation.setForwardOnly(true);
    bool res =   m_queryCreation.exec(QLatin1String(schema));
    if (!res)
        qWarning() << "Failed to create Tile table"  << m_queryCreation.lastError() <<  __FILE__ << __LINE__;

    m_queryFetchData = QSqlQuery(m_diskCache);
    m_queryFetchData.setForwardOnly(true);
    res = m_queryFetchData.prepare(QStringLiteral(
        "SELECT tile FROM Tile WHERE baseURL = :baseUrl "
        "AND x = :x AND y = :y AND z = :z AND dz = :dz"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryFetchData"  << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;

    m_queryFetchHash = QSqlQuery(m_diskCache);
    m_queryFetchHash.setForwardOnly(true);
    res = m_queryFetchHash.prepare(QStringLiteral(
        "SELECT md5 FROM Tile WHERE baseURL = :baseUrl "
        "AND x = :x AND y = :y AND z = :z AND dz = :dz"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryFetchHash"  << m_queryFetchHash.lastError() <<  __FILE__ << __LINE__;

    m_queryFetchBoth = QSqlQuery(m_diskCache);
    m_queryFetchBoth.setForwardOnly(true);
    res = m_queryFetchBoth.prepare(QStringLiteral(
        "SELECT md5, tile FROM Tile WHERE baseURL = :baseUrl "
        "AND x = :x AND y = :y AND z = :z AND dz = :dz"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryFetchHash"  << m_queryFetchBoth.lastError() <<  __FILE__ << __LINE__;


    m_queryInsertData = QSqlQuery(m_diskCache);
    m_queryInsertData.setForwardOnly(true);
    res = m_queryInsertData.prepare(QStringLiteral(
        "INSERT INTO Tile(baseURL, x, y, z, dz, md5, tile) "
        "VALUES (:baseURL, :x, :y, :z, :dz, :md5, :tile)"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryInsertData"  << m_queryInsertData.lastError() <<  __FILE__ << __LINE__;

    m_queryLockStatus = QSqlQuery(m_diskCache);
    m_queryLockStatus.setForwardOnly(true);
    res = m_queryLockStatus.prepare(QStringLiteral(
        "pragma lock_status"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryLockStatus"  << m_queryLockStatus.lastError() <<  __FILE__ << __LINE__;


    m_initialized = true;
}

bool CompoundTileCache::insert(const QString &tileBaseURL,
                               int x,
                               int y,
                               int sourceZoom,
                               int destinationZoom,
                               const QImage &tile)
{
    if (!m_initialized)
        return false;

    return insert(tileBaseURL, x, y, sourceZoom, destinationZoom, md5QImage(tile), tile);
}

bool CompoundTileCache::insert(const QString &tileBaseURL,
                               int x,
                               int y,
                               int sourceZoom,
                               int destinationZoom,
                               const QByteArray &md5,
                               const QImage &tile)
{
    if (!m_initialized)
        return false;

    ScopeExit releaser([this]() {m_queryInsertData.finish();});
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    tile.save(&buffer, "PNG", 0);

    int count = 0;
    bool res = 0;
    while (!res) {
        m_queryInsertData.bindValue(0, tileBaseURL);
        m_queryInsertData.bindValue(1, x);
        m_queryInsertData.bindValue(2, y);
        m_queryInsertData.bindValue(3, sourceZoom);
        m_queryInsertData.bindValue(4, destinationZoom);
        m_queryInsertData.bindValue(5, md5);
        m_queryInsertData.bindValue(6, data);

        res = m_queryInsertData.exec();
        if (!res) {
            ++count;
            qDebug() << m_queryInsertData.lastError() << __FILE__ << __LINE__;
//                     <<  m_queryInsertData.lastError().nativeErrorCode()
//                     <<  m_queryInsertData.lastError().databaseText()
//                     <<  m_queryInsertData.lastError().driverText() << __FILE__ << __LINE__;
            if (count >= 10) {
                return false;
            }
        }
    }
    m_diskCache.commit();
    return true;
}

QImage CompoundTileCache::tile(const QString &tileBaseURL,
                                   int x,
                                   int y,
                                   int sourceZoom,
                                   int destinationZoom)
{
    if (!m_initialized)
        return {};

    ScopeExit releaser([this]() {m_queryFetchData.finish();});
    m_queryFetchData.bindValue(0, tileBaseURL);
    m_queryFetchData.bindValue(1, x);
    m_queryFetchData.bindValue(2, y);
    m_queryFetchData.bindValue(3, sourceZoom);
    m_queryFetchData.bindValue(4, destinationZoom);

    if (!m_queryFetchData.exec()) {
        qDebug() << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    if (m_queryFetchData.first()) {
        auto data = m_queryFetchData.value(0).toByteArray();
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        QImage res;
        res.load(&buffer, "PNG");
        return res;
    }
    return {};
}

QByteArray CompoundTileCache::tileMD5(const QString &tileBaseURL, int x, int y, int sourceZoom, int destinationZoom)
{
    if (!m_initialized)
        return {};
    ScopeExit releaser([this]() {m_queryFetchHash.finish();});
    m_queryFetchHash.bindValue(0, tileBaseURL);
    m_queryFetchHash.bindValue(1, x);
    m_queryFetchHash.bindValue(2, y);
    m_queryFetchHash.bindValue(3, sourceZoom);
    m_queryFetchHash.bindValue(4, destinationZoom);

    if (!m_queryFetchHash.exec()) {
        qDebug() << m_queryFetchHash.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    if (m_queryFetchHash.first())
        return m_queryFetchHash.value(0).toByteArray();
    return {};
}

QPair<QByteArray, QImage> CompoundTileCache::tileRecord(const QString &tileBaseURL, int x, int y, int sourceZoom, int destinationZoom)
{
    if (!m_initialized)
        return {};
    ScopeExit releaser([this]() {m_queryFetchBoth.finish();});
    m_queryFetchBoth.bindValue(0, tileBaseURL);
    m_queryFetchBoth.bindValue(1, x);
    m_queryFetchBoth.bindValue(2, y);
    m_queryFetchBoth.bindValue(3, sourceZoom);
    m_queryFetchBoth.bindValue(4, destinationZoom);

    if (!m_queryFetchBoth.exec()) {
        qDebug() << m_queryFetchBoth.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    if (m_queryFetchBoth.first()) {
        auto md5 = m_queryFetchBoth.value(0).toByteArray();
        auto imageData = m_queryFetchBoth.value(1).toByteArray();
        QBuffer buffer(&imageData);
        buffer.open(QIODevice::ReadOnly);
        QImage ima;
        ima.load(&buffer, "PNG");
        return QPair<QByteArray, QImage>(md5, ima);
    }
    return {};
}

QString CompoundTileCache::lockStatus()
{
    if (!m_initialized)
        return {};


    if (!m_queryLockStatus.exec()) {
        qDebug() << m_queryLockStatus.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    if (m_queryLockStatus.first()) {
        auto res = m_queryLockStatus.value(0).toString();
        qDebug() << "CompoundTileCache("<< QThread::currentThread()->objectName() <<") lock status: "<<res;
    }
    return {};
}

quint64 CompoundTileCache::size() const
{
    if (!m_initialized)
        return 0;
    QFileInfo fi(m_sqlitePath);
    return fi.size();
}

QString CompoundTileCache::cachePath()
{
    return QStringLiteral("%1/compoundCache.sqlite").arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
}

quint64 CompoundTileCache::cacheSize()
{
    QFileInfo fi(cachePath());
    return fi.size();
}

QByteArray md5QImage(const QImage &i)
{
    QCryptographicHash h(QCryptographicHash::Md5);
    h.addData(reinterpret_cast<const char *>(i.constBits()), i.sizeInBytes());
    return h.result();
}
