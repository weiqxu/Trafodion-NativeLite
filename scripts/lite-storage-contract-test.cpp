#include "LiteStorage.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

void fail(const std::string &message,
          const LiteStorageStatus *status = NULL)
{
  std::cerr << "FAIL: " << message;
  if (status && !status->message.empty())
    std::cerr << ": " << status->message;
  std::cerr << std::endl;
  std::exit(1);
}

LiteStorageTxn *begin(LiteStorageSession *session)
{
  LiteStorageStatus status;
  LiteStorageTxn *txn = session->begin(&status);
  if (!txn)
    fail("begin transaction", &status);
  return txn;
}

void expectValue(LiteStorageTxn *txn,
                 const std::string &key,
                 const std::string &expected)
{
  LiteStorageStatus status;
  std::string value;
  bool found = false;
  if (!txn->get(key, &value, &found, &status))
    fail("read " + key, &status);
  if (!found || value != expected)
    fail("unexpected value for " + key + " (expected " + expected +
         ", got " + (found ? value : "<missing>") + ")");
}

void expectMissing(LiteStorageTxn *txn, const std::string &key)
{
  LiteStorageStatus status;
  std::string value;
  bool found = false;
  if (!txn->get(key, &value, &found, &status))
    fail("read missing " + key, &status);
  if (found)
    fail("unexpected value for " + key);
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 6)
    {
      std::cerr << "usage: storage-contract-test "
                   "rocksdb|sqlite DB CHECKPOINT BACKUP RESTORE"
                << std::endl;
      return 2;
    }

  LiteStorageStatus status;
  LiteStorageOptions options;
  options.synchronousCommit = true;
  options.lockTimeoutMillis = 100;
  LiteStorageEngine *engine = NULL;
  if (std::string(argv[1]) == "rocksdb")
    engine = LiteCreateRocksDBTransactionEngine();
  else if (std::string(argv[1]) == "sqlite")
    engine = LiteCreateSQLiteEngine();
  else
    fail("unknown storage backend " + std::string(argv[1]));
  if (!engine->open(argv[2], options, &status))
    fail("open engine", &status);

  LiteStorageSession *first = engine->createSession(&status);
  LiteStorageSession *second = engine->createSession(&status);
  if (!first || !second)
    fail("create storage sessions", &status);

  // Catalog, base row, unique key, and secondary index records commit in one
  // backend transaction and are invisible before commit.
  LiteStorageTxn *writer = begin(first);
  if (!writer->put("catalog/table/t", "definition-v1", &status) ||
      !writer->put("row/t/0001", "alpha", &status) ||
      !writer->put("unique/t/name/alpha", "row/t/0001", &status) ||
      !writer->put("index/t/name/alpha/0001", "row/t/0001", &status))
    fail("stage atomic record set", &status);

  LiteStorageTxn *observer = begin(second);
  expectMissing(observer, "catalog/table/t");
  expectMissing(observer, "row/t/0001");
  if (!observer->rollback(&status))
    fail("rollback observer", &status);
  delete observer;

  if (!writer->commit(&status))
    fail("commit atomic record set", &status);
  delete writer;

  LiteStorageTxn *reader = begin(second);
  expectValue(reader, "catalog/table/t", "definition-v1");
  expectValue(reader, "row/t/0001", "alpha");
  expectValue(reader, "unique/t/name/alpha", "row/t/0001");
  expectValue(reader, "index/t/name/alpha/0001", "row/t/0001");

  LiteStorageCursor *cursor =
      reader->scan("row/t/", "row/t0", &status);
  if (!cursor)
    fail("create bounded row cursor", &status);
  std::vector<std::string> rows;
  for (;;)
    {
      LiteStorageRecord record;
      bool end = false;
      if (!cursor->next(&record, &end, &status))
        fail("advance bounded row cursor", &status);
      if (end)
        break;
      rows.push_back(record.key + "=" + record.value);
    }
  delete cursor;
  if (rows.size() != 1 || rows[0] != "row/t/0001=alpha")
    fail("bounded cursor returned unexpected records");
  if (!reader->rollback(&status))
    fail("close reader snapshot", &status);
  delete reader;

  // A transaction snapshot remains stable across a concurrent commit.
  LiteStorageTxn *snapshotReader = begin(second);
  expectValue(snapshotReader, "row/t/0001", "alpha");
  LiteStorageTxn *updater = begin(first);
  if (!updater->put("row/t/0001", "alpha-v2", &status) ||
      !updater->commit(&status))
    fail("commit concurrent snapshot update", &status);
  delete updater;
  expectValue(snapshotReader, "row/t/0001", "alpha");
  if (!snapshotReader->rollback(&status))
    fail("close stable snapshot reader", &status);
  delete snapshotReader;

  // Pessimistic TransactionDB locking provides a stable retryable conflict
  // class instead of allowing last-writer-wins publication.
  LiteStorageTxn *lockOwner = begin(first);
  LiteStorageTxn *contender = begin(second);
  if (!lockOwner->put("row/t/conflict", "owner", &status))
    fail("acquire transaction lock", &status);
  if (contender->put("row/t/conflict", "contender", &status) ||
      status.code != LITE_STORAGE_CONFLICT || !status.retryable)
    fail("conflicting write did not return retryable conflict", &status);
  if (!contender->rollback(&status) || !lockOwner->rollback(&status))
    fail("rollback conflict transactions", &status);
  delete contender;
  delete lockOwner;

  LiteStorageTxn *aborted = begin(first);
  if (!aborted->put("row/t/0002", "beta", &status) ||
      !aborted->erase("row/t/0001", &status))
    fail("stage aborted record set", &status);
  if (!aborted->rollback(&status))
    fail("rollback record set", &status);
  delete aborted;

  reader = begin(second);
  expectValue(reader, "row/t/0001", "alpha-v2");
  expectMissing(reader, "row/t/0002");
  if (!reader->rollback(&status))
    fail("close rollback validation reader", &status);
  delete reader;

  LiteStorageTxn *cancelled = begin(first);
  cancelled->cancel();
  if (cancelled->put("row/t/0003", "gamma", &status) ||
      status.code != LITE_STORAGE_CANCELLED)
    fail("cancelled transaction accepted a write", &status);
  delete cancelled;

  if (!engine->verify(&status))
    fail("verify engine", &status);
  if (!engine->checkpoint(argv[3], &status))
    fail("create checkpoint", &status);
  if (!engine->backup(argv[4], &status))
    fail("create backup", &status);
  if (!engine->restore(argv[4], argv[5], &status))
    fail("restore backup", &status);

  LiteStorageMetrics metrics = engine->metrics();
  if (metrics.committedTransactions != 2 ||
      metrics.rolledBackTransactions < 7 || metrics.conflicts != 1 ||
      metrics.cancelledTransactions != 1 ||
      metrics.bytesWritten == 0 || metrics.bytesRead == 0)
    fail("storage metrics did not account for contract operations");

  delete second;
  delete first;
  engine->close();
  delete engine;

  LiteStorageEngine *restored = std::string(argv[1]) == "rocksdb"
      ? LiteCreateRocksDBTransactionEngine()
      : LiteCreateSQLiteEngine();
  options.createIfMissing = false;
  if (!restored->open(argv[5], options, &status))
    fail("open restored store", &status);
  LiteStorageSession *restoredSession =
      restored->createSession(&status);
  LiteStorageTxn *restoredReader = begin(restoredSession);
  expectValue(restoredReader, "catalog/table/t", "definition-v1");
  expectValue(restoredReader, "row/t/0001", "alpha-v2");
  expectValue(restoredReader, "unique/t/name/alpha", "row/t/0001");
  expectValue(restoredReader, "index/t/name/alpha/0001", "row/t/0001");
  if (!restoredReader->rollback(&status) || !restored->verify(&status))
    fail("verify restored store", &status);
  delete restoredReader;
  delete restoredSession;
  restored->close();
  delete restored;

  LiteStorageEngine *watermarked = std::string(argv[1]) == "rocksdb"
      ? LiteCreateRocksDBTransactionEngine()
      : LiteCreateSQLiteEngine();
  LiteStorageOptions watermarkOptions;
  watermarkOptions.minimumFreeBytes = UINT64_MAX;
  const std::string watermarkPath = std::string(argv[5]) + "-watermark";
  if (!watermarked->open(watermarkPath, watermarkOptions, &status))
    fail("open disk-watermark store", &status);
  LiteStorageSession *watermarkSession =
      watermarked->createSession(&status);
  if (!watermarkSession)
    fail("create disk-watermark session", &status);
  LiteStorageTxn *rejected = watermarkSession->begin(&status);
  if (rejected || status.code != LITE_STORAGE_NO_SPACE)
    fail("disk watermark did not reject a new transaction", &status);
  delete watermarkSession;
  watermarked->close();
  delete watermarked;
  std::cout << "Lite " << argv[1]
            << " storage contract checks passed" << std::endl;
  return 0;
}
