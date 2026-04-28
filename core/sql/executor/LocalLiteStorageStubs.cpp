// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "Platform.h"
#include "ExHbaseAccess.h"
#include "ExHdfsScan.h"
#include "hiveHook.h"

extern "C" const char *trafLocalLiteUnsupportedStorage()
{
  return "HDFS, Hive, and HBase are not supported in local-lite builds";
}

Int64 getTransactionIDFromContext()
{
  return 0;
}

ex_tcb *ExHbaseAccessTdb::build(ex_globals *)
{
  return NULL;
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
