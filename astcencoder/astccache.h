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
                quint64 x,
                quint64 y,
                quint64 z,
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
    QSqlDatabase m_diskCache;

    QSqlQuery m_queryCreation;
    QSqlQuery m_queryIdx;
    QSqlQuery m_queryCreationMeta;
    QSqlQuery m_queryFetchData;
    QSqlQuery m_queryInsertData;
    QSqlQuery m_queryInsertMetadata;
    QSqlQuery m_queryHasData;
    bool m_initialized{false};
};


#endif
