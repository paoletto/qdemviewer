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

#include "astccache_p.h"
#include <QFileInfo>
#include <QRandomGenerator>

namespace {
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

    qDebug () << "Opened "<<m_diskCache.databaseName() << m_diskCache.isOpen() << m_diskCache.lastError();

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
        qWarning() << "Failed to prepare  m_queryInsertData"  << m_queryFetchData.lastError() <<  __FILE__ << __LINE__;

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
    m_queryInsertData.bindValue(0, tileHash);
    m_queryInsertData.bindValue(1, blockX);
    m_queryInsertData.bindValue(2, blockY);
    m_queryInsertData.bindValue(3, quality);
    m_queryInsertData.bindValue(4, width);
    m_queryInsertData.bindValue(5, height);
    m_queryInsertData.bindValue(6, tile);

    if (!m_queryInsertData.exec()) {
        qDebug() << m_queryInsertData.lastError() <<  __FILE__ << __LINE__;
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

quint64 ASTCCache::size() const
{
    return 1234; //TODO: fixme
}
