// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LITE_ROCKSDB_STORE_H
#define LITE_ROCKSDB_STORE_H

#ifdef TRAF_LITE

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

struct LiteColumnDef
{
  LiteColumnDef()
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

struct LiteIndexDef
{
  LiteIndexDef()
    : objectUid(0), unique(false), keyEncodingVersion(1) {}

  std::string name;
  uint64_t objectUid;
  bool unique;
  uint32_t keyEncodingVersion;
  std::vector<size_t> keyColumns;
  std::vector<bool> descending;
};

struct LiteCheckDef
{
  std::string name;
  std::string expression;
};

struct LiteRIDef
{
  std::string name;
  std::vector<size_t> referencingColumns;
  std::string referencedCatalog;
  std::string referencedSchema;
  std::string referencedTable;
  std::vector<size_t> referencedColumns;
  std::string referencedConstraint;
};

struct LiteObjectRef
{
  std::string catalog;
  std::string schema;
  std::string name;
};

struct LiteTableDef
{
  LiteTableDef()
    : objectUid(0), nextRowId(1), view(false),
      viewUpdatable(false), viewInsertable(false), noSyskey(false) {}

  std::string catalog;
  std::string schema;
  std::string name;
  uint64_t objectUid;
  uint64_t nextRowId;
  std::vector<LiteColumnDef> columns;
  std::vector<size_t> primaryKeyColumns;
  // STORE BY columns describe physical clustering, not a SQL primary key.
  std::vector<size_t> storeByColumns;
  std::string primaryKeyName;
  std::vector< std::vector<size_t> > uniqueKeyColumns;
  std::vector<std::string> uniqueKeyNames;
  std::vector<LiteIndexDef> secondaryIndexes;
  std::vector<LiteCheckDef> checkConstraints;
  std::vector<LiteRIDef> riConstraints;
  bool view;
  std::string viewText;
  // The binder needs the check-option level and the view mutability flags
  // when it expands a lite view.
  std::string viewCheckText;
  bool viewUpdatable;
  bool viewInsertable;
  bool noSyskey;
  std::vector<LiteObjectRef> dependencies;
};

struct LiteRow
{
  uint64_t rowId;
  std::string value;
};

struct LiteRowMutation
{
  LiteRow before;
  std::string after;
};

// A bounded, testable view of the SQL-layer OCC state. The actual keys and
// ranges remain owned by LiteTxnContext; callers use these counters for
// qualification and leak/lifecycle checks only.
struct LiteOccState
{
  LiteOccState()
    : startSequence(0), readRanges(0), writeKeys(0), pointReads(0),
      missingPointReads(0), fullScans(0), indexRangeReads(0) {}

  uint64_t startSequence;
  uint64_t readRanges;
  uint64_t writeKeys;
  uint64_t pointReads;
  uint64_t missingPointReads;
  uint64_t fullScans;
  uint64_t indexRangeReads;
};

struct LiteColumnStatsDef
{
  LiteColumnStatsDef()
    : rowCount(0), nullCount(0), distinctCount(0) {}

  std::string columnName;
  uint64_t rowCount;
  uint64_t nullCount;
  uint64_t distinctCount;
};

struct LiteTableStatsDef
{
  LiteTableStatsDef() : rowCount(0), analyzedAt(0) {}

  uint64_t rowCount;
  uint64_t analyzedAt;
  std::vector<LiteColumnStatsDef> columns;
};

struct LiteSequenceDef
{
  LiteSequenceDef()
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

struct LiteTriggerDef
{
  LiteTriggerDef()
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
// as table definitions.  Lite Storage has no external security service, so the
// catalog is the source of truth for identities, role membership, ownership,
// and object privileges.
struct LiteAuthIdentity
{
  LiteAuthIdentity() : id(0), role(false) {}
  std::string name;
  uint64_t id;
  bool role;
};

enum LitePrivilege
{
  LITE_PRIV_SELECT    = 1 << 0,
  LITE_PRIV_INSERT    = 1 << 1,
  LITE_PRIV_UPDATE    = 1 << 2,
  LITE_PRIV_DELETE    = 1 << 3,
  LITE_PRIV_REFERENCES = 1 << 4,
  LITE_PRIV_USAGE     = 1 << 5,
  LITE_PRIV_ALL       = 0x3f
};

class LiteTxnContext;

class LiteRocksDBStore
{
public:
  LiteRocksDBStore();
  ~LiteRocksDBStore();

  static std::string defaultRoot();
  static std::string catalogPath();
  static std::string dataRoot();
  static std::string tablePath(const LiteTableDef &table);

  bool open(std::string *error);
  void close();

  bool createTable(const LiteTableDef &table, std::string *error,
                   const std::string &owner = std::string(),
                   LiteTxnContext *txnContext = 0);
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
  bool createSequence(const LiteSequenceDef &sequence,
                      std::string *error);
  bool alterSequence(const LiteSequenceDef &sequence,
                     std::string *error);
  bool dropSequence(const std::string &catalog,
                    const std::string &schema,
                    const std::string &name,
                    bool ifExists,
                    std::string *error);
  bool loadSequence(const std::string &catalog,
                    const std::string &schema,
                    const std::string &name,
                    LiteSequenceDef *sequence,
                    bool *found,
                    std::string *error);
  bool listSequences(const std::string &catalog,
                     const std::string &schema,
                     std::vector<LiteSequenceDef> *sequences,
                     std::string *error);
  bool allocateSequence(uint64_t objectUid,
                        int64_t requestedCount,
                        int64_t *nextValue,
                        int64_t *endValue,
                        LiteTxnContext *txnContext,
                        std::string *error);
  bool createTrigger(const LiteTriggerDef &trigger, std::string *error);
  bool dropTrigger(const std::string &catalog,
                   const std::string &schema,
                   const std::string &name,
                   bool ifExists,
                   std::string *error);

  bool loadAuthIdentity(const std::string &name,
                        LiteAuthIdentity *identity,
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
  // Generic catalog records used by the RocksDB-only lite UDR runtime.
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
  // Logical metadata rows persisted in the Lite catalog.  The
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
                    std::vector<LiteTriggerDef> *triggers,
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
                 LiteTableDef *table,
                 std::string *error);
  bool listTables(const std::string &catalog,
                  const std::string &schema,
                  std::vector<LiteTableDef> *tables,
                  std::string *error);
  bool listSchemas(const std::string &catalog,
                   std::vector<std::string> *schemas,
                   std::string *error);
  bool listCatalogs(std::vector<std::string> *catalogs,
                    std::string *error);
  bool createIndex(const LiteTableDef &table,
                   const LiteIndexDef &index,
                   std::string *error);
  bool dropIndex(const std::string &catalog,
                 const std::string &schema,
                 const std::string &name,
                 bool ifExists,
                 std::string *error);
  bool alterTable(const LiteTableDef &oldTable,
                  const LiteTableDef &newTable,
                  const std::vector<int> &newToOldColumn,
                  const std::vector<std::string> &addedValues,
                  LiteTxnContext *txnContext,
                  std::string *error);
  bool validateReferentialIntegrity(const LiteTableDef &table,
                                    std::string *error);
  bool collectTableStats(const LiteTableDef &table,
                         LiteTableStatsDef *stats,
                         std::string *error);
  bool loadTableStats(const std::string &catalog,
                      const std::string &schema,
                      const std::string &name,
                      LiteTableStatsDef *stats,
                      bool *found,
                      std::string *error);
  bool invalidateTableStats(const LiteTableDef &table,
                            std::string *error);

  bool insertRow(const LiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool updateRows(const LiteTableDef &table,
                  const std::vector<LiteRowMutation> &mutations,
                  std::string *error);
  bool deleteRows(const LiteTableDef &table,
                  const std::vector<LiteRow> &rows,
                  std::string *error);
  bool getRowByKey(const LiteTableDef &table,
                   const std::string &storageKey,
                   LiteRow *row,
                   bool *found,
                   std::string *error);
  bool scanRows(const LiteTableDef &table,
                std::vector<LiteRow> *rows,
                std::string *error);
  bool scanIndexPrefix(const LiteTableDef &table,
                       const std::string &physicalPrefix,
                       std::vector<LiteRow> *rows,
                       std::string *error);
  bool scanIndexRange(const LiteTableDef &table,
                      const std::string &startKey,
                      const std::string &endKey,
                      std::vector<LiteRow> *rows,
                      std::string *error);
  bool scanPrimaryRange(const LiteTableDef &table,
                        const std::string &startKey,
                        const std::string &endKey,
                        std::vector<LiteRow> *rows,
                        std::string *error);

private:
  friend class LiteTxnContext;

  LiteRocksDBStore(const LiteRocksDBStore &);
  LiteRocksDBStore &operator=(const LiteRocksDBStore &);

  bool getRowByKey(const LiteTableDef &table,
                   const std::string &storageKey,
                   const void *statementOwner,
                   uint64_t statementExecutionId,
                   LiteRow *row,
                   bool *found,
                   std::string *error);
  bool scanRows(const LiteTableDef &table,
                const void *statementOwner,
                uint64_t statementExecutionId,
                std::vector<LiteRow> *rows,
                std::string *error);
  bool scanIndexRange(const LiteTableDef &table,
                      const std::string &startKey,
                      const std::string &endKey,
                      const void *statementOwner,
                      uint64_t statementExecutionId,
                      std::vector<LiteRow> *rows,
                      std::string *error);
  bool scanPrimaryRange(const LiteTableDef &table,
                        const std::string &startKey,
                        const std::string &endKey,
                        const void *statementOwner,
                        uint64_t statementExecutionId,
                        std::vector<LiteRow> *rows,
                        std::string *error);

  bool opened_;
};

class LiteTxn
{
public:
  explicit LiteTxn(LiteRocksDBStore *store,
                        LiteTxnContext *txnContext,
                        const void *statementOwner = 0,
                        uint64_t statementExecutionId = 0);

  bool insertRow(const LiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool insertRows(const LiteTableDef &table,
                  const std::vector<std::string> &encodedRows,
                  std::string *error);
  bool upsertRow(const LiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool updateRows(const LiteTableDef &table,
                  const std::vector<LiteRowMutation> &mutations,
                  std::string *error);
  bool deleteRows(const LiteTableDef &table,
                  const std::vector<LiteRow> &rows,
                  std::string *error);
  bool scanRows(const LiteTableDef &table,
                std::vector<LiteRow> *rows,
                std::string *error);
  bool scanIndexPrefix(const LiteTableDef &table,
                       const std::string &physicalPrefix,
                       std::vector<LiteRow> *rows,
                       std::string *error);
  bool scanIndexRange(const LiteTableDef &table,
                      const std::string &startKey,
                      const std::string &endKey,
                      std::vector<LiteRow> *rows,
                      std::string *error);
  bool scanPrimaryRange(const LiteTableDef &table,
                        const std::string &startKey,
                        const std::string &endKey,
                        std::vector<LiteRow> *rows,
                        std::string *error);
  bool getRowByKey(const LiteTableDef &table,
                   const std::string &storageKey,
                   LiteRow *row,
                   bool *found,
                   std::string *error);

private:
  LiteTxn(const LiteTxn &);
  LiteTxn &operator=(const LiteTxn &);

  LiteRocksDBStore *store_;
  LiteTxnContext *txnContext_;
  const void *statementOwner_;
  uint64_t statementExecutionId_;
};

class LiteTxnManager
{
public:
  static const int64_t INVALID_EXECUTOR_TXN_ID = -1;

  static LiteTxnContext *createContext();
  static void resetContext(LiteTxnContext *txnContext);
  static void destroyContext(LiteTxnContext *txnContext);

  static bool begin(LiteTxnContext *txnContext, std::string *error);
  static bool beginForExecutor(LiteTxnContext *txnContext,
                               int64_t executorTxnId,
                               std::string *error);
  static bool commit(LiteTxnContext *txnContext, std::string *error);
  static bool commitForExecutor(LiteTxnContext *txnContext,
                                int64_t executorTxnId,
                                std::string *error);
  static bool rollback(LiteTxnContext *txnContext, std::string *error);
  static bool rollbackForExecutor(LiteTxnContext *txnContext,
                                  int64_t executorTxnId,
                                  std::string *error);
  // Flush the current DML image before a catalog/row-layout-changing DDL,
  // then keep the executor transaction context usable for following DML.
  static bool prepareDDLForExecutor(LiteTxnContext *txnContext,
                                    int64_t executorTxnId,
                                    std::string *error);
  static bool refreshDDLSequenceForExecutor(LiteTxnContext *txnContext,
                                            int64_t executorTxnId,
                                            std::string *error);
  static bool active(LiteTxnContext *txnContext);
  static uint64_t currentLocalTxnId(LiteTxnContext *txnContext);
  static int64_t currentExecutorTxnId(LiteTxnContext *txnContext);
  static bool occState(LiteTxnContext *txnContext,
                       LiteOccState *state);
  static void beginStatement(LiteTxnContext *txnContext,
                             const void *statementOwner,
                             uint64_t statementExecutionId);
  static void endStatement(LiteTxnContext *txnContext,
                           const void *statementOwner,
                           uint64_t statementExecutionId);
};

// Creates a consistent checkpoint of the active unified TransactionDB while
// excluding concurrent Lite storage mutations.
bool LiteRocksDBCheckpoint(const std::string &path, std::string *error);

// Process-wide cumulative SQL-layer OCC qualification counters.
std::string LiteOccMetricsJson();
void LiteOccMetricsReset();

#endif

#endif
