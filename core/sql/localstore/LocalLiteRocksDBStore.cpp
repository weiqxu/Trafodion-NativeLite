// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteRocksDBStore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <map>
#include <pthread.h>

#include <rocksdb/c.h>

static const unsigned char LOCAL_LITE_ROW_FORMAT_VERSION = 1;

static void setError(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
}

static bool checkRocksError(char *err, const std::string &prefix, std::string *error)
{
  if (!err)
    return true;
  setError(error, prefix + ": " + err);
  rocksdb_free(err);
  return false;
}

static bool mkdirOne(const std::string &path, std::string *error)
{
  struct stat st;
  if (stat(path.c_str(), &st) == 0)
    {
      if (S_ISDIR(st.st_mode))
        return true;
      setError(error, path + " exists but is not a directory");
      return false;
    }

  if (mkdir(path.c_str(), 0755) == 0)
    return true;

  if (errno == EEXIST)
    return true;

  setError(error, "mkdir " + path + ": " + strerror(errno));
  return false;
}

static bool mkdirs(const std::string &path, std::string *error)
{
  if (path.empty())
    return true;

  std::string curr;
  size_t i = 0;
  if (path[0] == '/')
    {
      curr = "/";
      i = 1;
    }

  while (i <= path.size())
    {
      size_t slash = path.find('/', i);
      std::string part = path.substr(i, slash == std::string::npos ? slash : slash - i);
      if (!part.empty())
        {
          if (curr.size() > 1)
            curr += "/";
          curr += part;
          if (!mkdirOne(curr, error))
            return false;
        }
      if (slash == std::string::npos)
        break;
      i = slash + 1;
    }

  return true;
}

static std::string parentDir(const std::string &path)
{
  size_t pos = path.rfind('/');
  if (pos == std::string::npos)
    return ".";
  if (pos == 0)
    return "/";
  return path.substr(0, pos);
}

static std::string escapeName(const std::string &s)
{
  std::string out;
  char buf[4];
  for (size_t i = 0; i < s.size(); i++)
    {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if ((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.')
        out += static_cast<char>(c);
      else
        {
          snprintf(buf, sizeof(buf), "%%%02X", c);
          out += buf;
        }
    }
  return out.empty() ? "_" : out;
}

static std::string tableKey(const std::string &catalog,
                            const std::string &schema,
                            const std::string &name)
{
  return "table|" + catalog + "|" + schema + "|" + name;
}

static std::string uidKey(uint64_t uid)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "uid|%020llu",
           static_cast<unsigned long long>(uid));
  return buf;
}

static void appendUint64(std::string &s, uint64_t v)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    s += static_cast<char>((v >> shift) & 0xff);
}

static uint64_t readUint64(const std::string &s, size_t *offset)
{
  uint64_t v = 0;
  for (int i = 0; i < 8 && *offset < s.size(); i++)
    {
      v = (v << 8) | static_cast<unsigned char>(s[*offset]);
      (*offset)++;
    }
  return v;
}

static std::string encodeTable(const LocalLiteTableDef &table)
{
  std::string out;
  out += "LLT1\n";
  out += table.catalog + "\n";
  out += table.schema + "\n";
  out += table.name + "\n";
  char buf[64];
  snprintf(buf, sizeof(buf), "%llu\n",
           static_cast<unsigned long long>(table.objectUid));
  out += buf;
  snprintf(buf, sizeof(buf), "%llu\n",
           static_cast<unsigned long long>(table.nextRowId));
  out += buf;
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.columns.size()));
  out += buf;
  for (size_t i = 0; i < table.columns.size(); i++)
    {
      out += table.columns[i].name;
      out += "\t";
      out += table.columns[i].type;
      out += "\t";
      out += table.columns[i].nullable ? "1" : "0";
      out += "\n";
    }
  return out;
}

static bool nextLine(const std::string &s, size_t *pos, std::string *line)
{
  if (*pos > s.size())
    return false;
  size_t end = s.find('\n', *pos);
  if (end == std::string::npos)
    return false;
  *line = s.substr(*pos, end - *pos);
  *pos = end + 1;
  return true;
}

static bool decodeTable(const std::string &encoded,
                        LocalLiteTableDef *table,
                        std::string *error)
{
  std::string line;
  size_t pos = 0;
  if (!nextLine(encoded, &pos, &line) || line != "LLT1")
    {
      setError(error, "invalid local-lite table metadata");
      return false;
    }
  if (!nextLine(encoded, &pos, &table->catalog) ||
      !nextLine(encoded, &pos, &table->schema) ||
      !nextLine(encoded, &pos, &table->name) ||
      !nextLine(encoded, &pos, &line))
    {
      setError(error, "truncated local-lite table metadata");
      return false;
    }
  table->objectUid = strtoull(line.c_str(), NULL, 10);
  if (!nextLine(encoded, &pos, &line))
    return false;
  table->nextRowId = strtoull(line.c_str(), NULL, 10);
  if (!nextLine(encoded, &pos, &line))
    return false;
  unsigned long count = strtoul(line.c_str(), NULL, 10);
  table->columns.clear();
  for (unsigned long i = 0; i < count; i++)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite column metadata");
          return false;
        }
      size_t p1 = line.find('\t');
      size_t p2 = (p1 == std::string::npos) ? p1 : line.find('\t', p1 + 1);
      if (p1 == std::string::npos || p2 == std::string::npos)
        {
          setError(error, "invalid local-lite column metadata");
          return false;
        }
      LocalLiteColumnDef col;
      col.name = line.substr(0, p1);
      col.type = line.substr(p1 + 1, p2 - p1 - 1);
      col.nullable = (line.substr(p2 + 1) == "1");
      table->columns.push_back(col);
    }
  return true;
}

static std::string encodeRowValue(const std::string &encodedRow)
{
  std::string out;
  out += static_cast<char>(LOCAL_LITE_ROW_FORMAT_VERSION);
  appendUint64(out, static_cast<uint64_t>(encodedRow.size()));
  out += encodedRow;
  return out;
}

static bool decodeRowValue(const std::string &value,
                           std::string *encodedRow,
                           std::string *error)
{
  if (value.size() < 9 ||
      static_cast<unsigned char>(value[0]) != LOCAL_LITE_ROW_FORMAT_VERSION)
    {
      setError(error, "invalid local-lite row format");
      return false;
    }
  size_t offset = 1;
  uint64_t len = readUint64(value, &offset);
  if (offset + len > value.size())
    {
      setError(error, "truncated local-lite row value");
      return false;
    }
  *encodedRow = value.substr(offset, static_cast<size_t>(len));
  return true;
}

class LocalLiteMutexGuard
{
public:
  explicit LocalLiteMutexGuard(pthread_mutex_t *mutex)
    : mutex_(mutex)
  {
    pthread_mutex_lock(mutex_);
  }

  ~LocalLiteMutexGuard()
  {
    pthread_mutex_unlock(mutex_);
  }

private:
  LocalLiteMutexGuard(const LocalLiteMutexGuard &);
  LocalLiteMutexGuard &operator=(const LocalLiteMutexGuard &);

  pthread_mutex_t *mutex_;
};

class LocalLiteStorageManager
{
public:
  static LocalLiteStorageManager &instance()
  {
    static LocalLiteStorageManager manager;
    return manager;
  }

  bool acquire(std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);

    if (refCount_ == 0)
      {
        if (!mkdirs(parentDir(LocalLiteRocksDBStore::catalogPath()), error))
          return false;

        rocksdb_options_t *options = rocksdb_options_create();
        rocksdb_options_set_create_if_missing(options, 1);

        char *err = NULL;
        rocksdb_t *db = rocksdb_open(options,
                                     LocalLiteRocksDBStore::catalogPath().c_str(),
                                     &err);
        rocksdb_options_destroy(options);
        if (!checkRocksError(err,
                             "open RocksDB catalog " +
                             LocalLiteRocksDBStore::catalogPath(),
                             error))
          return false;

        catalogDb_ = db;
      }

    refCount_++;
    return true;
  }

  void release()
  {
    LocalLiteMutexGuard guard(&mutex_);

    if (refCount_ == 0)
      return;

    refCount_--;
    if (refCount_ != 0)
      return;

    for (TableMap::iterator it = tableDbs_.begin(); it != tableDbs_.end(); ++it)
      rocksdb_close(it->second);
    tableDbs_.clear();

    if (catalogDb_)
      rocksdb_close(catalogDb_);
    catalogDb_ = NULL;
  }

  rocksdb_t *catalogDb()
  {
    return catalogDb_;
  }

  rocksdb_t *openTable(const std::string &path,
                       bool createIfMissing,
                       std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);

    TableMap::iterator it = tableDbs_.find(path);
    if (it != tableDbs_.end())
      return it->second;

    return openTableLocked(path, createIfMissing, error);
  }

  bool insertRow(const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);

    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;

    const uint64_t allocatedRowId = loaded.nextRowId;
    LocalLiteTableDef updated = loaded;
    updated.nextRowId++;

    std::string tableMetadataKey =
      tableKey(loaded.catalog, loaded.schema, loaded.name);
    std::string oldMetadata = encodeTable(loaded);
    std::string newMetadata = encodeTable(updated);
    if (!putCatalogLocked(tableMetadataKey, newMetadata,
                          "update local-lite row id metadata", error))
      return false;

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      {
        putCatalogLocked(tableMetadataKey, oldMetadata,
                         "rollback local-lite row id metadata", NULL);
        return false;
      }

    std::string key;
    appendUint64(key, allocatedRowId);
    std::string value = encodeRowValue(encodedRow);
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_put(db, writeOptions,
                key.data(), key.size(), value.data(), value.size(), &err);
    rocksdb_writeoptions_destroy(writeOptions);
    if (!checkRocksError(err, "write local-lite row", error))
      {
        std::string rollbackError;
        if (!putCatalogLocked(tableMetadataKey, oldMetadata,
                              "rollback local-lite row id metadata",
                              &rollbackError))
          {
            if (error)
              *error += "; " + rollbackError;
          }
        return false;
      }

    if (rowId)
      *rowId = allocatedRowId;
    return true;
  }

  void closeTable(const std::string &path)
  {
    LocalLiteMutexGuard guard(&mutex_);

    TableMap::iterator it = tableDbs_.find(path);
    if (it == tableDbs_.end())
      return;

    rocksdb_close(it->second);
    tableDbs_.erase(it);
  }

  pthread_mutex_t *mutex()
  {
    return &mutex_;
  }

private:
  typedef std::map<std::string, rocksdb_t *> TableMap;

  LocalLiteStorageManager()
    : refCount_(0),
      catalogDb_(NULL)
  {
    pthread_mutex_init(&mutex_, NULL);
  }

  LocalLiteStorageManager(const LocalLiteStorageManager &);
  LocalLiteStorageManager &operator=(const LocalLiteStorageManager &);

  rocksdb_t *openTableLocked(const std::string &path,
                             bool createIfMissing,
                             std::string *error)
  {
    TableMap::iterator it = tableDbs_.find(path);
    if (it != tableDbs_.end())
      return it->second;

    rocksdb_options_t *options = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(options, createIfMissing ? 1 : 0);
    char *err = NULL;
    rocksdb_t *db = rocksdb_open(options, path.c_str(), &err);
    rocksdb_options_destroy(options);
    if (!checkRocksError(err, "open RocksDB table " + path, error))
      return NULL;

    tableDbs_[path] = db;
    return db;
  }

  bool loadTableLocked(const std::string &catalog,
                       const std::string &schema,
                       const std::string &name,
                       LocalLiteTableDef *table,
                       std::string *error)
  {
    std::string key = tableKey(catalog, schema, name);
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    char *err = NULL;
    size_t valueLen = 0;
    char *value = rocksdb_get(catalogDb_, readOptions,
                              key.data(), key.size(), &valueLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, "read local-lite table metadata", error))
      return false;
    if (!value)
      {
        setError(error, "local-lite table does not exist: " +
                catalog + "." + schema + "." + name);
        return false;
      }

    std::string encoded(value, valueLen);
    rocksdb_free(value);
    return decodeTable(encoded, table, error);
  }

  bool putCatalogLocked(const std::string &key,
                        const std::string &value,
                        const std::string &errorPrefix,
                        std::string *error)
  {
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_put(catalogDb_, writeOptions,
                key.data(), key.size(), value.data(), value.size(), &err);
    rocksdb_writeoptions_destroy(writeOptions);
    return checkRocksError(err, errorPrefix, error);
  }

  unsigned int refCount_;
  rocksdb_t *catalogDb_;
  TableMap tableDbs_;
  pthread_mutex_t mutex_;
};

LocalLiteRocksDBStore::LocalLiteRocksDBStore()
  : opened_(false)
{
}

LocalLiteRocksDBStore::~LocalLiteRocksDBStore()
{
  close();
}

std::string LocalLiteRocksDBStore::defaultRoot()
{
  const char *overrideDir = getenv("TRAF_LOCAL_STORE_DIR");
  if (overrideDir && overrideDir[0])
    return overrideDir;

  const char *trafVar = getenv("TRAF_VAR");
  if (trafVar && trafVar[0])
    return std::string(trafVar) + "/localstore/rocksdb";

  return "./localstore/rocksdb";
}

std::string LocalLiteRocksDBStore::catalogPath()
{
  return defaultRoot() + "/catalog";
}

std::string LocalLiteRocksDBStore::dataRoot()
{
  return defaultRoot() + "/data";
}

std::string LocalLiteRocksDBStore::tablePath(const LocalLiteTableDef &table)
{
  char uid[32];
  snprintf(uid, sizeof(uid), "%020llu",
           static_cast<unsigned long long>(table.objectUid));
  return dataRoot() + "/" + escapeName(table.catalog) + "/" +
         escapeName(table.schema) + "/" + uid;
}

bool LocalLiteRocksDBStore::open(std::string *error)
{
  if (opened_)
    return true;

  if (!LocalLiteStorageManager::instance().acquire(error))
    return false;

  opened_ = true;
  return true;
}

void LocalLiteRocksDBStore::close()
{
  if (!opened_)
    return;

  LocalLiteStorageManager::instance().release();
  opened_ = false;
}

bool LocalLiteRocksDBStore::createTable(const LocalLiteTableDef &table,
                                        std::string *error)
{
  if (!open(error))
    return false;

  bool exists = false;
  if (!tableExists(table.catalog, table.schema, table.name, &exists, error))
    return false;
  if (exists)
    {
      setError(error, "local-lite table already exists: " +
              table.catalog + "." + table.schema + "." + table.name);
      return false;
    }

  if (!mkdirs(parentDir(tablePath(table)), error))
    return false;

  LocalLiteStorageManager::instance().closeTable(tablePath(table));
  rocksdb_options_t *tableOptions = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(tableOptions, 1);
  char *err = NULL;
  rocksdb_t *tableDb = rocksdb_open(tableOptions, tablePath(table).c_str(), &err);
  rocksdb_options_destroy(tableOptions);
  if (!checkRocksError(err, "create RocksDB table " + tablePath(table), error))
    return false;
  rocksdb_close(tableDb);

  LocalLiteTableDef copy = table;
  if (copy.nextRowId == 0)
    copy.nextRowId = 1;

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string encoded = encodeTable(copy);
  std::string key = tableKey(copy.catalog, copy.schema, copy.name);
  std::string uid = uidKey(copy.objectUid);
  rocksdb_writebatch_put(batch, key.data(), key.size(), encoded.data(), encoded.size());
  rocksdb_writebatch_put(batch, uid.data(), uid.size(), key.data(), key.size());
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "write local-lite table metadata", error))
    return false;
  return true;
}

bool LocalLiteRocksDBStore::dropTable(const std::string &catalog,
                                      const std::string &schema,
                                      const std::string &name,
                                      std::string *error)
{
  LocalLiteTableDef table;
  if (!loadTable(catalog, schema, name, &table, error))
    return false;

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string key = tableKey(catalog, schema, name);
  std::string uid = uidKey(table.objectUid);
  rocksdb_writebatch_delete(batch, key.data(), key.size());
  rocksdb_writebatch_delete(batch, uid.data(), uid.size());
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "delete local-lite table metadata", error))
    return false;

  LocalLiteStorageManager::instance().closeTable(tablePath(table));
  rocksdb_options_t *options = rocksdb_options_create();
  err = NULL;
  rocksdb_destroy_db(options, tablePath(table).c_str(), &err);
  rocksdb_options_destroy(options);
  if (!checkRocksError(err, "destroy RocksDB table " + tablePath(table), error))
    return false;
  return true;
}

bool LocalLiteRocksDBStore::tableExists(const std::string &catalog,
                                        const std::string &schema,
                                        const std::string &name,
                                        bool *exists,
                                        std::string *error)
{
  if (!open(error))
    return false;

  std::string key = tableKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions,
                            key.data(), key.size(), &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite table metadata", error))
    return false;
  if (value)
    {
      rocksdb_free(value);
      *exists = true;
      return true;
    }
  *exists = false;
  return true;
}

bool LocalLiteRocksDBStore::loadTable(const std::string &catalog,
                                      const std::string &schema,
                                      const std::string &name,
                                      LocalLiteTableDef *table,
                                      std::string *error)
{
  if (!open(error))
    return false;

  std::string key = tableKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions,
                            key.data(), key.size(), &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite table metadata", error))
    return false;
  if (!value)
    {
      setError(error, "local-lite table does not exist: " +
              catalog + "." + schema + "." + name);
      return false;
    }
  std::string encoded(value, valueLen);
  rocksdb_free(value);
  return decodeTable(encoded, table, error);
}

bool LocalLiteRocksDBStore::allocateRowId(const LocalLiteTableDef &table,
                                          uint64_t *rowId,
                                          std::string *error)
{
  if (!open(error))
    return false;

  LocalLiteMutexGuard guard(LocalLiteStorageManager::instance().mutex());

  LocalLiteTableDef loaded;
  if (!loadTable(table.catalog, table.schema, table.name, &loaded, error))
    return false;

  *rowId = loaded.nextRowId++;
  std::string key = tableKey(loaded.catalog, loaded.schema, loaded.name);
  std::string value = encodeTable(loaded);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(),
              writeOptions,
              key.data(), key.size(), value.data(), value.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  if (!checkRocksError(err, "update local-lite row id metadata", error))
    return false;
  return true;
}

bool LocalLiteRocksDBStore::insertRow(const LocalLiteTableDef &table,
                                      const std::string &encodedRow,
                                      uint64_t *rowId,
                                      std::string *error)
{
  if (!open(error))
    return false;

  return LocalLiteStorageManager::instance().insertRow(table, encodedRow,
                                                       rowId, error);
}

bool LocalLiteRocksDBStore::putRow(const LocalLiteTableDef &table,
                                   uint64_t rowId,
                                   const std::string &encodedRow,
                                   std::string *error)
{
  if (!open(error))
    return false;

  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(tablePath(table),
                                                               false,
                                                               error);
  if (!db)
    return false;

  std::string key;
  appendUint64(key, rowId);
  std::string value = encodeRowValue(encodedRow);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(db, writeOptions, key.data(), key.size(), value.data(), value.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  if (!checkRocksError(err, "write local-lite row", error))
    return false;
  return true;
}

bool LocalLiteRocksDBStore::scanRows(const LocalLiteTableDef &table,
                                     std::vector<LocalLiteRow> *rows,
                                     std::string *error)
{
  rows->clear();

  if (!open(error))
    return false;

  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(tablePath(table),
                                                               false,
                                                               error);
  if (!db)
    return false;

  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  const rocksdb_snapshot_t *snapshot = rocksdb_create_snapshot(db);
  rocksdb_readoptions_set_snapshot(readOptions, snapshot);
  rocksdb_iterator_t *it = rocksdb_create_iterator(db, readOptions);
  for (rocksdb_iter_seek_to_first(it); rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      size_t valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      std::string key(rawKey, keyLen);
      std::string value(rawValue, valueLen);
      size_t offset = 0;
      LocalLiteRow row;
      row.rowId = readUint64(key, &offset);
      if (!decodeRowValue(value, &row.value, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          rocksdb_release_snapshot(db, snapshot);
          return false;
        }
      rows->push_back(row);
    }

  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  rocksdb_release_snapshot(db, snapshot);
  if (!checkRocksError(err, "scan local-lite rows", error))
    return false;
  return true;
}

LocalLiteTxn::LocalLiteTxn(LocalLiteRocksDBStore *store)
  : store_(store)
{
}

bool LocalLiteTxn::insertRow(const LocalLiteTableDef &table,
                             const std::string &encodedRow,
                             uint64_t *rowId,
                             std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }

  return store_->insertRow(table, encodedRow, rowId, error);
}

bool LocalLiteTxn::scanRows(const LocalLiteTableDef &table,
                            std::vector<LocalLiteRow> *rows,
                            std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }

  return store_->scanRows(table, rows, error);
}

#endif
