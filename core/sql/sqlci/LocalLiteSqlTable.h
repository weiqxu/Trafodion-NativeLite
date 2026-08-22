// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_SQL_TABLE_H
#define LOCAL_LITE_SQL_TABLE_H

#ifdef TRAF_LOCAL_LITE

#include <string>
#include <vector>

class SqlciEnv;

bool LocalLiteSqlTable_process(const char *sql, SqlciEnv *sqlciEnv, short *retcode);

// Return the object names produced by a SQLCI GET metadata command.  The
// server uses this structured form to expose GET TABLES/SCHEMAS through T4;
// SQLCI itself continues to use LocalLiteSqlTable_process for text output.
bool LocalLiteSqlTable_getMetadata(const char *sql, SqlciEnv *sqlciEnv,
                                   std::string *title,
                                   std::vector<std::string> *objects,
                                   std::string *error);

// Return true for SQLCI-local statements whose PostgreSQL extended-query
// Describe response is NoData. This classifier must not execute or mutate the
// session; Execute remains the only side-effecting protocol phase.
bool LocalLiteSqlTable_isUtilityStatement(const char *sql);

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
