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
          const QString &key_,  // TODO: rename into nestingKey
          const QVariant &query_id);
    Query(const QVariantMap &m, bool nested = false);
    QString query; // SELECT ...
    QVariantMap args; // { {":pk" , 42}, {":name", "foo" } } // can also contain non-used arguments,
                      // that will be forwarded in the response, for client side filtering
    QString key; // the key under which the result will be placed in the output variant map
                 // e.g., { "query" : ...., "query_result" : {} } <- key was "query_result"
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
// the manager is used via singleton by adapter
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

    static void initDb(const QString &path, const QStringList &creation); // Public since it has to be called before starting the reactor
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

// Server side
class SqliteAdapter : public QObject
{
    Q_OBJECT
public:
    SqliteAdapter(QObject *parent = nullptr);

Q_SIGNALS:
    void queryResult(const QVariantMap data);
    void row(const QVariantMap data);

public Q_SLOTS:
    // Requires: "query_id" (a unique int that will included in the response)
    // "query" (string), "args" (variantList), "numCols" (int) (only for select)
    void sqliteSelect(const QVariantMap data);
    // like above, but query, args are grouped in a single VariantMap that also has a list of the same, named "nested".
    // they will be executed in sequence
    // numCols not used, as it will be using QSqlRecord.
    // args are a variant map with binding argument as key using the :<column> syntax
    void sqliteSelectMulti(const QVariantMap data);
    // the progressive versions emit row, not queryResult
    void sqliteSelectProgressive(const QVariantMap data);
    void sqliteSelectMultiProgressive(const QVariantMap data);
    void sqliteInsert(const QVariantMap data);
    void sqliteDelete(const QVariantMap data);
    void sqliteUpdate(const QVariantMap data);

    QVariantMap sqliteSelectSync(const QVariantMap data);
    /*
        purpose of Select Multi is to address problems of the type:
        "Give me all apartments within these ranges AND for each apartment give me all the pictures of the listing and
        details of all previous sales"
        with the response returned in a single enriched map.

        The risk of using such method is 1) computational complexity and 2) bandwidth usage.
        For this reason async approaches are included and should be preferred.

        An example is documented in the .cpp
    */
    QVariantMap sqliteSelectMultiSync(const QVariantMap data);
public:
    static QVariantMap toQuery(const QVariant &query_id, const QString &query, const QVariantMap &queryArgs);

public:
    int m_counter = 0;
};

// Client side
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

    // Ignore double-signalling problem. Client should ignore signals related to these query_ids
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
    QSharedPointer<QRemoteObjectDynamicReplica> sqliteReplica;// holds reference to replica
    QScopedPointer<QRemoteObjectNode> node;

    QEventLoop m_syncLoop;
    QTimer m_timeoutTimer;
};

#endif // SQLITEADAPTER_H
