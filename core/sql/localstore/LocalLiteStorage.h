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

#ifndef LOCAL_LITE_STORAGE_H
#define LOCAL_LITE_STORAGE_H

#include <stdint.h>

#include <string>

// Backend-neutral storage contract for the LocalLite single-node runtime.
// SQL/catalog encodings remain above this boundary.  A backend sees one
// ordered key space, so catalog records, base rows, unique records, and index
// records can participate in the same transaction.

enum LocalLiteStorageCode
{
  LOCAL_LITE_STORAGE_OK = 0,
  LOCAL_LITE_STORAGE_NOT_FOUND,
  LOCAL_LITE_STORAGE_CONFLICT,
  LOCAL_LITE_STORAGE_CANCELLED,
  LOCAL_LITE_STORAGE_NO_SPACE,
  LOCAL_LITE_STORAGE_CORRUPTION,
  LOCAL_LITE_STORAGE_INVALID,
  LOCAL_LITE_STORAGE_IO_ERROR
};

struct LocalLiteStorageStatus
{
  LocalLiteStorageStatus()
    : code(LOCAL_LITE_STORAGE_OK), retryable(false)
  {
  }

  bool ok() const { return code == LOCAL_LITE_STORAGE_OK; }

  LocalLiteStorageCode code;
  bool retryable;
  std::string message;
};

struct LocalLiteStorageOptions
{
  LocalLiteStorageOptions()
    : createIfMissing(true), synchronousCommit(true),
      minimumFreeBytes(0), lockTimeoutMillis(5000)
  {
  }

  bool createIfMissing;
  bool synchronousCommit;
  uint64_t minimumFreeBytes;
  int64_t lockTimeoutMillis;
};

struct LocalLiteStorageRecord
{
  std::string key;
  std::string value;
};

struct LocalLiteStorageMetrics
{
  LocalLiteStorageMetrics()
    : committedTransactions(0), rolledBackTransactions(0), conflicts(0),
      cancelledTransactions(0), bytesRead(0), bytesWritten(0),
      estimatedKeys(0), liveDataBytes(0)
  {
  }

  uint64_t committedTransactions;
  uint64_t rolledBackTransactions;
  uint64_t conflicts;
  uint64_t cancelledTransactions;
  uint64_t bytesRead;
  uint64_t bytesWritten;
  uint64_t estimatedKeys;
  uint64_t liveDataBytes;
};

class LocalLiteStorageCursor
{
public:
  virtual ~LocalLiteStorageCursor() {}

  // Returns one record at a time.  end is true only after the bounded range
  // is exhausted.  Implementations must never materialize the full range.
  virtual bool next(LocalLiteStorageRecord *record,
                    bool *end,
                    LocalLiteStorageStatus *status) = 0;
  virtual void cancel() = 0;
};

class LocalLiteStorageTxn
{
public:
  virtual ~LocalLiteStorageTxn() {}

  virtual bool get(const std::string &key,
                   std::string *value,
                   bool *found,
                   LocalLiteStorageStatus *status) = 0;
  virtual bool put(const std::string &key,
                   const std::string &value,
                   LocalLiteStorageStatus *status) = 0;
  virtual bool erase(const std::string &key,
                     LocalLiteStorageStatus *status) = 0;
  virtual LocalLiteStorageCursor *scan(const std::string &beginKey,
                                       const std::string &endKey,
                                       LocalLiteStorageStatus *status) = 0;
  virtual bool commit(LocalLiteStorageStatus *status) = 0;
  virtual bool rollback(LocalLiteStorageStatus *status) = 0;
  virtual void cancel() = 0;
};

class LocalLiteStorageSession
{
public:
  virtual ~LocalLiteStorageSession() {}

  virtual LocalLiteStorageTxn *begin(LocalLiteStorageStatus *status) = 0;
};

class LocalLiteStorageEngine
{
public:
  virtual ~LocalLiteStorageEngine() {}

  virtual const char *name() const = 0;
  virtual bool open(const std::string &path,
                    const LocalLiteStorageOptions &options,
                    LocalLiteStorageStatus *status) = 0;
  virtual void close() = 0;
  virtual LocalLiteStorageSession *createSession(
      LocalLiteStorageStatus *status) = 0;
  virtual bool checkpoint(const std::string &path,
                          LocalLiteStorageStatus *status) = 0;
  virtual bool backup(const std::string &path,
                      LocalLiteStorageStatus *status) = 0;
  virtual bool restore(const std::string &backupPath,
                       const std::string &restorePath,
                       LocalLiteStorageStatus *status) = 0;
  virtual bool verify(LocalLiteStorageStatus *status) = 0;
  virtual LocalLiteStorageMetrics metrics() const = 0;
};

// The M12 selected candidate.  Ownership is transferred to the caller.
LocalLiteStorageEngine *LocalLiteCreateRocksDBTransactionEngine();

// Independent transactional embedded candidate used by the M12B common
// correctness/recovery/workload gate.  Ownership is transferred to the caller.
LocalLiteStorageEngine *LocalLiteCreateSQLiteEngine();

#endif
