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

#include "LocalLiteStorageMigration.h"

#include <rocksdb/c.h>
#include <openssl/evp.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace
{

const char *FORMAT_KEY = "m12/format";
const char *FORMAT_VALUE = "LocalLiteTxnStore/1";
const char *COUNT_KEY = "m12/migration/record-count";
const char *DIGEST_KEY = "m12/migration/sha256";
const char *SOURCE_KEY = "m12/migration/source-format";

void setMigrationError(LocalLiteStorageStatus *status,
                       LocalLiteStorageCode code,
                       const std::string &message)
{
  if (!status)
    return;
  status->code = code;
  status->retryable = false;
  status->message = message;
}

std::string hex(const std::string &value)
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

bool isDirectory(const std::string &path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool isFile(const std::string &path)
{
  struct stat info;
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool collectTableDatabases(const std::string &root,
                           const std::string &relative,
                           std::vector<std::string> *paths,
                           LocalLiteStorageStatus *status)
{
  const std::string path = relative.empty() ? root : root + "/" + relative;
  if (isFile(path + "/CURRENT"))
    {
      paths->push_back(relative);
      return true;
    }
  DIR *directory = opendir(path.c_str());
  if (!directory)
    {
      if (errno == ENOENT)
        return true;
      setMigrationError(status, LOCAL_LITE_STORAGE_IO_ERROR,
                        "open legacy data directory " + path + ": " +
                        strerror(errno));
      return false;
    }
  std::vector<std::string> children;
  for (struct dirent *entry = readdir(directory); entry;
       entry = readdir(directory))
    if (std::string(entry->d_name) != "." &&
        std::string(entry->d_name) != "..")
      children.push_back(entry->d_name);
  closedir(directory);
  std::sort(children.begin(), children.end());
  for (size_t i = 0; i < children.size(); i++)
    {
      const std::string child = relative.empty()
          ? children[i] : relative + "/" + children[i];
      if (isDirectory(root + "/" + child) &&
          !collectTableDatabases(root, child, paths, status))
        return false;
    }
  return true;
}

void digestField(EVP_MD_CTX *digest, const std::string &value)
{
  const uint64_t length = value.size();
  unsigned char encoded[8];
  for (size_t i = 0; i < 8; i++)
    encoded[7 - i] = static_cast<unsigned char>(length >> (i * 8));
  EVP_DigestUpdate(digest, encoded, sizeof(encoded));
  if (!value.empty())
    EVP_DigestUpdate(digest, value.data(), value.size());
}

std::string finishDigest(EVP_MD_CTX *digest)
{
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_DigestFinal_ex(digest, bytes, &length);
  return hex(std::string(reinterpret_cast<const char *>(bytes), length));
}

bool migrateDatabase(const std::string &path,
                     const std::string &targetPrefix,
                     LocalLiteStorageTxn *target,
                     EVP_MD_CTX *digest,
                     uint64_t *recordCount,
                     uint64_t failAfter,
                     LocalLiteStorageStatus *status)
{
  rocksdb_options_t *options = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(options, 0);
  char *error = NULL;
  rocksdb_t *db = rocksdb_open(options, path.c_str(), &error);
  rocksdb_options_destroy(options);
  if (error)
    {
      const std::string message(error);
      rocksdb_free(error);
      setMigrationError(status, LOCAL_LITE_STORAGE_IO_ERROR,
                        "open legacy RocksDB " + path + ": " + message);
      return false;
    }
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *iterator = rocksdb_create_iterator(db, readOptions);
  bool ok = true;
  for (rocksdb_iter_seek_to_first(iterator); rocksdb_iter_valid(iterator);
       rocksdb_iter_next(iterator))
    {
      size_t keyLength = 0;
      size_t valueLength = 0;
      const char *rawKey = rocksdb_iter_key(iterator, &keyLength);
      const char *rawValue = rocksdb_iter_value(iterator, &valueLength);
      const std::string key(rawKey, keyLength);
      const std::string value(rawValue, valueLength);
      const std::string migratedKey = targetPrefix + hex(key);
      if (!target->put(migratedKey, value, status))
        {
          ok = false;
          break;
        }
      digestField(digest, migratedKey);
      digestField(digest, value);
      (*recordCount)++;
      if (failAfter != 0 && *recordCount == failAfter)
        {
          setMigrationError(status, LOCAL_LITE_STORAGE_IO_ERROR,
                            "injected legacy migration interruption");
          ok = false;
          break;
        }
    }
  error = NULL;
  rocksdb_iter_get_error(iterator, &error);
  if (ok && error)
    {
      setMigrationError(status, LOCAL_LITE_STORAGE_IO_ERROR,
                        "scan legacy RocksDB failed: " +
                        std::string(error));
      ok = false;
    }
  if (error)
    rocksdb_free(error);
  rocksdb_iter_destroy(iterator);
  rocksdb_readoptions_destroy(readOptions);
  rocksdb_close(db);
  return ok;
}

bool readRequired(LocalLiteStorageTxn *txn,
                  const std::string &key,
                  std::string *value,
                  LocalLiteStorageStatus *status)
{
  bool found = false;
  return txn->get(key, value, &found, status) && found;
}

} // namespace

bool LocalLiteMigrateLegacyStore(const std::string &legacyRoot,
                                 LocalLiteStorageEngine *target,
                                 LocalLiteStorageStatus *status)
{
  if (!target || !isDirectory(legacyRoot + "/catalog"))
    {
      setMigrationError(status, LOCAL_LITE_STORAGE_INVALID,
                        "legacy LocalLite catalog is missing");
      return false;
    }
  LocalLiteStorageSession *session = target->createSession(status);
  if (!session)
    return false;
  LocalLiteStorageTxn *txn = session->begin(status);
  if (!txn)
    {
      delete session;
      return false;
    }
  std::string existingFormat;
  bool found = false;
  if (!txn->get(FORMAT_KEY, &existingFormat, &found, status))
    {
      delete txn;
      delete session;
      return false;
    }
  if (found)
    {
      setMigrationError(status, LOCAL_LITE_STORAGE_INVALID,
                        "migration target is not empty");
      delete txn;
      delete session;
      return false;
    }

  uint64_t failAfter = 0;
  const char *fault = getenv("TRAF_LOCAL_LITE_MIGRATION_FAULT_AFTER_RECORDS");
  if (fault && fault[0])
    failAfter = strtoull(fault, NULL, 10);
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  EVP_DigestInit_ex(digest, EVP_sha256(), NULL);
  uint64_t recordCount = 0;
  bool ok = migrateDatabase(legacyRoot + "/catalog", "legacy/catalog/",
                            txn, digest, &recordCount, failAfter, status);
  std::vector<std::string> tables;
  if (ok)
    ok = collectTableDatabases(legacyRoot + "/data", "", &tables, status);
  for (size_t i = 0; ok && i < tables.size(); i++)
    ok = migrateDatabase(legacyRoot + "/data/" + tables[i],
                         "legacy/table/" + hex(tables[i]) + "/",
                         txn, digest, &recordCount, failAfter, status);
  const std::string digestValue = ok ? finishDigest(digest) : std::string();
  EVP_MD_CTX_free(digest);
  if (ok)
    {
      std::ostringstream count;
      count << recordCount;
      ok = txn->put(SOURCE_KEY, "per-table-rocksdb-v1", status) &&
           txn->put(COUNT_KEY, count.str(), status) &&
           txn->put(DIGEST_KEY, digestValue, status) &&
           txn->put(FORMAT_KEY, FORMAT_VALUE, status) &&
           txn->commit(status);
    }
  if (!ok)
    {
      LocalLiteStorageStatus ignored;
      txn->rollback(&ignored);
    }
  delete txn;
  delete session;
  return ok && LocalLiteVerifyMigratedStore(target, status);
}

bool LocalLiteVerifyMigratedStore(LocalLiteStorageEngine *target,
                                  LocalLiteStorageStatus *status)
{
  LocalLiteStorageSession *session = target->createSession(status);
  if (!session)
    return false;
  LocalLiteStorageTxn *txn = session->begin(status);
  if (!txn)
    {
      delete session;
      return false;
    }
  std::string format;
  std::string expectedCountText;
  std::string expectedDigest;
  bool ok = readRequired(txn, FORMAT_KEY, &format, status) &&
      readRequired(txn, COUNT_KEY, &expectedCountText, status) &&
      readRequired(txn, DIGEST_KEY, &expectedDigest, status) &&
      format == FORMAT_VALUE;
  const uint64_t expectedCount = ok
      ? strtoull(expectedCountText.c_str(), NULL, 10) : 0;
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  EVP_DigestInit_ex(digest, EVP_sha256(), NULL);
  uint64_t actualCount = 0;
  if (ok)
    {
      LocalLiteStorageCursor *cursor =
          txn->scan("legacy/", "legacy0", status);
      ok = cursor != NULL;
      while (ok)
        {
          LocalLiteStorageRecord record;
          bool end = false;
          if (!cursor->next(&record, &end, status))
            {
              ok = false;
              break;
            }
          if (end)
            break;
          digestField(digest, record.key);
          digestField(digest, record.value);
          actualCount++;
        }
      delete cursor;
    }
  const std::string actualDigest = ok ? finishDigest(digest) : std::string();
  EVP_MD_CTX_free(digest);
  if (ok && (actualCount != expectedCount || actualDigest != expectedDigest))
    {
      setMigrationError(status, LOCAL_LITE_STORAGE_CORRUPTION,
                        "migrated store count or SHA-256 mismatch");
      ok = false;
    }
  LocalLiteStorageStatus ignored;
  txn->rollback(&ignored);
  delete txn;
  delete session;
  return ok;
}
