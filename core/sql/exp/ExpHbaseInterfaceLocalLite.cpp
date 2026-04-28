/* -*-C++-*-
**********************************************************************/

#include "ex_stdh.h"
#include "ExpHbaseInterface.h"
#include "str.h"

#ifdef TRAF_LOCAL_LITE

class ExpHbaseInterface_LocalLite : public ExpHbaseInterface
{
public:
  ExpHbaseInterface_LocalLite(CollHeap *heap, const char *server, const char *zkPort)
    : ExpHbaseInterface(heap, server, zkPort)
  {}

  Lng32 init(ExHbaseAccessStats *hbs = NULL) { hbs_ = hbs; return HBASE_ACCESS_SUCCESS; }
  Lng32 cleanup() { return HBASE_ACCESS_SUCCESS; }
  Lng32 close() { return HBASE_ACCESS_SUCCESS; }
  Lng32 create(HbaseStr &, HBASE_NAMELIST &, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 create(HbaseStr &, NAText *, int, int, const char **, NABoolean, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 alter(HbaseStr &, NAText *, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 registerTruncateOnAbort(HbaseStr &, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 drop(HbaseStr &, NABoolean, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 truncate(HbaseStr &, NABoolean, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 dropAll(const char *, NABoolean, NABoolean) { return HBASE_ACCESS_ERROR; }
  NAArray<HbaseStr> *listAll(const char *) { return NULL; }
  Lng32 copy(HbaseStr &, HbaseStr &, NABoolean = FALSE) { return HBASE_ACCESS_ERROR; }
  Lng32 exists(HbaseStr &) { return HBASE_ACCESS_ERROR; }
  Lng32 getTable(HbaseStr &) { return HBASE_ACCESS_EOD; }
  Lng32 scanOpen(HbaseStr &, const Text &, const Text &, const LIST(HbaseStr) &,
                 const int64_t, const NABoolean, const NABoolean, const NABoolean,
                 const Lng32, const NABoolean, const LIST(NAString) *,
                 const LIST(NAString) *, const LIST(NAString) *, Float32 = 0.0f,
                 Float32 = -1.0f, NABoolean = FALSE, Lng32 = 0, char * = NULL,
                 char * = NULL, Lng32 = 0, Lng32 = 0) { return HBASE_ACCESS_ERROR; }
  Lng32 scanClose() { return HBASE_ACCESS_SUCCESS; }
  Lng32 isEmpty(HbaseStr &) { return HBASE_ACCESS_ERROR; }
  Lng32 getRowOpen(HbaseStr &, const HbaseStr &, const LIST(HbaseStr) &, const int64_t) { return HBASE_ACCESS_ERROR; }
  Lng32 getRowsOpen(HbaseStr &, const LIST(HbaseStr) *, const LIST(HbaseStr) &, const int64_t) { return HBASE_ACCESS_ERROR; }
  Lng32 nextRow() { return HBASE_ACCESS_EOD; }
  Lng32 nextCell(HbaseStr &, HbaseStr &, HbaseStr &, HbaseStr &, Int64 &) { return HBASE_ACCESS_EOD; }
  Lng32 completeAsyncOperation(Int32, NABoolean *, Int16) { return HBASE_ACCESS_ERROR; }
  Lng32 getColVal(int, BYTE *, Lng32 &, NABoolean, BYTE &) { return HBASE_ACCESS_ERROR; }
  Lng32 getColVal(NAHeap *, int, BYTE **, Lng32 &) { return HBASE_ACCESS_ERROR; }
  Lng32 getColName(int, char **, short &, Int64 &) { return HBASE_ACCESS_ERROR; }
  Lng32 getNumCellsPerRow(int &) { return HBASE_ACCESS_ERROR; }
  Lng32 getRowID(HbaseStr &) { return HBASE_ACCESS_ERROR; }
  Lng32 deleteRow(HbaseStr, HbaseStr, const LIST(HbaseStr) *, NABoolean, NABoolean, const int64_t, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 deleteRows(HbaseStr, short, HbaseStr, NABoolean, const int64_t, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 checkAndDeleteRow(HbaseStr &, HbaseStr &, HbaseStr &, HbaseStr &, NABoolean, NABoolean, const int64_t) { return HBASE_ACCESS_ERROR; }
  Lng32 deleteColumns(HbaseStr &, HbaseStr &) { return HBASE_ACCESS_ERROR; }
  Lng32 insertRow(HbaseStr, HbaseStr, HbaseStr, NABoolean, NABoolean, const int64_t, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 getRowsOpen(HbaseStr, short, HbaseStr, const LIST(HbaseStr) &) { return HBASE_ACCESS_ERROR; }
  Lng32 insertRows(HbaseStr, short, HbaseStr, HbaseStr, NABoolean, const int64_t, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 setWriteBufferSize(HbaseStr &, Lng32) { return HBASE_ACCESS_ERROR; }
  Lng32 setWriteToWAL(HbaseStr &, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 initHBLC(ExHbaseAccessStats *hbs = NULL) { hbs_ = hbs; return HBASE_ACCESS_ERROR; }
  Lng32 initHive() { return HBASE_ACCESS_ERROR; }
  Lng32 initHFileParams(HbaseStr &, Text &, Text &, Int64, const char *, const char *) { return HBASE_ACCESS_ERROR; }
  Lng32 addToHFile(short, HbaseStr &, HbaseStr &) { return HBASE_ACCESS_ERROR; }
  Lng32 closeHFile(HbaseStr &) { return HBASE_ACCESS_ERROR; }
  Lng32 doBulkLoad(HbaseStr &, Text &, Text &, NABoolean, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 bulkLoadCleanup(HbaseStr &, Text &) { return HBASE_ACCESS_ERROR; }
  Lng32 incrCounter(const char *, const char *, const char *, const char *, Int64, Int64 &) { return HBASE_ACCESS_ERROR; }
  Lng32 createCounterTable(const char *, const char *) { return HBASE_ACCESS_ERROR; }
  Lng32 checkAndInsertRow(HbaseStr &, HbaseStr &, HbaseStr &, NABoolean, NABoolean, const int64_t, NABoolean, Int16) { return HBASE_ACCESS_ERROR; }
  Lng32 checkAndUpdateRow(HbaseStr &, HbaseStr &, HbaseStr &, HbaseStr &, HbaseStr &, NABoolean, NABoolean, const int64_t, NABoolean) { return HBASE_ACCESS_ERROR; }
  Lng32 getClose() { return HBASE_ACCESS_SUCCESS; }
  Lng32 coProcAggr(HbaseStr &, Lng32, const Text &, const Text &, const Text &, const Text &, const NABoolean, const Lng32, Text &) { return HBASE_ACCESS_ERROR; }
  Lng32 grant(const Text &, const Text &, const std::vector<Text> &) { return HBASE_ACCESS_ERROR; }
  Lng32 revoke(const Text &, const Text &, const std::vector<Text> &) { return HBASE_ACCESS_ERROR; }
  NAArray<HbaseStr> *getRegionBeginKeys(const char *) { return NULL; }
  NAArray<HbaseStr> *getRegionEndKeys(const char *) { return NULL; }
  Lng32 estimateRowCount(HbaseStr &, Int32, Int32, Int32, NABoolean, Int64 &, Int32 &) { return HBASE_ACCESS_ERROR; }
  Lng32 cleanSnpTmpLocation(const char *) { return HBASE_ACCESS_ERROR; }
  Lng32 setArchivePermissions(const char *) { return HBASE_ACCESS_ERROR; }
  Lng32 getBlockCacheFraction(float &) { return HBASE_ACCESS_ERROR; }
  Lng32 getHbaseTableInfo(const HbaseStr &, Int32 &, Int32 &) { return HBASE_ACCESS_ERROR; }
  Lng32 getRegionsNodeName(const HbaseStr &, Int32, ARRAY(const char *) &) { return HBASE_ACCESS_ERROR; }
  NAArray<HbaseStr> *getRegionStats(const HbaseStr &) { return NULL; }
  NAArray<HbaseStr> *getClusterStats(Int32 &numEntries) { numEntries = 0; return NULL; }
  Lng32 createSnapshot(const NAString &, const NAString &) { return HBASE_ACCESS_ERROR; }
  Lng32 deleteSnapshot(const NAString &) { return HBASE_ACCESS_ERROR; }
  Lng32 verifySnapshot(const NAString &, const NAString &, NABoolean &exist) { exist = FALSE; return HBASE_ACCESS_ERROR; }
};

ExpHbaseInterface::ExpHbaseInterface(CollHeap *heap,
                                     const char *server,
                                     const char *zkPort)
{
  heap_ = heap;
  hbs_ = NULL;

  if ((server) && (strlen(server) <= MAX_SERVER_SIZE))
    strcpy(server_, server);
  else
    server_[0] = 0;

  if ((zkPort) && (strlen(zkPort) <= MAX_PORT_SIZE))
    strcpy(zkPort_, zkPort);
  else
    zkPort_[0] = 0;
}

ExpHbaseInterface *ExpHbaseInterface::newInstance(CollHeap *heap,
                                                  const char *server,
                                                  const char *zkPort)
{
  return new (heap) ExpHbaseInterface_LocalLite(heap, server, zkPort);
}

Lng32 ExpHbaseInterface::fetchAllRows(HbaseStr &, Lng32, HbaseStr &, HbaseStr &,
                                      HbaseStr &, LIST(NAString) &,
                                      LIST(NAString) &, LIST(NAString) &)
{
  return HBASE_ACCESS_ERROR;
}

Lng32 ExpHbaseInterface::copy(HbaseStr &, HbaseStr &, NABoolean)
{
  return HBASE_ACCESS_ERROR;
}

Lng32 ExpHbaseInterface::coProcAggr(HbaseStr &, Lng32, const Text &, const Text &,
                                    const Text &, const Text &, const NABoolean,
                                    const Lng32, Text &)
{
  return HBASE_ACCESS_ERROR;
}

NABoolean isParentQueryCanceled()
{
  return FALSE;
}

char *getHbaseErrStr(Lng32 errEnum)
{
  if ((errEnum >= HBASE_MIN_ERROR_NUM) && (errEnum <= HBASE_MAX_ERROR_NUM))
    return (char *)hbaseErrorEnumStr[errEnum - HBASE_MIN_ERROR_NUM];
  return (char *)"HBASE_ACCESS_ERROR";
}

#endif
