#pragma once

#include <Access/AccessRightsElement.h>
#include <Interpreters/IInterpreter.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/ConstraintsDescription.h>
#include <Storages/IStorage_fwd.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Common/ThreadPool.h>


namespace DB
{

class ASTCreateQuery;
class ASTExpressionList;
class ASTConstraintDeclaration;
class IDatabase;
using DatabasePtr = std::shared_ptr<IDatabase>;


/** Allows to create new table or database,
  *  or create an object for existing table or database.
  */
class InterpreterCreateQuery : public IInterpreter, WithContext
{
public:
    InterpreterCreateQuery(const ASTPtr & query_ptr_, ContextPtr context_);

    BlockIO execute() override;

    /// List of columns and their types in AST.
    static ASTPtr formatColumns(const NamesAndTypesList & columns);
    static ASTPtr formatColumns(const ColumnsDescription & columns);

    static ASTPtr formatIndices(const IndicesDescription & indices);
    static ASTPtr formatConstraints(const ConstraintsDescription & constraints);
    static ASTPtr formatProjections(const ProjectionsDescription & projections);

    void setForceRestoreData(bool has_force_restore_data_flag_)
    {
        has_force_restore_data_flag = has_force_restore_data_flag_;
    }

    void setInternal(bool internal_)
    {
        internal = internal_;
    }

    void setForceAttach(bool force_attach_)
    {
        force_attach = force_attach_;
    }

    /// Obtain information about columns, their types, default values and column comments,
    ///  for case when columns in CREATE query is specified explicitly.
    static ColumnsDescription getColumnsDescription(const ASTExpressionList & columns, ContextPtr context, bool attach);
    static ConstraintsDescription getConstraintsDescription(const ASTExpressionList * constraints);

    static void prepareOnClusterQuery(ASTCreateQuery & create, ContextPtr context, const String & cluster_name);

    void extendQueryLogElemImpl(QueryLogElement & elem, const ASTPtr & ast, ContextPtr) const override;

private:
    struct TableProperties
    {
        ColumnsDescription columns;
        IndicesDescription indices;
        ConstraintsDescription constraints;
        ProjectionsDescription projections;
    };

    BlockIO createDatabase(ASTCreateQuery & create);
    BlockIO createTable(ASTCreateQuery & create);

    /// Daisy : start
    bool createTableDistributed(const String & database, ASTCreateQuery & create);
    bool createDatabaseDistributed(ASTCreateQuery & create);
    /// Daisy : end

    /// Calculate list of columns, constraints, indices, etc... of table. Rewrite query in canonical way.
    TableProperties setProperties(ASTCreateQuery & create) const;
    void validateTableStructure(const ASTCreateQuery & create, const TableProperties & properties) const;
    void setEngine(ASTCreateQuery & create) const;
    AccessRightsElements getRequiredAccess() const;

    /// Create IStorage and add it to database. If table already exists and IF NOT EXISTS specified, do nothing and return false.
    bool doCreateTable(ASTCreateQuery & create, const TableProperties & properties);
    BlockIO doCreateOrReplaceTable(ASTCreateQuery & create, const InterpreterCreateQuery::TableProperties & properties);
    /// Inserts data in created table if it's CREATE ... SELECT
    BlockIO fillTableIfNeeded(const ASTCreateQuery & create);

    void assertOrSetUUID(ASTCreateQuery & create, const DatabasePtr & database) const;

    ASTPtr query_ptr;

    /// Skip safety threshold when loading tables.
    bool has_force_restore_data_flag = false;
    /// Is this an internal query - not from the user.
    bool internal = false;
    bool force_attach = false;

    mutable String as_database_saved;
    mutable String as_table_saved;
};
}
