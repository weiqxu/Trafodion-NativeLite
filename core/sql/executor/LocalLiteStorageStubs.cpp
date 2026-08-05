// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "Platform.h"
#include "ExHbaseAccess.h"
#include "ExHdfsScan.h"
#include "ExScheduler.h"
#include "LocalLiteRocksDBStore.h"
#include "LocalLiteRowCodec.h"
#include "ExpErrorEnums.h"
#include "ex_exe_stmt_globals.h"
#include "exp_clause_derived.h"
#include "exp_expr.h"
#include "exp_tuple_desc.h"
#include "hiveHook.h"
#include "sql_buffer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

static std::mutex localLiteScanRowMutex;
static std::map<std::string, std::deque<LocalLiteRow> >
  localLiteScanRows;

static Lng32 localLiteStorageDiagCode(const std::string &message)
{
  if (getenv("TEST_SCHEMA_NAME") != NULL)
    {
      if (message.find("duplicate local-lite") != std::string::npos)
        return -8102;
      if (message.find("referential integrity constraint") !=
          std::string::npos)
        return -8103;
      if (message.find("check constraint") != std::string::npos)
        return -8101;
    }
  return -EXE_INTERNAL_ERROR;
}

static std::string localLiteScanRowKey(const char *tableName,
                                       ExExeStmtGlobals *globals)
{
  char execution[40];
  snprintf(execution, sizeof(execution), "#%lu",
           static_cast<unsigned long>(globals->getExecutionCount()));
  return std::string(tableName ? tableName : "") + execution;
}

static void localLiteRememberScanRow(const std::string &key,
                                     const LocalLiteRow &row)
{
  std::lock_guard<std::mutex> lock(localLiteScanRowMutex);
  localLiteScanRows[key].push_back(row);
}

static bool localLiteTakeScanRow(const std::string &key, LocalLiteRow *row)
{
  std::lock_guard<std::mutex> lock(localLiteScanRowMutex);
  std::map<std::string, std::deque<LocalLiteRow> >::iterator it =
    localLiteScanRows.find(key);
  if (it == localLiteScanRows.end() || it->second.empty())
    {
      it = localLiteScanRows.end();
      for (std::map<std::string, std::deque<LocalLiteRow> >::iterator candidate =
             localLiteScanRows.begin();
           candidate != localLiteScanRows.end(); ++candidate)
        if (!candidate->second.empty())
          {
            if (it != localLiteScanRows.end())
              return false;
            it = candidate;
          }
      if (it == localLiteScanRows.end())
        return false;
    }
  *row = it->second.front();
  it->second.pop_front();
  if (it->second.empty())
    localLiteScanRows.erase(it);
  return true;
}

static void localLiteForgetScanRows(const std::string &key)
{
  std::lock_guard<std::mutex> lock(localLiteScanRowMutex);
  localLiteScanRows.erase(key);
}

static void localLiteTraceScan(const char *path, const char *tableName)
{
  const char *trace = getenv("TRAF_LOCAL_LITE_TRACE_SCAN");
  if (!trace || !trace[0])
    return;
  fprintf(stderr, "LOCAL_LITE_SCAN_%s table=%s\n",
          path ? path : "UNKNOWN",
          tableName ? tableName : "");
}

extern "C" const char *trafLocalLiteUnsupportedStorage()
{
  return "HDFS, Hive, and HBase are not supported in local-lite builds";
}

class LocalLiteUnsupportedHbaseTcb : public ex_tcb
{
public:
  LocalLiteUnsupportedHbaseTcb(const ComTdbHbaseAccess &tdb, ex_globals *globals)
    : ex_tcb(tdb, 1, globals)
  {
    allocateParentQueues(qparent_);
  }

  ~LocalLiteUnsupportedHbaseTcb()
  {
    freeResources();
  }

  void freeResources()
  {
    delete qparent_.up;
    delete qparent_.down;
    qparent_.up = NULL;
    qparent_.down = NULL;
  }

  ex_queue_pair getParentQueue() const { return qparent_; }
  Int32 numChildren() const { return 0; }
  const ex_tcb *getChild(Int32) const { return NULL; }

  ex_tcb_private_state *allocatePstates(Lng32 &numElems, Lng32 &pstateLength)
  {
    PstateAllocator<ex_tcb_private_state> pa;
    return pa.allocatePstates(this, numElems, pstateLength);
  }

  void registerSubtasks()
  {
    ex_tcb::registerSubtasks();
    ExScheduler *sched = getGlobals()->getScheduler();
    sched->registerInsertSubtask(ex_tcb::sWork, this, qparent_.down);
    sched->registerUnblockSubtask(ex_tcb::sWork, this, qparent_.up);
  }

  ExWorkProcRetcode work()
  {
    while (!qparent_.down->isEmpty())
      {
        if (qparent_.up->isFull())
          return WORK_OK;

        ex_queue_entry *down = qparent_.down->getHeadEntry();
        ex_queue_entry *up = qparent_.up->getTailEntry();

        if (down->downState.request != ex_queue::GET_NOMORE)
          {
            ComDiagsArea *diags =
              ComDiagsArea::allocate(getGlobals()->getDefaultHeap());
            *diags << DgSqlCode(-EXE_INTERNAL_ERROR)
                   << DgString0(trafLocalLiteUnsupportedStorage());

            up->setDiagsArea(diags);
            up->upState.status = ex_queue::Q_SQLERROR;
            up->upState.downIndex = qparent_.down->getHeadIndex();
            up->upState.parentIndex = down->downState.parentIndex;
            up->upState.setMatchNo(0);
            qparent_.up->insert();

            if (qparent_.up->isFull())
              return WORK_OK;
          }

        up = qparent_.up->getTailEntry();
        up->upState.status = ex_queue::Q_NO_DATA;
        up->upState.downIndex = qparent_.down->getHeadIndex();
        up->upState.parentIndex = down->downState.parentIndex;
        up->upState.setMatchNo(0);
        qparent_.up->insert();
        qparent_.down->removeHead();
      }
    return WORK_OK;
  }

private:
  ex_queue_pair qparent_;
};

static std::string localLiteTrim(const std::string &s)
{
  size_t b = 0;
  while (b < s.size() && isspace(static_cast<unsigned char>(s[b])))
    b++;
  size_t e = s.size();
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1])))
    e--;
  return s.substr(b, e - b);
}

static std::string localLiteUnquoteIdent(const std::string &s)
{
  std::string t = localLiteTrim(s);
  if (t.size() >= 2 && t[0] == '"' && t[t.size() - 1] == '"')
    return t.substr(1, t.size() - 2);
  return t;
}

static std::vector<std::string> localLiteSplitDottedName(const char *name)
{
  std::vector<std::string> parts;
  std::string curr;
  bool quoted = false;
  const char *p = name ? name : "";
  for (size_t i = 0; p[i]; i++)
    {
      char c = p[i];
      if (c == '"')
        quoted = !quoted;
      if (c == '.' && !quoted)
        {
          parts.push_back(localLiteUnquoteIdent(curr));
          curr.clear();
        }
      else
        curr += c;
    }
  parts.push_back(localLiteUnquoteIdent(curr));
  return parts;
}

static void localLiteTableNameParts(const char *name,
                                    std::string *catalog,
                                    std::string *schema,
                                    std::string *object)
{
  std::vector<std::string> parts = localLiteSplitDottedName(name);
  *catalog = "TRAFODION";
  *schema = "SEABASE";
  object->clear();
  if (parts.empty())
    return;

  *object = parts[parts.size() - 1];
  if (parts.size() >= 2)
    *schema = parts[parts.size() - 2];
  if (parts.size() >= 3)
    *catalog = parts[parts.size() - 3];
}

class LocalLiteHbaseScanTcb : public ex_tcb
{
public:
  LocalLiteHbaseScanTcb(const ComTdbHbaseAccess &tdb, ex_globals *globals)
    : ex_tcb(tdb, 1, globals),
      qparent_(),
      pool_(NULL),
      workAtp_(NULL),
      asciiRow_(NULL),
      convertRow_(NULL),
      matches_(0),
      started_(false),
      rowsLoaded_(false),
      rowIndex_(0)
  {
    Space *space = globals->getSpace();
    CollHeap *heap = globals->getDefaultHeap();

    pool_ = new(space) sql_buffer_pool(tdb.numBuffers_,
                                       tdb.bufferSize_,
                                       space,
                                       SqlBufferBase::NORMAL_);
    pool_->setStaticMode(TRUE);
    allocateParentQueues(qparent_);

    if (tdb.workCriDesc_)
      {
        workAtp_ = allocateAtp(tdb.workCriDesc_, space);
        if (tdb.asciiTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.asciiTuppIndex_), 0);
        if (tdb.convertTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.convertTuppIndex_), 0);
      }

    if (tdb.asciiRowLen_ > 0)
      asciiRow_ = new(heap) char[tdb.asciiRowLen_];
    if (tdb.convertRowLen_ > 0)
      convertRow_ = new(heap) char[tdb.convertRowLen_];

    if (tdb.convertExpr_)
      tdb.convertExpr_->fixup(0, getExpressionMode(), this, space, heap,
                              FALSE, globals);
    if (tdb.scanExpr_)
      tdb.scanExpr_->fixup(0, getExpressionMode(), this, space, heap,
                           FALSE, globals);
  }

  ~LocalLiteHbaseScanTcb()
  {
    freeResources();
  }

  void freeResources()
  {
    localLiteForgetScanRows(localLiteScanRowKey(
        scanTdb().getTableName(), getGlobals()->castToExExeStmtGlobals()));
    NADELETEBASICARRAY(asciiRow_, getGlobals()->getDefaultHeap());
    asciiRow_ = NULL;
    NADELETEBASICARRAY(convertRow_, getGlobals()->getDefaultHeap());
    convertRow_ = NULL;
    if (workAtp_)
      {
        deallocateAtp(workAtp_, getGlobals()->getSpace());
        workAtp_ = NULL;
      }
    delete pool_;
    pool_ = NULL;
    delete qparent_.up;
    qparent_.up = NULL;
    delete qparent_.down;
    qparent_.down = NULL;
  }

  ex_queue_pair getParentQueue() const { return qparent_; }
  Int32 numChildren() const { return 0; }
  const ex_tcb *getChild(Int32) const { return NULL; }

  ex_tcb_private_state *allocatePstates(Lng32 &numElems, Lng32 &pstateLength)
  {
    PstateAllocator<ex_tcb_private_state> pa;
    return pa.allocatePstates(this, numElems, pstateLength);
  }

  void registerSubtasks()
  {
    ex_tcb::registerSubtasks();
    ExScheduler *sched = getGlobals()->getScheduler();
    sched->registerInsertSubtask(ex_tcb::sWork, this, qparent_.down);
    sched->registerUnblockSubtask(ex_tcb::sWork, this, qparent_.up);
    sched->registerCancelSubtask(ex_tcb::sWork, this, qparent_.down);
  }

  ExWorkProcRetcode work()
  {
    while (!qparent_.down->isEmpty())
      {
        ex_queue_entry *down = qparent_.down->getHeadEntry();
        if (down->downState.request == ex_queue::GET_NOMORE)
          {
            if (sendDone())
              return WORK_OK;
            continue;
          }

        if (!started_)
          {
            matches_ = 0;
            rowIndex_ = 0;
            rows_.clear();
            rowsLoaded_ = false;
            started_ = true;
          }

        if (!rowsLoaded_)
          {
            std::string error;
            if (!loadRows(&error))
              {
                if (sendError(error))
                  return WORK_OK;
                if (sendDone())
                  return WORK_OK;
                continue;
              }
            rowsLoaded_ = true;
          }

        while (rowIndex_ < rows_.size())
          {
            if (qparent_.up->isFull())
              return WORK_OK;

            if ((down->downState.request == ex_queue::GET_N) &&
                (down->downState.requestValue == matches_))
              break;

            std::string error;
            Lng32 formattedLen = 0;
            bool pass = false;
            if (!formatAndEvaluate(rows_[rowIndex_++], &formattedLen,
                                   &pass, &error))
              {
                if (sendError(error))
                  return WORK_OK;
                if (sendDone())
                  return WORK_OK;
                return WORK_OK;
              }
            if (!pass)
              continue;

            short rc = 0;
            if (moveRowToUpQueue(convertRow_, formattedLen,
                                 rows_[rowIndex_ - 1], &rc))
              return WORK_OK;
          }

        if (sendDone())
          return WORK_OK;
      }
    return WORK_OK;
  }

private:
  const ComTdbHbaseAccess &scanTdb() const
  {
    return static_cast<const ComTdbHbaseAccess &>(tdb);
  }

  ComTdbHbaseAccess &scanTdb()
  {
    return const_cast<ComTdbHbaseAccess &>(
        static_cast<const ComTdbHbaseAccess &>(tdb));
  }

  bool loadRows(std::string *error)
  {
    std::string catalog;
    std::string schema;
    std::string object;
    localLiteTableNameParts(scanTdb().getTableName(),
                            &catalog, &schema, &object);
    if (object.empty())
      {
        *error = "invalid local-lite table name";
        return false;
      }

    if (!store_.loadTable(catalog, schema, object, &table_, error))
      return false;

    ExExeStmtGlobals *statementGlobals =
      getGlobals()->castToExExeStmtGlobals();
    LocalLiteTxn txn(&store_, statementGlobals,
                     statementGlobals->getExecutionCount());
    bool handledGetRows = false;
    bool handledIndexLookup = false;
    bool handledIndexRange = false;
    bool handledIndexBounded = false;
    if (!loadGetRows(&txn, &handledGetRows, &handledIndexLookup,
                     &handledIndexRange, &handledIndexBounded, error))
      return false;
    if (handledGetRows)
      {
        const char *scanPath = handledIndexBounded ? "INDEX_BOUNDED" :
            (handledIndexRange ? "INDEX_RANGE" :
             (handledIndexLookup ? "INDEX_EQ" : "GET_ROW"));
        localLiteTraceScan(scanPath, scanTdb().getTableName());
        return true;
      }

    localLiteTraceScan("FULL", scanTdb().getTableName());
    return txn.scanRows(table_, &rows_, error);
  }

  bool loadGetRows(LocalLiteTxn *txn,
                   bool *handled,
                   bool *handledIndexLookup,
                   bool *handledIndexRange,
                   bool *handledIndexBounded,
                   std::string *error)
  {
    *handled = false;
    *handledIndexLookup = false;
    *handledIndexRange = false;
    *handledIndexBounded = false;
    Queue *getRows = scanTdb().listOfGetRows();
    if (!getRows || getRows->numEntries() == 0)
      return true;

    std::vector<std::string> storageKeys;
    std::vector<std::string> indexPrefixes;
    std::vector< std::pair<std::string, std::string> > indexRanges;
    getRows->position();
    for (Lng32 i = 0; i < getRows->numEntries(); i++)
      {
        ComTdbHbaseAccess::HbaseGetRows *hgr =
          static_cast<ComTdbHbaseAccess::HbaseGetRows *>(getRows->getNext());
        if (!hgr || !hgr->rowIds() || hgr->rowIds()->numEntries() == 0)
          {
            *error = "local-lite get row id list is empty";
            return false;
          }

        Queue *rowIds = hgr->rowIds();
        rowIds->position();
        for (Lng32 j = 0; j < rowIds->numEntries(); j++)
          {
            const char *rawKey = static_cast<const char *>(rowIds->getNext());
            if (rawKey && strncmp(rawKey, "LLIB1:", 6) == 0)
              {
                std::string bounds(rawKey + 6);
                size_t separator = bounds.find(':');
                if (separator == std::string::npos)
                  {
                    *error = "invalid local-lite secondary index bounds";
                    return false;
                  }
                std::string startKey;
                std::string endKey;
                if (!decodeHexGetRowKey(
                        bounds.substr(0, separator).c_str(),
                        &startKey, error) ||
                    !decodeHexGetRowKey(
                        bounds.substr(separator + 1).c_str(),
                        &endKey, error))
                  return false;
                indexRanges.push_back(std::make_pair(startKey, endKey));
                *handledIndexRange = true;
                *handledIndexBounded = true;
                continue;
              }
            if (rawKey &&
                (strncmp(rawKey, "LLIX1:", 6) == 0 ||
                 strncmp(rawKey, "LLIR1:", 6) == 0))
              {
                if (strncmp(rawKey, "LLIR1:", 6) == 0)
                  *handledIndexRange = true;
                std::string prefix;
                if (!decodeHexGetRowKey(rawKey + 6, &prefix, error))
                  return false;
                indexPrefixes.push_back(prefix);
                continue;
              }
            std::string storageKey;
            if (!decodeGetRowKey(rawKey, &storageKey, error))
              return false;
            if (!isLocalLiteStorageKey(storageKey))
              return true;
            storageKeys.push_back(storageKey);
          }
      }

    *handled = true;
    if (!indexPrefixes.empty() || !indexRanges.empty())
      {
        if (!storageKeys.empty() ||
            indexPrefixes.size() + indexRanges.size() != 1)
          {
            *error = "invalid local-lite mixed secondary index lookup";
            return false;
          }
        *handledIndexLookup = true;
        // Pending transaction mutations have not reached the physical index.
        // A visible-row scan plus the retained executor predicate preserves
        // read-your-writes semantics until transactional index overlays exist.
        if (LocalLiteTxnManager::active())
          return txn->scanRows(table_, &rows_, error);
        if (!indexRanges.empty())
          return store_.scanIndexRange(table_, indexRanges[0].first,
                                       indexRanges[0].second,
                                       &rows_, error);
        return store_.scanIndexPrefix(table_, indexPrefixes[0],
                                      &rows_, error);
      }

    for (size_t i = 0; i < storageKeys.size(); i++)
      {
        LocalLiteRow row;
        bool found = false;
        if (!txn->getRowByKey(table_, storageKeys[i], &row, &found, error))
          return false;
        if (found)
          rows_.push_back(row);
      }
    return true;
  }

  bool decodeGetRowKey(const char *rawKey,
                       std::string *storageKey,
                       std::string *error)
  {
    if (!rawKey)
      {
        *error = "local-lite get row id is null";
        return false;
      }

    if (strncmp(rawKey, "LLPK1:", 6) == 0)
      return decodeHexGetRowKey(rawKey + 6, storageKey, error);

    if (rawKey[0] != 'P' && rawKey[0] != 'U')
      {
        short len = 0;
        memcpy(&len, rawKey, sizeof(len));
        if (len > 0 && len < 4096)
          {
            storageKey->assign(rawKey + sizeof(len),
                               static_cast<size_t>(len));
            return true;
          }
      }

    UInt32 fixedLen = scanTdb().getRowIDLen();
    if (fixedLen > 0)
      {
        storageKey->assign(rawKey, static_cast<size_t>(fixedLen));
        return true;
      }

    storageKey->assign(rawKey, strlen(rawKey));
    return true;
  }

  bool decodeHexGetRowKey(const char *hex,
                          std::string *storageKey,
                          std::string *error)
  {
    size_t len = strlen(hex);
    if ((len % 2) != 0)
      {
        *error = "invalid local-lite hex get row id length";
        return false;
      }

    storageKey->clear();
    storageKey->reserve(len / 2);
    for (size_t i = 0; i < len; i += 2)
      {
        int hi = decodeHexDigit(hex[i]);
        int lo = decodeHexDigit(hex[i + 1]);
        if (hi < 0 || lo < 0)
          {
            *error = "invalid local-lite hex get row id";
            return false;
          }
        storageKey->push_back(static_cast<char>((hi << 4) | lo));
      }
    return true;
  }

  int decodeHexDigit(char c)
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }

  bool isLocalLiteStorageKey(const std::string &storageKey)
  {
    if (storageKey.size() == 8)
      return true;
    return storageKey.size() > 0 &&
           (storageKey[0] == 'P' || storageKey[0] == 'U');
  }

  void deriveAlignedHeader(ExpTupleDesc *td,
                           UInt16 *firstFixedOffset,
                           UInt16 *bitmapEntryOffset,
                           UInt16 *bitmapOffset)
  {
    UInt32 first = scanTdb().asciiRowLen_;
    UInt32 bitmap = 0;
    for (UInt32 i = 0; i < td->numAttrs(); i++)
      {
        Attributes *attr = td->getAttr(i);
        if (!attr)
          continue;
        if (attr->getVCIndicatorLength() == 0 && attr->getOffset() < first)
          first = attr->getOffset();
        if (attr->getNullFlag() && attr->getNullIndOffset() > 0)
          bitmap = attr->getNullIndOffset();
      }
    if (first == scanTdb().asciiRowLen_)
      first = 8;

    *firstFixedOffset = static_cast<UInt16>(first);
    *bitmapOffset = static_cast<UInt16>(bitmap);
    *bitmapEntryOffset = 4;
  }

  bool formatAndEvaluate(const LocalLiteRow &row,
                         Lng32 *formattedLen,
                         bool *pass,
                         std::string *error)
  {
    ExpTupleDesc *asciiTd =
      scanTdb().workCriDesc_->getTupleDescriptor(scanTdb().asciiTuppIndex_);
    ExpTupleDesc *convertTd =
      scanTdb().workCriDesc_->getTupleDescriptor(scanTdb().convertTuppIndex_);
    if (!asciiTd || !convertTd)
      {
        *error = "local-lite missing scan tuple descriptor";
        return false;
      }

    std::vector<size_t> asciiSourceIndexes;
    if (!projectionSourceIndexes(asciiTd, &asciiSourceIndexes, error))
      return false;

    std::vector<size_t> convertSourceIndexes;
    if (!projectionSourceIndexes(convertTd, &convertSourceIndexes, error))
      return false;

    workAtp_->getTupp(scanTdb().asciiTuppIndex_).setDataPointer(asciiRow_);
    workAtp_->getTupp(scanTdb().convertTuppIndex_).setDataPointer(convertRow_);

    unsigned int asciiLen = 0;
    if (!LocalLiteProjectBinaryRow(table_, row.value, row.rowId,
                                   asciiSourceIndexes,
                                   asciiTd, asciiRow_,
                                   scanTdb().asciiRowLen_,
                                   &asciiLen, error))
      return false;

    unsigned int convertLen = 0;
    if (!LocalLiteProjectBinaryRow(table_, row.value, row.rowId,
                                   convertSourceIndexes,
                                   convertTd, convertRow_,
                                   scanTdb().convertRowLen_,
                                   &convertLen, error))
      return false;

    if (scanTdb().scanExpr_)
      {
        ex_expr::exp_return_type evalRetCode =
          scanTdb().scanExpr_->eval(downEntry()->getAtp(), workAtp_);
        if (evalRetCode == ex_expr::EXPR_ERROR)
          {
            *error = "local-lite scan predicate evaluation failed";
            return false;
          }
        if (evalRetCode == ex_expr::EXPR_FALSE)
          {
            *pass = false;
            *formattedLen = 0;
            return true;
          }
      }

    *pass = true;

    // HBase scan TCBs return the generated fixed convert row length to their
    // parent. Local-lite still projects variable columns into that row, but the
    // parent executor operators such as sort/group compare the generated tuple
    // shape, not the per-row adjusted payload length.
    *formattedLen = scanTdb().convertRowLen_;
    return true;
  }

  void writeVarLength(Attributes *attr,
                      char *row,
                      UInt32 lengthOffset,
                      UInt32 dataOffset,
                      const std::string &value,
                      UInt32 *nextLengthOffset,
                      UInt32 *nextDataOffset,
                      UInt32 *formattedLen)
  {
    UInt32 len = static_cast<UInt32>(value.size());
    if (len > static_cast<UInt32>(attr->getLength()))
      len = static_cast<UInt32>(attr->getLength());

    attr->setVarLength(len, row + lengthOffset);
    if (len > 0)
      str_cpy_all(row + dataOffset, value.data(), len);

    *nextLengthOffset = dataOffset + len;
    *nextDataOffset = *nextLengthOffset + attr->getVCIndicatorLength();
    *formattedLen = *nextLengthOffset;
  }

  bool formatDirectRow(const std::vector<std::string> &fields,
                       ExpTupleDesc *td,
                       char *row,
                       UInt32 *formattedLen,
                       std::string *error)
  {
    str_pad(row, scanTdb().convertRowLen_, '\0');

    UInt16 firstFixed = 0;
    UInt16 bitmapEntry = 0;
    UInt16 bitmap = 0;
    deriveAlignedHeader(td, &firstFixed, &bitmapEntry, &bitmap);
    ExpTupleDesc::setFirstFixedOffset(row, firstFixed,
                                      td->getTupleDataFormat());
    ExpAlignedFormat::setBitmapOffset(row + bitmapEntry, -bitmap);

    UInt32 rowLen = td->tupleDataLength();
    UInt32 voaOffset = 0;
    UInt32 lengthOffset = 0;
    UInt32 dataOffset = 0;
    bool firstVar = true;

    for (UInt32 i = 0; i < td->numAttrs(); i++)
      {
        Attributes *attr = td->getAttr(i);
        const std::string &field = fields[i];
        if (field.empty() && attr->getNullFlag())
          {
            ExpTupleDesc::setNullValue(row + attr->getNullIndOffset(),
                                       attr->getNullBitIndex(),
                                       td->getTupleDataFormat());
            continue;
          }
        if (attr->getNullFlag())
          ExpTupleDesc::clearNullValue(row + attr->getNullIndOffset(),
                                       attr->getNullBitIndex(),
                                       td->getTupleDataFormat());

        if (attr->getVCIndicatorLength() > 0)
          {
            if (firstVar)
              {
                voaOffset = attr->getVoaOffset();
                lengthOffset = attr->getVCLenIndOffset();
                dataOffset = lengthOffset + attr->getVCIndicatorLength();
                firstVar = false;
              }

            ExpTupleDesc::setVoaValue(row, voaOffset, lengthOffset,
                                      attr->getVCIndicatorLength());
            writeVarLength(attr, row, lengthOffset, dataOffset, field,
                           &lengthOffset, &dataOffset, &rowLen);
            ExpAlignedFormat::incrVoaOffset(voaOffset);
            continue;
          }

        char *target = row + attr->getOffset();
        switch (attr->getDatatype())
          {
          case REC_BIN8_SIGNED:
          case REC_BIN8_UNSIGNED:
            {
              Int8 v = static_cast<Int8>(strtol(field.c_str(), NULL, 10));
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              break;
            }
          case REC_BIN16_SIGNED:
          case REC_BIN16_UNSIGNED:
            {
              Int16 v = static_cast<Int16>(strtol(field.c_str(), NULL, 10));
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              break;
            }
          case REC_BIN32_SIGNED:
          case REC_BIN32_UNSIGNED:
            {
              Int32 v = static_cast<Int32>(strtol(field.c_str(), NULL, 10));
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              break;
            }
          case REC_BIN64_SIGNED:
          case REC_BIN64_UNSIGNED:
            {
              Int64 v = static_cast<Int64>(strtoll(field.c_str(), NULL, 10));
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              break;
            }
          case REC_BYTE_F_ASCII:
            {
              str_pad(target, attr->getLength(), ' ');
              UInt32 len = static_cast<UInt32>(field.size());
              if (len > static_cast<UInt32>(attr->getLength()))
                len = static_cast<UInt32>(attr->getLength());
              if (len > 0)
                str_cpy_all(target, field.data(), len);
              break;
            }
          default:
            *error = "unsupported local-lite scan column type";
            return false;
          }
      }

    rowLen = ExpAlignedFormat::adjustDataLength(row, rowLen,
                                                ExpAlignedFormat::ALIGNMENT,
                                                TRUE);
    *formattedLen = rowLen;
    return true;
  }

  bool decodeFetchedColumnIndex(const char *raw, size_t *index) const
  {
    if (!raw)
      return false;

    short len = 0;
    str_cpy_all(reinterpret_cast<char *>(&len), raw, sizeof(short));
    if (len <= 0)
      return false;

    const unsigned char *p =
      reinterpret_cast<const unsigned char *>(raw + sizeof(short));
    const unsigned char *end = p + len;
    while (p < end && *p != ':')
      p++;
    if (p == end)
      return false;
    p++;
    if (p < end && *p == '@')
      p++;
    if (p == end)
      return false;

    uint64_t qualifier = 0;
    unsigned shift = 0;
    while (p < end && shift < 64)
      {
        qualifier |= (static_cast<uint64_t>(*p) << shift);
        shift += 8;
        p++;
      }
    if (qualifier == 0)
      return false;

    *index = static_cast<size_t>(qualifier - 1);
    return true;
  }

  bool projectionSourceIndexes(ExpTupleDesc *td,
                               std::vector<size_t> *sourceIndexes,
                               std::string *error)
  {
    if (!td)
      {
        *error = "local-lite missing scan tuple descriptor";
        return false;
      }

    sourceIndexes->clear();
    Queue *fetched = scanTdb().listOfFetchedColNames();
    if (fetched && fetched->numEntries() >= td->numAttrs())
      {
        for (UInt32 i = 0; i < td->numAttrs(); i++)
          {
            size_t sourceIndex = 0;
            if (!decodeFetchedColumnIndex((char *)fetched->get(i),
                                          &sourceIndex))
              {
                *error = "local-lite scan projection does not match fetched columns";
                return false;
              }
            sourceIndexes->push_back(sourceIndex);
          }
        return true;
      }

    for (UInt32 i = 0; i < td->numAttrs(); i++)
      sourceIndexes->push_back(i);
    return true;
  }

  ex_queue_entry *downEntry()
  {
    return qparent_.down->getHeadEntry();
  }

  bool sendError(const std::string &message)
  {
    if (qparent_.up->isFull())
      return true;

    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    if (!up->getDiagsArea())
      {
        ComDiagsArea *diags =
          ComDiagsArea::allocate(getGlobals()->getDefaultHeap());
        Lng32 code = localLiteStorageDiagCode(message);
        *diags << DgSqlCode(code);
        if (code == -EXE_INTERNAL_ERROR)
          *diags << DgString0(message.c_str());
        up->setDiagsArea(diags);
      }
    up->upState.status = ex_queue::Q_SQLERROR;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return qparent_.up->isFull();
  }

  short moveRowToUpQueue(const char *row,
                         Lng32 len,
                         const LocalLiteRow &sourceRow,
                         short *rc)
  {
    if (qparent_.up->isFull())
      {
        if (rc)
          *rc = WORK_OK;
        return -1;
      }

    tupp p;
    if (pool_->get_free_tuple(p, len))
      {
        if (rc)
          *rc = WORK_POOL_BLOCKED;
        return -1;
      }
    str_cpy_all(p.getDataPointer(), row, len);
    localLiteRememberScanRow(localLiteScanRowKey(
        scanTdb().getTableName(), getGlobals()->castToExExeStmtGlobals()),
        sourceRow);

    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    up->getAtp()->getTupp(scanTdb().returnedTuppIndex_) = p;
    up->upState.status = ex_queue::Q_OK_MMORE;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(++matches_);
    qparent_.up->insert();
    return 0;
  }

  bool sendDone()
  {
    if (qparent_.up->isFull())
      return true;

    // A cancelled GET_N request must not leave fetched row handles behind.
    // Positioned DML consumes these handles, so retaining them after a
    // cancellation could associate a later statement with a stale cursor row.
    if (downEntry()->downState.request == ex_queue::GET_NOMORE)
      localLiteForgetScanRows(localLiteScanRowKey(
          scanTdb().getTableName(), getGlobals()->castToExExeStmtGlobals()));

    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    up->upState.status = ex_queue::Q_NO_DATA;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    qparent_.down->removeHead();
    started_ = false;
    rows_.clear();
    return false;
  }

  ex_queue_pair qparent_;
  sql_buffer_pool *pool_;
  atp_struct *workAtp_;
  char *asciiRow_;
  char *convertRow_;
  Lng32 matches_;
  bool started_;
  bool rowsLoaded_;
  size_t rowIndex_;
  LocalLiteRocksDBStore store_;
  LocalLiteTableDef table_;
  std::vector<LocalLiteRow> rows_;
};

class LocalLiteHbaseInsertTcb : public ex_tcb
{
public:
  LocalLiteHbaseInsertTcb(const ComTdbHbaseAccess &tdb, ex_globals *globals)
    : ex_tcb(tdb, 1, globals),
      qparent_(),
      pool_(NULL),
      workAtp_(NULL),
      convertRow_(NULL),
      matches_(0),
      pendingReturn_(FALSE),
      pendingDone_(FALSE)
  {
    Space *space = globals->getSpace();
    CollHeap *heap = globals->getDefaultHeap();

    allocateParentQueues(qparent_);
    pool_ = new(space) sql_buffer_pool(tdb.numBuffers_,
                                       tdb.bufferSize_,
                                       space,
                                       SqlBufferBase::NORMAL_);
    pool_->setStaticMode(TRUE);

    if (tdb.workCriDesc_)
      {
        workAtp_ = allocateAtp(tdb.workCriDesc_, space);
        if (tdb.convertTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.convertTuppIndex_), 0);
      }

    if (tdb.convertRowLen_ > 0)
      convertRow_ = new(heap) char[tdb.convertRowLen_];

    if (tdb.convertExpr_)
      tdb.convertExpr_->fixup(0, getExpressionMode(), this, space, heap,
                              FALSE, globals);
  }

  ~LocalLiteHbaseInsertTcb()
  {
    freeResources();
  }

  void freeResources()
  {
    NADELETEBASICARRAY(convertRow_, getGlobals()->getDefaultHeap());
    convertRow_ = NULL;
    if (workAtp_)
      {
        deallocateAtp(workAtp_, getGlobals()->getSpace());
        workAtp_ = NULL;
      }
    delete pool_;
    pool_ = NULL;
    delete qparent_.up;
    qparent_.up = NULL;
    delete qparent_.down;
    qparent_.down = NULL;
  }

  ex_queue_pair getParentQueue() const { return qparent_; }
  Int32 numChildren() const { return 0; }
  const ex_tcb *getChild(Int32) const { return NULL; }
  NABoolean isLocalLiteInsert() const { return TRUE; }

  ex_tcb_private_state *allocatePstates(Lng32 &numElems, Lng32 &pstateLength)
  {
    PstateAllocator<ex_tcb_private_state> pa;
    return pa.allocatePstates(this, numElems, pstateLength);
  }

  void registerSubtasks()
  {
    ex_tcb::registerSubtasks();
    ExScheduler *sched = getGlobals()->getScheduler();
    sched->registerInsertSubtask(ex_tcb::sWork, this, qparent_.down);
    sched->registerUnblockSubtask(ex_tcb::sWork, this, qparent_.up);
    sched->registerCancelSubtask(ex_tcb::sWork, this, qparent_.down);
  }

  ExWorkProcRetcode work()
  {
    while (!qparent_.down->isEmpty())
      {
        if (qparent_.up->isFull())
          return WORK_OK;

        ex_queue_entry *down = qparent_.down->getHeadEntry();
        if (pendingReturn_)
          {
            if (sendReturnedRow(down))
              return WORK_OK;
            pendingReturn_ = FALSE;
            pendingDone_ = TRUE;
            continue;
          }
        if (pendingDone_)
          {
            if (sendDone())
              return WORK_OK;
            pendingDone_ = FALSE;
            matches_ = 0;
            continue;
          }

        matches_ = 0;
        if (down->downState.request != ex_queue::GET_NOMORE)
          {
            std::string error;
            if (!loadTable(&error) ||
                !evaluateAndInsert(down, &error))
              {
                if (sendError(error))
                  return WORK_OK;
              }
          }
        if (insertTdb().returnRow() && matches_ > 0)
          pendingReturn_ = TRUE;
        else
          pendingDone_ = TRUE;
      }
    return WORK_OK;
  }

private:
  const ComTdbHbaseAccess &insertTdb() const
  {
    return static_cast<const ComTdbHbaseAccess &>(tdb);
  }

  ComTdbHbaseAccess &insertTdb()
  {
    return const_cast<ComTdbHbaseAccess &>(
        static_cast<const ComTdbHbaseAccess &>(tdb));
  }

  bool loadTable(std::string *error)
  {
    std::string catalog;
    std::string schema;
    std::string object;
    localLiteTableNameParts(insertTdb().getTableName(),
                            &catalog, &schema, &object);
    if (object.empty())
      {
        *error = "invalid local-lite table name";
        return false;
      }
    return store_.loadTable(catalog, schema, object, &table_, error);
  }

  bool evaluateAndInsert(ex_queue_entry *down, std::string *error)
  {
    if (!workAtp_ || !convertRow_ || insertTdb().convertRowLen_ <= 0)
      {
        *error = "local-lite insert missing executor row buffer";
        return false;
      }

    str_pad(convertRow_, insertTdb().convertRowLen_, '\0');
    workAtp_->getTupp(insertTdb().convertTuppIndex_).setDataPointer(convertRow_);

    ULng32 rowLen = static_cast<ULng32>(insertTdb().convertRowLen_);
    if (insertTdb().convertExpr_)
      {
        ex_expr::exp_return_type evalRetCode =
          insertTdb().convertExpr_->eval(down->getAtp(), workAtp_,
                                         NULL, -1, &rowLen);
        if (evalRetCode == ex_expr::EXPR_ERROR)
          {
            *error = "local-lite insert expression evaluation failed";
            return false;
          }
      }

    if (rowLen == 0)
      rowLen = static_cast<ULng32>(insertTdb().convertRowLen_);


    ExpTupleDesc *convertTd =
      insertTdb().workCriDesc_->getTupleDescriptor(insertTdb().convertTuppIndex_);
    std::string encodedRow;
    if (!LocalLiteNormalizeBinaryRow(table_, convertTd, convertRow_,
                                     static_cast<size_t>(rowLen),
                                     &encodedRow, error))
      return false;

    LocalLiteTxn txn(&store_);
    uint64_t rowId = 0;
    if (insertTdb().getAccessType() == ComTdbHbaseAccess::UPSERT_)
      {
        if (!txn.upsertRow(table_, encodedRow, &rowId, error))
          return false;
      }
    else if (!txn.insertRow(table_, encodedRow, &rowId, error))
      return false;

    matches_++;
    return true;
  }

  bool sendError(const std::string &message)
  {
    if (qparent_.up->isFull())
      return true;

    ex_queue_entry *down = qparent_.down->getHeadEntry();
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(down);
    if (!up->getDiagsArea())
      {
        ComDiagsArea *diags =
          ComDiagsArea::allocate(getGlobals()->getDefaultHeap());
        Lng32 code = localLiteStorageDiagCode(message);
        *diags << DgSqlCode(code);
        if (code == -EXE_INTERNAL_ERROR)
          *diags << DgString0(message.c_str());
        up->setDiagsArea(diags);
      }
    up->upState.status = ex_queue::Q_SQLERROR;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = down->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return qparent_.up->isFull();
  }

  bool sendReturnedRow(ex_queue_entry *down)
  {
    if (qparent_.up->isFull())
      return true;
    tupp returned;
    if (pool_->get_free_tuple(returned, insertTdb().convertRowLen_))
      return true;
    str_cpy_all(returned.getDataPointer(), convertRow_,
                insertTdb().convertRowLen_);
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(down);
    up->getAtp()->getTupp(insertTdb().returnedTuppIndex_) = returned;
    up->upState.status = ex_queue::Q_OK_MMORE;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = down->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return false;
  }

  bool sendDone()
  {
    if (qparent_.up->isFull())
      return true;

    ex_queue_entry *down = qparent_.down->getHeadEntry();
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(down);
    up->upState.status = ex_queue::Q_NO_DATA;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = down->downState.parentIndex;
    up->upState.setMatchNo(insertTdb().computeRowsAffected() ? matches_ : 0);

    if (matches_ > 0 && insertTdb().computeRowsAffected())
      {
        ExMasterStmtGlobals *g = getGlobals()->
          castToExExeStmtGlobals()->castToExMasterStmtGlobals();
        if (g)
          g->setRowsAffected(g->getRowsAffected() + matches_);
        else
          {
            ComDiagsArea *diags = up->getDiagsArea();
            if (!diags)
              {
                diags = ComDiagsArea::allocate(getGlobals()->getDefaultHeap());
                up->setDiagsArea(diags);
              }
            diags->addRowCount(matches_);
          }
      }

    qparent_.up->insert();
    qparent_.down->removeHead();
    return false;
  }

  ex_queue_pair qparent_;
  sql_buffer_pool *pool_;
  atp_struct *workAtp_;
  char *convertRow_;
  Lng32 matches_;
  NABoolean pendingReturn_;
  NABoolean pendingDone_;
  LocalLiteRocksDBStore store_;
  LocalLiteTableDef table_;
};

class LocalLiteHbaseDeleteTcb : public ex_tcb
{
public:
  LocalLiteHbaseDeleteTcb(const ComTdbHbaseAccess &tdb, ex_globals *globals)
    : ex_tcb(tdb, 1, globals),
      qparent_(),
      pool_(NULL),
      workAtp_(NULL),
      asciiRow_(NULL),
      convertRow_(NULL),
      matches_(0),
      pendingReturn_(FALSE),
      pendingDone_(FALSE)
  {
    Space *space = globals->getSpace();
    CollHeap *heap = globals->getDefaultHeap();
    allocateParentQueues(qparent_);
    pool_ = new(space) sql_buffer_pool(tdb.numBuffers_, tdb.bufferSize_,
                                       space, SqlBufferBase::NORMAL_);
    pool_->setStaticMode(TRUE);
    if (tdb.workCriDesc_)
      {
        workAtp_ = allocateAtp(tdb.workCriDesc_, space);
        if (tdb.asciiTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.asciiTuppIndex_), 0);
        if (tdb.convertTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.convertTuppIndex_), 0);
      }
    if (tdb.asciiRowLen_ > 0)
      asciiRow_ = new(heap) char[tdb.asciiRowLen_];
    if (tdb.convertRowLen_ > 0)
      convertRow_ = new(heap) char[tdb.convertRowLen_];
    if (tdb.scanExpr_)
      tdb.scanExpr_->fixup(0, getExpressionMode(), this, space, heap,
                           FALSE, globals);
    if (tdb.convertExpr_)
      tdb.convertExpr_->fixup(0, getExpressionMode(), this, space, heap,
                              FALSE, globals);
  }

  ~LocalLiteHbaseDeleteTcb() { freeResources(); }

  NABoolean isLocalLiteDelete() const { return TRUE; }

  void freeResources()
  {
    localLiteForgetScanRows(localLiteScanRowKey(
        deleteTdb().getTableName(), getGlobals()->castToExExeStmtGlobals()));
    NADELETEBASICARRAY(asciiRow_, getGlobals()->getDefaultHeap());
    NADELETEBASICARRAY(convertRow_, getGlobals()->getDefaultHeap());
    asciiRow_ = NULL;
    convertRow_ = NULL;
    if (workAtp_)
      {
        deallocateAtp(workAtp_, getGlobals()->getSpace());
        workAtp_ = NULL;
      }
    delete pool_;
    pool_ = NULL;
    delete qparent_.up;
    qparent_.up = NULL;
    delete qparent_.down;
    qparent_.down = NULL;
  }

  ex_queue_pair getParentQueue() const { return qparent_; }
  Int32 numChildren() const { return 0; }
  const ex_tcb *getChild(Int32) const { return NULL; }

  ex_tcb_private_state *allocatePstates(Lng32 &numElems, Lng32 &pstateLength)
  {
    PstateAllocator<ex_tcb_private_state> pa;
    return pa.allocatePstates(this, numElems, pstateLength);
  }

  void registerSubtasks()
  {
    ex_tcb::registerSubtasks();
    ExScheduler *sched = getGlobals()->getScheduler();
    sched->registerInsertSubtask(ex_tcb::sWork, this, qparent_.down);
    sched->registerUnblockSubtask(ex_tcb::sWork, this, qparent_.up);
    sched->registerCancelSubtask(ex_tcb::sWork, this, qparent_.down);
  }

  ExWorkProcRetcode work()
  {
    while (!qparent_.down->isEmpty())
      {
        if (qparent_.up->isFull())
          return WORK_OK;
        ex_queue_entry *down = qparent_.down->getHeadEntry();
        if (pendingReturn_)
          {
            if (sendReturnedRow())
              return WORK_OK;
            pendingReturn_ = FALSE;
            pendingDone_ = TRUE;
            continue;
          }
        if (pendingDone_)
          {
            if (sendDone())
              return WORK_OK;
            pendingDone_ = FALSE;
            matches_ = 0;
            continue;
          }
        matches_ = 0;
        if (down->downState.request != ex_queue::GET_NOMORE)
          {
            std::string error;
            if (!evaluateAndDelete(&error))
              {
                if (sendError(error))
                  return WORK_OK;
              }
          }
        if (deleteTdb().returnRow() && matches_ > 0)
          pendingReturn_ = TRUE;
        else
          pendingDone_ = TRUE;
      }
    return WORK_OK;
  }

private:
  const ComTdbHbaseAccess &deleteTdb() const
  {
    return static_cast<const ComTdbHbaseAccess &>(tdb);
  }

  ComTdbHbaseAccess &deleteTdb()
  {
    return const_cast<ComTdbHbaseAccess &>(
        static_cast<const ComTdbHbaseAccess &>(tdb));
  }

  ex_queue_entry *downEntry()
  {
    return qparent_.down->getHeadEntry();
  }

  bool decodeColumnIndex(const char *raw, size_t *index) const
  {
    if (!raw || !index)
      return false;
    short len = 0;
    str_cpy_all(reinterpret_cast<char *>(&len), raw, sizeof(short));
    if (len <= 0)
      return false;
    const unsigned char *p =
      reinterpret_cast<const unsigned char *>(raw + sizeof(short));
    const unsigned char *end = p + len;
    while (p < end && *p != ':')
      p++;
    if (p == end)
      return false;
    p++;
    if (p < end && *p == '@')
      p++;
    uint64_t qualifier = 0;
    unsigned shift = 0;
    while (p < end && shift < 64)
      {
        qualifier |= static_cast<uint64_t>(*p) << shift;
        shift += 8;
        p++;
      }
    if (qualifier == 0)
      return false;
    *index = static_cast<size_t>(qualifier - 1);
    return true;
  }

  bool sourceIndexes(ExpTupleDesc *td,
                     Queue *names,
                     std::vector<size_t> *indexes,
                     std::string *error) const
  {
    indexes->clear();
    if (!td)
      {
        *error = "local-lite delete missing tuple descriptor";
        return false;
      }
    if (names && names->numEntries() >= td->numAttrs())
      {
        for (UInt32 i = 0; i < td->numAttrs(); i++)
          {
            size_t index = 0;
            if (!decodeColumnIndex(static_cast<char *>(names->get(i)),
                                   &index))
              {
                *error = "local-lite delete column mapping is invalid";
                return false;
              }
            indexes->push_back(index);
          }
        return true;
      }
    for (UInt32 i = 0; i < td->numAttrs(); i++)
      indexes->push_back(i);
    return true;
  }

  bool loadTable(std::string *error)
  {
    std::string catalog;
    std::string schema;
    std::string object;
    localLiteTableNameParts(deleteTdb().getTableName(),
                            &catalog, &schema, &object);
    if (object.empty())
      {
        *error = "invalid local-lite table name";
        return false;
      }
    return store_.loadTable(catalog, schema, object, &table_, error);
  }

  bool evaluateAndDelete(std::string *error)
  {
    if (!workAtp_ || !asciiRow_ || !convertRow_ ||
        !deleteTdb().convertExpr_)
      {
        *error = "local-lite delete missing executor row buffers";
        return false;
      }
    if (!loadTable(error))
      return false;

    ExpTupleDesc *asciiTd = deleteTdb().workCriDesc_->getTupleDescriptor(
        deleteTdb().asciiTuppIndex_);
    std::vector<size_t> asciiIndexes;
    if (!sourceIndexes(asciiTd, deleteTdb().listOfFetchedColNames(),
                       &asciiIndexes, error))
      return false;

    ExExeStmtGlobals *statementGlobals =
      getGlobals()->castToExExeStmtGlobals();
    LocalLiteTxn txn(&store_, statementGlobals,
                     statementGlobals->getExecutionCount());
    std::vector<LocalLiteRow> sourceRows;
    LocalLiteRow sourceRow;
    if (localLiteTakeScanRow(localLiteScanRowKey(
            deleteTdb().getTableName(), statementGlobals), &sourceRow))
      sourceRows.push_back(sourceRow);
    else
      {
        std::vector<LocalLiteRow> rows;
        if (!txn.scanRows(table_, &rows, error))
          return false;
        for (size_t i = 0; i < rows.size(); i++)
          {
            workAtp_->getTupp(deleteTdb().asciiTuppIndex_)
              .setDataPointer(asciiRow_);
            workAtp_->getTupp(deleteTdb().convertTuppIndex_)
              .setDataPointer(convertRow_);
            unsigned int sourceLen = 0;
            if (!LocalLiteProjectBinaryRow(
                    table_, rows[i].value, rows[i].rowId,
                    asciiIndexes, asciiTd, asciiRow_,
                    deleteTdb().asciiRowLen_, &sourceLen, error))
              return false;
            str_pad(convertRow_, deleteTdb().convertRowLen_, '\0');
            ULng32 convertLen = deleteTdb().convertRowLen_;
            ex_expr::exp_return_type rc = deleteTdb().convertExpr_->eval(
                downEntry()->getAtp(), workAtp_, NULL, -1, &convertLen);
            if (rc == ex_expr::EXPR_ERROR)
              {
                *error = "local-lite delete source conversion failed";
                return false;
              }
            if (deleteTdb().scanExpr_)
              {
                rc = deleteTdb().scanExpr_->eval(downEntry()->getAtp(),
                                                 workAtp_);
                if (rc == ex_expr::EXPR_ERROR)
                  {
                    *error = "local-lite delete predicate evaluation failed";
                    return false;
                  }
                if (rc == ex_expr::EXPR_FALSE)
                  continue;
              }
            sourceRows.push_back(rows[i]);
          }
      }

    if (sourceRows.empty())
      return true;
    if (deleteTdb().returnRow())
      {
        // The normal scan-driven path supplies the selected row through the
        // side map, so materialize its executor tuple here for OLD values.
        // A trigger-enabled delete is driven one row per target request.
        const LocalLiteRow &returnedSource = sourceRows.back();
        workAtp_->getTupp(deleteTdb().asciiTuppIndex_)
          .setDataPointer(asciiRow_);
        workAtp_->getTupp(deleteTdb().convertTuppIndex_)
          .setDataPointer(convertRow_);
        unsigned int sourceLen = 0;
        if (!LocalLiteProjectBinaryRow(
                table_, returnedSource.value, returnedSource.rowId,
                asciiIndexes, asciiTd, asciiRow_, deleteTdb().asciiRowLen_,
                &sourceLen, error))
          return false;
        str_pad(convertRow_, deleteTdb().convertRowLen_, '\0');
        ULng32 convertLen = deleteTdb().convertRowLen_;
        if (deleteTdb().convertExpr_->eval(
                downEntry()->getAtp(), workAtp_, NULL, -1, &convertLen) ==
            ex_expr::EXPR_ERROR)
          {
            *error = "local-lite delete return-row conversion failed";
            return false;
          }
      }
    if (!txn.deleteRows(table_, sourceRows, error))
      return false;
    matches_ = static_cast<Lng32>(sourceRows.size());
    return true;
  }

  bool sendError(const std::string &message)
  {
    if (qparent_.up->isFull())
      return true;
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    if (!up->getDiagsArea())
      {
        ComDiagsArea *diags = ComDiagsArea::allocate(
            getGlobals()->getDefaultHeap());
        Lng32 code = localLiteStorageDiagCode(message);
        *diags << DgSqlCode(code);
        if (code == -EXE_INTERNAL_ERROR)
          *diags << DgString0(message.c_str());
        up->setDiagsArea(diags);
      }
    up->upState.status = ex_queue::Q_SQLERROR;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return qparent_.up->isFull();
  }

  bool sendDone()
  {
    if (qparent_.up->isFull())
      return true;
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    up->upState.status = ex_queue::Q_NO_DATA;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(deleteTdb().computeRowsAffected() ? matches_ : 0);
    if (matches_ > 0 && deleteTdb().computeRowsAffected())
      {
        ExMasterStmtGlobals *g = getGlobals()->castToExExeStmtGlobals()
          ->castToExMasterStmtGlobals();
        if (g)
          g->setRowsAffected(g->getRowsAffected() + matches_);
      }
    qparent_.up->insert();
    qparent_.down->removeHead();
    return false;
  }

  bool sendReturnedRow()
  {
    if (qparent_.up->isFull())
      return true;
    tupp returned;
    if (pool_->get_free_tuple(returned, deleteTdb().convertRowLen_))
      return true;
    str_cpy_all(returned.getDataPointer(), convertRow_,
                deleteTdb().convertRowLen_);
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    up->getAtp()->getTupp(deleteTdb().returnedTuppIndex_) = returned;
    up->upState.status = ex_queue::Q_OK_MMORE;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return false;
  }

  ex_queue_pair qparent_;
  sql_buffer_pool *pool_;
  atp_struct *workAtp_;
  char *asciiRow_;
  char *convertRow_;
  Lng32 matches_;
  NABoolean pendingReturn_;
  NABoolean pendingDone_;
  LocalLiteRocksDBStore store_;
  LocalLiteTableDef table_;
};

class LocalLiteHbaseUpdateTcb : public ex_tcb
{
public:
  LocalLiteHbaseUpdateTcb(const ComTdbHbaseAccess &tdb, ex_globals *globals)
    : ex_tcb(tdb, 1, globals),
      qparent_(),
      pool_(NULL),
      workAtp_(NULL),
      asciiRow_(NULL),
      convertRow_(NULL),
      updateRow_(NULL),
      mergeInsertRow_(NULL),
      matches_(0),
      pendingReturn_(FALSE),
      pendingDone_(FALSE)
  {
    Space *space = globals->getSpace();
    CollHeap *heap = globals->getDefaultHeap();
    allocateParentQueues(qparent_);
    pool_ = new(space) sql_buffer_pool(tdb.numBuffers_, tdb.bufferSize_,
                                       space, SqlBufferBase::NORMAL_);
    pool_->setStaticMode(TRUE);

    if (tdb.workCriDesc_)
      {
        workAtp_ = allocateAtp(tdb.workCriDesc_, space);
        if (tdb.asciiTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.asciiTuppIndex_), 0);
        if (tdb.convertTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.convertTuppIndex_), 0);
        if (tdb.updateTuppIndex_ > 0)
          pool_->get_free_tuple(workAtp_->getTupp(tdb.updateTuppIndex_), 0);
        if (tdb.mergeInsertTuppIndex_ > 0)
          pool_->get_free_tuple(
              workAtp_->getTupp(tdb.mergeInsertTuppIndex_), 0);
      }
    if (tdb.asciiRowLen_ > 0)
      asciiRow_ = new(heap) char[tdb.asciiRowLen_];
    if (tdb.convertRowLen_ > 0)
      convertRow_ = new(heap) char[tdb.convertRowLen_];
    if (tdb.updateRowLen_ > 0)
      updateRow_ = new(heap) char[tdb.updateRowLen_];
    if (tdb.mergeInsertRowLen_ > 0)
      mergeInsertRow_ = new(heap) char[tdb.mergeInsertRowLen_];

    if (tdb.scanExpr_)
      tdb.scanExpr_->fixup(0, getExpressionMode(), this, space, heap,
                           FALSE, globals);
    if (tdb.convertExpr_)
      tdb.convertExpr_->fixup(0, getExpressionMode(), this, space, heap,
                              FALSE, globals);
    if (tdb.updateExpr_)
      tdb.updateExpr_->fixup(0, getExpressionMode(), this, space, heap,
                             FALSE, globals);
    if (tdb.mergeInsertExpr_)
      tdb.mergeInsertExpr_->fixup(0, getExpressionMode(), this, space, heap,
                                  FALSE, globals);
    if (tdb.mergeUpdScanExpr_)
      tdb.mergeUpdScanExpr_->fixup(0, getExpressionMode(), this, space, heap,
                                   FALSE, globals);
    if (tdb.returnFetchExpr_)
      tdb.returnFetchExpr_->fixup(0, getExpressionMode(), this, space, heap,
                                  FALSE, globals);
    if (tdb.returnUpdateExpr_)
      tdb.returnUpdateExpr_->fixup(0, getExpressionMode(), this, space, heap,
                                   FALSE, globals);
  }

  ~LocalLiteHbaseUpdateTcb() { freeResources(); }

  NABoolean isLocalLiteUpdate() const { return TRUE; }

  void freeResources()
  {
    localLiteForgetScanRows(localLiteScanRowKey(
        updateTdb().getTableName(), getGlobals()->castToExExeStmtGlobals()));
    NADELETEBASICARRAY(asciiRow_, getGlobals()->getDefaultHeap());
    NADELETEBASICARRAY(convertRow_, getGlobals()->getDefaultHeap());
    NADELETEBASICARRAY(updateRow_, getGlobals()->getDefaultHeap());
    NADELETEBASICARRAY(mergeInsertRow_, getGlobals()->getDefaultHeap());
    asciiRow_ = NULL;
    convertRow_ = NULL;
    updateRow_ = NULL;
    mergeInsertRow_ = NULL;
    if (workAtp_)
      {
        deallocateAtp(workAtp_, getGlobals()->getSpace());
        workAtp_ = NULL;
      }
    delete pool_;
    pool_ = NULL;
    delete qparent_.up;
    qparent_.up = NULL;
    delete qparent_.down;
    qparent_.down = NULL;
  }

  ex_queue_pair getParentQueue() const { return qparent_; }
  Int32 numChildren() const { return 0; }
  const ex_tcb *getChild(Int32) const { return NULL; }

  ex_tcb_private_state *allocatePstates(Lng32 &numElems, Lng32 &pstateLength)
  {
    PstateAllocator<ex_tcb_private_state> pa;
    return pa.allocatePstates(this, numElems, pstateLength);
  }

  void registerSubtasks()
  {
    ex_tcb::registerSubtasks();
    ExScheduler *sched = getGlobals()->getScheduler();
    sched->registerInsertSubtask(ex_tcb::sWork, this, qparent_.down);
    sched->registerUnblockSubtask(ex_tcb::sWork, this, qparent_.up);
    sched->registerCancelSubtask(ex_tcb::sWork, this, qparent_.down);
  }

  ExWorkProcRetcode work()
  {
    while (!qparent_.down->isEmpty())
      {
        if (qparent_.up->isFull())
          return WORK_OK;

        ex_queue_entry *down = qparent_.down->getHeadEntry();
        if (pendingReturn_)
          {
            if (sendReturnedRow())
              return WORK_OK;
            pendingReturn_ = FALSE;
            pendingDone_ = TRUE;
            continue;
          }
        if (pendingDone_)
          {
            if (sendDone())
              return WORK_OK;
            pendingDone_ = FALSE;
            matches_ = 0;
            continue;
          }
        matches_ = 0;
        if (down->downState.request != ex_queue::GET_NOMORE)
          {
            std::string error;
            if (!evaluateAndUpdate(&error))
              {
                if (sendError(error))
                  return WORK_OK;
              }
          }
        if (updateTdb().returnRow() && matches_ > 0)
          pendingReturn_ = TRUE;
        else
          pendingDone_ = TRUE;
      }
    return WORK_OK;
  }

private:
  const ComTdbHbaseAccess &updateTdb() const
  {
    return static_cast<const ComTdbHbaseAccess &>(tdb);
  }

  ComTdbHbaseAccess &updateTdb()
  {
    return const_cast<ComTdbHbaseAccess &>(
        static_cast<const ComTdbHbaseAccess &>(tdb));
  }

  ex_queue_entry *downEntry()
  {
    return qparent_.down->getHeadEntry();
  }

  bool decodeColumnIndex(const char *raw, size_t *index) const
  {
    if (!raw || !index)
      return false;
    short len = 0;
    str_cpy_all(reinterpret_cast<char *>(&len), raw, sizeof(short));
    if (len <= 0)
      return false;
    const unsigned char *p =
      reinterpret_cast<const unsigned char *>(raw + sizeof(short));
    const unsigned char *end = p + len;
    while (p < end && *p != ':')
      p++;
    if (p == end)
      return false;
    p++;
    if (p < end && *p == '@')
      p++;
    uint64_t qualifier = 0;
    unsigned shift = 0;
    while (p < end && shift < 64)
      {
        qualifier |= static_cast<uint64_t>(*p) << shift;
        shift += 8;
        p++;
      }
    if (qualifier == 0)
      return false;
    *index = static_cast<size_t>(qualifier - 1);
    return true;
  }

  bool sourceIndexes(ExpTupleDesc *td,
                     Queue *names,
                     std::vector<size_t> *indexes,
                     std::string *error) const
  {
    indexes->clear();
    if (!td)
      {
        *error = "local-lite update missing tuple descriptor";
        return false;
      }
    if (names && names->numEntries() >= td->numAttrs())
      {
        for (UInt32 i = 0; i < td->numAttrs(); i++)
          {
            size_t index = 0;
            if (!decodeColumnIndex(static_cast<char *>(names->get(i)),
                                   &index))
              {
                *error = "local-lite update column mapping is invalid";
                return false;
              }
            indexes->push_back(index);
          }
        return true;
      }
    for (UInt32 i = 0; i < td->numAttrs(); i++)
      indexes->push_back(i);
    return true;
  }

  bool loadTable(std::string *error)
  {
    std::string catalog;
    std::string schema;
    std::string object;
    localLiteTableNameParts(updateTdb().getTableName(),
                            &catalog, &schema, &object);
    if (object.empty())
      {
        *error = "invalid local-lite table name";
        return false;
      }
    return store_.loadTable(catalog, schema, object, &table_, error);
  }

  bool evaluateAndUpdate(std::string *error)
  {
    const bool isMerge =
      updateTdb().getAccessType() == ComTdbHbaseAccess::MERGE_;
    if (!workAtp_ || !asciiRow_ || !convertRow_ ||
        !updateTdb().convertExpr_ ||
        (updateTdb().updateExpr_ && !updateRow_) ||
        (!isMerge && (!updateRow_ || !updateTdb().updateExpr_)))
      {
        *error = "local-lite update missing executor row buffers";
        return false;
      }
    if (!loadTable(error))
      return false;

    ExpTupleDesc *asciiTd = updateTdb().workCriDesc_->getTupleDescriptor(
        updateTdb().asciiTuppIndex_);
    ExpTupleDesc *updatedTd = updateTdb().updateExpr_ ?
      updateTdb().workCriDesc_->getTupleDescriptor(
          updateTdb().updateTuppIndex_) : NULL;
    std::vector<size_t> asciiIndexes;
    std::vector<size_t> updatedIndexes;
    if (!sourceIndexes(asciiTd, updateTdb().listOfFetchedColNames(),
                       &asciiIndexes, error) ||
        (updatedTd &&
         !sourceIndexes(updatedTd, updateTdb().listOfUpdatedColNames(),
                        &updatedIndexes, error)))
      return false;

    ExExeStmtGlobals *statementGlobals =
      getGlobals()->castToExExeStmtGlobals();
    LocalLiteTxn txn(&store_, statementGlobals,
                     statementGlobals->getExecutionCount());

    ex_cri_desc *downCri = updateTdb().criDescDown_;
    if (!downCri || downCri->noTuples() <= 0)
      {
        *error = "local-lite update missing source-row descriptor";
        return false;
      }
    std::vector<LocalLiteRow> sourceRows;
    LocalLiteRow sourceRow;
    if (isMerge)
      localLiteForgetScanRows(localLiteScanRowKey(
          updateTdb().getTableName(), statementGlobals));
    if (!isMerge &&
        localLiteTakeScanRow(localLiteScanRowKey(
            updateTdb().getTableName(), statementGlobals), &sourceRow))
      {
        sourceRows.push_back(sourceRow);
      }
    else
      {
        std::vector<LocalLiteRow> rows;
        if (!txn.scanRows(table_, &rows, error))
          return false;
        for (size_t i = 0; i < rows.size(); i++)
          {
            workAtp_->getTupp(updateTdb().asciiTuppIndex_)
              .setDataPointer(asciiRow_);
            workAtp_->getTupp(updateTdb().convertTuppIndex_)
              .setDataPointer(convertRow_);
            unsigned int fallbackLen = 0;
            if (!LocalLiteProjectBinaryRow(
                    table_, rows[i].value, rows[i].rowId,
                    asciiIndexes, asciiTd, asciiRow_,
                    updateTdb().asciiRowLen_, &fallbackLen, error))
              return false;
            str_pad(convertRow_, updateTdb().convertRowLen_, '\0');
            ULng32 convertedLen = updateTdb().convertRowLen_;
            ex_expr::exp_return_type fallbackRc =
              updateTdb().convertExpr_->eval(downEntry()->getAtp(), workAtp_,
                                              NULL, -1, &convertedLen);
            if (fallbackRc == ex_expr::EXPR_ERROR)
              {
                *error = "local-lite update source conversion failed";
                return false;
              }
            if (updateTdb().scanExpr_)
              {
                fallbackRc = updateTdb().scanExpr_->eval(
                    downEntry()->getAtp(), workAtp_);
                if (fallbackRc == ex_expr::EXPR_ERROR)
                  {
                    *error = "local-lite update predicate evaluation failed";
                    return false;
                  }
                if (fallbackRc == ex_expr::EXPR_FALSE)
                  continue;
              }
            sourceRows.push_back(rows[i]);
          }
        if (sourceRows.empty())
          return isMerge ? evaluateMergeInsert(&txn, error) : true;
      }

    if (isMerge && sourceRows.size() > 1)
      {
        *error = "local-lite MERGE source matched more than one target row";
        return false;
      }

    // A matched MERGE with no UPDATE action must not take the INSERT action.
    if (isMerge && !updateTdb().updateExpr_)
      return true;

    std::vector<LocalLiteRowMutation> mutations;
    for (size_t sourceIndex = 0;
         sourceIndex < sourceRows.size(); sourceIndex++)
      {
        sourceRow = sourceRows[sourceIndex];
        workAtp_->getTupp(updateTdb().asciiTuppIndex_)
          .setDataPointer(asciiRow_);
        workAtp_->getTupp(updateTdb().convertTuppIndex_)
          .setDataPointer(convertRow_);
        unsigned int sourceLen = 0;
        if (!LocalLiteProjectBinaryRow(
                table_, sourceRow.value, sourceRow.rowId,
                asciiIndexes, asciiTd, asciiRow_,
                updateTdb().asciiRowLen_, &sourceLen, error))
          return false;
        str_pad(convertRow_, updateTdb().convertRowLen_, '\0');
        ULng32 convertLen = updateTdb().convertRowLen_;
        ex_expr::exp_return_type rc = updateTdb().convertExpr_->eval(
            downEntry()->getAtp(), workAtp_, NULL, -1, &convertLen);
        if (rc == ex_expr::EXPR_ERROR)
          {
            *error = "local-lite update source conversion failed";
            return false;
          }

        if (isMerge && updateTdb().mergeUpdScanExpr_)
          {
            rc = updateTdb().mergeUpdScanExpr_->eval(
                downEntry()->getAtp(), workAtp_);
            if (rc == ex_expr::EXPR_ERROR)
              {
                *error = "local-lite MERGE update predicate evaluation failed";
                return false;
              }
            if (rc == ex_expr::EXPR_FALSE)
              continue;
          }

        workAtp_->getTupp(updateTdb().updateTuppIndex_)
          .setDataPointer(updateRow_);
        unsigned int projectedLen = 0;
        if (!LocalLiteProjectBinaryRow(
                table_, sourceRow.value, sourceRow.rowId,
                updatedIndexes, updatedTd, updateRow_,
                updateTdb().updateRowLen_, &projectedLen, error))
          return false;
        ULng32 updateLen = projectedLen;
        rc = updateTdb().updateExpr_->eval(
            downEntry()->getAtp(), workAtp_, NULL, -1, &updateLen);
        if (rc == ex_expr::EXPR_ERROR)
          {
            *error = "local-lite update expression evaluation failed";
            return false;
          }
        if (updateLen == 0)
          updateLen = updateTdb().updateRowLen_;

        LocalLiteRowMutation mutation;
        mutation.before = sourceRow;
        if (!LocalLiteApplyBinaryUpdate(
                table_, mutation.before.value, updatedTd, updateRow_,
                updateTdb().updateRowLen_, updatedIndexes,
                &mutation.after, error))
          return false;
        mutations.push_back(mutation);
      }
    if (mutations.empty())
      return true;
    if (!txn.updateRows(table_, mutations, error))
      return false;

    matches_ = static_cast<Lng32>(mutations.size());
    return true;
  }

  bool evaluateMergeInsert(LocalLiteTxn *txn, std::string *error)
  {
    if (!updateTdb().mergeInsertExpr_)
      return true;
    if (!txn || !mergeInsertRow_ || updateTdb().mergeInsertRowLen_ == 0)
      {
        *error = "local-lite MERGE missing insert row buffer";
        return false;
      }

    ExpTupleDesc *insertTd = updateTdb().workCriDesc_->getTupleDescriptor(
        updateTdb().mergeInsertTuppIndex_);
    if (!insertTd)
      {
        *error = "local-lite MERGE missing insert tuple descriptor";
        return false;
      }

    str_pad(mergeInsertRow_, updateTdb().mergeInsertRowLen_, '\0');
    workAtp_->getTupp(updateTdb().mergeInsertTuppIndex_)
      .setDataPointer(mergeInsertRow_);
    ULng32 rowLen = updateTdb().mergeInsertRowLen_;
    ex_expr::exp_return_type rc = updateTdb().mergeInsertExpr_->eval(
        downEntry()->getAtp(), workAtp_, NULL, -1, &rowLen);
    if (rc == ex_expr::EXPR_ERROR)
      {
        *error = "local-lite MERGE insert expression evaluation failed";
        return false;
      }
    if (rowLen == 0)
      rowLen = updateTdb().mergeInsertRowLen_;


    std::string encodedRow;
    if (!LocalLiteNormalizeBinaryRow(table_, insertTd, mergeInsertRow_,
                                     static_cast<size_t>(rowLen),
                                     &encodedRow, error))
      return false;
    uint64_t rowId = 0;
    if (!txn->insertRow(table_, encodedRow, &rowId, error))
      return false;
    matches_ = 1;
    return true;
  }

  bool sendError(const std::string &message)
  {
    if (qparent_.up->isFull())
      return true;
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    if (!up->getDiagsArea())
      {
        ComDiagsArea *diags = ComDiagsArea::allocate(
            getGlobals()->getDefaultHeap());
        Lng32 code = localLiteStorageDiagCode(message);
        *diags << DgSqlCode(code);
        if (code == -EXE_INTERNAL_ERROR)
          *diags << DgString0(message.c_str());
        up->setDiagsArea(diags);
      }
    up->upState.status = ex_queue::Q_SQLERROR;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return qparent_.up->isFull();
  }

  bool sendDone()
  {
    if (qparent_.up->isFull())
      return true;
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    up->upState.status = ex_queue::Q_NO_DATA;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(updateTdb().computeRowsAffected() ? matches_ : 0);
    if (matches_ > 0 && updateTdb().computeRowsAffected())
      {
        ExMasterStmtGlobals *g = getGlobals()->castToExExeStmtGlobals()
          ->castToExMasterStmtGlobals();
        if (g)
          g->setRowsAffected(g->getRowsAffected() + matches_);
      }
    qparent_.up->insert();
    qparent_.down->removeHead();
    return false;
  }

  bool sendReturnedRow()
  {
    if (qparent_.up->isFull())
      return true;
    ex_queue_entry *up = qparent_.up->getTailEntry();
    up->copyAtp(downEntry());
    if (updateTdb().returnFetchExpr_)
      {
        tupp fetched;
        if (pool_->get_free_tuple(
                fetched, updateTdb().returnFetchedRowLen_))
          return true;
        up->getAtp()->getTupp(updateTdb().returnedFetchedTuppIndex_) = fetched;
        if (updateTdb().returnFetchExpr_->eval(up->getAtp(), workAtp_) ==
            ex_expr::EXPR_ERROR)
          return true;
      }
    if (updateTdb().returnUpdateExpr_)
      {
        tupp updated;
        if (pool_->get_free_tuple(
                updated, updateTdb().returnUpdatedRowLen_))
          return true;
        up->getAtp()->getTupp(updateTdb().returnedUpdatedTuppIndex_) = updated;
        if (updateTdb().returnUpdateExpr_->eval(up->getAtp(), workAtp_) ==
            ex_expr::EXPR_ERROR)
          return true;
      }
    else
      {
        tupp returned;
        if (pool_->get_free_tuple(returned, updateTdb().convertRowLen_))
          return true;
        str_cpy_all(returned.getDataPointer(), convertRow_,
                    updateTdb().convertRowLen_);
        up->getAtp()->getTupp(updateTdb().returnedTuppIndex_) = returned;
      }
    up->upState.status = ex_queue::Q_OK_MMORE;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return false;
  }

  ex_queue_pair qparent_;
  sql_buffer_pool *pool_;
  atp_struct *workAtp_;
  char *asciiRow_;
  char *convertRow_;
  char *updateRow_;
  char *mergeInsertRow_;
  Lng32 matches_;
  NABoolean pendingReturn_;
  NABoolean pendingDone_;
  LocalLiteRocksDBStore store_;
  LocalLiteTableDef table_;
};

Int64 getTransactionIDFromContext()
{
  return 0;
}

ex_tcb *ExHbaseAccessTdb::build(ex_globals *globals)
{
  if (getAccessType() == ComTdbHbaseAccess::SELECT_)
    {
      LocalLiteHbaseScanTcb *tcb =
        new(globals->getSpace()) LocalLiteHbaseScanTcb(*this, globals);
      tcb->registerSubtasks();
      return tcb;
    }

  if (getAccessType() == ComTdbHbaseAccess::INSERT_ ||
      getAccessType() == ComTdbHbaseAccess::UPSERT_)
    {
      LocalLiteHbaseInsertTcb *tcb =
        new(globals->getSpace()) LocalLiteHbaseInsertTcb(*this, globals);
      tcb->registerSubtasks();
      return tcb;
    }

  if (getAccessType() == ComTdbHbaseAccess::UPDATE_)
    {
      LocalLiteHbaseUpdateTcb *tcb =
        new(globals->getSpace()) LocalLiteHbaseUpdateTcb(*this, globals);
      tcb->registerSubtasks();
      return tcb;
    }

  if (getAccessType() == ComTdbHbaseAccess::MERGE_)
    {
      LocalLiteHbaseUpdateTcb *tcb =
        new(globals->getSpace()) LocalLiteHbaseUpdateTcb(*this, globals);
      tcb->registerSubtasks();
      return tcb;
    }

  if (getAccessType() == ComTdbHbaseAccess::DELETE_)
    {
      LocalLiteHbaseDeleteTcb *tcb =
        new(globals->getSpace()) LocalLiteHbaseDeleteTcb(*this, globals);
      tcb->registerSubtasks();
      return tcb;
    }

  LocalLiteUnsupportedHbaseTcb *tcb =
    new(globals->getSpace()) LocalLiteUnsupportedHbaseTcb(*this, globals);
  tcb->registerSubtasks();
  return tcb;
}

ex_tcb *ExHbaseCoProcAggrTdb::build(ex_globals *)
{
  return NULL;
}

short ExHbaseAccessTcb::setupError(NAHeap *, ex_queue_pair &, Lng32,
                                   const char *, const char *)
{
  return -1;
}

short ExHbaseAccessTcb::setupError(Lng32, const char *, const char *)
{
  return -1;
}

void ExHbaseAccessTcb::buildLoggingPath(const char *loggingLocation,
                                        char *logId,
                                        const char *tableName,
                                        char *currCmdLoggingLocation)
{
  if (currCmdLoggingLocation == NULL)
    return;

  snprintf(currCmdLoggingLocation,
           ComMAX_3_PART_EXTERNAL_UTF8_NAME_LEN_IN_BYTES,
           "%s/ERR_%s_%s",
           loggingLocation ? loggingLocation : "",
           tableName ? tableName : "",
           (logId && logId[0]) ? logId : "local_lite");
}

ex_tcb *ExHdfsScanTdb::build(ex_globals *)
{
  return NULL;
}

ex_tcb *ExOrcFastAggrTdb::build(ex_globals *)
{
  return NULL;
}

short ExHdfsScanTcb::setupError(Lng32, Lng32, const char *, const char *,
                                const char *)
{
  return -1;
}

HiveMetaData::HiveMetaData(NAHeap *heap)
  : heap_(heap),
    tbl_(NULL),
    currDesc_(NULL),
    errCode_(0),
    errCodeStr_(NULL),
    errMethodName_(NULL),
    errDetail_(NULL)
{
}

HiveMetaData::~HiveMetaData()
{
}

NABoolean HiveMetaData::init()
{
  return FALSE;
}

struct hive_tbl_desc *HiveMetaData::getTableDesc(const char *, const char *,
                                                 NABoolean, NABoolean,
                                                 NABoolean)
{
  recordError(-1, "HiveMetaData::getTableDesc()");
  return NULL;
}

struct hive_tbl_desc *HiveMetaData::getFakedTableDesc(const char *)
{
  return NULL;
}

NABoolean HiveMetaData::validate(hive_tbl_desc *)
{
  return FALSE;
}

void HiveMetaData::position()
{
}

struct hive_tbl_desc *HiveMetaData::getNext()
{
  return NULL;
}

void HiveMetaData::advance()
{
}

NABoolean HiveMetaData::atEnd()
{
  return TRUE;
}

void HiveMetaData::clear()
{
}

void HiveMetaData::resetErrorInfo()
{
  errCode_ = 0;
  errCodeStr_ = NULL;
  errMethodName_ = NULL;
  errDetail_ = NULL;
}

NABoolean HiveMetaData::recordError(Int32 errCode, const char *errMethodName)
{
  errCode_ = errCode;
  errCodeStr_ = trafLocalLiteUnsupportedStorage();
  errMethodName_ = errMethodName;
  errDetail_ = trafLocalLiteUnsupportedStorage();
  return FALSE;
}

void HiveMetaData::recordParseError(Int32 errCode, const char *errCodeStr,
                                    const char *errMethodName,
                                    const char *errDetail)
{
  errCode_ = errCode;
  errCodeStr_ = errCodeStr;
  errMethodName_ = errMethodName;
  errDetail_ = errDetail;
}

#endif
