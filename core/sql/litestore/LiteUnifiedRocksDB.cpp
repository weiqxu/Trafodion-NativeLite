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

#define LITE_UNIFIED_ROCKSDB_IMPLEMENTATION
#include "LiteUnifiedRocksDB.h"

#include "LiteStorage.h"
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <pthread.h>
#include <string>
#include <vector>

namespace
{

const char *FORMAT_KEY = "m12/format";
const char *FORMAT_VALUE = "LiteTxnStore/2";
const char *ACTIVE_KEY = "m13/active";
const char *LAYOUT_KEY = "m13/layout";
const char *LAYOUT_VALUE = "unified-hex-v1";
const char *PHYSICAL_WAL_ROOT = "transactiondb-wal";
const char *PHYSICAL_WAL_PENDING_PREFIX = "pending/";
const char *PHYSICAL_COMMIT_MARKER_PREFIX = "m23/commit/";
const uint32_t LITE_DURABLE_SHARDS = 8;

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
      blockCache(NULL), syncWriteOptions(NULL), asyncWriteOptions(NULL),
      nextWalId(1)
  {
    for (uint32_t i = 0; i < LITE_DURABLE_SHARDS; i++)
      walDb[i] = NULL;
  }

  bool prepared;
  bool active;
  std::string root;
  rocksdb_transactiondb_t *transactionDb;
  rocksdb_t *baseDb;
  rocksdb_cache_t *blockCache;
  rocksdb_writeoptions_t *syncWriteOptions;
  rocksdb_writeoptions_t *asyncWriteOptions;
  rocksdb_t *walDb[LITE_DURABLE_SHARDS];
  uint64_t nextWalId;
};

UnifiedState unifiedState;

// A transaction batch is never split across writers: its complete physical
// WriteBatch is routed to exactly one queue, preserving atomicity. Each queue
// also owns a physical WAL intent lane; the unified TransactionDB remains the
// data commit point until cross-shard data transactions have a 2PC protocol.
struct GroupCommitRequest
{
  GroupCommitRequest() : batch(NULL), done(false), passed(false) {}
  LiteUnifiedWriteBatch *batch;
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

GroupCommitState syncGroupCommit[LITE_DURABLE_SHARDS];
GroupCommitState asyncGroupCommit[LITE_DURABLE_SHARDS];

static uint32_t durableShardCount()
{
  const char *setting = getenv("TRAF_LITE_DURABLE_SHARDS");
  if (!setting || !setting[0])
    return LITE_DURABLE_SHARDS;
  char *end = NULL;
  unsigned long parsed = strtoul(setting, &end, 10);
  if (!end || *end != '\0' || parsed == 0)
    return LITE_DURABLE_SHARDS;
  return static_cast<uint32_t>(std::min<unsigned long>(
      parsed, LITE_DURABLE_SHARDS));
}

pthread_mutex_t walIdMutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t allocateWalId()
{
  pthread_mutex_lock(&walIdMutex);
  if (unifiedState.nextWalId == 1)
    {
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      unifiedState.nextWalId = static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
          static_cast<uint64_t>(now.tv_nsec) / 1000ULL;
    }
  const uint64_t id = unifiedState.nextWalId++;
  pthread_mutex_unlock(&walIdMutex);
  return id;
}

static std::string walPendingKey(uint64_t id)
{
  char encoded[32];
  snprintf(encoded, sizeof(encoded), "%s%020llu",
           PHYSICAL_WAL_PENDING_PREFIX,
           static_cast<unsigned long long>(id));
  return std::string(encoded);
}

static std::string physicalCommitMarker(uint64_t id)
{
  char encoded[32];
  snprintf(encoded, sizeof(encoded), "%s%020llu",
           PHYSICAL_COMMIT_MARKER_PREFIX,
           static_cast<unsigned long long>(id));
  return std::string(encoded);
}

static bool isPhysicalWalPendingKey(const std::string &key)
{
  return key.compare(0, strlen(PHYSICAL_WAL_PENDING_PREFIX),
                     PHYSICAL_WAL_PENDING_PREFIX) == 0;
}

static bool parsePhysicalWalId(const std::string &key, uint64_t *id)
{
  if (!id || !isPhysicalWalPendingKey(key))
    return false;
  const char *text = key.c_str() + strlen(PHYSICAL_WAL_PENDING_PREFIX);
  char *end = NULL;
  const unsigned long long parsed = strtoull(text, &end, 10);
  if (!end || *end != '\0')
    return false;
  *id = static_cast<uint64_t>(parsed);
  return true;
}

static uint64_t groupCommitWindowMicros()
{
  const char *setting = getenv("TRAF_LITE_GROUP_COMMIT_WINDOW_US");
  if (!setting || !setting[0])
    return 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(setting, &end, 10);
  return end && *end == '\0' ? static_cast<uint64_t>(parsed) : 0;
}

static bool nativeWalFastPathEnabled()
{
  const char *mode = getenv("TRAF_LITE_PHYSICAL_WAL_MODE");
  if (mode && mode[0])
    return strcmp(mode, "legacy") != 0 && strcmp(mode, "intent") != 0;

  // Keep the Phase 8 fault names meaningful for existing recovery tests. A
  // native commit has no separate intent point, so those tests explicitly use
  // the legacy protocol unless the caller selects native mode.
  const char *fault = getenv("TRAF_LITE_PHYSICAL_WAL_FAULT");
  if (fault && (strcmp(fault, "after-intent") == 0 ||
                strcmp(fault, "after-canonical") == 0))
    return false;
  return true;
}

static std::string nativeWalEnvelope(uint32_t shard,
                                     const std::vector<uint64_t> &ids)
{
  std::string envelope("m23/native-wal/v1|");
  char encoded[64];
  snprintf(encoded, sizeof(encoded), "shard=%u|count=%u|", shard,
           static_cast<unsigned int>(ids.size()));
  envelope.append(encoded);
  for (size_t i = 0; i < ids.size(); i++)
    {
      if (i > 0)
        envelope.push_back(',');
      snprintf(encoded, sizeof(encoded), "%llu",
               static_cast<unsigned long long>(ids[i]));
      envelope.append(encoded);
    }
  return envelope;
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
        ": lite store is already open by another process; use one "
        "sqlci process per TRAF_LITE_STORE_DIR or choose a different "
        "TRAF_LITE_STORE_DIR: " + message);
  else
    setStringError(error, prefix + ": " + message);
}

bool pathExists(const std::string &path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0;
}

bool ensureDirectory(const std::string &path, std::string *error)
{
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST)
    return true;
  setStringError(error, "mkdir " + path + ": " + strerror(errno));
  return false;
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

bool getStorageValue(LiteStorageTxn *txn,
                     const std::string &key,
                     std::string *value,
                     bool *found,
                     LiteStorageStatus *status)
{
  return txn && txn->get(key, value, found, status);
}

bool storageIsEmpty(LiteStorageTxn *txn,
                    bool *empty,
                    LiteStorageStatus *status)
{
  if (!txn || !empty)
    return false;
  LiteStorageCursor *cursor = txn->scan("", "", status);
  if (!cursor)
    return false;
  LiteStorageRecord record;
  bool end = false;
  const bool ok = cursor->next(&record, &end, status);
  delete cursor;
  if (ok)
    *empty = end;
  return ok;
}

bool prepareTarget(const std::string &root, std::string *error)
{
  const std::string targetPath = LiteUnifiedRocksDBPath(root);
  if (pathExists(root + "/catalog") || pathExists(root + "/data"))
    {
      setStringError(error,
          "legacy per-table RocksDB layout is unsupported; remove catalog/ "
          "and data/ before starting this build");
      return false;
    }

  LiteStorageEngine *engine = LiteCreateRocksDBTransactionEngine();
  LiteStorageOptions options;
  options.createIfMissing = true;
  options.synchronousCommit = true;
  const char *minimumFree = getenv("TRAF_LITE_MINIMUM_FREE_BYTES");
  if (minimumFree && minimumFree[0])
    options.minimumFreeBytes = strtoull(minimumFree, NULL, 10);
  LiteStorageStatus status;
  if (!engine->open(targetPath, options, &status))
    {
      setUnifiedOpenError(error, "open Lite unified target",
                          status.message);
      delete engine;
      return false;
    }

  LiteStorageSession *session = engine->createSession(&status);
  LiteStorageTxn *txn = session ? session->begin(&status) : NULL;
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
      LiteStorageStatus ignored;
      txn->rollback(&ignored);
    }
  delete txn;
  delete session;
  if (!ok)
    {
      setStringError(error, "read Lite unified format: " +
                            status.message);
      engine->close();
      delete engine;
      return false;
    }
  if (formatFound && format != FORMAT_VALUE)
    {
      setStringError(error, "unsupported Lite unified format: " + format);
      engine->close();
      delete engine;
      return false;
    }
  if (activeFound && active != "1")
    {
      setStringError(error, "invalid Lite unified activation marker");
      engine->close();
      delete engine;
      return false;
    }
  if (layoutFound && layout != LAYOUT_VALUE)
    {
      setStringError(error, "unsupported Lite unified layout: " + layout);
      engine->close();
      delete engine;
      return false;
    }
  if (activeFound && (!formatFound || !layoutFound))
    {
      setStringError(error, "incomplete active Lite unified format");
      engine->close();
      delete engine;
      return false;
    }
  if (layoutFound && !activeFound)
    {
      setStringError(error, "Lite unified layout is not activated");
      engine->close();
      delete engine;
      return false;
    }
  if (!formatFound && !targetEmpty)
    {
      setStringError(error, "unrecognized non-empty Lite unified target");
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
          setStringError(error, "prepare Lite unified target: " +
                                status.message);
          engine->close();
          delete engine;
          return false;
        }

      const char *fault = getenv("TRAF_LITE_ACTIVATION_FAULT");
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
          setStringError(error, "activate Lite unified target: " +
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

static void closePhysicalWals()
{
  for (uint32_t i = 0; i < LITE_DURABLE_SHARDS; i++)
    if (unifiedState.walDb[i])
      {
        rocksdb_close(unifiedState.walDb[i]);
        unifiedState.walDb[i] = NULL;
      }
}

static bool openPhysicalWals(const std::string &root, std::string *error)
{
  if (!ensureDirectory(root + "/" + PHYSICAL_WAL_ROOT, error))
    return false;
  rocksdb_options_t *options = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(options, 1);
  rocksdb_options_set_paranoid_checks(options, 1);
  for (uint32_t i = 0; i < LITE_DURABLE_SHARDS; i++)
    {
      char suffix[32];
      snprintf(suffix, sizeof(suffix), "/shard-%u", i);
      const std::string path = root + "/" + PHYSICAL_WAL_ROOT + suffix;
      char *rocksError = NULL;
      unifiedState.walDb[i] = rocksdb_open(options, path.c_str(), &rocksError);
      if (rocksError)
        {
          const std::string message(rocksError);
          rocksdb_free(rocksError);
          rocksdb_options_destroy(options);
          closePhysicalWals();
          setUnifiedOpenError(error, "open Lite physical WAL shard",
                              message);
          return false;
        }
    }
  rocksdb_options_destroy(options);
  return true;
}

static bool recoverPhysicalWals(std::string *error)
{
  if (!unifiedState.baseDb || !unifiedState.syncWriteOptions)
    {
      setStringError(error, "Lite physical WAL recovery is unavailable");
      return false;
    }
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  for (uint32_t shard = 0; shard < LITE_DURABLE_SHARDS; shard++)
    {
      rocksdb_iterator_t *iterator =
          rocksdb_create_iterator(unifiedState.walDb[shard], readOptions);
      rocksdb_iter_seek_to_first(iterator);
      std::vector<std::pair<std::string, std::string> > pending;
      while (rocksdb_iter_valid(iterator))
        {
          size_t keyLength = 0;
          size_t valueLength = 0;
          const char *key = rocksdb_iter_key(iterator, &keyLength);
          const char *value = rocksdb_iter_value(iterator, &valueLength);
          const std::string pendingKey(key, keyLength);
          if (isPhysicalWalPendingKey(pendingKey))
            pending.push_back(std::make_pair(
                pendingKey, std::string(value, valueLength)));
          rocksdb_iter_next(iterator);
        }
      char *rocksError = NULL;
      rocksdb_iter_get_error(iterator, &rocksError);
      rocksdb_iter_destroy(iterator);
      if (rocksError)
        {
          setStringError(error, std::string("scan Lite physical WAL: ") +
                                rocksError);
          rocksdb_free(rocksError);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }

      for (std::vector<std::pair<std::string, std::string> >::const_iterator it =
               pending.begin(); it != pending.end(); ++it)
        {
          uint64_t id = 0;
          if (!parsePhysicalWalId(it->first, &id))
            {
              setStringError(error, "invalid Lite physical WAL intent key");
              rocksdb_readoptions_destroy(readOptions);
              return false;
            }
          rocksdb_writebatch_t *batch = rocksdb_writebatch_create_from(
              it->second.data(), it->second.size());
          if (!batch)
            {
              setStringError(error,
                             "invalid Lite physical WAL intent batch");
              rocksdb_readoptions_destroy(readOptions);
              return false;
            }
          const std::string marker = physicalCommitMarker(id);
          size_t markerLength = 0;
          char *markerError = NULL;
          char *markerValue = rocksdb_get(unifiedState.baseDb, readOptions,
                                          marker.data(), marker.size(),
                                          &markerLength, &markerError);
          if (markerError)
            {
              setStringError(error,
                             std::string("check Lite physical WAL "
                                         "commit marker: ") + markerError);
              rocksdb_free(markerError);
              rocksdb_writebatch_destroy(batch);
              rocksdb_readoptions_destroy(readOptions);
              return false;
            }
          if (!markerValue)
            {
              char *writeError = NULL;
              rocksdb_write(unifiedState.baseDb,
                            unifiedState.syncWriteOptions, batch, &writeError);
              if (writeError)
                {
                  setStringError(error,
                                 std::string("replay Lite physical WAL: ") +
                                     writeError);
                  rocksdb_free(writeError);
                  rocksdb_writebatch_destroy(batch);
                  rocksdb_readoptions_destroy(readOptions);
                  return false;
                }
            }
          else
            rocksdb_free(markerValue);
          rocksdb_writebatch_destroy(batch);

          rocksdb_writebatch_t *remove = rocksdb_writebatch_create();
          rocksdb_writebatch_delete(remove, it->first.data(), it->first.size());
          char *removeError = NULL;
          rocksdb_write(unifiedState.walDb[shard],
                        unifiedState.syncWriteOptions, remove, &removeError);
          rocksdb_writebatch_destroy(remove);
          if (removeError)
            {
              setStringError(error,
                             std::string("remove Lite physical WAL "
                                         "intent: ") + removeError);
              rocksdb_free(removeError);
              rocksdb_readoptions_destroy(readOptions);
              return false;
            }
        }
    }
  rocksdb_readoptions_destroy(readOptions);
  return true;
}

bool openUnifiedTarget(const std::string &root, std::string *error)
{
  rocksdb_options_t *options = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(options, 0);
  rocksdb_options_set_paranoid_checks(options, 1);
  const char *cacheText = getenv("TRAF_LITE_BLOCK_CACHE_BYTES");
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
      LiteUnifiedRocksDBPath(root).c_str(), &rocksError);
  rocksdb_transactiondb_options_destroy(transactionOptions);
  rocksdb_options_destroy(options);
  if (rocksError)
    {
      const std::string message(rocksError);
      rocksdb_free(rocksError);
      setUnifiedOpenError(error, "open active Lite TransactionDB",
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
      setStringError(error, "get active Lite TransactionDB base handle");
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
  if (!openPhysicalWals(root, error) || !recoverPhysicalWals(error))
    {
      closePhysicalWals();
      rocksdb_transactiondb_close_base_db(unifiedState.baseDb);
      unifiedState.baseDb = NULL;
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

struct LiteUnifiedWriteBatch
{
  LiteUnifiedWriteBatch()
    : physical(rocksdb_writebatch_create()),
      shardHash(1469598103934665603ULL), walId(0)
  {}
  ~LiteUnifiedWriteBatch() { rocksdb_writebatch_destroy(physical); }
  rocksdb_writebatch_t *physical;
  uint64_t shardHash;
  uint64_t walId;
};

static uint64_t batchKeyHash(const std::string &key)
{
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < key.size(); i++)
    {
      hash ^= static_cast<unsigned char>(key[i]);
      hash *= 1099511628211ULL;
    }
  return hash;
}

static void mixBatchShard(LiteUnifiedWriteBatch *batch,
                          const std::string &key)
{
  const uint64_t hash = batchKeyHash(key);
  batch->shardHash ^= hash + 0x9e3779b97f4a7c15ULL +
      (batch->shardHash << 6) + (batch->shardHash >> 2);
}

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
                       uint32_t shard, bool sync, std::string *error)
{
  const bool native = nativeWalFastPathEnabled();
  rocksdb_writebatch_t *combined = rocksdb_writebatch_create();
  rocksdb_writebatch_t *intents = native ? NULL : rocksdb_writebatch_create();
  GroupBatchCopy copy;
  copy.target = combined;
  std::vector<uint64_t> walIds;
  for (std::vector<GroupCommitRequest *>::const_iterator request =
           requests.begin(); request != requests.end(); ++request)
    {
      LiteUnifiedWriteBatch *batch = (*request)->batch;
      if (batch->walId == 0)
        batch->walId = allocateWalId();
      walIds.push_back(batch->walId);

      if (native)
        {
          // The native path deliberately copies only business operations. A
          // single group-level PutLogData record is appended below so the
          // metadata and all data share one unified WAL write.
          rocksdb_writebatch_iterate(batch->physical, &copy, copyGroupPut,
                                     copyGroupDelete);
          continue;
        }
      size_t serializedSize = 0;
      const char *serialized = rocksdb_writebatch_data(batch->physical,
                                                       &serializedSize);
      rocksdb_writebatch_t *withMarker = rocksdb_writebatch_create_from(
          serialized, serializedSize);
      if (!withMarker)
        {
          setStringError(error,
                         "create Lite physical WAL intent batch failed");
          if (intents)
            rocksdb_writebatch_destroy(intents);
          rocksdb_writebatch_destroy(combined);
          return false;
        }
      const std::string marker = physicalCommitMarker(batch->walId);
      rocksdb_writebatch_put(withMarker, marker.data(), marker.size(), "", 0);
      size_t intentSize = 0;
      const char *intentData = rocksdb_writebatch_data(withMarker, &intentSize);
      const std::string intentKey = walPendingKey(batch->walId);
      // The legacy intent database stores the serialized batch under a
      // pending key. The native path never reaches this branch.
      rocksdb_writebatch_put(intents, intentKey.data(), intentKey.size(),
                             intentData, intentSize);
      rocksdb_writebatch_iterate(withMarker, &copy, copyGroupPut,
                                 copyGroupDelete);
      rocksdb_writebatch_destroy(withMarker);
    }

  rocksdb_writeoptions_t *options = sync
      ? unifiedState.syncWriteOptions : unifiedState.asyncWriteOptions;
  if (!options || shard >= LITE_DURABLE_SHARDS || !unifiedState.baseDb ||
      (!native && !unifiedState.walDb[shard]))
    {
      setStringError(error, "Lite physical WAL writer is unavailable");
      if (intents)
        rocksdb_writebatch_destroy(intents);
      rocksdb_writebatch_destroy(combined);
      return false;
    }

  char *rocksError = NULL;
  const char *nativeFault = getenv("TRAF_LITE_NATIVE_WAL_FAULT");
  if (native && nativeFault && strcmp(nativeFault, "before-wal") == 0)
    _exit(94);
  if (!native)
    {
      rocksdb_write(unifiedState.walDb[shard], options, intents, &rocksError);
      rocksdb_writebatch_destroy(intents);
      intents = NULL;
      if (rocksError)
        {
          setStringError(error, std::string("write Lite physical WAL: ") +
                                rocksError);
          rocksdb_free(rocksError);
          rocksdb_writebatch_destroy(combined);
          return false;
        }
    }
  if (native)
    {
      const std::string envelope = nativeWalEnvelope(shard, walIds);
      rocksdb_writebatch_put_log_data(combined, envelope.data(),
                                      envelope.size());
    }

  if (!native)
    {
      // Legacy intent writes happen while batches are assembled above. The
      // canonical write below is still the atomic data commit point.
      const char *walFault = getenv("TRAF_LITE_PHYSICAL_WAL_FAULT");
      if (walFault && strcmp(walFault, "after-intent") == 0)
        _exit(94);
    }

  rocksError = NULL;
  rocksdb_write(unifiedState.baseDb, options, combined, &rocksError);
  rocksdb_writebatch_destroy(combined);
  if (rocksError)
    {
      setStringError(error, std::string("commit Lite unified batch: ") +
                            rocksError);
      rocksdb_free(rocksError);
      return false;
    }
  const char *walFault = getenv("TRAF_LITE_PHYSICAL_WAL_FAULT");
  if ((walFault && strcmp(walFault, "after-canonical") == 0) ||
      (nativeFault && strcmp(nativeFault, "after-canonical") == 0))
    _exit(95);

  if (native)
    return true;

  rocksdb_writebatch_t *removals = rocksdb_writebatch_create();
  for (std::vector<GroupCommitRequest *>::const_iterator request =
           requests.begin(); request != requests.end(); ++request)
    {
      const std::string intentKey = walPendingKey((*request)->batch->walId);
      rocksdb_writebatch_delete(removals, intentKey.data(), intentKey.size());
    }
  rocksError = NULL;
  rocksdb_write(unifiedState.walDb[shard], options, removals, &rocksError);
  rocksdb_writebatch_destroy(removals);
  if (rocksError)
    {
      // The commit marker makes cleanup/replay idempotent on the next open.
      setStringError(error, std::string("remove Lite physical WAL: ") +
                            rocksError);
      rocksdb_free(rocksError);
      return false;
    }
  return true;
}

std::string LiteUnifiedRocksDBPath(const std::string &root)
{
  return root + "/transactiondb";
}

bool LiteUnifiedRocksDBPrepare(const std::string &root,
                                    std::string *error)
{
  if (unifiedState.prepared)
    return true;
  unifiedState.prepared = true;
  unifiedState.root = root;
  if (!prepareTarget(root, error))
    {
      LiteUnifiedRocksDBShutdown();
      return false;
    }
  if (!openUnifiedTarget(root, error))
    {
      LiteUnifiedRocksDBShutdown();
      return false;
    }
  return true;
}

void LiteUnifiedRocksDBShutdown()
{
  closePhysicalWals();
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

bool LiteUnifiedRocksDBActive()
{
  return unifiedState.active;
}

uint64_t LiteUnifiedRocksDBSequence()
{
  return unifiedState.baseDb
      ? rocksdb_get_latest_sequence_number(unifiedState.baseDb) : 0;
}

uint32_t LiteUnifiedDurableShardCount()
{
  return durableShardCount();
}

uint32_t LiteUnifiedPhysicalWalShardCount()
{
  for (uint32_t i = 0; i < LITE_DURABLE_SHARDS; i++)
    if (!unifiedState.walDb[i])
      return i;
  return LITE_DURABLE_SHARDS;
}

bool LiteUnifiedNativeWalFastPathEnabled()
{
  return nativeWalFastPathEnabled();
}

LiteUnifiedWriteBatch *LiteUnifiedWriteBatchCreate()
{
  return new LiteUnifiedWriteBatch();
}

void LiteUnifiedWriteBatchDestroy(LiteUnifiedWriteBatch *batch)
{
  delete batch;
}

void LiteUnifiedWriteBatchPut(LiteUnifiedWriteBatch *batch,
                                   rocksdb_t *logicalDb,
                                   const std::string &key,
                                   const std::string &value)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(logicalDb);
  const std::string storedKey = physicalKey(logical, key.data(), key.size());
  mixBatchShard(batch, storedKey);
  rocksdb_writebatch_put(batch->physical, storedKey.data(), storedKey.size(),
                         value.data(), value.size());
}

void LiteUnifiedWriteBatchDelete(LiteUnifiedWriteBatch *batch,
                                      rocksdb_t *logicalDb,
                                      const std::string &key)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(logicalDb);
  const std::string storedKey = physicalKey(logical, key.data(), key.size());
  mixBatchShard(batch, storedKey);
  rocksdb_writebatch_delete(batch->physical, storedKey.data(),
                            storedKey.size());
}

bool LiteUnifiedWriteBatchCommit(LiteUnifiedWriteBatch *batch,
                                      bool sync,
                                      std::string *error)
{
  if (!batch || !unifiedState.baseDb)
    {
      setStringError(error, "Lite unified write batch is unavailable");
      return false;
    }
  const uint32_t shard = static_cast<uint32_t>(
      batch->shardHash % durableShardCount());
  GroupCommitState &group = sync ? syncGroupCommit[shard]
                                 : asyncGroupCommit[shard];
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
      const bool passed = writeGroup(requests, shard, sync, &groupError);

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

bool LiteUnifiedRocksDBCheckpoint(const std::string &path,
                                       std::string *error)
{
  if (!unifiedState.transactionDb || path.empty())
    {
      setStringError(error, "Lite checkpoint target is unavailable");
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

rocksdb_t *LiteRocksDBOpen(const rocksdb_options_t *options,
                                const char *name,
                                char **error)
{
  (void) options;
  LogicalDb *logical = new LogicalDb();
  if (!name || !logicalPrefixForPath(name, &logical->prefix))
    {
      setRocksError(error, std::string("unsupported Lite RocksDB path: ") +
                           (name ? name : "<null>"));
      delete logical;
      return NULL;
    }
  logical->real = unifiedState.baseDb;
  return reinterpret_cast<rocksdb_t *>(logical);
}

void LiteRocksDBClose(rocksdb_t *db)
{
  if (!db)
    return;
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  delete logical;
}

char *LiteRocksDBGet(rocksdb_t *db,
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

void LiteRocksDBMultiGet(
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

void LiteRocksDBPut(rocksdb_t *db,
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

void LiteRocksDBDelete(rocksdb_t *db,
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

rocksdb_writebatch_t *LiteRocksDBWriteBatchCreate()
{
  return reinterpret_cast<rocksdb_writebatch_t *>(new LogicalBatch());
}

void LiteRocksDBWriteBatchDestroy(rocksdb_writebatch_t *batch)
{
  delete reinterpret_cast<LogicalBatch *>(batch);
}

void LiteRocksDBWriteBatchPut(rocksdb_writebatch_t *batch,
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

void LiteRocksDBWriteBatchDelete(rocksdb_writebatch_t *batch,
                                      const char *key,
                                      size_t keyLength)
{
  BatchOperation operation;
  operation.erase = true;
  operation.key.assign(key, keyLength);
  reinterpret_cast<LogicalBatch *>(batch)->operations.push_back(operation);
}

void LiteRocksDBWrite(rocksdb_t *db,
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

rocksdb_iterator_t *LiteRocksDBCreateIterator(
    rocksdb_t *db, const rocksdb_readoptions_t *options)
{
  LogicalDb *logical = reinterpret_cast<LogicalDb *>(db);
  LogicalIterator *iterator = new LogicalIterator();
  iterator->real = rocksdb_create_iterator(logical->real, options);
  iterator->prefix = logical->prefix;
  return reinterpret_cast<rocksdb_iterator_t *>(iterator);
}

void LiteRocksDBIteratorDestroy(rocksdb_iterator_t *iterator)
{
  if (!iterator)
    return;
  LogicalIterator *logical = reinterpret_cast<LogicalIterator *>(iterator);
  rocksdb_iter_destroy(logical->real);
  delete logical;
}

unsigned char LiteRocksDBIteratorValid(
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

void LiteRocksDBIteratorSeekToFirst(rocksdb_iterator_t *iterator)
{
  LogicalIterator *logical = reinterpret_cast<LogicalIterator *>(iterator);
  rocksdb_iter_seek(logical->real,
                    logical->prefix.data(), logical->prefix.size());
}

void LiteRocksDBIteratorSeek(rocksdb_iterator_t *iterator,
                                  const char *key,
                                  size_t keyLength)
{
  LogicalIterator *logical = reinterpret_cast<LogicalIterator *>(iterator);
  const std::string storedKey =
      logical->prefix + hexEncode(std::string(key, keyLength));
  rocksdb_iter_seek(logical->real, storedKey.data(), storedKey.size());
}

void LiteRocksDBIteratorNext(rocksdb_iterator_t *iterator)
{
  rocksdb_iter_next(reinterpret_cast<LogicalIterator *>(iterator)->real);
}

const char *LiteRocksDBIteratorKey(const rocksdb_iterator_t *iterator,
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
      logical->decodeError = "invalid unified Lite logical key";
      *keyLength = 0;
      return "";
    }
  *keyLength = logical->decodedKey.size();
  return logical->decodedKey.data();
}

const char *LiteRocksDBIteratorValue(const rocksdb_iterator_t *iterator,
                                          size_t *valueLength)
{
  const LogicalIterator *logical =
      reinterpret_cast<const LogicalIterator *>(iterator);
  return rocksdb_iter_value(logical->real, valueLength);
}

void LiteRocksDBIteratorGetError(const rocksdb_iterator_t *iterator,
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

const rocksdb_snapshot_t *LiteRocksDBCreateSnapshot(rocksdb_t *db)
{
  return rocksdb_create_snapshot(reinterpret_cast<LogicalDb *>(db)->real);
}

void LiteRocksDBReleaseSnapshot(
    rocksdb_t *db, const rocksdb_snapshot_t *snapshot)
{
  rocksdb_release_snapshot(reinterpret_cast<LogicalDb *>(db)->real, snapshot);
}

void LiteRocksDBDestroy(const rocksdb_options_t *options,
                             const char *name,
                             char **error)
{
  (void) options;
  std::string prefix;
  if (!name || !logicalPrefixForPath(name, &prefix))
    {
      setRocksError(error, std::string("unsupported Lite RocksDB path: ") +
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
