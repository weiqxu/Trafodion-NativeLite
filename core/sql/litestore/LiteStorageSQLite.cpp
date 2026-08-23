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

#include "LiteStorage.h"

#include <sqlite3.h>

#include <pthread.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cstdio>

namespace
{

void runSQLiteStorageFault(const char *point, int exitCode)
{
  const char *fault = getenv("TRAF_LITE_STORAGE_FAULT");
  if (fault && std::string(fault) == point)
    _exit(exitCode);
}

void clearSQLiteStatus(LiteStorageStatus *status)
{
  if (!status)
    return;
  status->code = LITE_STORAGE_OK;
  status->retryable = false;
  status->message.clear();
}

void setSQLiteStatus(LiteStorageStatus *status,
                     LiteStorageCode code,
                     const std::string &message,
                     bool retryable = false)
{
  if (!status)
    return;
  status->code = code;
  status->retryable = retryable;
  status->message = message;
}

bool checkSQLite(int code,
                 sqlite3 *db,
                 const char *operation,
                 LiteStorageStatus *status)
{
  if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW)
    {
      clearSQLiteStatus(status);
      return true;
    }
  LiteStorageCode storageCode = LITE_STORAGE_IO_ERROR;
  bool retryable = false;
  if (code == SQLITE_BUSY || code == SQLITE_LOCKED)
    {
      storageCode = LITE_STORAGE_CONFLICT;
      retryable = true;
    }
  else if (code == SQLITE_INTERRUPT)
    storageCode = LITE_STORAGE_CANCELLED;
  else if (code == SQLITE_FULL)
    storageCode = LITE_STORAGE_NO_SPACE;
  else if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB)
    storageCode = LITE_STORAGE_CORRUPTION;
  else if (code == SQLITE_MISUSE || code == SQLITE_RANGE)
    storageCode = LITE_STORAGE_INVALID;
  std::string message(operation);
  message += ": ";
  message += db ? sqlite3_errmsg(db) : sqlite3_errstr(code);
  setSQLiteStatus(status, storageCode, message, retryable);
  return false;
}

class SQLiteMutexGuard
{
public:
  explicit SQLiteMutexGuard(pthread_mutex_t *mutex) : mutex_(mutex)
  {
    pthread_mutex_lock(mutex_);
  }
  ~SQLiteMutexGuard() { pthread_mutex_unlock(mutex_); }

private:
  SQLiteMutexGuard(const SQLiteMutexGuard &);
  SQLiteMutexGuard &operator=(const SQLiteMutexGuard &);
  pthread_mutex_t *mutex_;
};

class SQLiteEngine;

class SQLiteCursor : public LiteStorageCursor
{
public:
  SQLiteCursor(sqlite3 *db, sqlite3_stmt *statement, SQLiteEngine *engine)
    : db_(db), statement_(statement), engine_(engine), cancelled_(false),
      finished_(false)
  {
  }
  virtual ~SQLiteCursor() { sqlite3_finalize(statement_); }

  virtual bool next(LiteStorageRecord *record,
                    bool *end,
                    LiteStorageStatus *status);
  virtual void cancel()
  {
    cancelled_ = true;
    sqlite3_interrupt(db_);
  }

private:
  sqlite3 *db_;
  sqlite3_stmt *statement_;
  SQLiteEngine *engine_;
  bool cancelled_;
  bool finished_;
};

class SQLiteTransaction;

class SQLiteSession : public LiteStorageSession
{
public:
  SQLiteSession(SQLiteEngine *engine, sqlite3 *db) : engine_(engine), db_(db) {}
  virtual ~SQLiteSession() { sqlite3_close(db_); }
  virtual LiteStorageTxn *begin(LiteStorageStatus *status);

private:
  SQLiteEngine *engine_;
  sqlite3 *db_;
};

class SQLiteEngine : public LiteStorageEngine
{
  friend class SQLiteCursor;
  friend class SQLiteSession;
  friend class SQLiteTransaction;

public:
  SQLiteEngine() : admin_(NULL), opened_(false)
  {
    pthread_mutex_init(&metricsMutex_, NULL);
  }
  virtual ~SQLiteEngine()
  {
    close();
    pthread_mutex_destroy(&metricsMutex_);
  }
  virtual const char *name() const { return "sqlite-wal"; }

  virtual bool open(const std::string &path,
                    const LiteStorageOptions &options,
                    LiteStorageStatus *status)
  {
    if (opened_ || path.empty())
      {
        setSQLiteStatus(status, LITE_STORAGE_INVALID,
                        opened_ ? "storage engine is already open"
                                : "storage path is empty");
        return false;
      }
    const int flags = SQLITE_OPEN_READWRITE |
        (options.createIfMissing ? SQLITE_OPEN_CREATE : 0) |
        SQLITE_OPEN_FULLMUTEX;
    int code = sqlite3_open_v2(path.c_str(), &admin_, flags, NULL);
    if (!checkSQLite(code, admin_, "open SQLite storage", status))
      {
        if (admin_)
          sqlite3_close(admin_);
        admin_ = NULL;
        return false;
      }
    options_ = options;
    path_ = path;
    sqlite3_busy_timeout(admin_, static_cast<int>(options.lockTimeoutMillis));
    if (!exec(admin_, "PRAGMA journal_mode=WAL", status) ||
        !exec(admin_, options.synchronousCommit
                         ? "PRAGMA synchronous=FULL"
                         : "PRAGMA synchronous=NORMAL", status) ||
        !exec(admin_, "PRAGMA foreign_keys=ON", status) ||
        !exec(admin_, "CREATE TABLE IF NOT EXISTS kv("
                      "k BLOB PRIMARY KEY NOT NULL,"
                      "v BLOB NOT NULL) WITHOUT ROWID", status))
      {
        sqlite3_close(admin_);
        admin_ = NULL;
        return false;
      }
    opened_ = true;
    return true;
  }

  virtual void close()
  {
    if (!opened_)
      return;
    sqlite3_wal_checkpoint_v2(admin_, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                              NULL, NULL);
    sqlite3_close(admin_);
    admin_ = NULL;
    opened_ = false;
  }

  virtual LiteStorageSession *createSession(
      LiteStorageStatus *status)
  {
    if (!requireOpen(status))
      return NULL;
    sqlite3 *db = NULL;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX;
    int code = sqlite3_open_v2(path_.c_str(), &db, flags, NULL);
    if (!checkSQLite(code, db, "open SQLite session", status))
      {
        if (db)
          sqlite3_close(db);
        return NULL;
      }
    sqlite3_busy_timeout(db, static_cast<int>(options_.lockTimeoutMillis));
    if (!exec(db, options_.synchronousCommit
                     ? "PRAGMA synchronous=FULL"
                     : "PRAGMA synchronous=NORMAL", status))
      {
        sqlite3_close(db);
        return NULL;
      }
    return new SQLiteSession(this, db);
  }

  virtual bool checkpoint(const std::string &path,
                          LiteStorageStatus *status)
  {
    runSQLiteStorageFault("checkpoint", 88);
    return copyDatabase(path, status);
  }

  virtual bool backup(const std::string &path,
                      LiteStorageStatus *status)
  {
    runSQLiteStorageFault("backup", 89);
    return copyDatabase(path, status);
  }

  virtual bool restore(const std::string &backupPath,
                       const std::string &restorePath,
                       LiteStorageStatus *status)
  {
    if (backupPath.empty() || restorePath.empty())
      {
        setSQLiteStatus(status, LITE_STORAGE_INVALID,
                        "backup or restore path is empty");
        return false;
      }
    sqlite3 *source = NULL;
    sqlite3 *destination = NULL;
    int code = sqlite3_open_v2(backupPath.c_str(), &source,
                               SQLITE_OPEN_READONLY, NULL);
    if (!checkSQLite(code, source, "open SQLite backup for restore", status))
      {
        if (source)
          sqlite3_close(source);
        return false;
      }
    code = sqlite3_open_v2(restorePath.c_str(), &destination,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (!checkSQLite(code, destination, "open SQLite restore store", status))
      {
        sqlite3_close(source);
        if (destination)
          sqlite3_close(destination);
        return false;
      }
    sqlite3_backup *copy =
        sqlite3_backup_init(destination, "main", source, "main");
    if (!copy)
      {
        checkSQLite(sqlite3_errcode(destination), destination,
                    "initialize SQLite restore", status);
        sqlite3_close(destination);
        sqlite3_close(source);
        return false;
      }
    code = sqlite3_backup_step(copy, -1);
    const int finishCode = sqlite3_backup_finish(copy);
    if (code == SQLITE_DONE)
      code = finishCode;
    const bool ok = checkSQLite(code, destination,
                                "restore SQLite backup", status);
    sqlite3_close(destination);
    sqlite3_close(source);
    return ok;
  }

  virtual bool verify(LiteStorageStatus *status)
  {
    if (!requireOpen(status))
      return false;
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(admin_, "PRAGMA integrity_check", -1,
                                  &statement, NULL);
    if (!checkSQLite(code, admin_, "prepare SQLite integrity check", status))
      return false;
    code = sqlite3_step(statement);
    const bool ok = code == SQLITE_ROW &&
        std::string(reinterpret_cast<const char *>(
            sqlite3_column_text(statement, 0))) == "ok";
    sqlite3_finalize(statement);
    if (!ok)
      {
        if (code != SQLITE_ROW)
          return checkSQLite(code, admin_, "run SQLite integrity check",
                             status);
        setSQLiteStatus(status, LITE_STORAGE_CORRUPTION,
                        "SQLite integrity check failed");
        return false;
      }
    clearSQLiteStatus(status);
    return true;
  }

  virtual LiteStorageMetrics metrics() const
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    LiteStorageMetrics result = metrics_;
    sqlite3_stmt *statement = NULL;
    if (admin_ && sqlite3_prepare_v2(
            admin_, "SELECT count(*), coalesce(sum(length(k)+length(v)),0) "
                    "FROM kv", -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW)
      {
        result.estimatedKeys = sqlite3_column_int64(statement, 0);
        result.liveDataBytes = sqlite3_column_int64(statement, 1);
      }
    if (statement)
      sqlite3_finalize(statement);
    return result;
  }

private:
  bool requireOpen(LiteStorageStatus *status) const
  {
    if (opened_)
      return true;
    setSQLiteStatus(status, LITE_STORAGE_INVALID,
                    "storage engine is not open");
    return false;
  }

  bool exec(sqlite3 *db, const char *sql,
            LiteStorageStatus *status) const
  {
    char *message = NULL;
    int code = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (code == SQLITE_OK)
      {
        clearSQLiteStatus(status);
        return true;
      }
    if (message)
      sqlite3_free(message);
    return checkSQLite(code, db, "execute SQLite storage operation", status);
  }

  bool copyDatabase(const std::string &path,
                    LiteStorageStatus *status)
  {
    if (!requireOpen(status) || path.empty())
      {
        if (path.empty())
          setSQLiteStatus(status, LITE_STORAGE_INVALID,
                          "backup path is empty");
        return false;
      }
    sqlite3 *destination = NULL;
    int code = sqlite3_open_v2(path.c_str(), &destination,
                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                               NULL);
    if (!checkSQLite(code, destination, "open SQLite backup", status))
      {
        if (destination)
          sqlite3_close(destination);
        return false;
      }
    sqlite3_backup *backup =
        sqlite3_backup_init(destination, "main", admin_, "main");
    if (!backup)
      {
        checkSQLite(sqlite3_errcode(destination), destination,
                    "initialize SQLite backup", status);
        sqlite3_close(destination);
        return false;
      }
    code = sqlite3_backup_step(backup, -1);
    const int finishCode = sqlite3_backup_finish(backup);
    if (code == SQLITE_DONE)
      code = finishCode;
    const bool ok = checkSQLite(code, destination,
                                "copy SQLite backup", status);
    sqlite3_close(destination);
    return ok;
  }

  bool checkDiskWatermark(LiteStorageStatus *status) const
  {
    if (options_.minimumFreeBytes == 0)
      return true;
    std::string parent = path_;
    const std::string::size_type slash = parent.find_last_of('/');
    parent = slash == std::string::npos ? "." : parent.substr(0, slash);
    struct statvfs info;
    if (statvfs(parent.c_str(), &info) != 0)
      {
        setSQLiteStatus(status, LITE_STORAGE_IO_ERROR,
                        "read SQLite filesystem capacity failed");
        return false;
      }
    const uint64_t available =
        static_cast<uint64_t>(info.f_bavail) * info.f_frsize;
    if (available < options_.minimumFreeBytes)
      {
        setSQLiteStatus(status, LITE_STORAGE_NO_SPACE,
                        "storage disk watermark reached");
        return false;
      }
    return true;
  }

  void addRead(uint64_t bytes)
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    metrics_.bytesRead += bytes;
  }
  void addWrite(uint64_t bytes)
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    metrics_.bytesWritten += bytes;
  }
  void committed()
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    metrics_.committedTransactions++;
  }
  void rolledBack()
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    metrics_.rolledBackTransactions++;
  }
  void conflict()
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    metrics_.conflicts++;
  }
  void cancelled()
  {
    SQLiteMutexGuard guard(&metricsMutex_);
    metrics_.cancelledTransactions++;
  }

  sqlite3 *admin_;
  bool opened_;
  std::string path_;
  LiteStorageOptions options_;
  mutable pthread_mutex_t metricsMutex_;
  LiteStorageMetrics metrics_;
};

class SQLiteTransaction : public LiteStorageTxn
{
public:
  SQLiteTransaction(SQLiteEngine *engine, sqlite3 *db)
    : engine_(engine), db_(db), active_(true), cancelled_(false)
  {
  }
  virtual ~SQLiteTransaction()
  {
    if (active_)
      {
        sqlite3_exec(db_, "ROLLBACK", NULL, NULL, NULL);
        engine_->rolledBack();
      }
  }

  virtual bool get(const std::string &key,
                   std::string *value,
                   bool *found,
                   LiteStorageStatus *status)
  {
    if (!checkActive(status) || !value || !found)
      {
        if (!value || !found)
          setSQLiteStatus(status, LITE_STORAGE_INVALID,
                          "storage get output is missing");
        return false;
      }
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db_, "SELECT v FROM kv WHERE k=?1", -1,
                                  &statement, NULL);
    if (code == SQLITE_OK)
      code = sqlite3_bind_blob(statement, 1, key.data(), key.size(),
                               SQLITE_TRANSIENT);
    if (code == SQLITE_OK)
      code = sqlite3_step(statement);
    if (code == SQLITE_ROW)
      {
        const void *raw = sqlite3_column_blob(statement, 0);
        const int length = sqlite3_column_bytes(statement, 0);
        value->assign(static_cast<const char *>(raw), length);
        *found = true;
        engine_->addRead(key.size() + length);
        sqlite3_finalize(statement);
        clearSQLiteStatus(status);
        return true;
      }
    if (code == SQLITE_DONE)
      {
        value->clear();
        *found = false;
        sqlite3_finalize(statement);
        clearSQLiteStatus(status);
        return true;
      }
    sqlite3_finalize(statement);
    return checkSQLite(code, db_, "read SQLite transaction key", status);
  }

  virtual bool put(const std::string &key,
                   const std::string &value,
                   LiteStorageStatus *status)
  {
    if (!checkActive(status) || !engine_->checkDiskWatermark(status))
      return false;
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(
        db_, "INSERT INTO kv(k,v) VALUES(?1,?2) "
             "ON CONFLICT(k) DO UPDATE SET v=excluded.v", -1,
        &statement, NULL);
    if (code == SQLITE_OK)
      code = sqlite3_bind_blob(statement, 1, key.data(), key.size(),
                               SQLITE_TRANSIENT);
    if (code == SQLITE_OK)
      code = sqlite3_bind_blob(statement, 2, value.data(), value.size(),
                               SQLITE_TRANSIENT);
    if (code == SQLITE_OK)
      code = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (!checkSQLite(code, db_, "write SQLite transaction key", status))
      {
        if (status && status->code == LITE_STORAGE_CONFLICT)
          engine_->conflict();
        return false;
      }
    engine_->addWrite(key.size() + value.size());
    return true;
  }

  virtual bool erase(const std::string &key,
                     LiteStorageStatus *status)
  {
    if (!checkActive(status) || !engine_->checkDiskWatermark(status))
      return false;
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(db_, "DELETE FROM kv WHERE k=?1", -1,
                                  &statement, NULL);
    if (code == SQLITE_OK)
      code = sqlite3_bind_blob(statement, 1, key.data(), key.size(),
                               SQLITE_TRANSIENT);
    if (code == SQLITE_OK)
      code = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (!checkSQLite(code, db_, "delete SQLite transaction key", status))
      {
        if (status && status->code == LITE_STORAGE_CONFLICT)
          engine_->conflict();
        return false;
      }
    engine_->addWrite(key.size());
    return true;
  }

  virtual LiteStorageCursor *scan(const std::string &beginKey,
                                       const std::string &endKey,
                                       LiteStorageStatus *status)
  {
    if (!checkActive(status) ||
        (!endKey.empty() && endKey <= beginKey))
      {
        if (!endKey.empty() && endKey <= beginKey)
          setSQLiteStatus(status, LITE_STORAGE_INVALID,
                          "storage scan range is invalid");
        return NULL;
      }
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(
        db_, "SELECT k,v FROM kv WHERE k>=?1 AND (?2='' OR k<?2) "
             "ORDER BY k", -1, &statement, NULL);
    if (code == SQLITE_OK)
      code = sqlite3_bind_blob(statement, 1, beginKey.data(), beginKey.size(),
                               SQLITE_TRANSIENT);
    if (code == SQLITE_OK)
      code = sqlite3_bind_blob(statement, 2, endKey.data(), endKey.size(),
                               SQLITE_TRANSIENT);
    if (!checkSQLite(code, db_, "prepare SQLite range cursor", status))
      {
        sqlite3_finalize(statement);
        return NULL;
      }
    return new SQLiteCursor(db_, statement, engine_);
  }

  virtual bool commit(LiteStorageStatus *status)
  {
    if (!checkActive(status) || !engine_->checkDiskWatermark(status))
      return false;
    runSQLiteStorageFault("before-commit", 86);
    int code = sqlite3_exec(db_, "COMMIT", NULL, NULL, NULL);
    if (!checkSQLite(code, db_, "commit SQLite transaction", status))
      {
        if (status && status->code == LITE_STORAGE_CONFLICT)
          engine_->conflict();
        return false;
      }
    active_ = false;
    engine_->committed();
    runSQLiteStorageFault("after-commit", 87);
    return true;
  }

  virtual bool rollback(LiteStorageStatus *status)
  {
    if (!active_)
      {
        setSQLiteStatus(status, LITE_STORAGE_INVALID,
                        "storage transaction is not active");
        return false;
      }
    int code = sqlite3_exec(db_, "ROLLBACK", NULL, NULL, NULL);
    if (!checkSQLite(code, db_, "rollback SQLite transaction", status))
      return false;
    active_ = false;
    engine_->rolledBack();
    return true;
  }

  virtual void cancel()
  {
    if (!cancelled_)
      engine_->cancelled();
    cancelled_ = true;
    sqlite3_interrupt(db_);
  }

private:
  bool checkActive(LiteStorageStatus *status) const
  {
    if (!active_)
      {
        setSQLiteStatus(status, LITE_STORAGE_INVALID,
                        "storage transaction is not active");
        return false;
      }
    if (cancelled_)
      {
        setSQLiteStatus(status, LITE_STORAGE_CANCELLED,
                        "storage transaction was cancelled");
        return false;
      }
    clearSQLiteStatus(status);
    return true;
  }

  SQLiteEngine *engine_;
  sqlite3 *db_;
  bool active_;
  bool cancelled_;
};

LiteStorageTxn *SQLiteSession::begin(LiteStorageStatus *status)
{
  if (!engine_->requireOpen(status) ||
      !engine_->checkDiskWatermark(status))
    return NULL;
  const int code = sqlite3_exec(db_, "BEGIN DEFERRED", NULL, NULL, NULL);
  if (!checkSQLite(code, db_, "begin SQLite transaction", status))
    return NULL;
  return new SQLiteTransaction(engine_, db_);
}

bool SQLiteCursor::next(LiteStorageRecord *record,
                        bool *end,
                        LiteStorageStatus *status)
{
  if (!record || !end)
    {
      setSQLiteStatus(status, LITE_STORAGE_INVALID,
                      "storage cursor output is missing");
      return false;
    }
  if (cancelled_)
    {
      setSQLiteStatus(status, LITE_STORAGE_CANCELLED,
                      "storage cursor was cancelled");
      return false;
    }
  if (finished_)
    {
      *end = true;
      clearSQLiteStatus(status);
      return true;
    }
  const int code = sqlite3_step(statement_);
  if (code == SQLITE_DONE)
    {
      finished_ = true;
      *end = true;
      record->key.clear();
      record->value.clear();
      clearSQLiteStatus(status);
      return true;
    }
  if (!checkSQLite(code, db_, "advance SQLite range cursor", status))
    return false;
  const void *key = sqlite3_column_blob(statement_, 0);
  const int keyLength = sqlite3_column_bytes(statement_, 0);
  const void *value = sqlite3_column_blob(statement_, 1);
  const int valueLength = sqlite3_column_bytes(statement_, 1);
  record->key.assign(static_cast<const char *>(key), keyLength);
  record->value.assign(static_cast<const char *>(value), valueLength);
  engine_->addRead(keyLength + valueLength);
  *end = false;
  return true;
}

} // namespace

LiteStorageEngine *LiteCreateSQLiteEngine()
{
  return new SQLiteEngine();
}
