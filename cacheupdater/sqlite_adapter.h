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

#ifndef SQLITEADAPTER_H
#define SQLITEADAPTER_H

#include <QObject>
#include <QDebug>
#include <QDateTime>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCall>
#include <QFileInfo>
#include <QDir>
#include <QSqlError>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QMutexLocker>
#include <QThread>
#include <QVariantMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDataStream>
#include <QEventLoop>
#include <QTimer>

namespace  {
void print(const QVariantMap &data) {
    QJsonDocument js = QJsonDocument::fromVariant(data);
    QString jsonString = js.toJson(QJsonDocument::Indented);
    qDebug().noquote() << jsonString;
}
}

struct Query {
    Query() = default;
    Query(const QString &query_,
          const QVariantMap &args_,
          const QString &key_,
          const QVariant &query_id);
    Query(const QVariantMap &m, bool nested = false);
    QString query;
    QVariantMap args;
    QString key;
    QVariant queryId;
    bool valid = false;

    QVariantMap toMap() const;

    bool isValid() const;
};

struct NestedQuery
{
    Query query;
    QList<QSharedPointer<NestedQuery>> nested;
    QSharedPointer<NestedQuery> prev;
    bool valid = false;

    static QSharedPointer<NestedQuery> toNested(const QVariantMap &data);

    QVariantMap toMap() const;

    static QSharedPointer<NestedQuery> makeShared();
};

class QueryRecord;
struct SQLiteManager {
    static void setDatabase(const QString &path);
    static void setCreationStatement(const QStringList &create);
    static SQLiteManager& instance();

    QVariantMap sqliteSelectMulti(const QSharedPointer<NestedQuery> data, const QVariant queryId);
    QVariantMap sqliteSelect(const QVariantMap data);
    QVariantMap sqliteInsert(const QVariantMap data);
    QVariantMap sqliteDelete(const QVariantMap data);
    QVariantMap sqliteUpdate(const QVariantMap data);
public:
    SQLiteManager(SQLiteManager const&)       = delete;
    void operator=(SQLiteManager const&)      = delete;

    static void initDb(const QString &path, const QStringList &creation);
private:
    SQLiteManager();

protected:
    QVariantList sqliteSelect(const QSharedPointer<NestedQuery> data, const QSharedPointer<QueryRecord> parent);
    void initDb_real();

    QString m_dbPath;
    QStringList m_creation;
    QMutex m_mutex;
    QSqlDatabase m_db;
    int counter = 0;
};

class SqliteAdapter : public QObject
{
    Q_OBJECT
public:
    SqliteAdapter(QObject *parent = nullptr);

Q_SIGNALS:
    void queryResult(const QVariantMap data);
    void row(const QVariantMap data);

public Q_SLOTS:
    void sqliteSelect(const QVariantMap data);
    void sqliteSelectMulti(const QVariantMap data);
    void sqliteSelectProgressive(const QVariantMap data);
    void sqliteSelectMultiProgressive(const QVariantMap data);
    void sqliteInsert(const QVariantMap data);
    void sqliteDelete(const QVariantMap data);
    void sqliteUpdate(const QVariantMap data);

    QVariantMap sqliteSelectSync(const QVariantMap data);
    QVariantMap sqliteSelectMultiSync(const QVariantMap data);
public:
    static QVariantMap toQuery(const QVariant &query_id, const QString &query, const QVariantMap &queryArgs);

public:
    int m_counter = 0;
};

class DbClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool initialized READ isInitialized NOTIFY initializedChanged)
public:
    DbClient(QUrl serverUrl, QObject *parent = nullptr);
    ~DbClient();

    bool isInitialized() const;

Q_SIGNALS:
    void error(const QString &error);
    void initializedChanged();
    void queryFinished(const QVariantMap data);
    void rowReceived(const QVariantMap row);

public Q_SLOTS:
    void submitSelectProgressive(const Query &query);
    void submitSelect(const Query &query);
    void submitSelectMulti(const QString &query_id, const QSharedPointer<NestedQuery> &query);
    void submitSelectMultiProgressive(const QString &query_id, const QSharedPointer<NestedQuery> &query);
    void submitInsert(const Query &query);
    void submitDelete(const Query &query);
    void submitUpdate(const Query &query);

    QVariantMap select(const Query &query);
    QVariantMap selectMulti(const QVariant &query_id, const QSharedPointer<NestedQuery> &query);

public:
    QVariantMap syncSelect(const QVariantMap &query, const QLatin1String &selectType);

protected Q_SLOTS:
    void onQueryResult(const QVariantMap data);
    void onRowReceived(const QVariantMap data);
    void onInitialized();
    void onStateChanged(QRemoteObjectReplica::State state,
                        QRemoteObjectReplica::State oldState);
    void onNodeError(QRemoteObjectNode::ErrorCode errorCode);

public:
    bool m_initialized = false;
    QSharedPointer<QRemoteObjectDynamicReplica> sqliteReplica;
    QScopedPointer<QRemoteObjectNode> node;

    QEventLoop m_syncLoop;
    QTimer m_timeoutTimer;
};

#endif // SQLITEADAPTER_H
