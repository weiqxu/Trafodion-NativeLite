// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteRocksDBStore.h"
#include "LocalLiteStorage.h"
#include "LocalLiteRowCodec.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <pthread.h>
#include <set>
#include <sstream>

#include <rocksdb/c.h>

#include "LocalLiteUnifiedRocksDB.h"

static const unsigned char LOCAL_LITE_ROW_FORMAT_VERSION = 1;

static void setError(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
}

static std::string localLiteHexKey(const std::string &key)
{
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(key.size() * 2);
  for (size_t i = 0; i < key.size(); i++)
    {
      unsigned char c = static_cast<unsigned char>(key[i]);
      out += hex[c >> 4];
      out += hex[c & 0x0f];
    }
  return out;
}

static void traceStatementSnapshot(const char *action,
                                   const std::string &tablePath,
                                   uint64_t statementExecutionId)
{
  const char *trace = getenv("TRAF_LOCAL_LITE_TRACE_SNAPSHOT");
  if (!trace || !trace[0])
    return;

  fprintf(stderr, "LOCAL_LITE_SNAPSHOT_%s execution=%llu table=%s\n",
          action,
          static_cast<unsigned long long>(statementExecutionId),
          tablePath.c_str());
}

static bool containsNoCase(const std::string &s, const char *needle)
{
  size_t needleLen = strlen(needle);
  if (needleLen == 0 || s.size() < needleLen)
    return false;

  for (size_t i = 0; i <= s.size() - needleLen; i++)
    {
      size_t j = 0;
      for (; j < needleLen; j++)
        {
          unsigned char a = static_cast<unsigned char>(s[i + j]);
          unsigned char b = static_cast<unsigned char>(needle[j]);
          if (tolower(a) != tolower(b))
            break;
        }
      if (j == needleLen)
        return true;
    }
  return false;
}

static bool checkRocksError(char *err, const std::string &prefix, std::string *error)
{
  if (!err)
    return true;
  std::string rocksError(err);
  if (containsNoCase(rocksError, "/LOCK") ||
      containsNoCase(rocksError, " lock ") ||
      containsNoCase(rocksError, "lock hold"))
    {
      setError(error,
               prefix + ": local-lite store is already open by another "
               "process; use one sqlci process per TRAF_LOCAL_STORE_DIR or "
               "choose a different TRAF_LOCAL_STORE_DIR: " + rocksError);
    }
  else
    {
      setError(error, prefix + ": " + rocksError);
    }
  rocksdb_free(err);
  return false;
}

static bool mkdirOne(const std::string &path, std::string *error)
{
  struct stat st;
  if (stat(path.c_str(), &st) == 0)
    {
      if (S_ISDIR(st.st_mode))
        return true;
      setError(error, path + " exists but is not a directory");
      return false;
    }

  if (mkdir(path.c_str(), 0755) == 0)
    return true;

  if (errno == EEXIST)
    return true;

  setError(error, "mkdir " + path + ": " + strerror(errno));
  return false;
}

static bool mkdirs(const std::string &path, std::string *error)
{
  if (path.empty())
    return true;

  std::string curr;
  size_t i = 0;
  if (path[0] == '/')
    {
      curr = "/";
      i = 1;
    }

  while (i <= path.size())
    {
      size_t slash = path.find('/', i);
      std::string part = path.substr(i, slash == std::string::npos ? slash : slash - i);
      if (!part.empty())
        {
          if (curr.size() > 1)
            curr += "/";
          curr += part;
          if (!mkdirOne(curr, error))
            return false;
        }
      if (slash == std::string::npos)
        break;
      i = slash + 1;
    }

  return true;
}

static std::string parentDir(const std::string &path)
{
  size_t pos = path.rfind('/');
  if (pos == std::string::npos)
    return ".";
  if (pos == 0)
    return "/";
  return path.substr(0, pos);
}

static std::string escapeName(const std::string &s)
{
  std::string out;
  char buf[4];
  for (size_t i = 0; i < s.size(); i++)
    {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if ((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '_' || c == '-' || c == '.')
        out += static_cast<char>(c);
      else
        {
          snprintf(buf, sizeof(buf), "%%%02X", c);
          out += buf;
        }
    }
  return out.empty() ? "_" : out;
}

static std::string tableKey(const std::string &catalog,
                            const std::string &schema,
                            const std::string &name)
{
  return "table|" + catalog + "|" + schema + "|" + name;
}

static std::string schemaKey(const std::string &catalog,
                             const std::string &schema)
{
  return "schema|" + catalog + "|" + schema;
}

static std::string synonymKey(const std::string &catalog,
                              const std::string &schema,
                              const std::string &name)
{
  return "synonym|" + catalog + "|" + schema + "|" + name;
}

static std::string sequenceKey(const std::string &catalog,
                               const std::string &schema,
                               const std::string &name)
{
  return "sequence|" + catalog + "|" + schema + "|" + name;
}

static std::string triggerKey(const std::string &catalog,
                              const std::string &schema,
                              const std::string &name)
{
  return "trigger|" + catalog + "|" + schema + "|" + name;
}

static const char *LOCAL_LITE_ROOT_NAME = "DB__ROOT";
static const uint64_t LOCAL_LITE_ROOT_ID = 33333;

static std::string authKey(const std::string &name)
{
  return "auth|" + name;
}

static std::string authIdKey(uint64_t id)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "authid|%020llu",
           static_cast<unsigned long long>(id));
  return buf;
}

static std::string roleKey(const std::string &role,
                           const std::string &grantee)
{
  return "role|" + role + "|" + grantee;
}

static std::string privilegeKey(const std::string &catalog,
                                const std::string &schema,
                                const std::string &object,
                                const std::string &grantee)
{
  return "priv|" + catalog + "|" + schema + "|" + object + "|" + grantee;
}

static std::string ownerKey(const std::string &catalog,
                            const std::string &schema,
                            const std::string &object)
{
  return "owner|" + catalog + "|" + schema + "|" + object;
}

static std::string authorizationGenerationKey()
{
  return "auth-generation";
}

static std::string encodeAuthIdentity(const LocalLiteAuthIdentity &identity)
{
  return std::string("LLA1\n") +
         std::to_string(static_cast<unsigned long long>(identity.id)) + "\n" +
         (identity.role ? "ROLE\n" : "USER\n");
}

static std::string tableKey(const LocalLiteTableDef &table)
{
  return tableKey(table.catalog, table.schema, table.name);
}

static std::string statsKey(const std::string &catalog,
                            const std::string &schema,
                            const std::string &name)
{
  return "stats|" + catalog + "|" + schema + "|" + name;
}

static std::string encodeStats(const LocalLiteTableStatsDef &stats)
{
  std::string out("LLST1\n");
  char buf[64];
  snprintf(buf, sizeof(buf), "%llu\n",
           static_cast<unsigned long long>(stats.rowCount));
  out += buf;
  snprintf(buf, sizeof(buf), "%llu\n",
           static_cast<unsigned long long>(stats.analyzedAt));
  out += buf;
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(stats.columns.size()));
  out += buf;
  for (size_t i = 0; i < stats.columns.size(); i++)
    {
      const LocalLiteColumnStatsDef &column = stats.columns[i];
      out += column.columnName + "\n";
      snprintf(buf, sizeof(buf), "%llu\n",
               static_cast<unsigned long long>(column.rowCount));
      out += buf;
      snprintf(buf, sizeof(buf), "%llu\n",
               static_cast<unsigned long long>(column.nullCount));
      out += buf;
      snprintf(buf, sizeof(buf), "%llu\n",
               static_cast<unsigned long long>(column.distinctCount));
      out += buf;
    }
  return out;
}

static bool readStatsLine(const std::string &encoded, size_t *offset,
                          std::string *line)
{
  if (!offset || !line || *offset > encoded.size())
    return false;
  size_t end = encoded.find('\n', *offset);
  if (end == std::string::npos)
    return false;
  line->assign(encoded, *offset, end - *offset);
  *offset = end + 1;
  return true;
}

static bool decodeStats(const std::string &encoded,
                        LocalLiteTableStatsDef *stats,
                        std::string *error)
{
  if (!stats || encoded.compare(0, 6, "LLST1\n") != 0)
    {
      setError(error, "invalid local-lite statistics metadata");
      return false;
    }
  size_t offset = 6;
  std::string line;
  if (!readStatsLine(encoded, &offset, &line)) return false;
  stats->rowCount = strtoull(line.c_str(), NULL, 10);
  if (!readStatsLine(encoded, &offset, &line)) return false;
  stats->analyzedAt = strtoull(line.c_str(), NULL, 10);
  if (!readStatsLine(encoded, &offset, &line)) return false;
  size_t count = static_cast<size_t>(strtoul(line.c_str(), NULL, 10));
  stats->columns.clear();
  for (size_t i = 0; i < count; i++)
    {
      LocalLiteColumnStatsDef column;
      if (!readStatsLine(encoded, &offset, &column.columnName) ||
          !readStatsLine(encoded, &offset, &line)) return false;
      column.rowCount = strtoull(line.c_str(), NULL, 10);
      if (!readStatsLine(encoded, &offset, &line)) return false;
      column.nullCount = strtoull(line.c_str(), NULL, 10);
      if (!readStatsLine(encoded, &offset, &line)) return false;
      column.distinctCount = strtoull(line.c_str(), NULL, 10);
      stats->columns.push_back(column);
    }
  if (offset != encoded.size())
    {
      setError(error, "trailing local-lite statistics metadata");
      return false;
    }
  return true;
}

static std::string indexKey(const std::string &catalog,
                            const std::string &schema,
                            const std::string &name)
{
  return "index|" + catalog + "|" + schema + "|" + name;
}

static std::string uidKey(uint64_t uid)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "uid|%020llu",
           static_cast<unsigned long long>(uid));
  return buf;
}

static void appendUint64(std::string &s, uint64_t v)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    s += static_cast<char>((v >> shift) & 0xff);
}

static uint64_t readUint64(const std::string &s, size_t *offset)
{
  uint64_t v = 0;
  for (int i = 0; i < 8 && *offset < s.size(); i++)
    {
      v = (v << 8) | static_cast<unsigned char>(s[*offset]);
      (*offset)++;
    }
  return v;
}

static void appendTriggerString(std::string &out, const std::string &value)
{
  appendUint64(out, value.size());
  out.append(value);
}

static bool readTriggerString(const std::string &encoded, size_t *offset,
                              std::string *value)
{
  if (!offset || !value || *offset + 8 > encoded.size())
    return false;
  uint64_t size = readUint64(encoded, offset);
  if (size > encoded.size() - *offset)
    return false;
  value->assign(encoded.data() + *offset, static_cast<size_t>(size));
  *offset += static_cast<size_t>(size);
  return true;
}

static std::string encodeTrigger(const LocalLiteTriggerDef &trigger)
{
  std::string out("LLG1", 4);
  appendTriggerString(out, trigger.catalog);
  appendTriggerString(out, trigger.schema);
  appendTriggerString(out, trigger.name);
  appendTriggerString(out, trigger.subjectCatalog);
  appendTriggerString(out, trigger.subjectSchema);
  appendTriggerString(out, trigger.subjectTable);
  appendUint64(out, static_cast<uint64_t>(trigger.operation));
  appendUint64(out, static_cast<uint64_t>(trigger.activation));
  appendUint64(out, static_cast<uint64_t>(trigger.granularity));
  appendUint64(out, trigger.timestamp);
  appendUint64(out, trigger.allUpdateColumns ? 1 : 0);
  appendUint64(out, trigger.updateColumns.size());
  for (size_t i = 0; i < trigger.updateColumns.size(); i++)
    appendUint64(out, trigger.updateColumns[i]);
  appendTriggerString(out, trigger.sqlText);
  return out;
}

static bool decodeTrigger(const std::string &encoded,
                          LocalLiteTriggerDef *trigger,
                          std::string *error)
{
  if (!trigger || encoded.size() < 4 || encoded.compare(0, 4, "LLG1") != 0)
    {
      setError(error, "invalid local-lite trigger metadata");
      return false;
    }
  size_t offset = 4;
  if (!readTriggerString(encoded, &offset, &trigger->catalog) ||
      !readTriggerString(encoded, &offset, &trigger->schema) ||
      !readTriggerString(encoded, &offset, &trigger->name) ||
      !readTriggerString(encoded, &offset, &trigger->subjectCatalog) ||
      !readTriggerString(encoded, &offset, &trigger->subjectSchema) ||
      !readTriggerString(encoded, &offset, &trigger->subjectTable) ||
      offset + 6 * 8 > encoded.size())
    {
      setError(error, "truncated local-lite trigger metadata");
      return false;
    }
  trigger->operation = static_cast<int>(readUint64(encoded, &offset));
  trigger->activation = static_cast<int>(readUint64(encoded, &offset));
  trigger->granularity = static_cast<int>(readUint64(encoded, &offset));
  trigger->timestamp = readUint64(encoded, &offset);
  trigger->allUpdateColumns = readUint64(encoded, &offset) != 0;
  uint64_t count = readUint64(encoded, &offset);
  if (count > (encoded.size() - offset) / 8)
    {
      setError(error, "invalid local-lite trigger column metadata");
      return false;
    }
  trigger->updateColumns.clear();
  for (uint64_t i = 0; i < count; i++)
    trigger->updateColumns.push_back(
        static_cast<size_t>(readUint64(encoded, &offset)));
  if (!readTriggerString(encoded, &offset, &trigger->sqlText) ||
      offset != encoded.size())
    {
      setError(error, "truncated local-lite trigger SQL text");
      return false;
    }
  return true;
}

static std::string encodeSequence(const LocalLiteSequenceDef &sequence)
{
  std::string value("LLS1", 4);
  appendUint64(value, sequence.objectUid);
  appendUint64(value, static_cast<uint64_t>(sequence.fsDataType));
  appendUint64(value, static_cast<uint64_t>(sequence.startValue));
  appendUint64(value, static_cast<uint64_t>(sequence.increment));
  appendUint64(value, static_cast<uint64_t>(sequence.minValue));
  appendUint64(value, static_cast<uint64_t>(sequence.maxValue));
  appendUint64(value, static_cast<uint64_t>(sequence.nextValue));
  appendUint64(value, sequence.cycle ? 1 : 0);
  appendUint64(value, static_cast<uint64_t>(sequence.cache));
  appendUint64(value, sequence.numCalls);
  appendUint64(value, sequence.internal ? 1 : 0);
  return value;
}

static bool decodeSequence(const std::string &value,
                           const std::string &catalog,
                           const std::string &schema,
                           const std::string &name,
                           LocalLiteSequenceDef *sequence,
                           std::string *error)
{
  if (!sequence || value.size() != 4 + 11 * 8 ||
      value.compare(0, 4, "LLS1") != 0)
    {
      setError(error, "invalid local-lite sequence metadata");
      return false;
    }
  size_t offset = 4;
  sequence->catalog = catalog;
  sequence->schema = schema;
  sequence->name = name;
  sequence->objectUid = readUint64(value, &offset);
  sequence->fsDataType = static_cast<int>(readUint64(value, &offset));
  sequence->startValue = static_cast<int64_t>(readUint64(value, &offset));
  sequence->increment = static_cast<int64_t>(readUint64(value, &offset));
  sequence->minValue = static_cast<int64_t>(readUint64(value, &offset));
  sequence->maxValue = static_cast<int64_t>(readUint64(value, &offset));
  sequence->nextValue = static_cast<int64_t>(readUint64(value, &offset));
  sequence->cycle = readUint64(value, &offset) != 0;
  sequence->cache = static_cast<int64_t>(readUint64(value, &offset));
  sequence->numCalls = readUint64(value, &offset);
  sequence->internal = readUint64(value, &offset) != 0;
  return true;
}

static std::string encodeTable(const LocalLiteTableDef &table)
{
  std::string out;
  out += "LLT16\n";
  out += table.catalog + "\n";
  out += table.schema + "\n";
  out += table.name + "\n";
  char buf[64];
  snprintf(buf, sizeof(buf), "%llu\n",
           static_cast<unsigned long long>(table.objectUid));
  out += buf;
  snprintf(buf, sizeof(buf), "%llu\n",
           static_cast<unsigned long long>(table.nextRowId));
  out += buf;
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.columns.size()));
  out += buf;
  for (size_t i = 0; i < table.columns.size(); i++)
    {
      out += table.columns[i].name;
      out += "\t";
      out += table.columns[i].type;
      out += "\t";
      out += table.columns[i].nullable ? "1" : "0";
      out += "\t";
      snprintf(buf, sizeof(buf), "%d", table.columns[i].defaultClass);
      out += buf;
      out += "\t";
      for (size_t j = 0; j < table.columns[i].defaultValue.size(); j++)
        {
          static const char hex[] = "0123456789ABCDEF";
          unsigned char c = static_cast<unsigned char>(table.columns[i].defaultValue[j]);
          out += hex[c >> 4];
          out += hex[c & 15];
        }
      out += "\t";
      out += table.columns[i].added ? "1" : "0";
      out += "\t";
      out += table.columns[i].upshifted ? "1" : "0";
      out += "\t";
      out += table.columns[i].division ? "1" : "0";
      out += "\t";
      for (size_t j = 0; j < table.columns[i].computedText.size(); j++)
        {
          static const char hex[] = "0123456789ABCDEF";
          unsigned char c = static_cast<unsigned char>(table.columns[i].computedText[j]);
          out += hex[c >> 4];
          out += hex[c & 15];
        }
      out += "\n";
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.primaryKeyColumns.size()));
  out += buf;
  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    {
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(table.primaryKeyColumns[i]));
      out += buf;
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.storeByColumns.size()));
  out += buf;
  for (size_t i = 0; i < table.storeByColumns.size(); i++)
    {
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(table.storeByColumns[i]));
      out += buf;
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.uniqueKeyColumns.size()));
  out += buf;
  for (size_t i = 0; i < table.uniqueKeyColumns.size(); i++)
    {
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(table.uniqueKeyColumns[i].size()));
      out += buf;
      for (size_t j = 0; j < table.uniqueKeyColumns[i].size(); j++)
        {
          snprintf(buf, sizeof(buf), "%lu\n",
                   static_cast<unsigned long>(table.uniqueKeyColumns[i][j]));
          out += buf;
        }
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.secondaryIndexes.size()));
  out += buf;
  for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
    {
      const LocalLiteIndexDef &index = table.secondaryIndexes[i];
      out += index.name + "\n";
      snprintf(buf, sizeof(buf), "%llu\n",
               static_cast<unsigned long long>(index.objectUid));
      out += buf;
      out += index.unique ? "1\n" : "0\n";
      snprintf(buf, sizeof(buf), "%u\n", index.keyEncodingVersion);
      out += buf;
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(index.keyColumns.size()));
      out += buf;
      for (size_t j = 0; j < index.keyColumns.size(); j++)
        {
          snprintf(buf, sizeof(buf), "%lu\t%d\n",
                   static_cast<unsigned long>(index.keyColumns[j]),
                   (j < index.descending.size() && index.descending[j]) ? 1 : 0);
          out += buf;
        }
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.checkConstraints.size()));
  out += buf;
  for (size_t i = 0; i < table.checkConstraints.size(); i++)
    {
      const std::string fields[2] = { table.checkConstraints[i].name,
                                      table.checkConstraints[i].expression };
      for (size_t f = 0; f < 2; f++)
        {
          snprintf(buf, sizeof(buf), "%lu\n",
                   static_cast<unsigned long>(fields[f].size()));
          out += buf;
          out.append(fields[f]);
          out += "\n";
        }
    }
  out += table.view ? "1\n" : "0\n";
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.viewText.size()));
  out += buf;
  out.append(table.viewText);
  out += "\n";
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.viewCheckText.size()));
  out += buf;
  out.append(table.viewCheckText);
  out += "\n";
  out += table.viewUpdatable ? "1\n" : "0\n";
  out += table.viewInsertable ? "1\n" : "0\n";
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.riConstraints.size()));
  out += buf;
  for (size_t i = 0; i < table.riConstraints.size(); i++)
    {
      const LocalLiteRIDef &ri = table.riConstraints[i];
      const std::string fields[5] = { ri.name, ri.referencedCatalog,
                                      ri.referencedSchema,
                                      ri.referencedTable,
                                      ri.referencedConstraint };
      for (size_t f = 0; f < 5; f++)
        {
          snprintf(buf, sizeof(buf), "%lu\n",
                   static_cast<unsigned long>(fields[f].size()));
          out += buf;
          out.append(fields[f]);
          out += "\n";
        }
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(ri.referencingColumns.size()));
      out += buf;
      for (size_t j = 0; j < ri.referencingColumns.size(); j++)
        {
          snprintf(buf, sizeof(buf), "%lu\t%lu\n",
                   static_cast<unsigned long>(ri.referencingColumns[j]),
                   static_cast<unsigned long>(ri.referencedColumns[j]));
          out += buf;
        }
    }
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.dependencies.size()));
  out += buf;
  for (size_t i = 0; i < table.dependencies.size(); i++)
    {
      const std::string fields[3] = { table.dependencies[i].catalog,
                                      table.dependencies[i].schema,
                                      table.dependencies[i].name };
      for (size_t f = 0; f < 3; f++)
        {
          snprintf(buf, sizeof(buf), "%lu\n",
                   static_cast<unsigned long>(fields[f].size()));
          out += buf;
          out.append(fields[f]);
          out += "\n";
        }
    }
  const std::string keyNames[1] = { table.primaryKeyName };
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(keyNames[0].size()));
  out += buf;
  out.append(keyNames[0]);
  out += "\n";
  snprintf(buf, sizeof(buf), "%lu\n",
           static_cast<unsigned long>(table.uniqueKeyNames.size()));
  out += buf;
  for (size_t i = 0; i < table.uniqueKeyNames.size(); i++)
    {
      snprintf(buf, sizeof(buf), "%lu\n",
               static_cast<unsigned long>(table.uniqueKeyNames[i].size()));
      out += buf;
      out.append(table.uniqueKeyNames[i]);
      out += "\n";
    }
  out += table.noSyskey ? "1\n" : "0\n";
  return out;
}

static bool nextLine(const std::string &s, size_t *pos, std::string *line)
{
  if (*pos > s.size())
    return false;
  size_t end = s.find('\n', *pos);
  if (end == std::string::npos)
    return false;
  *line = s.substr(*pos, end - *pos);
  *pos = end + 1;
  return true;
}

static bool decodeTable(const std::string &encoded,
                        LocalLiteTableDef *table,
                        std::string *error)
{
  std::string line;
  size_t pos = 0;
  if (!nextLine(encoded, &pos, &line) ||
      (line != "LLT1" && line != "LLT2" && line != "LLT3" &&
       line != "LLT4" && line != "LLT5" && line != "LLT6" &&
       line != "LLT7" && line != "LLT8" && line != "LLT9" &&
       line != "LLT10" && line != "LLT11" && line != "LLT12" &&
       line != "LLT13" && line != "LLT14" && line != "LLT15" &&
       line != "LLT16"))
    {
      setError(error, "invalid local-lite table metadata");
      return false;
    }
  const int metadataVersion = atoi(line.c_str() + 3);
  const bool hasKeyMetadata = metadataVersion >= 2;
  const bool hasUniqueMetadata = metadataVersion >= 3;
  const bool hasIndexMetadata = metadataVersion >= 4;
  const bool hasIndexEncodingMetadata = metadataVersion >= 5;
  const bool hasColumnDefaultMetadata = metadataVersion >= 6;
  const bool hasCheckMetadata = metadataVersion >= 7;
  const bool hasViewMetadata = metadataVersion >= 8;
  const bool hasRIMetadata = metadataVersion >= 9;
  const bool hasDependencyMetadata = metadataVersion >= 10;
  const bool hasConstraintNameMetadata = metadataVersion >= 11;
  const bool hasViewConstraintMetadata = metadataVersion >= 12;
  const bool hasUpshiftMetadata = metadataVersion >= 13;
  const bool hasComputedMetadata = metadataVersion >= 14;
  const bool hasStoreByMetadata = metadataVersion >= 15;
  const bool hasNoSyskeyMetadata = metadataVersion >= 16;
  if (!nextLine(encoded, &pos, &table->catalog) ||
      !nextLine(encoded, &pos, &table->schema) ||
      !nextLine(encoded, &pos, &table->name) ||
      !nextLine(encoded, &pos, &line))
    {
      setError(error, "truncated local-lite table metadata");
      return false;
    }
  table->objectUid = strtoull(line.c_str(), NULL, 10);
  if (!nextLine(encoded, &pos, &line))
    return false;
  table->nextRowId = strtoull(line.c_str(), NULL, 10);
  if (!nextLine(encoded, &pos, &line))
    return false;
  unsigned long count = strtoul(line.c_str(), NULL, 10);
  table->columns.clear();
  table->primaryKeyColumns.clear();
  table->storeByColumns.clear();
  table->primaryKeyName.clear();
  table->uniqueKeyColumns.clear();
  table->uniqueKeyNames.clear();
  table->secondaryIndexes.clear();
  table->checkConstraints.clear();
  table->riConstraints.clear();
  table->dependencies.clear();
  table->view = false;
  table->viewText.clear();
  table->viewCheckText.clear();
  table->viewUpdatable = false;
  table->viewInsertable = false;
  table->noSyskey = false;
  for (unsigned long i = 0; i < count; i++)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite column metadata");
          return false;
        }
      size_t p1 = line.find('\t');
      size_t p2 = (p1 == std::string::npos) ? p1 : line.find('\t', p1 + 1);
      if (p1 == std::string::npos || p2 == std::string::npos)
        {
          setError(error, "invalid local-lite column metadata");
          return false;
        }
      LocalLiteColumnDef col;
      col.name = line.substr(0, p1);
      col.type = line.substr(p1 + 1, p2 - p1 - 1);
      if (!hasColumnDefaultMetadata)
        col.nullable = (line.substr(p2 + 1) == "1");
      else
        {
          size_t p3 = line.find('\t', p2 + 1);
          size_t p4 = p3 == std::string::npos ? p3 : line.find('\t', p3 + 1);
          size_t p5 = p4 == std::string::npos ? p4 : line.find('\t', p4 + 1);
          if (p3 == std::string::npos || p4 == std::string::npos ||
              p5 == std::string::npos)
            {
              setError(error, "invalid local-lite column default metadata");
              return false;
            }
          col.nullable = (line.substr(p2 + 1, p3 - p2 - 1) == "1");
          col.defaultClass = atoi(line.substr(p3 + 1, p4 - p3 - 1).c_str());
          std::string hex = line.substr(p4 + 1, p5 - p4 - 1);
          if (hex.size() % 2 != 0)
            {
              setError(error, "invalid local-lite encoded default value");
              return false;
            }
          for (size_t j = 0; j < hex.size(); j += 2)
            {
              char pair[3] = { hex[j], hex[j + 1], 0 };
              char *end = NULL;
              unsigned long value = strtoul(pair, &end, 16);
              if (!end || *end)
                {
                  setError(error, "invalid local-lite encoded default value");
                  return false;
                }
              col.defaultValue += static_cast<char>(value);
            }
          size_t p6 = line.find('\t', p5 + 1);
          col.added = ((p6 == std::string::npos ? line.substr(p5 + 1) :
                        line.substr(p5 + 1, p6 - p5 - 1)) == "1");
          if (hasUpshiftMetadata && p6 != std::string::npos)
            {
              size_t p7 = line.find('\t', p6 + 1);
              col.upshifted = (line.substr(p6 + 1, p7 == std::string::npos
                                            ? std::string::npos
                                            : p7 - p6 - 1) == "1");
              if (hasComputedMetadata && p7 != std::string::npos)
                {
                  size_t p8 = line.find('\t', p7 + 1);
                  col.division = (line.substr(p7 + 1, p8 == std::string::npos
                                               ? std::string::npos
                                               : p8 - p7 - 1) == "1");
                  if (p8 != std::string::npos)
                    {
                      std::string computedHex = line.substr(p8 + 1);
                      if (computedHex.size() % 2 != 0)
                        {
                          setError(error, "invalid local-lite computed column metadata");
                          return false;
                        }
                      for (size_t j = 0; j < computedHex.size(); j += 2)
                        {
                          char pair[3] = { computedHex[j], computedHex[j + 1], 0 };
                          char *end = NULL;
                          unsigned long value = strtoul(pair, &end, 16);
                          if (!end || *end)
                            {
                              setError(error, "invalid local-lite computed column metadata");
                              return false;
                            }
                          col.computedText += static_cast<char>(value);
                        }
                    }
                }
            }
        }
      table->columns.push_back(col);
    }
  if (hasKeyMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite primary key metadata");
          return false;
        }
      unsigned long keyCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < keyCount; i++)
        {
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite primary key metadata");
              return false;
            }
          unsigned long keyIndex = strtoul(line.c_str(), NULL, 10);
          if (keyIndex >= table->columns.size())
            {
              setError(error, "invalid local-lite primary key metadata");
              return false;
            }
          table->primaryKeyColumns.push_back(static_cast<size_t>(keyIndex));
        }
    }
  if (hasStoreByMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite STORE BY metadata");
          return false;
        }
      unsigned long storeByCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < storeByCount; i++)
        {
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite STORE BY metadata");
              return false;
            }
          unsigned long keyIndex = strtoul(line.c_str(), NULL, 10);
          if (keyIndex >= table->columns.size())
            {
              setError(error, "invalid local-lite STORE BY metadata");
              return false;
            }
          table->storeByColumns.push_back(static_cast<size_t>(keyIndex));
        }
    }
  if (hasUniqueMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite unique key metadata");
          return false;
        }
      unsigned long uniqueCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < uniqueCount; i++)
        {
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite unique key metadata");
              return false;
            }
          unsigned long keyColumnCount = strtoul(line.c_str(), NULL, 10);
          std::vector<size_t> keyColumns;
          for (unsigned long j = 0; j < keyColumnCount; j++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite unique key metadata");
                  return false;
                }
              unsigned long keyIndex = strtoul(line.c_str(), NULL, 10);
              if (keyIndex >= table->columns.size())
                {
                  setError(error, "invalid local-lite unique key metadata");
                  return false;
                }
              keyColumns.push_back(static_cast<size_t>(keyIndex));
            }
          if (keyColumns.empty())
            {
              setError(error, "invalid local-lite unique key metadata");
              return false;
            }
          table->uniqueKeyColumns.push_back(keyColumns);
        }
    }
  if (hasIndexMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite index metadata");
          return false;
        }
      unsigned long indexCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < indexCount; i++)
        {
          LocalLiteIndexDef index;
          if (!nextLine(encoded, &pos, &index.name) ||
              !nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite index metadata");
              return false;
            }
          index.objectUid = strtoull(line.c_str(), NULL, 10);
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite index metadata");
              return false;
            }
          index.unique = (line == "1");
          if (hasIndexEncodingMetadata)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite index metadata");
                  return false;
                }
              index.keyEncodingVersion =
                  static_cast<uint32_t>(strtoul(line.c_str(), NULL, 10));
              if (index.keyEncodingVersion == 0 ||
                  index.keyEncodingVersion > 4)
                {
                  setError(error, "invalid local-lite index key encoding");
                  return false;
                }
            }
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite index metadata");
              return false;
            }
          unsigned long keyCount = strtoul(line.c_str(), NULL, 10);
          if (index.name.empty() || index.objectUid == 0 || keyCount == 0)
            {
              setError(error, "invalid local-lite index metadata");
              return false;
            }
          for (unsigned long j = 0; j < keyCount; j++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite index metadata");
                  return false;
                }
              size_t tab = line.find('\t');
              if (tab == std::string::npos)
                {
                  setError(error, "invalid local-lite index metadata");
                  return false;
                }
              unsigned long column = strtoul(line.substr(0, tab).c_str(),
                                             NULL, 10);
              if (column >= table->columns.size())
                {
                  setError(error, "invalid local-lite index metadata");
                  return false;
                }
              index.keyColumns.push_back(static_cast<size_t>(column));
              index.descending.push_back(line.substr(tab + 1) == "1");
            }
          table->secondaryIndexes.push_back(index);
        }
    }
  if (hasCheckMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite check metadata");
          return false;
        }
      unsigned long checkCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < checkCount; i++)
        {
          LocalLiteCheckDef check;
          std::string *fields[2] = { &check.name, &check.expression };
          for (size_t f = 0; f < 2; f++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite check metadata");
                  return false;
                }
              unsigned long length = strtoul(line.c_str(), NULL, 10);
              if (pos + length >= encoded.size() || encoded[pos + length] != '\n')
                {
                  setError(error, "invalid local-lite check metadata");
                  return false;
                }
              fields[f]->assign(encoded, pos, length);
              pos += length + 1;
            }
          table->checkConstraints.push_back(check);
        }
    }
  if (hasViewMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite view metadata");
          return false;
        }
      table->view = (line == "1");
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite view metadata");
          return false;
        }
      unsigned long length = strtoul(line.c_str(), NULL, 10);
      if (pos + length >= encoded.size() || encoded[pos + length] != '\n')
        {
          setError(error, "invalid local-lite view metadata");
          return false;
        }
      table->viewText.assign(encoded, pos, length);
      pos += length + 1;
      if (table->view != !table->viewText.empty())
        {
          setError(error, "invalid local-lite view definition");
          return false;
        }
      if (hasViewConstraintMetadata)
        {
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite view constraint metadata");
              return false;
            }
          unsigned long checkLength = strtoul(line.c_str(), NULL, 10);
          if (pos + checkLength >= encoded.size() ||
              encoded[pos + checkLength] != '\n')
            {
              setError(error, "invalid local-lite view constraint metadata");
              return false;
            }
          table->viewCheckText.assign(encoded, pos, checkLength);
          pos += checkLength + 1;
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite view constraint metadata");
              return false;
            }
          table->viewUpdatable = (line == "1");
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite view constraint metadata");
              return false;
            }
          table->viewInsertable = (line == "1");
        }
    }
  if (hasRIMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite RI metadata");
          return false;
        }
      unsigned long riCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < riCount; i++)
        {
          LocalLiteRIDef ri;
          std::string *fields[5] = { &ri.name, &ri.referencedCatalog,
                                     &ri.referencedSchema,
                                     &ri.referencedTable,
                                     &ri.referencedConstraint };
          for (size_t f = 0; f < 5; f++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite RI metadata");
                  return false;
                }
              unsigned long length = strtoul(line.c_str(), NULL, 10);
              if (pos + length >= encoded.size() ||
                  encoded[pos + length] != '\n')
                {
                  setError(error, "invalid local-lite RI metadata");
                  return false;
                }
              fields[f]->assign(encoded, pos, length);
              pos += length + 1;
            }
          if (!nextLine(encoded, &pos, &line))
            {
              setError(error, "truncated local-lite RI column metadata");
              return false;
            }
          unsigned long columnCount = strtoul(line.c_str(), NULL, 10);
          for (unsigned long j = 0; j < columnCount; j++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite RI column metadata");
                  return false;
                }
              size_t tab = line.find('\t');
              if (tab == std::string::npos)
                {
                  setError(error, "invalid local-lite RI column metadata");
                  return false;
                }
              size_t child = strtoul(line.substr(0, tab).c_str(), NULL, 10);
              size_t parent = strtoul(line.substr(tab + 1).c_str(), NULL, 10);
              if (child >= table->columns.size())
                {
                  setError(error, "invalid local-lite RI child column");
                  return false;
                }
              ri.referencingColumns.push_back(child);
              ri.referencedColumns.push_back(parent);
            }
          if (ri.name.empty() || ri.referencedCatalog.empty() ||
              ri.referencedSchema.empty() || ri.referencedTable.empty() ||
              ri.referencedConstraint.empty() ||
              ri.referencingColumns.empty())
            {
              setError(error, "invalid local-lite RI metadata");
              return false;
            }
          table->riConstraints.push_back(ri);
        }
    }
  if (hasDependencyMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite dependency metadata");
          return false;
        }
      unsigned long dependencyCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < dependencyCount; i++)
        {
          LocalLiteObjectRef dependency;
          std::string *fields[3] = { &dependency.catalog,
                                     &dependency.schema,
                                     &dependency.name };
          for (size_t f = 0; f < 3; f++)
            {
              if (!nextLine(encoded, &pos, &line))
                {
                  setError(error, "truncated local-lite dependency metadata");
                  return false;
                }
              unsigned long length = strtoul(line.c_str(), NULL, 10);
              if (pos + length >= encoded.size() ||
                  encoded[pos + length] != '\n')
                {
                  setError(error, "invalid local-lite dependency metadata");
                  return false;
                }
              fields[f]->assign(encoded, pos, length);
              pos += length + 1;
            }
          if (dependency.catalog.empty() || dependency.schema.empty() ||
              dependency.name.empty())
            {
              setError(error, "invalid local-lite dependency metadata");
              return false;
            }
          table->dependencies.push_back(dependency);
        }
    }
  if (hasConstraintNameMetadata)
    {
      if (!nextLine(encoded, &pos, &line))
        {
          setError(error, "truncated local-lite key constraint metadata");
          return false;
        }
      unsigned long length = strtoul(line.c_str(), NULL, 10);
      if (pos + length >= encoded.size() || encoded[pos + length] != '\n')
        {
          setError(error, "invalid local-lite primary key name metadata");
          return false;
        }
      table->primaryKeyName.assign(encoded, pos, length);
      pos += length + 1;
      if (!nextLine(encoded, &pos, &line))
        return false;
      unsigned long uniqueCount = strtoul(line.c_str(), NULL, 10);
      for (unsigned long i = 0; i < uniqueCount; i++)
        {
          if (!nextLine(encoded, &pos, &line)) return false;
          length = strtoul(line.c_str(), NULL, 10);
          if (pos + length >= encoded.size() || encoded[pos + length] != '\n')
            {
              setError(error, "invalid local-lite unique key name metadata");
              return false;
            }
          table->uniqueKeyNames.push_back(encoded.substr(pos, length));
          pos += length + 1;
        }
      if (hasNoSyskeyMetadata)
        {
          if (!nextLine(encoded, &pos, &line)) return false;
          table->noSyskey = (line == "1");
        }
      if ((!table->primaryKeyColumns.empty()) != !table->primaryKeyName.empty() ||
          table->uniqueKeyNames.size() != table->uniqueKeyColumns.size())
        {
          setError(error, "inconsistent local-lite key constraint metadata");
          return false;
        }
    }
  else
    {
      const std::string prefix = table->catalog + "." + table->schema + "." +
                                 table->name;
      if (!table->primaryKeyColumns.empty()) table->primaryKeyName = prefix + "_PK";
      for (size_t i = 0; i < table->uniqueKeyColumns.size(); i++)
        table->uniqueKeyNames.push_back(
            prefix + "_UK_" + std::to_string(i + 1));
    }
  return true;
}

static std::string localLiteMetadataPrefix(const char *table)
{
  return std::string("md|") + table + "|";
}

static std::string localLiteMetadataUid(uint64_t uid)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%020llu",
           static_cast<unsigned long long>(uid));
  return buf;
}

static std::string localLiteMetadataObjectKey(
    const std::string &catalog, const std::string &schema,
    const std::string &name, const char *objectType)
{
  return localLiteMetadataPrefix("OBJECTS") + localLiteHexKey(catalog) + "|" +
         localLiteHexKey(schema) + "|" + localLiteHexKey(name) + "|" +
         objectType;
}

static std::string localLiteMetadataTableKey(uint64_t uid)
{
  return localLiteMetadataPrefix("TABLES") + localLiteMetadataUid(uid);
}

static std::string localLiteMetadataColumnKey(uint64_t uid, size_t ordinal)
{
  return localLiteMetadataPrefix("COLUMNS") + "v2|" +
         localLiteMetadataUid(uid) + "|" +
         localLiteMetadataUid(static_cast<uint64_t>(ordinal));
}

static std::string localLiteMetadataKeyKey(uint64_t uid,
                                           const std::string &keyId,
                                           size_t sequence)
{
  return localLiteMetadataPrefix("KEYS") + "v2|" +
         localLiteMetadataUid(uid) + "|" + localLiteHexKey(keyId) + "|" +
         localLiteMetadataUid(static_cast<uint64_t>(sequence));
}

static void appendLocalLiteMetadataField(std::string &out,
                                         const std::string &field)
{
  appendUint64(out, static_cast<uint64_t>(field.size()));
  out.append(field);
}

static std::string encodeLocalLiteMetadataRow(
    const std::vector<std::string> &fields)
{
  std::string out("LLMD1");
  appendUint64(out, static_cast<uint64_t>(fields.size()));
  for (size_t i = 0; i < fields.size(); i++)
    appendLocalLiteMetadataField(out, fields[i]);
  return out;
}

static void putLocalLiteMetadataRecord(
    rocksdb_writebatch_t *batch, const std::string &key,
    const std::vector<std::string> &fields)
{
  std::string value = encodeLocalLiteMetadataRow(fields);
  rocksdb_writebatch_put(batch, key.data(), key.size(), value.data(),
                         value.size());
}

static void deleteLocalLiteMetadataRecord(rocksdb_writebatch_t *batch,
                                           const std::string &key)
{
  rocksdb_writebatch_delete(batch, key.data(), key.size());
}

static void addLocalLiteMetadataForTable(rocksdb_writebatch_t *batch,
                                         const LocalLiteTableDef &table)
{
  const char *tableType = table.view ? "VIEW" : "TABLE";
  putLocalLiteMetadataRecord(
      batch,
      localLiteMetadataObjectKey(table.catalog, table.schema, table.name,
                                 tableType),
      std::vector<std::string>{table.catalog, table.schema, table.name,
                               tableType,
                               localLiteMetadataUid(table.objectUid)});
  putLocalLiteMetadataRecord(
      batch, localLiteMetadataTableKey(table.objectUid),
      std::vector<std::string>{localLiteMetadataUid(table.objectUid),
                               table.view ? "1" : "0",
                               std::to_string(table.columns.size()),
                               std::to_string(table.primaryKeyColumns.size()),
                               std::to_string(table.uniqueKeyColumns.size()),
                               std::to_string(table.secondaryIndexes.size()),
                               table.primaryKeyName});

  for (size_t i = 0; i < table.columns.size(); i++)
    {
      const LocalLiteColumnDef &column = table.columns[i];
      putLocalLiteMetadataRecord(
          batch, localLiteMetadataColumnKey(table.objectUid, i),
          std::vector<std::string>{localLiteMetadataUid(table.objectUid),
                                   std::to_string(i), column.name, column.type,
                                   column.nullable ? "1" : "0",
                                   std::to_string(column.defaultClass),
                                   column.defaultValue});
    }

  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    putLocalLiteMetadataRecord(
        batch, localLiteMetadataKeyKey(table.objectUid, "P", i),
        std::vector<std::string>{localLiteMetadataUid(table.objectUid), "P",
                                 table.primaryKeyName, std::to_string(i),
                                 std::to_string(table.primaryKeyColumns[i]),
                                 "0"});
  for (size_t key = 0; key < table.uniqueKeyColumns.size(); key++)
    for (size_t i = 0; i < table.uniqueKeyColumns[key].size(); i++)
      putLocalLiteMetadataRecord(
          batch, localLiteMetadataKeyKey(table.objectUid,
                                         "U" + std::to_string(key), i),
          std::vector<std::string>{localLiteMetadataUid(table.objectUid),
                                   "U", table.uniqueKeyNames[key],
                                   std::to_string(i),
                                   std::to_string(table.uniqueKeyColumns[key][i]),
                                   "0"});
  for (size_t index = 0; index < table.secondaryIndexes.size(); index++)
    {
      const LocalLiteIndexDef &idx = table.secondaryIndexes[index];
      putLocalLiteMetadataRecord(
          batch,
          localLiteMetadataObjectKey(table.catalog, table.schema, idx.name,
                                     "INDEX"),
          std::vector<std::string>{table.catalog, table.schema, idx.name,
                                   "INDEX",
                                   localLiteMetadataUid(idx.objectUid)});
      for (size_t i = 0; i < idx.keyColumns.size(); i++)
        putLocalLiteMetadataRecord(
            batch, localLiteMetadataKeyKey(table.objectUid,
                                           "S" + localLiteHexKey(idx.name), i),
            std::vector<std::string>{localLiteMetadataUid(table.objectUid),
                                     "S", idx.name, std::to_string(i),
                                     std::to_string(idx.keyColumns[i]),
                                     (i < idx.descending.size() &&
                                      idx.descending[i]) ? "1" : "0"});
    }
}

static void deleteLocalLiteMetadataForTable(rocksdb_writebatch_t *batch,
                                            const LocalLiteTableDef &table)
{
  deleteLocalLiteMetadataRecord(
      batch, localLiteMetadataObjectKey(table.catalog, table.schema,
                                        table.name,
                                        table.view ? "VIEW" : "TABLE"));
  deleteLocalLiteMetadataRecord(batch,
                                localLiteMetadataTableKey(table.objectUid));
  for (size_t i = 0; i < table.columns.size(); i++)
    deleteLocalLiteMetadataRecord(
        batch, localLiteMetadataColumnKey(table.objectUid, i));
  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    deleteLocalLiteMetadataRecord(
        batch, localLiteMetadataKeyKey(table.objectUid, "P", i));
  for (size_t key = 0; key < table.uniqueKeyColumns.size(); key++)
    for (size_t i = 0; i < table.uniqueKeyColumns[key].size(); i++)
      deleteLocalLiteMetadataRecord(
          batch, localLiteMetadataKeyKey(table.objectUid,
                                         "U" + std::to_string(key), i));
  for (size_t index = 0; index < table.secondaryIndexes.size(); index++)
    {
      const LocalLiteIndexDef &idx = table.secondaryIndexes[index];
      deleteLocalLiteMetadataRecord(
          batch, localLiteMetadataObjectKey(table.catalog, table.schema,
                                            idx.name, "INDEX"));
      for (size_t i = 0; i < idx.keyColumns.size(); i++)
        deleteLocalLiteMetadataRecord(
            batch, localLiteMetadataKeyKey(table.objectUid,
                                           "S" + localLiteHexKey(idx.name), i));
    }
}

static const char *LOCAL_LITE_METADATA_KEY_FORMAT =
    "format|metadata-key";

static bool collectLocalLiteMetadataV2Keys(
    const LocalLiteTableDef &table,
    std::set<std::string> *keys,
    std::string *error)
{
  for (size_t i = 0; i < table.columns.size(); i++)
    if (!keys->insert(
            localLiteMetadataColumnKey(table.objectUid, i)).second)
      {
        setError(error, "local-lite metadata column-key collision");
        return false;
      }
  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    if (!keys->insert(
            localLiteMetadataKeyKey(table.objectUid, "P", i)).second)
      {
        setError(error, "local-lite metadata primary-key collision");
        return false;
      }
  for (size_t key = 0; key < table.uniqueKeyColumns.size(); key++)
    for (size_t i = 0; i < table.uniqueKeyColumns[key].size(); i++)
      if (!keys->insert(localLiteMetadataKeyKey(
              table.objectUid, "U" + std::to_string(key), i)).second)
        {
          setError(error, "local-lite metadata unique-key collision");
          return false;
        }
  for (size_t index = 0; index < table.secondaryIndexes.size(); index++)
    for (size_t i = 0;
         i < table.secondaryIndexes[index].keyColumns.size(); i++)
      if (!keys->insert(localLiteMetadataKeyKey(
              table.objectUid,
              "S" + localLiteHexKey(table.secondaryIndexes[index].name),
              i)).second)
        {
          setError(error, "local-lite metadata index-key collision");
          return false;
        }
  return true;
}

static bool migrateLocalLiteMetadataKeys(rocksdb_t *catalogDb,
                                         std::string *error)
{
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *rocksError = NULL;
  size_t valueLength = 0;
  char *rawFormat = rocksdb_get(
      catalogDb, readOptions, LOCAL_LITE_METADATA_KEY_FORMAT,
      strlen(LOCAL_LITE_METADATA_KEY_FORMAT), &valueLength, &rocksError);
  if (!checkRocksError(rocksError,
                       "read local-lite metadata-key format", error))
    {
      rocksdb_readoptions_destroy(readOptions);
      return false;
    }
  if (rawFormat)
    {
      const std::string format(rawFormat, valueLength);
      rocksdb_free(rawFormat);
      rocksdb_readoptions_destroy(readOptions);
      if (format == "2")
        return true;
      setError(error, "unsupported local-lite metadata-key format " + format);
      return false;
    }

  std::vector<LocalLiteTableDef> tables;
  rocksdb_iterator_t *iterator =
      rocksdb_create_iterator(catalogDb, readOptions);
  const std::string tablePrefix("table|");
  for (rocksdb_iter_seek(iterator, tablePrefix.data(), tablePrefix.size());
       rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator))
    {
      size_t keyLength = 0;
      size_t tableLength = 0;
      const char *rawKey = rocksdb_iter_key(iterator, &keyLength);
      const std::string key(rawKey, keyLength);
      if (key.compare(0, tablePrefix.size(), tablePrefix) != 0)
        break;
      const char *rawTable = rocksdb_iter_value(iterator, &tableLength);
      LocalLiteTableDef table;
      if (!decodeTable(std::string(rawTable, tableLength), &table, error))
        {
          rocksdb_iter_destroy(iterator);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
      tables.push_back(table);
    }
  rocksError = NULL;
  rocksdb_iter_get_error(iterator, &rocksError);
  rocksdb_iter_destroy(iterator);
  if (!checkRocksError(rocksError,
                       "scan tables for metadata-key migration", error))
    {
      rocksdb_readoptions_destroy(readOptions);
      return false;
    }

  std::set<std::string> expectedKeys;
  for (size_t i = 0; i < tables.size(); i++)
    if (!collectLocalLiteMetadataV2Keys(tables[i], &expectedKeys, error))
      {
        rocksdb_readoptions_destroy(readOptions);
        return false;
      }

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  const char *prefixes[] = {"md|COLUMNS|", "md|KEYS|"};
  for (size_t prefixIndex = 0; prefixIndex < 2; prefixIndex++)
    {
      const std::string prefix(prefixes[prefixIndex]);
      iterator = rocksdb_create_iterator(catalogDb, readOptions);
      for (rocksdb_iter_seek(iterator, prefix.data(), prefix.size());
           rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator))
        {
          size_t keyLength = 0;
          const char *rawKey = rocksdb_iter_key(iterator, &keyLength);
          const std::string key(rawKey, keyLength);
          if (key.compare(0, prefix.size(), prefix) != 0)
            break;
          rocksdb_writebatch_delete(batch, key.data(), key.size());
        }
      rocksError = NULL;
      rocksdb_iter_get_error(iterator, &rocksError);
      rocksdb_iter_destroy(iterator);
      if (!checkRocksError(rocksError,
                           "scan legacy metadata keys", error))
        {
          rocksdb_writebatch_destroy(batch);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
    }
  rocksdb_readoptions_destroy(readOptions);

  for (size_t i = 0; i < tables.size(); i++)
    addLocalLiteMetadataForTable(batch, tables[i]);
  rocksdb_writebatch_put(batch, LOCAL_LITE_METADATA_KEY_FORMAT,
                         strlen(LOCAL_LITE_METADATA_KEY_FORMAT), "2", 1);

  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(writeOptions, 1);
  rocksError = NULL;
  rocksdb_write(catalogDb, writeOptions, batch, &rocksError);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(rocksError,
                       "migrate local-lite metadata keys", error))
    return false;

  readOptions = rocksdb_readoptions_create();
  size_t migratedKeyCount = 0;
  for (size_t prefixIndex = 0; prefixIndex < 2; prefixIndex++)
    {
      const std::string prefix =
          std::string(prefixes[prefixIndex]) + "v2|";
      iterator = rocksdb_create_iterator(catalogDb, readOptions);
      for (rocksdb_iter_seek(iterator, prefix.data(), prefix.size());
           rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator))
        {
          size_t keyLength = 0;
          const char *rawKey = rocksdb_iter_key(iterator, &keyLength);
          const std::string key(rawKey, keyLength);
          if (key.compare(0, prefix.size(), prefix) != 0)
            break;
          if (expectedKeys.find(key) == expectedKeys.end())
            {
              rocksdb_iter_destroy(iterator);
              rocksdb_readoptions_destroy(readOptions);
              setError(error,
                       "unexpected key after metadata-key migration");
              return false;
            }
          migratedKeyCount++;
        }
      rocksdb_iter_destroy(iterator);
    }
  rocksdb_readoptions_destroy(readOptions);
  if (migratedKeyCount != expectedKeys.size())
    {
      setError(error, "incomplete local-lite metadata-key migration");
      return false;
    }
  return true;
}

static bool isLocalLiteMetadataTable(const std::string &catalog,
                                     const std::string &schema,
                                     const std::string &name)
{
  if (catalog != "TRAFODION" || schema != "_MD_")
    return false;
  return name == "OBJECTS" || name == "TABLES" || name == "COLUMNS" ||
         name == "KEYS" || name == "INDEXES" ||
         name == "SEQUENCES_VIEW" || name == "TEXT";
}

static void addLocalLiteMetadataColumn(LocalLiteTableDef *table,
                                       const char *name,
                                       const char *type)
{
  LocalLiteColumnDef column;
  column.name = name;
  column.type = type;
  column.nullable = false;
  table->columns.push_back(column);
}

static bool localLiteMetadataTableDefinition(const std::string &catalog,
                                             const std::string &schema,
                                             const std::string &name,
                                             LocalLiteTableDef *table,
                                             std::string *error)
{
  if (!table || !isLocalLiteMetadataTable(catalog, schema, name))
    return false;
  // A scan TCB can be reused for multiple requests.  Rebuild the virtual
  // catalog descriptor from scratch instead of appending columns to the
  // descriptor left by the previous request.
  table->columns.clear();
  table->primaryKeyColumns.clear();
  table->storeByColumns.clear();
  table->uniqueKeyColumns.clear();
  table->uniqueKeyNames.clear();
  table->secondaryIndexes.clear();
  table->catalog = catalog;
  table->schema = schema;
  table->name = name;
  table->objectUid = 1000000000000ULL;
  if (name == "OBJECTS")
    {
      table->objectUid++;
      addLocalLiteMetadataColumn(table, "CATALOG_NAME", "VARCHAR(256)");
      addLocalLiteMetadataColumn(table, "SCHEMA_NAME", "VARCHAR(256)");
      addLocalLiteMetadataColumn(table, "OBJECT_NAME", "VARCHAR(256)");
      addLocalLiteMetadataColumn(table, "OBJECT_TYPE", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "OBJECT_UID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "CREATE_TIME", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "REDEF_TIME", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "VALID_DEF", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "DROPPABLE", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "OBJECT_OWNER", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "SCHEMA_OWNER", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "FLAGS", "VARCHAR(32)");
      table->primaryKeyColumns = {0, 1, 2, 3};
    }
  else if (name == "TABLES")
    {
      table->objectUid++;
      addLocalLiteMetadataColumn(table, "TABLE_UID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "ROW_FORMAT", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "IS_AUDITED", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "ROW_DATA_LENGTH", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "ROW_TOTAL_LENGTH", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "KEY_LENGTH", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "NUM_SALT_PARTNS", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "FLAGS", "VARCHAR(32)");
      table->primaryKeyColumns = {0};
    }
  else if (name == "COLUMNS")
    {
      table->objectUid++;
      const char *columns[][2] = {
        {"OBJECT_UID", "VARCHAR(32)"}, {"COLUMN_NAME", "VARCHAR(256)"},
        {"COLUMN_NUMBER", "VARCHAR(32)"}, {"COLUMN_CLASS", "VARCHAR(32)"},
        {"FS_DATA_TYPE", "VARCHAR(32)"}, {"SQL_DATA_TYPE", "VARCHAR(32)"},
        {"COLUMN_SIZE", "VARCHAR(32)"}, {"COLUMN_PRECISION", "VARCHAR(32)"},
        {"COLUMN_SCALE", "VARCHAR(32)"}, {"DATETIME_START_FIELD", "VARCHAR(32)"},
        {"DATETIME_END_FIELD", "VARCHAR(32)"}, {"IS_UPSHIFTED", "VARCHAR(32)"},
        {"COLUMN_FLAGS", "VARCHAR(32)"}, {"NULLABLE", "VARCHAR(32)"},
        {"CHARACTER_SET", "VARCHAR(40)"}, {"DEFAULT_CLASS", "VARCHAR(32)"},
        {"DEFAULT_VALUE", "VARCHAR(1024)"},
        {"COLUMN_HEADING", "VARCHAR(256)"},
        {"HBASE_COL_FAMILY", "VARCHAR(40)"},
        {"HBASE_COL_QUALIFIER", "VARCHAR(40)"},
        {"DIRECTION", "VARCHAR(32)"}, {"IS_OPTIONAL", "VARCHAR(32)"},
        {"FLAGS", "VARCHAR(32)"}
      };
      for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++)
        addLocalLiteMetadataColumn(table, columns[i][0], columns[i][1]);
      table->primaryKeyColumns = {0, 1};
    }
  else if (name == "KEYS")
    {
      table->objectUid++;
      addLocalLiteMetadataColumn(table, "OBJECT_UID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "COLUMN_NAME", "VARCHAR(256)");
      addLocalLiteMetadataColumn(table, "KEYSEQ_NUMBER", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "COLUMN_NUMBER", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "ORDERING", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "NONKEYCOL", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "FLAGS", "VARCHAR(32)");
      table->primaryKeyColumns = {0, 2};
    }
  else if (name == "INDEXES")
    {
      table->objectUid++;
      addLocalLiteMetadataColumn(table, "BASE_TABLE_UID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "KEYTAG", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "IS_UNIQUE", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "KEY_COLCOUNT", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "NONKEY_COLCOUNT", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "IS_EXPLICIT", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "INDEX_UID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "FLAGS", "VARCHAR(32)");
      table->primaryKeyColumns = {0, 6};
    }
  else if (name == "SEQUENCES_VIEW")
    {
      const char *columns[][2] = {
        {"CATALOG_NAME", "VARCHAR(256)"},
        {"SCHEMA_NAME", "VARCHAR(256)"},
        {"SEQ_NAME", "VARCHAR(256)"},
        {"START_VALUE", "VARCHAR(32)"},
        {"INCREMENT", "VARCHAR(32)"},
        {"MAX_VALUE", "VARCHAR(32)"},
        {"MIN_VALUE", "VARCHAR(32)"},
        {"CYCLE_OPTION", "VARCHAR(32)"},
        {"CACHE_OPTION", "VARCHAR(32)"},
        {"CACHE_SIZE", "VARCHAR(32)"},
        {"NEXT_VALUE", "VARCHAR(32)"},
        {"NUM_CALLS", "VARCHAR(32)"}
      };
      for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++)
        addLocalLiteMetadataColumn(table, columns[i][0], columns[i][1]);
    }
  else if (name == "TEXT")
    {
      addLocalLiteMetadataColumn(table, "TEXT_UID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "TEXT_TYPE", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "SUB_ID", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "SEQ_NUM", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "FLAGS", "VARCHAR(32)");
      addLocalLiteMetadataColumn(table, "TEXT", "VARCHAR(10000)");
      table->primaryKeyColumns = {0, 1, 2, 3};
    }
  table->primaryKeyName = catalog + "." + schema + "." + name + "_PK";
  // Metadata rows are exposed through a generated full-scan descriptor.  The
  // physical RocksDB row key is the synthetic row id, so do not advertise the
  // catalog key as an optimizer clustering key.
  table->primaryKeyColumns.clear();
  return true;
}

static bool appendLocalLiteMetadataRow(const LocalLiteTableDef &table,
                                       const std::vector<std::string> &fields,
                                       uint64_t rowId,
                                       std::vector<LocalLiteRow> *rows,
                                       std::string *error)
{
  LocalLiteRow row;
  row.rowId = rowId;
  if (!LocalLiteEncodeBinaryRow(table, fields, &row.value, error))
    return false;
  rows->push_back(row);
  return true;
}

static std::string localLiteMetadataCharset(const std::string &type)
{
  if (type.find("UCS2") != std::string::npos) return "UCS2";
  if (type.find("UTF8") != std::string::npos) return "UTF8";
  return "ISO88591";
}

static std::string localLiteMetadataColumnSize(const std::string &type)
{
  std::string upper = type;
  for (size_t i = 0; i < upper.size(); i++)
    upper[i] = static_cast<char>(toupper(static_cast<unsigned char>(upper[i])));
  if (upper.find("CHAR(") == 0 || upper.find("VARCHAR(") == 0)
    {
      size_t begin = upper.find('(') + 1;
      size_t end = upper.find_first_of(",)", begin);
      return upper.substr(begin, end == std::string::npos
                                   ? std::string::npos : end - begin);
    }
  if (upper.find("TINYINT") == 0 || upper.find("BOOLEAN") == 0) return "1";
  if (upper.find("SMALLINT") == 0) return "2";
  if (upper.find("INT") == 0 || upper.find("INTEGER") == 0 ||
      upper.find("DATE") == 0) return "4";
  if (upper.find("LARGEINT") == 0 || upper.find("BIGINT") == 0 ||
      upper.find("DOUBLE") == 0 || upper.find("NUMERIC") == 0)
    return "8";
  if (upper.find("REAL") == 0) return "4";
  return "0";
}

static bool localLiteBuildMetadataRows(
    const LocalLiteTableDef &metadataTable,
    const std::vector<LocalLiteTableDef> &tables,
    std::vector<LocalLiteRow> *rows,
    std::string *error)
{
  uint64_t rowId = 1;
  rows->clear();
  if (metadataTable.name == "SEQUENCES_VIEW")
    {
      LocalLiteRocksDBStore store;
      std::vector<LocalLiteSequenceDef> sequences;
      if (!store.listSequences("", "", &sequences, error))
        return false;
      for (size_t i = 0; i < sequences.size(); i++)
        {
          const LocalLiteSequenceDef &sequence = sequences[i];
          if (sequence.internal || sequence.schema == "_MD_")
            continue;
          if (!appendLocalLiteMetadataRow(
                  metadataTable,
                  {sequence.catalog, sequence.schema, sequence.name,
                   std::to_string(sequence.startValue),
                   std::to_string(sequence.increment),
                   std::to_string(sequence.maxValue),
                   std::to_string(sequence.minValue),
                   sequence.cycle ? "Y" : "N",
                   sequence.cache == 0 ? "N" : "Y",
                   std::to_string(sequence.cache),
                   std::to_string(sequence.nextValue),
                   std::to_string(sequence.numCalls)},
                  rowId++, rows, error))
            return false;
        }
      return true;
    }
  for (size_t t = 0; t < tables.size(); t++)
    {
      const LocalLiteTableDef &table = tables[t];
      if (metadataTable.name == "OBJECTS")
        {
          const char *type = table.view ? "VI" : "BT";
          if (!appendLocalLiteMetadataRow(
                  metadataTable,
                  {table.catalog, table.schema, table.name, type,
                   std::to_string(table.objectUid), "0", "0", "Y", "Y",
                   "0", "0", "0"}, rowId++, rows, error)) return false;
          for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
            {
              const LocalLiteIndexDef &index = table.secondaryIndexes[i];
              if (!appendLocalLiteMetadataRow(
                      metadataTable,
                      {table.catalog, table.schema, index.name, "IX",
                       std::to_string(index.objectUid), "0", "0", "Y", "Y",
                       "0", "0", "0"}, rowId++, rows, error)) return false;
            }
        }
      else if (metadataTable.name == "TABLES")
        {
          if (!appendLocalLiteMetadataRow(
                  metadataTable,
                  {std::to_string(table.objectUid), "AL", "Y", "0", "0",
                   "0", "0", "0"}, rowId++, rows, error)) return false;
        }
      else if (metadataTable.name == "COLUMNS")
        {
          size_t columnOffset = (!table.view && !table.noSyskey &&
                                 table.primaryKeyColumns.empty()) ? 1 : 0;
          if (columnOffset > 0)
            {
              std::vector<std::string> fields = {
                std::to_string(table.objectUid), "SYSKEY", "0", "S", "0",
                "LARGEINT", "8", "0", "0", "0", "0", "N", "0", "0",
                "ISO88591", "1", " ", " ", "#1", "0", "NA", "N", "0"};
              if (!appendLocalLiteMetadataRow(metadataTable, fields, rowId++,
                                              rows, error)) return false;
            }
          for (size_t i = 0; i < table.columns.size(); i++)
            {
              const LocalLiteColumnDef &column = table.columns[i];
              std::string qualifier = std::to_string(i + 1);
              std::vector<std::string> fields = {
                std::to_string(table.objectUid), column.name,
                std::to_string(i + columnOffset), column.division ? "S" :
                  (column.added ? "A" : "U"), "0", column.type,
                localLiteMetadataColumnSize(column.type), "0", "0", "0",
                "0", column.upshifted ? "Y" : "N", "0",
                column.nullable ? "1" : "0",
                localLiteMetadataCharset(column.type),
                std::to_string(column.defaultClass),
                column.defaultValue.empty() ? " " : column.defaultValue, " ",
                "#1", qualifier, "NA", "N", "0"};
              if (!appendLocalLiteMetadataRow(metadataTable, fields, rowId++,
                                              rows, error)) return false;
            }
        }
      else if (metadataTable.name == "KEYS")
        {
          size_t keySequence = 1;
          const size_t columnOffset = (!table.view && !table.noSyskey &&
                                       table.primaryKeyColumns.empty()) ? 1 : 0;
          bool hasDivisionKey = false;
          if (table.primaryKeyColumns.empty())
            for (size_t i = 0; i < table.columns.size(); i++)
              if (table.columns[i].division)
                {
                  hasDivisionKey = true;
                  if (!appendLocalLiteMetadataRow(
                          metadataTable,
                          {std::to_string(table.objectUid),
                           table.columns[i].name,
                           std::to_string(keySequence++),
                           std::to_string(i + columnOffset), "0", "0", "0"},
                          rowId++, rows, error)) return false;
                }
          if (!table.view && !table.noSyskey &&
              table.primaryKeyColumns.empty() && !hasDivisionKey)
            {
              if (!appendLocalLiteMetadataRow(
                      metadataTable,
                      {std::to_string(table.objectUid), "SYSKEY", "1", "0",
                       "0", "0", "0"}, rowId++, rows, error)) return false;
              keySequence++;
            }
          for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
            if (!appendLocalLiteMetadataRow(
                    metadataTable,
                    {std::to_string(table.objectUid),
                     table.columns[table.primaryKeyColumns[i]].name,
                     std::to_string(keySequence++),
                     std::to_string(table.primaryKeyColumns[i]), "0", "0",
                     "0"},
                    rowId++, rows, error)) return false;
          for (size_t i = 0; i < table.storeByColumns.size(); i++)
            if (!appendLocalLiteMetadataRow(
                    metadataTable,
                    {std::to_string(table.objectUid),
                     table.columns[table.storeByColumns[i]].name,
                     std::to_string(keySequence++),
                     std::to_string(table.storeByColumns[i] +
                                    columnOffset),
                     "0", "0", "0"}, rowId++, rows, error)) return false;
          if (!table.view && !table.noSyskey &&
              table.primaryKeyColumns.empty() && hasDivisionKey)
            if (!appendLocalLiteMetadataRow(
                    metadataTable,
                    {std::to_string(table.objectUid), "SYSKEY",
                     std::to_string(keySequence++), "0", "0", "0", "0"},
                    rowId++, rows, error)) return false;
          for (size_t key = 0; key < table.uniqueKeyColumns.size(); key++)
            for (size_t i = 0; i < table.uniqueKeyColumns[key].size(); i++)
              if (!appendLocalLiteMetadataRow(
                      metadataTable,
                      {std::to_string(table.objectUid),
                       table.columns[table.uniqueKeyColumns[key][i]].name,
                       std::to_string(i + 1),
                       std::to_string(table.uniqueKeyColumns[key][i]), "0", "0",
                       "0"}, rowId++, rows, error)) return false;
        }
      else if (metadataTable.name == "INDEXES")
        {
          for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
            {
              const LocalLiteIndexDef &index = table.secondaryIndexes[i];
              if (!appendLocalLiteMetadataRow(
                      metadataTable,
                      {std::to_string(table.objectUid), std::to_string(i + 1),
                       index.unique ? "1" : "0",
                       std::to_string(index.keyColumns.size()), "0", "1",
                      std::to_string(index.objectUid), "0"}, rowId++, rows,
                      error)) return false;
            }
        }
      else if (metadataTable.name == "TEXT")
        {
          for (size_t i = 0; i < table.columns.size(); i++)
            {
              const LocalLiteColumnDef &column = table.columns[i];
              if (!column.division || column.computedText.empty())
                continue;
              if (!appendLocalLiteMetadataRow(
                      metadataTable,
                      {std::to_string(table.objectUid), "4",
                       std::to_string(i + ((!table.view && !table.noSyskey &&
                                            table.primaryKeyColumns.empty())
                                           ? 1 : 0)),
                       "0", "0", column.computedText},
                      rowId++, rows, error)) return false;
            }
        }
    }
  return true;
}

static std::string encodeRowValue(const std::string &encodedRow)
{
  std::string out;
  out += static_cast<char>(LOCAL_LITE_ROW_FORMAT_VERSION);
  appendUint64(out, static_cast<uint64_t>(encodedRow.size()));
  out += encodedRow;
  return out;
}

static bool decodeRowValue(const std::string &value,
                           std::string *encodedRow,
                           std::string *error)
{
  if (value.size() < 9 ||
      static_cast<unsigned char>(value[0]) != LOCAL_LITE_ROW_FORMAT_VERSION)
    {
      setError(error, "invalid local-lite row format");
      return false;
    }
  size_t offset = 1;
  uint64_t len = readUint64(value, &offset);
  if (offset + len > value.size())
    {
      setError(error, "truncated local-lite row value");
      return false;
    }
  *encodedRow = value.substr(offset, static_cast<size_t>(len));
  return true;
}

struct LocalLitePhysicalIndexEntry
{
  std::string key;
  std::string rowKey;
  std::string value;
  bool unique;
};

static std::string encodeCoveringIndexValue(const std::string &rowKey,
                                             const std::string &encodedRow)
{
  std::string value("LLIV1", 5);
  appendUint64(value, static_cast<uint64_t>(rowKey.size()));
  value += rowKey;
  appendUint64(value, static_cast<uint64_t>(encodedRow.size()));
  value += encodedRow;
  return value;
}

static bool decodeIndexValue(const std::string &value,
                             std::string *rowKey,
                             std::string *encodedRow,
                             bool *covering,
                             std::string *error)
{
  *covering = value.size() >= 5 && value.compare(0, 5, "LLIV1") == 0;
  if (!*covering)
    {
      *rowKey = value;
      encodedRow->clear();
      return true;
    }
  size_t offset = 5;
  if (offset + 8 > value.size())
    {
      setError(error, "truncated local-lite covering index row key");
      return false;
    }
  uint64_t rowKeyLen = readUint64(value, &offset);
  if (rowKeyLen > value.size() - offset)
    {
      setError(error, "truncated local-lite covering index row key");
      return false;
    }
  *rowKey = value.substr(offset, static_cast<size_t>(rowKeyLen));
  offset += static_cast<size_t>(rowKeyLen);
  if (offset + 8 > value.size())
    {
      setError(error, "truncated local-lite covering index row payload");
      return false;
    }
  uint64_t rowLen = readUint64(value, &offset);
  if (rowLen > value.size() - offset || offset + rowLen != value.size())
    {
      setError(error, "truncated local-lite covering index row payload");
      return false;
    }
  *encodedRow = value.substr(offset, static_cast<size_t>(rowLen));
  return true;
}

static bool buildSecondaryIndexEntry(const LocalLiteTableDef &table,
                                     const LocalLiteIndexDef &index,
                                     const std::string &encodedRow,
                                     const std::string &rowKey,
                                     LocalLitePhysicalIndexEntry *entry,
                                     bool *hasEntry,
                                     std::string *error)
{
  if (!entry || !hasEntry)
    {
      setError(error, "missing local-lite secondary index output");
      return false;
    }

  std::string columnKey;
  bool hasColumnKey = false;
  bool containsNull = false;
  if (index.keyEncodingVersion >= 2)
    {
      if (!LocalLiteBuildOrderedSecondaryKeyPayload(
              table, index, encodedRow, &columnKey, &hasColumnKey,
              &containsNull, error))
        return false;
    }
  else if (!LocalLiteBuildUniqueKey(table, encodedRow, index.keyColumns, 0,
                                    &columnKey, &hasColumnKey, error))
    return false;

  // UNIQUE semantics allow multiple rows when any indexed column is NULL.
  // Those rows are intentionally absent from the physical index until NULL
  // entries are encoded for index scans in the optimizer-facing increment.
  if (!hasColumnKey)
    {
      *hasEntry = false;
      return true;
    }
  if (index.keyEncodingVersion < 2 &&
      (columnKey.size() < 9 || columnKey[0] != 'U'))
    {
      setError(error, "invalid local-lite secondary index key payload");
      return false;
    }

  entry->key.clear();
  entry->key.push_back('I');
  appendUint64(entry->key, index.objectUid);
  if (index.keyEncodingVersion >= 2)
    entry->key += columnKey;
  else
    entry->key.append(columnKey.data() + 9, columnKey.size() - 9);
  if (!index.unique || containsNull)
    {
      entry->key.push_back('R');
      appendUint64(entry->key, static_cast<uint64_t>(rowKey.size()));
      entry->key += rowKey;
    }
  entry->rowKey = rowKey;
  entry->value = index.keyEncodingVersion >= 4
      ? encodeCoveringIndexValue(rowKey, encodedRow)
      : rowKey;
  entry->unique = index.unique && !containsNull;
  *hasEntry = true;
  return true;
}

static bool buildSecondaryIndexEntries(
    const LocalLiteTableDef &table,
    const std::string &encodedRow,
    const std::string &rowKey,
    std::vector<LocalLitePhysicalIndexEntry> *entries,
    std::string *error)
{
  entries->clear();
  for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
    {
      LocalLitePhysicalIndexEntry entry;
      bool hasEntry = false;
      if (!buildSecondaryIndexEntry(table, table.secondaryIndexes[i],
                                    encodedRow, rowKey, &entry, &hasEntry,
                                    error))
        return false;
      if (hasEntry)
        entries->push_back(entry);
    }
  return true;
}

static bool buildRowRecords(const LocalLiteTableDef &table,
                            const LocalLiteRow &row,
                            const std::string &rowKey,
                            std::map<std::string, std::string> *records,
                            std::string *error)
{
  records->clear();
  (*records)[rowKey] = encodeRowValue(row.value);
  for (size_t keyIndex = 0;
       keyIndex < table.uniqueKeyColumns.size(); keyIndex++)
    {
      std::string uniqueKey;
      bool hasKey = false;
      if (!LocalLiteBuildUniqueKey(table, row.value,
                                   table.uniqueKeyColumns[keyIndex], keyIndex,
                                   &uniqueKey, &hasKey, error))
        return false;
      if (hasKey && !records->insert(
              std::make_pair(uniqueKey, rowKey)).second)
        {
          setError(error, "duplicate local-lite unique key");
          return false;
        }
    }
  std::vector<LocalLitePhysicalIndexEntry> entries;
  if (!buildSecondaryIndexEntries(table, row.value, rowKey, &entries, error))
    return false;
  for (size_t i = 0; i < entries.size(); i++)
    if (!records->insert(std::make_pair(entries[i].key,
                                        entries[i].value)).second)
      {
        setError(error, entries[i].unique
                        ? "duplicate local-lite unique index key"
                        : "duplicate local-lite secondary index key");
        return false;
      }
  return true;
}

class LocalLiteMutexGuard
{
public:
  explicit LocalLiteMutexGuard(pthread_mutex_t *mutex)
    : mutex_(mutex), locked_(false)
  {
    pthread_mutex_lock(mutex_);
    locked_ = true;
  }

  ~LocalLiteMutexGuard()
  {
    unlock();
  }

  void unlock()
  {
    if (locked_)
      {
        pthread_mutex_unlock(mutex_);
        locked_ = false;
      }
  }

  void lock()
  {
    if (!locked_)
      {
        pthread_mutex_lock(mutex_);
        locked_ = true;
      }
  }

private:
  LocalLiteMutexGuard(const LocalLiteMutexGuard &);
  LocalLiteMutexGuard &operator=(const LocalLiteMutexGuard &);

  pthread_mutex_t *mutex_;
  bool locked_;
};

struct LocalLiteJournalTable
{
  std::string catalog;
  std::string schema;
  std::string name;
  uint64_t objectUid;
  std::vector<LocalLiteRowMutation> updates;
  std::vector<LocalLiteRow> deletes;
  std::vector<LocalLiteRow> inserts;
};

static void appendJournalString(std::string &out, const std::string &value)
{
  appendUint64(out, value.size());
  out.append(value);
}

static bool readJournalUint64(const std::string &encoded,
                              size_t *offset,
                              uint64_t *value,
                              std::string *error)
{
  if (*offset + 8 > encoded.size())
    {
      setError(error, "truncated local-lite transaction journal");
      return false;
    }
  *value = readUint64(encoded, offset);
  return true;
}

static bool readJournalString(const std::string &encoded,
                              size_t *offset,
                              std::string *value,
                              std::string *error)
{
  uint64_t length = 0;
  if (!readJournalUint64(encoded, offset, &length, error) ||
      length > encoded.size() - *offset)
    {
      setError(error, "invalid local-lite transaction journal string");
      return false;
    }
  value->assign(encoded, *offset, static_cast<size_t>(length));
  *offset += static_cast<size_t>(length);
  return true;
}

static std::string encodeTransactionJournal(
    uint64_t transactionId,
    const std::vector<LocalLiteJournalTable> &tables)
{
  std::string encoded("LLTJ1", 5);
  appendUint64(encoded, transactionId);
  appendUint64(encoded, tables.size());
  for (size_t table = 0; table < tables.size(); table++)
    {
      appendJournalString(encoded, tables[table].catalog);
      appendJournalString(encoded, tables[table].schema);
      appendJournalString(encoded, tables[table].name);
      appendUint64(encoded, tables[table].objectUid);
      appendUint64(encoded, tables[table].updates.size());
      for (size_t i = 0; i < tables[table].updates.size(); i++)
        {
          appendUint64(encoded, tables[table].updates[i].before.rowId);
          appendJournalString(encoded, tables[table].updates[i].before.value);
          appendJournalString(encoded, tables[table].updates[i].after);
        }
      appendUint64(encoded, tables[table].deletes.size());
      for (size_t i = 0; i < tables[table].deletes.size(); i++)
        {
          appendUint64(encoded, tables[table].deletes[i].rowId);
          appendJournalString(encoded, tables[table].deletes[i].value);
        }
      appendUint64(encoded, tables[table].inserts.size());
      for (size_t i = 0; i < tables[table].inserts.size(); i++)
        {
          appendUint64(encoded, tables[table].inserts[i].rowId);
          appendJournalString(encoded, tables[table].inserts[i].value);
        }
    }
  return encoded;
}

static bool decodeTransactionJournal(
    const std::string &encoded,
    uint64_t *transactionId,
    std::vector<LocalLiteJournalTable> *tables,
    std::string *error)
{
  tables->clear();
  if (encoded.size() < 5 || encoded.compare(0, 5, "LLTJ1") != 0)
    {
      setError(error, "invalid local-lite transaction journal format");
      return false;
    }
  size_t offset = 5;
  uint64_t tableCount = 0;
  if (!readJournalUint64(encoded, &offset, transactionId, error) ||
      !readJournalUint64(encoded, &offset, &tableCount, error) ||
      tableCount > 100000)
    return false;
  tables->resize(static_cast<size_t>(tableCount));
  for (size_t table = 0; table < tables->size(); table++)
    {
      LocalLiteJournalTable &entry = (*tables)[table];
      if (!readJournalString(encoded, &offset, &entry.catalog, error) ||
          !readJournalString(encoded, &offset, &entry.schema, error) ||
          !readJournalString(encoded, &offset, &entry.name, error) ||
          !readJournalUint64(encoded, &offset, &entry.objectUid, error))
        return false;
      uint64_t count = 0;
      if (!readJournalUint64(encoded, &offset, &count, error) ||
          count > 10000000)
        return false;
      entry.updates.resize(static_cast<size_t>(count));
      for (size_t i = 0; i < entry.updates.size(); i++)
        if (!readJournalUint64(encoded, &offset,
                               &entry.updates[i].before.rowId, error) ||
            !readJournalString(encoded, &offset,
                               &entry.updates[i].before.value, error) ||
            !readJournalString(encoded, &offset,
                               &entry.updates[i].after, error))
          return false;
      if (!readJournalUint64(encoded, &offset, &count, error) ||
          count > 10000000)
        return false;
      entry.deletes.resize(static_cast<size_t>(count));
      for (size_t i = 0; i < entry.deletes.size(); i++)
        if (!readJournalUint64(encoded, &offset,
                               &entry.deletes[i].rowId, error) ||
            !readJournalString(encoded, &offset,
                               &entry.deletes[i].value, error))
          return false;
      if (!readJournalUint64(encoded, &offset, &count, error) ||
          count > 10000000)
        return false;
      entry.inserts.resize(static_cast<size_t>(count));
      for (size_t i = 0; i < entry.inserts.size(); i++)
        if (!readJournalUint64(encoded, &offset,
                               &entry.inserts[i].rowId, error) ||
            !readJournalString(encoded, &offset,
                               &entry.inserts[i].value, error))
          return false;
    }
  if (offset != encoded.size())
    {
      setError(error, "trailing bytes in local-lite transaction journal");
      return false;
    }
  return true;
}

static std::string transactionJournalKey(uint64_t transactionId)
{
  char id[32];
  snprintf(id, sizeof(id), "%020llu",
           static_cast<unsigned long long>(transactionId));
  return std::string("journal/") + id;
}

static std::string transactionTableMarker(uint64_t transactionId)
{
  std::string marker("J", 1);
  appendUint64(marker, transactionId);
  return marker;
}

class LocalLiteStorageManager
{
  friend class LocalLiteRocksDBStore;
public:
  static LocalLiteStorageManager &instance()
  {
    static LocalLiteStorageManager manager;
    return manager;
  }

  bool acquire(std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);

    if (refCount_ == 0)
      {
        if (!mkdirs(parentDir(LocalLiteRocksDBStore::catalogPath()), error))
          return false;
        if (!LocalLiteUnifiedRocksDBPrepare(
                LocalLiteRocksDBStore::defaultRoot(), error))
          return false;

        rocksdb_options_t *options = rocksdb_options_create();
        rocksdb_options_set_create_if_missing(options, 1);

        char *err = NULL;
        rocksdb_t *db = rocksdb_open(options,
                                     LocalLiteRocksDBStore::catalogPath().c_str(),
                                     &err);
        rocksdb_options_destroy(options);
        if (!checkRocksError(err,
                             "open RocksDB catalog " +
                             LocalLiteRocksDBStore::catalogPath(),
                             error))
          {
            LocalLiteUnifiedRocksDBShutdown();
            return false;
          }

        catalogDb_ = db;
        if (!migrateLocalLiteMetadataKeys(catalogDb_, error))
          {
            rocksdb_close(catalogDb_);
            catalogDb_ = NULL;
            LocalLiteUnifiedRocksDBShutdown();
            return false;
          }
        journalEngine_ = LocalLiteCreateRocksDBTransactionEngine();
        LocalLiteStorageOptions journalOptions;
        journalOptions.synchronousCommit = true;
        const char *minimumFree =
            getenv("TRAF_LOCAL_LITE_MINIMUM_FREE_BYTES");
        if (minimumFree && minimumFree[0])
          journalOptions.minimumFreeBytes = strtoull(minimumFree, NULL, 10);
        LocalLiteStorageStatus journalStatus;
        if (!journalEngine_->open(
                LocalLiteRocksDBStore::defaultRoot() +
                    "/m12-transaction-journal",
                journalOptions, &journalStatus))
          {
            setError(error, "open LocalLite M12 transaction journal: " +
                            journalStatus.message);
            delete journalEngine_;
            journalEngine_ = NULL;
            rocksdb_close(catalogDb_);
            catalogDb_ = NULL;
            LocalLiteUnifiedRocksDBShutdown();
            return false;
          }
        if (!recoverJournals(error))
          {
            journalEngine_->close();
            delete journalEngine_;
            journalEngine_ = NULL;
            rocksdb_close(catalogDb_);
            catalogDb_ = NULL;
            LocalLiteUnifiedRocksDBShutdown();
            return false;
          }
      }

    refCount_++;
    return true;
  }

  void release()
  {
    LocalLiteMutexGuard guard(&mutex_);

    if (refCount_ == 0)
      return;

    refCount_--;
    if (refCount_ != 0)
      return;

    releaseAllStatementSnapshotsLocked();

    for (TableMap::iterator it = tableDbs_.begin(); it != tableDbs_.end(); ++it)
      rocksdb_close(it->second);
    tableDbs_.clear();

    if (catalogDb_)
      rocksdb_close(catalogDb_);
    catalogDb_ = NULL;
    if (journalEngine_)
      {
        journalEngine_->close();
        delete journalEngine_;
        journalEngine_ = NULL;
      }
    LocalLiteUnifiedRocksDBShutdown();
  }

  bool persistJournal(const std::vector<LocalLiteJournalTable> &tables,
                      uint64_t *transactionId,
                      std::string *error)
  {
    if (!transactionId)
      {
        setError(error, "missing LocalLite M12 transaction journal id");
        return false;
      }
    LocalLiteStorageStatus status;
    LocalLiteStorageSession *session = journalEngine_
        ? journalEngine_->createSession(&status) : NULL;
    LocalLiteStorageTxn *txn = session ? session->begin(&status) : NULL;
    std::string encodedNextId;
    bool found = false;
    uint64_t nextId = 1;
    bool ok = txn && txn->get("meta/next-journal-id", &encodedNextId,
                              &found, &status);
    if (ok && found)
      {
        size_t offset = 0;
        if (encodedNextId.size() != 8)
          {
            status.code = LOCAL_LITE_STORAGE_CORRUPTION;
            status.message = "invalid LocalLite M12 next journal id";
            ok = false;
          }
        else
          nextId = readUint64(encodedNextId, &offset);
      }
    if (ok && nextId == 0)
      {
        status.code = LOCAL_LITE_STORAGE_CORRUPTION;
        status.message = "exhausted LocalLite M12 transaction journal ids";
        ok = false;
      }
    std::string encodedFollowingId;
    if (ok)
      appendUint64(encodedFollowingId, nextId + 1);
    if (ok)
      ok = txn->put("meta/next-journal-id", encodedFollowingId, &status) &&
           txn->put(transactionJournalKey(nextId),
                    encodeTransactionJournal(nextId, tables), &status) &&
           txn->commit(&status);
    if (!ok)
      setError(error, "persist LocalLite M12 transaction journal: " +
                      status.message);
    else
      *transactionId = nextId;
    delete txn;
    delete session;
    return ok;
  }

  bool removeJournal(uint64_t transactionId, std::string *error)
  {
    LocalLiteStorageStatus status;
    LocalLiteStorageSession *session = journalEngine_
        ? journalEngine_->createSession(&status) : NULL;
    LocalLiteStorageTxn *txn = session ? session->begin(&status) : NULL;
    const bool ok = txn &&
        txn->erase(transactionJournalKey(transactionId), &status) &&
        txn->commit(&status);
    if (!ok)
      setError(error, "remove LocalLite M12 transaction journal: " +
                      status.message);
    delete txn;
    delete session;
    return ok;
  }

  bool recoverJournals(std::string *error);

  rocksdb_t *catalogDb()
  {
    return catalogDb_;
  }

  rocksdb_t *openTable(const std::string &path,
                       bool createIfMissing,
                       std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);

    TableMap::iterator it = tableDbs_.find(path);
    if (it != tableDbs_.end())
      return it->second;

    return openTableLocked(path, createIfMissing, error);
  }

  void beginStatement(const void *statementOwner,
                      uint64_t statementExecutionId)
  {
    if (!statementOwner)
      return;

    LocalLiteMutexGuard guard(&mutex_);
    for (StatementMap::iterator it = statementSnapshots_.begin();
         it != statementSnapshots_.end();)
      {
        if (it->first.owner == statementOwner)
          {
            releaseStatementSnapshotsLocked(it->first, it->second);
            StatementMap::iterator stale = it++;
            statementSnapshots_.erase(stale);
          }
        else
          {
            ++it;
          }
      }

    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    SnapshotMap snapshots;
    StatementSnapshot entry;
    entry.db = catalogDb_;
    entry.snapshot = catalogDb_ ? rocksdb_create_snapshot(catalogDb_) : NULL;
    entry.readOptions = entry.snapshot ? rocksdb_readoptions_create() : NULL;
    if (entry.readOptions)
      {
        rocksdb_readoptions_set_snapshot(entry.readOptions, entry.snapshot);
        rocksdb_readoptions_set_fill_cache(entry.readOptions, 1);
      }
    if (entry.snapshot)
      {
        // Every logical catalog/table handle is a prefix view over the same
        // format-v2 TransactionDB.  Capture one physical snapshot when the
        // statement (or transaction-owned statement) begins instead of
        // acquiring unrelated lazy snapshots per logical table.
        snapshots[std::string()] = entry;
        traceStatementSnapshot("ACQUIRE", "<unified>",
                               statementExecutionId);
      }
    statementSnapshots_[key] = snapshots;
  }

  void endStatement(const void *statementOwner,
                    uint64_t statementExecutionId)
  {
    if (!statementOwner)
      return;

    LocalLiteMutexGuard guard(&mutex_);
    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    StatementMap::iterator it = statementSnapshots_.find(key);
    if (it == statementSnapshots_.end())
      return;

    releaseStatementSnapshotsLocked(it->first, it->second);
    statementSnapshots_.erase(it);
  }

  bool getStatementSnapshot(const std::string &path,
                            rocksdb_t *db,
                            const void *statementOwner,
                            uint64_t statementExecutionId,
                            const rocksdb_snapshot_t **snapshot,
                            std::string *error)
  {
    if (snapshot)
      *snapshot = NULL;
    if (!statementOwner || !snapshot)
      {
        setError(error, "missing local-lite statement snapshot context");
        return false;
      }

    LocalLiteMutexGuard guard(&mutex_);
    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    StatementMap::iterator statement = statementSnapshots_.find(key);
    if (statement == statementSnapshots_.end())
      {
        setError(error, "local-lite statement snapshot context is not active");
        return false;
      }

    SnapshotMap::iterator unified = statement->second.find(std::string());
    if (unified != statement->second.end())
      {
        *snapshot = unified->second.snapshot;
        traceStatementSnapshot("REUSE", path, statementExecutionId);
        return true;
      }

    (void) db;
    setError(error, "create local-lite unified RocksDB snapshot failed");
    return false;
  }

  bool getStatementReadOptions(const std::string &path,
                               rocksdb_t *db,
                               const void *statementOwner,
                               uint64_t statementExecutionId,
                               const rocksdb_readoptions_t **readOptions,
                               std::string *error)
  {
    if (readOptions)
      *readOptions = NULL;
    if (!statementOwner || !readOptions)
      {
        setError(error, "missing local-lite statement read context");
        return false;
      }

    LocalLiteMutexGuard guard(&mutex_);
    StatementKey key;
    key.owner = statementOwner;
    key.executionId = statementExecutionId;
    StatementMap::iterator statement = statementSnapshots_.find(key);
    if (statement == statementSnapshots_.end())
      {
        setError(error, "local-lite statement read context is not active");
        return false;
      }
    SnapshotMap::iterator unified = statement->second.find(std::string());
    if (unified != statement->second.end() && unified->second.readOptions)
      {
        *readOptions = unified->second.readOptions;
        traceStatementSnapshot("REUSE_OPTIONS", path, statementExecutionId);
        return true;
      }

    (void) db;
    setError(error, "create local-lite unified RocksDB read options failed");
    return false;
  }

  bool insertRow(const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);

    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;

    std::string key;
    if (!loaded.primaryKeyColumns.empty())
      {
        if (!LocalLiteBuildPrimaryKey(loaded, encodedRow, &key, error))
          return false;
      }
    else
      {
        appendUint64(key, loaded.nextRowId);
      }
    std::vector<std::string> uniqueKeys;
    for (size_t i = 0; i < loaded.uniqueKeyColumns.size(); i++)
      {
        std::string uniqueKey;
        bool hasUniqueKey = false;
        if (!LocalLiteBuildUniqueKey(loaded, encodedRow,
                                     loaded.uniqueKeyColumns[i], i,
                                     &uniqueKey, &hasUniqueKey, error))
          return false;
        if (hasUniqueKey)
          uniqueKeys.push_back(uniqueKey);
      }
    std::vector<LocalLitePhysicalIndexEntry> indexEntries;
    if (!buildSecondaryIndexEntries(loaded, encodedRow, key,
                                    &indexEntries, error))
      return false;

    const uint64_t allocatedRowId = loaded.nextRowId;
    LocalLiteTableDef updated = loaded;
    if (loaded.primaryKeyColumns.empty())
      updated.nextRowId++;

    std::string tableMetadataKey;
    std::string oldMetadata;
    if (loaded.primaryKeyColumns.empty())
      {
        tableMetadataKey = tableKey(loaded.catalog, loaded.schema, loaded.name);
        oldMetadata = encodeTable(loaded);
        std::string newMetadata = encodeTable(updated);
        if (!putCatalogLocked(tableMetadataKey, newMetadata,
                              "update local-lite row id metadata", error))
          return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      {
        if (loaded.primaryKeyColumns.empty())
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        return false;
      }

    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    char *err = NULL;
    size_t existingLen = 0;
    char *existing = rocksdb_get(db, readOptions,
                                 key.data(), key.size(), &existingLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, "read local-lite row", error))
      {
        if (loaded.primaryKeyColumns.empty())
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        return false;
      }
    if (existing)
      {
        rocksdb_free(existing);
        if (loaded.primaryKeyColumns.empty())
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        setError(error, loaded.primaryKeyColumns.empty()
                        ? "duplicate local-lite row key"
                        : "duplicate local-lite primary key");
        return false;
      }
    for (size_t i = 0; i < uniqueKeys.size(); i++)
      {
        readOptions = rocksdb_readoptions_create();
        err = NULL;
        existingLen = 0;
        existing = rocksdb_get(db, readOptions,
                               uniqueKeys[i].data(), uniqueKeys[i].size(),
                               &existingLen, &err);
        rocksdb_readoptions_destroy(readOptions);
        if (!checkRocksError(err, "read local-lite unique key", error))
          {
            if (loaded.primaryKeyColumns.empty())
              putCatalogLocked(tableMetadataKey, oldMetadata,
                               "rollback local-lite row id metadata", NULL);
            return false;
          }
        if (existing)
          {
            rocksdb_free(existing);
            if (loaded.primaryKeyColumns.empty())
              putCatalogLocked(tableMetadataKey, oldMetadata,
                               "rollback local-lite row id metadata", NULL);
            setError(error, "duplicate local-lite unique key");
            return false;
          }
      }
    for (size_t i = 0; i < indexEntries.size(); i++)
      {
        readOptions = rocksdb_readoptions_create();
        err = NULL;
        existingLen = 0;
        existing = rocksdb_get(db, readOptions,
                               indexEntries[i].key.data(),
                               indexEntries[i].key.size(),
                               &existingLen, &err);
        rocksdb_readoptions_destroy(readOptions);
        if (!checkRocksError(err, "read local-lite secondary index key",
                             error))
          {
            if (loaded.primaryKeyColumns.empty())
              putCatalogLocked(tableMetadataKey, oldMetadata,
                               "rollback local-lite row id metadata", NULL);
            return false;
          }
        if (existing)
          {
            rocksdb_free(existing);
            if (loaded.primaryKeyColumns.empty())
              putCatalogLocked(tableMetadataKey, oldMetadata,
                               "rollback local-lite row id metadata", NULL);
            setError(error, indexEntries[i].unique
                            ? "duplicate local-lite unique index key"
                            : "duplicate local-lite secondary index key");
            return false;
          }
      }

    std::string value = encodeRowValue(encodedRow);
    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    rocksdb_writebatch_put(batch, key.data(), key.size(),
                           value.data(), value.size());
    for (size_t i = 0; i < uniqueKeys.size(); i++)
      rocksdb_writebatch_put(batch, uniqueKeys[i].data(), uniqueKeys[i].size(),
                             key.data(), key.size());
    for (size_t i = 0; i < indexEntries.size(); i++)
      rocksdb_writebatch_put(batch, indexEntries[i].key.data(),
                             indexEntries[i].key.size(),
                             indexEntries[i].value.data(),
                             indexEntries[i].value.size());
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    err = NULL;
    rocksdb_write(db, writeOptions, batch, &err);
    rocksdb_writeoptions_destroy(writeOptions);
    rocksdb_writebatch_destroy(batch);
    if (!checkRocksError(err, "write local-lite row", error))
      {
        std::string rollbackError;
        if (loaded.primaryKeyColumns.empty() &&
            !putCatalogLocked(tableMetadataKey, oldMetadata,
                              "rollback local-lite row id metadata",
                              &rollbackError))
          {
            if (error)
              *error += "; " + rollbackError;
          }
        return false;
      }

    if (rowId)
      *rowId = allocatedRowId;
    return true;
  }

  bool updateRows(const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRowMutation> &mutations,
                  std::string *error)
  {
    if (mutations.empty())
      return true;

    LocalLiteMutexGuard guard(&mutex_);
    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;
    if (loaded.objectUid != table.objectUid)
      {
        setError(error, "local-lite table changed during update");
        return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      return false;

    std::vector<std::string> oldRowKeys(mutations.size());
    std::vector<std::string> newRowKeys(mutations.size());
    std::vector< std::vector<std::string> > oldUniqueKeys(mutations.size());
    std::vector< std::vector<std::string> > newUniqueKeys(mutations.size());
    std::vector< std::vector<LocalLitePhysicalIndexEntry> > oldIndexEntries(
        mutations.size());
    std::vector< std::vector<LocalLitePhysicalIndexEntry> > newIndexEntries(
        mutations.size());
    std::set<std::string> oldRowKeySet;
    std::set<std::string> newRowKeySet;
    std::set<std::string> newUniqueKeySet;
    std::set<std::string> newIndexKeySet;

    for (size_t i = 0; i < mutations.size(); i++)
      {
        if (loaded.primaryKeyColumns.empty())
          {
            appendUint64(oldRowKeys[i], mutations[i].before.rowId);
            newRowKeys[i] = oldRowKeys[i];
          }
        else
          {
            if (!LocalLiteBuildPrimaryKey(loaded, mutations[i].before.value,
                                          &oldRowKeys[i], error) ||
                !LocalLiteBuildPrimaryKey(loaded, mutations[i].after,
                                          &newRowKeys[i], error))
              return false;
          }
        if (!oldRowKeySet.insert(oldRowKeys[i]).second)
          {
            setError(error, "local-lite update selected a row more than once");
            return false;
          }
        if (!newRowKeySet.insert(newRowKeys[i]).second)
          {
            setError(error, "duplicate local-lite primary key");
            return false;
          }

        if (!buildSecondaryIndexEntries(
                loaded, mutations[i].before.value, oldRowKeys[i],
                &oldIndexEntries[i], error) ||
            !buildSecondaryIndexEntries(
                loaded, mutations[i].after, newRowKeys[i],
                &newIndexEntries[i], error))
          return false;
        for (size_t entry = 0; entry < newIndexEntries[i].size(); entry++)
          if (!newIndexKeySet.insert(newIndexEntries[i][entry].key).second)
            {
              setError(error, newIndexEntries[i][entry].unique
                              ? "duplicate local-lite unique index key"
                              : "duplicate local-lite secondary index key");
              return false;
            }

        for (size_t keyIndex = 0;
             keyIndex < loaded.uniqueKeyColumns.size(); keyIndex++)
          {
            std::string oldKey;
            std::string newKey;
            bool hasOldKey = false;
            bool hasNewKey = false;
            if (!LocalLiteBuildUniqueKey(
                    loaded, mutations[i].before.value,
                    loaded.uniqueKeyColumns[keyIndex], keyIndex,
                    &oldKey, &hasOldKey, error) ||
                !LocalLiteBuildUniqueKey(
                    loaded, mutations[i].after,
                    loaded.uniqueKeyColumns[keyIndex], keyIndex,
                    &newKey, &hasNewKey, error))
              return false;
            if (hasOldKey)
              oldUniqueKeys[i].push_back(oldKey);
            if (hasNewKey)
              {
                if (!newUniqueKeySet.insert(newKey).second)
                  {
                    setError(error, "duplicate local-lite unique key");
                    return false;
                  }
                newUniqueKeys[i].push_back(newKey);
              }
          }
      }

    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    for (size_t i = 0; i < mutations.size(); i++)
      {
        char *err = NULL;
        size_t valueLen = 0;
        char *value = rocksdb_get(db, readOptions,
                                  oldRowKeys[i].data(), oldRowKeys[i].size(),
                                  &valueLen, &err);
        if (!checkRocksError(err, "read local-lite row for update", error))
          {
            rocksdb_readoptions_destroy(readOptions);
            return false;
          }
        if (!value)
          {
            rocksdb_readoptions_destroy(readOptions);
            setError(error, "local-lite update row no longer exists");
            return false;
          }
        std::string currentValue(value, valueLen);
        rocksdb_free(value);
        std::string currentRow;
        if (!decodeRowValue(currentValue, &currentRow, error))
          {
            rocksdb_readoptions_destroy(readOptions);
            return false;
          }
        if (currentRow != mutations[i].before.value)
          {
            rocksdb_readoptions_destroy(readOptions);
            setError(error,
                     "local-lite update row changed; restart statement");
            return false;
          }

        if (newRowKeys[i] != oldRowKeys[i] &&
            oldRowKeySet.find(newRowKeys[i]) == oldRowKeySet.end())
          {
            err = NULL;
            valueLen = 0;
            value = rocksdb_get(db, readOptions,
                                newRowKeys[i].data(), newRowKeys[i].size(),
                                &valueLen, &err);
            if (!checkRocksError(err, "read updated local-lite row key", error))
              {
                rocksdb_readoptions_destroy(readOptions);
                return false;
              }
            if (value)
              {
                rocksdb_free(value);
                rocksdb_readoptions_destroy(readOptions);
                setError(error, "duplicate local-lite primary key");
                return false;
              }
          }

        for (size_t keyIndex = 0;
             keyIndex < newUniqueKeys[i].size(); keyIndex++)
          {
            err = NULL;
            valueLen = 0;
            value = rocksdb_get(db, readOptions,
                                newUniqueKeys[i][keyIndex].data(),
                                newUniqueKeys[i][keyIndex].size(),
                                &valueLen, &err);
            if (!checkRocksError(err, "read updated local-lite unique key",
                                 error))
              {
                rocksdb_readoptions_destroy(readOptions);
                return false;
              }
            if (!value)
              continue;
            std::string referencedRowKey;
            std::string ignoredRow;
            bool covering = false;
            std::string indexValue(value, valueLen);
            rocksdb_free(value);
            if (!decodeIndexValue(indexValue, &referencedRowKey,
                                  &ignoredRow, &covering, error))
              {
                rocksdb_readoptions_destroy(readOptions);
                return false;
              }
            if (oldRowKeySet.find(referencedRowKey) == oldRowKeySet.end())
              {
                rocksdb_readoptions_destroy(readOptions);
                setError(error, "duplicate local-lite unique key");
                return false;
              }
          }
        for (size_t entry = 0; entry < newIndexEntries[i].size(); entry++)
          {
            err = NULL;
            valueLen = 0;
            value = rocksdb_get(db, readOptions,
                                newIndexEntries[i][entry].key.data(),
                                newIndexEntries[i][entry].key.size(),
                                &valueLen, &err);
            if (!checkRocksError(err,
                                 "read updated local-lite index key", error))
              {
                rocksdb_readoptions_destroy(readOptions);
                return false;
              }
            if (!value)
              continue;
            // Version-4 secondary indexes store a covering value rather than
            // the bare base-row key. Decode both formats before deciding
            // whether an existing entry belongs to one of the rows being
            // replaced. Treating the covering payload as a row key makes a
            // key-preserving UPDATE on a UNIQUE index look like a conflict
            // after the catalog is reopened in a new SQLCI process.
            std::string referencedRowKey;
            std::string ignoredRow;
            bool covering = false;
            if (!decodeIndexValue(std::string(value, valueLen),
                                  &referencedRowKey, &ignoredRow,
                                  &covering, error))
              {
                rocksdb_readoptions_destroy(readOptions);
                return false;
              }
            rocksdb_free(value);
            if (oldRowKeySet.find(referencedRowKey) == oldRowKeySet.end())
              {
                rocksdb_readoptions_destroy(readOptions);
                setError(error, newIndexEntries[i][entry].unique
                                ? "duplicate local-lite unique index key"
                                : "duplicate local-lite secondary index key");
                return false;
              }
          }
      }
    rocksdb_readoptions_destroy(readOptions);

    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    for (size_t i = 0; i < mutations.size(); i++)
      {
        rocksdb_writebatch_delete(batch, oldRowKeys[i].data(),
                                  oldRowKeys[i].size());
        for (size_t keyIndex = 0;
             keyIndex < oldUniqueKeys[i].size(); keyIndex++)
          rocksdb_writebatch_delete(batch, oldUniqueKeys[i][keyIndex].data(),
                                    oldUniqueKeys[i][keyIndex].size());
        for (size_t entry = 0; entry < oldIndexEntries[i].size(); entry++)
          rocksdb_writebatch_delete(batch, oldIndexEntries[i][entry].key.data(),
                                    oldIndexEntries[i][entry].key.size());
      }
    for (size_t i = 0; i < mutations.size(); i++)
      {
        std::string value = encodeRowValue(mutations[i].after);
        rocksdb_writebatch_put(batch, newRowKeys[i].data(),
                               newRowKeys[i].size(),
                               value.data(), value.size());
        for (size_t keyIndex = 0;
             keyIndex < newUniqueKeys[i].size(); keyIndex++)
          rocksdb_writebatch_put(batch,
                                 newUniqueKeys[i][keyIndex].data(),
                                 newUniqueKeys[i][keyIndex].size(),
                                 newRowKeys[i].data(), newRowKeys[i].size());
        for (size_t entry = 0; entry < newIndexEntries[i].size(); entry++)
          rocksdb_writebatch_put(batch,
                                 newIndexEntries[i][entry].key.data(),
                                 newIndexEntries[i][entry].key.size(),
                                 newIndexEntries[i][entry].value.data(),
                                 newIndexEntries[i][entry].value.size());
      }

    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_write(db, writeOptions, batch, &err);
    rocksdb_writeoptions_destroy(writeOptions);
    rocksdb_writebatch_destroy(batch);
    return checkRocksError(err, "update local-lite rows", error);
  }

  bool replaceTableDefinition(
      const LocalLiteTableDef &oldTable,
      const LocalLiteTableDef &newTable,
      const std::vector<LocalLiteRowMutation> &mutations,
      std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    LocalLiteTableDef loaded;
    if (!loadTableLocked(oldTable.catalog, oldTable.schema, oldTable.name,
                         &loaded, error))
      return false;
    if (loaded.objectUid != oldTable.objectUid ||
        newTable.objectUid != oldTable.objectUid ||
        newTable.catalog != oldTable.catalog ||
        newTable.schema != oldTable.schema || newTable.name != oldTable.name)
      {
        setError(error, "local-lite table changed during ALTER");
        return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      return false;

    std::vector<std::string> oldRowKeys(mutations.size());
    std::vector<std::string> newRowKeys(mutations.size());
    std::vector< std::vector<std::string> > oldUniqueKeys(mutations.size());
    std::vector< std::vector<std::string> > newUniqueKeys(mutations.size());
    std::vector< std::vector<LocalLitePhysicalIndexEntry> > oldIndexes(
        mutations.size());
    std::vector< std::vector<LocalLitePhysicalIndexEntry> > newIndexes(
        mutations.size());
    std::set<std::string> rowKeySet;
    std::set<std::string> uniqueKeySet;
    std::set<std::string> indexKeySet;

    for (size_t i = 0; i < mutations.size(); i++)
      {
        if (loaded.primaryKeyColumns.empty())
          appendUint64(oldRowKeys[i], mutations[i].before.rowId);
        else if (!LocalLiteBuildPrimaryKey(loaded,
                                           mutations[i].before.value,
                                           &oldRowKeys[i], error))
          return false;
        if (newTable.primaryKeyColumns.empty())
          appendUint64(newRowKeys[i], mutations[i].before.rowId);
        else if (!LocalLiteBuildPrimaryKey(newTable, mutations[i].after,
                                           &newRowKeys[i], error))
          return false;
        if (!rowKeySet.insert(newRowKeys[i]).second)
          {
            setError(error, "duplicate local-lite primary key after ALTER");
            return false;
          }
        if (!buildSecondaryIndexEntries(loaded, mutations[i].before.value,
                                        oldRowKeys[i], &oldIndexes[i], error) ||
            !buildSecondaryIndexEntries(newTable, mutations[i].after,
                                        newRowKeys[i], &newIndexes[i], error))
          return false;
        for (size_t j = 0; j < newIndexes[i].size(); j++)
          if (!indexKeySet.insert(newIndexes[i][j].key).second)
            {
              setError(error, newIndexes[i][j].unique
                      ? "duplicate local-lite unique index key after ALTER"
                      : "duplicate local-lite secondary index key after ALTER");
              return false;
            }
        for (size_t j = 0; j < loaded.uniqueKeyColumns.size(); j++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildUniqueKey(loaded,
                                         mutations[i].before.value,
                                         loaded.uniqueKeyColumns[j], j,
                                         &key, &hasKey, error))
              return false;
            if (hasKey)
              oldUniqueKeys[i].push_back(key);
          }
        for (size_t j = 0; j < newTable.uniqueKeyColumns.size(); j++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildUniqueKey(newTable, mutations[i].after,
                                         newTable.uniqueKeyColumns[j], j,
                                         &key, &hasKey, error))
              return false;
            if (hasKey && !uniqueKeySet.insert(key).second)
              {
                setError(error, "duplicate local-lite unique key after ALTER");
                return false;
              }
            if (hasKey)
              newUniqueKeys[i].push_back(key);
          }
      }

    rocksdb_writebatch_t *forward = rocksdb_writebatch_create();
    rocksdb_writebatch_t *rollback = rocksdb_writebatch_create();
    for (size_t i = 0; i < mutations.size(); i++)
      {
        rocksdb_writebatch_delete(forward, oldRowKeys[i].data(),
                                  oldRowKeys[i].size());
        rocksdb_writebatch_delete(rollback, newRowKeys[i].data(),
                                  newRowKeys[i].size());
        for (size_t j = 0; j < oldUniqueKeys[i].size(); j++)
          rocksdb_writebatch_delete(forward, oldUniqueKeys[i][j].data(),
                                    oldUniqueKeys[i][j].size());
        for (size_t j = 0; j < newUniqueKeys[i].size(); j++)
          rocksdb_writebatch_delete(rollback, newUniqueKeys[i][j].data(),
                                    newUniqueKeys[i][j].size());
        for (size_t j = 0; j < oldIndexes[i].size(); j++)
          rocksdb_writebatch_delete(forward, oldIndexes[i][j].key.data(),
                                    oldIndexes[i][j].key.size());
        for (size_t j = 0; j < newIndexes[i].size(); j++)
          rocksdb_writebatch_delete(rollback, newIndexes[i][j].key.data(),
                                    newIndexes[i][j].key.size());
      }
    for (size_t i = 0; i < mutations.size(); i++)
      {
        std::string newValue = encodeRowValue(mutations[i].after);
        std::string oldValue = encodeRowValue(mutations[i].before.value);
        rocksdb_writebatch_put(forward, newRowKeys[i].data(),
                               newRowKeys[i].size(), newValue.data(),
                               newValue.size());
        rocksdb_writebatch_put(rollback, oldRowKeys[i].data(),
                               oldRowKeys[i].size(), oldValue.data(),
                               oldValue.size());
        for (size_t j = 0; j < newUniqueKeys[i].size(); j++)
          rocksdb_writebatch_put(forward, newUniqueKeys[i][j].data(),
                                 newUniqueKeys[i][j].size(),
                                 newRowKeys[i].data(), newRowKeys[i].size());
        for (size_t j = 0; j < oldUniqueKeys[i].size(); j++)
          rocksdb_writebatch_put(rollback, oldUniqueKeys[i][j].data(),
                                 oldUniqueKeys[i][j].size(),
                                 oldRowKeys[i].data(), oldRowKeys[i].size());
        for (size_t j = 0; j < newIndexes[i].size(); j++)
          rocksdb_writebatch_put(forward, newIndexes[i][j].key.data(),
                                 newIndexes[i][j].key.size(),
                                 newIndexes[i][j].value.data(),
                                 newIndexes[i][j].value.size());
        for (size_t j = 0; j < oldIndexes[i].size(); j++)
          rocksdb_writebatch_put(rollback, oldIndexes[i][j].key.data(),
                                 oldIndexes[i][j].key.size(),
                                 oldIndexes[i][j].value.data(),
                                 oldIndexes[i][j].value.size());
      }

    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_write(db, writeOptions, forward, &err);
    rocksdb_writebatch_destroy(forward);
    if (!checkRocksError(err, "rewrite local-lite rows for ALTER", error))
      {
        rocksdb_writebatch_destroy(rollback);
        rocksdb_writeoptions_destroy(writeOptions);
        return false;
      }

    std::string metadataKey = tableKey(loaded);
    std::string metadata = encodeTable(newTable);
    rocksdb_writebatch_t *catalogBatch = rocksdb_writebatch_create();
    rocksdb_writebatch_delete(catalogBatch, metadataKey.data(),
                              metadataKey.size());
    deleteLocalLiteMetadataForTable(catalogBatch, loaded);
    rocksdb_writebatch_put(catalogBatch, metadataKey.data(),
                           metadataKey.size(), metadata.data(),
                           metadata.size());
    addLocalLiteMetadataForTable(catalogBatch, newTable);
    err = NULL;
    rocksdb_write(catalogDb_, writeOptions, catalogBatch, &err);
    rocksdb_writebatch_destroy(catalogBatch);
    if (!checkRocksError(err, "write local-lite ALTER metadata", error))
      {
        char *rollbackErr = NULL;
        rocksdb_write(db, writeOptions, rollback, &rollbackErr);
        if (rollbackErr)
          {
            std::string ignored;
            checkRocksError(rollbackErr, "rollback local-lite ALTER", &ignored);
            if (error)
              *error += "; " + ignored;
          }
        rocksdb_writebatch_destroy(rollback);
        rocksdb_writeoptions_destroy(writeOptions);
        return false;
      }
    rocksdb_writebatch_destroy(rollback);
    rocksdb_writeoptions_destroy(writeOptions);
    return true;
  }

  bool deleteRows(const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRow> &rows,
                  std::string *error)
  {
    if (rows.empty())
      return true;

    LocalLiteMutexGuard guard(&mutex_);
    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;
    if (loaded.objectUid != table.objectUid)
      {
        setError(error, "local-lite table changed during delete");
        return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      return false;

    std::vector<std::string> rowKeys(rows.size());
    std::vector< std::vector<std::string> > uniqueKeys(rows.size());
    std::vector< std::vector<LocalLitePhysicalIndexEntry> > indexEntries(
        rows.size());
    std::set<std::string> selectedKeys;
    for (size_t i = 0; i < rows.size(); i++)
      {
        if (loaded.primaryKeyColumns.empty())
          appendUint64(rowKeys[i], rows[i].rowId);
        else if (!LocalLiteBuildPrimaryKey(loaded, rows[i].value,
                                           &rowKeys[i], error))
          return false;
        if (!selectedKeys.insert(rowKeys[i]).second)
          {
            setError(error, "local-lite delete selected a row more than once");
            return false;
          }
        if (!buildSecondaryIndexEntries(loaded, rows[i].value, rowKeys[i],
                                        &indexEntries[i], error))
          return false;

        for (size_t keyIndex = 0;
             keyIndex < loaded.uniqueKeyColumns.size(); keyIndex++)
          {
            std::string uniqueKey;
            bool hasUniqueKey = false;
            if (!LocalLiteBuildUniqueKey(
                    loaded, rows[i].value,
                    loaded.uniqueKeyColumns[keyIndex], keyIndex,
                    &uniqueKey, &hasUniqueKey, error))
              return false;
            if (hasUniqueKey)
              uniqueKeys[i].push_back(uniqueKey);
          }
      }

    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    for (size_t i = 0; i < rows.size(); i++)
      {
        char *err = NULL;
        size_t valueLen = 0;
        char *value = rocksdb_get(db, readOptions,
                                  rowKeys[i].data(), rowKeys[i].size(),
                                  &valueLen, &err);
        if (!checkRocksError(err, "read local-lite row for delete", error))
          {
            rocksdb_readoptions_destroy(readOptions);
            return false;
          }
        if (!value)
          {
            rocksdb_readoptions_destroy(readOptions);
            setError(error, "local-lite delete row no longer exists");
            return false;
          }
        std::string encodedValue(value, valueLen);
        rocksdb_free(value);
        std::string currentRow;
        if (!decodeRowValue(encodedValue, &currentRow, error))
          {
            rocksdb_readoptions_destroy(readOptions);
            return false;
          }
        if (currentRow != rows[i].value)
          {
            rocksdb_readoptions_destroy(readOptions);
            setError(error, "local-lite delete row changed; restart statement");
            return false;
          }
      }
    rocksdb_readoptions_destroy(readOptions);

    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    for (size_t i = 0; i < rows.size(); i++)
      {
        rocksdb_writebatch_delete(batch, rowKeys[i].data(), rowKeys[i].size());
        for (size_t keyIndex = 0;
             keyIndex < uniqueKeys[i].size(); keyIndex++)
          rocksdb_writebatch_delete(batch,
                                    uniqueKeys[i][keyIndex].data(),
                                    uniqueKeys[i][keyIndex].size());
        for (size_t entry = 0; entry < indexEntries[i].size(); entry++)
          rocksdb_writebatch_delete(batch, indexEntries[i][entry].key.data(),
                                    indexEntries[i][entry].key.size());
      }
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_write(db, writeOptions, batch, &err);
    rocksdb_writeoptions_destroy(writeOptions);
    rocksdb_writebatch_destroy(batch);
    return checkRocksError(err, "delete local-lite rows", error);
  }

  bool commitPendingRows(const LocalLiteTableDef &table,
                         const std::vector<LocalLiteRow> &rows,
                         std::string *error)
  {
    if (rows.empty())
      return true;

    LocalLiteMutexGuard guard(&mutex_);

    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;
    if (loaded.objectUid != table.objectUid)
      {
        setError(error,
                 "local-lite table changed while transaction was active");
        return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      return false;

    const bool keyless = loaded.primaryKeyColumns.empty();
    std::vector<std::string> rowKeys;
    std::vector< std::vector<std::string> > uniqueKeys;
    std::vector< std::vector<LocalLitePhysicalIndexEntry> > indexEntries;
    std::set<std::string> pendingRowKeys;
    std::set<std::string> pendingUniqueKeys;
    rowKeys.reserve(rows.size());
    uniqueKeys.resize(rows.size());
    indexEntries.resize(rows.size());

    for (size_t i = 0; i < rows.size(); i++)
      {
        std::string rowKey;
        if (keyless)
          {
            const uint64_t expectedRowId = loaded.nextRowId + i;
            if (rows[i].rowId != expectedRowId)
              {
                setError(
                    error,
                    "local-lite keyless row-id allocation changed while "
                    "transaction was active; restart and retry the "
                    "transaction");
                return false;
              }
            appendUint64(rowKey, rows[i].rowId);
          }
        else if (!LocalLiteBuildPrimaryKey(loaded, rows[i].value,
                                           &rowKey, error))
          {
            return false;
          }

        if (!pendingRowKeys.insert(rowKey).second)
          {
            setError(error, keyless ? "duplicate local-lite row key"
                                    : "duplicate local-lite primary key");
            return false;
          }
        rowKeys.push_back(rowKey);
        if (!buildSecondaryIndexEntries(loaded, rows[i].value, rowKey,
                                        &indexEntries[i], error))
          return false;

        for (size_t keyIndex = 0;
             keyIndex < loaded.uniqueKeyColumns.size(); keyIndex++)
          {
            std::string uniqueKey;
            bool hasUniqueKey = false;
            if (!LocalLiteBuildUniqueKey(
                    loaded, rows[i].value,
                    loaded.uniqueKeyColumns[keyIndex], keyIndex,
                    &uniqueKey, &hasUniqueKey, error))
              return false;
            if (!hasUniqueKey)
              continue;
            if (!pendingUniqueKeys.insert(uniqueKey).second)
              {
                setError(error, "duplicate local-lite unique key");
                return false;
              }
            uniqueKeys[i].push_back(uniqueKey);
          }
      }

    for (size_t i = 0; i < rowKeys.size(); i++)
      {
        bool exists = false;
        if (!keyExistsLocked(db, rowKeys[i], "read local-lite row",
                             &exists, error))
          return false;
        if (exists)
          {
            setError(error, keyless ? "duplicate local-lite row key"
                                    : "duplicate local-lite primary key");
            return false;
          }

        for (size_t keyIndex = 0;
             keyIndex < uniqueKeys[i].size(); keyIndex++)
          {
            if (!keyExistsLocked(db, uniqueKeys[i][keyIndex],
                                 "read local-lite unique key",
                                 &exists, error))
              return false;
            if (exists)
              {
                setError(error, "duplicate local-lite unique key");
                return false;
              }
          }
        for (size_t entry = 0; entry < indexEntries[i].size(); entry++)
          {
            if (!keyExistsLocked(db, indexEntries[i][entry].key,
                                 "read local-lite secondary index key",
                                 &exists, error))
              return false;
            if (exists)
              {
                setError(error, indexEntries[i][entry].unique
                                ? "duplicate local-lite unique index key"
                                : "duplicate local-lite secondary index key");
                return false;
              }
          }
      }

    std::string tableMetadataKey;
    std::string oldMetadata;
    if (keyless)
      {
        // Catalog metadata and table rows use separate RocksDB databases.
        // The manager mutex and rollback keep ordinary process execution
        // consistent, but a process crash between the two writes is not
        // atomic.
        LocalLiteTableDef updated = loaded;
        updated.nextRowId += rows.size();
        tableMetadataKey = tableKey(loaded.catalog, loaded.schema, loaded.name);
        oldMetadata = encodeTable(loaded);
        if (!putCatalogLocked(tableMetadataKey, encodeTable(updated),
                              "update local-lite row id metadata", error))
          return false;
      }

    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    for (size_t i = 0; i < rows.size(); i++)
      {
        std::string value = encodeRowValue(rows[i].value);
        rocksdb_writebatch_put(batch,
                               rowKeys[i].data(), rowKeys[i].size(),
                               value.data(), value.size());
        for (size_t keyIndex = 0;
             keyIndex < uniqueKeys[i].size(); keyIndex++)
          rocksdb_writebatch_put(batch,
                                 uniqueKeys[i][keyIndex].data(),
                                 uniqueKeys[i][keyIndex].size(),
                                 rowKeys[i].data(), rowKeys[i].size());
        for (size_t entry = 0; entry < indexEntries[i].size(); entry++)
          rocksdb_writebatch_put(batch,
                                 indexEntries[i][entry].key.data(),
                                 indexEntries[i][entry].key.size(),
                                 indexEntries[i][entry].value.data(),
                                 indexEntries[i][entry].value.size());
      }

    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_write(db, writeOptions, batch, &err);
    rocksdb_writeoptions_destroy(writeOptions);
    rocksdb_writebatch_destroy(batch);
    if (!checkRocksError(err, "commit local-lite table rows", error))
      {
        if (keyless)
          {
            std::string rollbackError;
            if (!putCatalogLocked(tableMetadataKey, oldMetadata,
                                  "rollback local-lite row id metadata",
                                  &rollbackError) &&
                error)
              *error += "; " + rollbackError;
            if (error)
              *error +=
                  "; keyless metadata and table rows use separate RocksDB "
                  "databases and are not crash-atomic";
          }
        return false;
      }

    return true;
  }

  bool commitPendingMutations(
      const LocalLiteTableDef &table,
      const std::vector<LocalLiteRowMutation> &updates,
      const std::vector<LocalLiteRow> &deletes,
      const std::vector<LocalLiteRow> &inserts,
      bool validateOnly,
      uint64_t journalTransactionId,
      std::string *error)
  {
    if (updates.empty() && deletes.empty() && inserts.empty())
      return true;

    LocalLiteMutexGuard guard(&mutex_);
    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;
    if (loaded.objectUid != table.objectUid)
      {
        setError(error,
                 "local-lite table changed while transaction was active");
        return false;
      }

    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      return false;

    const std::string journalMarker =
        transactionTableMarker(journalTransactionId);
    if (journalTransactionId != 0)
      {
        rocksdb_readoptions_t *markerReadOptions =
            rocksdb_readoptions_create();
        char *markerError = NULL;
        size_t markerValueLen = 0;
        char *markerValue = rocksdb_get(
            db, markerReadOptions, journalMarker.data(), journalMarker.size(),
            &markerValueLen, &markerError);
        rocksdb_readoptions_destroy(markerReadOptions);
        if (!checkRocksError(markerError,
                             "read local-lite transaction table marker",
                             error))
          return false;
        if (markerValue)
          {
            rocksdb_free(markerValue);
            return true;
          }
      }

    typedef std::map<std::string, std::string> RecordMap;
    typedef std::map<std::string, LocalLiteRow> RowMap;
    RecordMap originalRecords;
    RowMap originalRows;
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    rocksdb_iterator_t *it = rocksdb_create_iterator(db, readOptions);
    for (rocksdb_iter_seek_to_first(it); rocksdb_iter_valid(it);
         rocksdb_iter_next(it))
      {
        size_t keyLen = 0;
        size_t valueLen = 0;
        const char *rawKey = rocksdb_iter_key(it, &keyLen);
        const char *rawValue = rocksdb_iter_value(it, &valueLen);
        std::string key(rawKey, keyLen);
        std::string value(rawValue, valueLen);
        originalRecords[key] = value;
        if (!key.empty() &&
            (key[0] == 'U' || key[0] == 'I' || key[0] == 'J'))
          continue;

        LocalLiteRow row;
        row.rowId = 0;
        if (key.size() == 8)
          {
            size_t offset = 0;
            row.rowId = readUint64(key, &offset);
          }
        if (!decodeRowValue(value, &row.value, error))
          {
            rocksdb_iter_destroy(it);
            rocksdb_readoptions_destroy(readOptions);
            return false;
          }
        originalRows[key] = row;
      }
    char *iteratorError = NULL;
    rocksdb_iter_get_error(it, &iteratorError);
    rocksdb_iter_destroy(it);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(iteratorError,
                         "scan local-lite rows for transaction commit",
                         error))
      return false;

    RowMap finalRows = originalRows;
    std::set<std::string> removedKeys;
    std::vector<std::string> updatedKeys(updates.size());
    std::vector<std::string> insertedKeys(inserts.size());
    const bool keyless = loaded.primaryKeyColumns.empty();
    const uint64_t firstInsertedRowId =
        inserts.empty() ? loaded.nextRowId : inserts.front().rowId;
    const bool keylessMetadataAlreadyAdvanced = keyless && !inserts.empty() &&
        loaded.nextRowId == firstInsertedRowId + inserts.size();

    for (size_t i = 0; i < updates.size(); i++)
      {
        std::string oldKey;
        if (keyless)
          appendUint64(oldKey, updates[i].before.rowId);
        else if (!LocalLiteBuildPrimaryKey(loaded,
                                           updates[i].before.value,
                                           &oldKey, error))
          return false;
        RowMap::iterator current = originalRows.find(oldKey);
        if (current == originalRows.end() ||
            current->second.value != updates[i].before.value)
          {
            setError(error,
                     "local-lite update row changed; restart transaction");
            return false;
          }
        if (!removedKeys.insert(oldKey).second)
          {
            setError(error,
                     "local-lite transaction selected a row more than once");
            return false;
          }
        if (keyless)
          updatedKeys[i] = oldKey;
        else if (!LocalLiteBuildPrimaryKey(loaded, updates[i].after,
                                           &updatedKeys[i], error))
          return false;
      }

    for (size_t i = 0; i < deletes.size(); i++)
      {
        std::string rowKey;
        if (keyless)
          appendUint64(rowKey, deletes[i].rowId);
        else if (!LocalLiteBuildPrimaryKey(loaded, deletes[i].value,
                                           &rowKey, error))
          return false;
        RowMap::iterator current = originalRows.find(rowKey);
        if (current == originalRows.end() ||
            current->second.value != deletes[i].value)
          {
            setError(error,
                     "local-lite delete row changed; restart transaction");
            return false;
          }
        if (!removedKeys.insert(rowKey).second)
          {
            setError(error,
                     "local-lite transaction selected a row more than once");
            return false;
          }
      }

    for (std::set<std::string>::const_iterator key = removedKeys.begin();
         key != removedKeys.end(); ++key)
      finalRows.erase(*key);

    for (size_t i = 0; i < updates.size(); i++)
      {
        LocalLiteRow row;
        row.rowId = updates[i].before.rowId;
        row.value = updates[i].after;
        if (!finalRows.insert(std::make_pair(updatedKeys[i], row)).second)
          {
            setError(error, "duplicate local-lite primary key");
            return false;
          }
      }

    for (size_t i = 0; i < inserts.size(); i++)
      {
        if (keyless)
          {
            if (loaded.nextRowId != firstInsertedRowId &&
                !keylessMetadataAlreadyAdvanced)
              {
                setError(error,
                         "local-lite keyless row-id allocation changed while "
                         "transaction was active; restart and retry the "
                         "transaction");
                return false;
              }
            const uint64_t expectedRowId = firstInsertedRowId + i;
            if (inserts[i].rowId != expectedRowId)
              {
                setError(error,
                         "local-lite keyless row-id allocation changed while "
                         "transaction was active; restart and retry the "
                         "transaction");
                return false;
              }
            appendUint64(insertedKeys[i], inserts[i].rowId);
          }
        else if (!LocalLiteBuildPrimaryKey(loaded, inserts[i].value,
                                           &insertedKeys[i], error))
          return false;
        if (!finalRows.insert(
                std::make_pair(insertedKeys[i], inserts[i])).second)
          {
            setError(error, keyless ? "duplicate local-lite row key"
                                    : "duplicate local-lite primary key");
            return false;
          }
      }

    RecordMap finalRecords;
    for (RowMap::const_iterator row = finalRows.begin();
         row != finalRows.end(); ++row)
      {
        finalRecords[row->first] = encodeRowValue(row->second.value);
        for (size_t keyIndex = 0;
             keyIndex < loaded.uniqueKeyColumns.size(); keyIndex++)
          {
            std::string uniqueKey;
            bool hasUniqueKey = false;
            if (!LocalLiteBuildUniqueKey(
                    loaded, row->second.value,
                    loaded.uniqueKeyColumns[keyIndex], keyIndex,
                    &uniqueKey, &hasUniqueKey, error))
              return false;
            if (hasUniqueKey &&
                !finalRecords.insert(
                    std::make_pair(uniqueKey, row->first)).second)
              {
                setError(error, "duplicate local-lite unique key");
                return false;
              }
          }
        std::vector<LocalLitePhysicalIndexEntry> indexEntries;
        if (!buildSecondaryIndexEntries(loaded, row->second.value,
                                        row->first, &indexEntries, error))
          return false;
        for (size_t indexEntry = 0;
             indexEntry < indexEntries.size(); indexEntry++)
          if (!finalRecords.insert(std::make_pair(
                  indexEntries[indexEntry].key,
                  indexEntries[indexEntry].value)).second)
            {
              setError(error, "duplicate local-lite unique index key");
              return false;
            }
      }

    if (validateOnly)
      return true;

    std::string tableMetadataKey;
    std::string oldMetadata;
    if (keyless && !inserts.empty() && !keylessMetadataAlreadyAdvanced)
      {
        LocalLiteTableDef updated = loaded;
        updated.nextRowId += inserts.size();
        tableMetadataKey = tableKey(loaded.catalog, loaded.schema, loaded.name);
        oldMetadata = encodeTable(loaded);
        if (!putCatalogLocked(tableMetadataKey, encodeTable(updated),
                              "update local-lite row id metadata", error))
          return false;
      }

    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    if (journalTransactionId != 0)
      rocksdb_writebatch_put(batch,
                             journalMarker.data(), journalMarker.size(),
                             "1", 1);
    for (RecordMap::const_iterator record = originalRecords.begin();
         record != originalRecords.end(); ++record)
      if (finalRecords.find(record->first) == finalRecords.end())
        rocksdb_writebatch_delete(batch,
                                  record->first.data(), record->first.size());
    for (RecordMap::const_iterator record = finalRecords.begin();
         record != finalRecords.end(); ++record)
      {
        RecordMap::const_iterator old = originalRecords.find(record->first);
        if (old == originalRecords.end() || old->second != record->second)
          rocksdb_writebatch_put(batch,
                                 record->first.data(), record->first.size(),
                                 record->second.data(), record->second.size());
      }

    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *writeError = NULL;
    rocksdb_write(db, writeOptions, batch, &writeError);
    rocksdb_writeoptions_destroy(writeOptions);
    rocksdb_writebatch_destroy(batch);
    if (!checkRocksError(writeError,
                         "commit local-lite table mutations", error))
      {
        if (keyless && !inserts.empty() &&
            !keylessMetadataAlreadyAdvanced)
          putCatalogLocked(tableMetadataKey, oldMetadata,
                           "rollback local-lite row id metadata", NULL);
        return false;
      }
    return true;
  }

  bool appendPendingMutationsAtomic(
      const LocalLiteTableDef &table,
      const std::vector<LocalLiteRowMutation> &updates,
      const std::vector<LocalLiteRow> &deletes,
      const std::vector<LocalLiteRow> &inserts,
      LocalLiteUnifiedWriteBatch *batch,
      std::string *error)
  {
    if (updates.empty() && deletes.empty() && inserts.empty())
      return true;
    if (!batch)
      {
        setError(error, "missing local-lite unified commit batch");
        return false;
      }

    LocalLiteMutexGuard guard(&mutex_);
    LocalLiteTableDef loaded;
    if (!loadTableLocked(table.catalog, table.schema, table.name,
                         &loaded, error))
      return false;
    if (loaded.objectUid != table.objectUid)
      {
        setError(error,
                 "local-lite table changed while transaction was active");
        return false;
      }
    rocksdb_t *db = openTableLocked(LocalLiteRocksDBStore::tablePath(loaded),
                                    false, error);
    if (!db)
      return false;

    typedef std::map<std::string, std::string> RecordMap;
    RecordMap puts;
    std::set<std::string> removed;
    std::set<std::string> selectedRows;
    std::vector<std::string> updateKeys(updates.size());
    const bool keyless = loaded.primaryKeyColumns.empty();

    for (size_t phase = 0; phase < 2; phase++)
      {
        const size_t count = phase == 0 ? updates.size() : deletes.size();
        for (size_t i = 0; i < count; i++)
          {
            const LocalLiteRow &before = phase == 0
                ? updates[i].before : deletes[i];
            std::string rowKey;
            if (keyless)
              appendUint64(rowKey, before.rowId);
            else if (!LocalLiteBuildPrimaryKey(loaded, before.value,
                                               &rowKey, error))
              return false;
            if (!selectedRows.insert(rowKey).second)
              {
                setError(error,
                         "local-lite transaction selected a row more than once");
                return false;
              }

            rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
            char *rocksError = NULL;
            size_t valueLen = 0;
            char *value = rocksdb_get(db, readOptions,
                                      rowKey.data(), rowKey.size(),
                                      &valueLen, &rocksError);
            rocksdb_readoptions_destroy(readOptions);
            if (!checkRocksError(rocksError,
                                 "read local-lite commit before-image", error))
              return false;
            const std::string expected = encodeRowValue(before.value);
            const bool matches = value &&
                std::string(value, valueLen) == expected;
            if (value)
              rocksdb_free(value);
            if (!matches)
              {
                setError(error, phase == 0
                    ? "local-lite update row changed; restart transaction"
                    : "local-lite delete row changed; restart transaction");
                return false;
              }

            RecordMap oldRecords;
            if (!buildRowRecords(loaded, before, rowKey, &oldRecords, error))
              return false;
            for (RecordMap::const_iterator record = oldRecords.begin();
                 record != oldRecords.end(); ++record)
              removed.insert(record->first);
            if (phase == 0)
              {
                if (keyless)
                  updateKeys[i] = rowKey;
                else if (!LocalLiteBuildPrimaryKey(loaded, updates[i].after,
                                                   &updateKeys[i], error))
                  return false;
              }
          }
      }

    if (keyless && !inserts.empty())
      {
        if (inserts.front().rowId != loaded.nextRowId)
          {
            setError(error,
                     "local-lite keyless row-id allocation changed while "
                     "transaction was active; restart and retry the transaction");
            return false;
          }
        for (size_t i = 0; i < inserts.size(); i++)
          if (inserts[i].rowId != loaded.nextRowId + i)
            {
              setError(error,
                       "local-lite keyless row-id allocation is not contiguous");
              return false;
            }
      }

    for (size_t group = 0; group < 2; group++)
      {
        const size_t count = group == 0 ? updates.size() : inserts.size();
        for (size_t i = 0; i < count; i++)
          {
            LocalLiteRow row;
            std::string rowKey;
            if (group == 0)
              {
                row.rowId = updates[i].before.rowId;
                row.value = updates[i].after;
                rowKey = updateKeys[i];
              }
            else
              {
                row = inserts[i];
                if (keyless)
                  appendUint64(rowKey, row.rowId);
                else if (!LocalLiteBuildPrimaryKey(loaded, row.value,
                                                   &rowKey, error))
                  return false;
              }

            RecordMap newRecords;
            if (!buildRowRecords(loaded, row, rowKey, &newRecords, error))
              return false;
            for (RecordMap::const_iterator record = newRecords.begin();
                 record != newRecords.end(); ++record)
              {
                if (!puts.insert(*record).second)
                  {
                    setError(error, !record->first.empty() &&
                                    record->first[0] == 'U'
                                      ? "duplicate local-lite unique key"
                                      : (!record->first.empty() &&
                                         record->first[0] == 'I'
                                           ? "duplicate local-lite unique index key"
                                           : "duplicate local-lite primary key"));
                    return false;
                  }
                if (removed.find(record->first) != removed.end())
                  continue;
                bool exists = false;
                if (!keyExistsLocked(db, record->first,
                                     "read local-lite commit target key",
                                     &exists, error))
                  return false;
                if (exists)
                  {
                    setError(error, !record->first.empty() &&
                                    record->first[0] == 'U'
                                      ? "duplicate local-lite unique key"
                                      : (!record->first.empty() &&
                                         record->first[0] == 'I'
                                           ? "duplicate local-lite unique index key"
                                           : "duplicate local-lite primary key"));
                    return false;
                  }
              }
          }
      }

    for (std::set<std::string>::const_iterator key = removed.begin();
         key != removed.end(); ++key)
      LocalLiteUnifiedWriteBatchDelete(batch, db, *key);
    for (RecordMap::const_iterator record = puts.begin();
         record != puts.end(); ++record)
      LocalLiteUnifiedWriteBatchPut(batch, db, record->first, record->second);

    if (keyless && !inserts.empty())
      {
        LocalLiteTableDef updated = loaded;
        updated.nextRowId += inserts.size();
        LocalLiteUnifiedWriteBatchPut(
            batch, catalogDb_,
            tableKey(loaded.catalog, loaded.schema, loaded.name),
            encodeTable(updated));
      }
    LocalLiteUnifiedWriteBatchDelete(
        batch, catalogDb_, statsKey(loaded.catalog, loaded.schema, loaded.name));
    return true;
  }

  void closeTable(const std::string &path)
  {
    LocalLiteMutexGuard guard(&mutex_);

    releaseTableSnapshotsLocked(path);

    TableMap::iterator it = tableDbs_.find(path);
    if (it == tableDbs_.end())
      return;

    rocksdb_close(it->second);
    tableDbs_.erase(it);
  }

  pthread_mutex_t *mutex()
  {
    return &mutex_;
  }

private:
  typedef std::map<std::string, rocksdb_t *> TableMap;

  struct StatementKey
  {
    const void *owner;
    uint64_t executionId;
  };

  struct StatementKeyLess
  {
    bool operator()(const StatementKey &left,
                    const StatementKey &right) const
    {
      std::less<const void *> less;
      if (less(left.owner, right.owner))
        return true;
      if (less(right.owner, left.owner))
        return false;
      return left.executionId < right.executionId;
    }
  };

  struct StatementSnapshot
  {
    rocksdb_t *db;
    const rocksdb_snapshot_t *snapshot;
    rocksdb_readoptions_t *readOptions;
  };

  typedef std::map<std::string, StatementSnapshot> SnapshotMap;
  typedef std::map<StatementKey, SnapshotMap, StatementKeyLess> StatementMap;

  LocalLiteStorageManager()
    : refCount_(0),
      catalogDb_(NULL),
      journalEngine_(NULL)
  {
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex_, &attributes);
    pthread_mutexattr_destroy(&attributes);
  }

  LocalLiteStorageManager(const LocalLiteStorageManager &);
  LocalLiteStorageManager &operator=(const LocalLiteStorageManager &);

  rocksdb_t *openTableLocked(const std::string &path,
                             bool createIfMissing,
                             std::string *error)
  {
    TableMap::iterator it = tableDbs_.find(path);
    if (it != tableDbs_.end())
      return it->second;

    rocksdb_options_t *options = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(options, createIfMissing ? 1 : 0);
    char *err = NULL;
    rocksdb_t *db = rocksdb_open(options, path.c_str(), &err);
    rocksdb_options_destroy(options);
    if (!checkRocksError(err, "open RocksDB table " + path, error))
      return NULL;

    tableDbs_[path] = db;
    return db;
  }

  bool loadTableLocked(const std::string &catalog,
                       const std::string &schema,
                       const std::string &name,
                       LocalLiteTableDef *table,
                       std::string *error)
  {
    std::string key = tableKey(catalog, schema, name);
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    char *err = NULL;
    size_t valueLen = 0;
    char *value = rocksdb_get(catalogDb_, readOptions,
                              key.data(), key.size(), &valueLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, "read local-lite table metadata", error))
      return false;
    if (!value)
      {
        setError(error, "local-lite table does not exist: " +
                catalog + "." + schema + "." + name);
        return false;
      }

    std::string encoded(value, valueLen);
    rocksdb_free(value);
    return decodeTable(encoded, table, error);
  }

  bool putCatalogLocked(const std::string &key,
                        const std::string &value,
                        const std::string &errorPrefix,
                        std::string *error)
  {
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    char *err = NULL;
    rocksdb_put(catalogDb_, writeOptions,
                key.data(), key.size(), value.data(), value.size(), &err);
    rocksdb_writeoptions_destroy(writeOptions);
    return checkRocksError(err, errorPrefix, error);
  }

  bool keyExistsLocked(rocksdb_t *db,
                       const std::string &key,
                       const std::string &errorPrefix,
                       bool *exists,
                       std::string *error)
  {
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    char *err = NULL;
    size_t valueLen = 0;
    char *value = rocksdb_get(db, readOptions,
                              key.data(), key.size(), &valueLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, errorPrefix, error))
      return false;
    *exists = value != NULL;
    if (value)
      rocksdb_free(value);
    return true;
  }

  bool allocateSequence(uint64_t objectUid, int64_t requestedCount,
                        int64_t *nextValue, int64_t *endValue,
                        std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    std::string uid = uidKey(objectUid);
    rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
    char *err = NULL;
    size_t keyLen = 0;
    char *rawKey = rocksdb_get(catalogDb_, readOptions, uid.data(), uid.size(),
                               &keyLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, "read local-lite sequence UID", error))
      return false;
    if (!rawKey)
      {
        setError(error, "local-lite sequence does not exist");
        return false;
      }
    std::string key(rawKey, keyLen);
    rocksdb_free(rawKey);
    const std::string prefix("sequence|");
    if (key.compare(0, prefix.size(), prefix) != 0)
      {
        setError(error, "local-lite sequence UID resolves to another object");
        return false;
      }
    size_t first = key.find('|', prefix.size());
    size_t second = first == std::string::npos
        ? first : key.find('|', first + 1);
    if (first == std::string::npos || second == std::string::npos)
      {
        setError(error, "invalid local-lite sequence catalog key");
        return false;
      }
    std::string catalog = key.substr(prefix.size(), first - prefix.size());
    std::string schema = key.substr(first + 1, second - first - 1);
    std::string name = key.substr(second + 1);
    readOptions = rocksdb_readoptions_create();
    size_t valueLen = 0;
    char *rawValue = rocksdb_get(catalogDb_, readOptions,
                                 key.data(), key.size(), &valueLen, &err);
    rocksdb_readoptions_destroy(readOptions);
    if (!checkRocksError(err, "read local-lite sequence", error))
      return false;
    if (!rawValue)
      {
        setError(error, "local-lite sequence metadata is missing");
        return false;
      }
    std::string value(rawValue, valueLen);
    rocksdb_free(rawValue);
    LocalLiteSequenceDef sequence;
    if (!decodeSequence(value, catalog, schema, name, &sequence, error))
      return false;
    if (sequence.increment <= 0 || requestedCount <= 0)
      {
        setError(error, "invalid local-lite sequence allocation");
        return false;
      }
    int64_t next = sequence.nextValue;
    if (next > sequence.maxValue)
      {
        if (!sequence.cycle)
          {
            setError(error, "local-lite sequence has reached MAXVALUE");
            return false;
          }
        next = sequence.minValue;
      }
    uint64_t available = static_cast<uint64_t>(
        (sequence.maxValue - next) / sequence.increment) + 1;
    uint64_t requested = static_cast<uint64_t>(requestedCount);
    uint64_t count = requested < available ? requested : available;
    int64_t end = next + static_cast<int64_t>(count - 1) * sequence.increment;
    sequence.nextValue = end + sequence.increment;
    sequence.numCalls++;
    std::string updated = encodeSequence(sequence);
    rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
    err = NULL;
    rocksdb_put(catalogDb_, writeOptions, key.data(), key.size(),
                updated.data(), updated.size(), &err);
    rocksdb_writeoptions_destroy(writeOptions);
    if (!checkRocksError(err, "update local-lite sequence", error))
      return false;
    *nextValue = next;
    *endValue = end;
    return true;
  }

  void releaseStatementSnapshotsLocked(const StatementKey &key,
                                       SnapshotMap &snapshots)
  {
    for (SnapshotMap::iterator it = snapshots.begin();
         it != snapshots.end(); ++it)
      {
        rocksdb_release_snapshot(it->second.db, it->second.snapshot);
        rocksdb_readoptions_destroy(it->second.readOptions);
        traceStatementSnapshot("RELEASE", it->first, key.executionId);
      }
    snapshots.clear();
  }

  void releaseTableSnapshotsLocked(const std::string &path)
  {
    for (StatementMap::iterator statement = statementSnapshots_.begin();
         statement != statementSnapshots_.end(); ++statement)
      {
        SnapshotMap::iterator snapshot = statement->second.find(path);
        if (snapshot == statement->second.end())
          continue;
        rocksdb_release_snapshot(snapshot->second.db, snapshot->second.snapshot);
        rocksdb_readoptions_destroy(snapshot->second.readOptions);
        traceStatementSnapshot("RELEASE", path,
                               statement->first.executionId);
        statement->second.erase(snapshot);
      }
  }

  void releaseAllStatementSnapshotsLocked()
  {
    for (StatementMap::iterator it = statementSnapshots_.begin();
         it != statementSnapshots_.end(); ++it)
      releaseStatementSnapshotsLocked(it->first, it->second);
    statementSnapshots_.clear();
  }

  unsigned int refCount_;
  rocksdb_t *catalogDb_;
  LocalLiteStorageEngine *journalEngine_;
  TableMap tableDbs_;
  StatementMap statementSnapshots_;
  pthread_mutex_t mutex_;
};

bool LocalLiteStorageManager::recoverJournals(std::string *error)
{
  LocalLiteStorageStatus status;
  LocalLiteStorageSession *session = journalEngine_
      ? journalEngine_->createSession(&status) : NULL;
  LocalLiteStorageTxn *txn = session ? session->begin(&status) : NULL;
  LocalLiteStorageCursor *cursor = txn
      ? txn->scan("journal/", "journal0", &status) : NULL;
  std::vector<LocalLiteStorageRecord> records;
  bool ok = cursor != NULL;
  while (ok)
    {
      LocalLiteStorageRecord record;
      bool end = false;
      ok = cursor->next(&record, &end, &status);
      if (!ok || end)
        break;
      records.push_back(record);
    }
  delete cursor;
  if (txn)
    {
      LocalLiteStorageStatus rollbackStatus;
      txn->rollback(&rollbackStatus);
    }
  delete txn;
  delete session;
  if (!ok)
    {
      setError(error, "scan LocalLite M12 transaction journal: " +
                      status.message);
      return false;
    }

  for (size_t recordIndex = 0; recordIndex < records.size(); recordIndex++)
    {
      uint64_t transactionId = 0;
      std::vector<LocalLiteJournalTable> tables;
      if (!decodeTransactionJournal(records[recordIndex].value,
                                    &transactionId, &tables, error))
        return false;
      if (records[recordIndex].key != transactionJournalKey(transactionId))
        {
          setError(error,
                   "LocalLite M12 transaction journal key/id mismatch");
          return false;
        }
      for (size_t tableIndex = 0; tableIndex < tables.size(); tableIndex++)
        {
          LocalLiteTableDef table;
          table.catalog = tables[tableIndex].catalog;
          table.schema = tables[tableIndex].schema;
          table.name = tables[tableIndex].name;
          table.objectUid = tables[tableIndex].objectUid;
          if (!commitPendingMutations(
                  table, tables[tableIndex].updates,
                  tables[tableIndex].deletes,
                  tables[tableIndex].inserts, false, transactionId, error))
            {
              if (error)
                *error = "recover LocalLite M12 transaction " +
                         transactionJournalKey(transactionId) + ": " +
                         *error;
              return false;
            }
        }
      if (!removeJournal(transactionId, error))
        return false;
    }
  return true;
}

bool LocalLiteRocksDBCheckpoint(const std::string &path, std::string *error)
{
  LocalLiteMutexGuard guard(LocalLiteStorageManager::instance().mutex());
  return LocalLiteUnifiedRocksDBCheckpoint(path, error);
}

static bool readCatalogValue(const std::string &key,
                             std::string *value,
                             bool *found,
                             std::string *error)
{
  if (!value || !found)
    return false;
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *rawValue = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                               readOptions, key.data(), key.size(),
                               &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite authorization metadata", error))
    return false;
  *found = rawValue != NULL;
  if (rawValue)
    {
      value->assign(rawValue, valueLen);
      rocksdb_free(rawValue);
    }
  else
    value->clear();
  return true;
}

static bool writeCatalogValue(const std::string &key,
                              const std::string &value,
                              std::string *error)
{
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
              key.data(), key.size(), value.data(), value.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "write local-lite authorization metadata", error);
}

static bool deleteCatalogValue(const std::string &key, std::string *error)
{
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_delete(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
                 key.data(), key.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "delete local-lite authorization metadata", error);
}

static bool ensureRootAuthMetadata(std::string *error)
{
  std::string value;
  bool found = false;
  if (!readCatalogValue(authKey(LOCAL_LITE_ROOT_NAME), &value, &found, error))
    return false;
  if (!found)
    {
      LocalLiteAuthIdentity root;
      root.name = LOCAL_LITE_ROOT_NAME;
      root.id = LOCAL_LITE_ROOT_ID;
      root.role = false;
      if (!writeCatalogValue(authKey(root.name), encodeAuthIdentity(root), error) ||
          !writeCatalogValue(authIdKey(root.id), root.name, error))
        return false;
    }
  return true;
}

static bool validateLocalLiteChildRI(
    LocalLiteRocksDBStore *store, const LocalLiteTableDef &table,
    const std::vector<std::string> &rows, std::string *error);
static bool validateLocalLiteParentRI(
    LocalLiteRocksDBStore *store, const LocalLiteTableDef &parent,
    const std::vector<std::string> &oldRows,
    const std::vector<std::string> &newRows, std::string *error);

struct OccReadRange
{
  OccReadRange() : objectUid(0), full(false) {}
  uint64_t objectUid;
  std::string start;
  std::string end;
  bool full;
};

struct OccReadRangeLess
{
  bool operator()(const OccReadRange &left,
                  const OccReadRange &right) const
  {
    if (left.objectUid != right.objectUid)
      return left.objectUid < right.objectUid;
    if (left.full != right.full)
      return left.full < right.full;
    if (left.start != right.start)
      return left.start < right.start;
    return left.end < right.end;
  }
};

typedef std::pair<uint64_t, std::string> OccWriteKey;
typedef std::set<OccReadRange, OccReadRangeLess> OccReadSet;
typedef std::set<OccWriteKey> OccWriteSet;

struct LocalLiteOccMetrics
{
  LocalLiteOccMetrics()
    : transactionsStarted(0), transactionsCommitted(0),
      transactionsAbortedConflict(0), pointReads(0), missingPointReads(0),
      rangeReads(0), fullScans(0), primaryRangeReads(0),
      indexRangeReads(0), readRanges(0),
      writeKeys(0), validationCandidates(0), validationConflicts(0),
      pointValidationConflicts(0), rangeValidationConflicts(0),
      fullScanValidationConflicts(0), historyOverflowAborts(0),
      commitLatchWaitUs(0), validationLatencyUs(0),
      referentialValidationLatencyUs(0), batchBuildLatencyUs(0),
      publicationLatencyUs(0), visibilityPublicationLatencyUs(0),
      commitIntentWaitUs(0), physicalCommits(0),
      maximumPhysicalCommitOverlap(0) {}

  uint64_t transactionsStarted;
  uint64_t transactionsCommitted;
  uint64_t transactionsAbortedConflict;
  uint64_t pointReads;
  uint64_t missingPointReads;
  uint64_t rangeReads;
  uint64_t fullScans;
  uint64_t primaryRangeReads;
  uint64_t indexRangeReads;
  uint64_t readRanges;
  uint64_t writeKeys;
  uint64_t validationCandidates;
  uint64_t validationConflicts;
  uint64_t pointValidationConflicts;
  uint64_t rangeValidationConflicts;
  uint64_t fullScanValidationConflicts;
  uint64_t historyOverflowAborts;
  uint64_t commitLatchWaitUs;
  uint64_t validationLatencyUs;
  uint64_t referentialValidationLatencyUs;
  uint64_t batchBuildLatencyUs;
  uint64_t publicationLatencyUs;
  uint64_t visibilityPublicationLatencyUs;
  uint64_t commitIntentWaitUs;
  uint64_t physicalCommits;
  uint64_t maximumPhysicalCommitOverlap;
};

static LocalLiteOccMetrics localLiteOccMetrics;

static uint64_t monotonicMicros()
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
      static_cast<uint64_t>(now.tv_nsec) / 1000ULL;
}

static bool localLiteSynchronousCommit()
{
  const char *setting = getenv("TRAF_LOCAL_LITE_SYNC_COMMIT");
  if (!setting || !setting[0])
    return true;
  return strcmp(setting, "0") != 0 &&
      strcasecmp(setting, "false") != 0 &&
      strcasecmp(setting, "off") != 0 &&
      strcasecmp(setting, "async") != 0;
}

static void observeOccTransaction(uint64_t pointReads,
                                  uint64_t missingPointReads,
                                  uint64_t fullScans,
                                  uint64_t primaryRangeReads,
                                  uint64_t indexRangeReads,
                                  uint64_t readRanges,
                                  uint64_t writeKeys)
{
  localLiteOccMetrics.pointReads += pointReads;
  localLiteOccMetrics.missingPointReads += missingPointReads;
  localLiteOccMetrics.rangeReads +=
      fullScans + primaryRangeReads + indexRangeReads;
  localLiteOccMetrics.fullScans += fullScans;
  localLiteOccMetrics.primaryRangeReads += primaryRangeReads;
  localLiteOccMetrics.indexRangeReads += indexRangeReads;
  localLiteOccMetrics.readRanges += readRanges;
  localLiteOccMetrics.writeKeys += writeKeys;
}

struct OccCommittedWriteSet
{
  const void *owner;
  uint64_t sequence;
  OccWriteSet writes;
};

struct OccCommittedWriteRef
{
  const void *owner;
  uint64_t sequence;
  std::string key;
};

class LocalLiteOccCoordinator
{
public:
  LocalLiteOccCoordinator()
    : overflowSequence_(0), validationCandidates_(0),
      validationConflicts_(0), historyOverflowAborts_(0) {}

  void begin(const void *owner, uint64_t startSequence)
  {
    active_[owner] = startSequence;
  }

  void end(const void *owner)
  {
    active_.erase(owner);
    collect();
  }

  bool validate(const void *owner,
                uint64_t startSequence,
                const OccReadSet &reads,
                std::string *error)
  {
    if (overflowSequence_ != 0 && startSequence < overflowSequence_)
      {
        historyOverflowAborts_++;
        localLiteOccMetrics.historyOverflowAborts++;
        localLiteOccMetrics.transactionsAbortedConflict++;
        setError(error,
                 "local-lite serializable validation failed (SQLSTATE "
                 "40001, OCC history overflow); restart transaction");
        return false;
      }

    for (OccReadSet::const_iterator read = reads.begin();
         read != reads.end(); ++read)
      {
        std::map<uint64_t, std::vector<OccCommittedWriteRef> >::const_iterator
            indexed = indexedHistory_.find(read->objectUid);
        if (indexed == indexedHistory_.end())
          continue;
        for (std::vector<OccCommittedWriteRef>::const_iterator write =
               indexed->second.begin(); write != indexed->second.end(); ++write)
          {
            if (write->sequence <= startSequence || write->owner == owner)
              continue;
            validationCandidates_++;
            localLiteOccMetrics.validationCandidates++;
            {
              const bool intersects = write->key.empty() || read->full ||
                  (read->start == read->end
                       ? read->start == write->key
                       : write->key >= read->start &&
                           (read->end.empty() || write->key < read->end));
              if (intersects)
                {
                  const char *conflictType = read->full ? "full_scan" :
                      (read->start == read->end ? "point" : "range");
                  if (getenv("TRAF_LOCAL_LITE_TRACE_OCC"))
                    fprintf(stderr,
                            "LOCAL_LITE_OCC_CONFLICT type=%s owner=%p "
                            "object_uid=%llu start_sequence=%llu "
                            "write_sequence=%llu write=%s read_start=%s "
                            "read_end=%s full=%d\n",
                            conflictType, owner,
                            static_cast<unsigned long long>(read->objectUid),
                            static_cast<unsigned long long>(startSequence),
                            static_cast<unsigned long long>(write->sequence),
                            localLiteHexKey(write->key).c_str(),
                            localLiteHexKey(read->start).c_str(),
                            localLiteHexKey(read->end).c_str(),
                            read->full ? 1 : 0);
                  validationConflicts_++;
                  localLiteOccMetrics.validationConflicts++;
                  if (read->full)
                    localLiteOccMetrics.fullScanValidationConflicts++;
                  else if (read->start == read->end)
                    localLiteOccMetrics.pointValidationConflicts++;
                  else
                    localLiteOccMetrics.rangeValidationConflicts++;
                  localLiteOccMetrics.transactionsAbortedConflict++;
                  setError(error,
                           "local-lite serializable validation failed "
                           "(SQLSTATE 40001, OCC read/write conflict, type=" +
                           std::string(conflictType) + "); "
                           "restart transaction");
                  return false;
                }
            }
          }
      }
    return true;
  }

  void publish(const void *owner,
               uint64_t sequence,
               const OccWriteSet &writes)
  {
    if (writes.empty())
      return;
    OccCommittedWriteSet committed;
    committed.owner = owner;
    committed.sequence = sequence;
    committed.writes = writes;
    history_.push_back(committed);
    for (OccWriteSet::const_iterator write = writes.begin();
         write != writes.end(); ++write)
      {
        OccCommittedWriteRef ref;
        ref.owner = owner;
        ref.sequence = sequence;
        ref.key = write->second;
        indexedHistory_[write->first].push_back(ref);
      }
    collect();
  }

private:
  void removeIndexedSequence(uint64_t sequence)
  {
    for (std::map<uint64_t, std::vector<OccCommittedWriteRef> >::iterator
             indexed = indexedHistory_.begin();
         indexed != indexedHistory_.end();)
      {
        std::vector<OccCommittedWriteRef> &writes = indexed->second;
        for (std::vector<OccCommittedWriteRef>::iterator write = writes.begin();
             write != writes.end();)
          {
            if (write->sequence == sequence)
              write = writes.erase(write);
            else
              ++write;
          }
        if (writes.empty())
          indexed = indexedHistory_.erase(indexed);
        else
          ++indexed;
      }
  }

  void collect()
  {
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (std::map<const void *, uint64_t>::const_iterator active =
           active_.begin(); active != active_.end(); ++active)
      oldest = std::min(oldest, active->second);

    while (!history_.empty() &&
           (active_.empty() || history_.front().sequence <= oldest))
      {
        const uint64_t sequence = history_.front().sequence;
        history_.erase(history_.begin());
        removeIndexedSequence(sequence);
      }

    size_t writeCount = 0;
    for (size_t i = 0; i < history_.size(); i++)
      writeCount += history_[i].writes.size();
    while (writeCount > 1000000 && !history_.empty())
      {
        overflowSequence_ = std::max(overflowSequence_,
                                     history_.front().sequence);
        writeCount -= history_.front().writes.size();
        const uint64_t sequence = history_.front().sequence;
        history_.erase(history_.begin());
        removeIndexedSequence(sequence);
      }
    if (active_.empty())
      overflowSequence_ = 0;
  }

  std::map<const void *, uint64_t> active_;
  std::vector<OccCommittedWriteSet> history_;
  std::map<uint64_t, std::vector<OccCommittedWriteRef> > indexedHistory_;
  uint64_t overflowSequence_;
  uint64_t validationCandidates_;
  uint64_t validationConflicts_;
  uint64_t historyOverflowAborts_;
};

static LocalLiteOccCoordinator localLiteOccCoordinator;

struct LocalLiteActiveCommit
{
  const void *owner;
  OccReadSet reads;
  OccWriteSet writes;
};

class LocalLiteCommitCoordinator
{
public:
  LocalLiteCommitCoordinator() : physicalCommits_(0), maximumOverlap_(0)
  {
    pthread_mutex_init(&mutex_, NULL);
    pthread_cond_init(&condition_, NULL);
  }

  ~LocalLiteCommitCoordinator()
  {
    pthread_cond_destroy(&condition_);
    pthread_mutex_destroy(&mutex_);
  }

  void acquire(const void *owner,
               const OccReadSet &reads,
               const OccWriteSet &writes)
  {
    pthread_mutex_lock(&mutex_);
    while (conflicts(reads, writes))
      pthread_cond_wait(&condition_, &mutex_);
    LocalLiteActiveCommit active;
    active.owner = owner;
    active.reads = reads;
    active.writes = writes;
    active_.push_back(active);
    pthread_mutex_unlock(&mutex_);
  }

  void release(const void *owner)
  {
    pthread_mutex_lock(&mutex_);
    for (std::vector<LocalLiteActiveCommit>::iterator active = active_.begin();
         active != active_.end(); ++active)
      if (active->owner == owner)
        {
          active_.erase(active);
          break;
        }
    pthread_cond_broadcast(&condition_);
    pthread_mutex_unlock(&mutex_);
  }

  void enterPhysicalCommit()
  {
    pthread_mutex_lock(&mutex_);
    physicalCommits_++;
    maximumOverlap_ = std::max(maximumOverlap_, physicalCommits_);
    pthread_mutex_unlock(&mutex_);
  }

  void leavePhysicalCommit()
  {
    pthread_mutex_lock(&mutex_);
    if (physicalCommits_ > 0)
      physicalCommits_--;
    pthread_mutex_unlock(&mutex_);
  }

  uint64_t maximumOverlap() const
  {
    pthread_mutex_lock(&mutex_);
    const uint64_t result = maximumOverlap_;
    pthread_mutex_unlock(&mutex_);
    return result;
  }

  void resetMetrics()
  {
    pthread_mutex_lock(&mutex_);
    maximumOverlap_ = physicalCommits_;
    pthread_mutex_unlock(&mutex_);
  }

private:
  static bool keyIntersectsRead(const OccWriteKey &write,
                                const OccReadRange &read)
  {
    return write.first == read.objectUid &&
        (write.second.empty() || read.full ||
         (read.start == read.end
              ? read.start == write.second
              : write.second >= read.start &&
                  (read.end.empty() || write.second < read.end)));
  }

  static bool readWriteConflict(const OccReadSet &reads,
                                const OccWriteSet &writes)
  {
    for (OccReadSet::const_iterator read = reads.begin();
         read != reads.end(); ++read)
      for (OccWriteSet::const_iterator write = writes.begin();
           write != writes.end(); ++write)
        if (keyIntersectsRead(*write, *read))
          return true;
    return false;
  }

  static bool writeWriteConflict(const OccWriteSet &left,
                                 const OccWriteSet &right)
  {
    for (OccWriteSet::const_iterator a = left.begin(); a != left.end(); ++a)
      for (OccWriteSet::const_iterator b = right.begin(); b != right.end(); ++b)
        if (a->first == b->first &&
            (a->second.empty() || b->second.empty() || a->second == b->second))
          return true;
    return false;
  }

  bool conflicts(const OccReadSet &reads, const OccWriteSet &writes) const
  {
    for (std::vector<LocalLiteActiveCommit>::const_iterator active =
           active_.begin(); active != active_.end(); ++active)
      if (readWriteConflict(reads, active->writes) ||
          readWriteConflict(active->reads, writes) ||
          writeWriteConflict(writes, active->writes))
        return true;
    return false;
  }

  mutable pthread_mutex_t mutex_;
  pthread_cond_t condition_;
  std::vector<LocalLiteActiveCommit> active_;
  uint64_t physicalCommits_;
  uint64_t maximumOverlap_;
};

static LocalLiteCommitCoordinator localLiteCommitCoordinator;

class LocalLiteCommitIntentGuard
{
public:
  LocalLiteCommitIntentGuard(const void *owner,
                             const OccReadSet &reads,
                             const OccWriteSet &writes)
    : owner_(owner)
  {
    localLiteCommitCoordinator.acquire(owner, reads, writes);
  }
  ~LocalLiteCommitIntentGuard()
  {
    localLiteCommitCoordinator.release(owner_);
  }
private:
  const void *owner_;
};

static void publishOccTableChange(const void *owner, uint64_t objectUid)
{
  if (objectUid == 0)
    return;
  LocalLiteMutexGuard publication(
      LocalLiteStorageManager::instance().mutex());
  OccWriteSet writes;
  writes.insert(std::make_pair(objectUid, std::string()));
  localLiteOccCoordinator.publish(owner, LocalLiteUnifiedRocksDBSequence(),
                                  writes);
}

class LocalLiteTxnContext
{
public:
  LocalLiteTxnContext()
    : active_(false),
      nextLocalTxnId_(1),
      localTxnId_(0),
      beginSequence_(0),
      pointReads_(0),
      missingPointReads_(0),
      fullScans_(0),
      primaryRangeReads_(0),
      indexRangeReads_(0),
      executorTxnId_(LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID)
  {
    pthread_mutex_init(&mutex_, NULL);
  }

  ~LocalLiteTxnContext()
  {
    reset();
    pthread_mutex_destroy(&mutex_);
  }

  void reset()
  {
    LocalLiteMutexGuard guard(&mutex_);

    if (active_)
      {
        LocalLiteMutexGuard publication(
            LocalLiteStorageManager::instance().mutex());
        observeOccTransaction(pointReads_, missingPointReads_, fullScans_,
                              primaryRangeReads_, indexRangeReads_,
                              readRanges_.size(),
                              writeKeys_.size());
        LocalLiteStorageManager::instance().endStatement(this, localTxnId_);
        localLiteOccCoordinator.end(this);
      }
    for (std::map<const void *, uint64_t>::const_iterator it =
           statementExecutions_.begin();
         it != statementExecutions_.end(); ++it)
      LocalLiteStorageManager::instance().endStatement(it->first, it->second);

    statementExecutions_.clear();
    pendingTables_.clear();
    readRanges_.clear();
    writeKeys_.clear();
    localTxnId_ = 0;
    beginSequence_ = 0;
    pointReads_ = 0;
    missingPointReads_ = 0;
    fullScans_ = 0;
    primaryRangeReads_ = 0;
    indexRangeReads_ = 0;
    executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
    active_ = false;
    transactionStore_.close();
  }

  void beginStatement(const void *statementOwner,
                      uint64_t statementExecutionId)
  {
    if (!statementOwner)
      return;

    LocalLiteMutexGuard guard(&mutex_);
    // Statement hooks run before the first storage TCB opens its handle. Keep
    // one context-owned reference so the transaction/statement snapshot is
    // captured at the real statement boundary even in a fresh SQL process.
    std::string openError;
    transactionStore_.open(&openError);
    statementExecutions_[statementOwner] = statementExecutionId;
    LocalLiteStorageManager::instance().beginStatement(statementOwner,
                                                       statementExecutionId);
  }

  void endStatement(const void *statementOwner,
                    uint64_t statementExecutionId)
  {
    if (!statementOwner)
      return;

    LocalLiteMutexGuard guard(&mutex_);
    std::map<const void *, uint64_t>::iterator it =
        statementExecutions_.find(statementOwner);
    if (it == statementExecutions_.end() ||
        it->second != statementExecutionId)
      return;

    LocalLiteStorageManager::instance().endStatement(statementOwner,
                                                     statementExecutionId);
    statementExecutions_.erase(it);
    if (!active_)
      transactionStore_.close();
  }

  bool begin(int64_t executorTxnId, std::string *error)
  {
    uint64_t transactionId = 0;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          setError(error, "local-lite transaction already active");
          return false;
        }

      pendingTables_.clear();
      readRanges_.clear();
      writeKeys_.clear();
      pointReads_ = 0;
      missingPointReads_ = 0;
      fullScans_ = 0;
      primaryRangeReads_ = 0;
      indexRangeReads_ = 0;
      localTxnId_ = nextLocalTxnId_++;
      if (nextLocalTxnId_ == 0)
        nextLocalTxnId_ = 1;
      executorTxnId_ = executorTxnId;
      active_ = true;
      transactionId = localTxnId_;
    }

    if (!transactionStore_.open(error))
      {
        LocalLiteMutexGuard guard(&mutex_);
        pendingTables_.clear();
        localTxnId_ = 0;
        executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
        active_ = false;
        return false;
      }

    {
      LocalLiteMutexGuard atomicRead(
          LocalLiteStorageManager::instance().mutex());
      beginSequence_ = LocalLiteUnifiedRocksDBSequence();
      LocalLiteStorageManager::instance().beginStatement(this, transactionId);
      localLiteOccCoordinator.begin(this, beginSequence_);
      localLiteOccMetrics.transactionsStarted++;
    }
    return true;
  }

  bool commit(int64_t executorTxnId, std::string *error)
  {
    PendingMap pending;
    OccReadSet readRanges;
    OccWriteSet writeKeys;
    uint64_t transactionId = 0;
    uint64_t beginSequence = 0;
    uint64_t pointReads = 0;
    uint64_t missingPointReads = 0;
    uint64_t fullScans = 0;
    uint64_t primaryRangeReads = 0;
    uint64_t indexRangeReads = 0;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (!active_)
        {
          setError(error, "no active local-lite transaction");
          return false;
        }
      if (!matchesExecutorTxnId(executorTxnId))
        {
          setError(error, "local-lite transaction context mismatch");
          return false;
        }

      pending.swap(pendingTables_);
      readRanges = readRanges_;
      writeKeys = writeKeys_;
      transactionId = localTxnId_;
      beginSequence = beginSequence_;
      pointReads = pointReads_;
      missingPointReads = missingPointReads_;
      fullScans = fullScans_;
      primaryRangeReads = primaryRangeReads_;
      indexRangeReads = indexRangeReads_;
      localTxnId_ = 0;
      beginSequence_ = 0;
      executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
      active_ = false;
    }

    LocalLiteStorageManager::instance().endStatement(this, transactionId);
    // Keep readers and writers behind one process-wide publication latch.
    // Every logical table and catalog record is a prefix view over the same
    // TransactionDB, so one physical write batch is the commit decision.
    const uint64_t intentWaitStarted = monotonicMicros();
    LocalLiteCommitIntentGuard commitIntent(this, readRanges, writeKeys);
    const uint64_t intentAcquired = monotonicMicros();
    const uint64_t commitLatchWaitStarted = monotonicMicros();
    LocalLiteMutexGuard atomicPublication(
        LocalLiteStorageManager::instance().mutex());
    const uint64_t commitLatchAcquired = monotonicMicros();
    if (commitLatchAcquired >= commitLatchWaitStarted)
      localLiteOccMetrics.commitLatchWaitUs +=
          commitLatchAcquired - commitLatchWaitStarted;
    if (intentAcquired >= intentWaitStarted)
      localLiteOccMetrics.commitIntentWaitUs +=
          intentAcquired - intentWaitStarted;
    observeOccTransaction(pointReads, missingPointReads, fullScans,
                          primaryRangeReads, indexRangeReads,
                          readRanges.size(),
                          writeKeys.size());
    const uint64_t validationStarted = monotonicMicros();
    const bool validationPassed = writeKeys.empty() ||
        localLiteOccCoordinator.validate(this, beginSequence,
                                          readRanges, error);
    const uint64_t validationFinished = monotonicMicros();
    if (validationFinished >= validationStarted)
      localLiteOccMetrics.validationLatencyUs +=
          validationFinished - validationStarted;
    if (!validationPassed)
      {
        endOcc();
        atomicPublication.unlock();
        transactionStore_.close();
        return false;
      }
    const uint64_t referentialValidationStarted = monotonicMicros();
    const bool referentialValidationPassed =
        validatePendingReferentialIntegrity(pending, error);
    const uint64_t referentialValidationFinished = monotonicMicros();
    if (referentialValidationFinished >= referentialValidationStarted)
      localLiteOccMetrics.referentialValidationLatencyUs +=
          referentialValidationFinished - referentialValidationStarted;
    if (!referentialValidationPassed)
      {
        endOcc();
        atomicPublication.unlock();
        transactionStore_.close();
        return false;
      }
    const uint64_t batchBuildStarted = monotonicMicros();
    LocalLiteUnifiedWriteBatch *batch = LocalLiteUnifiedWriteBatchCreate();
    for (PendingMap::const_iterator t = pending.begin();
         t != pending.end(); ++t)
      if (!LocalLiteStorageManager::instance().appendPendingMutationsAtomic(
              t->second.table, t->second.updates, t->second.deletes,
              t->second.rows, batch, error))
        {
          LocalLiteUnifiedWriteBatchDestroy(batch);
          endOcc();
          atomicPublication.unlock();
          transactionStore_.close();
          return false;
        }
    const uint64_t batchBuildFinished = monotonicMicros();
    if (batchBuildFinished >= batchBuildStarted)
      localLiteOccMetrics.batchBuildLatencyUs +=
          batchBuildFinished - batchBuildStarted;

    const char *commitFault = getenv("TRAF_LOCAL_LITE_COMMIT_FAULT");
    if (commitFault &&
        (strcmp(commitFault, "before-batch") == 0 ||
         strcmp(commitFault, "before-journal") == 0))
      _exit(92);
    const uint64_t publicationStarted = monotonicMicros();
    atomicPublication.unlock();
    localLiteCommitCoordinator.enterPhysicalCommit();
    const char *commitHold = getenv("TRAF_LOCAL_LITE_COMMIT_HOLD_MS");
    if (commitHold && commitHold[0])
      usleep(static_cast<useconds_t>(strtoul(commitHold, NULL, 10)) * 1000U);
    const bool publicationPassed =
        LocalLiteUnifiedWriteBatchCommit(
            batch, localLiteSynchronousCommit(), error);
    localLiteCommitCoordinator.leavePhysicalCommit();
    const uint64_t publicationFinished = monotonicMicros();
    atomicPublication.lock();
    if (publicationFinished >= publicationStarted)
      localLiteOccMetrics.publicationLatencyUs +=
          publicationFinished - publicationStarted;
    localLiteOccMetrics.physicalCommits++;
    localLiteOccMetrics.maximumPhysicalCommitOverlap = std::max(
        localLiteOccMetrics.maximumPhysicalCommitOverlap,
        localLiteCommitCoordinator.maximumOverlap());
    if (!publicationPassed)
      {
        LocalLiteUnifiedWriteBatchDestroy(batch);
        endOcc();
        atomicPublication.unlock();
        transactionStore_.close();
        return false;
      }
    LocalLiteUnifiedWriteBatchDestroy(batch);
    if (commitFault &&
        (strcmp(commitFault, "after-batch") == 0 ||
         strcmp(commitFault, "after-journal") == 0))
      _exit(93);

    const uint64_t visibilityStarted = monotonicMicros();
    localLiteOccCoordinator.publish(this, LocalLiteUnifiedRocksDBSequence(),
                                    writeKeys);
    const uint64_t visibilityFinished = monotonicMicros();
    if (visibilityFinished >= visibilityStarted)
      localLiteOccMetrics.visibilityPublicationLatencyUs +=
          visibilityFinished - visibilityStarted;
    localLiteOccMetrics.transactionsCommitted++;
    endOcc();
    atomicPublication.unlock();
    transactionStore_.close();
    return true;
  }

  bool rollback(int64_t executorTxnId, std::string *error)
  {
    uint64_t transactionId = 0;
    uint64_t pointReads = 0;
    uint64_t missingPointReads = 0;
    uint64_t fullScans = 0;
    uint64_t primaryRangeReads = 0;
    uint64_t indexRangeReads = 0;
    uint64_t readRanges = 0;
    uint64_t writeKeys = 0;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (!active_)
        {
          setError(error, "no active local-lite transaction");
          return false;
        }
      if (!matchesExecutorTxnId(executorTxnId))
        {
          setError(error, "local-lite transaction context mismatch");
          return false;
        }

      pendingTables_.clear();
      transactionId = localTxnId_;
      pointReads = pointReads_;
      missingPointReads = missingPointReads_;
      fullScans = fullScans_;
      primaryRangeReads = primaryRangeReads_;
      indexRangeReads = indexRangeReads_;
      readRanges = readRanges_.size();
      writeKeys = writeKeys_.size();
      localTxnId_ = 0;
      beginSequence_ = 0;
      executorTxnId_ = LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
      active_ = false;
    }

    {
      LocalLiteMutexGuard publication(
          LocalLiteStorageManager::instance().mutex());
      observeOccTransaction(pointReads, missingPointReads, fullScans,
                            primaryRangeReads, indexRangeReads,
                            readRanges, writeKeys);
      LocalLiteStorageManager::instance().endStatement(this, transactionId);
      localLiteOccCoordinator.end(this);
    }
    transactionStore_.close();
    return true;
  }

  bool prepareDDL(int64_t executorTxnId, std::string *error)
  {
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (!active_)
        return true;
      if (!matchesExecutorTxnId(executorTxnId))
        {
          setError(error, "local-lite transaction context mismatch");
          return false;
        }
    }

    // ALTER TABLE rewrites both the row layout and the catalog definition.
    // Commit the current DML image first so no pending mutation retains the
    // old layout while the new definition is visible.
    if (!commit(executorTxnId, error))
      return false;
    return begin(executorTxnId, error);
  }

  bool refreshDDLSequence(int64_t executorTxnId, std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    if (active_ && !matchesExecutorTxnId(executorTxnId))
      {
        setError(error, "local-lite transaction context mismatch");
        return false;
      }
    // Sequence and identity allocation are autonomous. DDL is published as
    // an owner-tagged OCC table change. Neither operation is allowed to move
    // a transaction's original validation boundary forward.
    return true;
  }

  bool active()
  {
    LocalLiteMutexGuard guard(&mutex_);
    return active_;
  }

  uint64_t currentLocalTxnId()
  {
    LocalLiteMutexGuard guard(&mutex_);
    return active_ ? localTxnId_ : 0;
  }

  int64_t currentExecutorTxnId()
  {
    LocalLiteMutexGuard guard(&mutex_);
    return active_ ? executorTxnId_
                   : LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID;
  }

  bool occState(LocalLiteOccState *state)
  {
    if (!state)
      return false;
    LocalLiteMutexGuard guard(&mutex_);
    state->startSequence = beginSequence_;
    state->readRanges = readRanges_.size();
    state->writeKeys = writeKeys_.size();
    state->pointReads = pointReads_;
    state->missingPointReads = missingPointReads_;
    state->fullScans = fullScans_;
    state->indexRangeReads = indexRangeReads_;
    return active_;
  }

  bool insertRow(LocalLiteRocksDBStore *store,
                 const LocalLiteTableDef &table,
                 const std::string &encodedRow,
                 uint64_t *rowId,
                 std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return store->insertRow(table, encodedRow, rowId, error);

    const std::string key = tableKey(table);
    PendingTable &pending = pendingTables_[key];
    if (!pending.initialized)
      {
        LocalLiteTableDef loaded;
        if (!store->loadTable(table.catalog, table.schema, table.name,
                              &loaded, error))
          return false;
        pending.table = loaded;
        pending.nextRowId = loaded.nextRowId;
        pending.initialized = true;
      }

    if (!pending.table.primaryKeyColumns.empty())
      {
        std::string newKey;
        if (!LocalLiteBuildPrimaryKey(pending.table, encodedRow, &newKey, error))
          return false;
        for (size_t i = 0; i < pending.rows.size(); i++)
          {
            std::string existingKey;
            if (!LocalLiteBuildPrimaryKey(pending.table, pending.rows[i].value,
                                          &existingKey, error))
              return false;
            if (existingKey == newKey)
              {
                setError(error, "duplicate local-lite primary key");
                return false;
              }
          }
      }
    for (size_t keyIndex = 0;
         keyIndex < pending.table.uniqueKeyColumns.size(); keyIndex++)
      {
        std::string newKey;
        bool hasNewKey = false;
        if (!LocalLiteBuildUniqueKey(pending.table, encodedRow,
                                     pending.table.uniqueKeyColumns[keyIndex],
                                     keyIndex, &newKey, &hasNewKey, error))
          return false;
        if (!hasNewKey)
          continue;
        for (size_t i = 0; i < pending.rows.size(); i++)
          {
            std::string existingKey;
            bool hasExistingKey = false;
            if (!LocalLiteBuildUniqueKey(
                    pending.table, pending.rows[i].value,
                    pending.table.uniqueKeyColumns[keyIndex], keyIndex,
                    &existingKey, &hasExistingKey, error))
              return false;
            if (hasExistingKey && existingKey == newKey)
              {
                setError(error, "duplicate local-lite unique key");
                return false;
              }
          }
      }

    LocalLiteRow row;
    row.rowId = pending.nextRowId++;
    row.value = encodedRow;
    if (!recordRowWriteLocked(pending.table, row, error))
      return false;
    pending.rows.push_back(row);
    if (rowId)
      *rowId = row.rowId;
    return true;
  }

  bool insertRows(LocalLiteRocksDBStore *store,
                  const LocalLiteTableDef &table,
                  const std::vector<std::string> &encodedRows,
                  std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      {
        setError(error, "local-lite bulk insert requires an active transaction");
        return false;
      }
    if (encodedRows.empty())
      return true;

    PendingTable &pending = pendingTables_[tableKey(table)];
    if (!pending.initialized)
      {
        LocalLiteTableDef loaded;
        if (!store->loadTable(table.catalog, table.schema, table.name,
                              &loaded, error))
          return false;
        pending.table = loaded;
        pending.nextRowId = loaded.nextRowId;
        pending.initialized = true;
      }

    // Do not perform the per-row scan used by insertRow().  The unified
    // commit path already builds one RecordMap and rejects duplicate primary,
    // unique, and secondary-index keys for the whole batch.  This keeps a
    // 100K-row native load O(rows) in the staging path instead of repeatedly
    // comparing every new row with all prior pending rows.
    pending.rows.reserve(pending.rows.size() + encodedRows.size());
    for (size_t i = 0; i < encodedRows.size(); i++)
      {
        LocalLiteRow row;
        row.rowId = pending.nextRowId++;
        row.value = encodedRows[i];
        if (!recordRowWriteLocked(pending.table, row, error))
          return false;
        pending.rows.push_back(row);
      }
    return true;
  }

  bool updateRows(LocalLiteRocksDBStore *store,
                  const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRowMutation> &mutations,
                  std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return store->updateRows(table, mutations, error);

    PendingTable &pending = pendingTables_[tableKey(table)];
    if (!pending.initialized)
      {
        LocalLiteTableDef loaded;
        if (!store->loadTable(table.catalog, table.schema, table.name,
                              &loaded, error))
          return false;
        pending.table = loaded;
        pending.nextRowId = loaded.nextRowId;
        pending.initialized = true;
      }

    for (size_t i = 0; i < mutations.size(); i++)
      {
        LocalLiteRow after;
        after.rowId = mutations[i].before.rowId;
        after.value = mutations[i].after;
        if (!recordRowWriteLocked(pending.table, mutations[i].before, error))
          {
            if (error) *error = "record OCC before-image keys: " + *error;
            return false;
          }
        if (!recordRowWriteLocked(pending.table, after, error))
          {
            if (error) *error = "record OCC after-image keys: " + *error;
            return false;
          }
        bool merged = false;
        for (size_t rowIndex = 0; rowIndex < pending.rows.size(); rowIndex++)
          if (pending.rows[rowIndex].rowId == mutations[i].before.rowId &&
              pending.rows[rowIndex].value == mutations[i].before.value)
            {
              pending.rows[rowIndex].value = mutations[i].after;
              merged = true;
              break;
            }
        if (merged)
          continue;

        for (size_t updateIndex = 0;
             updateIndex < pending.updates.size(); updateIndex++)
          if (pending.updates[updateIndex].before.rowId ==
                mutations[i].before.rowId &&
              pending.updates[updateIndex].after == mutations[i].before.value)
            {
              pending.updates[updateIndex].after = mutations[i].after;
              merged = true;
              break;
            }
        if (!merged)
          pending.updates.push_back(mutations[i]);
      }
    return true;
  }

  bool deleteRows(LocalLiteRocksDBStore *store,
                  const LocalLiteTableDef &table,
                  const std::vector<LocalLiteRow> &rows,
                  std::string *error)
  {
    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return store->deleteRows(table, rows, error);

    PendingTable &pending = pendingTables_[tableKey(table)];
    if (!pending.initialized)
      {
        LocalLiteTableDef loaded;
        if (!store->loadTable(table.catalog, table.schema, table.name,
                              &loaded, error))
          return false;
        pending.table = loaded;
        pending.nextRowId = loaded.nextRowId;
        pending.initialized = true;
      }

    for (size_t i = 0; i < rows.size(); i++)
      {
        if (!recordRowWriteLocked(pending.table, rows[i], error))
          return false;
        bool merged = false;
        for (size_t rowIndex = 0; rowIndex < pending.rows.size(); rowIndex++)
          if (pending.rows[rowIndex].rowId == rows[i].rowId &&
              pending.rows[rowIndex].value == rows[i].value)
            {
              pending.rows.erase(pending.rows.begin() + rowIndex);
              merged = true;
              break;
            }
        if (merged)
          continue;

        for (size_t updateIndex = 0;
             updateIndex < pending.updates.size(); updateIndex++)
          if (pending.updates[updateIndex].before.rowId == rows[i].rowId &&
              pending.updates[updateIndex].after == rows[i].value)
            {
              pending.deletes.push_back(
                  pending.updates[updateIndex].before);
              pending.updates.erase(pending.updates.begin() + updateIndex);
              merged = true;
              break;
            }
        if (merged)
          continue;

        for (size_t deleteIndex = 0;
             deleteIndex < pending.deletes.size(); deleteIndex++)
          if (pending.deletes[deleteIndex].rowId == rows[i].rowId &&
              pending.deletes[deleteIndex].value == rows[i].value)
            {
              setError(error,
                       "local-lite delete selected a row more than once");
              return false;
            }
        pending.deletes.push_back(rows[i]);
      }
    return true;
  }

  bool scanRows(LocalLiteRocksDBStore *store,
                const LocalLiteTableDef &table,
                const void *statementOwner,
                uint64_t statementExecutionId,
                std::vector<LocalLiteRow> *rows,
                std::string *error)
  {
    const void *readOwner = statementOwner;
    uint64_t readExecutionId = statementExecutionId;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          readOwner = this;
          readExecutionId = localTxnId_;
          recordFullScanLocked(table);
        }
    }

    if (!store->scanRows(table, readOwner, readExecutionId,
                         rows, error))
      return false;

    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return true;

    PendingMap::iterator it = pendingTables_.find(tableKey(table));
    if (it == pendingTables_.end())
      return true;

    for (size_t updateIndex = 0;
         updateIndex < it->second.updates.size(); updateIndex++)
      {
        const LocalLiteRowMutation &mutation = it->second.updates[updateIndex];
        for (size_t rowIndex = 0; rowIndex < rows->size(); rowIndex++)
          if ((*rows)[rowIndex].rowId == mutation.before.rowId &&
              (*rows)[rowIndex].value == mutation.before.value)
            {
              (*rows)[rowIndex].value = mutation.after;
              break;
            }
      }
    for (size_t deleteIndex = 0;
         deleteIndex < it->second.deletes.size(); deleteIndex++)
      for (size_t rowIndex = 0; rowIndex < rows->size(); rowIndex++)
        if ((*rows)[rowIndex].rowId ==
              it->second.deletes[deleteIndex].rowId &&
            (*rows)[rowIndex].value ==
              it->second.deletes[deleteIndex].value)
          {
            rows->erase(rows->begin() + rowIndex);
            break;
          }
    for (size_t i = 0; i < it->second.rows.size(); i++)
      rows->push_back(it->second.rows[i]);
    return true;
  }

  bool scanIndexPrefix(LocalLiteRocksDBStore *store,
                       const LocalLiteTableDef &table,
                       const std::string &physicalPrefix,
                       const void *statementOwner,
                       uint64_t statementExecutionId,
                       std::vector<LocalLiteRow> *rows,
                       std::string *error)
  {
    std::string endKey = physicalPrefix;
    size_t pos = endKey.size();
    while (pos > 0 &&
           static_cast<unsigned char>(endKey[pos - 1]) == 0xff)
      pos--;
    if (pos == 0)
      {
        setError(error, "local-lite index prefix has no upper bound");
        return false;
      }
    endKey.resize(pos);
    endKey[pos - 1] = static_cast<char>(
        static_cast<unsigned char>(endKey[pos - 1]) + 1);
    return scanIndexRange(store, table, physicalPrefix, endKey,
                          statementOwner, statementExecutionId, rows, error);
  }

  bool scanIndexRange(LocalLiteRocksDBStore *store,
                      const LocalLiteTableDef &table,
                      const std::string &startKey,
                      const std::string &endKey,
                      const void *statementOwner,
                      uint64_t statementExecutionId,
                      std::vector<LocalLiteRow> *rows,
                      std::string *error)
  {
    return scanPhysicalRange(store, table, startKey, endKey, false,
                             statementOwner, statementExecutionId,
                             rows, error);
  }

  bool scanPrimaryRange(LocalLiteRocksDBStore *store,
                        const LocalLiteTableDef &table,
                        const std::string &startKey,
                        const std::string &endKey,
                        const void *statementOwner,
                        uint64_t statementExecutionId,
                        std::vector<LocalLiteRow> *rows,
                        std::string *error)
  {
    return scanPhysicalRange(store, table, startKey, endKey, true,
                             statementOwner, statementExecutionId,
                             rows, error);
  }

  bool scanPhysicalRange(LocalLiteRocksDBStore *store,
                         const LocalLiteTableDef &table,
                         const std::string &startKey,
                         const std::string &endKey,
                         bool primaryRange,
                         const void *statementOwner,
                         uint64_t statementExecutionId,
                         std::vector<LocalLiteRow> *rows,
                         std::string *error)
  {
    const void *readOwner = statementOwner;
    uint64_t readExecutionId = statementExecutionId;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          readOwner = this;
          readExecutionId = localTxnId_;
        }
    }
    const bool scanned = primaryRange
        ? store->scanPrimaryRange(table, startKey, endKey, readOwner,
                                  readExecutionId, rows, error)
        : store->scanIndexRange(table, startKey, endKey, readOwner,
                                readExecutionId, rows, error);
    if (!scanned)
      return false;

    LocalLiteMutexGuard guard(&mutex_);
    if (!active_)
      return true;

    OccReadRange range;
    range.objectUid = table.objectUid;
    range.start = startKey;
    range.end = endKey;
    readRanges_.insert(range);
    if (primaryRange)
      primaryRangeReads_++;
    else
      indexRangeReads_++;

    std::map<std::string, LocalLiteRow> visible;
    for (size_t i = 0; i < rows->size(); i++)
      {
        std::string key;
        if (!rowStorageKey(table, (*rows)[i], &key, error))
          return false;
        visible[key] = (*rows)[i];
        recordReadPointLocked(table, key);
      }

    PendingMap::iterator pending = pendingTables_.find(tableKey(table));
    if (pending != pendingTables_.end())
      {
        for (size_t i = 0; i < pending->second.deletes.size(); i++)
          {
            std::string key;
            if (!rowStorageKey(pending->second.table,
                               pending->second.deletes[i], &key, error))
              return false;
            visible.erase(key);
          }
        for (size_t i = 0; i < pending->second.updates.size(); i++)
          {
            LocalLiteRow after;
            after.rowId = pending->second.updates[i].before.rowId;
            after.value = pending->second.updates[i].after;
            std::string beforeKey;
            if (!rowStorageKey(pending->second.table,
                               pending->second.updates[i].before,
                               &beforeKey, error))
              return false;
            visible.erase(beforeKey);
            bool matches = false;
            if (primaryRange)
              {
                std::string key;
                if (!rowStorageKey(pending->second.table, after, &key, error))
                  return false;
                matches = key >= startKey && key < endKey;
              }
            else if (!rowMatchesIndexRange(pending->second.table, after,
                                           startKey, endKey, &matches, error))
              return false;
            if (matches)
              {
                std::string afterKey;
                if (!rowStorageKey(pending->second.table, after,
                                   &afterKey, error))
                  return false;
                visible[afterKey] = after;
              }
          }
        for (size_t i = 0; i < pending->second.rows.size(); i++)
          {
            bool matches = false;
            if (primaryRange)
              {
                std::string key;
                if (!rowStorageKey(pending->second.table,
                                   pending->second.rows[i], &key, error))
                  return false;
                matches = key >= startKey && key < endKey;
              }
            else if (!rowMatchesIndexRange(pending->second.table,
                                           pending->second.rows[i],
                                           startKey, endKey, &matches, error))
              return false;
            if (matches)
              {
                std::string key;
                if (!rowStorageKey(pending->second.table,
                                   pending->second.rows[i], &key, error))
                  return false;
                visible[key] = pending->second.rows[i];
              }
          }
      }

    rows->clear();
    for (std::map<std::string, LocalLiteRow>::const_iterator row =
           visible.begin(); row != visible.end(); ++row)
      rows->push_back(row->second);
    return true;
  }

  bool getRowByKey(LocalLiteRocksDBStore *store,
                   const LocalLiteTableDef &table,
                   const std::string &storageKey,
                   const void *statementOwner,
                   uint64_t statementExecutionId,
                   LocalLiteRow *row,
                   bool *found,
                   std::string *error)
  {
    if (found)
      *found = false;
    if (!row || !found)
      {
        setError(error, "missing local-lite get row output");
        return false;
      }

    const void *readOwner = statementOwner;
    uint64_t readExecutionId = statementExecutionId;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_)
        {
          readOwner = this;
          readExecutionId = localTxnId_;
          recordReadPointLocked(table, storageKey);
          PendingMap::iterator it = pendingTables_.find(tableKey(table));
          if (it != pendingTables_.end())
            {
              for (size_t i = 0; i < it->second.deletes.size(); i++)
                {
                  bool matchesDelete = false;
                  if (!pendingRowMatchesKey(it->second.table,
                                            it->second.deletes[i],
                                            storageKey,
                                            &matchesDelete,
                                            error))
                    return false;
                  if (matchesDelete)
                    return true;
                }
              for (size_t i = it->second.updates.size(); i > 0; i--)
                {
                  const LocalLiteRowMutation &mutation =
                    it->second.updates[i - 1];
                  LocalLiteRow updatedRow;
                  updatedRow.rowId = mutation.before.rowId;
                  updatedRow.value = mutation.after;
                  bool matchesAfter = false;
                  if (!pendingRowMatchesKey(it->second.table,
                                            updatedRow,
                                            storageKey,
                                            &matchesAfter,
                                            error))
                    return false;
                  if (matchesAfter)
                    {
                      row->rowId = mutation.before.rowId;
                      row->value = mutation.after;
                      *found = true;
                      return true;
                    }

                  bool matchesBefore = false;
                  if (!pendingRowMatchesKey(it->second.table,
                                            mutation.before,
                                            storageKey,
                                            &matchesBefore,
                                            error))
                    return false;
                  if (matchesBefore)
                    return true;
                }
              for (size_t i = 0; i < it->second.rows.size(); i++)
                {
                  if (!pendingRowMatchesKey(it->second.table,
                                            it->second.rows[i],
                                            storageKey,
                                            found,
                                            error))
                    return false;
                  if (*found)
                    {
                      *row = it->second.rows[i];
                      return true;
                    }
                }
            }
        }
    }

    if (!store->getRowByKey(table, storageKey, readOwner,
                            readExecutionId, row, found, error))
      return false;
    {
      LocalLiteMutexGuard guard(&mutex_);
      if (active_ && !*found)
        missingPointReads_++;
    }
    return true;
  }

private:
  void endOcc()
  {
    LocalLiteMutexGuard publication(
        LocalLiteStorageManager::instance().mutex());
    localLiteOccCoordinator.end(this);
  }

  bool rowStorageKey(const LocalLiteTableDef &table,
                     const LocalLiteRow &row,
                     std::string *key,
                     std::string *error)
  {
    key->clear();
    if (table.primaryKeyColumns.empty())
      {
        appendUint64(*key, row.rowId);
        return true;
      }
    return LocalLiteBuildPrimaryKey(table, row.value, key, error);
  }

  bool rowMatchesIndexRange(const LocalLiteTableDef &table,
                            const LocalLiteRow &row,
                            const std::string &startKey,
                            const std::string &endKey,
                            bool *matches,
                            std::string *error)
  {
    *matches = false;
    std::string rowKey;
    if (!rowStorageKey(table, row, &rowKey, error))
      return false;
    std::vector<LocalLitePhysicalIndexEntry> entries;
    if (!buildSecondaryIndexEntries(table, row.value, rowKey,
                                    &entries, error))
      return false;
    for (size_t i = 0; i < entries.size(); i++)
      if (entries[i].key >= startKey && entries[i].key < endKey)
        {
          *matches = true;
          break;
        }
    return true;
  }

  void recordReadPointLocked(const LocalLiteTableDef &table,
                             const std::string &key)
  {
    OccReadRange range;
    range.objectUid = table.objectUid;
    range.start = key;
    range.end = key;
    readRanges_.insert(range);
    pointReads_++;
  }

  void recordFullScanLocked(const LocalLiteTableDef &table)
  {
    if (getenv("TRAF_LOCAL_LITE_TRACE_OCC"))
      fprintf(stderr,
              "LOCAL_LITE_OCC_READ_FULL owner=%p object_uid=%llu table=%s.%s.%s\n",
              this, static_cast<unsigned long long>(table.objectUid),
              table.catalog.c_str(), table.schema.c_str(),
              table.name.c_str());
    OccReadRange range;
    range.objectUid = table.objectUid;
    range.full = true;
    readRanges_.insert(range);
    fullScans_++;
  }

  void recordWriteKeyLocked(const LocalLiteTableDef &table,
                            const std::string &key)
  {
    writeKeys_.insert(std::make_pair(table.objectUid, key));
    // Trafodion adds every write to its scan/read set so two blind writers
    // still conflict under optimistic validation.
    OccReadRange range;
    range.objectUid = table.objectUid;
    range.start = key;
    range.end = key;
    readRanges_.insert(range);
  }

  bool recordRowWriteLocked(const LocalLiteTableDef &table,
                            const LocalLiteRow &row,
                            std::string *error)
  {
    std::string rowKey;
    if (table.primaryKeyColumns.empty())
      appendUint64(rowKey, row.rowId);
    else if (!LocalLiteBuildPrimaryKey(table, row.value, &rowKey, error))
      return false;
    recordWriteKeyLocked(table, rowKey);

    for (size_t keyIndex = 0;
         keyIndex < table.uniqueKeyColumns.size(); keyIndex++)
      {
        std::string uniqueKey;
        bool hasKey = false;
        if (!LocalLiteBuildUniqueKey(table, row.value,
                                     table.uniqueKeyColumns[keyIndex],
                                     keyIndex, &uniqueKey, &hasKey, error))
          return false;
        if (hasKey)
          recordWriteKeyLocked(table, uniqueKey);
      }

    std::vector<LocalLitePhysicalIndexEntry> entries;
    if (!buildSecondaryIndexEntries(table, row.value, rowKey,
                                    &entries, error))
      return false;
    for (size_t i = 0; i < entries.size(); i++)
      recordWriteKeyLocked(table, entries[i].key);
    return true;
  }

  struct PendingTable
  {
    PendingTable() : initialized(false), nextRowId(0) {}

    bool initialized;
    LocalLiteTableDef table;
    uint64_t nextRowId;
    std::vector<LocalLiteRow> rows;
    std::vector<LocalLiteRowMutation> updates;
    std::vector<LocalLiteRow> deletes;
  };
  typedef std::map<std::string, PendingTable> PendingMap;

  bool validatePendingReferentialIntegrity(const PendingMap &pending,
                                            std::string *error)
  {
    std::vector<LocalLiteTableDef> tables;
    if (!transactionStore_.listTables("", "", &tables, error))
      return false;
    std::map<std::string, size_t> tableIndexes;
    for (size_t t = 0; t < tables.size(); t++)
      tableIndexes[tableKey(tables[t])] = t;

    // Only changed child rows can introduce a missing reference. Resolve the
    // common primary-key reference as one point lookup, overlaid with pending
    // parent changes, instead of reconstructing every table on every commit.
    for (size_t child = 0; child < tables.size(); child++)
      for (size_t r = 0; r < tables[child].riConstraints.size(); r++)
        {
          const LocalLiteRIDef &ri = tables[child].riConstraints[r];
          std::map<std::string, size_t>::const_iterator parent =
              tableIndexes.find(tableKey(ri.referencedCatalog,
                                         ri.referencedSchema,
                                         ri.referencedTable));
          if (parent == tableIndexes.end())
            {
              setError(error, "referenced local-lite table is missing for " +
                              ri.name);
              return false;
            }
          const LocalLiteTableDef &parentTable = tables[parent->second];
          PendingMap::const_iterator childChanges =
              pending.find(tableKey(tables[child]));
          PendingMap::const_iterator parentChanges =
              pending.find(tableKey(parentTable));

          if (childChanges != pending.end())
            {
              for (size_t row = 0; row < childChanges->second.rows.size(); row++)
                if (!validateChangedChildReference(
                        tables[child], childChanges->second.rows[row].value,
                        parentTable, ri, parentChanges, pending.end(), error))
                  return false;
              for (size_t row = 0;
                   row < childChanges->second.updates.size(); row++)
                if (!validateChangedChildReference(
                        tables[child],
                        childChanges->second.updates[row].after,
                        parentTable, ri, parentChanges, pending.end(), error))
                  return false;
            }

          if (parentChanges != pending.end() &&
              !validateRemovedParentReferences(
                  tables[child], parentTable, ri, childChanges,
                  parentChanges, pending.end(), error))
            return false;
        }
    return true;
  }

  bool buildReferencedPrimaryKey(const LocalLiteTableDef &parent,
                                 const std::vector<size_t> &columns,
                                 const std::string &constraintKey,
                                 std::string *storageKey)
  {
    if (parent.primaryKeyColumns != columns || constraintKey.size() < 8)
      return false;
    size_t offset = 0;
    const uint64_t count = readUint64(constraintKey, &offset);
    if (count != columns.size())
      return false;
    storageKey->clear();
    storageKey->push_back('P');
    appendUint64(*storageKey, count);
    for (size_t i = 0; i < columns.size(); i++)
      {
        if (offset + 8 > constraintKey.size())
          return false;
        const uint64_t length = readUint64(constraintKey, &offset);
        if (length > constraintKey.size() - offset)
          return false;
        appendUint64(*storageKey, static_cast<uint64_t>(columns[i]));
        appendUint64(*storageKey, length);
        storageKey->append(constraintKey, offset,
                           static_cast<size_t>(length));
        offset += static_cast<size_t>(length);
      }
    return offset == constraintKey.size();
  }

  bool rowConstraintKey(const LocalLiteTableDef &table,
                        const std::string &row,
                        const std::vector<size_t> &columns,
                        const std::string &expected,
                        bool *matches,
                        std::string *error)
  {
    std::string key;
    bool hasKey = false;
    if (!LocalLiteBuildConstraintKey(table, row, columns, &key, &hasKey,
                                     error))
      return false;
    *matches = hasKey && key == expected;
    return true;
  }

  bool referencedKeyExists(
      const LocalLiteTableDef &parent, const LocalLiteRIDef &ri,
      const std::string &key, PendingMap::const_iterator parentChanges,
      PendingMap::const_iterator pendingEnd, bool *exists,
      std::string *error)
  {
    *exists = false;
    bool removedFromStoredImage = false;
    if (parentChanges != pendingEnd)
      {
        for (size_t i = 0; i < parentChanges->second.rows.size(); i++)
          {
            bool matches = false;
            if (!rowConstraintKey(parent,
                                  parentChanges->second.rows[i].value,
                                  ri.referencedColumns, key, &matches, error))
              return false;
            if (matches) { *exists = true; return true; }
          }
        for (size_t i = 0; i < parentChanges->second.updates.size(); i++)
          {
            bool matchesAfter = false;
            bool matchesBefore = false;
            if (!rowConstraintKey(parent,
                                  parentChanges->second.updates[i].after,
                                  ri.referencedColumns, key, &matchesAfter,
                                  error) ||
                !rowConstraintKey(
                    parent, parentChanges->second.updates[i].before.value,
                    ri.referencedColumns, key, &matchesBefore, error))
              return false;
            if (matchesAfter) { *exists = true; return true; }
            removedFromStoredImage = removedFromStoredImage || matchesBefore;
          }
        for (size_t i = 0; i < parentChanges->second.deletes.size(); i++)
          {
            bool matches = false;
            if (!rowConstraintKey(parent,
                                  parentChanges->second.deletes[i].value,
                                  ri.referencedColumns, key, &matches, error))
              return false;
            removedFromStoredImage = removedFromStoredImage || matches;
          }
      }
    if (removedFromStoredImage)
      return true;

    std::string storageKey;
    if (buildReferencedPrimaryKey(parent, ri.referencedColumns, key,
                                  &storageKey))
      {
        LocalLiteRow row;
        return transactionStore_.getRowByKey(parent, storageKey, &row,
                                              exists, error);
      }

    std::vector<LocalLiteRow> rows;
    if (!transactionStore_.scanRows(parent, &rows, error))
      return false;
    for (size_t i = 0; i < rows.size(); i++)
      {
        bool matches = false;
        if (!rowConstraintKey(parent, rows[i].value, ri.referencedColumns,
                              key, &matches, error))
          return false;
        if (matches) { *exists = true; break; }
      }
    return true;
  }

  bool validateChangedChildReference(
      const LocalLiteTableDef &child, const std::string &row,
      const LocalLiteTableDef &parent, const LocalLiteRIDef &ri,
      PendingMap::const_iterator parentChanges,
      PendingMap::const_iterator pendingEnd, std::string *error)
  {
    std::string key;
    bool hasKey = false;
    if (!LocalLiteBuildConstraintKey(child, row, ri.referencingColumns,
                                     &key, &hasKey, error))
      return false;
    if (!hasKey)
      return true;
    bool exists = false;
    if (!referencedKeyExists(parent, ri, key, parentChanges, pendingEnd,
                             &exists, error))
      return false;
    if (!exists)
      {
        setError(error, "referential integrity constraint " + ri.name +
                        " is violated");
        return false;
      }
    return true;
  }

  bool validateRemovedParentReferences(
      const LocalLiteTableDef &child, const LocalLiteTableDef &parent,
      const LocalLiteRIDef &ri, PendingMap::const_iterator childChanges,
      PendingMap::const_iterator parentChanges,
      PendingMap::const_iterator pendingEnd, std::string *error)
  {
    std::set<std::string> removed;
    for (size_t i = 0; i < parentChanges->second.deletes.size(); i++)
      {
        std::string key;
        bool hasKey = false;
        if (!LocalLiteBuildConstraintKey(
                parent, parentChanges->second.deletes[i].value,
                ri.referencedColumns, &key, &hasKey, error))
          return false;
        if (hasKey) removed.insert(key);
      }
    for (size_t i = 0; i < parentChanges->second.updates.size(); i++)
      {
        std::string beforeKey;
        std::string afterKey;
        bool hasBefore = false;
        bool hasAfter = false;
        if (!LocalLiteBuildConstraintKey(
                parent, parentChanges->second.updates[i].before.value,
                ri.referencedColumns, &beforeKey, &hasBefore, error) ||
            !LocalLiteBuildConstraintKey(
                parent, parentChanges->second.updates[i].after,
                ri.referencedColumns, &afterKey, &hasAfter, error))
          return false;
        if (hasBefore && (!hasAfter || beforeKey != afterKey))
          removed.insert(beforeKey);
      }
    for (size_t i = 0; i < parentChanges->second.rows.size(); i++)
      {
        std::string key;
        bool hasKey = false;
        if (!LocalLiteBuildConstraintKey(
                parent, parentChanges->second.rows[i].value,
                ri.referencedColumns, &key, &hasKey, error))
          return false;
        if (hasKey) removed.erase(key);
      }
    for (size_t i = 0; i < parentChanges->second.updates.size(); i++)
      {
        std::string key;
        bool hasKey = false;
        if (!LocalLiteBuildConstraintKey(
                parent, parentChanges->second.updates[i].after,
                ri.referencedColumns, &key, &hasKey, error))
          return false;
        if (hasKey) removed.erase(key);
      }
    if (removed.empty())
      return true;

    std::vector<LocalLiteRow> rows;
    if (!transactionStore_.scanRows(child, &rows, error))
      return false;
    if (childChanges != pendingEnd)
      {
        for (size_t i = 0; i < childChanges->second.updates.size(); i++)
          for (size_t row = 0; row < rows.size(); row++)
            if (rows[row].rowId ==
                childChanges->second.updates[i].before.rowId)
              rows[row].value = childChanges->second.updates[i].after;
        for (size_t i = 0; i < childChanges->second.deletes.size(); i++)
          for (size_t row = 0; row < rows.size(); row++)
            if (rows[row].rowId == childChanges->second.deletes[i].rowId)
              { rows.erase(rows.begin() + row); break; }
        rows.insert(rows.end(), childChanges->second.rows.begin(),
                    childChanges->second.rows.end());
      }
    for (size_t i = 0; i < rows.size(); i++)
      {
        std::string key;
        bool hasKey = false;
        if (!LocalLiteBuildConstraintKey(child, rows[i].value,
                                         ri.referencingColumns, &key,
                                         &hasKey, error))
          return false;
        if (hasKey && removed.find(key) != removed.end())
          {
            setError(error, "referential integrity constraint " + ri.name +
                            " is violated");
            return false;
          }
      }
    return true;
  }

  bool pendingRowMatchesKey(const LocalLiteTableDef &table,
                            const LocalLiteRow &row,
                            const std::string &storageKey,
                            bool *matches,
                            std::string *error)
  {
    *matches = false;
    if (storageKey.size() > 0 && storageKey[0] == 'P')
      {
        std::string key;
        if (!LocalLiteBuildPrimaryKey(table, row.value, &key, error))
          return false;
        *matches = (key == storageKey);
        return true;
      }

    if (storageKey.size() > 0 && storageKey[0] == 'U')
      {
        for (size_t keyIndex = 0;
             keyIndex < table.uniqueKeyColumns.size(); keyIndex++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildUniqueKey(table, row.value,
                                         table.uniqueKeyColumns[keyIndex],
                                         keyIndex, &key, &hasKey, error))
              return false;
            if (hasKey && key == storageKey)
              {
                *matches = true;
                return true;
              }
          }
        return true;
      }

    if (storageKey.size() == 8)
      {
        std::string key;
        appendUint64(key, row.rowId);
        *matches = (key == storageKey);
      }
    return true;
  }

  bool matchesExecutorTxnId(int64_t executorTxnId) const
  {
    return executorTxnId == LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID ||
           executorTxnId_ == LocalLiteTxnManager::INVALID_EXECUTOR_TXN_ID ||
           executorTxnId == executorTxnId_;
  }

  LocalLiteTxnContext(const LocalLiteTxnContext &);
  LocalLiteTxnContext &operator=(const LocalLiteTxnContext &);

  bool active_;
  uint64_t nextLocalTxnId_;
  uint64_t localTxnId_;
  uint64_t beginSequence_;
  OccReadSet readRanges_;
  OccWriteSet writeKeys_;
  uint64_t pointReads_;
  uint64_t missingPointReads_;
  uint64_t fullScans_;
  uint64_t primaryRangeReads_;
  uint64_t indexRangeReads_;
  int64_t executorTxnId_;
  PendingMap pendingTables_;
  std::map<const void *, uint64_t> statementExecutions_;
  LocalLiteRocksDBStore transactionStore_;
  pthread_mutex_t mutex_;
};

LocalLiteRocksDBStore::LocalLiteRocksDBStore()
  : opened_(false)
{
}

LocalLiteRocksDBStore::~LocalLiteRocksDBStore()
{
  close();
}

std::string LocalLiteRocksDBStore::defaultRoot()
{
  const char *overrideDir = getenv("TRAF_LOCAL_STORE_DIR");
  if (overrideDir && overrideDir[0])
    return overrideDir;

  const char *trafVar = getenv("TRAF_VAR");
  if (trafVar && trafVar[0])
    return std::string(trafVar) + "/localstore/rocksdb";

  return "./localstore/rocksdb";
}

std::string LocalLiteRocksDBStore::catalogPath()
{
  return defaultRoot() + "/catalog";
}

std::string LocalLiteRocksDBStore::dataRoot()
{
  return defaultRoot() + "/data";
}

std::string LocalLiteRocksDBStore::tablePath(const LocalLiteTableDef &table)
{
  char uid[32];
  snprintf(uid, sizeof(uid), "%020llu",
           static_cast<unsigned long long>(table.objectUid));
  return dataRoot() + "/" + escapeName(table.catalog) + "/" +
         escapeName(table.schema) + "/" + uid;
}

bool LocalLiteRocksDBStore::open(std::string *error)
{
  if (opened_)
    return true;

  if (!LocalLiteStorageManager::instance().acquire(error))
    return false;

  opened_ = true;
  return true;
}

void LocalLiteRocksDBStore::close()
{
  if (!opened_)
    return;

  LocalLiteStorageManager::instance().release();
  opened_ = false;
}

bool LocalLiteRocksDBStore::createTable(const LocalLiteTableDef &table,
                                         std::string *error,
                                         const std::string &owner,
                                         LocalLiteTxnContext *txnContext)
{
  if (!open(error))
    return false;

  bool exists = false;
  if (!tableExists(table.catalog, table.schema, table.name, &exists, error))
    return false;
  if (exists)
    {
      setError(error, "local-lite table already exists: " +
              table.catalog + "." + table.schema + "." + table.name);
      return false;
    }

  std::string collidingIndexKey = indexKey(table.catalog, table.schema,
                                           table.name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions, collidingIndexKey.data(),
                            collidingIndexKey.size(), &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite object metadata", error))
    return false;
  if (value)
    {
      rocksdb_free(value);
      setError(error, "local-lite object already exists: " + table.catalog +
                      "." + table.schema + "." + table.name);
      return false;
    }

  if (!LocalLiteUnifiedRocksDBActive() &&
      !mkdirs(parentDir(tablePath(table)), error))
    return false;

  LocalLiteStorageManager::instance().closeTable(tablePath(table));
  rocksdb_options_t *tableOptions = rocksdb_options_create();
  rocksdb_options_set_create_if_missing(tableOptions, 1);
  err = NULL;
  rocksdb_t *tableDb = rocksdb_open(tableOptions, tablePath(table).c_str(), &err);
  rocksdb_options_destroy(tableOptions);
  if (!checkRocksError(err, "create RocksDB table " + tablePath(table), error))
    return false;
  rocksdb_close(tableDb);

  LocalLiteTableDef copy = table;
  if (copy.nextRowId == 0)
    copy.nextRowId = 1;

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string encoded = encodeTable(copy);
  std::string key = tableKey(copy.catalog, copy.schema, copy.name);
  std::string uid = uidKey(copy.objectUid);
  rocksdb_writebatch_put(batch, key.data(), key.size(), encoded.data(), encoded.size());
  rocksdb_writebatch_put(batch, uid.data(), uid.size(), key.data(), key.size());
  addLocalLiteMetadataForTable(batch, copy);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "write local-lite table metadata", error))
    return false;
  if (!owner.empty() &&
      !setTableOwner(copy.catalog, copy.schema, copy.name, owner, error))
    return false;
  publishOccTableChange(txnContext, copy.objectUid);
  return LocalLiteTxnManager::refreshDDLSequenceForExecutor(
      txnContext, LocalLiteTxnManager::currentExecutorTxnId(txnContext), error);
}

bool LocalLiteRocksDBStore::schemaExists(const std::string &catalog,
                                         const std::string &schema,
                                         bool *exists,
                                         std::string *error)
{
  if (!exists || !open(error))
    return false;
  std::string key = schemaKey(catalog, schema);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions, key.data(), key.size(),
                            &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite schema metadata", error))
    return false;
  *exists = value != NULL;
  if (value)
    rocksdb_free(value);
  return true;
}

bool LocalLiteRocksDBStore::createSchema(const std::string &catalog,
                                         const std::string &schema,
                                         bool ifNotExists,
                                         std::string *error)
{
  bool exists = false;
  if (catalog.empty() || schema.empty() ||
      !schemaExists(catalog, schema, &exists, error))
    return false;
  if (exists)
    {
      if (ifNotExists)
        return true;
      setError(error, "local-lite schema already exists: " + catalog + "." +
              schema);
      return false;
    }
  std::string key = schemaKey(catalog, schema);
  static const char value[] = "LLSC1";
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
              key.data(), key.size(), value, sizeof(value) - 1, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "write local-lite schema metadata", error);
}

bool LocalLiteRocksDBStore::resolveSynonym(
    const std::string &catalog, const std::string &schema,
    const std::string &name, std::string *targetCatalog,
    std::string *targetSchema, std::string *targetName, bool *found,
    std::string *error)
{
  if (!found || !targetCatalog || !targetSchema || !targetName || !open(error))
    return false;
  std::string key = synonymKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions, key.data(), key.size(), &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite synonym metadata", error))
    return false;
  *found = value != NULL;
  if (!value)
    return true;
  std::string encoded(value, valueLen);
  rocksdb_free(value);
  size_t first = encoded.find('\n');
  size_t second = first == std::string::npos
      ? first : encoded.find('\n', first + 1);
  if (first == std::string::npos || second == std::string::npos)
    {
      setError(error, "invalid local-lite synonym metadata");
      return false;
    }
  *targetCatalog = encoded.substr(0, first);
  *targetSchema = encoded.substr(first + 1, second - first - 1);
  *targetName = encoded.substr(second + 1);
  return !targetCatalog->empty() && !targetSchema->empty() && !targetName->empty();
}

bool LocalLiteRocksDBStore::createSynonym(
    const std::string &catalog, const std::string &schema,
    const std::string &name, const std::string &targetCatalog,
    const std::string &targetSchema, const std::string &targetName,
    std::string *error)
{
  bool targetExists = false;
  bool synonymExists = false;
  std::string tc, ts, tn;
  if (!tableExists(targetCatalog, targetSchema, targetName, &targetExists, error) ||
      !resolveSynonym(catalog, schema, name, &tc, &ts, &tn,
                      &synonymExists, error))
    return false;
  if (!targetExists)
    {
      setError(error, "local-lite synonym target does not exist: " +
               targetCatalog + "." + targetSchema + "." + targetName);
      return false;
    }
  if (synonymExists)
    {
      setError(error, "local-lite synonym already exists: " + catalog + "." +
               schema + "." + name);
      return false;
    }
  std::string key = synonymKey(catalog, schema, name);
  std::string value = targetCatalog + "\n" + targetSchema + "\n" + targetName;
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
              key.data(), key.size(), value.data(), value.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "write local-lite synonym metadata", error);
}

bool LocalLiteRocksDBStore::dropSynonym(const std::string &catalog,
                                        const std::string &schema,
                                        const std::string &name,
                                        bool ifExists,
                                        std::string *error)
{
  std::string tc, ts, tn;
  bool exists = false;
  if (!resolveSynonym(catalog, schema, name, &tc, &ts, &tn, &exists, error))
    return false;
  if (!exists)
    {
      if (ifExists)
        return true;
      setError(error, "local-lite synonym does not exist: " + catalog + "." +
               schema + "." + name);
      return false;
    }
  std::string key = synonymKey(catalog, schema, name);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_delete(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
                 key.data(), key.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "delete local-lite synonym metadata", error);
}

bool LocalLiteRocksDBStore::loadSequence(
    const std::string &catalog, const std::string &schema,
    const std::string &name, LocalLiteSequenceDef *sequence, bool *found,
    std::string *error)
{
  if (!sequence || !found || !open(error))
    return false;
  std::string key = sequenceKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *rawValue = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                               readOptions, key.data(), key.size(),
                               &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite sequence metadata", error))
    return false;
  *found = rawValue != NULL;
  if (!rawValue)
    return true;
  std::string value(rawValue, valueLen);
  rocksdb_free(rawValue);
  return decodeSequence(value, catalog, schema, name, sequence, error);
}

bool LocalLiteRocksDBStore::createSequence(
    const LocalLiteSequenceDef &sequence, std::string *error)
{
  LocalLiteSequenceDef ignored;
  bool exists = false;
  if (!loadSequence(sequence.catalog, sequence.schema, sequence.name,
                    &ignored, &exists, error))
    return false;
  bool tableExistsFlag = false;
  if (!tableExists(sequence.catalog, sequence.schema, sequence.name,
                   &tableExistsFlag, error))
    return false;
  std::string tc, ts, tn;
  bool synonymExists = false;
  if (!resolveSynonym(sequence.catalog, sequence.schema, sequence.name,
                      &tc, &ts, &tn, &synonymExists, error))
    return false;
  std::string idxKey = indexKey(sequence.catalog, sequence.schema,
                                sequence.name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *idxValue = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                               readOptions, idxKey.data(), idxKey.size(),
                               &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite object metadata", error))
    return false;
  bool indexExists = idxValue != NULL;
  if (idxValue) rocksdb_free(idxValue);
  if (exists || tableExistsFlag || synonymExists || indexExists)
    {
      setError(error, "local-lite object already exists: " +
               sequence.catalog + "." + sequence.schema + "." +
               sequence.name);
      return false;
    }
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string key = sequenceKey(sequence.catalog, sequence.schema,
                                sequence.name);
  std::string uid = uidKey(sequence.objectUid);
  std::string value = encodeSequence(sequence);
  rocksdb_writebatch_put(batch, key.data(), key.size(),
                         value.data(), value.size());
  rocksdb_writebatch_put(batch, uid.data(), uid.size(), key.data(), key.size());
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  return checkRocksError(err, "write local-lite sequence metadata", error);
}

bool LocalLiteRocksDBStore::alterSequence(
    const LocalLiteSequenceDef &sequence, std::string *error)
{
  LocalLiteSequenceDef existing;
  bool found = false;
  if (!loadSequence(sequence.catalog, sequence.schema, sequence.name,
                    &existing, &found, error))
    return false;
  if (!found || existing.objectUid != sequence.objectUid)
    {
      setError(error, "local-lite sequence does not exist: " +
               sequence.catalog + "." + sequence.schema + "." +
               sequence.name);
      return false;
    }
  std::string key = sequenceKey(sequence.catalog, sequence.schema,
                                sequence.name);
  std::string value = encodeSequence(sequence);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
              key.data(), key.size(), value.data(), value.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "update local-lite sequence metadata", error);
}

bool LocalLiteRocksDBStore::dropSequence(
    const std::string &catalog, const std::string &schema,
    const std::string &name, bool ifExists, std::string *error)
{
  LocalLiteSequenceDef sequence;
  bool found = false;
  if (!loadSequence(catalog, schema, name, &sequence, &found, error))
    return false;
  if (!found)
    {
      if (ifExists) return true;
      setError(error, "local-lite sequence does not exist: " + catalog + "." +
               schema + "." + name);
      return false;
    }
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string key = sequenceKey(catalog, schema, name);
  std::string uid = uidKey(sequence.objectUid);
  rocksdb_writebatch_delete(batch, key.data(), key.size());
  rocksdb_writebatch_delete(batch, uid.data(), uid.size());
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  return checkRocksError(err, "delete local-lite sequence metadata", error);
}

bool LocalLiteRocksDBStore::allocateSequence(
    uint64_t objectUid, int64_t requestedCount, int64_t *nextValue,
    int64_t *endValue, LocalLiteTxnContext *txnContext, std::string *error)
{
  if (!nextValue || !endValue || !open(error))
    return false;
  if (!LocalLiteStorageManager::instance().allocateSequence(
          objectUid, requestedCount, nextValue, endValue, error))
    return false;
  // Sequence allocation is an autonomous catalog write used by the current
  // statement.  Treat its unified-DB sequence advance as this transaction's
  // own work so a following row commit is not rejected as an external writer.
  return LocalLiteTxnManager::refreshDDLSequenceForExecutor(
      txnContext, LocalLiteTxnManager::currentExecutorTxnId(txnContext), error);
}

bool LocalLiteRocksDBStore::listTriggers(
    const std::string &subjectCatalog, const std::string &subjectSchema,
    const std::string &subjectTable, int operation,
    std::vector<LocalLiteTriggerDef> *triggers, std::string *error)
{
  if (!triggers || !open(error))
    return false;
  triggers->clear();
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, "trigger|", 8); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t keyLen = 0, valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < 8 || memcmp(rawKey, "trigger|", 8) != 0)
        break;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      LocalLiteTriggerDef trigger;
      if (!decodeTrigger(std::string(rawValue, valueLen), &trigger, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
      if ((subjectCatalog.empty() || trigger.subjectCatalog == subjectCatalog) &&
          (subjectSchema.empty() || trigger.subjectSchema == subjectSchema) &&
          (subjectTable.empty() || trigger.subjectTable == subjectTable) &&
          (operation < 0 || trigger.operation == operation))
        triggers->push_back(trigger);
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  return checkRocksError(err, "scan local-lite trigger metadata", error);
}

bool LocalLiteRocksDBStore::createTrigger(
    const LocalLiteTriggerDef &trigger, std::string *error)
{
  bool subjectExists = false;
  if (!tableExists(trigger.subjectCatalog, trigger.subjectSchema,
                   trigger.subjectTable, &subjectExists, error))
    return false;
  if (!subjectExists)
    {
      setError(error, "local-lite trigger subject table does not exist: " +
               trigger.subjectCatalog + "." + trigger.subjectSchema + "." +
               trigger.subjectTable);
      return false;
    }
  std::string key = triggerKey(trigger.catalog, trigger.schema, trigger.name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *existing = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                               readOptions, key.data(), key.size(),
                               &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite trigger metadata", error))
    return false;
  if (existing)
    {
      rocksdb_free(existing);
      setError(error, "local-lite trigger already exists: " + trigger.catalog +
               "." + trigger.schema + "." + trigger.name);
      return false;
    }
  std::string value = encodeTrigger(trigger);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
              key.data(), key.size(), value.data(), value.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "write local-lite trigger metadata", error);
}

bool LocalLiteRocksDBStore::dropTrigger(
    const std::string &catalog, const std::string &schema,
    const std::string &name, bool ifExists, std::string *error)
{
  if (!open(error))
    return false;
  std::string key = triggerKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *existing = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                               readOptions, key.data(), key.size(),
                               &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite trigger metadata", error))
    return false;
  if (!existing)
    {
      if (ifExists) return true;
      setError(error, "local-lite trigger does not exist: " + catalog + "." +
               schema + "." + name);
      return false;
    }
  LocalLiteTriggerDef trigger;
  if (!decodeTrigger(std::string(existing, valueLen), &trigger, error))
    {
      rocksdb_free(existing);
      return false;
    }
  rocksdb_free(existing);

  std::vector<LocalLiteTriggerDef> subjectTriggers;
  if (!listTriggers(trigger.subjectCatalog, trigger.subjectSchema,
                    trigger.subjectTable, -1, &subjectTriggers, error))
    return false;
  const bool lastSubjectTrigger = subjectTriggers.size() == 1;
  LocalLiteTableDef transition;
  bool dropTransition = false;
  if (lastSubjectTrigger)
    {
      const std::string transitionName = trigger.subjectTable + "__TEMP";
      bool transitionExists = false;
      if (!tableExists(trigger.subjectCatalog, trigger.subjectSchema,
                       transitionName, &transitionExists, error))
        return false;
      if (transitionExists)
        {
          if (!loadTable(trigger.subjectCatalog, trigger.subjectSchema,
                         transitionName, &transition, error))
            return false;
          dropTransition = true;
        }
    }

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  rocksdb_writebatch_delete(batch, key.data(), key.size());
  if (dropTransition)
    {
      std::string transitionKey = tableKey(transition);
      std::string transitionUid = uidKey(transition.objectUid);
      rocksdb_writebatch_delete(batch, transitionKey.data(),
                                transitionKey.size());
      rocksdb_writebatch_delete(batch, transitionUid.data(),
                                transitionUid.size());
      for (size_t i = 0; i < transition.secondaryIndexes.size(); i++)
        {
          std::string indexMetadata = indexKey(
              transition.catalog, transition.schema,
              transition.secondaryIndexes[i].name);
          std::string indexUid = uidKey(
              transition.secondaryIndexes[i].objectUid);
          rocksdb_writebatch_delete(batch, indexMetadata.data(),
                                    indexMetadata.size());
          rocksdb_writebatch_delete(batch, indexUid.data(), indexUid.size());
        }
    }
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
                batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "delete local-lite trigger metadata", error))
    return false;
  if (dropTransition)
    {
      LocalLiteStorageManager::instance().closeTable(tablePath(transition));
      rocksdb_options_t *options = rocksdb_options_create();
      err = NULL;
      rocksdb_destroy_db(options, tablePath(transition).c_str(), &err);
      rocksdb_options_destroy(options);
      if (!checkRocksError(err, "destroy RocksDB table " +
                               tablePath(transition), error))
        return false;
    }
  if (dropTransition)
    publishOccTableChange(NULL, transition.objectUid);
  return true;
}

bool LocalLiteRocksDBStore::loadAuthIdentity(
    const std::string &name, LocalLiteAuthIdentity *identity, bool *found,
    std::string *error)
{
  if (!identity || !found || !open(error) || !ensureRootAuthMetadata(error))
    return false;
  std::string value;
  if (!readCatalogValue(authKey(name), &value, found, error))
    return false;
  if (!*found)
    return true;
  if (value.compare(0, 5, "LLA1\n") != 0)
    {
      setError(error, "invalid local-lite authorization identity metadata");
      return false;
    }
  size_t first = value.find('\n', 5);
  size_t second = first == std::string::npos
      ? std::string::npos : value.find('\n', first + 1);
  if (first == std::string::npos || second == std::string::npos)
    {
      setError(error, "truncated local-lite authorization identity metadata");
      return false;
    }
  identity->name = name;
  identity->id = strtoull(value.substr(5, first - 5).c_str(), NULL, 10);
  identity->role = value.substr(first + 1, second - first - 1) == "ROLE";
  return true;
}

bool LocalLiteRocksDBStore::createAuthIdentity(
    const std::string &name, bool role, bool ifNotExists, std::string *error)
{
  if (name.empty() || name.find('|') != std::string::npos ||
      !open(error) || !ensureRootAuthMetadata(error))
    {
      if (name.empty() || name.find('|') != std::string::npos)
        setError(error, "invalid local-lite authorization name");
      return false;
    }
  LocalLiteAuthIdentity existing;
  bool found = false;
  if (!loadAuthIdentity(name, &existing, &found, error))
    return false;
  if (found)
    {
      if (ifNotExists && existing.role == role)
        return true;
      setError(error, "local-lite authorization identity already exists: " + name);
      return false;
    }
  uint64_t id = LOCAL_LITE_ROOT_ID + 1;
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, "authid|", 7); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < 7 || memcmp(rawKey, "authid|", 7) != 0)
        break;
      uint64_t candidate = strtoull(rawKey + 7, NULL, 10);
      if (candidate >= id) id = candidate + 1;
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "scan local-lite authorization identities", error))
    return false;

  LocalLiteAuthIdentity identity;
  identity.name = name;
  identity.id = id;
  identity.role = role;
  if (!writeCatalogValue(authKey(name), encodeAuthIdentity(identity), error) ||
      !writeCatalogValue(authIdKey(id), name, error))
    return false;
  return bumpAuthorizationGeneration(error);
}

bool LocalLiteRocksDBStore::dropAuthIdentity(
    const std::string &name, bool role, bool ifExists, std::string *error)
{
  LocalLiteAuthIdentity identity;
  bool found = false;
  if (!loadAuthIdentity(name, &identity, &found, error))
    return false;
  if (!found)
    {
      if (ifExists) return true;
      setError(error, "local-lite authorization identity does not exist: " + name);
      return false;
    }
  if (identity.role != role || name == LOCAL_LITE_ROOT_NAME)
    {
      setError(error, "local-lite authorization identity type cannot be dropped: " + name);
      return false;
    }
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string key = authKey(name);
  std::string idKey = authIdKey(identity.id);
  rocksdb_writebatch_delete(batch, key.data(), key.size());
  rocksdb_writebatch_delete(batch, idKey.data(), idKey.size());
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, "role|", 5); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < 5 || memcmp(rawKey, "role|", 5) != 0) break;
      std::string roleKeyValue(rawKey, keyLen);
      size_t separator = roleKeyValue.find('|', 5);
      std::string rolePart = separator == std::string::npos
          ? std::string() : roleKeyValue.substr(5, separator - 5);
      std::string granteePart = separator == std::string::npos
          ? std::string() : roleKeyValue.substr(separator + 1);
      if (rolePart == name || granteePart == name)
        rocksdb_writebatch_delete(batch, rawKey, keyLen);
    }
  for (rocksdb_iter_seek(it, "priv|", 5); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < 5 || memcmp(rawKey, "priv|", 5) != 0) break;
      std::string privilegeKeyValue(rawKey, keyLen);
      size_t granteeSeparator = privilegeKeyValue.rfind('|');
      if (granteeSeparator != std::string::npos &&
          privilegeKeyValue.substr(granteeSeparator + 1) == name)
        rocksdb_writebatch_delete(batch, rawKey, keyLen);
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "scan local-lite authorization dependencies", error))
    {
      rocksdb_writebatch_destroy(batch);
      return false;
    }
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
                batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "delete local-lite authorization identity", error))
    return false;
  return bumpAuthorizationGeneration(error);
}

bool LocalLiteRocksDBStore::grantRole(
    const std::string &role, const std::string &grantee, bool adminOption,
    std::string *error)
{
  LocalLiteAuthIdentity roleIdentity, granteeIdentity;
  bool roleFound = false, granteeFound = false;
  if (!loadAuthIdentity(role, &roleIdentity, &roleFound, error) ||
      !loadAuthIdentity(grantee, &granteeIdentity, &granteeFound, error))
    return false;
  if (!roleFound || !roleIdentity.role || !granteeFound || role == grantee)
    {
      setError(error, "invalid local-lite role grant");
      return false;
    }
  if (!writeCatalogValue(roleKey(role, grantee), adminOption ? "1" : "0", error))
    return false;
  return bumpAuthorizationGeneration(error);
}

bool LocalLiteRocksDBStore::revokeRole(
    const std::string &role, const std::string &grantee, std::string *error)
{
  LocalLiteAuthIdentity roleIdentity;
  bool found = false;
  if (!loadAuthIdentity(role, &roleIdentity, &found, error)) return false;
  if (!found || !roleIdentity.role)
    {
      setError(error, "local-lite role does not exist: " + role);
      return false;
    }
  if (!deleteCatalogValue(roleKey(role, grantee), error)) return false;
  return bumpAuthorizationGeneration(error);
}

bool LocalLiteRocksDBStore::grantPrivilege(
    const std::string &catalog, const std::string &schema,
    const std::string &object, uint32_t privilegeMask,
    const std::string &grantee, bool grantOption, std::string *error)
{
  LocalLiteAuthIdentity granteeIdentity;
  bool found = false;
  if (!loadAuthIdentity(grantee, &granteeIdentity, &found, error)) return false;
  if (!found && grantee != "PUBLIC")
    {
      setError(error, "local-lite grantee does not exist: " + grantee);
      return false;
    }
  bool objectExists = false;
  if (!tableExists(catalog, schema, object, &objectExists, error) || !objectExists)
    {
      setError(error, "local-lite privilege object does not exist: " +
               catalog + "." + schema + "." + object);
      return false;
    }
  std::string key = privilegeKey(catalog, schema, object, grantee);
  std::string value;
  bool current = false;
  if (!readCatalogValue(key, &value, &current, error)) return false;
  uint32_t mask = 0; bool oldGrantOption = false;
  if (current)
    {
      size_t sep = value.find('|');
      mask = static_cast<uint32_t>(strtoul(value.substr(0, sep).c_str(), NULL, 10));
      oldGrantOption = sep != std::string::npos && value.substr(sep + 1) == "1";
    }
  mask |= privilegeMask;
  value = std::to_string(static_cast<unsigned long>(mask)) + "|" +
          ((grantOption || oldGrantOption) ? "1" : "0");
  if (!writeCatalogValue(key, value, error)) return false;
  return bumpAuthorizationGeneration(error);
}

bool LocalLiteRocksDBStore::revokePrivilege(
    const std::string &catalog, const std::string &schema,
    const std::string &object, uint32_t privilegeMask,
    const std::string &grantee, std::string *error)
{
  std::string key = privilegeKey(catalog, schema, object, grantee);
  std::string value;
  bool found = false;
  if (!readCatalogValue(key, &value, &found, error)) return false;
  if (!found) return true;
  size_t sep = value.find('|');
  uint32_t mask = static_cast<uint32_t>(strtoul(value.substr(0, sep).c_str(), NULL, 10));
  mask &= ~privilegeMask;
  if (mask == 0)
    {
      if (!deleteCatalogValue(key, error)) return false;
    }
  else
    {
      std::string option = sep != std::string::npos ? value.substr(sep + 1) : "0";
      if (!writeCatalogValue(key, std::to_string(static_cast<unsigned long>(mask)) +
                                  "|" + option, error)) return false;
    }
  return bumpAuthorizationGeneration(error);
}

bool LocalLiteRocksDBStore::hasPrivilege(
    const std::string &catalog, const std::string &schema,
    const std::string &object, const std::string &user,
    uint32_t privilegeMask, std::string *error)
{
  if (!open(error) || !ensureRootAuthMetadata(error)) return false;
  if (user == LOCAL_LITE_ROOT_NAME) return true;
  std::string owner;
  bool ownerFound = false;
  if (!readCatalogValue(ownerKey(catalog, schema, object), &owner,
                        &ownerFound, error)) return false;
  if (ownerFound && owner == user) return true;

  std::set<std::string> principals;
  std::vector<std::string> pending;
  principals.insert(user);
  pending.push_back(user);
  for (size_t pos = 0; pos < pending.size(); pos++)
    {
      rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
      rocksdb_iterator_t *it = rocksdb_create_iterator(
          LocalLiteStorageManager::instance().catalogDb(), readOptions);
      for (rocksdb_iter_seek(it, "role|", 5); rocksdb_iter_valid(it);
           rocksdb_iter_next(it))
        {
          size_t keyLen = 0;
          const char *rawKey = rocksdb_iter_key(it, &keyLen);
          if (keyLen < 5 || memcmp(rawKey, "role|", 5) != 0) break;
          std::string key(rawKey, keyLen);
          size_t first = key.find('|', 5);
          if (first == std::string::npos || key.substr(first + 1) != pending[pos])
            continue;
          std::string role = key.substr(5, first - 5);
          if (principals.insert(role).second) pending.push_back(role);
        }
      char *err = NULL;
      rocksdb_iter_get_error(it, &err);
      rocksdb_iter_destroy(it);
      rocksdb_readoptions_destroy(readOptions);
      if (!checkRocksError(err, "scan local-lite role membership", error)) return false;
    }
  principals.insert("PUBLIC");
  for (std::set<std::string>::const_iterator it = principals.begin();
       it != principals.end(); ++it)
    {
      std::string value;
      bool found = false;
      if (!readCatalogValue(privilegeKey(catalog, schema, object, *it),
                            &value, &found, error)) return false;
      if (!found) continue;
      size_t sep = value.find('|');
      uint32_t mask = static_cast<uint32_t>(strtoul(value.substr(0, sep).c_str(), NULL, 10));
      if ((mask & privilegeMask) == privilegeMask) return true;
    }
  return false;
}

bool LocalLiteRocksDBStore::setTableOwner(
    const std::string &catalog, const std::string &schema,
    const std::string &object, const std::string &owner, std::string *error)
{
  return writeCatalogValue(ownerKey(catalog, schema, object), owner, error);
}

bool LocalLiteRocksDBStore::isTableOwner(
    const std::string &catalog, const std::string &schema,
    const std::string &object, const std::string &user, bool *owner,
    std::string *error)
{
  if (!owner || !open(error)) return false;
  std::string value;
  bool found = false;
  if (!readCatalogValue(ownerKey(catalog, schema, object), &value, &found, error))
    return false;
  *owner = found && value == user;
  return true;
}

bool LocalLiteRocksDBStore::bumpAuthorizationGeneration(std::string *error)
{
  std::string value;
  bool found = false;
  if (!readCatalogValue(authorizationGenerationKey(), &value, &found, error))
    return false;
  uint64_t generation = found ? strtoull(value.c_str(), NULL, 10) : 0;
  return writeCatalogValue(authorizationGenerationKey(),
                           std::to_string(static_cast<unsigned long long>(generation + 1)),
                           error);
}

bool LocalLiteRocksDBStore::loadCatalogRecord(
    const std::string &key, std::string *value, bool *found, std::string *error)
{
  if (!value || !found || !open(error))
    return false;
  return readCatalogValue(key, value, found, error);
}

bool LocalLiteRocksDBStore::storeCatalogRecord(
    const std::string &key, const std::string &value, std::string *error)
{
  if (!open(error))
    return false;
  return writeCatalogValue(key, value, error);
}

bool LocalLiteRocksDBStore::deleteCatalogRecord(
    const std::string &key, std::string *error)
{
  if (!open(error))
    return false;
  return deleteCatalogValue(key, error);
}

bool LocalLiteRocksDBStore::scanCatalogRecords(
    const std::string &prefix,
    std::vector< std::pair<std::string, std::string> > *records,
    std::string *error)
{
  if (!records || !open(error))
    return false;
  records->clear();
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, prefix.data(), prefix.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0, valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < prefix.size() ||
          memcmp(rawKey, prefix.data(), prefix.size()) != 0)
        break;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      records->push_back(std::make_pair(std::string(rawKey, keyLen),
                                        std::string(rawValue, valueLen)));
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  return checkRocksError(err, "scan local-lite catalog records", error);
}

bool LocalLiteRocksDBStore::scanMetadataRows(
    const std::string &metadataTable,
    std::vector< std::pair<std::string, std::string> > *records,
    std::string *error)
{
  if (metadataTable.empty() || metadataTable.find('|') != std::string::npos)
    {
      setError(error, "invalid local-lite metadata table name");
      return false;
    }
  return scanCatalogRecords(localLiteMetadataPrefix(metadataTable.c_str()),
                            records, error);
}

bool LocalLiteRocksDBStore::dropSchema(const std::string &catalog,
                                       const std::string &schema,
                                       bool ifExists,
                                       bool cascade,
                                       std::string *error)
{
  bool exists = false;
  if (!schemaExists(catalog, schema, &exists, error))
    return false;
  if (!exists)
    {
      if (ifExists)
        return true;
      setError(error, "local-lite schema does not exist: " + catalog + "." +
              schema);
      return false;
    }

  std::vector<LocalLiteTableDef> tables;
  if (!listTables(catalog, schema, &tables, error))
    return false;

  struct CatalogEntry
    {
      std::string key;
      std::string value;
    };
  std::vector<CatalogEntry> sequences;
  std::vector<CatalogEntry> synonyms;
  std::vector<CatalogEntry> triggers;
  const std::string sequencePrefix =
      "sequence|" + catalog + "|" + schema + "|";
  const std::string synonymPrefix =
      "synonym|" + catalog + "|" + schema + "|";
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek_to_first(it); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t keyLen = 0, valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      std::string key(rawKey, keyLen);
      if (key.compare(0, sequencePrefix.size(), sequencePrefix) == 0)
        {
          CatalogEntry entry;
          entry.key = key;
          entry.value.assign(rawValue, valueLen);
          sequences.push_back(entry);
        }
      else if (key.compare(0, 8, "synonym|") == 0)
        {
          std::string value(rawValue, valueLen);
          size_t first = value.find('\n');
          size_t second = first == std::string::npos
              ? first : value.find('\n', first + 1);
          bool inSchema =
              key.compare(0, synonymPrefix.size(), synonymPrefix) == 0;
          bool targetsSchema =
              first != std::string::npos && second != std::string::npos &&
              value.substr(0, first) == catalog &&
              value.substr(first + 1, second - first - 1) == schema;
          if (inSchema || targetsSchema)
            {
              CatalogEntry entry;
              entry.key = key;
              entry.value = value;
              synonyms.push_back(entry);
            }
        }
      else if (key.compare(0, 8, "trigger|") == 0)
        {
          LocalLiteTriggerDef trigger;
          std::string value(rawValue, valueLen);
          if (!decodeTrigger(value, &trigger, error))
            {
              rocksdb_iter_destroy(it);
              rocksdb_readoptions_destroy(readOptions);
              return false;
            }
          if ((trigger.catalog == catalog && trigger.schema == schema) ||
              (trigger.subjectCatalog == catalog &&
               trigger.subjectSchema == schema))
            {
              CatalogEntry entry;
              entry.key = key;
              entry.value = value;
              triggers.push_back(entry);
            }
        }
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "scan local-lite schema metadata", error))
    return false;
  std::vector<LocalLiteTableDef> allTables;
  if (!listTables("", "", &allTables, error))
    return false;
  if ((!tables.empty() || !sequences.empty() || !synonyms.empty() ||
       !triggers.empty()) && !cascade)
    {
      setError(error, "local-lite schema is not empty: " + catalog + "." +
              schema);
      return false;
    }
  if (cascade)
    {
      std::set<std::string> dropped;
      for (size_t i = 0; i < tables.size(); i++)
        dropped.insert(tableKey(tables[i]));
      bool added = true;
      while (added)
        {
          added = false;
          for (size_t i = 0; i < allTables.size(); i++)
            {
              if (!allTables[i].view ||
                  dropped.find(tableKey(allTables[i])) != dropped.end())
                continue;
              for (size_t d = 0; d < allTables[i].dependencies.size(); d++)
                if (dropped.find(tableKey(
                        allTables[i].dependencies[d].catalog,
                        allTables[i].dependencies[d].schema,
                        allTables[i].dependencies[d].name)) != dropped.end())
                  {
                    tables.push_back(allTables[i]);
                    dropped.insert(tableKey(allTables[i]));
                    added = true;
                    break;
                  }
            }
        }
    }
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  for (size_t i = 0; i < tables.size(); i++)
    {
      std::string key = tableKey(tables[i]);
      std::string uid = uidKey(tables[i].objectUid);
      std::string tableStatsKey = statsKey(tables[i].catalog,
                                           tables[i].schema,
                                           tables[i].name);
      rocksdb_writebatch_delete(batch, key.data(), key.size());
      rocksdb_writebatch_delete(batch, uid.data(), uid.size());
      rocksdb_writebatch_delete(batch, tableStatsKey.data(),
                                tableStatsKey.size());
      deleteLocalLiteMetadataForTable(batch, tables[i]);
      for (size_t x = 0; x < tables[i].secondaryIndexes.size(); x++)
        {
          std::string indexMetadata = indexKey(
              tables[i].catalog, tables[i].schema,
              tables[i].secondaryIndexes[x].name);
          std::string indexUid = uidKey(
              tables[i].secondaryIndexes[x].objectUid);
          rocksdb_writebatch_delete(batch, indexMetadata.data(),
                                    indexMetadata.size());
          rocksdb_writebatch_delete(batch, indexUid.data(), indexUid.size());
        }
    }
  for (size_t i = 0; i < sequences.size(); i++)
    {
      LocalLiteSequenceDef sequence;
      std::string name = sequences[i].key.substr(sequencePrefix.size());
      if (!decodeSequence(sequences[i].value, catalog, schema, name,
                          &sequence, error))
        {
          rocksdb_writebatch_destroy(batch);
          return false;
        }
      std::string uid = uidKey(sequence.objectUid);
      rocksdb_writebatch_delete(batch, sequences[i].key.data(),
                                sequences[i].key.size());
      rocksdb_writebatch_delete(batch, uid.data(), uid.size());
    }
  for (size_t i = 0; i < synonyms.size(); i++)
    rocksdb_writebatch_delete(batch, synonyms[i].key.data(),
                              synonyms[i].key.size());
  for (size_t i = 0; i < triggers.size(); i++)
    rocksdb_writebatch_delete(batch, triggers[i].key.data(),
                              triggers[i].key.size());

  // Remove RI edges from surviving tables as part of the same catalog batch.
  for (size_t i = 0; i < allTables.size(); i++)
    {
      bool isDropped = false;
      for (size_t d = 0; d < tables.size(); d++)
        if (tableKey(allTables[i]) == tableKey(tables[d])) isDropped = true;
      if (isDropped)
        continue;
      LocalLiteTableDef updated = allTables[i];
      for (size_t r = updated.riConstraints.size(); r > 0; r--)
        if (updated.riConstraints[r - 1].referencedCatalog == catalog &&
            updated.riConstraints[r - 1].referencedSchema == schema)
          updated.riConstraints.erase(updated.riConstraints.begin() + r - 1);
      if (updated.riConstraints.size() != allTables[i].riConstraints.size())
        {
          std::string key = tableKey(updated);
          std::string value = encodeTable(updated);
          rocksdb_writebatch_put(batch, key.data(), key.size(), value.data(),
                                 value.size());
          deleteLocalLiteMetadataForTable(batch, allTables[i]);
          addLocalLiteMetadataForTable(batch, updated);
        }
    }

  std::string key = schemaKey(catalog, schema);
  rocksdb_writebatch_delete(batch, key.data(), key.size());
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
                batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "delete local-lite schema metadata", error))
    return false;

  for (size_t i = 0; i < tables.size(); i++)
    {
      LocalLiteStorageManager::instance().closeTable(tablePath(tables[i]));
      rocksdb_options_t *options = rocksdb_options_create();
      err = NULL;
      rocksdb_destroy_db(options, tablePath(tables[i]).c_str(), &err);
      rocksdb_options_destroy(options);
      if (!checkRocksError(err, "destroy RocksDB table " +
                               tablePath(tables[i]), error))
        return false;
    }
  return true;
}

bool LocalLiteRocksDBStore::dropTable(const std::string &catalog,
                                      const std::string &schema,
                                      const std::string &name,
                                      std::string *error)
{
  LocalLiteTableDef table;
  if (!loadTable(catalog, schema, name, &table, error))
    return false;

  LocalLiteTableDef transition;
  bool dropTransition = false;
  std::vector<LocalLiteTriggerDef> subjectTriggers;
  if (!listTriggers(catalog, schema, name, -1, &subjectTriggers, error))
    return false;
  if (!subjectTriggers.empty())
    {
      const std::string transitionName = name + "__TEMP";
      bool transitionExists = false;
      if (!tableExists(catalog, schema, transitionName, &transitionExists,
                       error))
        return false;
      if (transitionExists)
        {
          if (!loadTable(catalog, schema, transitionName, &transition, error))
            return false;
          if (transition.columns.size() < 2 ||
              transition.columns[0].name != "@UNIQUE_EXECUTE_ID" ||
              transition.columns[1].name != "@UNIQUE_IUD_ID")
            {
              setError(error, "local-lite trigger transition table metadata "
                       "is invalid: " + catalog + "." + schema + "." +
                       transitionName);
              return false;
            }
          dropTransition = true;
        }
    }

  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  std::string key = tableKey(catalog, schema, name);
  std::string uid = uidKey(table.objectUid);
  std::string tableStatsKey = statsKey(catalog, schema, name);
  rocksdb_writebatch_delete(batch, key.data(), key.size());
  rocksdb_writebatch_delete(batch, uid.data(), uid.size());
  rocksdb_writebatch_delete(batch, tableStatsKey.data(), tableStatsKey.size());
  deleteLocalLiteMetadataForTable(batch, table);
  for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
    {
      std::string idxKey = indexKey(catalog, schema,
                                    table.secondaryIndexes[i].name);
      std::string idxUid = uidKey(table.secondaryIndexes[i].objectUid);
      rocksdb_writebatch_delete(batch, idxKey.data(), idxKey.size());
      rocksdb_writebatch_delete(batch, idxUid.data(), idxUid.size());
    }
  if (dropTransition)
    {
      std::string transitionKey = tableKey(transition);
      std::string transitionUid = uidKey(transition.objectUid);
      rocksdb_writebatch_delete(batch, transitionKey.data(),
                                transitionKey.size());
      rocksdb_writebatch_delete(batch, transitionUid.data(),
                                transitionUid.size());
      deleteLocalLiteMetadataForTable(batch, transition);
      for (size_t i = 0; i < transition.secondaryIndexes.size(); i++)
        {
          std::string idxKey = indexKey(
              transition.catalog, transition.schema,
              transition.secondaryIndexes[i].name);
          std::string idxUid = uidKey(
              transition.secondaryIndexes[i].objectUid);
          rocksdb_writebatch_delete(batch, idxKey.data(), idxKey.size());
          rocksdb_writebatch_delete(batch, idxUid.data(), idxUid.size());
        }
    }

  // A synonym is a catalog dependency on its target. Remove every synonym
  // that would otherwise become dangling in the same catalog write batch.
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  const std::string target = catalog + "\n" + schema + "\n" + name;
  for (rocksdb_iter_seek(it, "synonym|", 8); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t synonymKeyLen = 0, valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &synonymKeyLen);
      if (synonymKeyLen < 8 || memcmp(rawKey, "synonym|", 8) != 0)
        break;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      if (valueLen == target.size() &&
          memcmp(rawValue, target.data(), valueLen) == 0)
        rocksdb_writebatch_delete(batch, rawKey, synonymKeyLen);
    }
  for (rocksdb_iter_seek(it, "trigger|", 8); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t triggerKeyLen = 0, valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &triggerKeyLen);
      if (triggerKeyLen < 8 || memcmp(rawKey, "trigger|", 8) != 0)
        break;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      LocalLiteTriggerDef trigger;
      if (!decodeTrigger(std::string(rawValue, valueLen), &trigger, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          rocksdb_writebatch_destroy(batch);
          return false;
        }
      if (trigger.subjectCatalog == catalog && trigger.subjectSchema == schema &&
          trigger.subjectTable == name)
        rocksdb_writebatch_delete(batch, rawKey, triggerKeyLen);
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "scan local-lite synonym dependencies", error))
    {
      rocksdb_writebatch_destroy(batch);
      return false;
    }
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "delete local-lite table metadata", error))
    return false;

  // Ownership is separate from the table encoding so existing local-lite
  // catalogs remain readable across M8 upgrades.
  if (!deleteCatalogValue(ownerKey(catalog, schema, name), error))
    return false;
  if (dropTransition &&
      !deleteCatalogValue(ownerKey(transition.catalog, transition.schema,
                                   transition.name), error))
    return false;

  LocalLiteStorageManager::instance().closeTable(tablePath(table));
  rocksdb_options_t *options = rocksdb_options_create();
  err = NULL;
  rocksdb_destroy_db(options, tablePath(table).c_str(), &err);
  rocksdb_options_destroy(options);
  if (!checkRocksError(err, "destroy RocksDB table " + tablePath(table), error))
    return false;
  if (dropTransition)
    {
      LocalLiteStorageManager::instance().closeTable(tablePath(transition));
      rocksdb_options_t *options = rocksdb_options_create();
      err = NULL;
      rocksdb_destroy_db(options, tablePath(transition).c_str(), &err);
      rocksdb_options_destroy(options);
      if (!checkRocksError(err, "destroy RocksDB table " +
                               tablePath(transition), error))
        return false;
    }
  publishOccTableChange(NULL, table.objectUid);
  if (dropTransition)
    publishOccTableChange(NULL, transition.objectUid);
  return true;
}

bool LocalLiteRocksDBStore::tableExists(const std::string &catalog,
                                        const std::string &schema,
                                        const std::string &name,
                                        bool *exists,
                                        std::string *error)
{
  if (!open(error))
    return false;

  if (isLocalLiteMetadataTable(catalog, schema, name))
    {
      *exists = true;
      return true;
    }

  std::string key = tableKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions,
                            key.data(), key.size(), &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite table metadata", error))
    return false;
  if (value)
    {
      rocksdb_free(value);
      *exists = true;
      return true;
    }
  *exists = false;
  return true;
}

bool LocalLiteRocksDBStore::listTables(
    const std::string &catalog, const std::string &schema,
    std::vector<LocalLiteTableDef> *tables, std::string *error)
{
  if (!tables || !open(error))
    return false;
  tables->clear();
  std::string prefix = "table|";
  if (!catalog.empty())
    {
      prefix += catalog + "|";
      if (!schema.empty())
        prefix += schema + "|";
    }
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, prefix.data(), prefix.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      size_t valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < prefix.size() ||
          memcmp(rawKey, prefix.data(), prefix.size()) != 0)
        break;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      LocalLiteTableDef table;
      if (!decodeTable(std::string(rawValue, valueLen), &table, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
      tables->push_back(table);
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  return checkRocksError(err, "scan local-lite table metadata", error);
}

bool LocalLiteRocksDBStore::listSchemas(
    const std::string &catalog, std::vector<std::string> *schemas,
    std::string *error)
{
  if (!schemas || !open(error))
    return false;
  schemas->clear();

  const std::string prefix = catalog.empty()
      ? "schema|" : "schema|" + catalog + "|";
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, prefix.data(), prefix.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      std::string key(rawKey, keyLen);
      if (key.compare(0, prefix.size(), prefix) != 0)
        break;

      size_t catalogEnd = key.find('|', strlen("schema|"));
      if (catalogEnd == std::string::npos || catalogEnd + 1 >= key.size())
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          setError(error, "invalid local-lite schema metadata key");
          return false;
        }
      schemas->push_back(key.substr(catalogEnd + 1));
    }

  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "scan local-lite schema metadata", error))
    return false;

  // Older local-lite catalogs may contain table records created before the
  // explicit schema record was introduced.  Include those schemas as a
  // compatibility fallback while retaining empty schemas from schema| keys.
  std::vector<LocalLiteTableDef> tables;
  if (!listTables(catalog, "", &tables, error))
    return false;
  for (size_t i = 0; i < tables.size(); i++)
    schemas->push_back(tables[i].schema);

  // _MD_ is a built-in local-lite schema. Its metadata tables are exposed
  // through synthetic descriptors and therefore have no normal schema record.
  if (catalog == "TRAFODION")
    schemas->push_back("_MD_");

  std::sort(schemas->begin(), schemas->end());
  schemas->erase(std::unique(schemas->begin(), schemas->end()), schemas->end());
  return true;
}

bool LocalLiteRocksDBStore::listCatalogs(
    std::vector<std::string> *catalogs, std::string *error)
{
  if (!catalogs || !open(error))
    return false;
  catalogs->clear();

  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  const std::string prefix = "schema|";
  for (rocksdb_iter_seek(it, prefix.data(), prefix.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      std::string key(rawKey, keyLen);
      if (key.compare(0, prefix.size(), prefix) != 0)
        break;
      size_t catalogEnd = key.find('|', prefix.size());
      if (catalogEnd == std::string::npos || catalogEnd == prefix.size())
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          setError(error, "invalid local-lite schema metadata key");
          return false;
        }
      catalogs->push_back(key.substr(prefix.size(), catalogEnd - prefix.size()));
    }

  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "scan local-lite catalog metadata", error))
    return false;

  // Include catalogs from legacy table records as well.  This also makes the
  // catalog enumeration useful for catalogs that contain only old tables.
  std::vector<LocalLiteTableDef> tables;
  if (!listTables("", "", &tables, error))
    return false;
  for (size_t i = 0; i < tables.size(); i++)
    catalogs->push_back(tables[i].catalog);

  std::sort(catalogs->begin(), catalogs->end());
  catalogs->erase(std::unique(catalogs->begin(), catalogs->end()), catalogs->end());
  return true;
}

bool LocalLiteRocksDBStore::listSequences(
    const std::string &catalog,
    const std::string &schema,
    std::vector<LocalLiteSequenceDef> *sequences,
    std::string *error)
{
  if (!sequences || !open(error)) return false;
  sequences->clear();
  const bool listAll = catalog.empty() && schema.empty();
  const std::string prefix = listAll ? "sequence|" :
      "sequence|" + catalog + "|" + schema + "|";
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(
      LocalLiteStorageManager::instance().catalogDb(), readOptions);
  for (rocksdb_iter_seek(it, prefix.data(), prefix.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      std::string key(rawKey, keyLen);
      if (key.compare(0, prefix.size(), prefix) != 0) break;
      std::string sequenceCatalog = catalog;
      std::string sequenceSchema = schema;
      std::string name;
      if (listAll)
        {
          size_t catalogEnd = key.find('|', prefix.size());
          size_t schemaEnd = catalogEnd == std::string::npos
              ? std::string::npos : key.find('|', catalogEnd + 1);
          if (catalogEnd == std::string::npos || schemaEnd == std::string::npos)
            {
              rocksdb_iter_destroy(it);
              rocksdb_readoptions_destroy(readOptions);
              setError(error, "invalid local-lite sequence metadata key");
              return false;
            }
          sequenceCatalog = key.substr(prefix.size(), catalogEnd - prefix.size());
          sequenceSchema = key.substr(catalogEnd + 1,
                                      schemaEnd - catalogEnd - 1);
          name = key.substr(schemaEnd + 1);
        }
      else
        name = key.substr(prefix.size());
      size_t valueLen = 0;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      LocalLiteSequenceDef sequence;
      if (!decodeSequence(std::string(rawValue, valueLen), sequenceCatalog,
                          sequenceSchema,
                          name, &sequence, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
      sequences->push_back(sequence);
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  std::sort(sequences->begin(), sequences->end(),
            [](const LocalLiteSequenceDef &left,
               const LocalLiteSequenceDef &right) {
              return left.name < right.name;
            });
  return checkRocksError(err, "scan local-lite sequence metadata", error);
}

bool LocalLiteRocksDBStore::loadTable(const std::string &catalog,
                                      const std::string &schema,
                                      const std::string &name,
                                      LocalLiteTableDef *table,
                                      std::string *error)
{
  if (!open(error))
    return false;

  if (isLocalLiteMetadataTable(catalog, schema, name))
    return localLiteMetadataTableDefinition(catalog, schema, name, table,
                                            error);

  std::string key = tableKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions,
                            key.data(), key.size(), &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite table metadata", error))
    return false;
  if (!value)
    {
      // Optimizer-visible secondary indexes use their own external name in
      // the generated scan TDB.  They are still physically stored in the
      // owning RocksDB table, so resolve the catalog's index-to-table link.
      std::string idxKey = indexKey(catalog, schema, name);
      readOptions = rocksdb_readoptions_create();
      size_t tableKeyLen = 0;
      char *rawTableKey = rocksdb_get(
          LocalLiteStorageManager::instance().catalogDb(), readOptions,
          idxKey.data(), idxKey.size(), &tableKeyLen, &err);
      rocksdb_readoptions_destroy(readOptions);
      if (!checkRocksError(err, "read local-lite index metadata", error))
        return false;
      if (!rawTableKey)
        {
          setError(error, "local-lite table does not exist: " +
                  catalog + "." + schema + "." + name);
          return false;
        }
      std::string owningTableKey(rawTableKey, tableKeyLen);
      rocksdb_free(rawTableKey);
      readOptions = rocksdb_readoptions_create();
      value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                          readOptions, owningTableKey.data(),
                          owningTableKey.size(), &valueLen, &err);
      rocksdb_readoptions_destroy(readOptions);
      if (!checkRocksError(err, "read local-lite owning table metadata", error))
        return false;
      if (!value)
        {
          setError(error, "local-lite index references a missing table: " +
                  catalog + "." + schema + "." + name);
          return false;
        }
    }
  std::string encoded(value, valueLen);
  rocksdb_free(value);
  return decodeTable(encoded, table, error);
}

bool LocalLiteRocksDBStore::createIndex(const LocalLiteTableDef &table,
                                        const LocalLiteIndexDef &index,
                                        std::string *error)
{
  if (!open(error))
    return false;
  if (index.name.empty() || index.objectUid == 0 || index.keyColumns.empty() ||
      index.descending.size() != index.keyColumns.size())
    {
      setError(error, "invalid local-lite index definition");
      return false;
    }

  LocalLiteTableDef loaded;
  if (!loadTable(table.catalog, table.schema, table.name, &loaded, error))
    return false;
  bool tableNameCollision = false;
  if (!tableExists(loaded.catalog, loaded.schema, index.name,
                   &tableNameCollision, error))
    return false;
  if (tableNameCollision)
    {
      setError(error, "local-lite object already exists: " + loaded.catalog +
                      "." + loaded.schema + "." + index.name);
      return false;
    }
  for (size_t i = 0; i < index.keyColumns.size(); i++)
    {
      if (index.keyColumns[i] >= loaded.columns.size())
        {
          setError(error, "invalid local-lite index column metadata");
          return false;
        }
      for (size_t j = 0; j < i; j++)
        if (index.keyColumns[j] == index.keyColumns[i])
          {
            setError(error, "duplicate local-lite index column");
            return false;
          }
    }

  std::string idxKey = indexKey(loaded.catalog, loaded.schema, index.name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions, idxKey.data(), idxKey.size(),
                            &valueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite index metadata", error))
    return false;
  if (value)
    {
      rocksdb_free(value);
      setError(error, "local-lite index already exists: " + loaded.catalog +
                      "." + loaded.schema + "." + index.name);
      return false;
    }

  rocksdb_t *tableDb = LocalLiteStorageManager::instance().openTable(
      tablePath(loaded), false, error);
  if (!tableDb)
    return false;

  std::map<std::string, std::string> backfillEntries;
  readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(tableDb, readOptions);
  for (rocksdb_iter_seek_to_first(it); rocksdb_iter_valid(it);
       rocksdb_iter_next(it))
    {
      size_t rowKeyLen = 0;
      size_t rowValueLen = 0;
      const char *rawRowKey = rocksdb_iter_key(it, &rowKeyLen);
      const char *rawRowValue = rocksdb_iter_value(it, &rowValueLen);
      std::string rowKey(rawRowKey, rowKeyLen);
      if (!rowKey.empty() &&
          (rowKey[0] == 'U' || rowKey[0] == 'I' || rowKey[0] == 'J'))
        continue;

      std::string encodedRow;
      if (!decodeRowValue(std::string(rawRowValue, rowValueLen),
                          &encodedRow, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
      LocalLitePhysicalIndexEntry entry;
      bool hasEntry = false;
      if (!buildSecondaryIndexEntry(loaded, index, encodedRow, rowKey,
                                    &entry, &hasEntry, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          return false;
        }
      if (hasEntry &&
          !backfillEntries.insert(
              std::make_pair(entry.key, entry.value)).second)
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          setError(error, "duplicate local-lite unique index key");
          return false;
        }
    }
  char *iteratorError = NULL;
  rocksdb_iter_get_error(it, &iteratorError);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(iteratorError,
                       "scan local-lite rows for index backfill", error))
    return false;

  rocksdb_writebatch_t *dataBatch = rocksdb_writebatch_create();
  for (std::map<std::string, std::string>::const_iterator entry =
           backfillEntries.begin(); entry != backfillEntries.end(); ++entry)
    rocksdb_writebatch_put(dataBatch, entry->first.data(), entry->first.size(),
                           entry->second.data(), entry->second.size());
  rocksdb_writeoptions_t *dataWriteOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(tableDb, dataWriteOptions, dataBatch, &err);
  rocksdb_writeoptions_destroy(dataWriteOptions);
  rocksdb_writebatch_destroy(dataBatch);
  if (!checkRocksError(err, "backfill local-lite index", error))
    return false;

  LocalLiteTableDef oldLoaded = loaded;
  loaded.secondaryIndexes.push_back(index);
  std::string tabKey = tableKey(loaded);
  std::string idxUid = uidKey(index.objectUid);
  std::string encoded = encodeTable(loaded);
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  deleteLocalLiteMetadataForTable(batch, oldLoaded);
  rocksdb_writebatch_put(batch, tabKey.data(), tabKey.size(),
                         encoded.data(), encoded.size());
  rocksdb_writebatch_put(batch, idxKey.data(), idxKey.size(),
                         tabKey.data(), tabKey.size());
  rocksdb_writebatch_put(batch, idxUid.data(), idxUid.size(),
                         idxKey.data(), idxKey.size());
  addLocalLiteMetadataForTable(batch, loaded);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (checkRocksError(err, "write local-lite index metadata", error))
    {
      publishOccTableChange(NULL, loaded.objectUid);
      return true;
    }

  rocksdb_writebatch_t *rollback = rocksdb_writebatch_create();
  for (std::map<std::string, std::string>::const_iterator entry =
           backfillEntries.begin(); entry != backfillEntries.end(); ++entry)
    rocksdb_writebatch_delete(rollback, entry->first.data(),
                              entry->first.size());
  dataWriteOptions = rocksdb_writeoptions_create();
  char *rollbackError = NULL;
  rocksdb_write(tableDb, dataWriteOptions, rollback, &rollbackError);
  rocksdb_writeoptions_destroy(dataWriteOptions);
  rocksdb_writebatch_destroy(rollback);
  if (rollbackError)
    {
      if (error)
        *error += "; rollback local-lite index backfill: " +
                  std::string(rollbackError);
      rocksdb_free(rollbackError);
    }
  return false;
}

bool LocalLiteRocksDBStore::dropIndex(const std::string &catalog,
                                      const std::string &schema,
                                      const std::string &name,
                                      bool ifExists,
                                      std::string *error)
{
  if (!open(error))
    return false;

  std::string idxKey = indexKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t tableKeyLen = 0;
  char *rawTableKey = rocksdb_get(
      LocalLiteStorageManager::instance().catalogDb(), readOptions,
      idxKey.data(), idxKey.size(), &tableKeyLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite index metadata", error))
    return false;
  if (!rawTableKey)
    {
      if (ifExists)
        return true;
      setError(error, "local-lite index does not exist: " + catalog + "." +
                      schema + "." + name);
      return false;
    }
  std::string tabKey(rawTableKey, tableKeyLen);
  rocksdb_free(rawTableKey);

  readOptions = rocksdb_readoptions_create();
  size_t tableValueLen = 0;
  char *rawTable = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                               readOptions, tabKey.data(), tabKey.size(),
                               &tableValueLen, &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite table metadata", error))
    return false;
  if (!rawTable)
    {
      setError(error, "local-lite index references a missing table");
      return false;
    }

  LocalLiteTableDef table;
  std::string encodedTable(rawTable, tableValueLen);
  rocksdb_free(rawTable);
  if (!decodeTable(encodedTable, &table, error))
    return false;

  size_t found = table.secondaryIndexes.size();
  for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
    if (table.secondaryIndexes[i].name == name)
      {
        found = i;
        break;
      }
  if (found == table.secondaryIndexes.size())
    {
      setError(error, "local-lite index metadata is inconsistent");
      return false;
    }
  LocalLiteIndexDef droppedIndex = table.secondaryIndexes[found];
  LocalLiteTableDef oldTable = table;
  std::string idxUid = uidKey(droppedIndex.objectUid);
  table.secondaryIndexes.erase(table.secondaryIndexes.begin() + found);

  encodedTable = encodeTable(table);
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  deleteLocalLiteMetadataForTable(batch, oldTable);
  rocksdb_writebatch_put(batch, tabKey.data(), tabKey.size(),
                         encodedTable.data(), encodedTable.size());
  rocksdb_writebatch_delete(batch, idxKey.data(), idxKey.size());
  rocksdb_writebatch_delete(batch, idxUid.data(), idxUid.size());
  addLocalLiteMetadataForTable(batch, table);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(LocalLiteStorageManager::instance().catalogDb(),
                writeOptions, batch, &err);
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  if (!checkRocksError(err, "delete local-lite index metadata", error))
    return false;

  rocksdb_t *tableDb = LocalLiteStorageManager::instance().openTable(
      tablePath(table), false, error);
  if (!tableDb)
    return false;
  std::string physicalPrefix;
  physicalPrefix.push_back('I');
  appendUint64(physicalPrefix, droppedIndex.objectUid);
  readOptions = rocksdb_readoptions_create();
  rocksdb_iterator_t *it = rocksdb_create_iterator(tableDb, readOptions);
  rocksdb_writebatch_t *dataBatch = rocksdb_writebatch_create();
  for (rocksdb_iter_seek(it, physicalPrefix.data(), physicalPrefix.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      if (keyLen < physicalPrefix.size() ||
          memcmp(rawKey, physicalPrefix.data(), physicalPrefix.size()) != 0)
        break;
      rocksdb_writebatch_delete(dataBatch, rawKey, keyLen);
    }
  char *iteratorError = NULL;
  rocksdb_iter_get_error(it, &iteratorError);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(iteratorError,
                       "scan local-lite index for drop", error))
    {
      rocksdb_writebatch_destroy(dataBatch);
      return false;
    }
  rocksdb_writeoptions_t *dataWriteOptions = rocksdb_writeoptions_create();
  err = NULL;
  rocksdb_write(tableDb, dataWriteOptions, dataBatch, &err);
  rocksdb_writeoptions_destroy(dataWriteOptions);
  rocksdb_writebatch_destroy(dataBatch);
  if (!checkRocksError(err, "delete local-lite index records", error))
    return false;
  publishOccTableChange(NULL, table.objectUid);
  return true;
}

bool LocalLiteRocksDBStore::alterTable(
    const LocalLiteTableDef &oldTable,
    const LocalLiteTableDef &newTable,
    const std::vector<int> &newToOldColumn,
    const std::vector<std::string> &addedValues,
    LocalLiteTxnContext *txnContext,
    std::string *error)
{
  if (!LocalLiteTxnManager::prepareDDLForExecutor(
          txnContext,
          LocalLiteTxnManager::currentExecutorTxnId(txnContext), error))
    return false;
  if (!open(error))
    return false;
  // Trafodion DDL has an independent metadata boundary.  In particular,
  // legacy ALTER TABLE tests issue it between BEGIN WORK and COMMIT WORK;
  // do not reject the catalog/row rewrite merely because a DML transaction
  // is currently active.
  if (newToOldColumn.size() != newTable.columns.size() ||
      addedValues.size() != newTable.columns.size())
    {
      setError(error, "invalid local-lite ALTER TABLE row mapping");
      return false;
    }

  std::vector<LocalLiteRow> rows;
  if (!scanRows(oldTable, &rows, error))
    return false;
  LocalLiteTableDef persistedTable = newTable;
  std::vector<LocalLiteRowMutation> mutations(rows.size());
  for (size_t i = 0; i < rows.size(); i++)
    {
      mutations[i].before = rows[i];
      if (!oldTable.primaryKeyColumns.empty() &&
          newTable.primaryKeyColumns.empty())
        mutations[i].before.rowId = persistedTable.nextRowId++;
      if (!LocalLiteRebuildBinaryRow(oldTable, newTable, rows[i].value,
                                     newToOldColumn, addedValues,
                                     &mutations[i].after, error))
        return false;
    }
  if (!LocalLiteStorageManager::instance().replaceTableDefinition(
          oldTable, persistedTable, mutations, error))
    return false;
  publishOccTableChange(txnContext, oldTable.objectUid);
  return true;
}

static bool validateLocalLiteChildRI(
    LocalLiteRocksDBStore *store, const LocalLiteTableDef &table,
    const std::vector<std::string> &rows, std::string *error)
{
  for (size_t r = 0; r < table.riConstraints.size(); r++)
    {
      const LocalLiteRIDef &ri = table.riConstraints[r];
      LocalLiteTableDef parent;
      if (!store->loadTable(ri.referencedCatalog, ri.referencedSchema,
                            ri.referencedTable, &parent, error))
        return false;
      std::vector<LocalLiteRow> parentRows;
      if (!store->scanRows(parent, &parentRows, error))
        return false;
      std::set<std::string> parentKeys;
      for (size_t i = 0; i < parentRows.size(); i++)
        {
          std::string key;
          bool hasKey = false;
          if (!LocalLiteBuildConstraintKey(parent, parentRows[i].value,
                                           ri.referencedColumns, &key,
                                           &hasKey, error))
            return false;
          if (hasKey) parentKeys.insert(key);
        }
      // Self-referencing rows inserted by the same statement are also part
      // of the statement's final parent image.
      if (parent.objectUid == table.objectUid)
        for (size_t i = 0; i < rows.size(); i++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildConstraintKey(parent, rows[i],
                                             ri.referencedColumns, &key,
                                             &hasKey, error))
              return false;
            if (hasKey) parentKeys.insert(key);
          }
      for (size_t i = 0; i < rows.size(); i++)
        {
          std::string key;
          bool hasKey = false;
          if (!LocalLiteBuildConstraintKey(table, rows[i],
                                           ri.referencingColumns, &key,
                                           &hasKey, error))
            return false;
          if (hasKey && parentKeys.find(key) == parentKeys.end())
            {
              setError(error, "referential integrity constraint " + ri.name +
                              " is violated");
              return false;
            }
        }
    }
  return true;
}

static bool validateLocalLiteParentRI(
    LocalLiteRocksDBStore *store, const LocalLiteTableDef &parent,
    const std::vector<std::string> &oldRows,
    const std::vector<std::string> &newRows, std::string *error)
{
  std::vector<LocalLiteTableDef> tables;
  if (!store->listTables("", "", &tables, error))
    return false;
  for (size_t t = 0; t < tables.size(); t++)
    for (size_t r = 0; r < tables[t].riConstraints.size(); r++)
      {
        const LocalLiteRIDef &ri = tables[t].riConstraints[r];
        if (ri.referencedCatalog != parent.catalog ||
            ri.referencedSchema != parent.schema ||
            ri.referencedTable != parent.name)
          continue;
        std::set<std::string> removedKeys;
        for (size_t i = 0; i < oldRows.size(); i++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildConstraintKey(parent, oldRows[i],
                                             ri.referencedColumns, &key,
                                             &hasKey, error))
              return false;
            if (hasKey) removedKeys.insert(key);
          }
        for (size_t i = 0; i < newRows.size(); i++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildConstraintKey(parent, newRows[i],
                                             ri.referencedColumns, &key,
                                             &hasKey, error))
              return false;
            if (hasKey) removedKeys.erase(key);
          }
        if (removedKeys.empty()) continue;
        std::vector<LocalLiteRow> childRows;
        if (!store->scanRows(tables[t], &childRows, error))
          return false;
        for (size_t i = 0; i < childRows.size(); i++)
          {
            std::string key;
            bool hasKey = false;
            if (!LocalLiteBuildConstraintKey(tables[t], childRows[i].value,
                                             ri.referencingColumns, &key,
                                             &hasKey, error))
              return false;
            if (hasKey && removedKeys.find(key) != removedKeys.end())
              {
                setError(error, "referential integrity constraint " + ri.name +
                                " is violated");
                return false;
              }
          }
      }
  return true;
}

bool LocalLiteRocksDBStore::validateReferentialIntegrity(
    const LocalLiteTableDef &table, std::string *error)
{
  if (!open(error))
    return false;
  std::vector<LocalLiteRow> storedRows;
  if (!scanRows(table, &storedRows, error))
    return false;
  std::vector<std::string> rows;
  rows.reserve(storedRows.size());
  for (size_t i = 0; i < storedRows.size(); i++)
    rows.push_back(storedRows[i].value);
  return validateLocalLiteChildRI(this, table, rows, error);
}

bool LocalLiteRocksDBStore::insertRow(const LocalLiteTableDef &table,
                                      const std::string &encodedRow,
                                      uint64_t *rowId,
                                      std::string *error)
{
  if (!open(error))
    return false;

  std::vector<std::string> rows(1, encodedRow);
  if (!validateLocalLiteChildRI(this, table, rows, error))
    return false;

  if (!LocalLiteStorageManager::instance().insertRow(table, encodedRow,
                                                     rowId, error))
    return false;
  return invalidateTableStats(table, error);
}

bool LocalLiteRocksDBStore::updateRows(
    const LocalLiteTableDef &table,
    const std::vector<LocalLiteRowMutation> &mutations,
    std::string *error)
{
  if (!open(error))
    return false;
  std::vector<std::string> oldRows;
  std::vector<std::string> newRows;
  for (size_t i = 0; i < mutations.size(); i++)
    {
      oldRows.push_back(mutations[i].before.value);
      newRows.push_back(mutations[i].after);
    }
  if (!validateLocalLiteChildRI(this, table, newRows, error) ||
      !validateLocalLiteParentRI(this, table, oldRows, newRows, error))
    return false;
  if (!LocalLiteStorageManager::instance().updateRows(table, mutations,
                                                       error))
    return false;
  return invalidateTableStats(table, error);
}

bool LocalLiteRocksDBStore::deleteRows(
    const LocalLiteTableDef &table,
    const std::vector<LocalLiteRow> &rows,
    std::string *error)
{
  if (!open(error))
    return false;
  std::vector<std::string> oldRows;
  for (size_t i = 0; i < rows.size(); i++)
    oldRows.push_back(rows[i].value);
  std::vector<std::string> noNewRows;
  if (!validateLocalLiteParentRI(this, table, oldRows, noNewRows, error))
    return false;
  if (!LocalLiteStorageManager::instance().deleteRows(table, rows, error))
    return false;
  return invalidateTableStats(table, error);
}

bool LocalLiteRocksDBStore::collectTableStats(
    const LocalLiteTableDef &table, LocalLiteTableStatsDef *stats,
    std::string *error)
{
  if (!stats || !open(error))
    return false;
  std::vector<LocalLiteRow> rows;
  if (!scanRows(table, &rows, error))
    return false;

  LocalLiteTableStatsDef collected;
  collected.rowCount = rows.size();
  collected.analyzedAt = static_cast<uint64_t>(time(NULL));
  collected.columns.resize(table.columns.size());
  for (size_t i = 0; i < table.columns.size(); i++)
    {
      collected.columns[i].columnName = table.columns[i].name;
      collected.columns[i].rowCount = rows.size();
      std::set<std::string> distinctValues;
      for (size_t r = 0; r < rows.size(); r++)
        {
          bool isNull = false;
          if (!LocalLiteBinaryRowIsNull(table, rows[r].value, i, &isNull,
                                        error))
            return false;
          if (isNull)
            continue;
          std::string valueKey;
          bool hasKey = false;
          std::vector<size_t> columnOnly(1, i);
          if (!LocalLiteBuildConstraintKey(table, rows[r].value,
                                           columnOnly, &valueKey, &hasKey,
                                           error))
            return false;
          if (hasKey)
            distinctValues.insert(valueKey);
        }
      collected.columns[i].distinctCount = distinctValues.size();
    }
  for (size_t r = 0; r < rows.size(); r++)
    for (size_t c = 0; c < table.columns.size(); c++)
      {
        bool isNull = false;
        if (!LocalLiteBinaryRowIsNull(table, rows[r].value, c, &isNull,
                                      error))
          return false;
        if (isNull)
          collected.columns[c].nullCount++;
      }

  const std::string key = statsKey(table.catalog, table.schema, table.name);
  const std::string encoded = encodeStats(collected);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_put(LocalLiteStorageManager::instance().catalogDb(), writeOptions,
              key.data(), key.size(), encoded.data(), encoded.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  if (!checkRocksError(err, "write local-lite table statistics", error))
    return false;
  *stats = collected;
  return true;
}

bool LocalLiteRocksDBStore::loadTableStats(
    const std::string &catalog, const std::string &schema,
    const std::string &name, LocalLiteTableStatsDef *stats, bool *found,
    std::string *error)
{
  if (!stats || !found || !open(error))
    return false;
  *found = false;
  const std::string key = statsKey(catalog, schema, name);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(LocalLiteStorageManager::instance().catalogDb(),
                            readOptions, key.data(), key.size(), &valueLen,
                            &err);
  rocksdb_readoptions_destroy(readOptions);
  if (!checkRocksError(err, "read local-lite table statistics", error))
    return false;
  if (!value)
    return true;
  std::string encoded(value, valueLen);
  rocksdb_free(value);
  if (!decodeStats(encoded, stats, error))
    return false;
  *found = true;
  return true;
}

bool LocalLiteRocksDBStore::invalidateTableStats(
    const LocalLiteTableDef &table, std::string *error)
{
  if (!open(error))
    return false;
  const std::string key = statsKey(table.catalog, table.schema, table.name);
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  char *err = NULL;
  rocksdb_delete(LocalLiteStorageManager::instance().catalogDb(),
                 writeOptions, key.data(), key.size(), &err);
  rocksdb_writeoptions_destroy(writeOptions);
  return checkRocksError(err, "invalidate local-lite table statistics", error);
}

bool LocalLiteRocksDBStore::getRowByKey(const LocalLiteTableDef &table,
                                        const std::string &storageKey,
                                        LocalLiteRow *row,
                                        bool *found,
                                        std::string *error)
{
  return getRowByKey(table, storageKey, NULL, 0, row, found, error);
}

static bool getRowByKeyAtSnapshot(rocksdb_t *db,
                                  const rocksdb_snapshot_t *snapshot,
                                  const rocksdb_readoptions_t *sharedOptions,
                                  const std::string &storageKey,
                                  LocalLiteRow *row,
                                  bool *found,
                                  std::string *error)
{
  if (found)
    *found = false;
  if (!row || !found)
    {
      setError(error, "missing local-lite get row output");
      return false;
    }

  rocksdb_readoptions_t *ownedOptions = NULL;
  const rocksdb_readoptions_t *readOptions = sharedOptions;
  if (!readOptions)
    {
      ownedOptions = rocksdb_readoptions_create();
      readOptions = ownedOptions;
      if (snapshot)
        rocksdb_readoptions_set_snapshot(ownedOptions, snapshot);
      rocksdb_readoptions_set_fill_cache(ownedOptions, 1);
    }
  char *err = NULL;
  size_t valueLen = 0;
  char *value = rocksdb_get(db, readOptions,
                            storageKey.data(), storageKey.size(),
                            &valueLen, &err);
  if (ownedOptions)
    rocksdb_readoptions_destroy(ownedOptions);
  if (!checkRocksError(err, "read local-lite row", error))
    return false;
  if (!value)
    return true;

  std::string encodedValue(value, valueLen);
  rocksdb_free(value);

  std::string encodedRow;
  if (decodeRowValue(encodedValue, &encodedRow, NULL))
    {
      row->rowId = 0;
      if (storageKey.size() == 8)
        {
          size_t offset = 0;
          row->rowId = readUint64(storageKey, &offset);
        }
      row->value = encodedRow;
      *found = true;
      return true;
    }

  if (storageKey.size() > 0 && storageKey[0] == 'U')
    {
      std::string referencedKey = encodedValue;
      return getRowByKeyAtSnapshot(db, snapshot, sharedOptions, referencedKey,
                                   row, found, error);
    }

  setError(error, "invalid local-lite row format");
  return false;
}

bool LocalLiteRocksDBStore::getRowByKey(
    const LocalLiteTableDef &table,
    const std::string &storageKey,
    const void *statementOwner,
    uint64_t statementExecutionId,
    LocalLiteRow *row,
    bool *found,
    std::string *error)
{
  if (!open(error))
    return false;

  const std::string path = tablePath(table);
  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(path, false,
                                                               error);
  if (!db)
    return false;

  const rocksdb_snapshot_t *snapshot = NULL;
  const rocksdb_readoptions_t *readOptions = NULL;
  if (statementOwner &&
      !LocalLiteStorageManager::instance().getStatementSnapshot(
          path, db, statementOwner, statementExecutionId, &snapshot, error))
    return false;
  if (statementOwner &&
      !LocalLiteStorageManager::instance().getStatementReadOptions(
          path, db, statementOwner, statementExecutionId, &readOptions,
          error))
    return false;

  return getRowByKeyAtSnapshot(db, snapshot, readOptions, storageKey,
                               row, found, error);
}

bool LocalLiteRocksDBStore::scanRows(const LocalLiteTableDef &table,
                                     std::vector<LocalLiteRow> *rows,
                                     std::string *error)
{
  return scanRows(table, NULL, 0, rows, error);
}

bool LocalLiteRocksDBStore::scanIndexPrefix(
    const LocalLiteTableDef &table,
    const std::string &physicalPrefix,
    std::vector<LocalLiteRow> *rows,
    std::string *error)
{
  if (physicalPrefix.size() < 9 || physicalPrefix[0] != 'I')
    {
      setError(error, "invalid local-lite secondary index prefix");
      return false;
    }
  std::string endKey = physicalPrefix;
  size_t pos = endKey.size();
  while (pos > 0 &&
         static_cast<unsigned char>(endKey[pos - 1]) == 0xff)
    pos--;
  if (pos == 0)
    {
      setError(error, "local-lite index prefix has no upper bound");
      return false;
    }
  endKey.resize(pos);
  endKey[pos - 1] = static_cast<char>(
      static_cast<unsigned char>(endKey[pos - 1]) + 1);
  return scanIndexRange(table, physicalPrefix, endKey, rows, error);
}

bool LocalLiteRocksDBStore::scanIndexRange(
    const LocalLiteTableDef &table,
    const std::string &startKey,
    const std::string &endKey,
    std::vector<LocalLiteRow> *rows,
    std::string *error)
{
  return scanIndexRange(table, startKey, endKey, NULL, 0, rows, error);
}

bool LocalLiteRocksDBStore::scanIndexRange(
    const LocalLiteTableDef &table,
    const std::string &startKey,
    const std::string &endKey,
    const void *statementOwner,
    uint64_t statementExecutionId,
    std::vector<LocalLiteRow> *rows,
    std::string *error)
{
  rows->clear();
  if (startKey.size() < 9 || startKey[0] != 'I' ||
      endKey.empty() || endKey <= startKey)
    {
      setError(error, "invalid local-lite secondary index range");
      return false;
    }
  if (!open(error))
    return false;

  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(
      tablePath(table), false, error);
  if (!db)
    return false;

  const bool ownsSnapshot = (statementOwner == NULL);
  const rocksdb_snapshot_t *snapshot = NULL;
  const rocksdb_readoptions_t *sharedReadOptions = NULL;
  if (ownsSnapshot)
    snapshot = rocksdb_create_snapshot(db);
  else if (!LocalLiteStorageManager::instance().getStatementSnapshot(
               tablePath(table), db, statementOwner, statementExecutionId,
               &snapshot, error))
    return false;
  if (!snapshot)
    {
      setError(error, "create local-lite RocksDB index snapshot failed");
      return false;
    }
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_readoptions_set_snapshot(readOptions, snapshot);
  rocksdb_readoptions_set_fill_cache(readOptions, 1);
  if (!ownsSnapshot &&
      !LocalLiteStorageManager::instance().getStatementReadOptions(
          tablePath(table), db, statementOwner, statementExecutionId,
          &sharedReadOptions, error))
    {
      rocksdb_readoptions_destroy(readOptions);
      return false;
    }
  if (sharedReadOptions)
    {
      rocksdb_readoptions_destroy(readOptions);
      readOptions = const_cast<rocksdb_readoptions_t *>(sharedReadOptions);
    }
  rocksdb_iterator_t *it = rocksdb_create_iterator(db, readOptions);
  std::map<std::string, LocalLiteRow> rowsByStorageKey;
  std::vector<std::string> baseKeys;
  size_t coveringRows = 0;
  size_t baseLookups = 0;
  for (rocksdb_iter_seek(it, startKey.data(), startKey.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      size_t valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      std::string physicalKey(rawKey, keyLen);
      if (physicalKey >= endKey)
        break;

      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      std::string rowKey;
      std::string encodedRow;
      bool covering = false;
      if (!decodeIndexValue(std::string(rawValue, valueLen), &rowKey,
                            &encodedRow, &covering, error))
        {
          rocksdb_iter_destroy(it);
          if (!sharedReadOptions)
            rocksdb_readoptions_destroy(readOptions);
          if (ownsSnapshot)
            rocksdb_release_snapshot(db, snapshot);
          return false;
        }
      if (rowsByStorageKey.find(rowKey) != rowsByStorageKey.end())
        continue;

      LocalLiteRow row;
      if (covering)
        {
          row.rowId = 0;
          if (rowKey.size() == 8)
            {
              size_t offset = 0;
              row.rowId = readUint64(rowKey, &offset);
            }
          row.value = encodedRow;
          coveringRows++;
          rowsByStorageKey.insert(std::make_pair(rowKey, row));
        }
      else
        {
          baseKeys.push_back(rowKey);
        }
    }

  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  if (!checkRocksError(err, "scan local-lite secondary index", error))
    {
      if (!sharedReadOptions)
        rocksdb_readoptions_destroy(readOptions);
      if (ownsSnapshot)
        rocksdb_release_snapshot(db, snapshot);
      return false;
    }

  if (!baseKeys.empty())
    {
      std::vector<const char *> keyPointers(baseKeys.size());
      std::vector<size_t> keyLengths(baseKeys.size());
      std::vector<char *> values(baseKeys.size(), NULL);
      std::vector<size_t> valueLengths(baseKeys.size(), 0);
      std::vector<char *> errors(baseKeys.size(), NULL);
      for (size_t i = 0; i < baseKeys.size(); i++)
        {
          keyPointers[i] = baseKeys[i].data();
          keyLengths[i] = baseKeys[i].size();
        }
      rocksdb_multi_get(db, readOptions, baseKeys.size(), &keyPointers[0],
                        &keyLengths[0], &values[0], &valueLengths[0],
                        &errors[0]);
      baseLookups = baseKeys.size();
      for (size_t i = 0; i < baseKeys.size(); i++)
        {
          if (errors[i])
            {
              const std::string message(errors[i]);
              rocksdb_free(errors[i]);
              setError(error, "read local-lite secondary index base row: " +
                       message);
              for (size_t j = i + 1; j < baseKeys.size(); j++)
                if (errors[j]) rocksdb_free(errors[j]);
              for (size_t j = 0; j < baseKeys.size(); j++)
                if (values[j]) rocksdb_free(values[j]);
              if (!sharedReadOptions)
                rocksdb_readoptions_destroy(readOptions);
              if (ownsSnapshot)
                rocksdb_release_snapshot(db, snapshot);
              return false;
            }
          LocalLiteRow row;
          bool found = values[i] != NULL;
          if (found)
            {
              std::string encodedValue(values[i], valueLengths[i]);
              rocksdb_free(values[i]);
              if (!decodeRowValue(encodedValue, &row.value, NULL))
                {
                  // Unique-key entries may be indirections. Preserve the
                  // existing recursive path for that representation.
                  if (!getRowByKeyAtSnapshot(
                          db, snapshot, sharedReadOptions, baseKeys[i],
                          &row, &found, error))
                    {
                      if (!sharedReadOptions)
                        rocksdb_readoptions_destroy(readOptions);
                      if (ownsSnapshot)
                        rocksdb_release_snapshot(db, snapshot);
                      return false;
                    }
                }
              else
                {
                  row.rowId = 0;
                  if (baseKeys[i].size() == 8)
                    {
                      size_t offset = 0;
                      row.rowId = readUint64(baseKeys[i], &offset);
                    }
                }
            }
          if (!found)
            {
              setError(error,
                       "local-lite secondary index references a missing row");
              if (!sharedReadOptions)
                rocksdb_readoptions_destroy(readOptions);
              if (ownsSnapshot)
                rocksdb_release_snapshot(db, snapshot);
              return false;
            }
          rowsByStorageKey.insert(std::make_pair(baseKeys[i], row));
        }
    }
  if (!sharedReadOptions)
    rocksdb_readoptions_destroy(readOptions);
  if (ownsSnapshot)
    rocksdb_release_snapshot(db, snapshot);
  for (std::map<std::string, LocalLiteRow>::const_iterator row =
           rowsByStorageKey.begin(); row != rowsByStorageKey.end(); ++row)
    rows->push_back(row->second);
  const char *trace = getenv("TRAF_LOCAL_LITE_TRACE_SCAN");
  if (trace && trace[0])
    {
      std::string indexName;
      if (startKey.size() >= 9 && startKey[0] == 'I')
        {
          size_t uidOffset = 1;
          uint64_t indexUid = readUint64(startKey, &uidOffset);
          for (size_t i = 0; i < table.secondaryIndexes.size(); i++)
            if (table.secondaryIndexes[i].objectUid == indexUid)
              {
                indexName = table.secondaryIndexes[i].name;
                break;
              }
        }
      fprintf(stderr,
              "LOCAL_LITE_INDEX_BOUNDS index=%s start=%s end=%s candidates=%lu\n",
              indexName.c_str(), localLiteHexKey(startKey).c_str(),
              localLiteHexKey(endKey).c_str(),
              static_cast<unsigned long>(rows->size()));
    }
  if (trace && trace[0])
    fprintf(stderr,
            "LOCAL_LITE_INDEX_ONLY covering=%lu base_lookups=%lu\n",
            static_cast<unsigned long>(coveringRows),
            static_cast<unsigned long>(baseLookups));
  return true;
}

bool LocalLiteRocksDBStore::scanPrimaryRange(
    const LocalLiteTableDef &table,
    const std::string &startKey,
    const std::string &endKey,
    std::vector<LocalLiteRow> *rows,
    std::string *error)
{
  return scanPrimaryRange(table, startKey, endKey, NULL, 0, rows, error);
}

bool LocalLiteRocksDBStore::scanPrimaryRange(
    const LocalLiteTableDef &table,
    const std::string &startKey,
    const std::string &endKey,
    const void *statementOwner,
    uint64_t statementExecutionId,
    std::vector<LocalLiteRow> *rows,
    std::string *error)
{
  rows->clear();
  if (startKey.empty() || startKey[0] != 'P' || endKey <= startKey ||
      !open(error))
    {
      if (error && error->empty())
        *error = "invalid local-lite primary key range";
      return false;
    }
  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(
      tablePath(table), false, error);
  if (!db)
    return false;
  const bool ownsSnapshot = statementOwner == NULL;
  const rocksdb_snapshot_t *snapshot = ownsSnapshot
      ? rocksdb_create_snapshot(db) : NULL;
  if (!ownsSnapshot &&
      !LocalLiteStorageManager::instance().getStatementSnapshot(
          tablePath(table), db, statementOwner, statementExecutionId,
          &snapshot, error))
    return false;
  if (!snapshot)
    {
      setError(error, "create local-lite RocksDB primary range snapshot failed");
      return false;
    }
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_readoptions_set_snapshot(readOptions, snapshot);
  rocksdb_iterator_t *it = rocksdb_create_iterator(db, readOptions);
  for (rocksdb_iter_seek(it, startKey.data(), startKey.size());
       rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      size_t valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      std::string key(rawKey, keyLen);
      if (key >= endKey)
        break;
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      LocalLiteRow row;
      row.rowId = 0;
      if (!decodeRowValue(std::string(rawValue, valueLen), &row.value, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          if (ownsSnapshot) rocksdb_release_snapshot(db, snapshot);
          return false;
        }
      rows->push_back(row);
    }
  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (ownsSnapshot) rocksdb_release_snapshot(db, snapshot);
  return checkRocksError(err, "scan local-lite primary key range", error);
}

bool LocalLiteRocksDBStore::scanRows(const LocalLiteTableDef &table,
                                     const void *statementOwner,
                                     uint64_t statementExecutionId,
                                     std::vector<LocalLiteRow> *rows,
                                     std::string *error)
{
  rows->clear();

  if (!open(error))
    return false;

  if (isLocalLiteMetadataTable(table.catalog, table.schema, table.name))
    {
      std::vector<LocalLiteTableDef> tables;
      if (!listTables("", "", &tables, error))
        return false;
      return localLiteBuildMetadataRows(table, tables, rows, error);
    }

  const std::string path = tablePath(table);
  rocksdb_t *db = LocalLiteStorageManager::instance().openTable(path, false,
                                                               error);
  if (!db)
    return false;

  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  const bool ownsSnapshot = (statementOwner == NULL);
  const rocksdb_snapshot_t *snapshot = NULL;
  if (ownsSnapshot)
    snapshot = rocksdb_create_snapshot(db);
  else if (!LocalLiteStorageManager::instance().getStatementSnapshot(
               path, db, statementOwner, statementExecutionId,
               &snapshot, error))
    {
      rocksdb_readoptions_destroy(readOptions);
      return false;
    }
  if (!snapshot)
    {
      rocksdb_readoptions_destroy(readOptions);
      setError(error, "create local-lite RocksDB scan snapshot failed");
      return false;
    }
  rocksdb_readoptions_set_snapshot(readOptions, snapshot);
  rocksdb_iterator_t *it = rocksdb_create_iterator(db, readOptions);
  for (rocksdb_iter_seek_to_first(it); rocksdb_iter_valid(it); rocksdb_iter_next(it))
    {
      size_t keyLen = 0;
      size_t valueLen = 0;
      const char *rawKey = rocksdb_iter_key(it, &keyLen);
      const char *rawValue = rocksdb_iter_value(it, &valueLen);
      std::string key(rawKey, keyLen);
      if (key.size() > 0 &&
          (key[0] == 'U' || key[0] == 'I' || key[0] == 'J'))
        continue;
      std::string value(rawValue, valueLen);
      size_t offset = 0;
      LocalLiteRow row;
      row.rowId = (key.size() == 8) ? readUint64(key, &offset) : 0;
      if (!decodeRowValue(value, &row.value, error))
        {
          rocksdb_iter_destroy(it);
          rocksdb_readoptions_destroy(readOptions);
          if (ownsSnapshot)
            rocksdb_release_snapshot(db, snapshot);
          return false;
        }
      rows->push_back(row);
    }

  char *err = NULL;
  rocksdb_iter_get_error(it, &err);
  rocksdb_iter_destroy(it);
  rocksdb_readoptions_destroy(readOptions);
  if (ownsSnapshot)
    rocksdb_release_snapshot(db, snapshot);
  if (!checkRocksError(err, "scan local-lite rows", error))
    return false;
  return true;
}

LocalLiteTxn::LocalLiteTxn(LocalLiteRocksDBStore *store,
                           LocalLiteTxnContext *txnContext,
                           const void *statementOwner,
                           uint64_t statementExecutionId)
  : store_(store),
    txnContext_(txnContext),
    statementOwner_(statementOwner),
    statementExecutionId_(statementExecutionId)
{
}

bool LocalLiteTxn::insertRow(const LocalLiteTableDef &table,
                             const std::string &encodedRow,
                             uint64_t *rowId,
                             std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }

  return txnContext_->insertRow(store_, table, encodedRow, rowId, error);
}

bool LocalLiteTxn::insertRows(
    const LocalLiteTableDef &table,
    const std::vector<std::string> &encodedRows,
    std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext_->insertRows(store_, table, encodedRows, error);
}

bool LocalLiteTxn::upsertRow(const LocalLiteTableDef &table,
                             const std::string &encodedRow,
                             uint64_t *rowId,
                             std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  if (table.primaryKeyColumns.empty())
    {
      setError(error, "local-lite UPSERT requires a primary key");
      return false;
    }

  std::string key;
  if (!LocalLiteBuildPrimaryKey(table, encodedRow, &key, error))
    return false;

  LocalLiteRow existing;
  bool found = false;
  if (!getRowByKey(table, key, &existing, &found, error))
    return false;
  if (!found)
    return insertRow(table, encodedRow, rowId, error);

  LocalLiteRowMutation mutation;
  mutation.before = existing;
  mutation.after = encodedRow;
  std::vector<LocalLiteRowMutation> mutations(1, mutation);
  if (!updateRows(table, mutations, error))
    return false;
  if (rowId)
    *rowId = existing.rowId;
  return true;
}

bool LocalLiteTxn::updateRows(
    const LocalLiteTableDef &table,
    const std::vector<LocalLiteRowMutation> &mutations,
    std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext_->updateRows(store_, table, mutations, error);
}

bool LocalLiteTxn::deleteRows(const LocalLiteTableDef &table,
                              const std::vector<LocalLiteRow> &rows,
                              std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext_->deleteRows(store_, table, rows, error);
}

bool LocalLiteTxn::scanRows(const LocalLiteTableDef &table,
                            std::vector<LocalLiteRow> *rows,
                            std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }

  return txnContext_->scanRows(store_, table, statementOwner_,
                               statementExecutionId_, rows, error);
}

bool LocalLiteTxn::scanIndexPrefix(const LocalLiteTableDef &table,
                                   const std::string &physicalPrefix,
                                   std::vector<LocalLiteRow> *rows,
                                   std::string *error)
{
  if (!store_ || !txnContext_)
    {
      setError(error, "local-lite index scan missing transaction context");
      return false;
    }
  return txnContext_->scanIndexPrefix(store_, table, physicalPrefix,
                                      statementOwner_, statementExecutionId_,
                                      rows, error);
}

bool LocalLiteTxn::scanIndexRange(const LocalLiteTableDef &table,
                                  const std::string &startKey,
                                  const std::string &endKey,
                                  std::vector<LocalLiteRow> *rows,
                                  std::string *error)
{
  if (!store_ || !txnContext_)
    {
      setError(error, "local-lite index scan missing transaction context");
      return false;
    }
  return txnContext_->scanIndexRange(store_, table, startKey, endKey,
                                     statementOwner_, statementExecutionId_,
                                     rows, error);
}

bool LocalLiteTxn::scanPrimaryRange(const LocalLiteTableDef &table,
                                    const std::string &startKey,
                                    const std::string &endKey,
                                    std::vector<LocalLiteRow> *rows,
                                    std::string *error)
{
  if (!store_ || !txnContext_)
    {
      setError(error, "local-lite primary scan missing transaction context");
      return false;
    }
  return txnContext_->scanPrimaryRange(store_, table, startKey, endKey,
                                       statementOwner_, statementExecutionId_,
                                       rows, error);
}

bool LocalLiteTxn::getRowByKey(const LocalLiteTableDef &table,
                               const std::string &storageKey,
                               LocalLiteRow *row,
                               bool *found,
                               std::string *error)
{
  if (!store_)
    {
      setError(error, "local-lite transaction missing store");
      return false;
    }
  if (!txnContext_)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }

  return txnContext_->getRowByKey(store_, table, storageKey, statementOwner_,
                                  statementExecutionId_, row, found, error);
}

LocalLiteTxnContext *LocalLiteTxnManager::createContext()
{
  return new LocalLiteTxnContext();
}

void LocalLiteTxnManager::resetContext(LocalLiteTxnContext *txnContext)
{
  if (txnContext)
    txnContext->reset();
}

void LocalLiteTxnManager::destroyContext(LocalLiteTxnContext *txnContext)
{
  delete txnContext;
}

bool LocalLiteTxnManager::begin(LocalLiteTxnContext *txnContext,
                                std::string *error)
{
  return beginForExecutor(txnContext, INVALID_EXECUTOR_TXN_ID, error);
}

bool LocalLiteTxnManager::beginForExecutor(LocalLiteTxnContext *txnContext,
                                           int64_t executorTxnId,
                                           std::string *error)
{
  if (!txnContext)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext->begin(executorTxnId, error);
}

bool LocalLiteTxnManager::commit(LocalLiteTxnContext *txnContext,
                                 std::string *error)
{
  return commitForExecutor(txnContext, INVALID_EXECUTOR_TXN_ID, error);
}

bool LocalLiteTxnManager::commitForExecutor(LocalLiteTxnContext *txnContext,
                                            int64_t executorTxnId,
                                            std::string *error)
{
  if (!txnContext)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext->commit(executorTxnId, error);
}

bool LocalLiteTxnManager::rollback(LocalLiteTxnContext *txnContext,
                                   std::string *error)
{
  return rollbackForExecutor(txnContext, INVALID_EXECUTOR_TXN_ID, error);
}

bool LocalLiteTxnManager::rollbackForExecutor(LocalLiteTxnContext *txnContext,
                                              int64_t executorTxnId,
                                              std::string *error)
{
  if (!txnContext)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext->rollback(executorTxnId, error);
}

bool LocalLiteTxnManager::prepareDDLForExecutor(
    LocalLiteTxnContext *txnContext,
    int64_t executorTxnId,
    std::string *error)
{
  if (!txnContext)
    {
      setError(error, "local-lite transaction missing session context");
      return false;
    }
  return txnContext->prepareDDL(executorTxnId, error);
}

bool LocalLiteTxnManager::refreshDDLSequenceForExecutor(
    LocalLiteTxnContext *txnContext,
    int64_t executorTxnId,
    std::string *error)
{
  return !txnContext || txnContext->refreshDDLSequence(executorTxnId, error);
}

bool LocalLiteTxnManager::active(LocalLiteTxnContext *txnContext)
{
  return txnContext && txnContext->active();
}

uint64_t LocalLiteTxnManager::currentLocalTxnId(
    LocalLiteTxnContext *txnContext)
{
  return txnContext ? txnContext->currentLocalTxnId() : 0;
}

int64_t LocalLiteTxnManager::currentExecutorTxnId(
    LocalLiteTxnContext *txnContext)
{
  return txnContext ? txnContext->currentExecutorTxnId()
                    : INVALID_EXECUTOR_TXN_ID;
}

bool LocalLiteTxnManager::occState(LocalLiteTxnContext *txnContext,
                                   LocalLiteOccState *state)
{
  return txnContext && txnContext->occState(state);
}

std::string LocalLiteOccMetricsJson()
{
  LocalLiteMutexGuard guard(LocalLiteStorageManager::instance().mutex());
  std::ostringstream out;
  out << "{\"transactions_started\":"
      << localLiteOccMetrics.transactionsStarted
      << ",\"transactions_committed\":"
      << localLiteOccMetrics.transactionsCommitted
      << ",\"transactions_aborted_conflict\":"
      << localLiteOccMetrics.transactionsAbortedConflict
      << ",\"point_reads\":" << localLiteOccMetrics.pointReads
      << ",\"missing_point_reads\":"
      << localLiteOccMetrics.missingPointReads
      << ",\"range_reads\":" << localLiteOccMetrics.rangeReads
      << ",\"full_scans\":" << localLiteOccMetrics.fullScans
      << ",\"primary_range_reads\":"
      << localLiteOccMetrics.primaryRangeReads
      << ",\"index_range_reads\":"
      << localLiteOccMetrics.indexRangeReads
      << ",\"read_ranges\":" << localLiteOccMetrics.readRanges
      << ",\"write_keys\":" << localLiteOccMetrics.writeKeys
      << ",\"validation_candidates\":"
      << localLiteOccMetrics.validationCandidates
      << ",\"validation_conflicts\":"
      << localLiteOccMetrics.validationConflicts
      << ",\"point_validation_conflicts\":"
      << localLiteOccMetrics.pointValidationConflicts
      << ",\"range_validation_conflicts\":"
      << localLiteOccMetrics.rangeValidationConflicts
      << ",\"full_scan_validation_conflicts\":"
      << localLiteOccMetrics.fullScanValidationConflicts
      << ",\"history_overflow_aborts\":"
      << localLiteOccMetrics.historyOverflowAborts
      << ",\"commit_latch_wait_us\":"
      << localLiteOccMetrics.commitLatchWaitUs
      << ",\"validation_latency_us\":"
      << localLiteOccMetrics.validationLatencyUs
      << ",\"referential_validation_latency_us\":"
      << localLiteOccMetrics.referentialValidationLatencyUs
      << ",\"batch_build_latency_us\":"
      << localLiteOccMetrics.batchBuildLatencyUs
      << ",\"publication_latency_us\":"
      << localLiteOccMetrics.publicationLatencyUs
      << ",\"visibility_publication_latency_us\":"
      << localLiteOccMetrics.visibilityPublicationLatencyUs
      << ",\"commit_intent_wait_us\":"
      << localLiteOccMetrics.commitIntentWaitUs
      << ",\"physical_commits\":"
      << localLiteOccMetrics.physicalCommits
      << ",\"maximum_physical_commit_overlap\":"
      << std::max(localLiteOccMetrics.maximumPhysicalCommitOverlap,
                  localLiteCommitCoordinator.maximumOverlap())
      << ",\"synchronous_commit\":"
      << (localLiteSynchronousCommit() ? "true" : "false") << "}";
  return out.str();
}

void LocalLiteOccMetricsReset()
{
  LocalLiteMutexGuard guard(LocalLiteStorageManager::instance().mutex());
  localLiteOccMetrics = LocalLiteOccMetrics();
  localLiteCommitCoordinator.resetMetrics();
}

void LocalLiteTxnManager::beginStatement(LocalLiteTxnContext *txnContext,
                                         const void *statementOwner,
                                         uint64_t statementExecutionId)
{
  if (txnContext)
    txnContext->beginStatement(statementOwner, statementExecutionId);
}

void LocalLiteTxnManager::endStatement(LocalLiteTxnContext *txnContext,
                                       const void *statementOwner,
                                       uint64_t statementExecutionId)
{
  if (txnContext)
    txnContext->endStatement(statementOwner, statementExecutionId);
}

#endif
