// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteSqlTable.h"
#include "LocalLiteRocksDBStore.h"

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

static std::string unquoteIdentifier(const std::string &name)
{
  if (name.size() >= 2 && name[0] == '"' && name[name.size() - 1] == '"')
    return name.substr(1, name.size() - 2);
  return upper(name);
}

static bool parseSchemaName(const std::string &text,
                            std::string *catalog,
                            std::string *schema)
{
  std::string name = trim(text);
  size_t end = name.find_first_of(" \t\r\n");
  if (end != std::string::npos)
    name.resize(end);
  size_t dot = name.find('.');
  *catalog = dot == std::string::npos
      ? "TRAFODION" : unquoteIdentifier(name.substr(0, dot));
  *schema = unquoteIdentifier(
      dot == std::string::npos ? name : name.substr(dot + 1));
  return !catalog->empty() && !schema->empty();
}

static bool parseObjectName(const std::string &text,
                            std::string *catalog,
                            std::string *schema,
                            std::string *object)
{
  std::string name = trim(text);
  size_t end = name.find_first_of(" \t\r\n");
  if (end != std::string::npos)
    name.resize(end);
  size_t first = name.find('.');
  size_t second = first == std::string::npos
      ? std::string::npos : name.find('.', first + 1);
  if (first == std::string::npos)
    {
      *catalog = "TRAFODION";
      *schema = "SEABASE";
      *object = unquoteIdentifier(name);
    }
  else if (second == std::string::npos)
    {
      *catalog = "TRAFODION";
      *schema = unquoteIdentifier(name.substr(0, first));
      *object = unquoteIdentifier(name.substr(first + 1));
    }
  else
    {
      *catalog = unquoteIdentifier(name.substr(0, first));
      *schema = unquoteIdentifier(name.substr(first + 1, second - first - 1));
      *object = unquoteIdentifier(name.substr(second + 1));
    }
  return !catalog->empty() && !schema->empty() && !object->empty();
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

  if (startsWithWord(sql, "CREATE SCHEMA"))
    {
      std::string rest = trim(sql.substr(strlen("CREATE SCHEMA")));
      bool ifNotExists = false;
      if (upper(rest).compare(0, strlen("IF NOT EXISTS"), "IF NOT EXISTS") == 0)
        {
          ifNotExists = true;
          rest = trim(rest.substr(strlen("IF NOT EXISTS")));
        }
      std::string catalog;
      std::string schema;
      LocalLiteRocksDBStore store;
      std::string error;
      if (!parseSchemaName(rest, &catalog, &schema) ||
          !store.createSchema(catalog, schema, ifNotExists, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid local-lite schema name" : error);
      else
        *retcode = 0;
      return true;
    }
  if (startsWithWord(sql, "DROP SCHEMA"))
    {
      std::string rest = trim(sql.substr(strlen("DROP SCHEMA")));
      bool ifExists = false;
      if (upper(rest).compare(0, strlen("IF EXISTS"), "IF EXISTS") == 0)
        {
          ifExists = true;
          rest = trim(rest.substr(strlen("IF EXISTS")));
        }
      bool cascade = upper(rest).find(" CASCADE") != std::string::npos;
      std::string catalog;
      std::string schema;
      LocalLiteRocksDBStore store;
      std::string error;
      if (!parseSchemaName(rest, &catalog, &schema) ||
          !store.dropSchema(catalog, schema, ifExists, cascade, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid local-lite schema name" : error);
      else
        *retcode = 0;
      return true;
    }
  if (startsWithWord(sql, "CREATE SYNONYM"))
    {
      std::string rest = trim(sql.substr(strlen("CREATE SYNONYM")));
      std::string upperRest = upper(rest);
      size_t forPos = upperRest.find(" FOR ");
      std::string catalog, schema, object;
      std::string targetCatalog, targetSchema, targetObject;
      LocalLiteRocksDBStore store;
      std::string error;
      if (forPos == std::string::npos ||
          !parseObjectName(rest.substr(0, forPos), &catalog, &schema, &object) ||
          !parseObjectName(rest.substr(forPos + 5), &targetCatalog,
                           &targetSchema, &targetObject) ||
          !store.createSynonym(catalog, schema, object, targetCatalog,
                               targetSchema, targetObject, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid local-lite synonym definition" : error);
      else
        *retcode = 0;
      return true;
    }
  if (startsWithWord(sql, "DROP SYNONYM"))
    {
      std::string rest = trim(sql.substr(strlen("DROP SYNONYM")));
      bool ifExists = false;
      if (upper(rest).compare(0, strlen("IF EXISTS"), "IF EXISTS") == 0)
        {
          ifExists = true;
          rest = trim(rest.substr(strlen("IF EXISTS")));
        }
      std::string catalog, schema, object;
      LocalLiteRocksDBStore store;
      std::string error;
      if (!parseObjectName(rest, &catalog, &schema, &object) ||
          !store.dropSynonym(catalog, schema, object, ifExists, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid local-lite synonym name" : error);
      else
        *retcode = 0;
      return true;
    }
  if (startsWithWord(sql, "CREATE EXTERNAL TABLE") ||
      startsWithWord(sql, "CREATE HBASE TABLE") ||
      startsWithWord(sql, "CREATE VOLATILE TABLE"))
    {
      *retcode = reportError(sqlciEnv, "native HBase/Hive/volatile table DDL is not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "TRUNCATE TABLE"))
    {
      std::string catalog;
      std::string schema;
      std::string object;
      std::string error;
      LocalLiteRocksDBStore store;
      LocalLiteTableDef table;
      std::vector<LocalLiteRow> rows;
      if (!parseObjectName(sql.substr(strlen("TRUNCATE TABLE")),
                           &catalog, &schema, &object) ||
          !store.loadTable(catalog, schema, object, &table, &error) ||
          !store.scanRows(table, &rows, &error) ||
          !store.deleteRows(table, rows, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid local-lite table name" : error);
      else
        *retcode = 0;
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
