#include <Databases/DatabasesCommon.h>
#include <Interpreters/InterpreterCreateQuery.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ParserCreateQuery.h>
#include <Parsers/formatAST.h>
#include <Parsers/queryToString.h>
#include <Storages/StorageDictionary.h>
#include <Storages/StorageFactory.h>
#include <Common/typeid_cast.h>
#include <Common/escapeForFileName.h>
#include <TableFunctions/TableFunctionFactory.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int TABLE_ALREADY_EXISTS;
    extern const int UNKNOWN_TABLE;
    extern const int UNKNOWN_DATABASE;
}

DatabaseWithOwnTablesBase::DatabaseWithOwnTablesBase(const String & name_, const String & logger, ContextPtr context_)
        : IDatabase(name_), WithContext(context_->getGlobalContext()), log(&Poco::Logger::get(logger))
{
}

bool DatabaseWithOwnTablesBase::isTableExist(const String & table_name, ContextPtr) const
{
    std::lock_guard lock(mutex);
    return tables.find(table_name) != tables.end();
}

StoragePtr DatabaseWithOwnTablesBase::tryGetTable(const String & table_name, ContextPtr) const
{
    std::lock_guard lock(mutex);
    auto it = tables.find(table_name);
    if (it != tables.end())
        return it->second;
    return {};
}

DatabaseTablesIteratorPtr DatabaseWithOwnTablesBase::getTablesIterator(ContextPtr, const FilterByNameFunction & filter_by_table_name)
{
    std::lock_guard lock(mutex);
    if (!filter_by_table_name)
        return std::make_unique<DatabaseTablesSnapshotIterator>(tables, database_name);

    Tables filtered_tables;
    for (const auto & [table_name, storage] : tables)
        if (filter_by_table_name(table_name))
            filtered_tables.emplace(table_name, storage);

    return std::make_unique<DatabaseTablesSnapshotIterator>(std::move(filtered_tables), database_name);
}

bool DatabaseWithOwnTablesBase::empty() const
{
    std::lock_guard lock(mutex);
    return tables.empty();
}

StoragePtr DatabaseWithOwnTablesBase::detachTable(const String & table_name)
{
    std::unique_lock lock(mutex);
    return detachTableUnlocked(table_name, lock);
}

StoragePtr DatabaseWithOwnTablesBase::detachTableUnlocked(const String & table_name, std::unique_lock<std::mutex> &)
{
    StoragePtr res;

    auto it = tables.find(table_name);
    if (it == tables.end())
        throw Exception(ErrorCodes::UNKNOWN_TABLE, "Table {}.{} doesn't exist",
                        backQuote(database_name), backQuote(table_name));
    res = it->second;
    tables.erase(it);

    auto table_id = res->getStorageID();
    if (table_id.hasUUID())
    {
        assert(database_name == DatabaseCatalog::TEMPORARY_DATABASE || getUUID() != UUIDHelpers::Nil);
        DatabaseCatalog::instance().removeUUIDMapping(table_id.uuid);
    }

    return res;
}

void DatabaseWithOwnTablesBase::attachTable(const String & table_name, const StoragePtr & table, const String &)
{
    std::unique_lock lock(mutex);
    attachTableUnlocked(table_name, table, lock);
}

void DatabaseWithOwnTablesBase::attachTableUnlocked(const String & table_name, const StoragePtr & table, std::unique_lock<std::mutex> &)
{
    auto table_id = table->getStorageID();
    if (table_id.database_name != database_name)
        throw Exception(ErrorCodes::UNKNOWN_DATABASE, "Database was renamed to `{}`, cannot create table in `{}`",
                        database_name, table_id.database_name);

    if (table_id.hasUUID())
    {
        assert(database_name == DatabaseCatalog::TEMPORARY_DATABASE || getUUID() != UUIDHelpers::Nil);
        DatabaseCatalog::instance().addUUIDMapping(table_id.uuid, shared_from_this(), table);
    }

    if (!tables.emplace(table_name, table).second)
    {
        if (table_id.hasUUID())
            DatabaseCatalog::instance().removeUUIDMapping(table_id.uuid);
        throw Exception(ErrorCodes::TABLE_ALREADY_EXISTS, "Table {} already exists.", table_id.getFullTableName());
    }
}

void DatabaseWithOwnTablesBase::shutdown()
{
    /// You can not hold a lock during shutdown.
    /// Because inside `shutdown` function tables can work with database, and mutex is not recursive.

    Tables tables_snapshot;
    {
        std::lock_guard lock(mutex);
        tables_snapshot = tables;
    }

    for (const auto & kv : tables_snapshot)
    {
        kv.second->flush();
    }

    for (const auto & kv : tables_snapshot)
    {
        auto table_id = kv.second->getStorageID();
        kv.second->flushAndShutdown();
        if (table_id.hasUUID())
        {
            assert(getDatabaseName() == DatabaseCatalog::TEMPORARY_DATABASE || getUUID() != UUIDHelpers::Nil);
            DatabaseCatalog::instance().removeUUIDMapping(table_id.uuid);
        }
    }

    std::lock_guard lock(mutex);
    tables.clear();
}

DatabaseWithOwnTablesBase::~DatabaseWithOwnTablesBase()
{
    try
    {
        DatabaseWithOwnTablesBase::shutdown();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

StoragePtr DatabaseWithOwnTablesBase::getTableUnlocked(const String & table_name, std::unique_lock<std::mutex> &) const
{
    auto it = tables.find(table_name);
    if (it != tables.end())
        return it->second;
    throw Exception(ErrorCodes::UNKNOWN_TABLE, "Table {}.{} doesn't exist",
                    backQuote(database_name), backQuote(table_name));
}

/// Daisy: starts.
StorageInMemoryCreateQueryPtr parseCreateQueryFromAST(const ASTPtr & query, const String & database_, const String & table_)
{
    return parseCreateQueryFromAST(query.get(), database_, table_);
}

StorageInMemoryCreateQueryPtr parseCreateQueryFromAST(const IAST * query, const String & database_, const String & table_)
{
    /// if AST is nullptr, return empty create query string.
    if (!query)
        return std::make_shared<StorageInMemoryCreateQuery>("", "", "");

    ASTPtr query_clone = query->clone();
    auto * create = query_clone->as<ASTCreateQuery>();

    if (!create)
    {
        WriteBufferFromOwnString query_buf;
        formatAST(*query, query_buf, true);
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Query '{}' is not CREATE query", query_buf.str());
    }

    create->attach = false;
    create->database = database_;
    create->table = table_;

    /// We remove everything that is not needed for ATTACH from the query.
    assert(!create->temporary);
    create->as_database.clear();
    create->as_table.clear();
    create->if_not_exists = false;
    create->is_populate = false;
    create->replace_view = false;
    create->replace_table = false;
    create->create_or_replace = false;

    /// For views it is necessary to save the SELECT query itself, for the rest - on the contrary
    if (!create->isView())
        create->select = nullptr;

    create->format = nullptr;
    create->out_file = nullptr;

    /// parse 'query_uuid'
    String query_uuid_str = queryToString(*create);

    /// parse 'query'
    create->uuid = UUIDHelpers::Nil;
    create->to_inner_uuid = UUIDHelpers::Nil;
    String query_str = queryToString(*create);

    /// parse 'engine_full'
    String engine_full_str;
    if (create->storage)
    {
        engine_full_str = queryToString(*(create->storage));
        const char * const extra_head = " ENGINE = ";
        if (startsWith(engine_full_str, extra_head))
            engine_full_str = engine_full_str.substr(strlen(extra_head));
    }
    return std::make_shared<StorageInMemoryCreateQuery>(query_str, query_uuid_str, engine_full_str);
}
/// Daisy: starts.
}
