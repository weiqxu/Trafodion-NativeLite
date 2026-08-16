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

#ifndef LOCAL_LITE_UNIFIED_ROCKSDB_H
#define LOCAL_LITE_UNIFIED_ROCKSDB_H

#include <rocksdb/c.h>

#include <stdint.h>
#include <string>

// Prepares the exclusive M13 runtime layout. Fresh stores are created directly
// in TransactionDB and a pre-activation interruption is retryable. Old
// per-table catalog/data directories and unsupported format markers are
// rejected; there is no old-layout migration or fallback path.
bool LocalLiteUnifiedRocksDBPrepare(const std::string &root,
                                    std::string *error);
void LocalLiteUnifiedRocksDBShutdown();
bool LocalLiteUnifiedRocksDBActive();
uint64_t LocalLiteUnifiedRocksDBSequence();
bool LocalLiteUnifiedRocksDBCheckpoint(const std::string &path,
                                       std::string *error);
std::string LocalLiteUnifiedRocksDBPath(const std::string &root);

rocksdb_t *LocalLiteRocksDBOpen(const rocksdb_options_t *options,
                                const char *name,
                                char **error);
void LocalLiteRocksDBClose(rocksdb_t *db);
char *LocalLiteRocksDBGet(rocksdb_t *db,
                          const rocksdb_readoptions_t *options,
                          const char *key,
                          size_t keyLength,
                          size_t *valueLength,
                          char **error);
void LocalLiteRocksDBPut(rocksdb_t *db,
                         const rocksdb_writeoptions_t *options,
                         const char *key,
                         size_t keyLength,
                         const char *value,
                         size_t valueLength,
                         char **error);
void LocalLiteRocksDBDelete(rocksdb_t *db,
                            const rocksdb_writeoptions_t *options,
                            const char *key,
                            size_t keyLength,
                            char **error);
void LocalLiteRocksDBWrite(rocksdb_t *db,
                           const rocksdb_writeoptions_t *options,
                           rocksdb_writebatch_t *batch,
                           char **error);

rocksdb_writebatch_t *LocalLiteRocksDBWriteBatchCreate();
void LocalLiteRocksDBWriteBatchDestroy(rocksdb_writebatch_t *batch);
void LocalLiteRocksDBWriteBatchPut(rocksdb_writebatch_t *batch,
                                   const char *key,
                                   size_t keyLength,
                                   const char *value,
                                   size_t valueLength);
void LocalLiteRocksDBWriteBatchDelete(rocksdb_writebatch_t *batch,
                                      const char *key,
                                      size_t keyLength);

rocksdb_iterator_t *LocalLiteRocksDBCreateIterator(
    rocksdb_t *db, const rocksdb_readoptions_t *options);
void LocalLiteRocksDBIteratorDestroy(rocksdb_iterator_t *iterator);
unsigned char LocalLiteRocksDBIteratorValid(
    const rocksdb_iterator_t *iterator);
void LocalLiteRocksDBIteratorSeekToFirst(rocksdb_iterator_t *iterator);
void LocalLiteRocksDBIteratorSeek(rocksdb_iterator_t *iterator,
                                  const char *key,
                                  size_t keyLength);
void LocalLiteRocksDBIteratorNext(rocksdb_iterator_t *iterator);
const char *LocalLiteRocksDBIteratorKey(const rocksdb_iterator_t *iterator,
                                        size_t *keyLength);
const char *LocalLiteRocksDBIteratorValue(const rocksdb_iterator_t *iterator,
                                          size_t *valueLength);
void LocalLiteRocksDBIteratorGetError(const rocksdb_iterator_t *iterator,
                                      char **error);

const rocksdb_snapshot_t *LocalLiteRocksDBCreateSnapshot(rocksdb_t *db);
void LocalLiteRocksDBReleaseSnapshot(
    rocksdb_t *db, const rocksdb_snapshot_t *snapshot);
void LocalLiteRocksDBDestroy(const rocksdb_options_t *options,
                             const char *name,
                             char **error);

#ifndef LOCAL_LITE_UNIFIED_ROCKSDB_IMPLEMENTATION
#define rocksdb_open LocalLiteRocksDBOpen
#define rocksdb_close LocalLiteRocksDBClose
#define rocksdb_get LocalLiteRocksDBGet
#define rocksdb_put LocalLiteRocksDBPut
#define rocksdb_delete LocalLiteRocksDBDelete
#define rocksdb_write LocalLiteRocksDBWrite
#define rocksdb_writebatch_create LocalLiteRocksDBWriteBatchCreate
#define rocksdb_writebatch_destroy LocalLiteRocksDBWriteBatchDestroy
#define rocksdb_writebatch_put LocalLiteRocksDBWriteBatchPut
#define rocksdb_writebatch_delete LocalLiteRocksDBWriteBatchDelete
#define rocksdb_create_iterator LocalLiteRocksDBCreateIterator
#define rocksdb_iter_destroy LocalLiteRocksDBIteratorDestroy
#define rocksdb_iter_valid LocalLiteRocksDBIteratorValid
#define rocksdb_iter_seek_to_first LocalLiteRocksDBIteratorSeekToFirst
#define rocksdb_iter_seek LocalLiteRocksDBIteratorSeek
#define rocksdb_iter_next LocalLiteRocksDBIteratorNext
#define rocksdb_iter_key LocalLiteRocksDBIteratorKey
#define rocksdb_iter_value LocalLiteRocksDBIteratorValue
#define rocksdb_iter_get_error LocalLiteRocksDBIteratorGetError
#define rocksdb_create_snapshot LocalLiteRocksDBCreateSnapshot
#define rocksdb_release_snapshot LocalLiteRocksDBReleaseSnapshot
#define rocksdb_destroy_db LocalLiteRocksDBDestroy
#endif

#endif
