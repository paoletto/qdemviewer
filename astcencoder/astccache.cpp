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

#include "astccache.h"

#include <QString>
#include <QFileInfo>
#include <QDateTime>
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
          tileHash TEXT
        , blockX INTEGER
        , blockY INTEGER
        , quality REAL
        , width INTEGER
        , height INTEGER
        , tile BLOB
        , ts DATETIME DEFAULT NULL
        , x INTEGER DEFAULT NULL
        , y INTEGER DEFAULT NULL
        , z INTEGER DEFAULT NULL
        , PRIMARY KEY (tileHash, blockX, blockY, quality, width, height)
    )
    )";

    static constexpr char tsindex[] = R"(
    CREATE INDEX IF NOT EXISTS idxLastAccess ON Tile(ts);
    )";


    m_queryCreation = QSqlQuery(m_diskCache);
    m_queryCreation.setForwardOnly(true);
    bool res =   m_queryCreation.exec(QLatin1String(schema));
    if (!res)
        qWarning() << "Failed to create Tile table"  << m_queryCreation.lastError() <<  __FILE__ << __LINE__;
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
    res = m_queryFetchData.prepare(QStringLiteral(
        "SELECT tile FROM Tile WHERE tileHash = :hash AND blockX = :blockX AND blockY = :blockY AND quality = :quality AND width = :width AND height = :height"));
    if (!res)
        qWarning() << "Failed to prepare  m_queryFetchData"  << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;


    m_queryInsertData = QSqlQuery(m_diskCache);
    m_queryInsertData.setForwardOnly(true);
    res = m_queryInsertData.prepare(QStringLiteral(
        "INSERT INTO Tile(tileHash, blockX, blockY, quality, width, height, tile, ts, x, y, z) "
        "VALUES (:hash, :blockX, :blockY, :quality, :width, :height, :tile, :ts, :x, :y, :z)"));
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
                       quint64 x,
                       quint64 y,
                       quint64 z,
                       const QByteArray &tile)
{
    if (!m_initialized) {
        qWarning() << "ASTCCache::insert: database not initialized";
        return false;
    }
    ScopeExit releaser([this]() {
        m_diskCache.commit();
        m_queryInsertData.finish();
    });
    m_queryInsertData.bindValue(0, tileHash.toBase64());
    m_queryInsertData.bindValue(1, blockX);
    m_queryInsertData.bindValue(2, blockY);
    m_queryInsertData.bindValue(3, quality);
    m_queryInsertData.bindValue(4, width);
    m_queryInsertData.bindValue(5, height);
    m_queryInsertData.bindValue(6, tile);
    m_queryInsertData.bindValue(7, QDateTime::currentDateTimeUtc());
    m_queryInsertData.bindValue(8, x);
    m_queryInsertData.bindValue(9, y);
    m_queryInsertData.bindValue(10, z);

    if (!m_queryInsertData.exec()) {
        qDebug() << m_queryInsertData.lastError() <<  __FILE__ << __LINE__ << "for "<< width << ","<<height;
        return false;
    }
    return true;
}

QByteArray ASTCCache::tile(const QByteArray &tileHash,
                           int blockX,
                           int blockY,
                           float quality,
                           int width,
                           int height)
{
    if (!m_initialized) {
        qWarning() << "ASTCCache::tile: database not initialized";
        return {};
    }
    ScopeExit releaser([this]() {m_queryFetchData.finish();});
    m_queryFetchData.bindValue(0, tileHash.toBase64());
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
    if (!m_initialized) {
        qWarning() << "ASTCCache::contains: database not initialized";
        return false;
    }
    ScopeExit releaser([this]() {m_queryHasData.finish();});
    m_queryHasData.bindValue(0, tileHash.toBase64());
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
