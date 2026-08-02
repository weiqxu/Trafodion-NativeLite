// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteSqlTable.h"

#include "SqlciEnv.h"

#include <ctype.h>
#include <string.h>

#include <string>

static std::string trim(const std::string &s)
{
  size_t b = 0;
  while (b < s.size() && isspace(static_cast<unsigned char>(s[b])))
    b++;
  size_t e = s.size();
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1])))
    e--;
  return s.substr(b, e - b);
}

static std::string upper(const std::string &s)
{
  std::string out = s;
  for (size_t i = 0; i < out.size(); i++)
    out[i] = static_cast<char>(toupper(static_cast<unsigned char>(out[i])));
  return out;
}

static bool startsWithWord(const std::string &sql, const char *prefix)
{
  std::string p(prefix);
  if (sql.size() < p.size() || upper(sql.substr(0, p.size())) != p)
    return false;
  return sql.size() == p.size() ||
         isspace(static_cast<unsigned char>(sql[p.size()]));
}

static void writeLine(SqlciEnv *env, const std::string &line)
{
  env->get_logfile()->WriteAll(line.c_str());
}

static short reportError(SqlciEnv *env, const std::string &message)
{
  writeLine(env, "*** ERROR[local-lite] " + message);
  return 1;
}

bool LocalLiteSqlTable_process(const char *sqlText, SqlciEnv *sqlciEnv, short *retcode)
{
  if (!sqlText || !sqlciEnv || !retcode)
    return false;

  std::string sql = trim(sqlText);
  while (!sql.empty() && sql[sql.size() - 1] == ';')
    sql = trim(sql.substr(0, sql.size() - 1));
  if (sql.empty())
    return false;

  if (startsWithWord(sql, "CREATE INDEX"))
    {
      *retcode = reportError(sqlciEnv, "CREATE INDEX is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "CREATE VIEW"))
    {
      *retcode = reportError(sqlciEnv, "CREATE VIEW is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "CREATE SEQUENCE"))
    {
      *retcode = reportError(sqlciEnv, "CREATE SEQUENCE is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "CREATE SCHEMA"))
    {
      *retcode = reportError(sqlciEnv, "CREATE SCHEMA is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "CREATE SYNONYM"))
    {
      *retcode = reportError(sqlciEnv, "CREATE SYNONYM is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "CREATE EXTERNAL TABLE") ||
      startsWithWord(sql, "CREATE HBASE TABLE") ||
      startsWithWord(sql, "CREATE VOLATILE TABLE"))
    {
      *retcode = reportError(sqlciEnv, "native HBase/Hive/volatile table DDL is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "ALTER TABLE"))
    {
      *retcode = reportError(sqlciEnv, "ALTER TABLE is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "TRUNCATE TABLE"))
    {
      *retcode = reportError(sqlciEnv, "TRUNCATE TABLE is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "UPSERT USING LOAD"))
    {
      *retcode = reportError(
          sqlciEnv,
          "UPSERT USING LOAD is not supported for local-lite RocksDB tables; use UPSERT");
      return true;
    }
  return false;
}

#endif
