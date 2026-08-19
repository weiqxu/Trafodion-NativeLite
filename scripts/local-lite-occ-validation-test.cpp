#include "LocalLiteRocksDBStore.h"

#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

bool LocalLiteBuildPrimaryKey(const LocalLiteTableDef &,
                              const std::string &,
                              std::string *,
                              std::string *error)
{
  if (error) *error = "primary key codec is not used by OCC probe";
  return false;
}

bool LocalLiteBuildUniqueKey(const LocalLiteTableDef &,
                             const std::string &,
                             const std::vector<size_t> &,
                             size_t,
                             std::string *,
                             bool *hasKey,
                             std::string *error)
{
  if (hasKey) *hasKey = false;
  if (error) error->clear();
  return true;
}

bool LocalLiteBuildOrderedSecondaryKeyPayload(
    const LocalLiteTableDef &,
    const LocalLiteIndexDef &,
    const std::string &encodedRow,
    std::string *payload,
    bool *hasKey,
    bool *containsNull,
    std::string *error)
{
  if (payload) *payload = encodedRow;
  if (hasKey) *hasKey = true;
  if (containsNull) *containsNull = false;
  if (error) error->clear();
  return true;
}

class Context
{
public:
  Context() : value(LocalLiteTxnManager::createContext()) {}
  ~Context() { LocalLiteTxnManager::destroyContext(value); }
  LocalLiteTxnContext *value;
};

static std::string rowKey(uint64_t rowId)
{
  std::string key;
  for (int shift = 56; shift >= 0; shift -= 8)
    key += static_cast<char>((rowId >> shift) & 0xff);
  return key;
}

static std::string indexPrefix(uint64_t indexUid, const std::string &value)
{
  std::string key("I");
  for (int shift = 56; shift >= 0; shift -= 8)
    key += static_cast<char>((indexUid >> shift) & 0xff);
  key += value;
  return key;
}

static bool begin(Context &context)
{
  std::string error;
  if (LocalLiteTxnManager::begin(context.value, &error)) return true;
  fprintf(stderr, "begin failed: %s\n", error.c_str());
  return false;
}

static bool commit(Context &context, std::string *error)
{
  return LocalLiteTxnManager::commit(context.value, error);
}

static bool update(LocalLiteRocksDBStore *store,
                   Context &context,
                   const LocalLiteTableDef &table,
                   uint64_t rowId,
                   const std::string &after)
{
  LocalLiteTxn txn(store, context.value);
  LocalLiteRow before;
  bool found = false;
  std::string error;
  if (!txn.getRowByKey(table, rowKey(rowId), &before, &found, &error) ||
      !found)
    {
      fprintf(stderr, "point read failed: %s\n", error.c_str());
      return false;
    }
  LocalLiteRowMutation mutation;
  mutation.before = before;
  mutation.after = after;
  std::vector<LocalLiteRowMutation> mutations(1, mutation);
  if (!txn.updateRows(table, mutations, &error))
    {
      fprintf(stderr, "update failed: %s\n", error.c_str());
      return false;
    }
  return true;
}

static bool insert(LocalLiteRocksDBStore *store,
                   Context &context,
                   const LocalLiteTableDef &table,
                   const std::string &value,
                   uint64_t expectedRowId)
{
  LocalLiteTxn txn(store, context.value);
  uint64_t rowId = 0;
  std::string error;
  if (!txn.insertRow(table, value, &rowId, &error) || rowId != expectedRowId)
    {
      fprintf(stderr, "insert failed: %s row=%llu expected=%llu\n",
              error.c_str(), static_cast<unsigned long long>(rowId),
              static_cast<unsigned long long>(expectedRowId));
      return false;
    }
  return true;
}

static bool expectConflict(Context &context, const char *caseName)
{
  std::string error;
  if (commit(context, &error))
    {
      fprintf(stderr, "%s unexpectedly committed\n", caseName);
      return false;
    }
  if (error.find("SQLSTATE 40001") == std::string::npos ||
      error.find("restart transaction") == std::string::npos)
    {
      fprintf(stderr, "%s returned unclassified error: %s\n",
              caseName, error.c_str());
      return false;
    }
  return true;
}

static uint64_t metric(const std::string &json, const std::string &name)
{
  const std::string marker = "\"" + name + "\":";
  const size_t position = json.find(marker);
  if (position == std::string::npos)
    return 0;
  return strtoull(json.c_str() + position + marker.size(), NULL, 10);
}

int main()
{
  LocalLiteTableDef a;
  a.catalog = "TRAFODION";
  a.schema = "SEABASE";
  a.name = "OCC_A";
  a.objectUid = 7101;
  LocalLiteColumnDef column;
  column.name = "V";
  column.type = "VARCHAR(32)";
  column.nullable = false;
  a.columns.push_back(column);
  LocalLiteTableDef b = a;
  b.name = "OCC_B";
  b.objectUid = 7102;
  LocalLiteTableDef indexed = a;
  indexed.name = "OCC_INDEXED";
  indexed.objectUid = 7103;
  LocalLiteIndexDef index;
  index.name = "OCC_INDEXED_IX";
  index.objectUid = 7201;
  index.keyEncodingVersion = 2;
  index.keyColumns.push_back(0);
  index.descending.push_back(false);
  indexed.secondaryIndexes.push_back(index);
  LocalLiteTableDef parallelA = a;
  parallelA.name = "OCC_PARALLEL_A";
  parallelA.objectUid = 7104;
  LocalLiteTableDef parallelB = a;
  parallelB.name = "OCC_PARALLEL_B";
  parallelB.objectUid = 7105;

  LocalLiteRocksDBStore setup;
  std::string error;
  uint64_t rowId = 0;
  if (!setup.createTable(a, &error) ||
      !setup.insertRow(a, "a0", &rowId, &error) || rowId != 1 ||
      !setup.createTable(b, &error) ||
      !setup.insertRow(b, "b0", &rowId, &error) || rowId != 1 ||
      !setup.createTable(parallelA, &error) ||
      !setup.insertRow(parallelA, "pa0", &rowId, &error) || rowId != 1 ||
      !setup.createTable(parallelB, &error) ||
      !setup.insertRow(parallelB, "pb0", &rowId, &error) || rowId != 1 ||
      !setup.createTable(indexed, &error) ||
      !setup.insertRow(indexed, "a", &rowId, &error) || rowId != 1 ||
      !setup.insertRow(indexed, "b", &rowId, &error) || rowId != 2)
    {
      fprintf(stderr, "setup failed: %s\n", error.c_str());
      return 1;
    }

  // Disjoint read/write sets may overlap in the physical RocksDB commit while
  // each transaction retains one atomic batch and OCC visibility boundary.
  std::atomic<int> ready(0);
  std::atomic<bool> go(false);
  bool parallelPassed[2] = {false, false};
  setenv("TRAF_LOCAL_LITE_COMMIT_HOLD_MS", "100", 1);
  std::thread parallelFirst([&]() {
    Context context;
    LocalLiteRocksDBStore store;
    std::string threadError;
    if (!begin(context) || !update(&store, context, parallelA, 1, "pa1"))
      return;
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    parallelPassed[0] = commit(context, &threadError);
  });
  std::thread parallelSecond([&]() {
    Context context;
    LocalLiteRocksDBStore store;
    std::string threadError;
    if (!begin(context) || !update(&store, context, parallelB, 1, "pb1"))
      return;
    ready.fetch_add(1);
    while (!go.load()) std::this_thread::yield();
    parallelPassed[1] = commit(context, &threadError);
  });
  while (ready.load() != 2) std::this_thread::yield();
  go.store(true);
  parallelFirst.join();
  parallelSecond.join();
  unsetenv("TRAF_LOCAL_LITE_COMMIT_HOLD_MS");
  if (!parallelPassed[0] || !parallelPassed[1] ||
      metric(LocalLiteOccMetricsJson(),
             "maximum_physical_commit_overlap") < 2)
    {
      fprintf(stderr, "disjoint physical commits did not overlap: %s\n",
              LocalLiteOccMetricsJson().c_str());
      return 1;
    }

  // Independent tables begin at the same sequence and must both commit.
  Context first;
  Context second;
  LocalLiteRocksDBStore firstStore;
  LocalLiteRocksDBStore secondStore;
  if (!begin(first) || !begin(second) ||
      !update(&firstStore, first, a, 1, "a-independent") ||
      !update(&secondStore, second, b, 1, "b-independent") ||
      !commit(first, &error) || !commit(second, &error))
    {
      fprintf(stderr, "independent OCC commit failed: %s\n", error.c_str());
      return 1;
    }

  // Same-key blind/update writers are serialized by writes-as-reads.
  Context sameFirst;
  Context sameSecond;
  LocalLiteRocksDBStore sameFirstStore;
  LocalLiteRocksDBStore sameSecondStore;
  if (!begin(sameFirst) || !begin(sameSecond) ||
      !update(&sameFirstStore, sameFirst, a, 1, "a-winner") ||
      !update(&sameSecondStore, sameSecond, a, 1, "a-loser") ||
      !commit(sameFirst, &error) ||
      !expectConflict(sameSecond, "same-key conflict"))
    return 1;

  // A full scan conflicts with a row inserted into the scanned table even
  // when the reader's own write targets an independent table.
  Context phantomReader;
  Context phantomWriter;
  LocalLiteRocksDBStore phantomReaderStore;
  LocalLiteRocksDBStore phantomWriterStore;
  if (!begin(phantomReader) || !begin(phantomWriter)) return 1;
  LocalLiteTxn scanTxn(&phantomReaderStore, phantomReader.value);
  std::vector<LocalLiteRow> rows;
  if (!scanTxn.scanRows(a, &rows, &error) || rows.size() != 1 ||
      !insert(&phantomWriterStore, phantomWriter, a, "a-phantom", 2) ||
      !commit(phantomWriter, &error) ||
      !update(&phantomReaderStore, phantomReader, b, 1, "b-phantom-reader") ||
      !expectConflict(phantomReader, "phantom conflict"))
    return 1;

  // A not-found point read protects that exact future key.
  Context missingReader;
  Context missingWriter;
  LocalLiteRocksDBStore missingReaderStore;
  LocalLiteRocksDBStore missingWriterStore;
  if (!begin(missingReader) || !begin(missingWriter)) return 1;
  LocalLiteTxn missingTxn(&missingReaderStore, missingReader.value);
  LocalLiteRow missing;
  bool found = true;
  if (!missingTxn.getRowByKey(a, rowKey(3), &missing, &found, &error) || found ||
      !insert(&missingWriterStore, missingWriter, a, "a-missing", 3) ||
      !commit(missingWriter, &error) ||
      !update(&missingReaderStore, missingReader, b, 1, "b-missing-reader") ||
      !expectConflict(missingReader, "missing-key conflict"))
    return 1;

  // Index reads use the transaction snapshot and overlay pending mutations
  // without recording a conservative base-table full scan.
  Context indexOverlay;
  LocalLiteRocksDBStore indexOverlayStore;
  if (!begin(indexOverlay)) return 1;
  LocalLiteTxn indexTxn(&indexOverlayStore, indexOverlay.value);
  rows.clear();
  if (!indexTxn.scanIndexPrefix(indexed, indexPrefix(index.objectUid, "a"),
                                &rows, &error) || rows.size() != 1 ||
      !update(&indexOverlayStore, indexOverlay, indexed, 1, "z") ||
      !insert(&indexOverlayStore, indexOverlay, indexed, "ax", 3))
    return 1;
  rows.clear();
  if (!indexTxn.scanIndexPrefix(indexed, indexPrefix(index.objectUid, "a"),
                                &rows, &error) || rows.size() != 1 ||
      rows[0].value != "ax")
    {
      fprintf(stderr, "transactional index overlay returned wrong rows\n");
      return 1;
    }
  LocalLiteOccState indexState;
  if (!LocalLiteTxnManager::occState(indexOverlay.value, &indexState) ||
      indexState.indexRangeReads < 2 || indexState.fullScans != 0 ||
      !LocalLiteTxnManager::rollback(indexOverlay.value, &error))
    {
      fprintf(stderr, "transactional index range was not tracked precisely\n");
      return 1;
    }

  // An empty index range still protects against a matching phantom.
  Context indexReader;
  Context indexWriter;
  LocalLiteRocksDBStore indexReaderStore;
  LocalLiteRocksDBStore indexWriterStore;
  if (!begin(indexReader) || !begin(indexWriter)) return 1;
  LocalLiteTxn emptyIndexTxn(&indexReaderStore, indexReader.value);
  rows.clear();
  if (!emptyIndexTxn.scanIndexPrefix(indexed,
                                     indexPrefix(index.objectUid, "q"),
                                     &rows, &error) || !rows.empty() ||
      !insert(&indexWriterStore, indexWriter, indexed, "q", 3) ||
      !commit(indexWriter, &error) ||
      !update(&indexReaderStore, indexReader, b, 1, "b-index-reader") ||
      !expectConflict(indexReader, "index phantom conflict"))
    return 1;

  // DDL publishes a whole-table write event. A transaction that read the
  // dropped object cannot serialize a later write as if the object survived.
  Context ddlReader;
  LocalLiteRocksDBStore ddlReaderStore;
  if (!begin(ddlReader)) return 1;
  LocalLiteTxn ddlScan(&ddlReaderStore, ddlReader.value);
  rows.clear();
  if (!ddlScan.scanRows(a, &rows, &error) || rows.size() != 3 ||
      !setup.dropTable(a.catalog, a.schema, a.name, &error) ||
      !update(&ddlReaderStore, ddlReader, b, 1, "b-ddl-reader") ||
      !expectConflict(ddlReader, "DDL conflict"))
    return 1;

  const std::string metrics = LocalLiteOccMetricsJson();
  if (metric(metrics, "point_validation_conflicts") < 2 ||
      metric(metrics, "range_validation_conflicts") < 1 ||
      metric(metrics, "full_scan_validation_conflicts") < 2)
    {
      fprintf(stderr, "OCC conflict classification is incomplete: %s\n",
              metrics.c_str());
      return 1;
    }

  printf("local-lite Trafodion OCC validation probe passed\n");
  return 0;
}
