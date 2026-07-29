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
  std::string name;
  std::string type;
  bool nullable;
};

struct LocalLiteTableDef
{
  std::string catalog;
  std::string schema;
  std::string name;
  uint64_t objectUid;
  uint64_t nextRowId;
  std::vector<LocalLiteColumnDef> columns;
  std::vector<size_t> primaryKeyColumns;
  std::vector< std::vector<size_t> > uniqueKeyColumns;
};

struct LocalLiteRow
{
  uint64_t rowId;
  std::string value;
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

  bool insertRow(const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error);
  bool getRowByKey(const LocalLiteTableDef &table,
                   const std::string &storageKey,
                   LocalLiteRow *row,
                   bool *found,
                   std::string *error);
  bool scanRows(const LocalLiteTableDef &table,
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
