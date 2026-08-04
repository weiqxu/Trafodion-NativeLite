// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteSqlTable.h"
#include "LocalLiteUdr.h"
#include "LocalLiteRocksDBStore.h"

#include "SqlciEnv.h"
#include "SQLCLIdev.h"

#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>
#include <cstdlib>

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

static void writeLocalLiteDDL(SqlciEnv *env, const LocalLiteTableDef &table)
{
  std::string line = "CREATE ";
  line += table.view ? "VIEW " : "TABLE ";
  line += "\"" + table.catalog + "\".\"" + table.schema + "\".\"" + table.name + "\"";
  writeLine(env, line);
  if (table.view)
    writeLine(env, " AS " + table.viewText);
  else
    {
      writeLine(env, "(");
      for (size_t i = 0; i < table.columns.size(); i++)
        {
          const LocalLiteColumnDef &column = table.columns[i];
          line = "  \"" + column.name + "\" " + column.type;
          if (!column.nullable) line += " NOT NULL";
          if (!column.defaultValue.empty()) line += " DEFAULT " + column.defaultValue;
          if (i + 1 < table.columns.size()) line += ",";
          writeLine(env, line);
        }
      if (!table.primaryKeyColumns.empty())
        {
          line = "  PRIMARY KEY (";
          for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
            {
              if (i) line += ", ";
              line += "\"" + table.columns[table.primaryKeyColumns[i]].name + "\"";
            }
          writeLine(env, line + ")");
        }
      writeLine(env, ")");
      for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
        {
          const LocalLiteIndexDef &index = table.secondaryIndexes[i];
          line = "CREATE ";
          if (index.unique) line += "UNIQUE ";
          line += "INDEX \"" + index.name + "\" ON \"" +
                  table.catalog + "\".\"" + table.schema + "\".\"" +
                  table.name + "\" (";
          for (size_t k = 0; k < index.keyColumns.size(); k++)
            {
              if (k) line += ", ";
              line += "\"" + table.columns[index.keyColumns[k]].name +
                      "\"";
            }
          writeLine(env, line + ")");
        }
    }
  writeLine(env, ";");
}

static void writeLocalLiteStats(SqlciEnv *env, const LocalLiteTableDef &table,
                                const LocalLiteTableStatsDef &stats)
{
  writeLine(env, "Table: " + table.catalog + "." + table.schema + "." + table.name);
  writeLine(env, "Rows: " + std::to_string(static_cast<unsigned long long>(stats.rowCount)));
  for (size_t i = 0; i < stats.columns.size(); i++)
    {
      const LocalLiteColumnStatsDef &column = stats.columns[i];
      writeLine(env, "Column " + column.columnName + ": rows=" +
                std::to_string(static_cast<unsigned long long>(column.rowCount)) +
                " nulls=" + std::to_string(static_cast<unsigned long long>(column.nullCount)) +
                " uec=" + std::to_string(static_cast<unsigned long long>(column.distinctCount)));
    }
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

static std::string localLiteCurrentUser(SqlciEnv *env)
{
  if (!env || env->getUserNameFromCommandLine().length() == 0)
    return "DB__ROOT";
  return upper(env->getUserNameFromCommandLine().data());
}

static std::string localLiteNextToken(const std::string &text, size_t *offset)
{
  if (!offset) return std::string();
  while (*offset < text.size() &&
         isspace(static_cast<unsigned char>(text[*offset]))) (*offset)++;
  size_t begin = *offset;
  while (*offset < text.size() &&
         !isspace(static_cast<unsigned char>(text[*offset])) &&
         text[*offset] != ',' && text[*offset] != ';' && text[*offset] != ')')
    (*offset)++;
  return text.substr(begin, *offset - begin);
}

static std::string localLiteStripIdentifier(const std::string &token)
{
  std::string value = trim(token);
  while (!value.empty() && (value[value.size() - 1] == ';' ||
                            value[value.size() - 1] == ','))
    value.resize(value.size() - 1);
  return unquoteIdentifier(value);
}

static bool localLiteFindKeyword(const std::string &sql,
                                 const char *keyword,
                                 size_t start,
                                 size_t *position)
{
  std::string text = upper(sql);
  std::string word(keyword);
  size_t found = text.find(word, start);
  while (found != std::string::npos)
    {
      bool left = found == 0 || isspace(static_cast<unsigned char>(text[found - 1]));
      bool right = found + word.size() >= text.size() ||
                   isspace(static_cast<unsigned char>(text[found + word.size()])) ||
                   text[found + word.size()] == ';';
      if (left && right)
        {
          if (position) *position = found;
          return true;
        }
      found = text.find(word, found + 1);
    }
  return false;
}

static uint32_t localLitePrivilegeMask(const std::string &text)
{
  std::string value = upper(text);
  uint32_t mask = 0;
  if (value.find("ALL") != std::string::npos) return LOCAL_LITE_PRIV_ALL;
  if (value.find("SELECT") != std::string::npos) mask |= LOCAL_LITE_PRIV_SELECT;
  if (value.find("INSERT") != std::string::npos) mask |= LOCAL_LITE_PRIV_INSERT;
  if (value.find("UPDATE") != std::string::npos) mask |= LOCAL_LITE_PRIV_UPDATE;
  if (value.find("DELETE") != std::string::npos) mask |= LOCAL_LITE_PRIV_DELETE;
  if (value.find("REFERENCES") != std::string::npos) mask |= LOCAL_LITE_PRIV_REFERENCES;
  if (value.find("USAGE") != std::string::npos) mask |= LOCAL_LITE_PRIV_USAGE;
  return mask;
}

static bool localLiteIsRoot(const std::string &user)
{
  return user == "DB__ROOT";
}

static bool localLiteObjectPrivilege(LocalLiteRocksDBStore *store,
                                     const std::string &catalog,
                                     const std::string &schema,
                                     const std::string &object,
                                     const std::string &user,
                                     uint32_t mask,
                                     std::string *error)
{
  if (localLiteIsRoot(user)) return true;
  bool allowed = store->hasPrivilege(catalog, schema, object, user, mask, error);
  if (!allowed && error && error->empty())
    *error = "authorization denied for " + object;
  return allowed;
}

static bool localLiteParseObjectAfter(const std::string &sql,
                                      size_t position,
                                      std::string *catalog,
                                      std::string *schema,
                                      std::string *object)
{
  size_t offset = position;
  std::string token = localLiteNextToken(sql, &offset);
  if (token.empty() || upper(token) == "IF")
    {
      if (upper(token) == "IF")
        {
          localLiteNextToken(sql, &offset); // NOT
          localLiteNextToken(sql, &offset); // EXISTS
          token = localLiteNextToken(sql, &offset);
        }
    }
  token = localLiteStripIdentifier(token);
  return !token.empty() && token[0] != '(' &&
         parseObjectName(token, catalog, schema, object);
}

static bool localLiteAuthorizeSql(const std::string &sql,
                                  SqlciEnv *env,
                                  std::string *error)
{
  std::string user = localLiteCurrentUser(env);
  LocalLiteRocksDBStore store;

  // Authorization DDL is implemented entirely in the RocksDB catalog.  The
  // normal compiler path has no service-stack privilege metadata to update.
  if (startsWithWord(sql, "CREATE USER") || startsWithWord(sql, "CREATE ROLE"))
    {
      if (!localLiteIsRoot(user)) { *error = "only DB__ROOT may create authorization identities"; return true; }
      bool role = startsWithWord(sql, "CREATE ROLE");
      size_t offset = role ? strlen("CREATE ROLE") : strlen("CREATE USER");
      std::string rest = trim(sql.substr(offset));
      bool ifNotExists = upper(rest).compare(0, 13, "IF NOT EXISTS") == 0;
      if (ifNotExists) rest = trim(rest.substr(13));
      size_t nameOffset = 0;
      std::string name = localLiteStripIdentifier(localLiteNextToken(rest, &nameOffset));
      if (name.empty()) { *error = "invalid local-lite authorization identity"; return true; }
      if (!store.createAuthIdentity(name, role, ifNotExists, error)) return true;
      return true;
    }
  if (startsWithWord(sql, "DROP USER") || startsWithWord(sql, "DROP ROLE"))
    {
      if (!localLiteIsRoot(user)) { *error = "only DB__ROOT may drop authorization identities"; return true; }
      bool role = startsWithWord(sql, "DROP ROLE");
      std::string rest = trim(sql.substr(role ? strlen("DROP ROLE") : strlen("DROP USER")));
      bool ifExists = upper(rest).compare(0, 9, "IF EXISTS") == 0;
      if (ifExists) rest = trim(rest.substr(9));
      size_t zero = 0;
      std::string name = localLiteStripIdentifier(localLiteNextToken(rest, &zero));
      if (name.empty()) { *error = "invalid local-lite authorization identity"; return true; }
      store.dropAuthIdentity(name, role, ifExists, error);
      return true;
    }
  if (startsWithWord(sql, "SET SESSION AUTHORIZATION"))
    {
      std::string name = localLiteStripIdentifier(trim(sql.substr(strlen("SET SESSION AUTHORIZATION"))));
      if (name.empty()) { *error = "invalid local-lite session authorization"; return true; }
      env->setUserNameFromCommandLine(name.c_str());
      short rc = 0;
      if (!LocalLiteSqlTable_setCurrentUser(env, &rc))
        *error = "unknown local-lite authorization identity: " + name;
      return true;
    }
  if (startsWithWord(sql, "GRANT") || startsWithWord(sql, "REVOKE"))
    {
      bool grant = startsWithWord(sql, "GRANT");
      std::string rest = trim(sql.substr(grant ? strlen("GRANT") : strlen("REVOKE")));
      std::string restUpper = upper(rest);
      size_t onPos = restUpper.find(" ON ");
      if (onPos == std::string::npos)
        {
          size_t toPos = restUpper.find(grant ? " TO " : " FROM ");
          if (toPos == std::string::npos) { *error = "invalid local-lite role grant"; return true; }
          std::string role = localLiteStripIdentifier(trim(rest.substr(0, toPos)));
          if (upper(role).compare(0, 5, "ROLE ") == 0)
            role = localLiteStripIdentifier(trim(role.substr(5)));
          std::string grantee = localLiteStripIdentifier(trim(rest.substr(toPos + (grant ? 4 : 6))));
          if (!localLiteIsRoot(user)) { *error = "only DB__ROOT may grant roles"; return true; }
          if (grant) store.grantRole(role, grantee, restUpper.find("ADMIN OPTION") != std::string::npos, error);
          else store.revokeRole(role, grantee, error);
          return true;
        }
      std::string privilegeText = trim(rest.substr(0, onPos));
      size_t toPos = upper(rest).find(grant ? " TO " : " FROM ", onPos + 4);
      if (toPos == std::string::npos) { *error = "invalid local-lite object privilege"; return true; }
      std::string objectText = trim(rest.substr(onPos + 4, toPos - onPos - 4));
      std::string grantee = localLiteStripIdentifier(trim(rest.substr(toPos + (grant ? 4 : 6))));
      std::string catalog, schema, object;
      if (!parseObjectName(objectText, &catalog, &schema, &object)) { *error = "invalid local-lite privilege object"; return true; }
      uint32_t mask = localLitePrivilegeMask(privilegeText);
      if (mask == 0) { *error = "invalid local-lite privilege list"; return true; }
      bool owner = false;
      if (!store.isTableOwner(catalog, schema, object, user, &owner, error)) return true;
      if (!localLiteIsRoot(user) && !owner) { *error = "only the object owner may change local-lite privileges"; return true; }
      if (grant) store.grantPrivilege(catalog, schema, object, mask, grantee,
                                      restUpper.find("WITH GRANT OPTION") != std::string::npos, error);
      else store.revokePrivilege(catalog, schema, object, mask, grantee, error);
      return true;
    }

  std::string catalog, schema, object;
  uint32_t required = 0;
  std::string operation;
  size_t position = 0;
  if (startsWithWord(sql, "SELECT") || startsWithWord(sql, "INVOKE") ||
      startsWithWord(sql, "SHOWDDL") ||
      startsWithWord(sql, "SHOWSTATS") || startsWithWord(sql, "UPDATE STATISTICS"))
    { required = LOCAL_LITE_PRIV_SELECT; operation = "SELECT"; }
  else if (startsWithWord(sql, "INSERT"))
    { required = LOCAL_LITE_PRIV_INSERT; operation = "INSERT"; }
  else if (startsWithWord(sql, "UPDATE"))
    { required = LOCAL_LITE_PRIV_UPDATE; operation = "UPDATE"; }
  else if (startsWithWord(sql, "DELETE"))
    { required = LOCAL_LITE_PRIV_DELETE; operation = "DELETE"; }
  else if (startsWithWord(sql, "DROP TABLE") || startsWithWord(sql, "ALTER TABLE") ||
           startsWithWord(sql, "TRUNCATE TABLE"))
    { required = LOCAL_LITE_PRIV_ALL; operation = "DDL"; }
  else if (startsWithWord(sql, "CREATE INDEX") || startsWithWord(sql, "DROP INDEX"))
    { required = LOCAL_LITE_PRIV_ALL; operation = "DDL"; }
  else if (startsWithWord(sql, "CREATE VIEW") || startsWithWord(sql, "CREATE TRIGGER"))
    { required = LOCAL_LITE_PRIV_ALL; operation = "DDL"; }
  else if (startsWithWord(sql, "CREATE TABLE"))
    return false; // an authenticated user owns newly created tables
  else if (startsWithWord(sql, "CREATE SCHEMA") || startsWithWord(sql, "DROP SCHEMA"))
    {
      if (!localLiteIsRoot(user)) *error = "only DB__ROOT may change local-lite schemas";
      return true;
    }
  else
    return false;

  if (operation == "SELECT")
    {
      if (startsWithWord(sql, "INVOKE"))
        position = strlen("INVOKE");
      else
        {
          size_t fromPos = 0;
          if (!localLiteFindKeyword(sql, "FROM", 0, &fromPos)) return false;
          position = fromPos + strlen("FROM");
        }
    }
  else if (startsWithWord(sql, "INSERT")) position = upper(sql).find("INTO") + 4;
  else if (startsWithWord(sql, "UPDATE")) position = strlen("UPDATE");
  else if (startsWithWord(sql, "DELETE")) position = upper(sql).find("FROM") + 4;
  else if (startsWithWord(sql, "DROP TABLE") || startsWithWord(sql, "ALTER TABLE") ||
           startsWithWord(sql, "TRUNCATE TABLE")) position = upper(sql).find("TABLE") + 5;
  else if (startsWithWord(sql, "CREATE INDEX"))
    {
      size_t onPos = 0;
      if (!localLiteFindKeyword(sql, "ON", 0, &onPos)) return false;
      position = onPos + 2;
    }
  else if (startsWithWord(sql, "DROP INDEX"))
    {
      if (!localLiteIsRoot(user))
        {
          *error = "only DB__ROOT may drop local-lite indexes";
          return true;
        }
      // Root is allowed to continue through the normal local-lite DDL path.
      return false;
    }
  else if (startsWithWord(sql, "CREATE VIEW") || startsWithWord(sql, "CREATE TRIGGER"))
    position = upper(sql).find(startsWithWord(sql, "CREATE VIEW") ? "VIEW" : "TRIGGER") +
              (startsWithWord(sql, "CREATE VIEW") ? 4 : 7);

  if (!localLiteParseObjectAfter(sql, position, &catalog, &schema, &object)) return false;
  bool exists = false;
  if (!store.tableExists(catalog, schema, object, &exists, error)) return true;
  if (!exists && operation != "DDL") return false;
  if (!localLiteObjectPrivilege(&store, catalog, schema, object, user, required, error))
    {
      if (error && error->empty()) *error = "authorization denied for " + operation +
          " on " + catalog + "." + schema + "." + object;
      return true;
    }
  return false;
}

bool LocalLiteSqlTable_setCurrentUser(SqlciEnv *sqlciEnv, short *retcode)
{
  if (!sqlciEnv || !retcode) return false;
  std::string user = localLiteCurrentUser(sqlciEnv);
  LocalLiteRocksDBStore store;
  LocalLiteAuthIdentity identity;
  bool found = false;
  std::string error;
  if (!store.loadAuthIdentity(user, &identity, &found, &error) || !found || identity.role)
    {
      *retcode = reportError(sqlciEnv, error.empty() ?
          "unknown local-lite authorization identity: " + user : error);
      return false;
    }
  setenv("TRAF_LOCAL_LITE_USER", user.c_str(), 1);
  Lng32 rc = SQL_EXEC_SetSessionAttr_Internal(SESSION_DATABASE_USER,
                                              static_cast<Lng32>(identity.id),
                                              const_cast<char *>(user.c_str()));
  if (rc != 0)
    {
      *retcode = reportError(sqlciEnv, "unable to set local-lite session identity: " + user);
      return false;
    }
  *retcode = 0;
  return true;
}

bool LocalLiteSqlTable_checkAuthorization(const char *sqlText,
                                          SqlciEnv *sqlciEnv,
                                          short *retcode)
{
  if (!sqlText || !sqlciEnv || !retcode) return false;
  std::string error;
  localLiteAuthorizeSql(trim(sqlText), sqlciEnv, &error);
  if (!error.empty())
    {
      *retcode = reportError(sqlciEnv, error);
      return false;
    }
  *retcode = 0;
  return true;
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

  std::string authorizationError;
  bool authorizationHandled = localLiteAuthorizeSql(
      sql, sqlciEnv, &authorizationError);
  if (authorizationHandled)
    {
      *retcode = authorizationError.empty() ? 0 :
          reportError(sqlciEnv, authorizationError);
      return true;
    }

  // UDR DDL and invocation use the RocksDB-only local runtime.  This must be
  // checked before the normal compiler path, which expects MX metadata tables
  // and a separate UDR server process.
  if (LocalLiteUdr_process(sql.c_str(), sqlciEnv, retcode))
    return true;

  if (startsWithWord(sql, "SHOWDDL") || startsWithWord(sql, "SHOWSTATS"))
    {
      bool showStats = startsWithWord(sql, "SHOWSTATS");
      std::string rest = trim(sql.substr(showStats ? strlen("SHOWSTATS") : strlen("SHOWDDL")));
      if (showStats && upper(rest).compare(0, 9, "FOR TABLE") == 0)
        rest = trim(rest.substr(9));
      std::string catalog, schema, object, error;
      LocalLiteRocksDBStore store;
      LocalLiteTableDef table;
      if (!parseObjectName(rest, &catalog, &schema, &object) ||
          !store.loadTable(catalog, schema, object, &table, &error))
        *retcode = reportError(sqlciEnv, error.empty() ? "local-lite table does not exist" : error);
      else if (!showStats)
        { writeLocalLiteDDL(sqlciEnv, table); *retcode = 0; }
      else
        {
          LocalLiteTableStatsDef stats; bool found = false;
          if (!store.loadTableStats(catalog, schema, object, &stats, &found, &error) ||
              (!found && !store.collectTableStats(table, &stats, &error)))
            *retcode = reportError(sqlciEnv, error);
          else
            { writeLocalLiteStats(sqlciEnv, table, stats); *retcode = 0; }
        }
      return true;
    }
  if (startsWithWord(sql, "INVOKE"))
    {
      std::string rest = trim(sql.substr(strlen("INVOKE")));
      std::string catalog, schema, object, error;
      LocalLiteRocksDBStore store;
      LocalLiteTableDef table;
      if (!parseObjectName(rest, &catalog, &schema, &object) ||
          !store.loadTable(catalog, schema, object, &table, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "local-lite table does not exist" : error);
      else
        {
          writeLocalLiteDDL(sqlciEnv, table);
          *retcode = 0;
        }
      return true;
    }
  if (startsWithWord(sql, "UPDATE STATISTICS"))
    {
      std::string rest = trim(sql.substr(strlen("UPDATE STATISTICS")));
      std::string restUpper = upper(rest);
      size_t tablePos = restUpper.find("TABLE");
      if (tablePos != std::string::npos) rest = trim(rest.substr(tablePos + 5));
      std::string catalog, schema, object, error;
      LocalLiteRocksDBStore store; LocalLiteTableDef table; LocalLiteTableStatsDef stats;
      if (!parseObjectName(rest, &catalog, &schema, &object) ||
          !store.loadTable(catalog, schema, object, &table, &error) ||
          !store.collectTableStats(table, &stats, &error))
        *retcode = reportError(sqlciEnv, error.empty() ? "invalid local-lite statistics request" : error);
      else
        *retcode = 0;
      return true;
    }

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
      startsWithWord(sql, "CREATE HBASE TABLE"))
    {
      *retcode = reportError(sqlciEnv, "native HBase/Hive table DDL is not supported in local-lite");
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
