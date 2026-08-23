// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LITE_UDR_H
#define LITE_UDR_H

#ifdef TRAF_LITE

class SqlciEnv;

// RocksDB-only UDR adapter. It deliberately sits beside the SQLCI lite
// table adapter: standard MXUDR compilation requires _MD_ tables and an
// external UDR server, neither of which exists in the standalone lite
// profile.
bool LiteUdr_process(const char *sql,
                          SqlciEnv *env,
                          short *retcode);

bool LiteUdr_prepare(const char *sql,
                          const char *statementName,
                          SqlciEnv *env,
                          short *retcode);

bool LiteUdr_executePrepared(const char *statementName,
                                  SqlciEnv *env,
                                  short *retcode);

#endif

#endif
