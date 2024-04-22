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

#ifndef ASTCACHE_P_H
#define ASTCACHE_P_H

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

class ASTCCache {
public:
    ASTCCache(const QString &sqlitePath);
    virtual ~ASTCCache() {}

    bool insert(const QByteArray &tileHash,
                int blockX,
                int blockY,
                float quality,
                int width,
                int height,
                qint64 x,
                qint64 y,
                qint64 z,
                const QByteArray &tile);

    QByteArray tile(const QByteArray &tileHash,
                    int blockX,
                    int blockY,
                    float quality,
                    int width,
                    int height);

    bool contains(const QByteArray &tileHash, int blockX, int blockY, float quality);

    quint64 size() const;

protected:
    QString m_sqlitePath;
    // DBs
    QSqlDatabase m_diskCache;

    // Queries
    QSqlQuery m_queryCreation;
    QSqlQuery m_queryCreationMeta;
    QSqlQuery m_queryFetchData;
    QSqlQuery m_queryInsertData;
    QSqlQuery m_queryInsertMetadata;
    QSqlQuery m_queryHasData;
    bool m_initialized{false};
};


#endif
