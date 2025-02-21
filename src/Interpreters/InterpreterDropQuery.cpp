#include <Poco/File.h>

#include <Databases/IDatabase.h>
#include <Interpreters/Context.h>
#include <Interpreters/executeDDLQueryOnCluster.h>
#include <Interpreters/InterpreterDropQuery.h>
#include <Interpreters/ExternalDictionariesLoader.h>
#include <Interpreters/QueryLog.h>
#include <Interpreters/BlockUtils.h>
#include <Access/AccessRightsElement.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/queryToString.h>
#include <Storages/IStorage.h>
#include <Common/escapeForFileName.h>
#include <Common/quoteString.h>
#include <Common/typeid_cast.h>

/// Daisy : start
#include <Databases/DatabaseReplicated.h>
#include <DistributedMetadata/CatalogService.h>
#include <common/ClockUtils.h>
/// Daisy : end

#if !defined(ARCADIA_BUILD)
#    include "config_core.h"
#endif

#if USE_MYSQL
#   include <Databases/MySQL/DatabaseMaterializeMySQL.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int SYNTAX_ERROR;
    extern const int UNKNOWN_TABLE;
    extern const int UNKNOWN_DATABASE;
    extern const int NOT_IMPLEMENTED;
    extern const int INCORRECT_QUERY;
}


static DatabasePtr tryGetDatabase(const String & database_name, bool if_exists)
{
    return if_exists ? DatabaseCatalog::instance().tryGetDatabase(database_name) : DatabaseCatalog::instance().getDatabase(database_name);
}


InterpreterDropQuery::InterpreterDropQuery(const ASTPtr & query_ptr_, ContextPtr context_) : WithContext(context_), query_ptr(query_ptr_)
{
}


BlockIO InterpreterDropQuery::execute()
{
    auto & drop = query_ptr->as<ASTDropQuery &>();
    if (!drop.cluster.empty())
        return executeDDLQueryOnCluster(query_ptr, getContext(), getRequiredAccessForDDLOnCluster());

    if (getContext()->getSettingsRef().database_atomic_wait_for_drop_and_detach_synchronously)
        drop.no_delay = true;

    if (!drop.table.empty())
        return executeToTable(drop);
    else if (!drop.database.empty())
        return executeToDatabase(drop);
    else
        throw Exception("Nothing to drop, both names are empty", ErrorCodes::LOGICAL_ERROR);
}


void InterpreterDropQuery::waitForTableToBeActuallyDroppedOrDetached(const ASTDropQuery & query, const DatabasePtr & db, const UUID & uuid_to_wait)
{
    if (uuid_to_wait == UUIDHelpers::Nil)
        return;

    if (query.kind == ASTDropQuery::Kind::Drop)
        DatabaseCatalog::instance().waitTableFinallyDropped(uuid_to_wait);
    else if (query.kind == ASTDropQuery::Kind::Detach)
        db->waitDetachedTableNotInUse(uuid_to_wait);
}

/// Daisy : start
bool InterpreterDropQuery::deleteTableDistributed(const ASTDropQuery & query)
{
    auto ctx = getContext();
    if (!ctx->isDistributed())
    {
        return false;
    }

    if (!ctx->getQueryParameters().contains("_payload"))
    {
        /// FIXME:
        /// Build json payload here from SQL statement
        /// context.setDistributedDDLOperation(true);
        return false;
    }

    if (ctx->isDistributedDDLOperation())
    {
        const auto & catalog_service = CatalogService::instance(ctx);
        auto tables = catalog_service.findTableByName(query.database, query.table);
        if (tables.empty())
        {
            throw Exception(fmt::format("Table {}.{} does not exist.", query.database, query.table), ErrorCodes::UNKNOWN_TABLE);
        }
        if (tables[0]->engine != "DistributedMergeTree")
        {
            /// FIXME:  We only support `DistributedMergeTree` table engine for now
            return false;
        }

        auto * log = &Poco::Logger::get("InterpreterDropQuery");

        auto query_str = queryToString(query);
        LOG_INFO(log, "Drop DistributedMergeTree query={} query_id={}", query_str, ctx->getCurrentQueryId());

        std::vector<std::pair<String, String>> string_cols
            = {{"payload", ctx->getQueryParameters().at("_payload")},
               {"database", query.database},
               {"table", query.table},
               {"query_id", ctx->getCurrentQueryId()},
               {"user", ctx->getUserName()}};

        std::vector<std::pair<String, Int32>> int32_cols;

        /// Milliseconds since epoch
        std::vector<std::pair<String, UInt64>> uint64_cols = {{"timestamp", MonotonicMilliseconds::now()}};

        Block block = buildBlock(string_cols, int32_cols, uint64_cols);
        /// Schema: (payload, database, table, timestamp, query_id, user)

        appendDDLBlock(std::move(block), ctx, {"table_type"}, DWAL::OpCode::DELETE_TABLE, log);

        LOG_INFO(
            log, "Request of dropping DistributedMergeTree query={} query_id={} has been accepted", query_str, ctx->getCurrentQueryId());

        /// FIXME, project tasks status
        return true;
    }
    return false;
}

bool InterpreterDropQuery::deleteDatabaseDistributed(const ASTDropQuery & query)
{
    auto ctx = getContext();
    if (!ctx->isDistributed())
    {
        return false;
    }

    if (!ctx->getQueryParameters().contains("_payload"))
    {
        /// FIXME:
        /// Build json payload here from SQL statement
        /// context.setDistributedDDLOperation(true);
        return false;
    }

    if (ctx->isDistributedDDLOperation())
    {
        const auto & database = DatabaseCatalog::instance().tryGetDatabase(query.database);
        if (!database)
        {
            throw Exception(fmt::format("Databases {} does not exist.", query.database), ErrorCodes::UNKNOWN_DATABASE);
        }

        auto * log = &Poco::Logger::get("InterpreterDropQuery");

        auto query_str = queryToString(query);
        LOG_INFO(log, "Drop database query={} query_id={}", query_str, ctx->getCurrentQueryId());

        std::vector<std::pair<String, String>> string_cols
            = {{"payload", ctx->getQueryParameters().at("_payload")},
               {"database", query.database},
               {"query_id", ctx->getCurrentQueryId()},
               {"user", ctx->getUserName()}};

        std::vector<std::pair<String, Int32>> int32_cols;

        /// Milliseconds since epoch
        std::vector<std::pair<String, UInt64>> uint64_cols = {{"timestamp", MonotonicMilliseconds::now()}};

        Block block = buildBlock(string_cols, int32_cols, uint64_cols);
        /// Schema: (payload, database, timestamp, query_id, user)

        appendDDLBlock(std::move(block), ctx, {"table_type"}, DWAL::OpCode::DELETE_DATABASE, log);

        LOG_INFO(log, "Request of dropping database query={} query_id={} has been accepted", query_str, ctx->getCurrentQueryId());

        /// FIXME, project tasks status
        return true;
    }
    return false;
}
/// Daisy : end

BlockIO InterpreterDropQuery::executeToTable(ASTDropQuery & query)
{
    DatabasePtr database;

    /// Daisy : start
    if (deleteTableDistributed(query))
    {
        return {};
    }
    /// Daisy : end

    UUID table_to_wait_on = UUIDHelpers::Nil;
    auto res = executeToTableImpl(query, database, table_to_wait_on);
    if (query.no_delay)
        waitForTableToBeActuallyDroppedOrDetached(query, database, table_to_wait_on);
    return res;
}

BlockIO InterpreterDropQuery::executeToTableImpl(ASTDropQuery & query, DatabasePtr & db, UUID & uuid_to_wait)
{
    /// NOTE: it does not contain UUID, we will resolve it with locked DDLGuard
    auto table_id = StorageID(query);
    if (query.temporary || table_id.database_name.empty())
    {
        if (getContext()->tryResolveStorageID(table_id, Context::ResolveExternal))
            return executeToTemporaryTable(table_id.getTableName(), query.kind);
        else
            query.database = table_id.database_name = getContext()->getCurrentDatabase();
    }

    if (query.temporary)
    {
        if (query.if_exists)
            return {};
        throw Exception("Temporary table " + backQuoteIfNeed(table_id.table_name) + " doesn't exist",
                        ErrorCodes::UNKNOWN_TABLE);
    }

    auto ddl_guard = (!query.no_ddl_lock ? DatabaseCatalog::instance().getDDLGuard(table_id.database_name, table_id.table_name) : nullptr);

    /// If table was already dropped by anyone, an exception will be thrown
    auto [database, table] = query.if_exists ? DatabaseCatalog::instance().tryGetDatabaseAndTable(table_id, getContext())
                                             : DatabaseCatalog::instance().getDatabaseAndTable(table_id, getContext());

    if (database && table)
    {
        auto & ast_drop_query = query.as<ASTDropQuery &>();

        if (ast_drop_query.is_view && !table->isView())
            throw Exception(ErrorCodes::INCORRECT_QUERY,
                "Table {} is not a View",
                table_id.getNameForLogs());

        if (ast_drop_query.is_dictionary && !table->isDictionary())
            throw Exception(ErrorCodes::INCORRECT_QUERY,
                "Table {} is not a Dictionary",
                table_id.getNameForLogs());

        /// Now get UUID, so we can wait for table data to be finally dropped
        table_id.uuid = database->tryGetTableUUID(table_id.table_name);

        /// Prevents recursive drop from drop database query. The original query must specify a table.
        bool is_drop_or_detach_database = query_ptr->as<ASTDropQuery>()->table.empty();
        bool is_replicated_ddl_query = typeid_cast<DatabaseReplicated *>(database.get()) &&
                                       getContext()->getClientInfo().query_kind != ClientInfo::QueryKind::SECONDARY_QUERY &&
                                       !is_drop_or_detach_database;

        AccessFlags drop_storage;

        if (table->isView())
            drop_storage = AccessType::DROP_VIEW;
        else if (table->isDictionary())
            drop_storage = AccessType::DROP_DICTIONARY;
        else
            drop_storage = AccessType::DROP_TABLE;

        if (is_replicated_ddl_query)
        {
            if (query.kind == ASTDropQuery::Kind::Detach)
                getContext()->checkAccess(drop_storage, table_id);
            else if (query.kind == ASTDropQuery::Kind::Truncate)
                getContext()->checkAccess(AccessType::TRUNCATE, table_id);
            else if (query.kind == ASTDropQuery::Kind::Drop)
                getContext()->checkAccess(drop_storage, table_id);

            ddl_guard->releaseTableLock();
            table.reset();
            return typeid_cast<DatabaseReplicated *>(database.get())->tryEnqueueReplicatedDDL(query.clone(), getContext());
        }

        if (query.kind == ASTDropQuery::Kind::Detach)
        {
            getContext()->checkAccess(drop_storage, table_id);

            if (table->isDictionary())
            {
                /// If DROP DICTIONARY query is not used, check if Dictionary can be dropped with DROP TABLE query
                if (!query.is_dictionary)
                    table->checkTableCanBeDetached();
            }
            else
                table->checkTableCanBeDetached();

            table->flushAndShutdown();
            TableExclusiveLockHolder table_lock;

            if (database->getUUID() == UUIDHelpers::Nil)
                table_lock = table->lockExclusively(getContext()->getCurrentQueryId(), getContext()->getSettingsRef().lock_acquire_timeout);

            if (query.permanently)
            {
                /// Drop table from memory, don't touch data, metadata file renamed and will be skipped during server restart
                database->detachTablePermanently(getContext(), table_id.table_name);
            }
            else
            {
                /// Drop table from memory, don't touch data and metadata
                database->detachTable(table_id.table_name);
            }
        }
        else if (query.kind == ASTDropQuery::Kind::Truncate)
        {
            if (table->isDictionary())
                throw Exception("Cannot TRUNCATE dictionary", ErrorCodes::SYNTAX_ERROR);

            getContext()->checkAccess(AccessType::TRUNCATE, table_id);

            table->checkTableCanBeDropped();

            auto table_lock = table->lockExclusively(getContext()->getCurrentQueryId(), getContext()->getSettingsRef().lock_acquire_timeout);
            auto metadata_snapshot = table->getInMemoryMetadataPtr();
            /// Drop table data, don't touch metadata
            table->truncate(query_ptr, metadata_snapshot, getContext(), table_lock);
        }
        else if (query.kind == ASTDropQuery::Kind::Drop)
        {
            getContext()->checkAccess(drop_storage, table_id);

            if (table->isDictionary())
            {
                /// If DROP DICTIONARY query is not used, check if Dictionary can be dropped with DROP TABLE query
                if (!query.is_dictionary)
                    table->checkTableCanBeDropped();
            }
            else
                table->checkTableCanBeDropped();

            table->flushAndShutdown();

            TableExclusiveLockHolder table_lock;
            if (database->getUUID() == UUIDHelpers::Nil)
                table_lock = table->lockExclusively(getContext()->getCurrentQueryId(), getContext()->getSettingsRef().lock_acquire_timeout);

            database->dropTable(getContext(), table_id.table_name, query.no_delay);
        }

        db = database;
        uuid_to_wait = table_id.uuid;
    }

    return {};
}

BlockIO InterpreterDropQuery::executeToTemporaryTable(const String & table_name, ASTDropQuery::Kind kind)
{
    if (kind == ASTDropQuery::Kind::Detach)
        throw Exception("Unable to detach temporary table.", ErrorCodes::SYNTAX_ERROR);
    else
    {
        auto context_handle = getContext()->hasSessionContext() ? getContext()->getSessionContext() : getContext();
        auto resolved_id = context_handle->tryResolveStorageID(StorageID("", table_name), Context::ResolveExternal);
        if (resolved_id)
        {
            StoragePtr table = DatabaseCatalog::instance().getTable(resolved_id, getContext());
            if (kind == ASTDropQuery::Kind::Truncate)
            {
                auto table_lock
                    = table->lockExclusively(getContext()->getCurrentQueryId(), getContext()->getSettingsRef().lock_acquire_timeout);
                /// Drop table data, don't touch metadata
                auto metadata_snapshot = table->getInMemoryMetadataPtr();
                table->truncate(query_ptr, metadata_snapshot, getContext(), table_lock);
            }
            else if (kind == ASTDropQuery::Kind::Drop)
            {
                context_handle->removeExternalTable(table_name);
                table->flushAndShutdown();
                auto table_lock = table->lockExclusively(getContext()->getCurrentQueryId(), getContext()->getSettingsRef().lock_acquire_timeout);
                /// Delete table data
                table->drop();
                table->is_dropped = true;
            }
        }
    }

    return {};
}

BlockIO InterpreterDropQuery::executeToDatabase(const ASTDropQuery & query)
{
    /// Daisy : start
    if (deleteDatabaseDistributed(query))
    {
        return {};
    }
    /// Daisy : end

    DatabasePtr database;
    std::vector<UUID> tables_to_wait;
    BlockIO res;
    try
    {
        res = executeToDatabaseImpl(query, database, tables_to_wait);
    }
    catch (...)
    {
        if (query.no_delay)
        {
            for (const auto & table_uuid : tables_to_wait)
                waitForTableToBeActuallyDroppedOrDetached(query, database, table_uuid);
        }
        throw;
    }

    if (query.no_delay)
    {
        for (const auto & table_uuid : tables_to_wait)
            waitForTableToBeActuallyDroppedOrDetached(query, database, table_uuid);
    }
    return res;
}

BlockIO InterpreterDropQuery::executeToDatabaseImpl(const ASTDropQuery & query, DatabasePtr & database, std::vector<UUID> & uuids_to_wait)
{
    const auto & database_name = query.database;
    auto ddl_guard = DatabaseCatalog::instance().getDDLGuard(database_name, "");

    database = tryGetDatabase(database_name, query.if_exists);
    if (database)
    {
        if (query.kind == ASTDropQuery::Kind::Truncate)
        {
            throw Exception("Unable to truncate database", ErrorCodes::SYNTAX_ERROR);
        }
        else if (query.kind == ASTDropQuery::Kind::Detach || query.kind == ASTDropQuery::Kind::Drop)
        {
            bool drop = query.kind == ASTDropQuery::Kind::Drop;
            getContext()->checkAccess(AccessType::DROP_DATABASE, database_name);

            if (query.kind == ASTDropQuery::Kind::Detach && query.permanently)
                throw Exception("DETACH PERMANENTLY is not implemented for databases", ErrorCodes::NOT_IMPLEMENTED);

#if USE_MYSQL
            if (database->getEngineName() == "MaterializeMySQL")
                stopDatabaseSynchronization(database);
#endif
            if (auto * replicated = typeid_cast<DatabaseReplicated *>(database.get()))
                replicated->stopReplication();

            if (database->shouldBeEmptyOnDetach())
            {
                ASTDropQuery query_for_table;
                query_for_table.kind = query.kind;
                query_for_table.if_exists = true;
                query_for_table.database = database_name;
                query_for_table.no_delay = query.no_delay;

                /// Flush should not be done if shouldBeEmptyOnDetach() == false,
                /// since in this case getTablesIterator() may do some additional work,
                /// see DatabaseMaterializeMySQL<>::getTablesIterator()
                for (auto iterator = database->getTablesIterator(getContext()); iterator->isValid(); iterator->next())
                {
                    iterator->table()->flush();
                }

                for (auto iterator = database->getTablesIterator(getContext()); iterator->isValid(); iterator->next())
                {
                    DatabasePtr db;
                    UUID table_to_wait = UUIDHelpers::Nil;
                    query_for_table.table = iterator->name();
                    query_for_table.is_dictionary = iterator->table()->isDictionary();
                    executeToTableImpl(query_for_table, db, table_to_wait);
                    uuids_to_wait.push_back(table_to_wait);
                }
            }

            /// Protects from concurrent CREATE TABLE queries
            auto db_guard = DatabaseCatalog::instance().getExclusiveDDLGuardForDatabase(database_name);

            if (!drop)
                database->assertCanBeDetached(true);

            /// DETACH or DROP database itself
            DatabaseCatalog::instance().detachDatabase(database_name, drop, database->shouldBeEmptyOnDetach());
        }
    }

    return {};
}


AccessRightsElements InterpreterDropQuery::getRequiredAccessForDDLOnCluster() const
{
    AccessRightsElements required_access;
    const auto & drop = query_ptr->as<const ASTDropQuery &>();

    if (drop.table.empty())
    {
        if (drop.kind == ASTDropQuery::Kind::Detach)
            required_access.emplace_back(AccessType::DROP_DATABASE, drop.database);
        else if (drop.kind == ASTDropQuery::Kind::Drop)
            required_access.emplace_back(AccessType::DROP_DATABASE, drop.database);
    }
    else if (drop.is_dictionary)
    {
        if (drop.kind == ASTDropQuery::Kind::Detach)
            required_access.emplace_back(AccessType::DROP_DICTIONARY, drop.database, drop.table);
        else if (drop.kind == ASTDropQuery::Kind::Drop)
            required_access.emplace_back(AccessType::DROP_DICTIONARY, drop.database, drop.table);
    }
    else if (!drop.temporary)
    {
        /// It can be view or table.
        if (drop.kind == ASTDropQuery::Kind::Drop)
            required_access.emplace_back(AccessType::DROP_TABLE | AccessType::DROP_VIEW, drop.database, drop.table);
        else if (drop.kind == ASTDropQuery::Kind::Truncate)
            required_access.emplace_back(AccessType::TRUNCATE, drop.database, drop.table);
        else if (drop.kind == ASTDropQuery::Kind::Detach)
            required_access.emplace_back(AccessType::DROP_TABLE | AccessType::DROP_VIEW, drop.database, drop.table);
    }

    return required_access;
}

void InterpreterDropQuery::extendQueryLogElemImpl(QueryLogElement & elem, const ASTPtr &, ContextPtr) const
{
    elem.query_kind = "Drop";
}

}
