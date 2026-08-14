#include "LocalLiteStorage.h"

#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

LocalLiteStorageEngine *createEngine(const std::string &backend)
{
  if (backend == "rocksdb")
    return LocalLiteCreateRocksDBTransactionEngine();
  if (backend == "sqlite")
    return LocalLiteCreateSQLiteEngine();
  return NULL;
}

void fail(const std::string &message, const LocalLiteStorageStatus &status)
{
  std::cerr << "FAIL: " << message;
  if (!status.message.empty())
    std::cerr << ": " << status.message;
  std::cerr << std::endl;
  std::exit(1);
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 4)
    return 2;
  const std::string backend(argv[1]);
  const std::string mode(argv[2]);
  LocalLiteStorageEngine *engine = createEngine(backend);
  if (!engine)
    return 2;
  LocalLiteStorageOptions options;
  options.synchronousCommit = true;
  LocalLiteStorageStatus status;
  if (!engine->open(argv[3], options, &status))
    fail("open crash-test engine", status);
  LocalLiteStorageSession *session = engine->createSession(&status);
  if (!session)
    fail("create crash-test session", status);

  if (mode == "committed" || mode == "uncommitted")
    {
      const bool commit = mode == "committed";
      const std::string rowKey = commit ? "row/crash/0001"
                                        : "row/crash/uncommitted";
      const std::string indexKey = commit ? "index/crash/0001"
                                          : "index/crash/uncommitted";
      LocalLiteStorageTxn *txn = session->begin(&status);
      if (!txn ||
          !txn->put("catalog/crash/table", "definition", &status) ||
          !txn->put(rowKey, mode, &status) ||
          !txn->put(indexKey, rowKey, &status))
        fail("stage crash-test transaction", status);
      if (commit && !txn->commit(&status))
        fail("commit crash-test transaction", status);
      // Model SIGKILL: skip transaction/session/engine destructors and SQLite
      // close or RocksDB WAL cleanup. Synchronous commit must survive; an
      // uncommitted write set must disappear.
      _exit(commit ? 0 : 77);
    }

  if (mode == "fault-commit")
    {
      LocalLiteStorageTxn *txn = session->begin(&status);
      if (!txn ||
          !txn->put("catalog/fault/table", "definition", &status) ||
          !txn->put("row/fault/0001", "value", &status) ||
          !txn->put("index/fault/0001", "row/fault/0001", &status) ||
          !txn->commit(&status))
        fail("execute fault-boundary transaction", status);
      fail("fault-boundary transaction did not terminate", status);
    }

  if (mode == "checkpoint" || mode == "backup")
    {
      const std::string destination = std::string(argv[3]) + "-" + mode;
      const bool ok = mode == "checkpoint"
          ? engine->checkpoint(destination, &status)
          : engine->backup(destination, &status);
      if (!ok)
        fail("execute faulted " + mode, status);
      fail(mode + " fault did not terminate", status);
    }

  if (mode != "verify" && mode != "verify-fault-absent" &&
      mode != "verify-fault-present")
    return 2;
  LocalLiteStorageTxn *txn = session->begin(&status);
  if (!txn)
    fail("begin crash verification", status);
  std::string value;
  bool found = false;
  if (!txn->get("catalog/crash/table", &value, &found, &status) ||
      !found || value != "definition")
    fail("committed catalog record did not recover", status);
  if (!txn->get("row/crash/0001", &value, &found, &status) ||
      !found || value != "committed")
    fail("committed row did not recover", status);
  if (!txn->get("row/crash/uncommitted", &value, &found, &status) || found)
    fail("uncommitted row became visible", status);
  if (mode != "verify")
    {
      const bool expected = mode == "verify-fault-present";
      const char *keys[] = {"catalog/fault/table", "row/fault/0001",
                            "index/fault/0001"};
      for (size_t i = 0; i < 3; i++)
        {
          if (!txn->get(keys[i], &value, &found, &status) ||
              found != expected)
            fail(expected ? "fault commit lost a record"
                          : "pre-commit fault leaked a record", status);
        }
    }
  if (!txn->rollback(&status) || !engine->verify(&status))
    fail("verify recovered store", status);
  delete txn;
  delete session;
  engine->close();
  delete engine;
  return 0;
}
