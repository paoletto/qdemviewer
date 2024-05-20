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

#include "sqlite_adapter.h"
#include <QSqlRecord>
#include <QSqlField>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#define LOCK()  QMutexLocker m(&m_mutex);
#define UNLOCK() m.unlock();

namespace  {
constexpr const quint16 port{1234};
const QUrl serverUrl{QStringLiteral("tcp://:") + QString::number(port)};
const QString serverObjectName{"dbserver"};
 QString randomString(int length)
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
bool fileExists(QString path)
{
    QFileInfo check_file(path);
    // check if file exists and if yes: Is it really a file and no directory?
    return check_file.exists() && check_file.isFile();
}
}

/*
    Query and NestedQuery
*/

Query::Query(const QString &query_, const QVariantMap &args_, const QString &key_, const QVariant &query_id) // key for nested, query_id for non_nested // ToDo explain!
    : query(query_), args(args_), key(key_), queryId(query_id), valid(true) {}

Query::Query(const QVariantMap &m, bool nested)
{
    if (nested && (!(m.contains("key") || !m.value("key").canConvert<QString>()))) // necessary if nested
        return;
    if (!(m.contains("query") || !m.value("query").canConvert<QString>())) // necessary
        return;
    if (m.contains("args") && !m.value("args").canConvert<QVariantMap>()) // optional
        return;
    if (!nested && !(m.contains("query_id"))) // necessary if !nested
        return;
    valid = true;
    query = m.value("query").toString();
    if (m.contains("args"))
        args = m.value("args").toMap();
    if (nested)
        key = m.value("key").toString();
    else
        queryId = m.value("query_id");
}

QVariantMap Query::toMap() const
{
    QVariantMap res;
    res["query"] = query;
    if (!args.isEmpty())
        res["args"] = args;
    if (!key.isEmpty())
        res["key"] = key;
    if (queryId.isValid())
        res["query_id"] = queryId;
    return res;
}

bool Query::isValid() const { return valid; }

QSharedPointer<NestedQuery> NestedQuery::toNested(const QVariantMap &data)
{
    QSharedPointer<NestedQuery> result(new NestedQuery);
    NestedQuery &res = *result;
    res.query = { data, true };
    if (!res.query.isValid())
        return result;

    if ((data.contains("nested") && data.value("nested").canConvert<QVariantList>())) {
        const QVariantList nested = data.value("nested").toList();
        for (const auto &n: nested) {
            if (!n.canConvert<QVariantMap>())
                return result;
            const QVariantMap nestedQueryArgs = n.toMap();
            QSharedPointer<NestedQuery> nestedQuery = toNested(nestedQueryArgs);
            nestedQuery->prev = result;
            res.nested.append(nestedQuery);
        }
    }
    res.valid = true;
    return result;
}

QVariantMap NestedQuery::toMap() const
{
    QVariantMap res = query.toMap();
    if (!nested.isEmpty()) {
        QVariantList nestedRecords;
        for (const auto &n: nested) {
            QVariantMap record = n->toMap();
            nestedRecords << record;
        }
        res["nested"] = nestedRecords;
    }
    return res;
}

QSharedPointer<NestedQuery> NestedQuery::makeShared() {
    QSharedPointer<NestedQuery> res(new NestedQuery);
    res->valid = true;
    return res;
}

/*
    SqliteAdapter
*/

SqliteAdapter::SqliteAdapter(QObject *parent)
: QObject(parent)
{
}

void SqliteAdapter::sqliteSelect(const QVariantMap data)
{
    QVariantMap res;
    {
        QDebug deb = qDebug();
        deb << "SqliteAdapter::sqliteSelect "<<data["query"];
        res = sqliteSelectSync(data);
        deb << " completed";
    }
#if 0 // ToDo move into QRemoteObjects
    QByteArray ba;
    QDataStream ds(&ba, QIODevice::ReadWrite);
    ds << res;
    QByteArray bc = qCompress(ba, 9);
    qDebug() << "emitting ~"<<ba.size()<<" - "<< bc.size() <<"bytes";
#endif
    emit queryResult(res);
}

void SqliteAdapter::sqliteSelectMulti(const QVariantMap data)
{

    emit queryResult(sqliteSelectMultiSync(data));
}

void SqliteAdapter::sqliteSelectProgressive(const QVariantMap data)
{
    qDebug() << "SqliteAdapter::sqliteSelectProgressive "<<data;
    QVariant queryId = data.value("query_id");
    QVariantMap queryResult;
    {
        QDebug deb = qDebug();
        deb << "SqliteAdapter::sqliteSelect "<<data["query"];
        queryResult = sqliteSelectSync(data);
        deb << " completed";
    }
    QVariantList res = queryResult.value("query_result").toList();
    for (int i = 0; i < res.size(); ++i) {
        QVariantMap rowData;
        rowData["query"] = data;
        rowData["row"] = res.at(i).toMap();
        rowData["row_id"] = i;
        rowData["row_cnt"] = res.size();
        rowData["query_id"] = queryId;
        emit row(rowData);
        if (i > 0 && !(i % 1000)) {
            qInfo() << "emitted row "<<i<< "/"<<res.size();
        }
    }
}

void SqliteAdapter::sqliteSelectMultiProgressive(const QVariantMap data)
{
    QVariant queryId = data.value("query_id");
    QVariantMap queryResult = sqliteSelectMultiSync(data);
//    QVariantList res = queryResult.value(data.value("query").toMap().value("query").toMap().value("key").toString()).toList();
    QVariantList res = queryResult.value("query_result").toList();
    for (int i = 0; i < res.size(); ++i) {
        QVariantMap rowData;
        rowData["query"] = data;
        rowData["row"] = res.at(i).toMap();
        rowData["row_id"] = i;
        rowData["row_cnt"] = res.size();
        rowData["query_id"] = queryId;
        emit row(rowData);
    }
    // if no value, emit the error?
}

void SqliteAdapter::sqliteInsert(const QVariantMap data)
{
    QVariantMap res = SQLiteManager::instance().sqliteInsert(data);
    res["query"] = data;
    emit queryResult(res);
}

void SqliteAdapter::sqliteDelete(const QVariantMap data)
{
    QVariantMap res = SQLiteManager::instance().sqliteUpdate(data);
    res["query"] = data;
    emit queryResult(res);
}

void SqliteAdapter::sqliteUpdate(const QVariantMap data)
{
    QVariantMap res = SQLiteManager::instance().sqliteUpdate(data);
    res["query"] = data;
    emit queryResult(res);
}

QVariantMap SqliteAdapter::sqliteSelectSync(const QVariantMap data)
{
    QVariantMap res = SQLiteManager::instance().sqliteSelect(data);
    res["query"] = data;
    return res;
}

/*
    EXAMPLE OUTPUT (Note that in the input db TestSlave rows were present only for PK "2")

{
    "error": "",
    "query": {
        "query": {
            "args": {
                "testArg": "to show where it ends up"
            },
            "key": "query_result",
            "nested": [
                {
                    "args": {
                        ":pk": ":pk",
                        "Foo": "Bar",
                        "bar": 123
                    },
                    "key": "TestSlave",
                    "nested": [
                        {
                            "args": {
                                ":desc": ":desc"
                            },
                            "key": "TestSlaveSlave",
                            "query": "SELECT * FROM testSlaveSlave WHERE desc = :desc"
                        }
                    ],
                    "query": "SELECT * FROM testSlave WHERE pk = :pk"
                }
            ],
            "query": "SELECT pk, text_field, date_field FROM test"
        },
        "query_id": 322
    },
    "query_id": "322",
    "query_result": [
        {
            "TestSlave": [
            ],
            "date_field": "2020-09-26T16:43:35.242",
            "pk": 1,
            "text_field": "foo"
        },
        {
            "TestSlave": [
                {
                    "TestSlaveSlave": [
                        {
                            "desc": "barSlave",
                            "descSlave": "A"
                        },
                        {
                            "desc": "barSlave",
                            "descSlave": "B"
                        },
                        {
                            "desc": "barSlave",
                            "descSlave": "C"
                        }
                    ],
                    "desc": "barSlave",
                    "pk": 2
                },
                {
                    "TestSlaveSlave": [
                    ],
                    "desc": "bazSlave",
                    "pk": 2
                },
                {
                    "TestSlaveSlave": [
                    ],
                    "desc": "fooSlave",
                    "pk": 2
                }
            ],
            "date_field": "2020-09-26T16:43:35.245",
            "pk": 2,
            "text_field": "bar"
        },
        {
            "TestSlave": [
            ],
            "date_field": "2020-09-26T16:43:35.247",
            "pk": 3,
            "text_field": "baz"
        }
    ]
}

*/
QVariantMap SqliteAdapter::sqliteSelectMultiSync(const QVariantMap data)
{
    QVariantMap res;
    QSharedPointer<NestedQuery> query;

    if (data.contains("query") && data.value("query").canConvert<QVariantMap>()) {
        query = NestedQuery::toNested(data.value("query").toMap());
    }
    if (!query || !query->valid)
        return res;

    res = SQLiteManager::instance().sqliteSelectMulti(query, data.value("query_id").toString());
    res["query"] = data;
    return res;
}

QVariantMap SqliteAdapter::toQuery(const QVariant &query_id, const QString &query, const QVariantMap &queryArgs)
{
    QVariantMap res;
    res["query_id"] = query_id;
    res["query"] = query;
    res["args"] = queryArgs;
    return res;
}

/*
    SQLiteManager
*/

void SQLiteManager::initDb_real() {
    LOCK();
    bool res = false;
    if (m_db.isValid() || SQLiteManager::m_dbPath.isEmpty())
        return;

    const QFileInfo fi(SQLiteManager::m_dbPath);
    const QString path = fi.absolutePath();
    const QString fname = fi.fileName();

    if (!QDir::root().mkpath(path))
        qDebug() << "SQLiteManager::initDb(): QDir::root().mkpath " << path << " Failed";
    QString databaseFilename = QDir(path).filePath(fname);

    if (fileExists(databaseFilename)) {
        res = true;
    } else {
        if (m_creation.isEmpty()) {
            qDebug() << "SQLiteManager::initDb(): " << SQLiteManager::m_dbPath << " does not exist and the CREATE statement is empty";
            return;
        }
        qDebug() << "SQLiteManager::initDb(): creating and initializing" << m_dbPath;
    }

    // Create the database if not present and open
    QString connectionName = randomString(9);
    m_db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    m_db.setDatabaseName(databaseFilename);
    if (!m_db.open()) {
        qDebug () << "Failed to open "<<m_db.databaseName() << " : " << m_db.lastError();
        qFatal("Impossible to create the SQLITE database for the cache");
        return;
    }

    if (res)
        return;

    // init db
    qDebug() << "DB was empty: initializing.";
    QSqlQuery query(m_db);
    query.setForwardOnly(true);
    for (const auto &q: qAsConst(m_creation)) {
        res = query.exec(q);
        if (query.lastError().type() != QSqlError::NoError) {
            qDebug () << "Failed to CREATE "<<m_db.databaseName() << " : " << query.lastError();
            return;
        }
    }
}

struct QueryRecord
{
    QVariantMap record;
    QSharedPointer<QueryRecord> parent;
};

namespace  {
QRegularExpression notColon("[^:]");
}

QVariantList SQLiteManager::sqliteSelect(const QSharedPointer<NestedQuery> data, const QSharedPointer<QueryRecord> parent)
{
    QSqlQuery query(m_db);
    query.prepare(data->query.query);
    for (const QString &k: data->query.args.keys())
    {
        QVariant arg = data->query.args.value(k);
        if (arg.type() != QVariant::String) {
            query.bindValue(k, arg);
        } else {
            const QString sArg = arg.toString();
            const int indent = sArg.indexOf(notColon);
            if (indent > 0) { // disregard < 0 case
                const QString column = sArg.mid(indent);
                QSharedPointer<QueryRecord> source = parent;
                // find leading :, walk back to get the value
                for (int i = 1; i < indent; ++i) {
                    source = source->parent;
                }
                const QVariant val = source->record.value(column);
                query.bindValue(k, val);
            } else {
                query.bindValue(k, arg);
            }
        }
    }

    if (!query.exec()) {
        qDebug() << "query " << data->query.query << "failed!" << query.lastError() <<  __FILE__ << __LINE__;
        return {};
    }

    QVariantList res;
    while (query.next()) {
        QVariantMap row;
        const QSqlRecord record = query.record();
        for  (int i = 0; i < record.count(); ++i)
            row[record.field(i).name()] = record.value(i);

        QSharedPointer<QueryRecord> queryRecord(new QueryRecord);
        queryRecord->record = row;
        queryRecord->parent = parent;

        for (const auto &nestedQuery: data->nested) {
            QVariantList nestedQueryOutput = sqliteSelect(nestedQuery, queryRecord);
            row[nestedQuery->query.key] = nestedQueryOutput;
        }
        res << row;
    }
    return res;
}

QVariantMap SQLiteManager::sqliteSelectMulti(const QSharedPointer<NestedQuery> data, const QVariant queryId)
{
    QVariantMap res;
    if (!data)
        return res;
    LOCK()
    return {{"query_result", sqliteSelect(data, QSharedPointer<QueryRecord>())},
            {"error" , "" }, {"query_id", queryId}};

}

QVariantMap SQLiteManager::sqliteSelect(const QVariantMap data) {
    const QDateTime now = QDateTime::currentDateTime();
//    qDebug() << "Payload at " << now << " : \n"<<data;

    QString queryString = data["query"].toString();
    const QVariantMap args = data.value("args").toMap();

    LOCK()
    QSqlQuery query(m_db);
    query.prepare(queryString);
    for (const QString &k: args.keys()) {
        //qDebug() << "binding "<<k << " to "<<args.value(k);
        query.bindValue(k, args.value(k));
    }

    if (!query.exec()) {
        if (!query.lastError().text().startsWith("UNIQUE constraint failed"))
            qDebug() << "query " << queryString << "failed!" << query.lastError() <<  __FILE__ << __LINE__;
        return {{"error" , query.lastError().text() }, {"query_id", data.value("query_id")}};
    }

    QVariantList res;
    while (query.next()) {
        QVariantMap row;
        const QSqlRecord record = query.record();
        for  (int i = 0; i < record.count(); ++i)
            row[record.field(i).name()] = record.value(i);
        res << row;
    }

    ++counter;
    return {{"query_result", res}, {"error" , query.lastError().text() }, {"query_id", data.value("query_id")}};
}

QVariantMap SQLiteManager::sqliteInsert(const QVariantMap data)
{
    QString queryString = data["query"].toString();
    QVariantMap args = data.value("args").toMap();

    LOCK()
    QSqlQuery query(m_db);
    query.prepare(queryString);
    for (const auto &k: args.keys()) {
        query.bindValue(k, args.value(k));
    }

    if (!query.exec()) {
        if (!query.lastError().text().startsWith("UNIQUE constraint failed"))
            qDebug() << "query " << queryString << "failed!" << query.lastError() <<  __FILE__ << __LINE__;
        return {{"error" , query.lastError().text() }, {"query_id", data.value("query_id")}};
    }

    return {{"error" , query.lastError().text() }, {"query_id", data.value("query_id")}};
}

QVariantMap SQLiteManager::sqliteDelete(const QVariantMap data)
{
    return sqliteInsert(data);
}

QVariantMap SQLiteManager::sqliteUpdate(const QVariantMap data)
{
    return sqliteInsert(data);
}

void SQLiteManager::initDb(const QString &path, const QStringList &creation)
{
    setDatabase(path);
    setCreationStatement(creation);
    SQLiteManager::instance().initDb_real();
}

void SQLiteManager::setDatabase(const QString &path)
{
    instance().m_dbPath = path;
}

void SQLiteManager::setCreationStatement(const QStringList &creationStatement)
{
    instance().m_creation = creationStatement;
}

SQLiteManager& SQLiteManager::instance()
{
    static SQLiteManager   instance_; // Guaranteed to be destroyed.
    // Instantiated on first use.
    return instance_;
}

SQLiteManager::SQLiteManager()
{
}

/*
    DbClient
*/

DbClient::DbClient(QUrl serverUrl, QObject *parent)
    : QObject(parent)
{
    node.reset(new QRemoteObjectNode);
    QObject::connect(node.data(), &QRemoteObjectNode::error,
                     this, &DbClient::onNodeError);
    auto res = node->connectToNode(serverUrl);
    if (!res) {
        const auto err = QStringLiteral("DbClient: failed connecting to") + serverUrl.toString();
        qFatal(err.toStdString().c_str());
    }
    sqliteReplica.reset(node->acquireDynamic(serverObjectName)); // acquire replica of source from host node
    if (!sqliteReplica) {
        const auto err = QStringLiteral("DbClient: failed acquiring ") + serverObjectName;
        qFatal(err.toStdString().c_str());
    }

    //connect signal for replica valid changed with signal slot initialization
    QObject::connect(sqliteReplica.data(), &QRemoteObjectDynamicReplica::initialized,
                     this, &DbClient::onInitialized);
    QObject::connect(sqliteReplica.data(), &QRemoteObjectDynamicReplica::stateChanged,
                     this, &DbClient::onStateChanged);

}

DbClient::~DbClient() {}

bool DbClient::isInitialized() const
{
    return m_initialized;
}

void DbClient::submitSelectProgressive(const Query &query)
{
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteSelectProgressive", Qt::QueuedConnection, Q_ARG(QVariantMap, query.toMap()));
}

void DbClient::submitSelect(const Query &query)
{
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteSelect", Qt::QueuedConnection, Q_ARG(QVariantMap, query.toMap()));
}

void DbClient::submitSelectMulti(const QString &query_id, const QSharedPointer<NestedQuery> &query)
{
    QVariantMap params;
    params["query"] = query->toMap();
    params["query_id"] = query_id;
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteSelectMulti", Qt::QueuedConnection, Q_ARG(QVariantMap, params));
}

void DbClient::submitSelectMultiProgressive(const QString &query_id, const QSharedPointer<NestedQuery> &query)
{
    QVariantMap params;
    params["query"] = query->toMap();
    params["query_id"] = query_id;
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteSelectMultiProgressive", Qt::QueuedConnection, Q_ARG(QVariantMap, params));
}

void DbClient::submitInsert(const Query &query)
{
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteInsert", Qt::QueuedConnection, Q_ARG(QVariantMap, query.toMap()));
}

void DbClient::submitDelete(const Query &query)
{
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteDelete", Qt::QueuedConnection, Q_ARG(QVariantMap, query.toMap()));
}

void DbClient::submitUpdate(const Query &query)
{
    QMetaObject::invokeMethod(sqliteReplica.get(), "sqliteUpdate", Qt::QueuedConnection, Q_ARG(QVariantMap, query.toMap()));
}

QVariantMap DbClient::select(const Query &query)
{
    return syncSelect(query.toMap(), QLatin1String("sqliteSelectSync"));
}

QVariantMap DbClient::selectMulti(const QVariant &query_id, const QSharedPointer<NestedQuery> &query)
{
    QVariantMap params;
    params["query"] = query->toMap();
    params["query_id"] = query_id;
    return syncSelect(params, QLatin1String("sqliteSelectMultiSync"));
}

QVariantMap DbClient::syncSelect(const QVariantMap &query, const QLatin1String &selectType)
{
    //        qDebug() << "m_sqliteInitialized"<<m_sqliteInitialized;
    //        QObject::connect(sqliteReplica.data(), SIGNAL(queryResult(const QVariantMap)), this, SLOT(foo()));
    //        QObject::connect(sqliteReplica.data(), SIGNAL(queryResult(const QVariantMap)), &loop, SLOT(quit()));
    //        QObject::connect(sqliteReplica.data(), SIGNAL(queryResult(const QVariantMap)), this, SLOT(onQueryResult(const QVariantMap)));
    //        QObject::connect(sqliteReplica.data(), SIGNAL(queryResult(const QVariantMap)), catcher, SLOT(onQueryResult(const QVariantMap)));
    //        QObject::connect(sqliteReplica.data(), SIGNAL(queryResult(const QVariantMap)), catcher, SLOT(onPayload(const QVariantMap)));
    //        connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
    //        qDebug() << "PAST CONNECTS!" << selectType.data();
    //        QMetaObject::invokeMethod(sqliteReplica.get(), selectType.data(), Qt::QueuedConnection, Q_ARG(QVariantMap, params));
    QVariantMap res;
    QRemoteObjectPendingCall call;
    QRemoteObjectDynamicReplica *replica = sqliteReplica.get();
    QMetaObject::invokeMethod(replica,
                              selectType.data(),
                              Qt::DirectConnection,
                              //                                  Q_RETURN_ARG(QVariantMap, res),
                              Q_RETURN_ARG(QRemoteObjectPendingCall, call),
                              Q_ARG(const QVariantMap, query));

    auto e = call.error(); // , QRemoteObjectPendingCall::InvalidMessage);
    call.waitForFinished();
    res = call.returnValue().toMap();
    return res;
}

void DbClient::onQueryResult(const QVariantMap data)
{ // Slot to receive source state
    //        qDebug() << "onQueryResult payload:\n";
    //        print(data);

    //        qDebug() << " ==== end ====";

    emit queryFinished(data);
}

void DbClient::onRowReceived(const QVariantMap data)
{
//    qDebug() << "onRowReceived";
//    print(data);
    emit rowReceived(data);
}

void DbClient::onInitialized()
{
    QObject::connect(sqliteReplica.data(), SIGNAL(queryResult(const QVariantMap)), this, SLOT(onQueryResult(const QVariantMap)));
    QObject::connect(sqliteReplica.data(), SIGNAL(row(const QVariantMap)), this, SLOT(onRowReceived(const QVariantMap)));
    m_initialized = true;
    emit initializedChanged();
}

void DbClient::onStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState)
{
    qDebug() << "DbClient::onStateChanged" << state << " - " << oldState;
}

void DbClient::onNodeError(QRemoteObjectNode::ErrorCode errorCode)
{
    qDebug() << "DbClient::onNodeError" << errorCode;
}
