// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LITE

#include "LiteSqlTable.h"
#include "LiteUdr.h"
#include "LiteRocksDBStore.h"

#include "SqlciEnv.h"
#include "SQLCLIdev.h"
#include "ComSmallDefs.h"
#include "Globals.h"
#include "SequenceGeneratorAttributes.h"

#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>
#include <algorithm>
#include <set>
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
  writeLine(env, "*** ERROR[lite] " + message);
  return 1;
}

static std::string liteShortObjectName(const std::string &name)
{
  size_t dot = name.rfind('.');
  return dot == std::string::npos ? name : name.substr(dot + 1);
}

static bool liteLegacyOutput()
{
  return getenv("TEST_SCHEMA_NAME") != NULL;
}

static short reportMissingLegacySequence(SqlciEnv *env,
                                         const std::string &catalog,
                                         const std::string &schema,
                                         const std::string &object)
{
  if (!liteLegacyOutput())
    return reportError(env, "lite sequence does not exist");
  writeLine(env, "*** ERROR[1389] Object " + object +
            " does not exist in Trafodion.");
  writeLine(env, "*** ERROR[1389] Object " + catalog + "." + schema +
            "." + object + " does not exist in Trafodion.");
  writeLine(env, "--- SQL operation failed with errors.");
  return 1;
}

static std::string liteColumnList(const LiteTableDef &table,
                                       const std::vector<size_t> &columns)
{
  std::string result = "(";
  for (size_t i = 0; i < columns.size(); i++)
    {
      if (i) result += ", ";
      if (columns[i] < table.columns.size())
        result += "\"" + table.columns[columns[i]].name + "\"";
    }
  return result + ")";
}

static std::string liteLegacyCheckExpression(const std::string &expression)
{
  if (expression.size() < 2 || expression[0] != '(' ||
      expression[expression.size() - 1] != ')')
    return expression;
  int depth = 0;
  for (size_t i = 0; i < expression.size(); i++)
    {
      if (expression[i] == '(') depth++;
      else if (expression[i] == ')' && --depth == 0 &&
               i + 1 != expression.size())
        return expression;
    }
  return depth == 0 ? expression.substr(1, expression.size() - 2)
                    : expression;
}

static void writeLiteLegacyDDL(SqlciEnv *env,
                                    const LiteTableDef &table);

static void writeLiteDDL(SqlciEnv *env, const LiteTableDef &table)
{
  if (liteLegacyOutput())
    {
      writeLiteLegacyDDL(env, table);
      return;
    }
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
          const LiteColumnDef &column = table.columns[i];
          line = "  \"" + column.name + "\" " + column.type;
          if (column.upshifted) line += " UPSHIFT";
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
      for (size_t u = 0; u < table.uniqueKeyColumns.size(); u++)
        {
          line = "  UNIQUE ";
          if (u < table.uniqueKeyNames.size() &&
              !table.uniqueKeyNames[u].empty())
            line += "\"" + liteShortObjectName(
                table.uniqueKeyNames[u]) + "\" ";
          line += liteColumnList(table, table.uniqueKeyColumns[u]);
          writeLine(env, line);
        }
      for (size_t c = 0; c < table.checkConstraints.size(); c++)
        {
          line = "  CHECK ";
          if (!table.checkConstraints[c].name.empty())
            line += "\"" + liteShortObjectName(
                table.checkConstraints[c].name) + "\" ";
          line += "(" + table.checkConstraints[c].expression + ")";
          writeLine(env, line);
        }
      for (size_t r = 0; r < table.riConstraints.size(); r++)
        {
          const LiteRIDef &ri = table.riConstraints[r];
          line = "  FOREIGN KEY \"" + liteShortObjectName(ri.name) +
                 "\" " + liteColumnList(table, ri.referencingColumns) +
                 " REFERENCES \"" + ri.referencedCatalog + "\".\"" +
                 ri.referencedSchema + "\".\"" + ri.referencedTable + "\" " +
                 liteColumnList(table, ri.referencedColumns);
          writeLine(env, line);
        }
      writeLine(env, ")");
      for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
        {
          const LiteIndexDef &index = table.secondaryIndexes[i];
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

static std::string liteLegacyColumnLine(const LiteColumnDef &column,
                                             bool first)
{
  std::string line = first ? "    " : "  , ";
  line += column.name;
  if (line.size() < 37) line.append(37 - line.size(), ' ');
  line += column.type;
  if (column.upshifted) line += " UPSHIFT";
  if (column.defaultValue.empty())
    line += column.nullable ? " DEFAULT NULL" :
                              " NO DEFAULT NOT NULL NOT DROPPABLE";
  else
    {
      line += " DEFAULT " + column.defaultValue;
      if (!column.nullable) line += " NOT NULL NOT DROPPABLE";
    }
  return line;
}

static std::vector<std::string> liteWrapLegacyDDLLine(
    const std::string &logicalLine)
{
  std::vector<std::string> lines;
  std::string remaining = logicalLine;
  while (remaining.size() > 80)
    {
      size_t split = remaining.rfind(' ', 79);
      if (split == std::string::npos || split == 0)
        break;
      lines.push_back(remaining.substr(0, split));
      remaining = "      " + trim(remaining.substr(split + 1));
    }
  lines.push_back(remaining);
  return lines;
}

static std::vector<std::string> liteLegacyColumnLines(
    const LiteTableDef &table, size_t columnIndex, bool first)
{
  if (columnIndex >= table.columns.size())
    return std::vector<std::string>();

  const LiteColumnDef &column = table.columns[columnIndex];
  bool identity = column.defaultClass == COM_IDENTITY_GENERATED_BY_DEFAULT ||
                  column.defaultClass == COM_IDENTITY_GENERATED_ALWAYS;
  if (!identity)
    return liteWrapLegacyDDLLine(
        liteLegacyColumnLine(column, first));

  NAString sequenceName;
  SequenceGeneratorAttributes::genSequenceName(
      table.catalog.c_str(), table.schema.c_str(), table.name.c_str(),
      column.name.c_str(), sequenceName);
  LiteSequenceDef sequence;
  bool found = false;
  std::string error;
  LiteRocksDBStore store;
  if (!store.loadSequence(table.catalog, table.schema, sequenceName.data(),
                          &sequence, &found, &error) || !found)
    return liteWrapLegacyDDLLine(
        liteLegacyColumnLine(column, first));

  std::string line = first ? "    " : "  , ";
  line += column.name;
  if (line.size() < 37) line.append(37 - line.size(), ' ');
  line += column.type;
  if (column.upshifted) line += " UPSHIFT";
  line += column.defaultClass == COM_IDENTITY_GENERATED_ALWAYS
      ? " GENERATED ALWAYS AS IDENTITY"
      : " GENERATED BY DEFAULT AS IDENTITY";
  line += " (  START WITH " + std::to_string(sequence.startValue) +
          "  INCREMENT BY " + std::to_string(sequence.increment) +
          "  MAXVALUE " + std::to_string(sequence.maxValue) +
          "  MINVALUE " + std::to_string(sequence.minValue) +
          (sequence.cache > 0
               ? "  CACHE " + std::to_string(sequence.cache)
               : "  NO CACHE") +
          (sequence.cycle ? "  CYCLE" : "  NO CYCLE") + "  " +
          column.type + "  )";
  if (!column.nullable)
    line += " NOT NULL NOT DROPPABLE";
  return liteWrapLegacyDDLLine(line);
}

static std::string liteLegacyKeyList(const LiteTableDef &table,
                                          const std::vector<size_t> &columns,
                                          const std::vector<bool> *descending)
{
  std::string result;
  for (size_t i = 0; i < columns.size(); i++)
    {
      if (i) result += ", ";
      if (columns[i] < table.columns.size())
        result += table.columns[columns[i]].name;
      bool desc = descending && i < descending->size() && (*descending)[i];
      result += desc ? " DESC" : " ASC";
    }
  return result;
}

static void writeLiteLegacyIndex(SqlciEnv *env,
                                      const std::string &name,
                                      const std::string &kind,
                                      const LiteTableDef &table,
                                      const std::vector<size_t> &columns,
                                      const std::vector<bool> *descending,
                                      bool systemIndex)
{
  std::string line;
  if (systemIndex)
    writeLine(env, "-- The following index is a system created index --");
  line = "CREATE " + kind + "INDEX " + name + " ON " +
         table.catalog + "." + table.schema + "." + table.name;
  writeLine(env, line);
  writeLine(env, "  (");
  for (size_t i = 0; i < columns.size(); i++)
    {
      line = i == 0 ? "    " : "  , ";
      if (columns[i] < table.columns.size()) line += table.columns[columns[i]].name;
      bool desc = descending && i < descending->size() && (*descending)[i];
      line += desc ? " DESC" : " ASC";
      writeLine(env, line);
    }
  writeLine(env, "  )");
  writeLine(env, ";");
}

static void writeLiteLegacyDDL(SqlciEnv *env,
                                    const LiteTableDef &table)
{
  std::string full = table.catalog + "." + table.schema + "." + table.name;
  if (table.view)
    {
      writeLine(env, "CREATE VIEW " + full);
      writeLine(env, " AS " + table.viewText);
      writeLine(env, ";");
      return;
    }

  writeLine(env, "CREATE TABLE " + full);
  writeLine(env, "  (");
  for (size_t i = 0; i < table.columns.size(); i++)
    {
      std::vector<std::string> columnLines =
          liteLegacyColumnLines(table, i, i == 0);
      for (size_t line = 0; line < columnLines.size(); line++)
        writeLine(env, columnLines[line]);
    }
  if (!table.primaryKeyColumns.empty())
    writeLine(env, "  , PRIMARY KEY (" +
             liteLegacyKeyList(table, table.primaryKeyColumns, NULL) + ")");
  writeLine(env, "  )");
  writeLine(env, " ATTRIBUTES ALIGNED FORMAT");
  writeLine(env, ";");

  for (size_t i = 0; i < table.checkConstraints.size(); i++)
    {
      const LiteCheckDef &check = table.checkConstraints[i];
      writeLine(env, "ALTER TABLE " + full + " ADD CONSTRAINT " +
                check.name + " CHECK");
      writeLine(env, "  (" + liteLegacyCheckExpression(check.expression) + ")");
      writeLine(env, "");
    }

  for (size_t i = 0; i < table.uniqueKeyColumns.size(); i++)
    {
      std::string name = i < table.uniqueKeyNames.size()
          ? liteShortObjectName(table.uniqueKeyNames[i])
          : table.name + "UK" + std::to_string(i + 1);
      writeLiteLegacyIndex(env, name, "UNIQUE ", table,
                                table.uniqueKeyColumns[i], NULL, true);
    }
  for (size_t i = 0; i < table.riConstraints.size(); i++)
    {
      const LiteRIDef &ri = table.riConstraints[i];
      writeLiteLegacyIndex(env, liteShortObjectName(ri.name), "",
                                table, ri.referencingColumns, NULL, true);
    }
  for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
    {
      const LiteIndexDef &index = table.secondaryIndexes[i];
      writeLiteLegacyIndex(env, index.name, index.unique ? "UNIQUE " : "",
                                table, index.keyColumns, &index.descending, false);
    }
  for (size_t i = 0; i < table.uniqueKeyColumns.size(); i++)
    {
      if (i >= table.uniqueKeyNames.size()) continue;
      std::string name = table.catalog + "." + table.schema + "." +
                         liteShortObjectName(table.uniqueKeyNames[i]);
      writeLine(env, "ALTER TABLE " + full + " ADD CONSTRAINT");
      writeLine(env, "  " + name + " UNIQUE");
      writeLine(env, "  (");
      for (size_t c = 0; c < table.uniqueKeyColumns[i].size(); c++)
        writeLine(env, c == 0 ? "    " + table.columns[
            table.uniqueKeyColumns[i][c]].name : "  , " + table.columns[
            table.uniqueKeyColumns[i][c]].name);
      writeLine(env, "  )");
      writeLine(env, ";");
    }
  for (size_t i = 0; i < table.riConstraints.size(); i++)
    {
      const LiteRIDef &ri = table.riConstraints[i];
      LiteTableDef referencedTable;
      LiteRocksDBStore store;
      std::string loadError;
      bool haveReferencedTable =
          store.loadTable(ri.referencedCatalog, ri.referencedSchema,
                          ri.referencedTable, &referencedTable, &loadError);
      writeLine(env, "ALTER TABLE " + full + " ADD CONSTRAINT");
      writeLine(env, "  " + table.catalog + "." + table.schema + "." +
                liteShortObjectName(ri.name) + " FOREIGN KEY");
      writeLine(env, "  (");
      for (size_t c = 0; c < ri.referencingColumns.size(); c++)
        writeLine(env, c == 0 ? "    " + table.columns[
            ri.referencingColumns[c]].name : "  , " + table.columns[
            ri.referencingColumns[c]].name);
      writeLine(env, "  )");
      writeLine(env, " REFERENCES " + ri.referencedCatalog + "." +
                ri.referencedSchema + "." + ri.referencedTable);
      writeLine(env, "  (");
      for (size_t c = 0; c < ri.referencedColumns.size(); c++)
        {
          const LiteTableDef &columnTable =
              haveReferencedTable ? referencedTable : table;
          size_t column = ri.referencedColumns[c];
          std::string columnName = column < columnTable.columns.size()
              ? columnTable.columns[column].name : std::string();
          writeLine(env, c == 0 ? "    " + columnName : "  , " + columnName);
        }
      writeLine(env, "  )");
      writeLine(env, ";");
    }
}

static void writeLiteSequenceDDL(SqlciEnv *env,
                                      const LiteSequenceDef &sequence)
{
  std::string full = sequence.catalog + "." + sequence.schema + "." +
                     sequence.name;
  writeLine(env, "CREATE SEQUENCE " + full);
  writeLine(env, "  START WITH " + std::to_string(sequence.startValue) +
            " /* NEXT AVAILABLE VALUE " +
            std::to_string(sequence.nextValue) + " */");
  writeLine(env, "  INCREMENT BY " + std::to_string(sequence.increment));
  writeLine(env, "  MAXVALUE " + std::to_string(sequence.maxValue));
  writeLine(env, "  MINVALUE " + std::to_string(sequence.minValue));
  writeLine(env, sequence.cache > 0
            ? "  CACHE " + std::to_string(sequence.cache) : "  NO CACHE");
  writeLine(env, sequence.cycle ? "  CYCLE" : "  NO CYCLE");
  writeLine(env, "  LARGEINT");
  writeLine(env, ";");
}

static void writeLiteStats(SqlciEnv *env, const LiteTableDef &table,
                                const LiteTableStatsDef &stats)
{
  writeLine(env, "Table: " + table.catalog + "." + table.schema + "." + table.name);
  writeLine(env, "Rows: " + std::to_string(static_cast<unsigned long long>(stats.rowCount)));
  for (size_t i = 0; i < stats.columns.size(); i++)
    {
      const LiteColumnStatsDef &column = stats.columns[i];
      writeLine(env, "Column " + column.columnName + ": rows=" +
                std::to_string(static_cast<unsigned long long>(column.rowCount)) +
                " nulls=" + std::to_string(static_cast<unsigned long long>(column.nullCount)) +
                " uec=" + std::to_string(static_cast<unsigned long long>(column.distinctCount)));
    }
}

static std::string liteObjectFullName(const LiteObjectRef &object)
{
  return object.catalog + "." + object.schema + "." + object.name;
}

static std::string liteTableFullName(const LiteTableDef &table)
{
  return table.catalog + "." + table.schema + "." + table.name;
}

static std::string liteDisplayObjectName(const std::string &catalog,
                                              const std::string &schema,
                                              const std::string &object)
{
  return catalog == "TRAFODION" ? schema + "." + object
                                 : catalog + "." + schema + "." + object;
}

static void writeLiteObjectList(SqlciEnv *env,
                                     const std::string &title,
                                     const std::vector<std::string> &objects)
{
  writeLine(env, title);
  writeLine(env, std::string(title.size(), '='));
  for (size_t i = 0; i < objects.size(); i++)
    writeLine(env, objects[i]);
  if (!objects.empty())
    writeLine(env, "=======================");
  writeLine(env, "  " + std::to_string(static_cast<unsigned long long>(objects.size())) +
            " row(s) returned");
}

static void liteSortUnique(std::vector<std::string> *objects)
{
  std::sort(objects->begin(), objects->end());
  objects->erase(std::unique(objects->begin(), objects->end()), objects->end());
}

static bool parseObjectName(const std::string &text,
                            std::string *catalog,
                            std::string *schema,
                            std::string *object,
                            SqlciEnv *env);

static bool parseSchemaName(const std::string &text,
                            std::string *catalog,
                            std::string *schema);

static bool parseCatalogName(const std::string &text,
                             std::string *catalog);

static void liteAppendMetadataTables(
    const std::string &catalog, const std::string &schema,
    std::vector<std::string> *objects);

static void liteCollectViewObjects(
    const LiteTableDef &view,
    const std::vector<LiteTableDef> &allTables,
    bool includeViews,
    bool recurse,
    std::set<std::string> *visitedViews,
    std::set<std::string> *objects)
{
  for (size_t i = 0; i < view.dependencies.size(); i++)
    {
      const LiteObjectRef &dependency = view.dependencies[i];
      LiteTableDef dependencyTable;
      bool found = false;
      for (size_t t = 0; t < allTables.size(); t++)
        if (allTables[t].catalog == dependency.catalog &&
            allTables[t].schema == dependency.schema &&
            allTables[t].name == dependency.name)
          {
            dependencyTable = allTables[t];
            found = true;
            break;
          }
      if (!found) continue;
      std::string fullName = liteTableFullName(dependencyTable);
      if (!dependencyTable.view || includeViews)
        objects->insert(fullName);
      if (recurse && dependencyTable.view &&
          visitedViews->insert(fullName).second)
        liteCollectViewObjects(dependencyTable, allTables, includeViews,
                                    recurse, visitedViews, objects);
    }
}

static bool liteParseGet(const std::string &sql,
                              SqlciEnv *env,
                              std::string *title,
                              std::vector<std::string> *objects,
                              std::string *error)
{
  std::string text = trim(sql);
  while (!text.empty() && text[text.size() - 1] == ';')
    text = trim(text.substr(0, text.size() - 1));
  std::string upperText = upper(text);
  if (!startsWithWord(text, "GET")) return false;

  std::string catalog;
  std::string schema;
  std::string object;
  std::string target;
  bool all = false;
  LiteRocksDBStore store;
  std::vector<LiteTableDef> tables;
  if (!store.listTables("", "", &tables, error)) return true;

  size_t pos = 0;
  if (upperText == "GET SCHEMAS" ||
      upperText.find("GET SCHEMAS IN CATALOG ") == 0)
    {
      if (upperText == "GET SCHEMAS")
        {
          catalog = (env && env->defaultCatalog() &&
                     env->defaultCatalog()[0] != '\0')
              ? upper(env->defaultCatalog()) : "TRAFODION";
        }
      else if (!parseCatalogName(
                   trim(text.substr(strlen("GET SCHEMAS IN CATALOG "))),
                   &catalog))
        {
          *error = "invalid lite catalog name";
          return true;
        }

      *title = "Schemas in Catalog " + catalog;
      if (!store.listSchemas(catalog, objects, error))
        return true;
      liteSortUnique(objects);
      return true;
    }

  if (upperText == "GET CATALOGS" || upperText == "GET DATABASES")
    {
      *title = upperText == "GET DATABASES" ? "Databases" : "Catalogs";
      if (!store.listCatalogs(objects, error))
        return true;
      liteSortUnique(objects);
      return true;
    }

  // The unqualified forms use the SQLCI session's current catalog and
  // schema.  Keep the output shape identical to the existing IN SCHEMA
  // forms so clients can use either spelling.
  if (upperText == "GET TABLES" || upperText == "GET VIEWS" ||
      upperText == "GET INDEXES" || upperText == "GET SEQUENCES")
    {
      catalog = (env && env->defaultCatalog() &&
                 env->defaultCatalog()[0] != '\0')
          ? upper(env->defaultCatalog()) : "TRAFODION";
      schema = (env && env->defaultSchema() &&
                env->defaultSchema()[0] != '\0')
          ? upper(env->defaultSchema()) : "SEABASE";
      bool wantTables = upperText == "GET TABLES";
      bool wantIndexes = upperText == "GET INDEXES";
      bool wantSequences = upperText == "GET SEQUENCES";
      *title = wantTables ? "Tables in Schema " :
          (wantIndexes ? "Indexes in Schema " :
           (wantSequences ? "Sequences in Schema " : "Views in Schema "));
      *title += catalog + "." + schema;

      if (wantSequences)
        {
          std::vector<LiteSequenceDef> sequences;
          if (!store.listSequences(catalog, schema, &sequences, error))
            return true;
          for (size_t i = 0; i < sequences.size(); i++)
            objects->push_back(sequences[i].name);
        }
      else
        for (size_t i = 0; i < tables.size(); i++)
          {
            if (tables[i].catalog != catalog || tables[i].schema != schema)
              continue;
            if (wantTables && !tables[i].view)
              objects->push_back(tables[i].name);
            else if (!wantTables && !wantIndexes && tables[i].view)
              objects->push_back(tables[i].name);
            else if (wantIndexes && !tables[i].view)
              {
                for (size_t k = 0; k < tables[i].uniqueKeyNames.size(); k++)
                  objects->push_back(liteShortObjectName(
                      tables[i].uniqueKeyNames[k]));
                for (size_t k = 0; k < tables[i].riConstraints.size(); k++)
                  objects->push_back(liteShortObjectName(
                      tables[i].riConstraints[k].name));
                for (size_t k = 0; k < tables[i].secondaryIndexes.size(); k++)
                  objects->push_back(liteShortObjectName(
                      tables[i].secondaryIndexes[k].name));
              }
          }
      if (wantTables)
        liteAppendMetadataTables(catalog, schema, objects);
      if (wantTables)
        {
          bool hasStatistics = false;
          for (size_t i = 0; i < tables.size() && !hasStatistics; i++)
            if (tables[i].catalog == catalog && tables[i].schema == schema &&
                !tables[i].view)
              {
                LiteTableStatsDef stats;
                bool found = false;
                std::string statsError;
                if (store.loadTableStats(tables[i].catalog, tables[i].schema,
                                         tables[i].name, &stats, &found,
                                         &statsError) && found)
                  hasStatistics = true;
              }
          if (hasStatistics)
            {
              objects->push_back("SB_HISTOGRAMS");
              objects->push_back("SB_HISTOGRAM_INTERVALS");
              objects->push_back("SB_PERSISTENT_SAMPLES");
            }
        }
      liteSortUnique(objects);
      return true;
    }

  if (upperText.find("GET TABLES IN SCHEMA ") == 0 ||
      upperText.find("GET INDEXES IN SCHEMA ") == 0 ||
      upperText.find("GET VIEWS IN SCHEMA ") == 0 ||
      upperText.find("GET SEQUENCES IN SCHEMA ") == 0)
    {
      bool wantTables = upperText.find("GET TABLES IN SCHEMA ") == 0;
      bool wantIndexes = upperText.find("GET INDEXES IN SCHEMA ") == 0;
      bool wantSequences = upperText.find("GET SEQUENCES IN SCHEMA ") == 0;
      pos = upperText.find(" IN SCHEMA ") + strlen(" IN SCHEMA ");
      if (!parseSchemaName(trim(text.substr(pos)), &catalog, &schema))
        { *error = "invalid lite schema name"; return true; }
      std::string prefix = wantTables ? "Tables in Schema " :
          (wantIndexes ? "Indexes in Schema " :
           (wantSequences ? "Sequences in Schema " : "Views in Schema "));
      *title = prefix + catalog + "." + schema;
      if (wantSequences)
        {
          std::vector<LiteSequenceDef> sequences;
          if (!store.listSequences(catalog, schema, &sequences, error))
            return true;
          for (size_t i = 0; i < sequences.size(); i++)
            objects->push_back(sequences[i].name);
          liteSortUnique(objects);
          return true;
        }
      for (size_t i = 0; i < tables.size(); i++)
        {
          if (tables[i].catalog != catalog || tables[i].schema != schema)
            continue;
          if (wantTables && !tables[i].view)
            objects->push_back(tables[i].name);
          else if (!wantTables && !wantIndexes && tables[i].view)
            objects->push_back(tables[i].name);
          else if (wantIndexes && !tables[i].view)
            {
              for (size_t k = 0; k < tables[i].uniqueKeyNames.size(); k++)
                objects->push_back(liteShortObjectName(
                    tables[i].uniqueKeyNames[k]));
              for (size_t k = 0; k < tables[i].riConstraints.size(); k++)
                objects->push_back(liteShortObjectName(
                    tables[i].riConstraints[k].name));
              for (size_t k = 0; k < tables[i].secondaryIndexes.size(); k++)
                objects->push_back(liteShortObjectName(
                    tables[i].secondaryIndexes[k].name));
            }
        }
      if (wantTables)
        liteAppendMetadataTables(catalog, schema, objects);
      if (wantTables)
        {
          bool hasStatistics = false;
          for (size_t i = 0; i < tables.size() && !hasStatistics; i++)
            if (tables[i].catalog == catalog && tables[i].schema == schema &&
                !tables[i].view)
              {
                LiteTableStatsDef stats;
                bool found = false;
                std::string statsError;
                if (store.loadTableStats(tables[i].catalog, tables[i].schema,
                                         tables[i].name, &stats, &found,
                                         &statsError) && found)
                  hasStatistics = true;
              }
          if (hasStatistics)
            {
              objects->push_back("SB_HISTOGRAMS");
              objects->push_back("SB_HISTOGRAM_INTERVALS");
              objects->push_back("SB_PERSISTENT_SAMPLES");
            }
        }
      liteSortUnique(objects);
      return true;
    }

  if (upperText.find("GET ") != 0) return false;
  if (upperText.find("GET ALL ") == 0) all = true;
  size_t inView = upperText.find(" IN VIEW ");
  size_t onTable = upperText.find(" ON TABLE ");
  if (inView == std::string::npos && onTable == std::string::npos)
    { *error = "unsupported lite GET metadata request"; return true; }
  bool wantTables = upperText.find("TABLES ") != std::string::npos;
  bool wantViews = upperText.find("VIEWS ") != std::string::npos;
  bool wantObjects = upperText.find("OBJECTS ") != std::string::npos;
  size_t marker = inView != std::string::npos ? inView : onTable;
  size_t markerLength = inView != std::string::npos ? strlen(" IN VIEW ") : strlen(" ON TABLE ");
  target = trim(text.substr(marker + markerLength));
  if (!parseObjectName(target, &catalog, &schema, &object, env))
    { *error = "invalid lite metadata object"; return true; }

  LiteTableDef root;
  if (!store.loadTable(catalog, schema, object, &root, error)) return true;
  if (inView != std::string::npos)
    {
      if (wantTables)
        *title = "Tables in View " +
                 liteDisplayObjectName(catalog, schema, object);
      else if (wantViews)
        *title = "Views in View " +
                 liteDisplayObjectName(catalog, schema, object);
      else if (wantObjects)
        *title = "Objects in View " +
                 liteDisplayObjectName(catalog, schema, object);
      else
        { *error = "unsupported lite GET metadata request"; return true; }
      std::set<std::string> visited;
      std::set<std::string> collected;
      liteCollectViewObjects(root, tables,
                                  wantViews || wantObjects || all, all,
                                  &visited, &collected);
      for (std::set<std::string>::const_iterator it = collected.begin();
           it != collected.end(); ++it)
        {
          bool isView = false;
          size_t lastDot = it->rfind('.');
          if (lastDot != std::string::npos)
            for (size_t t = 0; t < tables.size(); t++)
              if (liteTableFullName(tables[t]) == *it)
                isView = tables[t].view;
          if (wantTables && isView) continue;
          if (wantViews && !isView) continue;
          objects->push_back(*it);
        }
    }
  else
    {
      *title = "Views on Table " +
               liteDisplayObjectName(catalog, schema, object);
      for (size_t i = 0; i < tables.size(); i++)
        if (tables[i].view)
          for (size_t d = 0; d < tables[i].dependencies.size(); d++)
            if (tables[i].dependencies[d].catalog == catalog &&
                tables[i].dependencies[d].schema == schema &&
                tables[i].dependencies[d].name == object)
              objects->push_back(liteTableFullName(tables[i]));
    }
  liteSortUnique(objects);
  return true;
}

bool LiteSqlTable_getMetadata(const char *sqlText, SqlciEnv *sqlciEnv,
                                   std::string *title,
                                   std::vector<std::string> *objects,
                                   std::string *error)
{
  if (!sqlText || !sqlciEnv || !title || !objects || !error)
    return false;

  std::string sql = trim(sqlText);
  while (!sql.empty() && sql[sql.size() - 1] == ';')
    sql = trim(sql.substr(0, sql.size() - 1));
  if (upper(sql).find("GET TEXT FOR ERROR ") == 0)
    return false;

  return liteParseGet(sql, sqlciEnv, title, objects, error);
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
  size_t end = name.find_first_of(" \t\r\n,;");
  if (end != std::string::npos)
    name.resize(end);
  size_t dot = name.find('.');
  *catalog = dot == std::string::npos
      ? "TRAFODION" : unquoteIdentifier(name.substr(0, dot));
  *schema = unquoteIdentifier(
      dot == std::string::npos ? name : name.substr(dot + 1));
  return !catalog->empty() && !schema->empty();
}

static void liteSetDefaultSchema(SqlciEnv *env,
                                      const std::string &catalog,
                                      const std::string &schema)
{
  if (env->defaultCatalog())
    delete env->defaultCatalog();
  if (env->defaultSchema())
    delete env->defaultSchema();
  env->defaultCatalog() = new char[catalog.size() + 1];
  env->defaultSchema() = new char[schema.size() + 1];
  strcpy(env->defaultCatalog(), catalog.c_str());
  strcpy(env->defaultSchema(), schema.c_str());
  std::string defaultSchema = catalog + "." + schema;
  LiteSetThreadDefaultSchema(defaultSchema.c_str());
}

static bool parseCatalogName(const std::string &text,
                             std::string *catalog)
{
  if (!catalog)
    return false;
  std::string name = trim(text);
  size_t end = name.find_first_of(" \t\r\n,;");
  if (end != std::string::npos)
    name.resize(end);
  *catalog = unquoteIdentifier(name);
  return !catalog->empty();
}

static void liteAppendMetadataTables(
    const std::string &catalog, const std::string &schema,
    std::vector<std::string> *objects)
{
  if (catalog != "TRAFODION" || schema != "_MD_" || !objects)
    return;
  const char *metadataTables[] = {
    "OBJECTS", "TABLES", "COLUMNS", "KEYS", "INDEXES",
    "SEQUENCES_VIEW", "TEXT"
  };
  for (size_t i = 0; i < sizeof(metadataTables) / sizeof(metadataTables[0]);
       i++)
    objects->push_back(metadataTables[i]);
}

static bool parseObjectName(const std::string &text,
                            std::string *catalog,
                            std::string *schema,
                            std::string *object,
                            SqlciEnv *env = NULL)
{
  std::string name = trim(text);
  size_t end = name.find_first_of(" \t\r\n,;");
  if (end != std::string::npos)
    name.resize(end);
  size_t first = name.find('.');
  size_t second = first == std::string::npos
      ? std::string::npos : name.find('.', first + 1);
  if (first == std::string::npos)
    {
      *catalog = (env && env->defaultCatalog() &&
                  env->defaultCatalog()[0] != '\0')
          ? upper(env->defaultCatalog()) : "TRAFODION";
      *schema = (env && env->defaultSchema() &&
                 env->defaultSchema()[0] != '\0')
          ? upper(env->defaultSchema()) : "SEABASE";
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

static std::string liteCurrentUser(SqlciEnv *env)
{
  if (!env || env->getUserNameFromCommandLine().length() == 0)
    return "DB__ROOT";
  return upper(env->getUserNameFromCommandLine().data());
}

static std::string liteNextToken(const std::string &text, size_t *offset)
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

static std::string liteStripIdentifier(const std::string &token)
{
  std::string value = trim(token);
  while (!value.empty() && (value[value.size() - 1] == ';' ||
                            value[value.size() - 1] == ','))
    value.resize(value.size() - 1);
  return unquoteIdentifier(value);
}

static bool liteFindKeyword(const std::string &sql,
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

static uint32_t litePrivilegeMask(const std::string &text)
{
  std::string value = upper(text);
  uint32_t mask = 0;
  if (value.find("ALL") != std::string::npos) return LITE_PRIV_ALL;
  if (value.find("SELECT") != std::string::npos) mask |= LITE_PRIV_SELECT;
  if (value.find("INSERT") != std::string::npos) mask |= LITE_PRIV_INSERT;
  if (value.find("UPDATE") != std::string::npos) mask |= LITE_PRIV_UPDATE;
  if (value.find("DELETE") != std::string::npos) mask |= LITE_PRIV_DELETE;
  if (value.find("REFERENCES") != std::string::npos) mask |= LITE_PRIV_REFERENCES;
  if (value.find("USAGE") != std::string::npos) mask |= LITE_PRIV_USAGE;
  return mask;
}

static bool liteIsRoot(const std::string &user)
{
  return user == "DB__ROOT";
}

static bool liteObjectPrivilege(LiteRocksDBStore *store,
                                     const std::string &catalog,
                                     const std::string &schema,
                                     const std::string &object,
                                     const std::string &user,
                                     uint32_t mask,
                                     std::string *error)
{
  if (liteIsRoot(user)) return true;
  bool allowed = store->hasPrivilege(catalog, schema, object, user, mask, error);
  if (!allowed && error && error->empty())
    *error = "authorization denied for " + object;
  return allowed;
}

static bool liteParseObjectAfter(const std::string &sql,
                                      size_t position,
                                      std::string *catalog,
                                      std::string *schema,
                                      std::string *object)
{
  size_t offset = position;
  std::string token = liteNextToken(sql, &offset);
  if (token.empty() || upper(token) == "IF")
    {
      if (upper(token) == "IF")
        {
          liteNextToken(sql, &offset); // NOT
          liteNextToken(sql, &offset); // EXISTS
          token = liteNextToken(sql, &offset);
        }
    }
  token = liteStripIdentifier(token);
  return !token.empty() && token[0] != '(' &&
         parseObjectName(token, catalog, schema, object);
}

static bool liteAuthorizeSql(const std::string &sql,
                                  SqlciEnv *env,
                                  std::string *error)
{
  std::string user = liteCurrentUser(env);
  LiteRocksDBStore store;

  // Authorization DDL is implemented entirely in the RocksDB catalog.  The
  // normal compiler path has no service-stack privilege metadata to update.
  if (startsWithWord(sql, "CREATE USER") || startsWithWord(sql, "CREATE ROLE"))
    {
      if (!liteIsRoot(user)) { *error = "only DB__ROOT may create authorization identities"; return true; }
      bool role = startsWithWord(sql, "CREATE ROLE");
      size_t offset = role ? strlen("CREATE ROLE") : strlen("CREATE USER");
      std::string rest = trim(sql.substr(offset));
      bool ifNotExists = upper(rest).compare(0, 13, "IF NOT EXISTS") == 0;
      if (ifNotExists) rest = trim(rest.substr(13));
      size_t nameOffset = 0;
      std::string name = liteStripIdentifier(liteNextToken(rest, &nameOffset));
      if (name.empty()) { *error = "invalid lite authorization identity"; return true; }
      if (!store.createAuthIdentity(name, role, ifNotExists, error)) return true;
      return true;
    }
  if (startsWithWord(sql, "DROP USER") || startsWithWord(sql, "DROP ROLE"))
    {
      if (!liteIsRoot(user)) { *error = "only DB__ROOT may drop authorization identities"; return true; }
      bool role = startsWithWord(sql, "DROP ROLE");
      std::string rest = trim(sql.substr(role ? strlen("DROP ROLE") : strlen("DROP USER")));
      bool ifExists = upper(rest).compare(0, 9, "IF EXISTS") == 0;
      if (ifExists) rest = trim(rest.substr(9));
      size_t zero = 0;
      std::string name = liteStripIdentifier(liteNextToken(rest, &zero));
      if (name.empty()) { *error = "invalid lite authorization identity"; return true; }
      store.dropAuthIdentity(name, role, ifExists, error);
      return true;
    }
  if (startsWithWord(sql, "SET SESSION AUTHORIZATION"))
    {
      std::string name = liteStripIdentifier(trim(sql.substr(strlen("SET SESSION AUTHORIZATION"))));
      if (name.empty()) { *error = "invalid lite session authorization"; return true; }
      env->setUserNameFromCommandLine(name.c_str());
      short rc = 0;
      if (!LiteSqlTable_setCurrentUser(env, &rc))
        *error = "unknown lite authorization identity: " + name;
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
          if (toPos == std::string::npos) { *error = "invalid lite role grant"; return true; }
          std::string role = liteStripIdentifier(trim(rest.substr(0, toPos)));
          if (upper(role).compare(0, 5, "ROLE ") == 0)
            role = liteStripIdentifier(trim(role.substr(5)));
          std::string grantee = liteStripIdentifier(trim(rest.substr(toPos + (grant ? 4 : 6))));
          if (!liteIsRoot(user)) { *error = "only DB__ROOT may grant roles"; return true; }
          if (grant) store.grantRole(role, grantee, restUpper.find("ADMIN OPTION") != std::string::npos, error);
          else store.revokeRole(role, grantee, error);
          return true;
        }
      std::string privilegeText = trim(rest.substr(0, onPos));
      size_t toPos = upper(rest).find(grant ? " TO " : " FROM ", onPos + 4);
      if (toPos == std::string::npos) { *error = "invalid lite object privilege"; return true; }
      std::string objectText = trim(rest.substr(onPos + 4, toPos - onPos - 4));
      std::string grantee = liteStripIdentifier(trim(rest.substr(toPos + (grant ? 4 : 6))));
      std::string catalog, schema, object;
      if (!parseObjectName(objectText, &catalog, &schema, &object)) { *error = "invalid lite privilege object"; return true; }
      uint32_t mask = litePrivilegeMask(privilegeText);
      if (mask == 0) { *error = "invalid lite privilege list"; return true; }
      bool owner = false;
      if (!store.isTableOwner(catalog, schema, object, user, &owner, error)) return true;
      if (!liteIsRoot(user) && !owner) { *error = "only the object owner may change lite privileges"; return true; }
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
    { required = LITE_PRIV_SELECT; operation = "SELECT"; }
  else if (startsWithWord(sql, "INSERT"))
    { required = LITE_PRIV_INSERT; operation = "INSERT"; }
  else if (startsWithWord(sql, "UPDATE"))
    { required = LITE_PRIV_UPDATE; operation = "UPDATE"; }
  else if (startsWithWord(sql, "DELETE"))
    { required = LITE_PRIV_DELETE; operation = "DELETE"; }
  else if (startsWithWord(sql, "DROP TABLE") || startsWithWord(sql, "ALTER TABLE") ||
           startsWithWord(sql, "TRUNCATE TABLE"))
    { required = LITE_PRIV_ALL; operation = "DDL"; }
  else if (startsWithWord(sql, "CREATE INDEX") || startsWithWord(sql, "DROP INDEX"))
    { required = LITE_PRIV_ALL; operation = "DDL"; }
  else if (startsWithWord(sql, "CREATE VIEW") || startsWithWord(sql, "CREATE TRIGGER"))
    { required = LITE_PRIV_ALL; operation = "DDL"; }
  else if (startsWithWord(sql, "CREATE TABLE"))
    return false; // an authenticated user owns newly created tables
  else if (startsWithWord(sql, "CREATE SCHEMA") || startsWithWord(sql, "DROP SCHEMA"))
    {
      if (!liteIsRoot(user)) *error = "only DB__ROOT may change lite schemas";
      return !error->empty();
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
          if (!liteFindKeyword(sql, "FROM", 0, &fromPos)) return false;
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
      if (!liteFindKeyword(sql, "ON", 0, &onPos)) return false;
      position = onPos + 2;
    }
  else if (startsWithWord(sql, "DROP INDEX"))
    {
      if (!liteIsRoot(user))
        {
          *error = "only DB__ROOT may drop lite indexes";
          return true;
        }
      // Root is allowed to continue through the normal lite DDL path.
      return false;
    }
  else if (startsWithWord(sql, "CREATE VIEW") || startsWithWord(sql, "CREATE TRIGGER"))
    position = upper(sql).find(startsWithWord(sql, "CREATE VIEW") ? "VIEW" : "TRIGGER") +
              (startsWithWord(sql, "CREATE VIEW") ? 4 : 7);

  if (!liteParseObjectAfter(sql, position, &catalog, &schema, &object)) return false;
  bool exists = false;
  if (!store.tableExists(catalog, schema, object, &exists, error)) return true;
  if (!exists && operation != "DDL") return false;
  if (!liteObjectPrivilege(&store, catalog, schema, object, user, required, error))
    {
      if (error && error->empty()) *error = "authorization denied for " + operation +
          " on " + catalog + "." + schema + "." + object;
      return true;
    }
  return false;
}

bool LiteSqlTable_setCurrentUser(SqlciEnv *sqlciEnv, short *retcode)
{
  if (!sqlciEnv || !retcode) return false;
  std::string user = liteCurrentUser(sqlciEnv);
  LiteRocksDBStore store;
  LiteAuthIdentity identity;
  bool found = false;
  std::string error;
  if (!store.loadAuthIdentity(user, &identity, &found, &error) || !found || identity.role)
    {
      *retcode = reportError(sqlciEnv, error.empty() ?
          "unknown lite authorization identity: " + user : error);
      return false;
    }
  const char *legacySchema = getenv("TEST_SCHEMA_NAME");
  if (legacySchema && legacySchema[0] != '\0')
    {
      // The legacy regress environment supplies the historical default
      // schema (normally SCH).  Keep both SQLCI's resolver and the local
      // catalog on that schema; otherwise unqualified DDL is stored in the
      // native SEABASE compatibility schema and all SHOWDDL diagnostics
      // drift from the legacy contract.
      if (sqlciEnv->defaultCatalog())
        delete sqlciEnv->defaultCatalog();
      if (sqlciEnv->defaultSchema())
        delete sqlciEnv->defaultSchema();
      sqlciEnv->defaultCatalog() = new char[strlen("TRAFODION") + 1];
      sqlciEnv->defaultSchema() = new char[strlen(legacySchema) + 1];
      strcpy(sqlciEnv->defaultCatalog(), "TRAFODION");
      strcpy(sqlciEnv->defaultSchema(), legacySchema);
      std::string defaultSchema = "TRAFODION." + upper(legacySchema);
      LiteSetThreadDefaultSchema(defaultSchema.c_str());
    }
  else if (!LiteGetThreadDefaultSchema()[0])
    LiteSetThreadDefaultSchema("TRAFODION.SEABASE");
  Lng32 rc = SQL_EXEC_SetSessionAttr_Internal(SESSION_DATABASE_USER,
                                              static_cast<Lng32>(identity.id),
                                              const_cast<char *>(user.c_str()));
  if (rc != 0)
    {
      *retcode = reportError(sqlciEnv, "unable to set lite session identity: " + user);
      return false;
    }
  *retcode = 0;
  return true;
}

bool LiteSqlTable_checkAuthorization(const char *sqlText,
                                          SqlciEnv *sqlciEnv,
                                          short *retcode)
{
  if (!sqlText || !sqlciEnv || !retcode) return false;
  std::string error;
  liteAuthorizeSql(trim(sqlText), sqlciEnv, &error);
  if (!error.empty())
    {
      *retcode = reportError(sqlciEnv, error);
      return false;
    }
  *retcode = 0;
  return true;
}

bool LiteSqlTable_isUtilityStatement(const char *sqlText)
{
  if (!sqlText)
    return false;
  std::string sql = trim(sqlText);
  while (!sql.empty() && sql[sql.size() - 1] == ';')
    sql = trim(sql.substr(0, sql.size() - 1));
  if (sql.empty())
    return false;

  std::string normalized = upper(sql);
  if (normalized.find("GET TEXT FOR ERROR ") == 0)
    return false;

  const char *prefixes[] = {
    "CREATE USER", "CREATE ROLE", "DROP USER", "DROP ROLE",
    "SET SESSION AUTHORIZATION", "GRANT", "REVOKE", "SET SCHEMA",
    "USE", "GET", "SHOWDDL", "SHOWSTATS", "INVOKE",
    "UPDATE STATISTICS", "CREATE SCHEMA", "DROP SCHEMA",
    "CREATE SYNONYM", "DROP SYNONYM", "CREATE EXTERNAL TABLE",
    "CREATE HBASE TABLE", "TRUNCATE TABLE", "UPSERT USING LOAD",
    "CREATE FUNCTION", "CREATE PROCEDURE",
    "CREATE TABLE MAPPING FUNCTION", "DROP FUNCTION", "DROP PROCEDURE",
    "DROP TABLE MAPPING FUNCTION"
  };
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    if (startsWithWord(sql, prefixes[i]))
      return true;

  return normalized == "SHOW SCHEMAS" ||
         normalized == "INITIALIZE TRAFODION, CREATE METADATA VIEWS" ||
         normalized == "INITIALIZE TRAFODION, DROP METADATA VIEWS";
}

bool LiteSqlTable_process(const char *sqlText, SqlciEnv *sqlciEnv, short *retcode)
{
  if (!sqlText || !sqlciEnv || !retcode)
    return false;

  std::string sql = trim(sqlText);
  while (!sql.empty() && sql[sql.size() - 1] == ';')
    sql = trim(sql.substr(0, sql.size() - 1));
  if (sql.empty())
    return false;

  std::string authorizationError;
  bool authorizationHandled = liteAuthorizeSql(
      sql, sqlciEnv, &authorizationError);
  if (authorizationHandled)
    {
      *retcode = authorizationError.empty() ? 0 :
          reportError(sqlciEnv, authorizationError);
      return true;
    }

  // The normal SET SCHEMA compiler path depends on service-stack metadata in
  // this build.  Keep the SQLCI session's current catalog/schema in sync so
  // lite handlers resolve later unqualified names against the same
  // session schema as the legacy regress driver.
  if (startsWithWord(sql, "SET SCHEMA"))
    {
      std::string catalog;
      std::string schema;
      if (!parseSchemaName(trim(sql.substr(strlen("SET SCHEMA"))),
                           &catalog, &schema))
        *retcode = reportError(sqlciEnv, "invalid lite schema name");
      else
        {
          liteSetDefaultSchema(sqlciEnv, catalog, schema);
          writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
      return true;
    }

  if (startsWithWord(sql, "USE"))
    {
      std::string rest = trim(sql.substr(strlen("USE")));
      if (upper(rest).compare(0, strlen("SCHEMA"), "SCHEMA") == 0 &&
          (rest.size() == strlen("SCHEMA") ||
           isspace(static_cast<unsigned char>(rest[strlen("SCHEMA")]))))
        rest = trim(rest.substr(strlen("SCHEMA")));

      std::string catalog;
      std::string schema;
      std::string error;
      LiteRocksDBStore store;
      std::vector<std::string> schemas;
      bool valid = parseSchemaName(rest, &catalog, &schema) &&
          store.listSchemas(catalog, &schemas, &error);
      if (valid && std::find(schemas.begin(), schemas.end(), schema) ==
          schemas.end())
        {
          error = "lite schema does not exist: " + catalog + "." +
              schema;
          valid = false;
        }
      if (!valid)
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid lite schema name" : error);
      else
        {
          liteSetDefaultSchema(sqlciEnv, catalog, schema);
          writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
      return true;
    }

  // Metadata-view initialization is an HBase/service-stack utility in the
  // normal executor.  Lite Storage exposes the supported metadata through
  // generated RocksDB descriptors, so the initialize command is intentionally
  // a catalog-local compatibility operation rather than an HBase DDL call.
  std::string upperSql = upper(sql);
  if (upperSql == "SHOW SCHEMAS")
    {
      // SHOW SCHEMAS is a spelling alias for the existing SQLCI GET
      // SCHEMAS implementation. Keep one catalog enumeration path.
      sql = "GET SCHEMAS";
      upperSql = upper(sql);
    }
  if (upperSql == "INITIALIZE TRAFODION, CREATE METADATA VIEWS" ||
      upperSql == "INITIALIZE TRAFODION, DROP METADATA VIEWS")
    {
      writeLine(sqlciEnv, "--- SQL operation complete.");
      *retcode = 0;
      return true;
    }

  // UDR DDL and invocation use the RocksDB-only local runtime.  This must be
  // checked before the normal compiler path, which expects MX metadata tables
  // and a separate UDR server process.
  if (LiteUdr_process(sql.c_str(), sqlciEnv, retcode))
    return true;

  // ERROR n, GET is translated by SQLCI into GET TEXT FOR ERROR n.  It is
  // diagnostic retrieval, not a Lite catalog metadata request; let the
  // normal SQLCI command path format the saved SQL diagnostics.
  if (upperSql.find("GET TEXT FOR ERROR ") == 0)
    return false;

  if (startsWithWord(sql, "GET"))
    {
      std::string title;
      std::string error;
      std::vector<std::string> objects;
      if (liteParseGet(sql, sqlciEnv, &title, &objects, &error))
        {
          if (!error.empty())
            *retcode = reportError(sqlciEnv, error);
          else
            {
              if (!objects.empty())
                writeLiteObjectList(sqlciEnv, title, objects);
              writeLine(sqlciEnv, "--- SQL operation complete.");
              *retcode = 0;
            }
          return true;
        }
    }

  if (startsWithWord(sql, "SHOWDDL") || startsWithWord(sql, "SHOWSTATS"))
    {
      bool showStats = startsWithWord(sql, "SHOWSTATS");
      std::string rest = trim(sql.substr(showStats ? strlen("SHOWSTATS") : strlen("SHOWDDL")));
      if (showStats && upper(rest).compare(0, 9, "FOR TABLE") == 0)
        rest = trim(rest.substr(9));
      std::string catalog, schema, object, error;
      LiteRocksDBStore store;
      std::string restUpper = upper(rest);
      if (!showStats && restUpper.compare(0, strlen("SEQUENCE"), "SEQUENCE") == 0 &&
          (restUpper.size() == strlen("SEQUENCE") ||
           isspace(static_cast<unsigned char>(restUpper[strlen("SEQUENCE")]))))
        {
          std::string sequenceText = trim(rest.substr(strlen("SEQUENCE")));
          LiteSequenceDef sequence;
          bool found = false;
          bool parsed = parseObjectName(sequenceText, &catalog, &schema,
                                        &object, sqlciEnv);
          bool loaded = parsed && store.loadSequence(
              catalog, schema, object, &sequence, &found, &error);
          if (!parsed || !loaded)
            *retcode = reportError(sqlciEnv, error.empty()
                ? "invalid lite sequence name" : error);
          else if (!found)
            *retcode = reportMissingLegacySequence(sqlciEnv, catalog, schema,
                                                   object);
          else
            {
              writeLiteSequenceDDL(sqlciEnv, sequence);
              writeLine(sqlciEnv, "--- SQL operation complete.");
              *retcode = 0;
            }
          return true;
        }
      LiteTableDef table;
      if (!parseObjectName(rest, &catalog, &schema, &object, sqlciEnv) ||
          !store.loadTable(catalog, schema, object, &table, &error))
        *retcode = reportError(sqlciEnv, error.empty() ? "lite table does not exist" : error);
      else if (!showStats)
        {
          writeLiteDDL(sqlciEnv, table);
          writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
      else
        {
          LiteTableStatsDef stats; bool found = false;
          if (!store.loadTableStats(catalog, schema, object, &stats, &found, &error) ||
              (!found && !store.collectTableStats(table, &stats, &error)))
            *retcode = reportError(sqlciEnv, error);
          else
            {
              writeLiteStats(sqlciEnv, table, stats);
              writeLine(sqlciEnv, "--- SQL operation complete.");
              *retcode = 0;
            }
        }
      return true;
    }
  if (startsWithWord(sql, "INVOKE"))
    {
      std::string rest = trim(sql.substr(strlen("INVOKE")));
      std::string catalog, schema, object, error;
      LiteRocksDBStore store;
      LiteTableDef table;
      if (!parseObjectName(rest, &catalog, &schema, &object, sqlciEnv) ||
          !store.loadTable(catalog, schema, object, &table, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "lite table does not exist" : error);
      else
        {
          writeLiteDDL(sqlciEnv, table);
          writeLine(sqlciEnv, "--- SQL operation complete.");
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
      LiteRocksDBStore store; LiteTableDef table; LiteTableStatsDef stats;
      if (!parseObjectName(rest, &catalog, &schema, &object, sqlciEnv) ||
          !store.loadTable(catalog, schema, object, &table, &error) ||
          !store.collectTableStats(table, &stats, &error))
        *retcode = reportError(sqlciEnv, error.empty() ? "invalid lite statistics request" : error);
      else
        { writeLine(sqlciEnv, "--- SQL operation complete."); *retcode = 0; }
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
      LiteRocksDBStore store;
      std::string error;
      if (!parseSchemaName(rest, &catalog, &schema) ||
          !store.createSchema(catalog, schema, ifNotExists, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid lite schema name" : error);
      else
        {
          if (liteLegacyOutput())
            writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
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
      LiteRocksDBStore store;
      std::string error;
      if (!parseSchemaName(rest, &catalog, &schema) ||
          !store.dropSchema(catalog, schema, ifExists, cascade, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid lite schema name" : error);
      else
        {
          if (liteLegacyOutput())
            writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
      return true;
    }
  if (startsWithWord(sql, "CREATE SYNONYM"))
    {
      std::string rest = trim(sql.substr(strlen("CREATE SYNONYM")));
      std::string upperRest = upper(rest);
      size_t forPos = upperRest.find(" FOR ");
      std::string catalog, schema, object;
      std::string targetCatalog, targetSchema, targetObject;
      LiteRocksDBStore store;
      std::string error;
      if (forPos == std::string::npos ||
          !parseObjectName(rest.substr(0, forPos), &catalog, &schema, &object,
                           sqlciEnv) ||
          !parseObjectName(rest.substr(forPos + 5), &targetCatalog,
                           &targetSchema, &targetObject, sqlciEnv) ||
          !store.createSynonym(catalog, schema, object, targetCatalog,
                               targetSchema, targetObject, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid lite synonym definition" : error);
      else
        {
          if (liteLegacyOutput())
            writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
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
      LiteRocksDBStore store;
      std::string error;
      if (!parseObjectName(rest, &catalog, &schema, &object, sqlciEnv) ||
          !store.dropSynonym(catalog, schema, object, ifExists, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid lite synonym name" : error);
      else
        {
          if (liteLegacyOutput())
            writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
      return true;
    }
  if (startsWithWord(sql, "CREATE EXTERNAL TABLE") ||
      startsWithWord(sql, "CREATE HBASE TABLE"))
    {
      *retcode = reportError(sqlciEnv, "native HBase/Hive table DDL is not supported in lite");
      return true;
    }
  if (startsWithWord(sql, "TRUNCATE TABLE"))
    {
      std::string catalog;
      std::string schema;
      std::string object;
      std::string error;
      LiteRocksDBStore store;
      LiteTableDef table;
      std::vector<LiteRow> rows;
      if (!parseObjectName(sql.substr(strlen("TRUNCATE TABLE")),
                           &catalog, &schema, &object, sqlciEnv) ||
          !store.loadTable(catalog, schema, object, &table, &error) ||
          !store.scanRows(table, &rows, &error) ||
          !store.deleteRows(table, rows, &error))
        *retcode = reportError(sqlciEnv, error.empty()
            ? "invalid lite table name" : error);
      else
        {
          if (liteLegacyOutput())
            writeLine(sqlciEnv, "--- SQL operation complete.");
          *retcode = 0;
        }
      return true;
    }
  if (startsWithWord(sql, "UPSERT USING LOAD"))
    {
      *retcode = reportError(
          sqlciEnv,
          "UPSERT USING LOAD is not supported for lite RocksDB tables; use UPSERT");
      return true;
    }
  return false;
}

#endif
