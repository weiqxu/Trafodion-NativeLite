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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

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

static std::string unquoteIdent(const std::string &s)
{
  std::string t = trim(s);
  if (t.size() >= 2 && t[0] == '"' && t[t.size() - 1] == '"')
    return t.substr(1, t.size() - 2);
  return upper(t);
}

static std::vector<std::string> splitDottedName(const std::string &name)
{
  std::vector<std::string> parts;
  std::string curr;
  bool quoted = false;
  for (size_t i = 0; i < name.size(); i++)
    {
      char c = name[i];
      if (c == '"')
        quoted = !quoted;
      if (c == '.' && !quoted)
        {
          parts.push_back(unquoteIdent(curr));
          curr.clear();
        }
      else
        curr += c;
    }
  parts.push_back(unquoteIdent(curr));
  return parts;
}

static LocalLiteTableDef tableFromName(const std::string &name)
{
  std::vector<std::string> parts = splitDottedName(name);
  LocalLiteTableDef table;
  table.catalog = "TRAFODION";
  table.schema = "SEABASE";
  if (parts.size() == 1)
    table.name = parts[0];
  else if (parts.size() == 2)
    {
      table.schema = parts[0];
      table.name = parts[1];
    }
  else
    {
      table.catalog = parts[parts.size() - 3];
      table.schema = parts[parts.size() - 2];
      table.name = parts[parts.size() - 1];
    }
  return table;
}

static std::vector<std::string> splitCommaList(const std::string &s)
{
  std::vector<std::string> out;
  std::string curr;
  bool quoted = false;
  int parens = 0;
  for (size_t i = 0; i < s.size(); i++)
    {
      char c = s[i];
      if (c == '\'' && (i == 0 || s[i - 1] != '\\'))
        quoted = !quoted;
      else if (!quoted && c == '(')
        parens++;
      else if (!quoted && c == ')' && parens > 0)
        parens--;

      if (c == ',' && !quoted && parens == 0)
        {
          out.push_back(trim(curr));
          curr.clear();
        }
      else
        curr += c;
    }
  if (!trim(curr).empty())
    out.push_back(trim(curr));
  return out;
}

static bool splitValueTuples(const std::string &s,
                             std::vector<std::string> *tuples,
                             std::string *error)
{
  tuples->clear();
  size_t i = 0;
  while (i < s.size())
    {
      while (i < s.size() &&
             (isspace(static_cast<unsigned char>(s[i])) || s[i] == ','))
        i++;
      if (i >= s.size())
        break;
      if (s[i] != '(')
        {
          *error = "invalid INSERT VALUES syntax";
          return false;
        }

      size_t start = i + 1;
      bool quoted = false;
      int parens = 1;
      i++;
      for (; i < s.size(); i++)
        {
          char c = s[i];
          if (c == '\'' && (i == 0 || s[i - 1] != '\\'))
            {
              if (quoted && i + 1 < s.size() && s[i + 1] == '\'')
                {
                  i++;
                  continue;
                }
              quoted = !quoted;
            }
          else if (!quoted && c == '(')
            parens++;
          else if (!quoted && c == ')')
            {
              parens--;
              if (parens == 0)
                {
                  tuples->push_back(s.substr(start, i - start));
                  i++;
                  break;
                }
            }
        }

      if (parens != 0)
        {
          *error = "invalid INSERT VALUES syntax";
          return false;
        }
    }

  if (tuples->empty())
    {
      *error = "invalid INSERT VALUES syntax";
      return false;
    }
  return true;
}

static std::string unquoteValue(const std::string &s)
{
  std::string t = trim(s);
  if (upper(t) == "NULL")
    return "";
  if (t.size() >= 2 && t[0] == '\'' && t[t.size() - 1] == '\'')
    {
      std::string out;
      for (size_t i = 1; i + 1 < t.size(); i++)
        {
          if (t[i] == '\'' && i + 1 < t.size() - 1 && t[i + 1] == '\'')
            i++;
          out += t[i];
        }
      return out;
    }
  return t;
}

static void appendUint32(std::string &s, unsigned int v)
{
  s += static_cast<char>((v >> 24) & 0xff);
  s += static_cast<char>((v >> 16) & 0xff);
  s += static_cast<char>((v >> 8) & 0xff);
  s += static_cast<char>(v & 0xff);
}

static unsigned int readUint32(const std::string &s, size_t *offset)
{
  unsigned int v = 0;
  for (int i = 0; i < 4 && *offset < s.size(); i++)
    {
      v = (v << 8) | static_cast<unsigned char>(s[*offset]);
      (*offset)++;
    }
  return v;
}

static std::string encodeFields(const std::vector<std::string> &fields)
{
  std::string out;
  appendUint32(out, static_cast<unsigned int>(fields.size()));
  for (size_t i = 0; i < fields.size(); i++)
    {
      appendUint32(out, static_cast<unsigned int>(fields[i].size()));
      out += fields[i];
    }
  return out;
}

static bool decodeFields(const std::string &encoded, std::vector<std::string> *fields)
{
  size_t offset = 0;
  if (encoded.size() < 4)
    return false;
  unsigned int count = readUint32(encoded, &offset);
  fields->clear();
  for (unsigned int i = 0; i < count; i++)
    {
      if (offset + 4 > encoded.size())
        return false;
      unsigned int len = readUint32(encoded, &offset);
      if (offset + len > encoded.size())
        return false;
      fields->push_back(encoded.substr(offset, len));
      offset += len;
    }
  return true;
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

static bool containsUnsupportedType(const std::string &type)
{
  std::string u = upper(type);
  return u.find("LOB") != std::string::npos ||
         u.find("BLOB") != std::string::npos ||
         u.find("CLOB") != std::string::npos ||
         u.find("ARRAY") != std::string::npos;
}

static uint64_t newObjectUid()
{
  uint64_t uid = static_cast<uint64_t>(time(NULL));
  uid = (uid << 24) ^ static_cast<uint64_t>(getpid() & 0xffff);
  uid = (uid << 8) ^ static_cast<uint64_t>(rand() & 0xff);
  return uid ? uid : 1;
}

static short processCreate(const std::string &sql, SqlciEnv *env)
{
  size_t open = sql.find('(');
  size_t close = sql.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open)
    return reportError(env, "invalid CREATE TABLE syntax");

  std::string name = trim(sql.substr(strlen("CREATE TABLE"), open - strlen("CREATE TABLE")));
  LocalLiteTableDef table = tableFromName(name);
  table.objectUid = newObjectUid();
  table.nextRowId = 1;

  std::vector<std::string> cols = splitCommaList(sql.substr(open + 1, close - open - 1));
  if (cols.empty())
    return reportError(env, "CREATE TABLE requires at least one column");

  for (size_t i = 0; i < cols.size(); i++)
    {
      std::string col = trim(cols[i]);
      size_t sp = col.find_first_of(" \t\r\n");
      if (sp == std::string::npos)
        return reportError(env, "invalid column definition: " + col);
      LocalLiteColumnDef c;
      c.name = unquoteIdent(col.substr(0, sp));
      c.type = trim(col.substr(sp + 1));
      c.nullable = (upper(c.type).find("NOT NULL") == std::string::npos);
      if (containsUnsupportedType(c.type))
        return reportError(env, "unsupported local-lite column type: " + c.type);
      table.columns.push_back(c);
    }

  LocalLiteRocksDBStore store;
  std::string error;
  if (!store.createTable(table, &error))
    return reportError(env, error);

  writeLine(env, "--- SQL operation complete.");
  return 0;
}

static short processDrop(const std::string &sql, SqlciEnv *env)
{
  std::string name = trim(sql.substr(strlen("DROP TABLE")));
  LocalLiteTableDef table = tableFromName(name);
  LocalLiteRocksDBStore store;
  std::string error;
  if (!store.dropTable(table.catalog, table.schema, table.name, &error))
    return reportError(env, error);
  writeLine(env, "--- SQL operation complete.");
  return 0;
}

static short processInsert(const std::string &sql, SqlciEnv *env)
{
  std::string u = upper(sql);
  size_t valuesPos = u.find(" VALUES");
  if (valuesPos == std::string::npos)
    return reportError(env, "local-lite only supports INSERT INTO <table> VALUES (...)");
  std::string name = trim(sql.substr(strlen("INSERT INTO"), valuesPos - strlen("INSERT INTO")));
  std::vector<std::string> tuples;
  std::string parseError;
  if (!splitValueTuples(sql.substr(valuesPos + strlen(" VALUES")), &tuples, &parseError))
    return reportError(env, parseError);

  LocalLiteTableDef tableName = tableFromName(name);
  LocalLiteRocksDBStore store;
  std::string error;
  LocalLiteTableDef table;
  if (!store.loadTable(tableName.catalog, tableName.schema, tableName.name, &table, &error))
    return reportError(env, error);

  for (size_t t = 0; t < tuples.size(); t++)
    {
      std::vector<std::string> rawValues = splitCommaList(tuples[t]);
      if (rawValues.size() != table.columns.size())
        return reportError(env, "INSERT value count does not match local-lite table column count");

      std::vector<std::string> values;
      for (size_t i = 0; i < rawValues.size(); i++)
        values.push_back(unquoteValue(rawValues[i]));

      uint64_t rowId = 0;
      if (!store.allocateRowId(table, &rowId, &error))
        return reportError(env, error);
      if (!store.putRow(table, rowId, encodeFields(values), &error))
        return reportError(env, error);
    }

  char msg[80];
  snprintf(msg, sizeof(msg), "--- %lu row(s) inserted.",
           static_cast<unsigned long>(tuples.size()));
  writeLine(env, msg);
  return 0;
}

static short processSelect(const std::string &sql, SqlciEnv *env)
{
  std::string u = upper(sql);
  size_t fromPos = u.find(" FROM ");
  if (fromPos == std::string::npos || upper(trim(sql.substr(0, fromPos))) != "SELECT *")
    return reportError(env, "local-lite only supports SELECT * FROM <table>");

  std::string name = trim(sql.substr(fromPos + strlen(" FROM ")));
  LocalLiteTableDef tableName = tableFromName(name);
  LocalLiteRocksDBStore store;
  std::string error;
  LocalLiteTableDef table;
  if (!store.loadTable(tableName.catalog, tableName.schema, tableName.name, &table, &error))
    return reportError(env, error);

  std::vector<LocalLiteRow> rows;
  if (!store.scanRows(table, &rows, &error))
    return reportError(env, error);

  std::string heading;
  std::string underline;
  for (size_t i = 0; i < table.columns.size(); i++)
    {
      if (i)
        {
          heading += " ";
          underline += " ";
        }
      heading += table.columns[i].name;
      underline += std::string(table.columns[i].name.size() ? table.columns[i].name.size() : 1, '-');
    }

  writeLine(env, "");
  writeLine(env, heading);
  writeLine(env, underline);
  writeLine(env, "");

  for (size_t i = 0; i < rows.size(); i++)
    {
      std::vector<std::string> values;
      if (!decodeFields(rows[i].value, &values))
        return reportError(env, "invalid local-lite stored row");
      std::string line;
      for (size_t c = 0; c < values.size(); c++)
        {
          if (c)
            line += " ";
          line += values[c];
        }
      writeLine(env, line);
    }

  char msg[80];
  snprintf(msg, sizeof(msg), "--- %lu row(s) selected.",
           static_cast<unsigned long>(rows.size()));
  writeLine(env, "");
  writeLine(env, msg);
  return 0;
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
  if (startsWithWord(sql, "UPDATE") ||
      startsWithWord(sql, "DELETE") ||
      startsWithWord(sql, "MERGE"))
    {
      *retcode = reportError(sqlciEnv, "UPDATE, DELETE, and MERGE are not supported in local-lite");
      return true;
    }
  if (startsWithWord(sql, "UPSERT"))
    {
      *retcode = reportError(sqlciEnv, "UPSERT is not supported in local-lite v1; use INSERT");
      return true;
    }

  if (startsWithWord(sql, "CREATE TABLE"))
    {
      *retcode = processCreate(sql, sqlciEnv);
      return true;
    }
  if (startsWithWord(sql, "DROP TABLE"))
    {
      *retcode = processDrop(sql, sqlciEnv);
      return true;
    }
  if (startsWithWord(sql, "INSERT INTO"))
    {
      *retcode = processInsert(sql, sqlciEnv);
      return true;
    }
  if (startsWithWord(sql, "SELECT"))
    {
      std::string u = upper(sql);
      if (u.find(" FROM ") != std::string::npos &&
          u.find("VALUES") == std::string::npos)
        {
          *retcode = processSelect(sql, sqlciEnv);
          return true;
        }
    }

  return false;
}

#endif
