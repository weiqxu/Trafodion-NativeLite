// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_SQL_TABLE_H
#define LOCAL_LITE_SQL_TABLE_H

#ifdef TRAF_LOCAL_LITE

class SqlciEnv;

bool LocalLiteSqlTable_process(const char *sql, SqlciEnv *sqlciEnv, short *retcode);

#endif

#endif
