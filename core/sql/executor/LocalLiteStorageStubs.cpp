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
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

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
            if (moveRowToUpQueue(convertRow_, formattedLen, &rc))
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
    return store_.scanRows(table_, &rows_, error);
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

    std::vector<size_t> sourceIndexes;
    if (!projectionSourceIndexes(asciiTd, &sourceIndexes, error))
      return false;

    if (sourceIndexes.size() != convertTd->numAttrs())
      {
        *error = "local-lite scan projection does not match tuple descriptor";
        return false;
      }

    workAtp_->getTupp(scanTdb().asciiTuppIndex_).setDataPointer(asciiRow_);
    workAtp_->getTupp(scanTdb().convertTuppIndex_).setDataPointer(convertRow_);

    unsigned int convertLen = 0;
    if (!LocalLiteProjectBinaryRow(table_, row.value, sourceIndexes,
                                   convertTd, convertRow_,
                                   scanTdb().convertRowLen_,
                                   &convertLen, error))
      return false;

    *pass = true;

    *formattedLen = static_cast<Lng32>(convertLen);
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
                                      td->getTupleDataFormat());
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

  bool projectionSourceIndexes(ExpTupleDesc *asciiTd,
                               std::vector<size_t> *sourceIndexes,
                               std::string *error)
  {
    if (!asciiTd)
      {
        *error = "local-lite missing scan tuple descriptor";
        return false;
      }

    sourceIndexes->clear();
    Queue *fetched = scanTdb().listOfFetchedColNames();
    if (fetched && fetched->numEntries() >= asciiTd->numAttrs())
      {
        for (UInt32 i = 0; i < asciiTd->numAttrs(); i++)
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

    for (UInt32 i = 0; i < asciiTd->numAttrs(); i++)
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
        *diags << DgSqlCode(-EXE_INTERNAL_ERROR)
               << DgString0(message.c_str());
        up->setDiagsArea(diags);
      }
    up->upState.status = ex_queue::Q_SQLERROR;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = downEntry()->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return qparent_.up->isFull();
  }

  short moveRowToUpQueue(const char *row, Lng32 len, short *rc)
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
      matches_(0)
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
    if (tdb.insConstraintExpr_)
      tdb.insConstraintExpr_->fixup(0, getExpressionMode(), this, space,
                                    heap, FALSE, globals);
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

        matches_ = 0;
        ex_queue_entry *down = qparent_.down->getHeadEntry();
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

        if (sendDone())
          return WORK_OK;
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

    if (insertTdb().insConstraintExpr_)
      {
        ex_expr::exp_return_type evalRetCode =
          insertTdb().insConstraintExpr_->eval(down->getAtp(), workAtp_);
        if (evalRetCode == ex_expr::EXPR_ERROR)
          {
            *error = "local-lite insert constraint evaluation failed";
            return false;
          }
        if (evalRetCode == ex_expr::EXPR_FALSE)
          return true;
      }

    ExpTupleDesc *convertTd =
      insertTdb().workCriDesc_->getTupleDescriptor(insertTdb().convertTuppIndex_);
    std::string encodedRow;
    if (!LocalLiteNormalizeBinaryRow(table_, convertTd, convertRow_,
                                     static_cast<size_t>(rowLen),
                                     &encodedRow, error))
      return false;

    uint64_t rowId = 0;
    if (!store_.allocateRowId(table_, &rowId, error))
      return false;
    if (!store_.putRow(table_, rowId, encodedRow, error))
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
        *diags << DgSqlCode(-EXE_INTERNAL_ERROR)
               << DgString0(message.c_str());
        up->setDiagsArea(diags);
      }
    up->upState.status = ex_queue::Q_SQLERROR;
    up->upState.downIndex = qparent_.down->getHeadIndex();
    up->upState.parentIndex = down->downState.parentIndex;
    up->upState.setMatchNo(matches_);
    qparent_.up->insert();
    return qparent_.up->isFull();
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

  if (getAccessType() == ComTdbHbaseAccess::INSERT_)
    {
      LocalLiteHbaseInsertTcb *tcb =
        new(globals->getSpace()) LocalLiteHbaseInsertTcb(*this, globals);
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
