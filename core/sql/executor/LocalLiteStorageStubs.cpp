// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "Platform.h"

extern "C" const char *trafLocalLiteUnsupportedStorage()
{
  return "HDFS, Hive, and HBase are not supported in local-lite builds";
}

#endif
