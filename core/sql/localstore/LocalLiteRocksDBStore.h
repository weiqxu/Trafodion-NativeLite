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
};

struct LocalLiteRow
{
  uint64_t rowId;
  std::string value;
};

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

  bool allocateRowId(const LocalLiteTableDef &table,
                     uint64_t *rowId,
                     std::string *error);
  bool putRow(const LocalLiteTableDef &table,
              uint64_t rowId,
              const std::string &encodedRow,
              std::string *error);
  bool scanRows(const LocalLiteTableDef &table,
                std::vector<LocalLiteRow> *rows,
                std::string *error);

private:
  LocalLiteRocksDBStore(const LocalLiteRocksDBStore &);
  LocalLiteRocksDBStore &operator=(const LocalLiteRocksDBStore &);

  void *catalogDb_;
};

#endif

#endif
