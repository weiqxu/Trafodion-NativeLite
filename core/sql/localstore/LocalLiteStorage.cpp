// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@

#include "LocalLiteStorage.h"

#include <rocksdb/c.h>

#include <pthread.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cstring>

namespace
{

void runStorageFault(const char *point, int exitCode)
{
  const char *fault = getenv("TRAF_LOCAL_LITE_STORAGE_FAULT");
  if (fault && std::string(fault) == point)
    _exit(exitCode);
}

void clearStatus(LocalLiteStorageStatus *status)
{
  if (!status)
    return;
  status->code = LOCAL_LITE_STORAGE_OK;
  status->retryable = false;
  status->message.clear();
}

void setStatus(LocalLiteStorageStatus *status,
               LocalLiteStorageCode code,
               const std::string &message,
               bool retryable = false)
{
  if (!status)
    return;
  status->code = code;
  status->retryable = retryable;
  status->message = message;
}

LocalLiteStorageCode classifyRocksError(const std::string &message,
                                        bool *retryable)
{
  *retryable = false;
  if (message.find("Busy") != std::string::npos ||
      message.find("TimedOut") != std::string::npos ||
      message.find("lock") != std::string::npos ||
      message.find("Conflict") != std::string::npos)
    {
      *retryable = true;
      return LOCAL_LITE_STORAGE_CONFLICT;
    }
  if (message.find("No space") != std::string::npos ||
      message.find("no space") != std::string::npos)
    return LOCAL_LITE_STORAGE_NO_SPACE;
  if (message.find("Corruption") != std::string::npos ||
      message.find("corruption") != std::string::npos)
    return LOCAL_LITE_STORAGE_CORRUPTION;
  if (message.find("Invalid argument") != std::string::npos)
    return LOCAL_LITE_STORAGE_INVALID;
  return LOCAL_LITE_STORAGE_IO_ERROR;
}

bool consumeRocksError(char *error,
                       const char *operation,
                       LocalLiteStorageStatus *status)
{
  if (!error)
    {
      clearStatus(status);
      return true;
    }

  std::string message(operation);
  message += ": ";
  message += error;
  rocksdb_free(error);
  bool retryable = false;
  const LocalLiteStorageCode code = classifyRocksError(message, &retryable);
  setStatus(status, code, message, retryable);
  return false;
}

class MutexGuard
{
public:
  explicit MutexGuard(pthread_mutex_t *mutex) : mutex_(mutex)
  {
    pthread_mutex_lock(mutex_);
  }
  ~MutexGuard() { pthread_mutex_unlock(mutex_); }

private:
  MutexGuard(const MutexGuard &);
  MutexGuard &operator=(const MutexGuard &);
  pthread_mutex_t *mutex_;
};

class RocksDBTransactionEngine;

class RocksDBCursor : public LocalLiteStorageCursor
{
public:
  RocksDBCursor(rocksdb_iterator_t *iterator,
                const std::string &beginKey,
                const std::string &endKey,
                RocksDBTransactionEngine *engine)
    : iterator_(iterator), endKey_(endKey), engine_(engine), cancelled_(false)
  {
    rocksdb_iter_seek(iterator_, beginKey.data(), beginKey.size());
  }

  virtual ~RocksDBCursor()
  {
    if (iterator_)
      rocksdb_iter_destroy(iterator_);
  }

  virtual bool next(LocalLiteStorageRecord *record,
                    bool *end,
                    LocalLiteStorageStatus *status);
  virtual void cancel() { cancelled_ = true; }

private:
  RocksDBCursor(const RocksDBCursor &);
  RocksDBCursor &operator=(const RocksDBCursor &);

  rocksdb_iterator_t *iterator_;
  std::string endKey_;
  RocksDBTransactionEngine *engine_;
  bool cancelled_;
};

class RocksDBTransaction;

class RocksDBSession : public LocalLiteStorageSession
{
public:
  explicit RocksDBSession(RocksDBTransactionEngine *engine)
    : engine_(engine)
  {
  }

  virtual LocalLiteStorageTxn *begin(LocalLiteStorageStatus *status);

private:
  RocksDBTransactionEngine *engine_;
};

class RocksDBTransactionEngine : public LocalLiteStorageEngine
{
  friend class RocksDBCursor;
  friend class RocksDBSession;
  friend class RocksDBTransaction;

public:
  RocksDBTransactionEngine()
    : db_(NULL), opened_(false)
  {
    pthread_mutex_init(&metricsMutex_, NULL);
  }

  virtual ~RocksDBTransactionEngine()
  {
    close();
    pthread_mutex_destroy(&metricsMutex_);
  }

  virtual const char *name() const { return "rocksdb-transactiondb"; }

  virtual bool open(const std::string &path,
                    const LocalLiteStorageOptions &options,
                    LocalLiteStorageStatus *status)
  {
    if (opened_)
      {
        setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                  "storage engine is already open");
        return false;
      }
    if (path.empty())
      {
        setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                  "storage path is empty");
        return false;
      }

    rocksdb_options_t *dbOptions = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(dbOptions,
                                          options.createIfMissing ? 1 : 0);
    rocksdb_options_set_paranoid_checks(dbOptions, 1);
    rocksdb_transactiondb_options_t *txnDbOptions =
        rocksdb_transactiondb_options_create();
    rocksdb_transactiondb_options_set_default_lock_timeout(
        txnDbOptions, options.lockTimeoutMillis);
    char *error = NULL;
    db_ = rocksdb_transactiondb_open(dbOptions, txnDbOptions,
                                     path.c_str(), &error);
    rocksdb_transactiondb_options_destroy(txnDbOptions);
    rocksdb_options_destroy(dbOptions);
    if (!consumeRocksError(error, "open RocksDB TransactionDB", status))
      {
        db_ = NULL;
        return false;
      }

    path_ = path;
    options_ = options;
    opened_ = true;
    clearStatus(status);
    return true;
  }

  virtual void close()
  {
    if (!opened_)
      return;
    rocksdb_transactiondb_close(db_);
    db_ = NULL;
    opened_ = false;
    path_.clear();
  }

  virtual LocalLiteStorageSession *createSession(
      LocalLiteStorageStatus *status)
  {
    if (!opened_)
      {
        setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                  "storage engine is not open");
        return NULL;
      }
    clearStatus(status);
    return new RocksDBSession(this);
  }

  virtual bool checkpoint(const std::string &path,
                          LocalLiteStorageStatus *status)
  {
    if (!requireOpen(status) || path.empty())
      {
        if (path.empty())
          setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                    "checkpoint path is empty");
        return false;
      }
    runStorageFault("checkpoint", 88);
    char *error = NULL;
    rocksdb_checkpoint_t *checkpoint =
        rocksdb_transactiondb_checkpoint_object_create(db_, &error);
    if (!consumeRocksError(error, "create checkpoint object", status))
      return false;
    rocksdb_checkpoint_create(checkpoint, path.c_str(), 0, &error);
    rocksdb_checkpoint_object_destroy(checkpoint);
    return consumeRocksError(error, "create checkpoint", status);
  }

  virtual bool backup(const std::string &path,
                      LocalLiteStorageStatus *status)
  {
    if (!requireOpen(status) || path.empty())
      {
        if (path.empty())
          setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                    "backup path is empty");
        return false;
      }
    runStorageFault("backup", 89);
    char *error = NULL;
    rocksdb_options_t *backupOptions = rocksdb_options_create();
    rocksdb_backup_engine_t *backupEngine =
        rocksdb_backup_engine_open(backupOptions, path.c_str(), &error);
    rocksdb_options_destroy(backupOptions);
    if (!consumeRocksError(error, "open backup engine", status))
      return false;
    rocksdb_t *base = rocksdb_transactiondb_get_base_db(db_);
    rocksdb_backup_engine_create_new_backup_flush(backupEngine, base, 1,
                                                  &error);
    rocksdb_transactiondb_close_base_db(base);
    rocksdb_backup_engine_close(backupEngine);
    return consumeRocksError(error, "create consistent backup", status);
  }

  virtual bool restore(const std::string &backupPath,
                       const std::string &restorePath,
                       LocalLiteStorageStatus *status)
  {
    if (backupPath.empty() || restorePath.empty())
      {
        setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                  "backup or restore path is empty");
        return false;
      }
    rocksdb_options_t *backupOptions = rocksdb_options_create();
    char *error = NULL;
    rocksdb_backup_engine_t *backupEngine =
        rocksdb_backup_engine_open(backupOptions, backupPath.c_str(), &error);
    rocksdb_options_destroy(backupOptions);
    if (!consumeRocksError(error, "open backup for restore", status))
      return false;
    rocksdb_restore_options_t *restoreOptions =
        rocksdb_restore_options_create();
    rocksdb_backup_engine_restore_db_from_latest_backup(
        backupEngine, restorePath.c_str(), restorePath.c_str(),
        restoreOptions, &error);
    rocksdb_restore_options_destroy(restoreOptions);
    rocksdb_backup_engine_close(backupEngine);
    return consumeRocksError(error, "restore backup", status);
  }

  virtual bool verify(LocalLiteStorageStatus *status)
  {
    if (!requireOpen(status))
      return false;
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    rocksdb_iterator_t *iterator =
        rocksdb_transactiondb_create_iterator(db_, readOptions);
    rocksdb_iter_seek_to_first(iterator);
    while (rocksdb_iter_valid(iterator))
      rocksdb_iter_next(iterator);
    char *error = NULL;
    rocksdb_iter_get_error(iterator, &error);
    rocksdb_iter_destroy(iterator);
    rocksdb_readoptions_destroy(readOptions);
    return consumeRocksError(error, "verify storage key space", status);
  }

  virtual LocalLiteStorageMetrics metrics() const
  {
    MutexGuard guard(&metricsMutex_);
    LocalLiteStorageMetrics result = metrics_;
    if (db_)
      {
        rocksdb_transactiondb_property_int(
            db_, "rocksdb.estimate-num-keys", &result.estimatedKeys);
        rocksdb_transactiondb_property_int(
            db_, "rocksdb.estimate-live-data-size", &result.liveDataBytes);
      }
    return result;
  }

private:
  bool requireOpen(LocalLiteStorageStatus *status) const
  {
    if (opened_)
      return true;
    setStatus(status, LOCAL_LITE_STORAGE_INVALID,
              "storage engine is not open");
    return false;
  }

  bool checkDiskWatermark(LocalLiteStorageStatus *status) const
  {
    if (options_.minimumFreeBytes == 0)
      return true;
    struct statvfs info;
    if (statvfs(path_.c_str(), &info) != 0)
      {
        setStatus(status, LOCAL_LITE_STORAGE_IO_ERROR,
                  "read storage filesystem capacity failed");
        return false;
      }
    const uint64_t available =
        static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
    if (available < options_.minimumFreeBytes)
      {
        setStatus(status, LOCAL_LITE_STORAGE_NO_SPACE,
                  "storage disk watermark reached");
        return false;
      }
    return true;
  }

  void addRead(uint64_t bytes)
  {
    MutexGuard guard(&metricsMutex_);
    metrics_.bytesRead += bytes;
  }
  void addWrite(uint64_t bytes)
  {
    MutexGuard guard(&metricsMutex_);
    metrics_.bytesWritten += bytes;
  }
  void transactionCommitted()
  {
    MutexGuard guard(&metricsMutex_);
    metrics_.committedTransactions++;
  }
  void transactionRolledBack()
  {
    MutexGuard guard(&metricsMutex_);
    metrics_.rolledBackTransactions++;
  }
  void transactionCancelled()
  {
    MutexGuard guard(&metricsMutex_);
    metrics_.cancelledTransactions++;
  }
  void transactionConflict()
  {
    MutexGuard guard(&metricsMutex_);
    metrics_.conflicts++;
  }

  rocksdb_transactiondb_t *db_;
  bool opened_;
  std::string path_;
  LocalLiteStorageOptions options_;
  mutable pthread_mutex_t metricsMutex_;
  LocalLiteStorageMetrics metrics_;
};

class RocksDBTransaction : public LocalLiteStorageTxn
{
public:
  RocksDBTransaction(RocksDBTransactionEngine *engine,
                     rocksdb_transaction_t *transaction)
    : engine_(engine), transaction_(transaction), active_(true),
      cancelled_(false), snapshot_(rocksdb_transaction_get_snapshot(transaction))
  {
  }

  virtual ~RocksDBTransaction()
  {
    if (active_)
      {
        char *error = NULL;
        rocksdb_transaction_rollback(transaction_, &error);
        if (error)
          rocksdb_free(error);
        engine_->transactionRolledBack();
      }
    if (snapshot_)
      rocksdb_free(const_cast<rocksdb_snapshot_t *>(snapshot_));
    rocksdb_transaction_destroy(transaction_);
  }

  virtual bool get(const std::string &key,
                   std::string *value,
                   bool *found,
                   LocalLiteStorageStatus *status)
  {
    if (!checkActive(status) || !value || !found)
      {
        if (!value || !found)
          setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                    "storage get output is missing");
        return false;
      }
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    if (snapshot_)
      rocksdb_readoptions_set_snapshot(readOptions, snapshot_);
    char *error = NULL;
    size_t valueLength = 0;
    char *raw = rocksdb_transaction_get(transaction_, readOptions,
                                        key.data(), key.size(),
                                        &valueLength, &error);
    rocksdb_readoptions_destroy(readOptions);
    if (!consumeRocksError(error, "transaction get", status))
      return false;
    *found = raw != NULL;
    if (raw)
      {
        value->assign(raw, valueLength);
        rocksdb_free(raw);
        engine_->addRead(key.size() + valueLength);
      }
    else
      value->clear();
    return true;
  }

  virtual bool put(const std::string &key,
                   const std::string &value,
                   LocalLiteStorageStatus *status)
  {
    if (!checkActive(status) || !engine_->checkDiskWatermark(status))
      return false;
    char *error = NULL;
    rocksdb_transaction_put(transaction_, key.data(), key.size(),
                            value.data(), value.size(), &error);
    if (!consumeRocksError(error, "transaction put", status))
      {
        if (status && status->code == LOCAL_LITE_STORAGE_CONFLICT)
          engine_->transactionConflict();
        return false;
      }
    engine_->addWrite(key.size() + value.size());
    return true;
  }

  virtual bool erase(const std::string &key,
                     LocalLiteStorageStatus *status)
  {
    if (!checkActive(status) || !engine_->checkDiskWatermark(status))
      return false;
    char *error = NULL;
    rocksdb_transaction_delete(transaction_, key.data(), key.size(), &error);
    if (!consumeRocksError(error, "transaction delete", status))
      {
        if (status && status->code == LOCAL_LITE_STORAGE_CONFLICT)
          engine_->transactionConflict();
        return false;
      }
    engine_->addWrite(key.size());
    return true;
  }

  virtual LocalLiteStorageCursor *scan(const std::string &beginKey,
                                       const std::string &endKey,
                                       LocalLiteStorageStatus *status)
  {
    if (!checkActive(status) ||
        (!endKey.empty() && endKey <= beginKey))
      {
        if (!endKey.empty() && endKey <= beginKey)
          setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                    "storage scan range is invalid");
        return NULL;
      }
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    if (snapshot_)
      rocksdb_readoptions_set_snapshot(readOptions, snapshot_);
    rocksdb_iterator_t *iterator =
        rocksdb_transaction_create_iterator(transaction_, readOptions);
    rocksdb_readoptions_destroy(readOptions);
    if (!iterator)
      {
        setStatus(status, LOCAL_LITE_STORAGE_IO_ERROR,
                  "create transaction cursor failed");
        return NULL;
      }
    clearStatus(status);
    return new RocksDBCursor(iterator, beginKey, endKey, engine_);
  }

  virtual bool commit(LocalLiteStorageStatus *status)
  {
    if (!checkActive(status) || !engine_->checkDiskWatermark(status))
      return false;
    runStorageFault("before-commit", 86);
    char *error = NULL;
    rocksdb_transaction_commit(transaction_, &error);
    if (!consumeRocksError(error, "commit transaction", status))
      {
        if (status && status->code == LOCAL_LITE_STORAGE_CONFLICT)
          engine_->transactionConflict();
        return false;
      }
    if (engine_->options_.synchronousCommit)
      {
        rocksdb_transactiondb_flush_wal(engine_->db_, 1, &error);
        if (!consumeRocksError(error, "synchronously flush transaction WAL",
                               status))
          return false;
      }
    active_ = false;
    engine_->transactionCommitted();
    runStorageFault("after-commit", 87);
    return true;
  }

  virtual bool rollback(LocalLiteStorageStatus *status)
  {
    if (!checkActive(status))
      return false;
    char *error = NULL;
    rocksdb_transaction_rollback(transaction_, &error);
    if (!consumeRocksError(error, "rollback transaction", status))
      return false;
    active_ = false;
    engine_->transactionRolledBack();
    return true;
  }

  virtual void cancel()
  {
    if (!cancelled_)
      engine_->transactionCancelled();
    cancelled_ = true;
  }

private:
  bool checkActive(LocalLiteStorageStatus *status) const
  {
    if (!active_)
      {
        setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                  "storage transaction is not active");
        return false;
      }
    if (cancelled_)
      {
        setStatus(status, LOCAL_LITE_STORAGE_CANCELLED,
                  "storage transaction was cancelled");
        return false;
      }
    clearStatus(status);
    return true;
  }

  RocksDBTransactionEngine *engine_;
  rocksdb_transaction_t *transaction_;
  bool active_;
  bool cancelled_;
  const rocksdb_snapshot_t *snapshot_;
};

LocalLiteStorageTxn *RocksDBSession::begin(LocalLiteStorageStatus *status)
{
  if (!engine_->requireOpen(status) ||
      !engine_->checkDiskWatermark(status))
    return NULL;
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(
      writeOptions, engine_->options_.synchronousCommit ? 1 : 0);
  rocksdb_transaction_options_t *txnOptions =
      rocksdb_transaction_options_create();
  rocksdb_transaction_options_set_set_snapshot(txnOptions, 1);
  rocksdb_transaction_options_set_deadlock_detect(txnOptions, 1);
  rocksdb_transaction_options_set_lock_timeout(
      txnOptions, engine_->options_.lockTimeoutMillis);
  rocksdb_transaction_t *transaction =
      rocksdb_transaction_begin(engine_->db_, writeOptions, txnOptions, NULL);
  rocksdb_transaction_options_destroy(txnOptions);
  rocksdb_writeoptions_destroy(writeOptions);
  if (!transaction)
    {
      setStatus(status, LOCAL_LITE_STORAGE_IO_ERROR,
                "begin storage transaction failed");
      return NULL;
    }
  clearStatus(status);
  return new RocksDBTransaction(engine_, transaction);
}

bool RocksDBCursor::next(LocalLiteStorageRecord *record,
                         bool *end,
                         LocalLiteStorageStatus *status)
{
  if (!record || !end)
    {
      setStatus(status, LOCAL_LITE_STORAGE_INVALID,
                "storage cursor output is missing");
      return false;
    }
  if (cancelled_)
    {
      setStatus(status, LOCAL_LITE_STORAGE_CANCELLED,
                "storage cursor was cancelled");
      return false;
    }
  if (!rocksdb_iter_valid(iterator_))
    {
      char *error = NULL;
      rocksdb_iter_get_error(iterator_, &error);
      if (!consumeRocksError(error, "advance storage cursor", status))
        return false;
      *end = true;
      record->key.clear();
      record->value.clear();
      return true;
    }

  size_t keyLength = 0;
  const char *key = rocksdb_iter_key(iterator_, &keyLength);
  std::string currentKey(key, keyLength);
  if (!endKey_.empty() && currentKey >= endKey_)
    {
      clearStatus(status);
      *end = true;
      record->key.clear();
      record->value.clear();
      return true;
    }
  size_t valueLength = 0;
  const char *value = rocksdb_iter_value(iterator_, &valueLength);
  record->key.swap(currentKey);
  record->value.assign(value, valueLength);
  rocksdb_iter_next(iterator_);
  engine_->addRead(keyLength + valueLength);
  clearStatus(status);
  *end = false;
  return true;
}

} // namespace

LocalLiteStorageEngine *LocalLiteCreateRocksDBTransactionEngine()
{
  return new RocksDBTransactionEngine();
}
