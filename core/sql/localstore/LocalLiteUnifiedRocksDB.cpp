// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@

#define LOCAL_LITE_UNIFIED_ROCKSDB_IMPLEMENTATION
#include "LocalLiteUnifiedRocksDB.h"

#include "LocalLiteStorage.h"
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <pthread.h>
#include <string>
#include <vector>

namespace
{

const char *FORMAT_KEY = "m12/format";
const char *FORMAT_VALUE = "LocalLiteTxnStore/2";
const char *ACTIVE_KEY = "m13/active";
const char *LAYOUT_KEY = "m13/layout";
const char *LAYOUT_VALUE = "unified-hex-v1";

struct LogicalDb
{
  LogicalDb() : real(NULL) {}
  rocksdb_t *real;
  std::string prefix;
};

struct BatchOperation
{
  bool erase;
  std::string key;
  std::string value;
};

struct LogicalBatch
{
  std::vector<BatchOperation> operations;
};

struct LogicalIterator
{
  LogicalIterator() : real(NULL) {}
  rocksdb_iterator_t *real;
  std::string prefix;
  mutable std::string decodedKey;
  mutable std::string decodeError;
};

struct UnifiedState
{
  UnifiedState()
    : prepared(false), active(false), transactionDb(NULL), baseDb(NULL),
      blockCache(NULL), syncWriteOptions(NULL), asyncWriteOptions(NULL)
  {
  }

  bool prepared;
  bool active;
  std::string root;
  rocksdb_transactiondb_t *transactionDb;
  rocksdb_t *baseDb;
  rocksdb_cache_t *blockCache;
  rocksdb_writeoptions_t *syncWriteOptions;
  rocksdb_writeoptions_t *asyncWriteOptions;
};

UnifiedState unifiedState;

struct GroupCommitRequest
{
  GroupCommitRequest() : batch(NULL), done(false), passed(false) {}
  LocalLiteUnifiedWriteBatch *batch;
  bool done;
  bool passed;
  std::string error;
};

struct GroupCommitState
{
  GroupCommitState() : processing(false)
  {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&condition, NULL);
  }
  ~GroupCommitState()
  {
    pthread_cond_destroy(&condition);
    pthread_mutex_destroy(&mutex);
  }
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  bool processing;
  std::vector<GroupCommitRequest *> pending;
};

GroupCommitState syncGroupCommit;
GroupCommitState asyncGroupCommit;

static uint64_t groupCommitWindowMicros()
{
  const char *setting = getenv("TRAF_LOCAL_LITE_GROUP_COMMIT_WINDOW_US");
  if (!setting || !setting[0])
    return 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(setting, &end, 10);
  return end && *end == '\0' ? static_cast<uint64_t>(parsed) : 0;
}

void setStringError(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
}

void setRocksError(char **error, const std::string &message)
{
  if (!error)
    return;
  *error = static_cast<char *>(malloc(message.size() + 1));
  if (*error)
    memcpy(*error, message.c_str(), message.size() + 1);
}

bool isLockError(const std::string &message)
{
  std::string lower(message);
  for (size_t i = 0; i < lower.size(); i++)
    lower[i] = static_cast<char>(tolower(
        static_cast<unsigned char>(lower[i])));
  return lower.find("/lock") != std::string::npos ||
      lower.find(" lock ") != std::string::npos ||
      lower.find("lock file") != std::string::npos ||
      lower.find("lock hold") != std::string::npos;
}

void setUnifiedOpenError(std::string *error,
                         const std::string &prefix,
                         const std::string &message)
{
  if (isLockError(message))
    setStringError(error, prefix +
        ": local-lite store is already open by another process; use one "
        "sqlci process per TRAF_LOCAL_STORE_DIR or choose a different "
        "TRAF_LOCAL_STORE_DIR: " + message);
  else
    setStringError(error, prefix + ": " + message);
}

bool pathExists(const std::string &path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0;
}

std::string hexEncode(const std::string &value)
{
  static const char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (size_t i = 0; i < value.size(); i++)
    {
      const unsigned char c = static_cast<unsigned char>(value[i]);
      result += digits[c >> 4];
      result += digits[c & 15];
    }
  return result;
}

int hexDigit(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool hexDecode(const char *value, size_t length, std::string *result)
{
  result->clear();
  if ((length & 1) != 0)
    return false;
  result->reserve(length / 2);
  for (size_t i = 0; i < length; i += 2)
    {
      const int high = hexDigit(value[i]);
      const int low = hexDigit(value[i + 1]);
      if (high < 0 || low < 0)
        return false;
      result->push_back(static_cast<char>((high << 4) | low));
    }
  return true;
}

bool hasPrefix(const char *value, size_t length, const std::string &prefix)
{
  return length >= prefix.size() &&
      memcmp(value, prefix.data(), prefix.size()) == 0;
}

bool logicalPrefixForPath(const std::string &path, std::string *prefix)
{
  if (!unifiedState.active)
    return false;
  const std::string catalog = unifiedState.root + "/catalog";
  if (path == catalog)
    {
      *prefix = "catalog/";
      return true;
    }
  const std::string data = unifiedState.root + "/data/";
  if (path.compare(0, data.size(), data) == 0)
    {
      *prefix = "table/" + hexEncode(path.substr(data.size())) + "/";
      return true;
    }
  return false;
}

std::string physicalKey(const LogicalDb *db,
                        const char *key,
                        size_t keyLength)
{
  return db->prefix + hexEncode(std::string(key, keyLength));
}

bool getStorageValue(LocalLiteStorageTxn *txn,
                     const std::string &key,
                     std::string *value,
                     bool *found,
                     LocalLiteStorageStatus *status)
{
  return txn && txn->get(key, value, found, status);
}

bool storageIsEmpty(LocalLiteStorageTxn *txn,
                    bool *empty,
                    LocalLiteStorageStatus *status)
{
  if (!txn || !empty)
    return false;
  LocalLiteStorageCursor *cursor = txn->scan("", "", status);
  if (!cursor)
    return false;
  LocalLiteStorageRecord record;
  bool end = false;
  const bool ok = cursor->next(&record, &end, status);
  delete cursor;
  if (ok)
    *empty = end;
  return ok;
}

bool prepareTarget(const std::string &root, std::string *error)
{
  const std::string targetPath = LocalLiteUnifiedRocksDBPath(root);
  if (pathExists(root + "/catalog") || pathExists(root + "/data"))
    {
      setStringError(error,
          "legacy per-table RocksDB layout is unsupported; remove catalog/ "
          "and data/ before starting this build");
      return false;
    }

  LocalLiteStorageEngine *engine = LocalLiteCreateRocksDBTransactionEngine();
  LocalLiteStorageOptions options;
  options.createIfMissing = true;
  options.synchronousCommit = true;
  const char *minimumFree = getenv("TRAF_LOCAL_LITE_MINIMUM_FREE_BYTES");
  if (minimumFree && minimumFree[0])
    options.minimumFreeBytes = strtoull(minimumFree, NULL, 10);
  LocalLiteStorageStatus status;
  if (!engine->open(targetPath, options, &status))
    {
      setUnifiedOpenError(error, "open LocalLite unified target",
                          status.message);
      delete engine;
      return false;
    }

  LocalLiteStorageSession *session = engine->createSession(&status);
  LocalLiteStorageTxn *txn = session ? session->begin(&status) : NULL;
  std::string format;
  std::string active;
  std::string layout;
  bool formatFound = false;
  bool activeFound = false;
  bool layoutFound = false;
  bool targetEmpty = false;
  bool ok = getStorageValue(txn, FORMAT_KEY, &format, &formatFound, &status) &&
      getStorageValue(txn, ACTIVE_KEY, &active, &activeFound, &status) &&
      getStorageValue(txn, LAYOUT_KEY, &layout, &layoutFound, &status) &&
      storageIsEmpty(txn, &targetEmpty, &status);
  if (txn)
    {
      LocalLiteStorageStatus ignored;
      txn->rollback(&ignored);
    }
  delete txn;
  delete session;
  if (!ok)
    {
      setStringError(error, "read LocalLite unified format: " +
                            status.message);
      engine->close();
      delete engine;
      return false;
    }
  if (formatFound && format != FORMAT_VALUE)
    {
      setStringError(error, "unsupported LocalLite unified format: " + format);
      engine->close();
      delete engine;
      return false;
    }
  if (activeFound && active != "1")
    {
      setStringError(error, "invalid LocalLite unified activation marker");
      engine->close();
      delete engine;
      return false;
    }
  if (layoutFound && layout != LAYOUT_VALUE)
    {
      setStringError(error, "unsupported LocalLite unified layout: " + layout);
      engine->close();
      delete engine;
      return false;
    }
  if (activeFound && (!formatFound || !layoutFound))
    {
      setStringError(error, "incomplete active LocalLite unified format");
      engine->close();
      delete engine;
      return false;
    }
  if (layoutFound && !activeFound)
    {
      setStringError(error, "LocalLite unified layout is not activated");
      engine->close();
      delete engine;
      return false;
    }
  if (!formatFound && !targetEmpty)
    {
      setStringError(error, "unrecognized non-empty LocalLite unified target");
      engine->close();
      delete engine;
      return false;
    }

  if (!activeFound || active != "1")
    {
      if (!formatFound)
        {
          session = engine->createSession(&status);
          txn = session ? session->begin(&status) : NULL;
          ok = txn && txn->put(FORMAT_KEY, FORMAT_VALUE, &status) &&
              txn->commit(&status);
          delete txn;
          delete session;
        }
      if (!ok)
        {
          setStringError(error, "prepare LocalLite unified target: " +
                                status.message);
          engine->close();
          delete engine;
          return false;
        }

      const char *fault = getenv("TRAF_LOCAL_LITE_ACTIVATION_FAULT");
      if (fault && strcmp(fault, "after-format") == 0)
        _exit(91);

      session = engine->createSession(&status);
      txn = session ? session->begin(&status) : NULL;
      ok = txn && txn->put(ACTIVE_KEY, "1", &status) &&
          txn->put(LAYOUT_KEY, LAYOUT_VALUE, &status) &&
          txn->commit(&status);
      delete txn;
      delete session;
      if (!ok)
        {
          setStringError(error, "activate LocalLite unified target: " +
                                status.message);
          engine->close();
          delete engine;
          return false;
        }
    }

  engine->close();
  delete engine;
  return true;
}

bool openUnifiedTarget(const std::string &root, std::string *error)
{
  rocksdb_options_t *options = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(options, 0);
  rocksdb_options_set_paranoid_checks(options, 1);
  const char *cacheText = getenv("TRAF_LOCAL_LITE_BLOCK_CACHE_BYTES");
  // Keep the default conservative for existing deployments; operators can
  // opt in after sizing the process RSS with the bounded cache knob.
  uint64_t cacheBytes = 0;
  if (cacheText && cacheText[0])
    {
      char *end = NULL;
      const unsigned long long parsed = strtoull(cacheText, &end, 10);
      if (end && *end == '\0')
        cacheBytes = static_cast<uint64_t>(parsed);
    }
  if (cacheBytes > 0)
    {
      unifiedState.blockCache = rocksdb_cache_create_lru(
          static_cast<size_t>(cacheBytes));
      rocksdb_block_based_table_options_t *tableOptions =
          rocksdb_block_based_options_create();
      rocksdb_block_based_options_set_block_cache(
          tableOptions, unifiedState.blockCache);
      rocksdb_block_based_options_set_filter_policy(
          tableOptions, rocksdb_filterpolicy_create_bloom(10));
      rocksdb_block_based_options_set_cache_index_and_filter_blocks(
          tableOptions, 1);
      rocksdb_block_based_options_set_pin_l0_filter_and_index_blocks_in_cache(
          tableOptions, 1);
      rocksdb_options_set_block_based_table_factory(options, tableOptions);
      rocksdb_block_based_options_destroy(tableOptions);
    }
  unifiedState.syncWriteOptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(unifiedState.syncWriteOptions, 1);
  unifiedState.asyncWriteOptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(unifiedState.asyncWriteOptions, 0);
  rocksdb_transactiondb_options_t *transactionOptions =
      rocksdb_transactiondb_options_create();
  char *rocksError = NULL;
  unifiedState.transactionDb = rocksdb_transactiondb_open(
      options, transactionOptions,
      LocalLiteUnifiedRocksDBPath(root).c_str(), &rocksError);
  rocksdb_transactiondb_options_destroy(transactionOptions);
  rocksdb_options_destroy(options);
  if (rocksError)
    {
      const std::string message(rocksError);
      rocksdb_free(rocksError);
      setUnifiedOpenError(error, "open active LocalLite TransactionDB",
                          message);
      unifiedState.transactionDb = NULL;
      rocksdb_writeoptions_destroy(unifiedState.syncWriteOptions);
      rocksdb_writeoptions_destroy(unifiedState.asyncWriteOptions);
      unifiedState.syncWriteOptions = NULL;
      unifiedState.asyncWriteOptions = NULL;
      if (unifiedState.blockCache)
        {
          rocksdb_cache_destroy(unifiedState.blockCache);
          unifiedState.blockCache = NULL;
        }
      return false;
    }
  unifiedState.baseDb =
      rocksdb_transactiondb_get_base_db(unifiedState.transactionDb);
  if (!unifiedState.baseDb)
    {
      setStringError(error, "get active LocalLite TransactionDB base handle");
      rocksdb_transactiondb_close(unifiedState.transactionDb);
      unifiedState.transactionDb = NULL;
      rocksdb_writeoptions_destroy(unifiedState.syncWriteOptions);
      rocksdb_writeoptions_destroy(unifiedState.asyncWriteOptions);
      unifiedState.syncWriteOptions = NULL;
      unifiedState.asyncWriteOptions = NULL;
      if (unifiedState.blockCache)
        {
          rocksdb_cache_destroy(unifiedState.blockCache);
          unifiedState.blockCache = NULL;
        }
      return false;
    }
  unifiedState.active = true;
  return true;
}

} // namespace

struct LocalLiteUnifiedWriteBatch
{
  LocalLiteUnifiedWriteBatch() : physical(rocksdb_writebatch_create()) {}
  ~LocalLiteUnifiedWriteBatch() { rocksdb_writebatch_destroy(physical); }
  rocksdb_writebatch_t *physical;
};

struct GroupBatchCopy
{
  rocksdb_writebatch_t *target;
};

static void copyGroupPut(void *state, const char *key, size_t keyLength,
                         const char *value, size_t valueLength)
{
  GroupBatchCopy *copy = static_cast<GroupBatchCopy *>(state);
  rocksdb_writebatch_put(copy->target, key, keyLength, value, valueLength);
}

static void copyGroupDelete(void *state, const char *key, size_t keyLength)
{
  GroupBatchCopy *copy = static_cast<GroupBatchCopy *>(state);
  rocksdb_writebatch_delete(copy->target, key, keyLength);
}

static bool writeGroup(const std::vector<GroupCommitRequest *> &requests,
                       bool sync, std::string *error)
{
  rocksdb_writebatch_t *combined = rocksdb_writebatch_create();
  GroupBatchCopy copy;
  copy.target = combined;
  for (std::vector<GroupCommitRequest *>::const_iterator request =
           requests.begin(); request != requests.end(); ++request)
    rocksdb_writebatch_iterate((*request)->batch->physical, &copy,
                               copyGroupPut, copyGroupDelete);
  rocksdb_writeoptions_t *options = sync
      ? unifiedState.syncWriteOptions : unifiedState.asyncWriteOptions;
  char *rocksError = NULL;
  if (!options)
    {
      setStringError(error, "LocalLite unified write options are unavailable");
      rocksdb_writebatch_destroy(combined);
      return false;
    }
  rocksdb_write(unifiedState.baseDb, options, combined, &rocksError);
  rocksdb_writebatch_destroy(combined);
  if (rocksError)
    {
      setStringError(error, std::string("commit LocalLite unified batch: ") +
                            rocksError);
      rocksdb_free(rocksError);
      return false;
    }
  return true;
}

std::string LocalLiteUnifiedRocksDBPath(const std::string &root)
{
  return root + "/transactiondb";
}

bool LocalLiteUnifiedRocksDBPrepare(const std::string &root,
                                    std::string *error)
{
  if (unifiedState.prepared)
    return true;
  unifiedState.prepared = true;
  unifiedState.root = root;
  if (!prepareTarget(root, error))
    {
      LocalLiteUnifiedRocksDBShutdown();
      return false;
    }
  if (!openUnifiedTarget(root, error))
    {
      LocalLiteUnifiedRocksDBShutdown();
      return false;
    }
  return true;
}

void LocalLiteUnifiedRocksDBShutdown()
{
  if (unifiedState.baseDb)
    rocksdb_transactiondb_close_base_db(unifiedState.baseDb);
  unifiedState.baseDb = NULL;
  if (unifiedState.transactionDb)
    rocksdb_transactiondb_close(unifiedState.transactionDb);
  unifiedState.transactionDb = NULL;
  if (unifiedState.syncWriteOptions)
    rocksdb_writeoptions_destroy(unifiedState.syncWriteOptions);
  if (unifiedState.asyncWriteOptions)
    rocksdb_writeoptions_destroy(unifiedState.asyncWriteOptions);
  unifiedState.syncWriteOptions = NULL;
  unifiedState.asyncWriteOptions = NULL;
  if (unifiedState.blockCache)
    rocksdb_cache_destroy(unifiedState.blockCache);
  unifiedState.blockCache = NULL;
  unifiedState.active = false;
  unifiedState.prepared = false;
  unifiedState.root.clear();
}

bool LocalLiteUnifiedRocksDBActive()
{
  return unifiedState.active;
}

uint64_t LocalLiteUnifiedRocksDBSequence()
{
  return unifiedState.baseDb
      ? rocksdb_get_latest_sequence_number(unifiedState.baseDb) : 0;
}

LocalLiteUnifiedWriteBatch *LocalLiteUnifiedWriteBatchCreate()
{
  return new LocalLiteUnifiedWriteBatch();
}

void LocalLiteUnifiedWriteBatchDestroy(LocalLiteUnifiedWriteBatch *batch)
{
  delete batch;
}

void LocalLiteUnifiedWriteBatchPut(LocalLiteUnifiedWriteBatch *batch,
                                   rocksdb_t *logicalDb,
                                   const std::string &key,
                                   const std::string &value)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(logicalDb);
  const std::string storedKey = physicalKey(logical, key.data(), key.size());
  rocksdb_writebatch_put(batch->physical, storedKey.data(), storedKey.size(),
                         value.data(), value.size());
}

void LocalLiteUnifiedWriteBatchDelete(LocalLiteUnifiedWriteBatch *batch,
                                      rocksdb_t *logicalDb,
                                      const std::string &key)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(logicalDb);
  const std::string storedKey = physicalKey(logical, key.data(), key.size());
  rocksdb_writebatch_delete(batch->physical, storedKey.data(),
                            storedKey.size());
}

bool LocalLiteUnifiedWriteBatchCommit(LocalLiteUnifiedWriteBatch *batch,
                                      bool sync,
                                      std::string *error)
{
  if (!batch || !unifiedState.baseDb)
    {
      setStringError(error, "LocalLite unified write batch is unavailable");
      return false;
    }
  GroupCommitState &group = sync ? syncGroupCommit : asyncGroupCommit;
  GroupCommitRequest request;
  request.batch = batch;
  pthread_mutex_lock(&group.mutex);
  group.pending.push_back(&request);
  const bool leader = !group.processing;
  if (leader)
    group.processing = true;
  pthread_cond_signal(&group.condition);
  pthread_mutex_unlock(&group.mutex);
  if (!leader)
    {
      pthread_mutex_lock(&group.mutex);
      while (!request.done)
        pthread_cond_wait(&group.condition, &group.mutex);
      if (!request.passed && error)
        *error = request.error;
      pthread_mutex_unlock(&group.mutex);
      return request.passed;
    }

  // The leader drains the queue, but does not hold the group mutex while the
  // physical RocksDB write (including synchronous WAL) is in progress. This
  // lets other callers enqueue the next batch instead of blocking on the
  // global publication mutex. A non-zero window only coalesces requests that
  // arrive before the first batch is detached.
  const uint64_t window = groupCommitWindowMicros();
  for (;;)
    {
      pthread_mutex_lock(&group.mutex);
      if (window > 0)
        {
          struct timespec deadline;
          clock_gettime(CLOCK_REALTIME, &deadline);
          deadline.tv_sec += static_cast<time_t>(window / 1000000ULL);
          deadline.tv_nsec += static_cast<long>(window % 1000000ULL) * 1000L;
          if (deadline.tv_nsec >= 1000000000L)
            {
              deadline.tv_sec++;
              deadline.tv_nsec -= 1000000000L;
            }
          while (group.pending.size() == 1)
            if (pthread_cond_timedwait(&group.condition, &group.mutex,
                                       &deadline) == ETIMEDOUT)
              break;
        }
      std::vector<GroupCommitRequest *> requests;
      requests.swap(group.pending);
      pthread_mutex_unlock(&group.mutex);

      std::string groupError;
      const bool passed = writeGroup(requests, sync, &groupError);

      pthread_mutex_lock(&group.mutex);
      for (std::vector<GroupCommitRequest *>::iterator current =
               requests.begin(); current != requests.end(); ++current)
        {
          (*current)->passed = passed;
          (*current)->error = groupError;
          (*current)->done = true;
        }
      pthread_cond_broadcast(&group.condition);
      if (group.pending.empty())
        {
          group.processing = false;
          pthread_cond_broadcast(&group.condition);
          pthread_mutex_unlock(&group.mutex);
          if (!passed && error)
            *error = groupError;
          return request.passed;
        }
      pthread_mutex_unlock(&group.mutex);
    }
}

bool LocalLiteUnifiedRocksDBCheckpoint(const std::string &path,
                                       std::string *error)
{
  if (!unifiedState.transactionDb || path.empty())
    {
      setStringError(error, "LocalLite checkpoint target is unavailable");
      return false;
    }
  char *rocksError = NULL;
  rocksdb_checkpoint_t *checkpoint =
      rocksdb_transactiondb_checkpoint_object_create(
          unifiedState.transactionDb, &rocksError);
  if (rocksError)
    {
      setStringError(error, std::string("create checkpoint object: ") +
                            rocksError);
      rocksdb_free(rocksError);
      return false;
    }
  rocksdb_checkpoint_create(checkpoint, path.c_str(), 0, &rocksError);
  rocksdb_checkpoint_object_destroy(checkpoint);
  if (rocksError)
    {
      setStringError(error, std::string("create checkpoint: ") + rocksError);
      rocksdb_free(rocksError);
      return false;
    }
  return true;
}

rocksdb_t *LocalLiteRocksDBOpen(const rocksdb_options_t *options,
                                const char *name,
                                char **error)
{
  (void) options;
  LogicalDb *logical = new LogicalDb();
  if (!name || !logicalPrefixForPath(name, &logical->prefix))
    {
      setRocksError(error, std::string("unsupported LocalLite RocksDB path: ") +
                           (name ? name : "<null>"));
      delete logical;
      return NULL;
    }
  logical->real = unifiedState.baseDb;
  return reinterpret_cast<rocksdb_t *>(logical);
}

void LocalLiteRocksDBClose(rocksdb_t *db)
{
  if (!db)
    return;
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  delete logical;
}

char *LocalLiteRocksDBGet(rocksdb_t *db,
                          const rocksdb_readoptions_t *options,
                          const char *key,
                          size_t keyLength,
                          size_t *valueLength,
                          char **error)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  const std::string storedKey = physicalKey(logical, key, keyLength);
  return rocksdb_get(logical->real, options,
                     storedKey.data(), storedKey.size(),
                     valueLength, error);
}

void LocalLiteRocksDBMultiGet(
    rocksdb_t *db, const rocksdb_readoptions_t *options, size_t keyCount,
    const char *const *keys, const size_t *keyLengths, char **values,
    size_t *valueLengths, char **errors)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  std::vector<std::string> storedKeys(keyCount);
  std::vector<const char *> physicalKeys(keyCount);
  std::vector<size_t> physicalLengths(keyCount);
  for (size_t i = 0; i < keyCount; i++)
    {
      storedKeys[i] = physicalKey(logical, keys[i], keyLengths[i]);
      physicalKeys[i] = storedKeys[i].data();
      physicalLengths[i] = storedKeys[i].size();
    }
  rocksdb_multi_get(logical->real, options, keyCount,
                    physicalKeys.empty() ? NULL : &physicalKeys[0],
                    physicalLengths.empty() ? NULL : &physicalLengths[0],
                    values, valueLengths, errors);
}

void LocalLiteRocksDBPut(rocksdb_t *db,
                         const rocksdb_writeoptions_t *options,
                         const char *key,
                         size_t keyLength,
                         const char *value,
                         size_t valueLength,
                         char **error)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  const std::string storedKey = physicalKey(logical, key, keyLength);
  rocksdb_put(logical->real, options, storedKey.data(), storedKey.size(),
              value, valueLength, error);
}

void LocalLiteRocksDBDelete(rocksdb_t *db,
                            const rocksdb_writeoptions_t *options,
                            const char *key,
                            size_t keyLength,
                            char **error)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  const std::string storedKey = physicalKey(logical, key, keyLength);
  rocksdb_delete(logical->real, options,
                 storedKey.data(), storedKey.size(), error);
}

rocksdb_writebatch_t *LocalLiteRocksDBWriteBatchCreate()
{
  return reinterpret_cast<rocksdb_writebatch_t *>(new LogicalBatch());
}

void LocalLiteRocksDBWriteBatchDestroy(rocksdb_writebatch_t *batch)
{
  delete reinterpret_cast<LogicalBatch *>(batch);
}

void LocalLiteRocksDBWriteBatchPut(rocksdb_writebatch_t *batch,
                                   const char *key,
                                   size_t keyLength,
                                   const char *value,
                                   size_t valueLength)
{
  BatchOperation operation;
  operation.erase = false;
  operation.key.assign(key, keyLength);
  operation.value.assign(value, valueLength);
  reinterpret_cast<LogicalBatch *>(batch)->operations.push_back(operation);
}

void LocalLiteRocksDBWriteBatchDelete(rocksdb_writebatch_t *batch,
                                      const char *key,
                                      size_t keyLength)
{
  BatchOperation operation;
  operation.erase = true;
  operation.key.assign(key, keyLength);
  reinterpret_cast<LogicalBatch *>(batch)->operations.push_back(operation);
}

void LocalLiteRocksDBWrite(rocksdb_t *db,
                           const rocksdb_writeoptions_t *options,
                           rocksdb_writebatch_t *batch,
                           char **error)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  LogicalBatch *logicalBatch = reinterpret_cast<LogicalBatch *>(batch);
  rocksdb_writebatch_t *physical = rocksdb_writebatch_create();
  for (size_t i = 0; i < logicalBatch->operations.size(); i++)
    {
      const BatchOperation &operation = logicalBatch->operations[i];
      const std::string key = physicalKey(
          logical, operation.key.data(), operation.key.size());
      if (operation.erase)
        rocksdb_writebatch_delete(physical, key.data(), key.size());
      else
        rocksdb_writebatch_put(physical, key.data(), key.size(),
                               operation.value.data(), operation.value.size());
    }
  rocksdb_write(logical->real, options, physical, error);
  rocksdb_writebatch_destroy(physical);
}

rocksdb_iterator_t *LocalLiteRocksDBCreateIterator(
    rocksdb_t *db, const rocksdb_readoptions_t *options)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  LogicalIterator *iterator = new LogicalIterator();
  iterator->real = rocksdb_create_iterator(logical->real, options);
  iterator->prefix = logical->prefix;
  return reinterpret_cast<rocksdb_iterator_t *>(iterator);
}

void LocalLiteRocksDBIteratorDestroy(rocksdb_iterator_t *iterator)
{
  if (!iterator)
    return;
  LogicalIterator *logical = reinterpret_cast<LogicalIterator *>(iterator);
  rocksdb_iter_destroy(logical->real);
  delete logical;
}

unsigned char LocalLiteRocksDBIteratorValid(
    const rocksdb_iterator_t *iterator)
{
  const LogicalIterator *logical =
      reinterpret_cast<const LogicalIterator *>(iterator);
  if (!rocksdb_iter_valid(logical->real))
    return 0;
  size_t keyLength = 0;
  const char *key = rocksdb_iter_key(logical->real, &keyLength);
  return hasPrefix(key, keyLength, logical->prefix) ? 1 : 0;
}

void LocalLiteRocksDBIteratorSeekToFirst(rocksdb_iterator_t *iterator)
{
  LogicalIterator *logical = reinterpret_cast<LogicalIterator *>(iterator);
  rocksdb_iter_seek(logical->real,
                    logical->prefix.data(), logical->prefix.size());
}

void LocalLiteRocksDBIteratorSeek(rocksdb_iterator_t *iterator,
                                  const char *key,
                                  size_t keyLength)
{
  LogicalIterator *logical = reinterpret_cast<LogicalIterator *>(iterator);
  const std::string storedKey =
      logical->prefix + hexEncode(std::string(key, keyLength));
  rocksdb_iter_seek(logical->real, storedKey.data(), storedKey.size());
}

void LocalLiteRocksDBIteratorNext(rocksdb_iterator_t *iterator)
{
  rocksdb_iter_next(reinterpret_cast<LogicalIterator *>(iterator)->real);
}

const char *LocalLiteRocksDBIteratorKey(const rocksdb_iterator_t *iterator,
                                        size_t *keyLength)
{
  const LogicalIterator *logical =
      reinterpret_cast<const LogicalIterator *>(iterator);
  size_t storedLength = 0;
  const char *stored = rocksdb_iter_key(logical->real, &storedLength);
  if (!hasPrefix(stored, storedLength, logical->prefix) ||
      !hexDecode(stored + logical->prefix.size(),
                 storedLength - logical->prefix.size(),
                 &logical->decodedKey))
    {
      logical->decodeError = "invalid unified LocalLite logical key";
      *keyLength = 0;
      return "";
    }
  *keyLength = logical->decodedKey.size();
  return logical->decodedKey.data();
}

const char *LocalLiteRocksDBIteratorValue(const rocksdb_iterator_t *iterator,
                                          size_t *valueLength)
{
  const LogicalIterator *logical =
      reinterpret_cast<const LogicalIterator *>(iterator);
  return rocksdb_iter_value(logical->real, valueLength);
}

void LocalLiteRocksDBIteratorGetError(const rocksdb_iterator_t *iterator,
                                      char **error)
{
  const LogicalIterator *logical =
      reinterpret_cast<const LogicalIterator *>(iterator);
  if (!logical->decodeError.empty())
    {
      setRocksError(error, logical->decodeError);
      return;
    }
  rocksdb_iter_get_error(logical->real, error);
}

const rocksdb_snapshot_t *LocalLiteRocksDBCreateSnapshot(rocksdb_t *db)
{
  return rocksdb_create_snapshot(reinterpret_cast<LogicalDb *>(db)->real);
}

void LocalLiteRocksDBReleaseSnapshot(
    rocksdb_t *db, const rocksdb_snapshot_t *snapshot)
{
  rocksdb_release_snapshot(reinterpret_cast<LogicalDb *>(db)->real, snapshot);
}

void LocalLiteRocksDBDestroy(const rocksdb_options_t *options,
                             const char *name,
                             char **error)
{
  (void) options;
  std::string prefix;
  if (!name || !logicalPrefixForPath(name, &prefix))
    {
      setRocksError(error, std::string("unsupported LocalLite RocksDB path: ") +
                           (name ? name : "<null>"));
      return;
    }
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *iterator =
      rocksdb_create_iterator(unifiedState.baseDb, readOptions);
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  for (rocksdb_iter_seek(iterator, prefix.data(), prefix.size());
       rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator))
    {
      size_t keyLength = 0;
      const char *key = rocksdb_iter_key(iterator, &keyLength);
      if (!hasPrefix(key, keyLength, prefix))
        break;
      rocksdb_writebatch_delete(batch, key, keyLength);
    }
  rocksdb_iter_get_error(iterator, error);
  rocksdb_iter_destroy(iterator);
  rocksdb_readoptions_destroy(readOptions);
  if (!error || !*error)
    {
      rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
      rocksdb_writeoptions_set_sync(writeOptions, 1);
      rocksdb_write(unifiedState.baseDb, writeOptions, batch, error);
      rocksdb_writeoptions_destroy(writeOptions);
    }
  rocksdb_writebatch_destroy(batch);
}
