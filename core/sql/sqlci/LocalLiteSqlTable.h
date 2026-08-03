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

// Resolve SQLCI's -u/SET SESSION AUTHORIZATION identity from the RocksDB
// catalog and publish it to the CLI context used by CURRENT_USER/SESSION_USER.
bool LocalLiteSqlTable_setCurrentUser(SqlciEnv *sqlciEnv, short *retcode);

// Check a statement again at EXECUTE time.  SQLCI prepared statements can
// outlive a GRANT/REVOKE or a session-identity change, so authorization is
// intentionally evaluated immediately before every execution as well as at
// prepare time.
bool LocalLiteSqlTable_checkAuthorization(const char *sql,
                                          SqlciEnv *sqlciEnv,
                                          short *retcode);

#endif

#endif
