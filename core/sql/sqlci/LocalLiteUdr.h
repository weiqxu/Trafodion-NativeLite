// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_UDR_H
#define LOCAL_LITE_UDR_H

#ifdef TRAF_LOCAL_LITE

class SqlciEnv;

// RocksDB-only UDR adapter. It deliberately sits beside the SQLCI local-lite
// table adapter: standard MXUDR compilation requires _MD_ tables and an
// external UDR server, neither of which exists in the standalone local-lite
// profile.
bool LocalLiteUdr_process(const char *sql,
                          SqlciEnv *env,
                          short *retcode);

bool LocalLiteUdr_prepare(const char *sql,
                          const char *statementName,
                          SqlciEnv *env,
                          short *retcode);

bool LocalLiteUdr_executePrepared(const char *statementName,
                                  SqlciEnv *env,
                                  short *retcode);

#endif

#endif
