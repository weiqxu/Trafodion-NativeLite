// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_ROCKSDB_STORE_H
#define LOCAL_LITE_ROCKSDB_STORE_H

#ifdef TRAF_LOCAL_LITE

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

struct LocalLiteColumnDef
{
  LocalLiteColumnDef()
    : nullable(true), upshifted(false), defaultClass(0), added(false),
      division(false) {}

  std::string name;
  std::string type;
  bool nullable;
  bool upshifted;
  int defaultClass;
  std::string defaultValue;
  bool added;
  bool division;
  std::string computedText;
};

struct LocalLiteIndexDef
{
  LocalLiteIndexDef()
    : objectUid(0), unique(false), keyEncodingVersion(1) {}

  std::string name;
  uint64_t objectUid;
  bool unique;
  uint32_t keyEncodingVersion;
  std::vector<size_t> keyColumns;
  std::vector<bool> descending;
};

struct LocalLiteCheckDef
{
  std::string name;
  std::string expression;
};

struct LocalLiteRIDef
{
  std::string name;
  std::vector<size_t> referencingColumns;
  std::string referencedCatalog;
  std::string referencedSchema;
  std::string referencedTable;
  std::vector<size_t> referencedColumns;
  std::string referencedConstraint;
};

struct LocalLiteObjectRef
{
  std::string catalog;
  std::string schema;
  std::string name;
};

struct LocalLiteTableDef
{
  LocalLiteTableDef()
    : objectUid(0), nextRowId(1), view(false),
      viewUpdatable(false), viewInsertable(false), noSyskey(false) {}

  std::string catalog;
  std::string schema;
  std::string name;
  uint64_t objectUid;
  uint64_t nextRowId;
  std::vector<LocalLiteColumnDef> columns;
  std::vector<size_t> primaryKeyColumns;
  // STORE BY columns describe physical clustering, not a SQL primary key.
  std::vector<size_t> storeByColumns;
  std::string primaryKeyName;
  std::vector< std::vector<size_t> > uniqueKeyColumns;
  std::vector<std::string> uniqueKeyNames;
  std::vector<LocalLiteIndexDef> secondaryIndexes;
  std::vector<LocalLiteCheckDef> checkConstraints;
  std::vector<LocalLiteRIDef> riConstraints;
  bool view;
  std::string viewText;
  // The binder needs the check-option level and the view mutability flags
  // when it expands a local-lite view.
  std::string viewCheckText;
  bool viewUpdatable;
  bool viewInsertable;
  bool noSyskey;
  std::vector<LocalLiteObjectRef> dependencies;
};

struct LocalLiteRow
{
  uint64_t rowId;
  std::string value;
};

struct LocalLiteRowMutation
{
  LocalLiteRow before;
  std::string after;
};

struct LocalLiteColumnStatsDef
{
  LocalLiteColumnStatsDef()
    : rowCount(0), nullCount(0), distinctCount(0) {}

  std::string columnName;
  uint64_t rowCount;
  uint64_t nullCount;
  uint64_t distinctCount;
};

struct LocalLiteTableStatsDef
{
  LocalLiteTableStatsDef() : rowCount(0), analyzedAt(0) {}

  uint64_t rowCount;
  uint64_t analyzedAt;
  std::vector<LocalLiteColumnStatsDef> columns;
};

struct LocalLiteSequenceDef
{
  LocalLiteSequenceDef()
    : objectUid(0), fsDataType(0), startValue(1), increment(1),
      minValue(1), maxValue(9223372036854775806LL), nextValue(1),
      cycle(false), cache(25), numCalls(0), internal(false) {}

  std::string catalog;
  std::string schema;
  std::string name;
  uint64_t objectUid;
  int fsDataType;
  int64_t startValue;
  int64_t increment;
  int64_t minValue;
  int64_t maxValue;
  int64_t nextValue;
  bool cycle;
  int64_t cache;
  uint64_t numCalls;
  bool internal;
};

struct LocalLiteTriggerDef
{
  LocalLiteTriggerDef()
    : operation(0), activation(0), granularity(0), timestamp(0),
      allUpdateColumns(true) {}

  std::string catalog;
  std::string schema;
  std::string name;
  std::string subjectCatalog;
  std::string subjectSchema;
  std::string subjectTable;
  int operation;
  int activation;
  int granularity;
  uint64_t timestamp;
  bool allUpdateColumns;
  std::vector<size_t> updateColumns;
  std::string sqlText;
};

// Authorization metadata is intentionally kept in the same RocksDB catalog
// as table definitions.  Local-lite has no external security service, so the
// catalog is the source of truth for identities, role membership, ownership,
// and object privileges.
struct LocalLiteAuthIdentity
{
  LocalLiteAuthIdentity() : id(0), role(false) {}
  std::string name;
  uint64_t id;
  bool role;
};

enum LocalLitePrivilege
{
  LOCAL_LITE_PRIV_SELECT    = 1 << 0,
  LOCAL_LITE_PRIV_INSERT    = 1 << 1,
  LOCAL_LITE_PRIV_UPDATE    = 1 << 2,
  LOCAL_LITE_PRIV_DELETE    = 1 << 3,
  LOCAL_LITE_PRIV_REFERENCES = 1 << 4,
  LOCAL_LITE_PRIV_USAGE     = 1 << 5,
  LOCAL_LITE_PRIV_ALL       = 0x3f
};

class LocalLiteTxnState;

class LocalLiteRocksDBStore
{
public:
  LocalLiteRocksDBStore();
  ~LocalLiteRocksDBStore();

  static std::string defaultRoot();
  static std::string catalogPath();
  static std::string dataRoot();
  static std::string tablePath(const LocalLiteTableDef &table);

  bool open(std::string *error);
  void close();

  bool createTable(const LocalLiteTableDef &table, std::string *error);
  bool createSchema(const std::string &catalog,
                    const std::string &schema,
                    bool ifNotExists,
                    std::string *error);
  bool dropSchema(const std::string &catalog,
                  const std::string &schema,
                  bool ifExists,
                  bool cascade,
                  std::string *error);
  bool schemaExists(const std::string &catalog,
                    const std::string &schema,
                    bool *exists,
                    std::string *error);
  bool createSynonym(const std::string &catalog,
                     const std::string &schema,
                     const std::string &name,
                     const std::string &targetCatalog,
                     const std::string &targetSchema,
                     const std::string &targetName,
                     std::string *error);
  bool dropSynonym(const std::string &catalog,
                   const std::string &schema,
                   const std::string &name,
                   bool ifExists,
                   std::string *error);
  bool resolveSynonym(const std::string &catalog,
                      const std::string &schema,
                      const std::string &name,
                      std::string *targetCatalog,
                      std::string *targetSchema,
                      std::string *targetName,
                      bool *found,
                      std::string *error);
  bool createSequence(const LocalLiteSequenceDef &sequence,
                      std::string *error);
  bool alterSequence(const LocalLiteSequenceDef &sequence,
                     std::string *error);
  bool dropSequence(const std::string &catalog,
                    const std::string &schema,
                    const std::string &name,
                    bool ifExists,
                    std::string *error);
  bool loadSequence(const std::string &catalog,
                    const std::string &schema,
                    const std::string &name,
                    LocalLiteSequenceDef *sequence,
                    bool *found,
                    std::string *error);
  bool listSequences(const std::string &catalog,
                     const std::string &schema,
                     std::vector<LocalLiteSequenceDef> *sequences,
                     std::string *error);
  bool allocateSequence(uint64_t objectUid,
                        int64_t requestedCount,
                        int64_t *nextValue,
                        int64_t *endValue,
                        std::string *error);
  bool createTrigger(const LocalLiteTriggerDef &trigger, std::string *error);
  bool dropTrigger(const std::string &catalog,
                   const std::string &schema,
                   const std::string &name,
                   bool ifExists,
                   std::string *error);

  bool loadAuthIdentity(const std::string &name,
                        LocalLiteAuthIdentity *identity,
                        bool *found,
                        std::string *error);
  bool createAuthIdentity(const std::string &name,
                          bool role,
                          bool ifNotExists,
                          std::string *error);
  bool dropAuthIdentity(const std::string &name,
                        bool role,
                        bool ifExists,
                        std::string *error);
  bool grantRole(const std::string &role,
                 const std::string &grantee,
                 bool adminOption,
                 std::string *error);
  bool revokeRole(const std::string &role,
                  const std::string &grantee,
                  std::string *error);
  bool grantPrivilege(const std::string &catalog,
                      const std::string &schema,
                      const std::string &object,
                      uint32_t privilegeMask,
                      const std::string &grantee,
                      bool grantOption,
                      std::string *error);
  bool revokePrivilege(const std::string &catalog,
                       const std::string &schema,
                       const std::string &object,
                       uint32_t privilegeMask,
                       const std::string &grantee,
                       std::string *error);
  bool hasPrivilege(const std::string &catalog,
                    const std::string &schema,
                    const std::string &object,
                    const std::string &user,
                    uint32_t privilegeMask,
                    std::string *error);
  bool setTableOwner(const std::string &catalog,
                     const std::string &schema,
                     const std::string &object,
                     const std::string &owner,
                     std::string *error);
  bool isTableOwner(const std::string &catalog,
                    const std::string &schema,
                    const std::string &object,
                    const std::string &user,
                    bool *owner,
                    std::string *error);
  bool bumpAuthorizationGeneration(std::string *error);
  // Generic catalog records used by the RocksDB-only local-lite UDR runtime.
  // Keys are namespaced by the caller (for example, "udr|"), so the UDR
  // layer can evolve its metadata without exposing the RocksDB handle.
  bool loadCatalogRecord(const std::string &key,
                         std::string *value,
                         bool *found,
                         std::string *error);
  bool storeCatalogRecord(const std::string &key,
                          const std::string &value,
                          std::string *error);
  bool deleteCatalogRecord(const std::string &key,
                          std::string *error);
  bool scanCatalogRecords(const std::string &prefix,
                          std::vector< std::pair<std::string, std::string> > *records,
                          std::string *error);
  // Logical metadata rows persisted in the local catalog.  The
  // returned key/value pairs are ordered by RocksDB key and use the stable
  // "md|<table>|..." key space; callers can expose them as _MD_ scans
  // without depending on HBase.
  bool scanMetadataRows(
      const std::string &metadataTable,
      std::vector< std::pair<std::string, std::string> > *records,
      std::string *error);
  bool listTriggers(const std::string &subjectCatalog,
                    const std::string &subjectSchema,
                    const std::string &subjectTable,
                    int operation,
                    std::vector<LocalLiteTriggerDef> *triggers,
                    std::string *error);
  bool dropTable(const std::string &catalog,
                 const std::string &schema,
                 const std::string &name,
                 std::string *error);
  bool tableExists(const std::string &catalog,
                   const std::string &schema,
                   const std::string &name,
                   bool *exists,
                   std::string *error);
  bool loadTable(const std::string &catalog,
                 const std::string &schema,
                 const std::string &name,
                 LocalLiteTableDef *table,
                 std::string *error);
  bool listTables(const std::string &catalog,
                  const std::string &schema,
                  std::vector<LocalLiteTableDef> *tables,
                  std::string *error);
  bool listSchemas(const std::string &catalog,
                   std::vector<std::string> *schemas,
                   std::string *error);
  bool listCatalogs(std::vector<std::string> *catalogs,
                    std::string *error);
  bool createIndex(const LocalLiteTableDef &table,
                   const LocalLiteIndexDef &index,
                   std::string *error);
  bool dropIndex(const std::string &catalog,
                 const std::string &schema,
                 const std::string &name,
                 bool ifExists,
                 std::string *error);
  bool alterTable(const LocalLiteTableDef &oldTable,
                  const LocalLiteTableDef &newTable,
                  const std::vector<int> &newToOldColumn,
                  const std::vector<std::string> &addedValues,
                  std::string *error);
  bool validateReferentialIntegrity(const LocalLiteTableDef &table,
                                    std::string *error);
  bool collectTableStats(const LocalLiteTableDef &table,
                         LocalLiteTableStatsDef *stats,
                         std::string *error);
  bool loadTableStats(const std::string &catalog,
                      const std::string &schema,
                      const std::string &name,
                      LocalLiteTableStatsDef *stats,
                      bool *found,
                      std::string *error);
  bool invalidateTableStats(const LocalLiteTableDef &table,
                            std::string *error);

  bool insertRow(const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool updateRows(const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRowMutation> &mutations,
                  std::string *error);
  bool deleteRows(const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRow> &rows,
                  std::string *error);
  bool getRowByKey(const LocalLiteTableDef &table,
                   const std::string &storageKey,
                   LocalLiteRow *row,
                   bool *found,
                   std::string *error);
  bool scanRows(const LocalLiteTableDef &table,
                std::vector<LocalLiteRow> *rows,
                std::string *error);
  bool scanIndexPrefix(const LocalLiteTableDef &table,
                       const std::string &physicalPrefix,
                       std::vector<LocalLiteRow> *rows,
                       std::string *error);
  bool scanIndexRange(const LocalLiteTableDef &table,
                      const std::string &startKey,
                      const std::string &endKey,
                      std::vector<LocalLiteRow> *rows,
                      std::string *error);

private:
  friend class LocalLiteTxnState;

  LocalLiteRocksDBStore(const LocalLiteRocksDBStore &);
  LocalLiteRocksDBStore &operator=(const LocalLiteRocksDBStore &);

  bool getRowByKey(const LocalLiteTableDef &table,
                   const std::string &storageKey,
                   const void *statementOwner,
                   uint64_t statementExecutionId,
                   LocalLiteRow *row,
                   bool *found,
                   std::string *error);
  bool scanRows(const LocalLiteTableDef &table,
                const void *statementOwner,
                uint64_t statementExecutionId,
                std::vector<LocalLiteRow> *rows,
                std::string *error);

  bool opened_;
};

class LocalLiteTxn
{
public:
  explicit LocalLiteTxn(LocalLiteRocksDBStore *store,
                        const void *statementOwner = 0,
                        uint64_t statementExecutionId = 0);

  bool insertRow(const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool upsertRow(const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool updateRows(const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRowMutation> &mutations,
                  std::string *error);
  bool deleteRows(const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRow> &rows,
                  std::string *error);
  bool scanRows(const LocalLiteTableDef &table,
                std::vector<LocalLiteRow> *rows,
                std::string *error);
  bool getRowByKey(const LocalLiteTableDef &table,
                   const std::string &storageKey,
                   LocalLiteRow *row,
                   bool *found,
                   std::string *error);

private:
  LocalLiteTxn(const LocalLiteTxn &);
  LocalLiteTxn &operator=(const LocalLiteTxn &);

  LocalLiteRocksDBStore *store_;
  const void *statementOwner_;
  uint64_t statementExecutionId_;
};

class LocalLiteTxnManager
{
public:
  static const int64_t INVALID_EXECUTOR_TXN_ID = -1;

  static bool begin(std::string *error);
  static bool beginForExecutor(int64_t executorTxnId, std::string *error);
  static bool commit(std::string *error);
  static bool commitForExecutor(int64_t executorTxnId, std::string *error);
  static bool rollback(std::string *error);
  static bool rollbackForExecutor(int64_t executorTxnId, std::string *error);
  // Flush the current DML image before a catalog/row-layout-changing DDL,
  // then keep the executor transaction context usable for following DML.
  static bool prepareDDLForExecutor(int64_t executorTxnId,
                                    std::string *error);
  static bool active();
  static uint64_t currentLocalTxnId();
  static int64_t currentExecutorTxnId();
  static void beginStatement(const void *statementOwner,
                             uint64_t statementExecutionId);
  static void endStatement(const void *statementOwner,
                           uint64_t statementExecutionId);
};

#endif

#endif
