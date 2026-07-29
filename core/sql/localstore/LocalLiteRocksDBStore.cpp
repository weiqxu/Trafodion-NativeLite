// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteRocksDBStore.h"
#include "LocalLiteRowCodec.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <functional>
#include <map>
#include <pthread.h>

#include <rocksdb/c.h>

static const unsigned char LOCAL_LITE_ROW_FORMAT_VERSION = 1;

static void setError(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
}

static void traceStatementSnapshot(const char *action,
                                   const std::string &tablePath,
                                   uint64_t statementExecutionId)
{
  const char *trace = getenv("TRAF_LOCAL_LITE_TRACE_SNAPSHOT");
  if (!trace || !trace[0])
    return;

  fprintf(stderr, "LOCAL_LITE_SNAPSHOT_%s execution=%llu table=%s\n",
          action,
          static_cast<unsigned long long>(statementExecutionId),
          tablePath.c_str());
}

static bool containsNoCase(const std::string &s, const char *needle)
{
  size_t needleLen = strlen(needle);
  if (needleLen == 0 || s.size() < needleLen)
    return false;

  for (size_t i = 0; i <= s.size() - needleLen; i++)
    {
      size_t j = 0;
      for (; j < needleLen; j++)
        {
          unsigned char a = static_cast<unsigned char>(s[i + j]);
          unsigned char b = static_cast<unsigned char>(needle[j]);
          if (tolower(a) != tolower(b))
            break;
        }
      if (j == needleLen)
        return true;
    }
  return false;
}

static bool checkRocksError(char *err, const std::string &prefix, std::string *error)
{
  if (!err)
    return true;
  std::string rocksError(err);
  if (containsNoCase(rocksError, "/LOCK") ||
      containsNoCase(rocksError, " lock ") ||
      containsNoCase(rocksError, "lock hold"))
    {
      setError(error,
               prefix + ": local-lite store is already open by another "
               "process; use one sqlci process per TRAF_LOCAL_STORE_DIR or "
               "choose a different TRAF_LOCAL_STORE_DIR: " + rocksError);
    }
  else
    {
      setError(error, prefix + ": " + rocksError);
    }
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

static std::string tableKey(const LocalLiteTableDef &table)
{
  return tableKey(table.catalog, table.schema, table.name);
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
  out += "LLT3\n";
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
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.primaryKeyColumns.size()));
  out += buf;
  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    {
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(table.primaryKeyColumns[i]));
      out += buf;
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.uniqueKeyColumns.size()));
  out += buf;
  for (size_t i = 0; i < table.uniqueKeyColumns.size(); i++)
    {
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(table.uniqueKeyColumns[i].size()));
      out += buf;
      for (size_t j = 0; j < table.uniqueKeyColumns[i].size(); j++)
        {
          snprintf(buf, sizeof(buf), "%lu\n",
                   static_cast<unsigned long>(table.uniqueKeyColumns[i][j]));
          out += buf;
        }
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
  if (!nextLine(encoded, &pos, &line) ||
      (line != "LLT1" && line != "LLT2" && line != "LLT3"))
    {
      setError(error, "invalid local-lite table metadata");
      return false;
    }
  const bool hasKeyMetadata = (line == "LLT2" || line == "LLT3");
  const bool hasUniqueMetadata = (line == "LLT3");
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
  table->primaryKeyColumns.clear();
  table->uniqueKeyColumns.clear();
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
  if (hasKeyMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite primary key metadata");
          return false;
        }
      unsigned long keyCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < keyCount; i++)
        {
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite primary key metadata");
              return false;
            }
          unsigned long keyIndex = strtoul(line.c_str(), NULL, 10);
          if (keyIndex >= table->columns.size())
            {
              setError(error, "invalid local-lite primary key metadata");
              return false;
            }
          table->primaryKeyColumns.push_back(static_cast<size_t>(keyIndex));
        }
    }
  if (hasUniqueMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite unique key metadata");
          return false;
        }
      unsigned long uniqueCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < uniqueCount; i++)
        {
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite unique key metadata");
              return false;
            }
          unsigned long keyColumnCount = strtoul(line.c_str(), NULL, 10);
          std::vector<size_t> keyColumns;
          for (unsigned long j = 0; j < keyColumnCount; j++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite unique key metadata");
                  return false;
                }
              unsigned long keyIndex = strtoul(line.c_str(), NULL, 10);
              if (keyIndex >= table->columns.size())
                {
                  setError(error, "invalid local-lite unique key metadata");
                  return false;
                }
              keyColumns.push_back(static_cast<size_t>(keyIndex));
            }
          if (keyColumns.empty())
            {
              setError(error, "invalid local-lite unique key metadata");
              return false;
            }
          table->uniqueKeyColumns.push_back(keyColumns);
        }
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

    releaseAllStatementSnapshotsLocked();

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

  void beginStatement(const void *statementOwner,
                      uint64_t statementExecutionId)
  {
    if (!statementOwner)
      return;

    LocalLiteMutexGuard guard(&mutex_);
    for (StatementMap::iterator it = statementSnapshots_.begin();
         it != statementSnapshots_.end();)
      {
        if (it->first.owner == statementOwner)
          {
            releaseStatementSnapshotsLocked(it->first, it->second);
            StatementMap::iterator stale = it++;
            statementSnapshots_.erase(stale);
          }
        else
          {
            ++it;
          }
      }

    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    statementSnapshots_[key] = SnapshotMap();
  }

  void endStatement(const void *statementOwner,
                    uint64_t statementExecutionId)
  {
    if (!statementOwner)
      return;

    LocalLiteMutexGuard guard(&mutex_);
    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    StatementMap::iterator it = statementSnapshots_.find(key);
    if (it == statementSnapshots_.end())
      return;

    releaseStatementSnapshotsLocked(it->first, it->second);
    statementSnapshots_.erase(it);
  }

  bool getStatementSnapshot(const std::string &path,
                            rocksdb_t *db,
                            const void *statementOwner,
                            uint64_t statementExecutionId,
                            const rocksdb_snapshot_t **snapshot,
                            std::string *error)
  {
    if (snapshot)
      *snapshot = NULL;
    if (!statementOwner || !snapshot)
      {
        setError(error, "missing local-lite statement snapshot context");
        return false;
      }

    LocalLiteMutexGuard guard(&mutex_);
    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    StatementMap::iterator statement = statementSnapshots_.find(key);
    if (statement == statementSnapshots_.end())
      {
        setError(error, "local-lite statement snapshot context is not active");
        return false;
      }

    SnapshotMap::iterator existing = statement->second.find(path);
    if (existing != statement->second.end())
      {
        if (existing->second.db != db)
          {
            setError(error, "local-lite statement snapshot table handle changed");
            return false;
          }
        *snapshot = existing->second.snapshot;
        traceStatementSnapshot("REUSE", path, statementExecutionId);
        return true;
      }

    StatementSnapshot entry;
    entry.db = db;
    entry.snapshot = rocksdb_create_snapshot(db);
    if (!entry.snapshot)
      {
        setError(error, "create local-lite RocksDB statement snapshot failed");
        return false;
      }
    statement->second[path] = entry;
    *snapshot = entry.snapshot;
    traceStatementSnapshot("ACQUIRE", path, statementExecutionId);
    return true;
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

    std::string key;
    if (!loaded.primaryKeyColumns.empty())
      {
        if (!LocalLiteBuildPrimaryKey(loaded, encodedRow, &key, error))
          return false;
      }
    else
      {
        appendUint64(key, loaded.nextRowId);
      }
    std::vector<std::string> uniqueKeys;
    for (size_t i = 0; i < loaded.uniqueKeyColumns.size(); i++)
      {
        std::string uniqueKey;
        bool hasUniqueKey = false;
        if (!LocalLiteBuildUniqueKey(loaded, encodedRow,
                                     loaded.uniqueKeyColumns[i], i,
                                     &uniqueKey, &hasUniqueKey, error))
          return false;
        if (hasUniqueKey)
          uniqueKeys.push_back(uniqueKey);
      }

    const uint64_t allocatedRowId = loaded.nextRowId;
    LocalLiteTableDef updated = loaded;
    if (loaded.primaryKeyColumns.empty())
      updated.nextRowId++;

    std::string tableMetadataKey;
    std::string oldMetadata;
    if (loaded.primaryKeyColumns.empty())
      {
        tableMetadataKey = tableKey(loaded.catalog, loaded.schema, loaded.name);
        oldMetadata = encodeTable(loaded);
        std::string newMetadata = encodeTable(updated);
        if (!putCatalogLocked(tableMetadataKey, newMetadata,
                              "update local-lite row id metadata", error))
          return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      {
        if (loaded.primaryKeyColumns.empty())
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        return false;
      }

    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    char *err = NULL;
    size_t existingLen = 0;
    char *existing = rocksdb_get(db, readOptions,
                                 key.data(), key.size(), &existingLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, "read local-lite row", error))
      {
        if (loaded.primaryKeyColumns.empty())
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        return false;
      }
    if (existing)
      {
        rocksdb_free(existing);
        if (loaded.primaryKeyColumns.empty())
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        setError(error, loaded.primaryKeyColumns.empty()
                        ? "duplicate local-lite row key"
                        : "duplicate local-lite primary key");
        return false;
      }
    for (size_t i = 0; i < uniqueKeys.size(); i++)
      {
        readOptions = rocksdb_readoptions_create();
        err = NULL;
        existingLen = 0;
        existing = rocksdb_get(db, readOptions,
                               uniqueKeys[i].data(), uniqueKeys[i].size(),
                               &existingLen, &err);
        rocksdb_readoptions_destroy(readOptions);
        if (!checkRocksError(err, "read local-lite unique key", error))
          {
            if (loaded.primaryKeyColumns.empty())
              putCatalogLocked(tableMetadataKey, oldMetadata,
                               "rollback local-lite row id metadata", NULL);
            return false;
          }
        if (existing)
          {
            rocksdb_free(existing);
            if (loaded.primaryKeyColumns.empty())
              putCatalogLocked(tableMetadataKey, oldMetadata,
                               "rollback local-lite row id metadata", NULL);
            setError(error, "duplicate local-lite unique key");
            return false;
          }
      }

    std::string value = encodeRowValue(encodedRow);
    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    rocksdb_writebatch_put(batch, key.data(), key.size(),
                           value.data(), value.size());
    for (size_t i = 0; i < uniqueKeys.size(); i++)
      rocksdb_writebatch_put(batch, uniqueKeys[i].data(), uniqueKeys[i].size(),
                             key.data(), key.size());
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    err = NULL;
    rocksdb_write(db, writeOptions, batch, &err);
    rocksdb_writeoptions_destroy(writeOptions);
    rocksdb_writebatch_destroy(batch);
    if (!checkRocksError(err, "write local-lite row", error))
      {
        std::string rollbackError;
        if (loaded.primaryKeyColumns.empty() &&
            !putCatalogLocked(tableMetadataKey, oldMetadata,
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

    releaseTableSnapshotsLocked(path);

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

  struct StatementKey
  {
    const void *owner;
    uint64_t executionId;
  };

  struct StatementKeyLess
  {
    bool operator()(const StatementKey &left,
                    const StatementKey &right) const
    {
      std::less<const void *> less;
      if (less(left.owner, right.owner))
        return true;
      if (less(right.owner, left.owner))
        return false;
      return left.executionId < right.executionId;
    }
  };

  struct StatementSnapshot
  {
    rocksdb_t *db;
    const rocksdb_snapshot_t *snapshot;
  };

  typedef std::map<std::string, StatementSnapshot> SnapshotMap;
  typedef std::map<StatementKey, SnapshotMap, StatementKeyLess> StatementMap;

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

  void releaseStatementSnapshotsLocked(const StatementKey &key,
                                       SnapshotMap &snapshots)
  {
    for (SnapshotMap::iterator it = snapshots.begin();
         it != snapshots.end(); ++it)
      {
        rocksdb_release_snapshot(it->second.db, it->second.snapshot);
        traceStatementSnapshot("RELEASE", it->first, key.executionId);
      }
    snapshots.clear();
  }

  void releaseTableSnapshotsLocked(const std::string &path)
  {
    for (StatementMap::iterator statement = statementSnapshots_.begin();
         statement != statementSnapshots_.end(); ++statement)
      {
        SnapshotMap::iterator snapshot = statement->second.find(path);
        if (snapshot == statement->second.end())
          continue;
        rocksdb_release_snapshot(snapshot->second.db, snapshot->second.snapshot);
        traceStatementSnapshot("RELEASE", path,
                               statement->first.executionId);
        statement->second.erase(snapshot);
      }
  }

  void releaseAllStatementSnapshotsLocked()
  {
    for (StatementMap::iterator it = statementSnapshots_.begin();
         it != statementSnapshots_.end(); ++it)
      releaseStatementSnapshotsLocked(it->first, it->second);
    statementSnapshots_.clear();
  }

  unsigned int refCount_;
  rocksdb_t *catalogDb_;
  TableMap tableDbs_;
  StatementMap statementSnapshots_;
  pthread_mutex_t mutex_;
};

class LocalLiteTxnState
{
public:
  LocalLiteTxnState()
    : active_(false),
      nextLocalTxnId_(1),
      localTxnId_(0),
      executorTxnId_(LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID)
  {
    pthread_mutex_init(&mutex_, NULL);
  }

  bool begin(int64_t executorTxnId, std::string *error)
  {
    uint64_t transactionId = 0;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          setError(error, "local-lite transaction already active");
          return false;
        }

      pendingTables_.clear();
      localTxnId_ = nextLocalTxnId_++;
      if (nextLocalTxnId_ == 0)
        nextLocalTxnId_ = 1;
      executorTxnId_ = executorTxnId;
      active_ = true;
      transactionId = localTxnId_;
    }

    if (!transactionStore_.open(error))
      {
        LocalLiteMutexGuard guard(&mutex_);
        pendingTables_.clear();
        localTxnId_ = 0;
        executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
        active_ = false;
        return false;
      }

    LocalLiteStorageManager::instance().beginStatement(this, transactionId);
    return true;
  }

  bool commit(int64_t executorTxnId, std::string *error)
  {
    PendingMap pending;
    uint64_t transactionId = 0;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (!active_)
        {
          setError(error, "no active local-lite transaction");
          return false;
        }
      if (!matchesExecutorTxnId(executorTxnId))
        {
          setError(error, "local-lite transaction context mismatch");
          return false;
        }

      pending.swap(pendingTables_);
      transactionId = localTxnId_;
      localTxnId_ = 0;
      executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
      active_ = false;
    }

    LocalLiteStorageManager::instance().endStatement(this, transactionId);
    transactionStore_.close();

    LocalLiteRocksDBStore store;
    for (PendingMap::iterator t = pending.begin(); t != pending.end(); ++t)
      {
        for (size_t i = 0; i < t->second.rows.size(); i++)
          {
            uint64_t rowId = 0;
            if (!store.insertRow(t->second.table, t->second.rows[i].value,
                                 &rowId, error))
              return false;
          }
      }
    return true;
  }

  bool rollback(int64_t executorTxnId, std::string *error)
  {
    uint64_t transactionId = 0;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (!active_)
        {
          setError(error, "no active local-lite transaction");
          return false;
        }
      if (!matchesExecutorTxnId(executorTxnId))
        {
          setError(error, "local-lite transaction context mismatch");
          return false;
        }

      pendingTables_.clear();
      transactionId = localTxnId_;
      localTxnId_ = 0;
      executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
      active_ = false;
    }

    LocalLiteStorageManager::instance().endStatement(this, transactionId);
    transactionStore_.close();
    return true;
  }

  bool active()
  {
    LocalLiteMutexGuard guard(&mutex_);
    return active_;
  }

  uint64_t currentLocalTxnId()
  {
    LocalLiteMutexGuard guard(&mutex_);
    return active_ ? localTxnId_ : 0;
  }

  int64_t currentExecutorTxnId()
  {
    LocalLiteMutexGuard guard(&mutex_);
    return active_ ? executorTxnId_
                   : LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
  }

  bool insertRow(LocalLiteRocksDBStore *store,
                 const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return store->insertRow(table, encodedRow, rowId, error);

    const std::string key = tableKey(table);
    PendingTable &pending = pendingTables_[key];
    if (pending.rows.empty())
      {
        LocalLiteTableDef loaded;
        if (!store->loadTable(table.catalog, table.schema, table.name,
                              &loaded, error))
          return false;
        pending.table = loaded;
        pending.nextRowId = loaded.nextRowId;
      }

    if (!pending.table.primaryKeyColumns.empty())
      {
        std::string newKey;
        if (!LocalLiteBuildPrimaryKey(pending.table, encodedRow, &newKey, error))
          return false;
        for (size_t i = 0; i < pending.rows.size(); i++)
          {
            std::string existingKey;
            if (!LocalLiteBuildPrimaryKey(pending.table, pending.rows[i].value,
                                          &existingKey, error))
              return false;
            if (existingKey == newKey)
              {
                setError(error, "duplicate local-lite primary key");
                return false;
              }
          }
      }
    for (size_t keyIndex = 0;
         keyIndex < pending.table.uniqueKeyColumns.size(); keyIndex++)
      {
        std::string newKey;
        bool hasNewKey = false;
        if (!LocalLiteBuildUniqueKey(pending.table, encodedRow,
                                     pending.table.uniqueKeyColumns[keyIndex],
                                     keyIndex, &newKey, &hasNewKey, error))
          return false;
        if (!hasNewKey)
          continue;
        for (size_t i = 0; i < pending.rows.size(); i++)
          {
            std::string existingKey;
            bool hasExistingKey = false;
            if (!LocalLiteBuildUniqueKey(
                    pending.table, pending.rows[i].value,
                    pending.table.uniqueKeyColumns[keyIndex], keyIndex,
                    &existingKey, &hasExistingKey, error))
              return false;
            if (hasExistingKey && existingKey == newKey)
              {
                setError(error, "duplicate local-lite unique key");
                return false;
              }
          }
      }

    LocalLiteRow row;
    row.rowId = pending.nextRowId++;
    row.value = encodedRow;
    pending.rows.push_back(row);
    if (rowId)
      *rowId = row.rowId;
    return true;
  }

  bool scanRows(LocalLiteRocksDBStore *store,
                const LocalLiteTableDef &table,
                const void *statementOwner,
                uint64_t statementExecutionId,
                std::vector<LocalLiteRow> *rows,
                std::string *error)
  {
    const void *readOwner = statementOwner;
    uint64_t readExecutionId = statementExecutionId;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          readOwner = this;
          readExecutionId = localTxnId_;
        }
    }

    if (!store->scanRows(table, readOwner, readExecutionId,
                         rows, error))
      return false;

    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return true;

    PendingMap::iterator it = pendingTables_.find(tableKey(table));
    if (it == pendingTables_.end())
      return true;

    for (size_t i = 0; i < it->second.rows.size(); i++)
      rows->push_back(it->second.rows[i]);
    return true;
  }

  bool getRowByKey(LocalLiteRocksDBStore *store,
                   const LocalLiteTableDef &table,
                   const std::string &storageKey,
                   const void *statementOwner,
                   uint64_t statementExecutionId,
                   LocalLiteRow *row,
                   bool *found,
                   std::string *error)
  {
    if (found)
      *found = false;
    if (!row || !found)
      {
        setError(error, "missing local-lite get row output");
        return false;
      }

    const void *readOwner = statementOwner;
    uint64_t readExecutionId = statementExecutionId;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          readOwner = this;
          readExecutionId = localTxnId_;
          PendingMap::iterator it = pendingTables_.find(tableKey(table));
          if (it != pendingTables_.end())
            {
              for (size_t i = 0; i < it->second.rows.size(); i++)
                {
                  if (!pendingRowMatchesKey(it->second.table,
                                            it->second.rows[i],
                                            storageKey,
                                            found,
                                            error))
                    return false;
                  if (*found)
                    {
                      *row = it->second.rows[i];
                      return true;
                    }
                }
            }
        }
    }

    return store->getRowByKey(table, storageKey, readOwner,
                              readExecutionId, row, found, error);
  }

  static LocalLiteTxnState &instance()
  {
    static LocalLiteTxnState state;
    return state;
  }

private:
  struct PendingTable
  {
    LocalLiteTableDef table;
    uint64_t nextRowId;
    std::vector<LocalLiteRow> rows;
  };
  typedef std::map<std::string, PendingTable> PendingMap;

  bool pendingRowMatchesKey(const LocalLiteTableDef &table,
                            const LocalLiteRow &row,
                            const std::string &storageKey,
                            bool *matches,
                            std::string *error)
  {
    *matches = false;
    if (storageKey.size() > 0 && storageKey[0] == 'P')
      {
        std::string key;
        if (!LocalLiteBuildPrimaryKey(table, row.value, &key, error))
          return false;
        *matches = (key == storageKey);
        return true;
      }

    if (storageKey.size() > 0 && storageKey[0] == 'U')
      {
        for (size_t keyIndex = 0;
             keyIndex < table.uniqueKeyColumns.size(); keyIndex++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildUniqueKey(table, row.value,
                                         table.uniqueKeyColumns[keyIndex],
                                         keyIndex, &key, &hasKey, error))
              return false;
            if (hasKey && key == storageKey)
              {
                *matches = true;
                return true;
              }
          }
        return true;
      }

    if (storageKey.size() == 8)
      {
        std::string key;
        appendUint64(key, row.rowId);
        *matches = (key == storageKey);
      }
    return true;
  }

  bool matchesExecutorTxnId(int64_t executorTxnId) const
  {
    return executorTxnId == LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID ||
           executorTxnId_ == LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID ||
           executorTxnId == executorTxnId_;
  }

  LocalLiteTxnState(const LocalLiteTxnState &);
  LocalLiteTxnState &operator=(const LocalLiteTxnState &);

  bool active_;
  uint64_t nextLocalTxnId_;
  uint64_t localTxnId_;
  int64_t executorTxnId_;
  PendingMap pendingTables_;
  LocalLiteRocksDBStore transactionStore_;
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

bool LocalLiteRocksDBStore::getRowByKey(const LocalLiteTableDef &table,
                                        const std::string &storageKey,
                                        LocalLiteRow *row,
                                        bool *found,
                                        std::string *error)
{
  return getRowByKey(table, storageKey, NULL, 0, row, found, error);
}

static bool getRowByKeyAtSnapshot(rocksdb_t *db,
                                  const rocksdb_snapshot_t *snapshot,
                                  const std::string &storageKey,
                                  LocalLiteRow *row,
                                  bool *found,
                                  std::string *error)
{
  if (found)
    *found = false;
  if (!row || !found)
    {
      setError(error, "missing local-lite get row output");
      return false;
    }

  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  if (snapshot)
    rocksdb_readoptions_set_snapshot(readOptions, snapshot);
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(db, readOptions,
                            storageKey.data(), storageKey.size(),
                            &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite row", error))
    return false;
  if (!value)
    return true;

  std::string encodedValue(value, valueLen);
  rocksdb_free(value);

  std::string encodedRow;
  if (decodeRowValue(encodedValue, &encodedRow, NULL))
    {
      row->rowId = 0;
      if (storageKey.size() == 8)
        {
          size_t offset = 0;
          row->rowId = readUint64(storageKey, &offset);
        }
      row->value = encodedRow;
      *found = true;
      return true;
    }

  if (storageKey.size() > 0 && storageKey[0] == 'U')
    {
      std::string referencedKey = encodedValue;
      return getRowByKeyAtSnapshot(db, snapshot, referencedKey,
                                   row, found, error);
    }

  setError(error, "invalid local-lite row format");
  return false;
}

bool LocalLiteRocksDBStore::getRowByKey(
    const LocalLiteTableDef &table,
    const std::string &storageKey,
    const void *statementOwner,
    uint64_t statementExecutionId,
    LocalLiteRow *row,
    bool *found,
    std::string *error)
{
  if (!open(error))
    return false;

  const std::string path = tablePath(table);
  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(path, false,
                                                               error);
  if (!db)
    return false;

  const rocksdb_snapshot_t *snapshot = NULL;
  if (statementOwner &&
      !LocalLiteStorageManager::instance().getStatementSnapshot(
          path, db, statementOwner, statementExecutionId, &snapshot, error))
    return false;

  return getRowByKeyAtSnapshot(db, snapshot, storageKey, row, found, error);
}

bool LocalLiteRocksDBStore::scanRows(const LocalLiteTableDef &table,
                                     std::vector<LocalLiteRow> *rows,
                                     std::string *error)
{
  return scanRows(table, NULL, 0, rows, error);
}

bool LocalLiteRocksDBStore::scanRows(const LocalLiteTableDef &table,
                                     const void *statementOwner,
                                     uint64_t statementExecutionId,
                                     std::vector<LocalLiteRow> *rows,
                                     std::string *error)
{
  rows->clear();

  if (!open(error))
    return false;

  const std::string path = tablePath(table);
  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(path, false,
                                                               error);
  if (!db)
    return false;

  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  const bool ownsSnapshot = (statementOwner == NULL);
  const rocksdb_snapshot_t *snapshot = NULL;
  if (ownsSnapshot)
    snapshot = rocksdb_create_snapshot(db);
  else if (!LocalLiteStorageManager::instance().getStatementSnapshot(
               path, db, statementOwner, statementExecutionId,
               &snapshot, error))
    {
      rocksdb_readoptions_destroy(readOptions);
      return false;
    }
  if (!snapshot)
    {
      rocksdb_readoptions_destroy(readOptions);
      setError(error, "create local-lite RocksDB scan snapshot failed");
      return false;
    }
  rocksdb_readoptions_set_snapshot(readOptions, snapshot);
  rocksdb_iterator_t *it = rocksdb_create_iterator(db, readOptions);
  for (rocksdb_iter_seek_to_first(it); rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      size_t valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      std::string key(rawKey, keyLen);
      if (key.size() > 0 && key[0] == 'U')
        continue;
      std::string value(rawValue, valueLen);
      size_t offset = 0;
      LocalLiteRow row;
      row.rowId = (key.size() == 8) ? readUint64(key, &offset) : 0;
      if (!decodeRowValue(value, &row.value, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          if (ownsSnapshot)
            rocksdb_release_snapshot(db, snapshot);
          return false;
        }
      rows->push_back(row);
    }

  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (ownsSnapshot)
    rocksdb_release_snapshot(db, snapshot);
  if (!checkRocksError(err, "scan local-lite rows", error))
    return false;
  return true;
}

LocalLiteTxn::LocalLiteTxn(LocalLiteRocksDBStore *store,
                           const void *statementOwner,
                           uint64_t statementExecutionId)
  : store_(store),
    statementOwner_(statementOwner),
    statementExecutionId_(statementExecutionId)
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

  return LocalLiteTxnState::instance().insertRow(store_, table, encodedRow,
                                                rowId, error);
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

  return LocalLiteTxnState::instance().scanRows(
      store_, table, statementOwner_, statementExecutionId_, rows, error);
}

bool LocalLiteTxn::getRowByKey(const LocalLiteTableDef &table,
                               const std::string &storageKey,
                               LocalLiteRow *row,
                               bool *found,
                               std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }

  return LocalLiteTxnState::instance().getRowByKey(
      store_, table, storageKey, statementOwner_, statementExecutionId_,
      row, found, error);
}

bool LocalLiteTxnManager::begin(std::string *error)
{
  return beginForExecutor(INVALID_EXECUTOR_TXN_ID, error);
}

bool LocalLiteTxnManager::beginForExecutor(int64_t executorTxnId,
                                           std::string *error)
{
  return LocalLiteTxnState::instance().begin(executorTxnId, error);
}

bool LocalLiteTxnManager::commit(std::string *error)
{
  return commitForExecutor(INVALID_EXECUTOR_TXN_ID, error);
}

bool LocalLiteTxnManager::commitForExecutor(int64_t executorTxnId,
                                            std::string *error)
{
  return LocalLiteTxnState::instance().commit(executorTxnId, error);
}

bool LocalLiteTxnManager::rollback(std::string *error)
{
  return rollbackForExecutor(INVALID_EXECUTOR_TXN_ID, error);
}

bool LocalLiteTxnManager::rollbackForExecutor(int64_t executorTxnId,
                                              std::string *error)
{
  return LocalLiteTxnState::instance().rollback(executorTxnId, error);
}

bool LocalLiteTxnManager::active()
{
  return LocalLiteTxnState::instance().active();
}

uint64_t LocalLiteTxnManager::currentLocalTxnId()
{
  return LocalLiteTxnState::instance().currentLocalTxnId();
}

int64_t LocalLiteTxnManager::currentExecutorTxnId()
{
  return LocalLiteTxnState::instance().currentExecutorTxnId();
}

void LocalLiteTxnManager::beginStatement(const void *statementOwner,
                                         uint64_t statementExecutionId)
{
  LocalLiteStorageManager::instance().beginStatement(statementOwner,
                                                     statementExecutionId);
}

void LocalLiteTxnManager::endStatement(const void *statementOwner,
                                       uint64_t statementExecutionId)
{
  LocalLiteStorageManager::instance().endStatement(statementOwner,
                                                   statementExecutionId);
}

#endif
