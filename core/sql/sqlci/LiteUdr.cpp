// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LITE

#include "LiteUdr.h"
#include "LiteRocksDBStore.h"
#include "SqlciEnv.h"
#include "SQLCLIdev.h"
#include "sqludr.h"

#include <dlfcn.h>
#include <errno.h>
#include <sys/wait.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum UdrKind { UDR_FUNCTION = 1, UDR_PROCEDURE = 2, UDR_TABLE_FUNCTION = 3 };

struct UdrParam
{
  std::string name;
  std::string type;
  bool output;
  UdrParam() : output(false) {}
};

struct UdrDef
{
  std::string catalog;
  std::string schema;
  std::string name;
  int kind;
  std::string language;
  std::string symbol;
  std::string library;
  std::string owner;
  std::vector<UdrParam> params;
  std::vector<UdrParam> returns;
  int resultSets;
  bool deterministic;
  bool isolate;
  std::string sqlAccess;
  UdrDef() : kind(UDR_FUNCTION), resultSets(0), deterministic(false), isolate(false) {}
};

static std::string trim(const std::string &s)
{
  size_t b = 0;
  while (b < s.size() && isspace(static_cast<unsigned char>(s[b]))) b++;
  size_t e = s.size();
  while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  return s.substr(b, e - b);
}

static std::string upper(const std::string &s)
{
  std::string r = s;
  for (size_t i = 0; i < r.size(); i++)
    r[i] = static_cast<char>(toupper(static_cast<unsigned char>(r[i])));
  return r;
}

static bool wordAt(const std::string &sql, const char *word, size_t pos)
{
  std::string w = upper(word);
  if (pos + w.size() > sql.size() || upper(sql.substr(pos, w.size())) != w)
    return false;
  return (pos == 0 || isspace(static_cast<unsigned char>(sql[pos - 1]))) &&
         (pos + w.size() == sql.size() ||
          isspace(static_cast<unsigned char>(sql[pos + w.size()])) ||
          sql[pos + w.size()] == '(' || sql[pos + w.size()] == ';');
}

static bool starts(const std::string &sql, const char *prefix)
{
  std::string p = upper(prefix);
  return sql.size() >= p.size() && upper(sql.substr(0, p.size())) == p &&
         (sql.size() == p.size() || isspace(static_cast<unsigned char>(sql[p.size()])));
}

static std::string unquote(const std::string &s)
{
  std::string r = trim(s);
  if (r.size() >= 2 && ((r[0] == '\'' && r[r.size() - 1] == '\'') ||
                        (r[0] == '"' && r[r.size() - 1] == '"')))
    return r.substr(1, r.size() - 2);
  return upper(r);
}

static std::string token(const std::string &s, size_t *offset)
{
  while (*offset < s.size() && isspace(static_cast<unsigned char>(s[*offset]))) (*offset)++;
  size_t b = *offset;
  while (*offset < s.size() && !isspace(static_cast<unsigned char>(s[*offset])) &&
         s[*offset] != ',' && s[*offset] != '(' && s[*offset] != ')') (*offset)++;
  return s.substr(b, *offset - b);
}

static size_t matchingParen(const std::string &s, size_t open)
{
  int depth = 0;
  char quote = 0;
  for (size_t i = open; i < s.size(); i++)
    {
      char c = s[i];
      if (quote)
        {
          if (c == quote && (i + 1 >= s.size() || s[i + 1] != quote)) quote = 0;
          else if (c == quote) i++;
          continue;
        }
      if (c == '\'' || c == '"') { quote = c; continue; }
      if (c == '(') depth++;
      else if (c == ')' && --depth == 0) return i;
    }
  return std::string::npos;
}

static std::vector<std::string> splitTop(const std::string &s)
{
  std::vector<std::string> result;
  size_t begin = 0;
  int depth = 0;
  char quote = 0;
  for (size_t i = 0; i < s.size(); i++)
    {
      char c = s[i];
      if (quote)
        {
          if (c == quote && (i + 1 >= s.size() || s[i + 1] != quote)) quote = 0;
          else if (c == quote) i++;
          continue;
        }
      if (c == '\'' || c == '"') quote = c;
      else if (c == '(') depth++;
      else if (c == ')') depth--;
      else if (c == ',' && depth == 0)
        { result.push_back(trim(s.substr(begin, i - begin))); begin = i + 1; }
    }
  if (begin < s.size()) result.push_back(trim(s.substr(begin)));
  return result;
}

static bool objectName(const std::string &input,
                       std::string *catalog, std::string *schema, std::string *name)
{
  std::string s = trim(input);
  size_t end = s.find_first_of(" \t\r\n");
  if (end != std::string::npos) s.resize(end);
  size_t first = s.find('.');
  size_t second = first == std::string::npos ? std::string::npos : s.find('.', first + 1);
  if (first == std::string::npos)
    { *catalog = "TRAFODION"; *schema = "SEABASE"; *name = unquote(s); }
  else if (second == std::string::npos)
    { *catalog = "TRAFODION"; *schema = unquote(s.substr(0, first)); *name = unquote(s.substr(first + 1)); }
  else
    { *catalog = unquote(s.substr(0, first)); *schema = unquote(s.substr(first + 1, second - first - 1)); *name = unquote(s.substr(second + 1)); }
  return !catalog->empty() && !schema->empty() && !name->empty();
}

static std::string udrKey(const UdrDef &d)
{ return "udr|" + d.catalog + "|" + d.schema + "|" + d.name; }

static std::string libraryKey(const std::string &catalog,
                              const std::string &schema,
                              const std::string &name)
{ return "udr-lib|" + catalog + "|" + schema + "|" + name; }

static std::string escapeField(const std::string &s)
{
  std::string r;
  for (size_t i = 0; i < s.size(); i++)
    { if (s[i] == '\\' || s[i] == '\n') r += '\\'; r += s[i] == '\n' ? 'n' : s[i]; }
  return r;
}

static std::string unescapeField(const std::string &s)
{
  std::string r;
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] == '\\' && i + 1 < s.size())
      { char c = s[++i]; r += c == 'n' ? '\n' : c; }
    else r += s[i];
  return r;
}

static std::string encodeParams(const std::vector<UdrParam> &params)
{
  std::string r;
  for (size_t i = 0; i < params.size(); i++)
    { if (i) r += ';'; r += escapeField(params[i].name) + ":" + escapeField(params[i].type) + ":" + (params[i].output ? "1" : "0"); }
  return r;
}

static bool decodeParams(const std::string &s, std::vector<UdrParam> *params)
{
  params->clear();
  if (s.empty()) return true;
  std::vector<std::string> entries = splitTop(s);
  // Metadata uses semicolons rather than commas so VARCHAR expressions remain intact.
  entries.clear(); size_t begin = 0;
  while (begin <= s.size())
    { size_t end = s.find(';', begin); entries.push_back(s.substr(begin, end == std::string::npos ? s.size() - begin : end - begin)); if (end == std::string::npos) break; begin = end + 1; }
  for (size_t i = 0; i < entries.size(); i++)
    {
      std::vector<std::string> fields; size_t b = 0;
      while (b <= entries[i].size()) { size_t e = entries[i].find(':', b); fields.push_back(entries[i].substr(b, e == std::string::npos ? entries[i].size() - b : e - b)); if (e == std::string::npos) break; b = e + 1; }
      if (fields.size() != 3) return false;
      UdrParam p; p.name = unescapeField(fields[0]); p.type = unescapeField(fields[1]); p.output = fields[2] == "1"; params->push_back(p);
    }
  return true;
}

static std::string encodeUdr(const UdrDef &d)
{
  std::ostringstream out;
  out << "LTU1\n" << d.kind << "\n" << escapeField(d.language) << "\n"
      << escapeField(d.symbol) << "\n" << escapeField(d.library) << "\n"
      << escapeField(d.owner) << "\n" << encodeParams(d.params) << "\n"
      << encodeParams(d.returns) << "\n" << d.resultSets << "\n"
      << (d.deterministic ? 1 : 0) << "\n" << (d.isolate ? 1 : 0) << "\n"
      << escapeField(d.sqlAccess) << "\n";
  return out.str();
}

static bool readLine(const std::string &s, size_t *offset, std::string *line)
{
  size_t end = s.find('\n', *offset);
  if (end == std::string::npos) return false;
  *line = s.substr(*offset, end - *offset); *offset = end + 1; return true;
}

static bool decodeUdr(const std::string &s, const std::string &key, UdrDef *d)
{
  if (s.compare(0, 5, "LTU1\n") != 0) return false;
  size_t off = 5; std::string line;
  if (!readLine(s, &off, &line)) return false; d->kind = atoi(line.c_str());
  if (!readLine(s, &off, &line)) return false; d->language = unescapeField(line);
  if (!readLine(s, &off, &line)) return false; d->symbol = unescapeField(line);
  if (!readLine(s, &off, &line)) return false; d->library = unescapeField(line);
  if (!readLine(s, &off, &line)) return false; d->owner = unescapeField(line);
  if (!readLine(s, &off, &line) || !decodeParams(line, &d->params)) return false;
  if (!readLine(s, &off, &line) || !decodeParams(line, &d->returns)) return false;
  if (!readLine(s, &off, &line)) return false; d->resultSets = atoi(line.c_str());
  if (!readLine(s, &off, &line)) return false; d->deterministic = atoi(line.c_str()) != 0;
  if (!readLine(s, &off, &line)) return false; d->isolate = atoi(line.c_str()) != 0;
  if (!readLine(s, &off, &line)) return false; d->sqlAccess = unescapeField(line);
  size_t first = key.find('|', 4), second = first == std::string::npos ? std::string::npos : key.find('|', first + 1);
  if (first == std::string::npos || second == std::string::npos) return false;
  d->catalog = key.substr(4, first - 4); d->schema = key.substr(first + 1, second - first - 1); d->name = key.substr(second + 1);
  return true;
}

static bool loadLibrary(LiteRocksDBStore *store, const std::string &id,
                        std::string *path, std::string *language, std::string *error)
{
  std::string c, s, n; if (!objectName(id, &c, &s, &n)) { *error = "invalid lite UDR library"; return false; }
  std::string value; bool found = false;
  if (!store->loadCatalogRecord(libraryKey(c, s, n), &value, &found, error)) return false;
  if (!found) { *error = "lite UDR library does not exist: " + id; return false; }
  if (value.compare(0, 6, "LTUL1\n") != 0) { *error = "invalid lite UDR library metadata"; return false; }
  size_t off = 6; std::string line;
  if (!readLine(value, &off, &line)) return false; *language = unescapeField(line);
  if (!readLine(value, &off, &line)) return false; *path = unescapeField(line);
  return true;
}

static bool loadRoutine(LiteRocksDBStore *store, const std::string &id,
                        UdrDef *def, std::string *error)
{
  std::string c, s, n; if (!objectName(id, &c, &s, &n)) { *error = "invalid lite UDR name"; return false; }
  std::string key = "udr|" + c + "|" + s + "|" + n, value; bool found = false;
  if (!store->loadCatalogRecord(key, &value, &found, error)) return false;
  if (!found) { *error = "lite UDR does not exist: " + id; return false; }
  if (!decodeUdr(value, key, def)) { *error = "invalid lite UDR metadata: " + id; return false; }
  return true;
}

static std::vector<UdrParam> parseParams(const std::string &text)
{
  std::vector<UdrParam> result; std::vector<std::string> parts = splitTop(text);
  for (size_t i = 0; i < parts.size(); i++)
    {
      size_t off = 0; UdrParam p; std::string first = upper(token(parts[i], &off));
      if (first == "IN" || first == "OUT" || first == "INOUT") { p.output = first != "IN"; }
      else { off = 0; }
      p.name = unquote(token(parts[i], &off)); p.type = upper(trim(parts[i].substr(off)));
      if (!p.name.empty() && !p.type.empty()) result.push_back(p);
    }
  return result;
}

static bool parseRoutineDDL(const std::string &sql, UdrDef *d, std::string *error)
{
  std::string u = upper(sql); size_t pos = 0;
  if (starts(sql, "CREATE TABLE MAPPING FUNCTION")) { d->kind = UDR_TABLE_FUNCTION; pos = strlen("CREATE TABLE MAPPING FUNCTION"); }
  else if (starts(sql, "CREATE FUNCTION")) { d->kind = UDR_FUNCTION; pos = strlen("CREATE FUNCTION"); }
  else if (starts(sql, "CREATE PROCEDURE")) { d->kind = UDR_PROCEDURE; pos = strlen("CREATE PROCEDURE"); }
  else { return false; }
  std::string rest = trim(sql.substr(pos)); size_t nameEnd = rest.find_first_of(" \t\r\n(");
  if (nameEnd == std::string::npos || !objectName(rest.substr(0, nameEnd), &d->catalog, &d->schema, &d->name)) { *error = "invalid lite UDR name"; return true; }
  size_t open = rest.find('(', nameEnd); if (open == std::string::npos) { *error = "lite UDR parameters are required"; return true; }
  size_t close = matchingParen(rest, open); if (close == std::string::npos) { *error = "unterminated lite UDR parameter list"; return true; }
  d->params = parseParams(rest.substr(open + 1, close - open - 1));
  std::string tail = trim(rest.substr(close + 1)); std::string tailUpper = upper(tail);
  size_t language = tailUpper.find("LANGUAGE");
  if (d->kind == UDR_FUNCTION || d->kind == UDR_TABLE_FUNCTION)
    {
      size_t returns = tailUpper.find("RETURNS");
      if (returns == std::string::npos || (language != std::string::npos && returns > language)) { *error = "lite UDR RETURNS clause is required"; return true; }
      size_t rbegin = returns + 7, rend = language == std::string::npos ? tail.size() : language;
      std::string r = trim(tail.substr(rbegin, rend - rbegin));
      if (upper(r).compare(0, 5, "TABLE") == 0)
        { size_t ro = r.find('('); size_t rc = ro == std::string::npos ? std::string::npos : matchingParen(r, ro); if (ro == std::string::npos || rc == std::string::npos) { *error = "invalid lite table UDR result columns"; return true; } r = r.substr(ro + 1, rc - ro - 1); }
      else if (r.size() && r[0] != '(')
        {
          size_t first = r.find_first_of(" \t");
          UdrParam result;
          if (first == std::string::npos) { result.name = "RESULT"; result.type = upper(r); }
          else { result.name = unquote(r.substr(0, first)); result.type = upper(trim(r.substr(first + 1))); }
          result.output = true; d->returns.push_back(result); r.clear();
        }
      else if (r.size() >= 2) r = r.substr(1, r.size() - 2);
      if (!r.empty()) { d->returns = parseParams(r); for (size_t i = 0; i < d->returns.size(); i++) d->returns[i].output = true; }
    }
  if (language == std::string::npos) { *error = "lite UDR LANGUAGE clause is required"; return true; }
  size_t langStart = language + 10, langEnd = tail.find_first_of(" \t\r\n", langStart); d->language = upper(tail.substr(langStart, langEnd == std::string::npos ? tail.size() - langStart : langEnd - langStart));
  size_t ext = tailUpper.find("EXTERNAL NAME"); if (ext == std::string::npos) { *error = "lite UDR EXTERNAL NAME clause is required"; return true; }
  size_t exStart = ext + strlen("EXTERNAL NAME"); d->symbol = unquote(token(tail, &exStart));
  size_t lib = tailUpper.find("LIBRARY", exStart); if (lib == std::string::npos) { *error = "lite UDR LIBRARY clause is required"; return true; }
  size_t libStart = lib + strlen("LIBRARY"); d->library = unquote(token(tail, &libStart));
  size_t rs = tailUpper.find("DYNAMIC RESULT SETS"); if (rs != std::string::npos) d->resultSets = atoi(trim(tail.substr(rs + 20)).c_str());
  d->deterministic = tailUpper.find("DETERMINISTIC") != std::string::npos;
  d->isolate = tailUpper.find("ISOLATE") != std::string::npos;
  d->sqlAccess = tailUpper.find("MODIFIES SQL DATA") != std::string::npos ? "MODIFIES SQL DATA" :
                 (tailUpper.find("CONTAINS SQL") != std::string::npos ? "CONTAINS SQL" : "NO SQL");
  if (d->kind == UDR_PROCEDURE)
    for (size_t i = 0; i < d->params.size(); i++) if (d->params[i].output) d->returns.push_back(d->params[i]);
  return true;
}

static void writeLine(SqlciEnv *env, const std::string &line)
{ env->get_logfile()->WriteAll(line.c_str()); }

static std::string currentUser(SqlciEnv *env)
{
  if (!env || env->getUserNameFromCommandLine().length() == 0)
    return "DB__ROOT";
  std::string user = env->getUserNameFromCommandLine().data();
  return upper(user);
}

static short fail(SqlciEnv *env, std::string *error, const std::string &message)
{ if (error) *error = message; return 1; }

static bool loadDefLibrary(LiteRocksDBStore *store, UdrDef *d, std::string *path, std::string *error)
{
  std::string language; return loadLibrary(store, d->library, path, &language, error);
}

struct Value
{
  enum Kind { INT, DOUBLE, STRING, NULLVAL } kind;
  int32_t i; double d; std::string s;
  Value() : kind(NULLVAL), i(0), d(0) {}
};

static std::vector<std::string> parseCallArgs(const std::string &s)
{ return splitTop(s); }

static bool parseValue(const std::string &text, Value *v)
{
  std::string s = trim(text); if (upper(s) == "NULL") { v->kind = Value::NULLVAL; return true; }
  if (s.size() >= 2 && s[0] == '\'' && s[s.size() - 1] == '\'') { v->kind = Value::STRING; v->s = s.substr(1, s.size() - 2); return true; }
  char *end = NULL; long val = strtol(s.c_str(), &end, 10); if (end && *end == 0) { v->kind = Value::INT; v->i = static_cast<int32_t>(val); return true; }
  double d = strtod(s.c_str(), &end); if (end && *end == 0) { v->kind = Value::DOUBLE; v->d = d; return true; }
  return false;
}

typedef SQLUDR_INT32 (*Fn0)(SQLUDR_INT32*, SQLUDR_INT16*, SQLUDR_CHAR*, SQLUDR_CHAR*, SQLUDR_INT32, SQLUDR_STATEAREA*, SQLUDR_UDRINFO*);
typedef SQLUDR_INT32 (*Fn1)(SQLUDR_INT32*, SQLUDR_INT32*, SQLUDR_INT16*, SQLUDR_INT16*, SQLUDR_CHAR*, SQLUDR_CHAR*, SQLUDR_INT32, SQLUDR_STATEAREA*, SQLUDR_UDRINFO*);
typedef SQLUDR_INT32 (*Fn2)(SQLUDR_INT32*, SQLUDR_INT32*, SQLUDR_INT32*, SQLUDR_INT16*, SQLUDR_INT16*, SQLUDR_INT16*, SQLUDR_CHAR*, SQLUDR_CHAR*, SQLUDR_INT32, SQLUDR_STATEAREA*, SQLUDR_UDRINFO*);
typedef SQLUDR_INT32 (*Fn2x2)(SQLUDR_INT32*, SQLUDR_INT32*, SQLUDR_INT32*, SQLUDR_INT32*, SQLUDR_INT16*, SQLUDR_INT16*, SQLUDR_INT16*, SQLUDR_INT16*, SQLUDR_CHAR*, SQLUDR_CHAR*, SQLUDR_INT32, SQLUDR_STATEAREA*, SQLUDR_UDRINFO*);

static bool invokeNative(const UdrDef &d, const std::string &path,
                         const std::vector<Value> &args, std::vector<Value> *out,
                         std::string *error)
{
  if (args.size() > 2 || d.returns.size() > 2) { *error = "lite native UDR supports at most two INT inputs and two outputs"; return false; }
  for (size_t i = 0; i < args.size(); i++) if (args[i].kind != Value::INT && args[i].kind != Value::NULLVAL) { *error = "lite native UDR currently accepts INT arguments only"; return false; }
  for (size_t i = 0; i < d.returns.size(); i++) if (upper(d.returns[i].type).find("INT") == std::string::npos) { *error = "lite native UDR currently returns INT values only"; return false; }
  void *handle = path == "builtin" ? dlopen(NULL, RTLD_NOW | RTLD_GLOBAL) : dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) { *error = "unable to load lite UDR library " + path + ": " + dlerror(); return false; }
  void *symbol = d.symbol.empty() ? NULL : dlsym(handle, d.symbol.c_str());
  if (!symbol) { *error = "unable to resolve lite UDR symbol " + d.symbol; if (path != "builtin") dlclose(handle); return false; }
  SQLUDR_INT32 a = args.size() > 0 && args[0].kind == Value::INT ? args[0].i : 0;
  SQLUDR_INT32 b = args.size() > 1 && args[1].kind == Value::INT ? args[1].i : 0;
  SQLUDR_INT32 r1 = 0, r2 = 0; SQLUDR_INT16 ai = args.size() > 0 && args[0].kind == Value::NULLVAL ? SQLUDR_NULL : 0;
  SQLUDR_INT16 bi = args.size() > 1 && args[1].kind == Value::NULLVAL ? SQLUDR_NULL : 0, r1i = 0, r2i = 0;
  SQLUDR_CHAR state[SQLUDR_SQLSTATE_SIZE] = "00000", msg[SQLUDR_MSGTEXT_SIZE] = ""; SQLUDR_STATEAREA area; memset(&area, 0, sizeof(area)); area.version = SQLUDR_STATEAREA_CURRENT_VERSION;
  SQLUDR_INT32 rc = 0;
  for (int call = SQLUDR_CALLTYPE_INITIAL; call <= SQLUDR_CALLTYPE_FINAL; call++)
    {
      if (args.size() == 0) rc = reinterpret_cast<Fn0>(symbol)(&r1, &r1i, state, msg, call, &area, NULL);
      else if (args.size() == 1) rc = reinterpret_cast<Fn1>(symbol)(&a, &r1, &ai, &r1i, state, msg, call, &area, NULL);
      else if (d.returns.size() > 1) rc = reinterpret_cast<Fn2x2>(symbol)(&a, &b, &r1, &r2, &ai, &bi, &r1i, &r2i, state, msg, call, &area, NULL);
      else rc = reinterpret_cast<Fn2>(symbol)(&a, &b, &r1, &ai, &bi, &r1i, state, msg, call, &area, NULL);
      if (rc != SQLUDR_SUCCESS && call != SQLUDR_CALLTYPE_FINAL) break;
    }
  if (path != "builtin") dlclose(handle);
  if (rc != SQLUDR_SUCCESS) { *error = std::string("lite UDR failed") + (msg[0] ? ": " + std::string(msg) : ""); return false; }
  out->clear(); for (size_t i = 0; i < d.returns.size(); i++) { Value v; v.kind = (i == 0 && r1i != SQLUDR_NULL) || (i == 1 && r2i != SQLUDR_NULL) ? Value::INT : Value::NULLVAL; v.i = i == 0 ? r1 : r2; out->push_back(v); }
  return true;
}

static bool invokeJava(const UdrDef &d, const std::string &path,
                       const std::vector<Value> &args, std::vector<Value> *out,
                       std::string *error)
{
  std::ostringstream cmd; cmd << "java -cp '" << path << "' '" << d.symbol << "'";
  for (size_t i = 0; i < args.size(); i++) { if (args[i].kind != Value::INT) { *error = "lite Java UDR accepts INT arguments only"; return false; } cmd << " " << args[i].i; }
  FILE *pipe = popen(cmd.str().c_str(), "r"); if (!pipe) { *error = "unable to launch lite Java UDR"; return false; }
  char buf[512] = ""; std::string output; while (fgets(buf, sizeof(buf), pipe)) output += buf; int status = pclose(pipe);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { *error = "lite Java UDR failed"; return false; }
  std::vector<std::string> values = splitTop(trim(output)); out->clear();
  for (size_t i = 0; i < d.returns.size(); i++) { Value v; if (i >= values.size() || !parseValue(values[i], &v)) { *error = "invalid lite Java UDR result"; return false; } out->push_back(v); }
  return true;
}

static bool parseInvocation(const std::string &sql, std::string *name,
                            std::vector<std::string> *args, bool *table)
{
  std::string s = trim(sql), u = upper(s); size_t begin = 0;
  if (u.find("FROM TABLE(") != std::string::npos) { begin = u.find("TABLE(") + 6; *table = true; }
  else if (starts(s, "VALUES")) begin = 6;
  else if (starts(s, "CALL")) begin = 4;
  else return false;
  while (begin < s.size() && (isspace(static_cast<unsigned char>(s[begin])) || s[begin] == '(' || s[begin] == '*')) begin++;
  size_t open = s.find('(', begin); if (open == std::string::npos) return false; size_t close = matchingParen(s, open); if (close == std::string::npos) return false;
  *name = trim(s.substr(begin, open - begin));
  // EXPLAIN(NULL, ...) is a built-in table-valued metadata request, not a
  // user-defined table-mapping function.
  if (upper(*name) == "EXPLAIN") return false;
  std::string argText = s.substr(open + 1, close - open - 1); *args = parseCallArgs(argText); return !name->empty();
}

static bool invoke(const UdrDef &d, LiteRocksDBStore *store,
                   const std::vector<std::string> &argTexts,
                   std::vector<Value> *out, std::string *error)
{
  size_t argCount = argTexts.size() == 1 && trim(argTexts[0]).empty() ? 0 : argTexts.size();
  size_t inputCount = 0;
  for (size_t i = 0; i < d.params.size(); i++) if (!d.params[i].output) inputCount++;
  if (argCount != inputCount) { *error = "lite UDR argument count mismatch"; return false; }
  std::vector<Value> args; for (size_t i = 0; i < argCount; i++) { Value v; if (!parseValue(argTexts[i], &v)) { *error = "invalid lite UDR argument"; return false; } args.push_back(v); }
  std::string path; if (!loadDefLibrary(store, const_cast<UdrDef*>(&d), &path, error)) return false;
  if (upper(d.language) == "JAVA") return invokeJava(d, path, args, out, error);
  return invokeNative(d, path, args, out, error);
}

static void printValues(SqlciEnv *env, const UdrDef &d, const std::vector<Value> &values)
{
  std::string header;
  for (size_t i = 0; i < d.returns.size(); i++) { if (i) header += "  "; header += d.returns[i].name; }
  if (!header.empty()) { writeLine(env, header); writeLine(env, "--------------------"); }
  std::string row;
  for (size_t i = 0; i < values.size(); i++) { if (i) row += "  "; if (values[i].kind == Value::NULLVAL) row += "?"; else if (values[i].kind == Value::INT) row += std::to_string(values[i].i); else if (values[i].kind == Value::DOUBLE) { std::ostringstream x; x << values[i].d; row += x.str(); } else row += values[i].s; }
  writeLine(env, row); writeLine(env, "--- 1 row(s) selected.");
}

// Built-in routines make the lite regression deterministic while the
// same invocation path also supports dlopen() libraries and Java adapters.
extern "C" SQLUDR_INT32 lite_udr_add2(SQLUDR_INT32 *a, SQLUDR_INT32 *b, SQLUDR_INT32 *r, SQLUDR_INT16 *ai, SQLUDR_INT16 *bi, SQLUDR_INT16 *ri, SQLUDR_CHAR *state, SQLUDR_CHAR *msg, SQLUDR_INT32 call, SQLUDR_STATEAREA *, SQLUDR_UDRINFO *)
{ if (call == SQLUDR_CALLTYPE_FINAL) return SQLUDR_SUCCESS; if (*ai == SQLUDR_NULL || *bi == SQLUDR_NULL) *ri = SQLUDR_NULL; else *r = *a + *b; return SQLUDR_SUCCESS; }
extern "C" SQLUDR_INT32 lite_udr_swap2(SQLUDR_INT32 *a, SQLUDR_INT32 *b, SQLUDR_INT32 *r1, SQLUDR_INT32 *r2, SQLUDR_INT16 *ai, SQLUDR_INT16 *bi, SQLUDR_INT16 *i1, SQLUDR_INT16 *i2, SQLUDR_CHAR *, SQLUDR_CHAR *, SQLUDR_INT32 call, SQLUDR_STATEAREA *, SQLUDR_UDRINFO *)
{ if (call == SQLUDR_CALLTYPE_FINAL) return SQLUDR_SUCCESS; if (*ai == SQLUDR_NULL) *i2 = SQLUDR_NULL; else *r2 = *a; if (*bi == SQLUDR_NULL) *i1 = SQLUDR_NULL; else *r1 = *b; return SQLUDR_SUCCESS; }
extern "C" SQLUDR_INT32 lite_udr_fail(SQLUDR_INT32 *, SQLUDR_INT32 *, SQLUDR_INT32 *, SQLUDR_INT16 *, SQLUDR_INT16 *, SQLUDR_INT16 *, SQLUDR_CHAR *, SQLUDR_CHAR *msg, SQLUDR_INT32 call, SQLUDR_STATEAREA *, SQLUDR_UDRINFO *)
{ if (call != SQLUDR_CALLTYPE_FINAL) strcpy(msg, "intentional lite UDR failure"); return call == SQLUDR_CALLTYPE_FINAL ? SQLUDR_SUCCESS : SQLUDR_ERROR; }

static bool handleDDL(const std::string &sql, SqlciEnv *env, short *retcode, std::string *error)
{
  LiteRocksDBStore store;
  const bool root = currentUser(env) == "DB__ROOT";
  if (starts(sql, "CREATE LIBRARY"))
    {
      if (!root) { *error = "only DB__ROOT may create lite UDR libraries"; return true; }
      std::string rest = trim(sql.substr(strlen("CREATE LIBRARY"))); size_t end = rest.find_first_of(" \t\r\n"); std::string c,s,n;
      if (end == std::string::npos || !objectName(rest.substr(0,end), &c,&s,&n)) { *error = "invalid lite UDR library name"; return true; }
      size_t file = upper(rest).find("FILE", end); if (file == std::string::npos) { *error = "lite UDR library FILE clause is required"; return true; }
      size_t o = file + 4; std::string path = unquote(token(rest, &o)); std::string value = "LTUL1\nC\n" + escapeField(path) + "\n";
      if (!store.storeCatalogRecord(libraryKey(c,s,n), value, error)) return true; *retcode = 0; return true;
    }
  if (starts(sql, "DROP LIBRARY"))
    {
      if (!root) { *error = "only DB__ROOT may drop lite UDR libraries"; return true; }
      std::string c,s,n; std::string rest = trim(sql.substr(strlen("DROP LIBRARY"))); if (!objectName(rest,&c,&s,&n)) { *error = "invalid lite UDR library name"; return true; }
      std::vector< std::pair<std::string,std::string> > records; if (!store.scanCatalogRecords("udr|", &records, error)) return true;
      for (size_t i = 0; i < records.size(); i++) { UdrDef d; if (decodeUdr(records[i].second, records[i].first, &d) && d.library == c+"."+s+"."+n) store.deleteCatalogRecord(records[i].first, error); }
      store.deleteCatalogRecord(libraryKey(c,s,n), error); *retcode = 0; return true;
    }
  if (starts(sql, "CREATE FUNCTION") || starts(sql, "CREATE PROCEDURE") || starts(sql, "CREATE TABLE MAPPING FUNCTION"))
    {
      if (!root) { *error = "only DB__ROOT may create lite UDR routines"; return true; }
      UdrDef d; if (!parseRoutineDDL(sql, &d, error)) return false; if (!error->empty()) return true;
      d.owner = "DB__ROOT"; std::string path, language;
      std::string lc, ls, ln;
      if (!objectName(d.library, &lc, &ls, &ln)) { *error = "invalid lite UDR library name"; return true; }
      d.library = lc + "." + ls + "." + ln;
      if (!loadLibrary(&store, d.library, &path, &language, error)) return true;
      if (!store.storeCatalogRecord(udrKey(d), encodeUdr(d), error)) return true; *retcode = 0; return true;
    }
  if (starts(sql, "DROP FUNCTION") || starts(sql, "DROP PROCEDURE") || starts(sql, "DROP TABLE MAPPING FUNCTION"))
    {
      if (!root) { *error = "only DB__ROOT may drop lite UDR routines"; return true; }
      size_t p = starts(sql,"DROP FUNCTION") ? strlen("DROP FUNCTION") : (starts(sql,"DROP PROCEDURE") ? strlen("DROP PROCEDURE") : strlen("DROP TABLE MAPPING FUNCTION")); std::string c,s,n;
      if (!objectName(trim(sql.substr(p)),&c,&s,&n)) { *error = "invalid lite UDR name"; return true; }
      if (!store.deleteCatalogRecord("udr|"+c+"|"+s+"|"+n, error)) return true; *retcode = 0; return true;
    }
  return false;
}

static std::map<std::string, std::string> prepared;

static bool runInvocation(const std::string &sql, SqlciEnv *env, short *retcode)
{
  bool table = false; std::string name; std::vector<std::string> args; if (!parseInvocation(sql,&name,&args,&table)) return false;
  LiteRocksDBStore store; UdrDef d; std::string error; if (!loadRoutine(&store,name,&d,&error)) { *retcode = fail(env,&error,error); writeLine(env,"*** ERROR[lite] "+error); return true; }
  std::vector<Value> values; if (!invoke(d,&store,args,&values,&error)) { *retcode = fail(env,&error,error); writeLine(env,"*** ERROR[lite] "+error); return true; }
  if (d.kind == UDR_PROCEDURE && d.returns.empty()) { writeLine(env,"--- SQL operation complete."); *retcode = 0; }
  else { printValues(env,d,values); *retcode = 0; }
  return true;
}

} // namespace

bool LiteUdr_process(const char *sqlText, SqlciEnv *env, short *retcode)
{
  if (!sqlText || !env || !retcode) return false;
  std::string sql = trim(sqlText); while (!sql.empty() && sql[sql.size()-1] == ';') sql = trim(sql.substr(0,sql.size()-1));
  std::string error;
  if (handleDDL(sql, env, retcode, &error)) { if (!error.empty()) *retcode = fail(env,&error,error), writeLine(env,"*** ERROR[lite] "+error); return true; }
  if (runInvocation(sql, env, retcode)) return true;
  return false;
}

bool LiteUdr_prepare(const char *sqlText, const char *statementName,
                          SqlciEnv *env, short *retcode)
{
  if (!sqlText || !statementName || !env || !retcode) return false;
  std::string sql = trim(sqlText);
  std::string sqlUpper = upper(sql);
  if (!starts(sql,"VALUES") && !starts(sql,"CALL") &&
      (sqlUpper.find("FROM TABLE(") == std::string::npos ||
       sqlUpper.find("FROM TABLE(EXPLAIN") != std::string::npos))
    return false;
  prepared[statementName] = sql; writeLine(env,"--- SQL command prepared."); *retcode = 0; return true;
}

bool LiteUdr_executePrepared(const char *statementName, SqlciEnv *env, short *retcode)
{
  if (!statementName || !env || !retcode) return false;
  std::map<std::string,std::string>::const_iterator it = prepared.find(statementName); if (it == prepared.end()) return false;
  return runInvocation(it->second, env, retcode);
}

#endif
