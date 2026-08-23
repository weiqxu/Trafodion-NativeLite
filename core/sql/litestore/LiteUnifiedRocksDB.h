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

#ifndef LITE_UNIFIED_ROCKSDB_H
#define LITE_UNIFIED_ROCKSDB_H

#include <rocksdb/c.h>

#include <stdint.h>
#include <string>

// Prepares the exclusive M13 runtime layout. Fresh stores are created directly
// in TransactionDB and a pre-activation interruption is retryable. Old
// per-table catalog/data directories and unsupported format markers are
// rejected; there is no old-layout migration or fallback path.
bool LiteUnifiedRocksDBPrepare(const std::string &root,
                                    std::string *error);
void LiteUnifiedRocksDBShutdown();
bool LiteUnifiedRocksDBActive();
uint64_t LiteUnifiedRocksDBSequence();
uint32_t LiteUnifiedDurableShardCount();
uint32_t LiteUnifiedPhysicalWalShardCount();
bool LiteUnifiedNativeWalFastPathEnabled();
bool LiteUnifiedRocksDBCheckpoint(const std::string &path,
                                       std::string *error);
std::string LiteUnifiedRocksDBPath(const std::string &root);

// A physical write batch may contain records from any logical catalog/table
// handle. Native mode writes the translated keys and a WAL-only commit envelope
// to the unified TransactionDB in one sequence-number transition. Legacy mode
// retains the Phase 8 shard intent protocol for recovery/fault compatibility.
struct LiteUnifiedWriteBatch;
LiteUnifiedWriteBatch *LiteUnifiedWriteBatchCreate();
void LiteUnifiedWriteBatchDestroy(LiteUnifiedWriteBatch *batch);
void LiteUnifiedWriteBatchPut(LiteUnifiedWriteBatch *batch,
                                   rocksdb_t *logicalDb,
                                   const std::string &key,
                                   const std::string &value);
void LiteUnifiedWriteBatchDelete(LiteUnifiedWriteBatch *batch,
                                      rocksdb_t *logicalDb,
                                      const std::string &key);
bool LiteUnifiedWriteBatchCommit(LiteUnifiedWriteBatch *batch,
                                      bool sync,
                                      std::string *error);

rocksdb_t *LiteRocksDBOpen(const rocksdb_options_t *options,
                                const char *name,
                                char **error);
void LiteRocksDBClose(rocksdb_t *db);
char *LiteRocksDBGet(rocksdb_t *db,
                          const rocksdb_readoptions_t *options,
                          const char *key,
                          size_t keyLength,
                          size_t *valueLength,
                          char **error);
void LiteRocksDBMultiGet(
    rocksdb_t *db, const rocksdb_readoptions_t *options, size_t keyCount,
    const char *const *keys, const size_t *keyLengths, char **values,
    size_t *valueLengths, char **errors);
void LiteRocksDBPut(rocksdb_t *db,
                         const rocksdb_writeoptions_t *options,
                         const char *key,
                         size_t keyLength,
                         const char *value,
                         size_t valueLength,
                         char **error);
void LiteRocksDBDelete(rocksdb_t *db,
                            const rocksdb_writeoptions_t *options,
                            const char *key,
                            size_t keyLength,
                            char **error);
void LiteRocksDBWrite(rocksdb_t *db,
                           const rocksdb_writeoptions_t *options,
                           rocksdb_writebatch_t *batch,
                           char **error);

rocksdb_writebatch_t *LiteRocksDBWriteBatchCreate();
void LiteRocksDBWriteBatchDestroy(rocksdb_writebatch_t *batch);
void LiteRocksDBWriteBatchPut(rocksdb_writebatch_t *batch,
                                   const char *key,
                                   size_t keyLength,
                                   const char *value,
                                   size_t valueLength);
void LiteRocksDBWriteBatchDelete(rocksdb_writebatch_t *batch,
                                      const char *key,
                                      size_t keyLength);

rocksdb_iterator_t *LiteRocksDBCreateIterator(
    rocksdb_t *db, const rocksdb_readoptions_t *options);
void LiteRocksDBIteratorDestroy(rocksdb_iterator_t *iterator);
unsigned char LiteRocksDBIteratorValid(
    const rocksdb_iterator_t *iterator);
void LiteRocksDBIteratorSeekToFirst(rocksdb_iterator_t *iterator);
void LiteRocksDBIteratorSeek(rocksdb_iterator_t *iterator,
                                  const char *key,
                                  size_t keyLength);
void LiteRocksDBIteratorNext(rocksdb_iterator_t *iterator);
const char *LiteRocksDBIteratorKey(const rocksdb_iterator_t *iterator,
                                        size_t *keyLength);
const char *LiteRocksDBIteratorValue(const rocksdb_iterator_t *iterator,
                                          size_t *valueLength);
void LiteRocksDBIteratorGetError(const rocksdb_iterator_t *iterator,
                                      char **error);

const rocksdb_snapshot_t *LiteRocksDBCreateSnapshot(rocksdb_t *db);
void LiteRocksDBReleaseSnapshot(
    rocksdb_t *db, const rocksdb_snapshot_t *snapshot);
void LiteRocksDBDestroy(const rocksdb_options_t *options,
                             const char *name,
                             char **error);

#ifndef LITE_UNIFIED_ROCKSDB_IMPLEMENTATION
#define rocksdb_open LiteRocksDBOpen
#define rocksdb_close LiteRocksDBClose
#define rocksdb_get LiteRocksDBGet
#define rocksdb_multi_get LiteRocksDBMultiGet
#define rocksdb_put LiteRocksDBPut
#define rocksdb_delete LiteRocksDBDelete
#define rocksdb_write LiteRocksDBWrite
#define rocksdb_writebatch_create LiteRocksDBWriteBatchCreate
#define rocksdb_writebatch_destroy LiteRocksDBWriteBatchDestroy
#define rocksdb_writebatch_put LiteRocksDBWriteBatchPut
#define rocksdb_writebatch_delete LiteRocksDBWriteBatchDelete
#define rocksdb_create_iterator LiteRocksDBCreateIterator
#define rocksdb_iter_destroy LiteRocksDBIteratorDestroy
#define rocksdb_iter_valid LiteRocksDBIteratorValid
#define rocksdb_iter_seek_to_first LiteRocksDBIteratorSeekToFirst
#define rocksdb_iter_seek LiteRocksDBIteratorSeek
#define rocksdb_iter_next LiteRocksDBIteratorNext
#define rocksdb_iter_key LiteRocksDBIteratorKey
#define rocksdb_iter_value LiteRocksDBIteratorValue
#define rocksdb_iter_get_error LiteRocksDBIteratorGetError
#define rocksdb_create_snapshot LiteRocksDBCreateSnapshot
#define rocksdb_release_snapshot LiteRocksDBReleaseSnapshot
#define rocksdb_destroy_db LiteRocksDBDestroy
#endif

#endif
