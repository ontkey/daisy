#pragma once

#include <Databases/IDatabase.h>
#include <Interpreters/IInterpreter.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/IAST_fwd.h>


namespace DB
{
class Context;
using DatabaseAndTable = std::pair<DatabasePtr, StoragePtr>;
class AccessRightsElements;

/** Allow to either drop table with all its data (DROP),
  * or remove information about table (just forget) from server (DETACH),
  * or just clear all data in table (TRUNCATE).
  */
class InterpreterDropQuery : public IInterpreter, WithContext
{
public:
    InterpreterDropQuery(const ASTPtr & query_ptr_, ContextPtr context_);

    /// Drop table or database.
    BlockIO execute() override;

    void extendQueryLogElemImpl(QueryLogElement & elem, const ASTPtr &, ContextPtr) const override;

private:
    AccessRightsElements getRequiredAccessForDDLOnCluster() const;
    ASTPtr query_ptr;

    BlockIO executeToDatabase(const ASTDropQuery & query);
    BlockIO executeToDatabaseImpl(const ASTDropQuery & query, DatabasePtr & database, std::vector<UUID> & uuids_to_wait);

    BlockIO executeToTable(ASTDropQuery & query);
    BlockIO executeToTableImpl(ASTDropQuery & query, DatabasePtr & db, UUID & uuid_to_wait);

    static void waitForTableToBeActuallyDroppedOrDetached(const ASTDropQuery & query, const DatabasePtr & db, const UUID & uuid_to_wait);

    BlockIO executeToDictionary(const String & database_name, const String & dictionary_name, ASTDropQuery::Kind kind, bool if_exists, bool is_temporary, bool no_ddl_lock);

    BlockIO executeToTemporaryTable(const String & table_name, ASTDropQuery::Kind kind);

    /// Daisy : start
    bool deleteTableDistributed(const ASTDropQuery & query);
    bool deleteDatabaseDistributed(const ASTDropQuery & query);
    /// Daisy : end
};
}
