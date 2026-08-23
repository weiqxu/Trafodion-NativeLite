#include "LiteStorage.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

void fail(const std::string &message, const LiteStorageStatus &status)
{
  std::cerr << "FAIL: " << message;
  if (!status.message.empty())
    std::cerr << ": " << status.message;
  std::cerr << std::endl;
  std::exit(1);
}

uint64_t percentile(const std::vector<uint64_t> &sorted, size_t percent)
{
  const size_t index = ((sorted.size() - 1) * percent + 99) / 100;
  return sorted[index];
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 4)
    {
      std::cerr << "usage: storage-benchmark rocksdb|sqlite PATH TXNS"
                << std::endl;
      return 2;
    }
  const size_t transactionCount = std::strtoul(argv[3], NULL, 10);
  if (transactionCount < 10)
    return 2;

  LiteStorageEngine *engine = NULL;
  if (std::string(argv[1]) == "rocksdb")
    engine = LiteCreateRocksDBTransactionEngine();
  else if (std::string(argv[1]) == "sqlite")
    engine = LiteCreateSQLiteEngine();
  else
    return 2;

  LiteStorageStatus status;
  LiteStorageOptions options;
  options.synchronousCommit = true;
  options.lockTimeoutMillis = 1000;
  if (!engine->open(argv[2], options, &status))
    fail("open benchmark engine", status);
  LiteStorageSession *session = engine->createSession(&status);
  if (!session)
    fail("create benchmark session", status);

  std::vector<uint64_t> latencyMicros;
  latencyMicros.reserve(transactionCount);
  const std::chrono::steady_clock::time_point workloadStart =
      std::chrono::steady_clock::now();
  for (size_t transaction = 0; transaction < transactionCount; transaction++)
    {
      const std::chrono::steady_clock::time_point start =
          std::chrono::steady_clock::now();
      LiteStorageTxn *txn = session->begin(&status);
      if (!txn)
        fail("begin benchmark transaction", status);
      for (size_t record = 0; record < 4; record++)
        {
          std::ostringstream key;
          key << "row/bench/" << std::setw(8) << std::setfill('0')
              << transaction << "/" << record;
          const std::string value(128, static_cast<char>('a' + record));
          if (!txn->put(key.str(), value, &status))
            fail("write benchmark record", status);
        }
      if (!txn->commit(&status))
        fail("commit benchmark transaction", status);
      delete txn;
      const std::chrono::steady_clock::time_point finish =
          std::chrono::steady_clock::now();
      latencyMicros.push_back(
          std::chrono::duration_cast<std::chrono::microseconds>(
              finish - start).count());
    }
  const std::chrono::steady_clock::time_point workloadFinish =
      std::chrono::steady_clock::now();

  LiteStorageTxn *reader = session->begin(&status);
  if (!reader)
    fail("begin benchmark scan", status);
  LiteStorageCursor *cursor =
      reader->scan("row/bench/", "row/bench0", &status);
  if (!cursor)
    fail("create benchmark cursor", status);
  size_t rows = 0;
  for (;;)
    {
      LiteStorageRecord record;
      bool end = false;
      if (!cursor->next(&record, &end, &status))
        fail("advance benchmark cursor", status);
      if (end)
        break;
      rows++;
    }
  delete cursor;
  if (!reader->rollback(&status))
    fail("close benchmark reader", status);
  delete reader;
  if (rows != transactionCount * 4)
    fail("benchmark scan count mismatch", status);

  std::sort(latencyMicros.begin(), latencyMicros.end());
  const uint64_t elapsedMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(
          workloadFinish - workloadStart).count();
  const double transactionsPerSecond = elapsedMicros == 0 ? 0.0 :
      transactionCount * 1000000.0 / elapsedMicros;
  std::cout << "backend=" << argv[1]
            << " transactions=" << transactionCount
            << " p50_us=" << percentile(latencyMicros, 50)
            << " p95_us=" << percentile(latencyMicros, 95)
            << " p99_us=" << percentile(latencyMicros, 99)
            << " txn_per_sec=" << std::fixed << std::setprecision(2)
            << transactionsPerSecond << std::endl;

  delete session;
  engine->close();
  delete engine;
  return 0;
}
