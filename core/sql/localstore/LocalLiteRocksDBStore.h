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
#include <vector>

struct LocalLiteColumnDef
{
  LocalLiteColumnDef()
    : nullable(true), defaultClass(0), added(false) {}

  std::string name;
  std::string type;
  bool nullable;
  int defaultClass;
  std::string defaultValue;
  bool added;
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
  LocalLiteTableDef() : objectUid(0), nextRowId(1), view(false) {}

  std::string catalog;
  std::string schema;
  std::string name;
  uint64_t objectUid;
  uint64_t nextRowId;
  std::vector<LocalLiteColumnDef> columns;
  std::vector<size_t> primaryKeyColumns;
  std::string primaryKeyName;
  std::vector< std::vector<size_t> > uniqueKeyColumns;
  std::vector<std::string> uniqueKeyNames;
  std::vector<LocalLiteIndexDef> secondaryIndexes;
  std::vector<LocalLiteCheckDef> checkConstraints;
  std::vector<LocalLiteRIDef> riConstraints;
  bool view;
  std::string viewText;
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
