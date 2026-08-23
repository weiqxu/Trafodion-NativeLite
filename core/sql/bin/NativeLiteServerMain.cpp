// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one or more
// contributor license agreements. See the NOTICE file distributed with this
// work for additional information regarding copyright ownership.
// @@@ END COPYRIGHT @@@

#include "Platform.h"

#ifdef TRAF_LITE

#include "BaseTypes.h"
#include "ComDiags.h"
#include "Context.h"
#include "ExExeUtilCli.h"
#include "Formatter.h"
#include "Globals.h"
#include "LiteConfig.h"
#include "LiteRocksDBStore.h"
#include "LiteSqlTable.h"
#include "QRLogger.h"
#include "SQLCLIdev.h"
#include "SqlciEnv.h"
#include "dfs2rec.h"
#include "ex_transaction.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern void my_mpi_fclose();

THREAD_P jmp_buf ExportJmpBuf;
extern THREAD_P jmp_buf *ExportJmpBufPtr;
THREAD_P SqlciEnv *global_sqlci_env = NULL;

namespace {

const uint32_t kMaxMessageSize = 64U * 1024U * 1024U;

const int16_t kT4Cancel = 1013;
const int16_t kT4GetObjectRef = 1019;
const int16_t kT4SqlConnect = 3001;
const int16_t kT4SqlDisconnect = 3002;
const int16_t kT4SetConnectionOption = 3003;
const int16_t kT4EndTransaction = 3004;
const int16_t kT4Prepare = 3005;
const int16_t kT4Execute = 3011;
const int16_t kT4Execute2 = 3025;
const int16_t kT4ExecuteDirect = 3012;
const int16_t kT4Fetch = 3009;
const int16_t kT4FreeStatement = 3015;
const int16_t kT4GetCatalogs = 3016;
const int32_t kT4Signature = 12345;
const int32_t kT4ClientHeaderVersion = 101;
const int32_t kT4ServerHeaderVersion = 201;
const int32_t kT4WriteRequestFirst = 1;
const int32_t kT4ReadResponseFirst = 3;

std::atomic<bool> gStopping(false);
std::atomic<int> gListenFd(-1);

bool currentTransactionInProgress();
size_t countT4Parameters(const std::string &sql);
bool rewriteT4ParametersAsVarchar(const std::string &sql,
                                  std::string *output);
bool substituteT4Parameters(const std::string &sql,
                            const std::vector<std::string> &values,
                            const std::vector<bool> &nulls,
                            std::string *output);
bool splitT4Statements(const std::string &sql,
                       std::vector<std::string> *statements);

uint16_t getU16(const char *p)
{
  uint16_t value;
  memcpy(&value, p, sizeof(value));
  return ntohs(value);
}

uint32_t getU32(const char *p)
{
  uint32_t value;
  memcpy(&value, p, sizeof(value));
  return ntohl(value);
}

void appendU16(std::string *out, uint16_t value)
{
  value = htons(value);
  out->append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void appendU32(std::string *out, uint32_t value)
{
  value = htonl(value);
  out->append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void appendU64(std::string *out, uint64_t value)
{
  appendU32(out, static_cast<uint32_t>(value >> 32));
  appendU32(out, static_cast<uint32_t>(value));
}

void appendT4String(std::string *out, const std::string &value)
{
  if (value.empty())
    {
      appendU32(out, 0);
      return;
    }
  appendU32(out, static_cast<uint32_t>(value.size() + 1));
  out->append(value);
  out->push_back('\0');
}

void appendT4Version(std::string *out, uint16_t component, uint16_t major,
                     uint16_t minor, uint32_t build)
{
  appendU16(out, component);
  appendU16(out, major);
  appendU16(out, minor);
  appendU32(out, build);
}

struct T4Header
{
  int16_t operation;
  int32_t dialogueId;
  uint32_t totalLength;
  uint32_t compressedLength;
  char compression;
  char compressionType;
  int32_t headerType;
  int32_t signature;
  int32_t version;
  char platform;
  char transport;
  char swap;
  int16_t error;
  int16_t errorDetail;

  T4Header()
      : operation(0), dialogueId(0), totalLength(0), compressedLength(0),
        compression('N'), compressionType(0), headerType(0), signature(0),
        version(0), platform('P'), transport('T'), swap('N'), error(0),
        errorDetail(0)
  {
  }
};

bool decodeT4Header(const char *bytes, T4Header *header)
{
  header->operation = static_cast<int16_t>(getU16(bytes));
  header->dialogueId = static_cast<int32_t>(getU32(bytes + 4));
  header->totalLength = getU32(bytes + 8);
  header->compressedLength = getU32(bytes + 12);
  header->compression = bytes[16];
  header->compressionType = bytes[17];
  header->headerType = static_cast<int32_t>(getU32(bytes + 20));
  header->signature = static_cast<int32_t>(getU32(bytes + 24));
  header->version = static_cast<int32_t>(getU32(bytes + 28));
  header->platform = bytes[32];
  header->transport = bytes[33];
  header->swap = bytes[34];
  header->error = static_cast<int16_t>(getU16(bytes + 36));
  header->errorDetail = static_cast<int16_t>(getU16(bytes + 38));
  return header->signature == kT4Signature &&
         header->version == kT4ClientHeaderVersion &&
         header->headerType == kT4WriteRequestFirst &&
         header->totalLength <= kMaxMessageSize &&
         header->compression != 'Y';
}

std::string encodeT4Header(const T4Header &request, uint32_t bodyLength,
                           int16_t error = 0, int16_t errorDetail = 0)
{
  std::string bytes;
  appendU16(&bytes, static_cast<uint16_t>(request.operation));
  appendU16(&bytes, 0);
  appendU32(&bytes, static_cast<uint32_t>(request.dialogueId));
  appendU32(&bytes, bodyLength);
  appendU32(&bytes, 0);
  bytes.push_back('N');
  bytes.push_back(0);
  appendU16(&bytes, 0);
  appendU32(&bytes, kT4ReadResponseFirst);
  appendU32(&bytes, kT4Signature);
  appendU32(&bytes, kT4ServerHeaderVersion);
  bytes.push_back('N');
  bytes.push_back('T');
  bytes.push_back('N');
  bytes.push_back(0);
  appendU16(&bytes, static_cast<uint16_t>(error));
  appendU16(&bytes, static_cast<uint16_t>(errorDetail));
  return bytes;
}

bool writeExact(int fd, const void *buffer, size_t length);

bool sendT4Response(int fd, const T4Header &request, const std::string &body,
                    int16_t error = 0, int16_t errorDetail = 0)
{
  std::string header = encodeT4Header(
      request, static_cast<uint32_t>(body.size()), error, errorDetail);
  return writeExact(fd, header.data(), header.size()) &&
         writeExact(fd, body.data(), body.size());
}

class T4Reader
{
public:
  explicit T4Reader(const std::string &data) : data_(data), offset_(0) {}

  bool skip(size_t count)
  {
    if (count > data_.size() - offset_)
      return false;
    offset_ += count;
    return true;
  }

  bool readU16(uint16_t *value)
  {
    if (data_.size() - offset_ < 2)
      return false;
    *value = getU16(data_.data() + offset_);
    offset_ += 2;
    return true;
  }

  bool readU32(uint32_t *value)
  {
    if (data_.size() - offset_ < 4)
      return false;
    *value = getU32(data_.data() + offset_);
    offset_ += 4;
    return true;
  }

  bool readU64(uint64_t *value)
  {
    uint32_t high = 0;
    uint32_t low = 0;
    if (!readU32(&high) || !readU32(&low))
      return false;
    *value = (static_cast<uint64_t>(high) << 32) | low;
    return true;
  }

  bool readString(std::string *value)
  {
    uint32_t length = 0;
    if (!readU32(&length))
      return false;
    if (length == 0)
      {
        value->clear();
        return true;
      }
    if (length > data_.size() - offset_ || data_[offset_ + length - 1] != 0)
      return false;
    value->assign(data_.data() + offset_, length - 1);
    offset_ += length;
    return true;
  }

  bool readStringWithCharset(std::string *value)
  {
    if (!readString(value))
      return false;
    if (value->empty())
      return true;
    uint32_t charset = 0;
    return readU32(&charset);
  }

  bool readBytes(size_t length, std::string *value)
  {
    if (length > data_.size() - offset_)
      return false;
    value->assign(data_.data() + offset_, length);
    offset_ += length;
    return true;
  }

  size_t remaining() const { return data_.size() - offset_; }

private:
  const std::string &data_;
  size_t offset_;
};

std::string trim(const std::string &value)
{
  size_t begin = 0;
  while (begin < value.size() &&
         isspace(static_cast<unsigned char>(value[begin])))
    begin++;
  size_t end = value.size();
  while (end > begin && isspace(static_cast<unsigned char>(value[end - 1])))
    end--;
  return value.substr(begin, end - begin);
}

std::string upper(const std::string &value)
{
  std::string result(value);
  for (size_t i = 0; i < result.size(); i++)
    result[i] = static_cast<char>(
        toupper(static_cast<unsigned char>(result[i])));
  return result;
}

std::string firstWord(const std::string &sql)
{
  std::string value = trim(sql);
  size_t end = 0;
  while (end < value.size() &&
         (isalnum(static_cast<unsigned char>(value[end])) ||
          value[end] == '_'))
    end++;
  return upper(value.substr(0, end));
}

bool startsWith(const std::string &value, const std::string &prefix)
{
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool readExact(int fd, void *buffer, size_t length)
{
  char *p = static_cast<char *>(buffer);
  while (length > 0)
    {
      ssize_t count = recv(fd, p, length, 0);
      if (count == 0)
        return false;
      if (count < 0)
        {
          if (errno == EINTR)
            continue;
          return false;
        }
      p += count;
      length -= static_cast<size_t>(count);
    }
  return true;
}

bool writeExact(int fd, const void *buffer, size_t length)
{
  const char *p = static_cast<const char *>(buffer);
  while (length > 0)
    {
      ssize_t count = send(fd, p, length, MSG_NOSIGNAL);
      if (count < 0)
        {
          if (errno == EINTR)
            continue;
          return false;
        }
      p += count;
      length -= static_cast<size_t>(count);
    }
  return true;
}

struct Cell
{
  bool isNull;
  std::string value;
  Cell() : isNull(true) {}
  explicit Cell(const std::string &v) : isNull(false), value(v) {}
};

struct Column
{
  std::string name;
  uint32_t oid;
  int16_t typeLength;
  int32_t typeModifier;
  int16_t format;
  Lng32 fsType;
  Lng32 length;
  Lng32 precision;
  Lng32 scale;
  Lng32 charset;
  Lng32 nullable;

  Column()
      : oid(25), typeLength(-1), typeModifier(-1), format(0), fsType(0),
        length(0), precision(0), scale(0), charset(0), nullable(0)
  {
  }
};

struct QueryResult
{
  std::vector<Column> columns;
  std::vector<std::vector<Cell> > rows;
  std::string commandTag;
  std::string sqlstate;
  std::string error;

  bool ok() const { return error.empty(); }
};

struct T4StatementState
{
  std::string sql;
  size_t parameterCount;
  std::vector<std::string> parameterParts;
  std::string batchPrefix;
  std::vector<std::string> batchParameterParts;
  bool oldFetchFormat;
  bool prepared;
  std::shared_ptr<struct NativeLitePreparedPlan> preparedPlan;
  QueryResult result;
  size_t rowOffset;
  T4StatementState()
      : parameterCount(0), oldFetchFormat(false), prepared(false),
        rowOffset(0) {}
};

struct T4TypeInfo
{
  int32_t sqlType;
  int32_t odbcType;
  uint32_t capacity;
  uint32_t precision;
  uint32_t datetimeCode;
  bool signedType;
  bool variable;
};

T4TypeInfo t4TypeInfo(const Column &column)
{
  T4TypeInfo info = {-601, 12, 256, 256, 0, false, true};
  uint32_t declared = column.length > 0
      ? static_cast<uint32_t>(column.length) : 256U;
  info.capacity = std::min<uint32_t>(
      std::max<uint32_t>(declared, 32U), 0x7fffU);
  info.precision = info.capacity;
  if (column.scale != 0)
    return info;
  switch (column.fsType)
    {
    case REC_BOOLEAN:
      info = T4TypeInfo{-701, 16, 1, 1, 0, false, false}; break;
    case REC_BIN8_SIGNED:
      info = T4TypeInfo{-403, -6, 1, 3, 0, true, false}; break;
    case REC_BIN8_UNSIGNED:
      info = T4TypeInfo{-404, -6, 1, 3, 0, false, false}; break;
    case REC_BIN16_SIGNED:
      info = T4TypeInfo{5, 5, 2, 5, 0, true, false}; break;
    case REC_BPINT_UNSIGNED:
    case REC_BIN16_UNSIGNED:
      info = T4TypeInfo{-502, 5, 2, 5, 0, false, false}; break;
    case REC_BIN32_SIGNED:
      info = T4TypeInfo{4, 4, 4, 10, 0, true, false}; break;
    case REC_BIN32_UNSIGNED:
      info = T4TypeInfo{-401, 4, 4, 10, 0, false, false}; break;
    case REC_BIN64_SIGNED:
      info = T4TypeInfo{-402, -5, 8, 19, 0, true, false}; break;
    case REC_FLOAT32:
      info = T4TypeInfo{7, 7, 4, 7, 0, true, false}; break;
    case REC_FLOAT64:
      info = T4TypeInfo{8, 8, 8, 15, 0, true, false}; break;
    case REC_DATETIME:
      info.sqlType = 9;
      info.datetimeCode = static_cast<uint32_t>(column.precision);
      info.odbcType = column.precision == REC_DTCODE_DATE ? 91 :
          (column.precision == REC_DTCODE_TIME ? 92 : 93);
      info.capacity = column.precision == REC_DTCODE_DATE ? 10 :
          (column.precision == REC_DTCODE_TIME ? 8 :
           std::max<uint32_t>(declared, 19));
      info.precision = info.capacity;
      info.variable = false;
      break;
    default:
      break;
    }
  return info;
}

uint32_t t4ColumnCapacity(const Column &column)
{
  T4TypeInfo info = t4TypeInfo(column);
  return info.capacity + (info.variable ? 2U : 0U);
}

uint32_t t4RowLength(const QueryResult &result)
{
  uint64_t length = 0;
  for (size_t i = 0; i < result.columns.size(); i++)
    length += 2U + t4ColumnCapacity(result.columns[i]);
  return static_cast<uint32_t>(std::min<uint64_t>(length, 0x7fffffffU));
}

void appendT4Descriptor(std::string *reply, const Column &column,
                        uint32_t valueOffset)
{
  const T4TypeInfo info = t4TypeInfo(column);
  const uint32_t capacity = t4ColumnCapacity(column);
  appendU32(reply, valueOffset + 2); // value offset in each row
  appendU32(reply, valueOffset);     // nullable indicator offset
  appendU32(reply, 0);               // descriptor version
  appendU32(reply, static_cast<uint32_t>(info.sqlType));
  appendU32(reply, info.datetimeCode);
  appendU32(reply, info.capacity);
  appendU32(reply, info.precision);
  appendU32(reply, static_cast<uint32_t>(column.scale));
  appendU32(reply, 1);               // nullable
  appendU32(reply, info.signedType ? 1 : 0);
  appendU32(reply, static_cast<uint32_t>(info.odbcType));
  appendU32(reply, info.precision);
  appendU32(reply, 15);              // SQL UTF-8 charset
  appendU32(reply, 15);              // ODBC UTF-8 charset
  appendT4String(reply, column.name);
  appendT4String(reply, std::string()); // table
  appendT4String(reply, "TRAFODION");
  appendT4String(reply, "SEABASE");
  appendT4String(reply, column.name);
  appendU32(reply, 0);               // interval leading precision
  appendU32(reply, 0);               // parameter mode
}

std::string encodeT4Rows(const QueryResult &result, size_t begin, size_t end)
{
  end = std::min(end, result.rows.size());
  std::string values;
  const uint32_t rowLength = t4RowLength(result);
  values.reserve(static_cast<size_t>(rowLength) * (end - begin));
  for (size_t rowIndex = begin; rowIndex < end; rowIndex++)
    {
      size_t rowBegin = values.size();
      for (size_t columnIndex = 0;
           columnIndex < result.columns.size(); columnIndex++)
        {
          const Column &column = result.columns[columnIndex];
          const T4TypeInfo info = t4TypeInfo(column);
          const uint32_t capacity = t4ColumnCapacity(column);
          const Cell &cell = result.rows[rowIndex][columnIndex];
          appendU16(&values, cell.isNull ? 0xffffU : 0U);
          if (cell.isNull)
            {
              values.append(capacity, '\0');
              continue;
            }
          if (info.variable)
            {
              const uint32_t length = std::min<uint32_t>(
                  static_cast<uint32_t>(cell.value.size()), info.capacity);
              appendU16(&values, static_cast<uint16_t>(length));
              values.append(cell.value.data(), length);
              values.append(info.capacity - length, '\0');
            }
          else if (info.sqlType == -701)
            values.push_back(upper(cell.value) == "TRUE" || cell.value == "1"
                                 ? 1 : 0);
          else if (info.sqlType == -403 || info.sqlType == -404)
            values.push_back(static_cast<char>(strtol(cell.value.c_str(),
                                                       NULL, 10)));
          else if (info.capacity == 2)
            appendU16(&values, static_cast<uint16_t>(
                strtol(cell.value.c_str(), NULL, 10)));
          else if (info.capacity == 4 && info.sqlType != 7)
            appendU32(&values, static_cast<uint32_t>(
                strtoll(cell.value.c_str(), NULL, 10)));
          else if (info.capacity == 8 && info.sqlType == -402)
            appendU64(&values, static_cast<uint64_t>(
                strtoll(cell.value.c_str(), NULL, 10)));
          else if (info.sqlType == 7)
            {
              float number = static_cast<float>(strtod(cell.value.c_str(), NULL));
              uint32_t bits = 0;
              memcpy(&bits, &number, sizeof(bits));
              appendU32(&values, bits);
            }
          else if (info.sqlType == 8)
            {
              double number = strtod(cell.value.c_str(), NULL);
              uint64_t bits = 0;
              memcpy(&bits, &number, sizeof(bits));
              appendU64(&values, bits);
            }
          else
            {
              uint32_t length = std::min<uint32_t>(
                  static_cast<uint32_t>(cell.value.size()), info.capacity);
              values.append(cell.value.data(), length);
              values.append(info.capacity - length, '\0');
            }
        }
      if (values.size() - rowBegin < rowLength)
        values.append(rowLength - (values.size() - rowBegin), '\0');
    }
  return values;
}

std::string encodeT4OldRows(const QueryResult &result, size_t begin,
                            size_t end)
{
  end = std::min(end, result.rows.size());
  std::string values;
  for (size_t rowIndex = begin; rowIndex < end; rowIndex++)
    for (size_t columnIndex = 0;
         columnIndex < result.columns.size(); columnIndex++)
      {
        const Cell &cell = result.rows[rowIndex][columnIndex];
        values.push_back(cell.isNull ? static_cast<char>(-1) : 0);
        if (cell.isNull)
          continue;
        uint16_t length = static_cast<uint16_t>(
            std::min<size_t>(cell.value.size(), 0x7fffU));
        appendU16(&values, length);
        values.append(cell.value.data(), length);
        values.push_back('\0');
      }
  return values;
}

struct NativeLitePreparedParameter
{
  Lng32 fsDatatype;
  Lng32 length;
  Lng32 indOffset;
  Lng32 varOffset;

  NativeLitePreparedParameter()
      : fsDatatype(0), length(0), indOffset(-1), varOffset(-1) {}
};

struct NativeLitePreparedPlan
{
  std::unique_ptr<ExeCliInterface> cli;
  std::string sql;
  std::string sourceSql;
  QueryResult description;
  std::vector<NativeLitePreparedParameter> parameters;
  bool executed;

  NativeLitePreparedPlan() : executed(false) {}
};

struct Session
{
  SQLCTX_HANDLE contextHandle;
  SqlciEnv *env;
  std::string user;
  std::string database;
  int32_t backendPid;
  std::atomic<bool> cancelRequested;
  std::atomic<bool> statementPending;
  std::atomic<char> transactionStatus;
  bool failedTransaction;
  bool autoCommit;
  uint64_t statementSequence;
  std::thread::id ownerThread;
  std::mutex lifecycleMutex;
  std::mutex compilerMutex;
  std::condition_variable cancelCondition;
  unsigned int activeCancels;
  bool closing;
  bool slotAcquired;
  std::map<std::string, std::shared_ptr<NativeLitePreparedPlan> > planCache;
  std::deque<std::string> planCacheLru;

  Session()
      : contextHandle(0), env(NULL), backendPid(0),
        cancelRequested(false), statementPending(false), transactionStatus('I'),
        failedTransaction(false), autoCommit(true), statementSequence(0),
        activeCancels(0), closing(false), slotAcquired(false)
  {
  }
};

enum RequestType
{
  REQUEST_CREATE,
  REQUEST_EXECUTE,
  REQUEST_DESCRIBE,
  REQUEST_PREPARE,
  REQUEST_EXECUTE_PREPARED,
  REQUEST_DESTROY,
  REQUEST_STOP
};

struct EngineRequest
{
  RequestType type;
  std::shared_ptr<Session> session;
  std::string sql;
  std::shared_ptr<NativeLitePreparedPlan> preparedPlan;
  std::vector<std::vector<std::string> > parameterRows;
  std::vector<std::vector<bool> > parameterNullRows;
  QueryResult result;
  bool success;
  std::string error;
  bool complete;
  std::mutex mutex;
  std::condition_variable condition;

  explicit EngineRequest(RequestType requestType)
      : type(requestType), success(false), complete(false)
  {
  }
};

uint32_t oidForType(Lng32 fsType, Lng32 precision, Lng32 scale)
{
  switch (fsType)
    {
    case REC_BOOLEAN:
      return 16;
    case REC_BIN8_SIGNED:
    case REC_BIN8_UNSIGNED:
    case REC_BIN16_SIGNED:
      return scale == 0 ? 21 : 1700;
    case REC_BPINT_UNSIGNED:
    case REC_BIN16_UNSIGNED:
    case REC_BIN32_SIGNED:
      return scale == 0 ? 23 : 1700;
    case REC_BIN32_UNSIGNED:
    case REC_BIN64_SIGNED:
      return scale == 0 ? 20 : 1700;
    case REC_BIN64_UNSIGNED:
      return 1700;
    case REC_FLOAT32:
      return 700;
    case REC_FLOAT64:
      return 701;
    case REC_DECIMAL_UNSIGNED:
    case REC_DECIMAL_LS:
    case REC_DECIMAL_LSE:
    case REC_NUM_BIG_SIGNED:
    case REC_NUM_BIG_UNSIGNED:
      return 1700;
    case REC_BINARY_STRING:
    case REC_VARBINARY_STRING:
      return 17;
    case REC_DATETIME:
      if (precision == REC_DTCODE_DATE)
        return 1082;
      if (precision == REC_DTCODE_TIME)
        return 1083;
      return 1114;
    case REC_BYTE_F_ASCII:
    case REC_NCHAR_F_UNICODE:
      return 1042;
    case REC_BYTE_V_ASCII:
    case REC_BYTE_V_ASCII_LONG:
    case REC_BYTE_V_ANSI:
    case REC_NCHAR_V_UNICODE:
    case REC_NCHAR_V_ANSI_UNICODE:
      return 1043;
    default:
      return 25;
    }
}

int16_t lengthForOid(uint32_t oid)
{
  switch (oid)
    {
    case 16:
      return 1;
    case 20:
      return 8;
    case 21:
      return 2;
    case 23:
    case 700:
      return 4;
    case 701:
      return 8;
    case 1082:
      return 4;
    case 1083:
    case 1114:
      return 8;
    default:
      return -1;
    }
}

std::string wideMessageToUtf8(const NAWchar *message)
{
  if (!message)
    return std::string();
  std::string result;
  for (size_t i = 0; message[i] != 0; i++)
    {
      uint32_t code = static_cast<uint16_t>(message[i]);
      if (code < 0x80)
        result.push_back(static_cast<char>(code));
      else if (code < 0x800)
        {
          result.push_back(static_cast<char>(0xc0 | (code >> 6)));
          result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
      else
        {
          result.push_back(static_cast<char>(0xe0 | (code >> 12)));
          result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
          result.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
  return result;
}

void setDiagnosticsError(ExeCliInterface *cli, Lng32 returnCode,
                         QueryResult *result)
{
  ComDiagsArea *diags = NULL;
  cli->allocAndRetrieveSQLDiagnostics(diags);
  if (diags && diags->getNumber(DgSqlCode::ERROR_) > 0)
    {
      ComCondition *condition = diags->getErrorEntry(1);
      char state[6] = "XX000";
      condition->getSQLSTATE(state);
      state[5] = '\0';
      result->sqlstate = state;
      result->error = wideMessageToUtf8(condition->getMessageText(FALSE));
    }
  if (result->error.empty())
    {
      std::ostringstream text;
      text << "NativeLite CLI error " << returnCode;
      result->error = text.str();
      result->sqlstate = "XX000";
    }
  if (returnCode == -4082 || returnCode == 4082)
    result->sqlstate = "42P01";
  if (diags)
    diags->deAllocate();
}

void clearCapture(Logfile *log)
{
  FILE *stream = log ? log->GetLogfile() : NULL;
  if (!stream)
    return;
  fflush(stream);
  int truncateResult = ftruncate(fileno(stream), 0);
  (void) truncateResult;
  rewind(stream);
  clearerr(stream);
}

std::string readCapture(Logfile *log)
{
  FILE *stream = log ? log->GetLogfile() : NULL;
  if (!stream)
    return std::string();
  fflush(stream);
  rewind(stream);
  std::string content;
  char buffer[4096];
  for (;;)
    {
      size_t count = fread(buffer, 1, sizeof(buffer), stream);
      if (count > 0)
        content.append(buffer, count);
      if (count < sizeof(buffer))
        break;
    }
  clearerr(stream);
  fseek(stream, 0, SEEK_END);
  return trim(content);
}

std::string sqlstateForUtilityError(const std::string &message)
{
  std::string text = upper(message);
  if (text.find("AUTHORIZATION") != std::string::npos ||
      text.find("PRIVILEGE") != std::string::npos ||
      text.find("ONLY DB__ROOT") != std::string::npos ||
      text.find("OWNER") != std::string::npos)
    return "42501";
  if (text.find("DOES NOT EXIST") != std::string::npos ||
      text.find("NOT EXIST") != std::string::npos)
    return "42P01";
  if (text.find("ALREADY EXISTS") != std::string::npos)
    return "42P07";
  return "HY000";
}

std::string commandTag(const std::string &sql, Int64 rows, size_t selected)
{
  std::string word = firstWord(sql);
  std::ostringstream tag;
  if (word == "SELECT" || word == "WITH" || word == "VALUES")
    tag << "SELECT " << selected;
  else if (word == "INSERT" || word == "UPSERT")
    tag << "INSERT 0 " << std::max<Int64>(rows, 0);
  else if (word == "UPDATE" || word == "DELETE" || word == "MERGE")
    tag << word << " " << std::max<Int64>(rows, 0);
  else if (word == "BEGIN" || word == "START")
    tag << "BEGIN";
  else if (word == "COMMIT")
    tag << "COMMIT";
  else if (word == "ROLLBACK")
    tag << "ROLLBACK";
  else if (word == "CREATE")
    {
      std::string normalized = upper(trim(sql));
      tag << (startsWith(normalized, "CREATE TABLE") ? "CREATE TABLE" :
              startsWith(normalized, "CREATE VIEW") ? "CREATE VIEW" :
              startsWith(normalized, "CREATE SCHEMA") ? "CREATE SCHEMA" :
              startsWith(normalized, "CREATE USER") ? "CREATE USER" :
              startsWith(normalized, "CREATE ROLE") ? "CREATE ROLE" :
              "CREATE");
    }
  else if (word == "DROP")
    tag << "DROP";
  else if (word == "SET" || word == "CONTROL")
    tag << "SET";
  else if (word.empty())
    tag << "EMPTY";
  else
    tag << word;
  return tag.str();
}

bool isRollback(const std::string &sql)
{
  return firstWord(sql) == "ROLLBACK";
}

class NativeLiteEngine
{
public:
  NativeLiteEngine()
      : initialized_(false), initializationFailed_(false), stopping_(false),
        defaultContext_(0), bootstrapEnv_(NULL), activeExecutorRequests_(0),
        maximumExecutorRequests_(0), activeCompilerRequests_(0),
        maximumCompilerRequests_(0)
  {
    worker_ = std::thread(&NativeLiteEngine::run, this);
  }

  ~NativeLiteEngine()
  {
    stop();
  }

  bool waitUntilReady(std::string *error)
  {
    std::unique_lock<std::mutex> lock(queueMutex_);
    readyCondition_.wait(lock, [this] { return initialized_.load(); });
    if (initializationFailed_ && error)
      *error = initializationError_;
    return !initializationFailed_;
  }

  bool create(const std::shared_ptr<Session> &session, std::string *error)
  {
    EngineRequest request(REQUEST_CREATE);
    request.session = session;
    submit(&request);
    if (!request.success && error)
      *error = request.error;
    return request.success;
  }

  QueryResult execute(const std::shared_ptr<Session> &session,
                      const std::string &sql)
  {
    EngineRequest request(REQUEST_EXECUTE);
    request.session = session;
    request.sql = sql;
    session->cancelRequested.store(false);
    session->statementPending.store(true);
    submit(&request);
    session->statementPending.store(false);
    session->cancelRequested.store(false);
    return request.result;
  }

  QueryResult describe(const std::shared_ptr<Session> &session,
                       const std::string &sql)
  {
    EngineRequest request(REQUEST_DESCRIBE);
    request.session = session;
    request.sql = sql;
    session->cancelRequested.store(false);
    session->statementPending.store(true);
    submit(&request);
    session->statementPending.store(false);
    session->cancelRequested.store(false);
    return request.result;
  }

  std::shared_ptr<NativeLitePreparedPlan> prepare(
      const std::shared_ptr<Session> &session, const std::string &sql,
      QueryResult *result)
  {
    EngineRequest request(REQUEST_PREPARE);
    request.session = session;
    request.sql = sql;
    session->cancelRequested.store(false);
    session->statementPending.store(true);
    submit(&request);
    session->statementPending.store(false);
    session->cancelRequested.store(false);
    if (result)
      *result = request.result;
    return request.preparedPlan;
  }

  QueryResult executePrepared(
      const std::shared_ptr<Session> &session,
      const std::shared_ptr<NativeLitePreparedPlan> &plan,
      const std::vector<std::vector<std::string> > &rows,
      const std::vector<std::vector<bool> > &nullRows)
  {
    EngineRequest request(REQUEST_EXECUTE_PREPARED);
    request.session = session;
    request.preparedPlan = plan;
    request.parameterRows = rows;
    request.parameterNullRows = nullRows;
    session->cancelRequested.store(false);
    session->statementPending.store(true);
    submit(&request);
    session->statementPending.store(false);
    session->cancelRequested.store(false);
    return request.result;
  }

  QueryResult executeBatch(const std::shared_ptr<Session> &session,
                           const std::vector<std::string> &statements)
  {
    QueryResult result;
    if (statements.empty())
      {
        result.sqlstate = "42601";
        result.error = "NativeLite batch contains no statements";
        return result;
      }
    bool selectBatch = true;
    for (size_t index = 0; index < statements.size(); index++)
      {
        const std::string verb = firstWord(statements[index]);
        if (verb != "SELECT" && verb != "WITH" && verb != "VALUES")
          {
            selectBatch = false;
            break;
          }
      }
    QueryResult aggregate;
    for (size_t index = 0; index < statements.size(); index++)
      {
        result = execute(session, statements[index]);
        if (!result.ok())
          return result;
        if (selectBatch)
          {
            if (index == 0)
              aggregate = result;
            else
              {
                if (result.columns.size() != aggregate.columns.size())
                  {
                    aggregate.sqlstate = "21000";
                    aggregate.error =
                        "NativeLite SELECT batch column count mismatch";
                    return aggregate;
                  }
                for (size_t column = 0;
                     column < aggregate.columns.size(); column++)
                  if (result.columns[column].oid !=
                          aggregate.columns[column].oid ||
                      result.columns[column].typeLength !=
                          aggregate.columns[column].typeLength)
                    {
                      aggregate.sqlstate = "42804";
                      aggregate.error =
                          "NativeLite SELECT batch column type mismatch";
                      return aggregate;
                    }
                aggregate.rows.insert(aggregate.rows.end(),
                                      result.rows.begin(), result.rows.end());
              }
          }
      }
    if (selectBatch)
      {
        aggregate.commandTag = "SELECT " +
            std::to_string(aggregate.rows.size());
        return aggregate;
      }
    return result;
  }

  void destroy(const std::shared_ptr<Session> &session)
  {
    if (!session || session->contextHandle == 0)
      return;
    EngineRequest request(REQUEST_DESTROY);
    request.session = session;
    submit(&request);
  }

  void cancel(const std::shared_ptr<Session> &session)
  {
    if (!session)
      return;
    SQLCTX_HANDLE contextHandle = 0;
    {
      std::lock_guard<std::mutex> lock(session->lifecycleMutex);
      if (session->closing || !session->statementPending.load() ||
          session->contextHandle == 0)
        return;
      session->activeCancels++;
      contextHandle = session->contextHandle;
      session->cancelRequested.store(true);
    }

    // SQL_EXEC_Cancel is explicitly designed for a cancel thread and does not
    // acquire the normal CLI semaphore. Switch this thread to the target
    // ContextCli so cancellation cannot affect a peer session.
    SQLCTX_HANDLE previous = 0;
    if (SQL_EXEC_SwitchContext_Internal(contextHandle, &previous, TRUE) == 0)
      {
        SQL_EXEC_Cancel(NULL);
        if (previous != 0)
          SQL_EXEC_SwitchContext_Internal(previous, NULL, TRUE);
      }
    {
      std::lock_guard<std::mutex> lock(session->lifecycleMutex);
      if (session->activeCancels > 0)
        session->activeCancels--;
      session->cancelCondition.notify_all();
    }
  }

  void stop()
  {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true))
      {
        if (worker_.joinable())
          worker_.join();
        return;
      }
    queueCondition_.notify_all();
    if (worker_.joinable())
      worker_.join();
  }

private:
  void submit(EngineRequest *request)
  {
    // Execution is session-affine in M21.  The connection thread owns the
    // session ContextCli, SqlciEnv, compiler context and prepared plans for
    // the whole connection lifetime.  Keep EngineRequest as a small local
    // compatibility envelope while removing the global execution queue.
    runDirect(request);
  }

  void runDirect(EngineRequest *request)
  {
    ExportJmpBufPtr = &ExportJmpBuf;
    if (setjmp(ExportJmpBuf))
      {
        std::cerr << "NativeLite terminating after a CLI assertion"
                  << std::endl;
        _exit(1);
      }
    processRequest(request);
    finish(request);
  }

  void processRequest(EngineRequest *request)
  {
    if (request->session && request->type != REQUEST_CREATE)
      {
        const std::thread::id owner = request->session->ownerThread;
        if (owner != std::thread::id() && owner != std::this_thread::get_id())
          {
            request->result.sqlstate = "08003";
            request->result.error =
                "NativeLite session request crossed its owner thread";
            request->success = false;
            return;
          }
      }
    if (request->type == REQUEST_CREATE)
      createSession(request);
    else if (request->type == REQUEST_DESTROY)
      destroySession(request);
    else if (request->type == REQUEST_DESCRIBE)
      request->result = runStatement(request->session, request->sql, true);
    else if (request->type == REQUEST_EXECUTE)
      request->result = runStatement(request->session, request->sql, false);
    else if (request->type == REQUEST_PREPARE)
      prepareStatement(request);
    else if (request->type == REQUEST_EXECUTE_PREPARED)
      request->result = executePreparedStatement(
          request->session, request->preparedPlan,
          request->parameterRows, request->parameterNullRows);
  }

  void finish(EngineRequest *request)
  {
    {
      std::lock_guard<std::mutex> lock(request->mutex);
      request->complete = true;
    }
    request->condition.notify_one();
  }

  void run()
  {
    ExportJmpBufPtr = &ExportJmpBuf;
    SqlciEnv bootstrap;
    bootstrapEnv_ = &bootstrap;
    bootstrap.setNoBanner(TRUE);
    bootstrap.get_logfile()->setNoDisplay(TRUE);
    global_sqlci_env = &bootstrap;

    if (setjmp(ExportJmpBuf))
      {
        if (initialized_.load())
          {
            // SQLCI treats an assertion as process-fatal. The engine cannot
            // safely unwind arbitrary compiler/executor frames with C++
            // destructors after longjmp, so terminate instead of leaving a
            // client blocked forever on an abandoned EngineRequest.
            std::cerr << "NativeLite terminating after a CLI assertion"
                      << std::endl;
            _exit(1);
          }
        std::lock_guard<std::mutex> lock(queueMutex_);
        initializationFailed_ = true;
        initializationError_ = "CLI assertion while initializing NativeLite";
        initialized_.store(true);
        readyCondition_.notify_all();
        return;
      }

    Lng32 rc = SQL_EXEC_CurrentContext(&defaultContext_);
    if (rc != 0)
      {
        std::ostringstream text;
        text << "unable to initialize CLI context: " << rc;
        initializationFailed_ = true;
        initializationError_ = text.str();
      }
    else
      {
        QRLogger::initLog4cxx(QRLogger::QRL_MXEXE);
        if (!storeLease_.open(&initializationError_))
          initializationFailed_ = true;
      }

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      initialized_.store(true);
    }
    readyCondition_.notify_all();
    if (initializationFailed_)
      return;

    // This thread is only the process bootstrap/lifecycle thread in M21.  It
    // never consumes client execution requests.  Keeping the default context
    // alive here lets connection threads create/delete session contexts under
    // contextLifecycleMutex_ without sharing a session's TLS state.
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCondition_.wait(lock, [this] { return stopping_.load(); });
    }

    storeLease_.close();
    global_sqlci_env = NULL;
    bootstrapEnv_ = NULL;
  }

  bool switchTo(const std::shared_ptr<Session> &session)
  {
    return session && session->contextHandle != 0 &&
           SQL_EXEC_SwitchContext_Internal(session->contextHandle, NULL,
                                           TRUE) == 0;
  }

  void createSession(EngineRequest *request)
  {
    std::lock_guard<std::mutex> lifecycleLock(contextLifecycleMutex_);
    std::shared_ptr<Session> session = request->session;
    SQL_EXEC_SwitchContext_Internal(defaultContext_, NULL, TRUE);
    Lng32 rc = SQL_EXEC_CreateContext(&session->contextHandle, NULL, 0);
    if (rc != 0)
      {
        std::ostringstream text;
        text << "unable to create session context: " << rc;
        request->error = text.str();
        return;
      }
    if (!switchTo(session))
      {
        request->error = "unable to switch to the new session context";
        cleanupFailedCreate(session);
        return;
      }

    session->env = new SqlciEnv();
    session->env->setNoBanner(TRUE);
    session->env->setTerminalCharset(CharInfo::UTF8);
    session->env->get_logfile()->setNoDisplay(TRUE);
    session->env->setUserNameFromCommandLine(session->user.c_str());

    if (!session->env->get_logfile()->OpenTemporary())
      {
        request->error = "unable to create session diagnostic capture";
        cleanupFailedCreate(session);
        return;
      }

    global_sqlci_env = session->env;
    short identityRc = 0;
    if (!LiteSqlTable_setCurrentUser(session->env, &identityRc))
      {
        request->error = readCapture(session->env->get_logfile());
        if (request->error.empty())
          request->error = "unknown NativeLite authorization identity";
        cleanupFailedCreate(session);
        return;
      }

    ContextCli *context = GetCliGlobals()->currContext();
    ExeCliInterface cli(context->exHeap(), SQLCHARSETCODE_UTF8, context);
    cli.setNotExeUtilInternalQuery(TRUE);
    const char *sessionDefaults[] = {
      "SET SESSION DEFAULT INTERNAL_FORMAT_IO 'ON';",
      "CONTROL QUERY DEFAULT TRAF_TINYINT_SUPPORT 'ON';",
      "CONTROL QUERY DEFAULT TRAF_TINYINT_RETURN_VALUES 'ON';",
      "CONTROL QUERY DEFAULT TRAF_TINYINT_INPUT_PARAMS 'ON';",
      "CONTROL QUERY DEFAULT TRAF_LARGEINT_UNSIGNED_IO 'ON';",
      "CONTROL QUERY DEFAULT TRAF_BOOLEAN_IO 'ON';",
      "CONTROL QUERY DEFAULT TRAF_BINARY_SUPPORT 'ON';",
      "CONTROL QUERY DEFAULT TRAF_BINARY_INPUT 'ON';",
      "CONTROL QUERY DEFAULT TRAF_BINARY_OUTPUT 'ON';"
    };
    for (size_t i = 0;
         i < sizeof(sessionDefaults) / sizeof(sessionDefaults[0]); i++)
      {
        rc = cli.executeImmediate(sessionDefaults[i], NULL, NULL, TRUE);
        if (rc < 0)
          {
            QueryResult failure;
            setDiagnosticsError(&cli, rc, &failure);
            request->error = failure.error;
            cleanupFailedCreate(session);
            return;
          }
      }

    updateTransactionStatus(session);
    session->ownerThread = std::this_thread::get_id();
    request->success = true;
  }

  void cleanupFailedCreate(const std::shared_ptr<Session> &session)
  {
    SQLCTX_HANDLE failedContext = session->contextHandle;
    SQL_EXEC_SwitchContext_Internal(defaultContext_, NULL, TRUE);
    global_sqlci_env = NULL;
    if (failedContext != 0)
      SQL_EXEC_DeleteContext(failedContext);
    session->contextHandle = 0;
    if (session->env)
      {
        delete session->env;
        session->env = NULL;
      }
  }

  void destroySession(EngineRequest *request)
  {
    std::shared_ptr<Session> session = request->session;
    if (session)
      {
        std::unique_lock<std::mutex> sessionLock(session->lifecycleMutex);
        session->closing = true;
        session->cancelCondition.wait(
            sessionLock, [session] { return session->activeCancels == 0; });
      }
    std::lock_guard<std::mutex> lifecycleLock(contextLifecycleMutex_);
    if (!session || session->contextHandle == 0)
      {
        request->success = true;
        return;
      }
    SQLCTX_HANDLE handle = session->contextHandle;
    if (switchTo(session))
      {
        ContextCli *context = GetCliGlobals()->currContext();
        if (context && context->getTransaction())
          context->getTransaction()->resetLiteTransaction();
        SQL_EXEC_ResetContext(handle, NULL);
      }
    SQL_EXEC_SwitchContext_Internal(defaultContext_, NULL, TRUE);
    global_sqlci_env = NULL;
    SQL_EXEC_DeleteContext(handle);
    session->contextHandle = 0;
    if (session->env)
      {
        delete session->env;
        session->env = NULL;
      }
    session->transactionStatus.store('I');
    request->success = true;
  }

  void applySessionEnvironment(const std::shared_ptr<Session> &session)
  {
    const char *catalog = session->env->defaultCatalog();
    const char *schema = session->env->defaultSchema();
    std::string qualified =
        std::string(catalog && catalog[0] ? catalog : "TRAFODION") + "." +
        (schema && schema[0] ? schema : "SEABASE");
    LiteSetThreadDefaultSchema(qualified.c_str());
    global_sqlci_env = session->env;
  }

  void updateTransactionStatus(const std::shared_ptr<Session> &session)
  {
    bool active = currentTransactionInProgress();
    if (!active)
      {
        session->failedTransaction = false;
        session->transactionStatus.store('I');
      }
    else
      session->transactionStatus.store(
          session->failedTransaction ? 'E' : 'T');
  }

  QueryResult builtin(const std::shared_ptr<Session> &session,
                      const std::string &sql, bool describeOnly,
                      bool *handled)
  {
    QueryResult result;
    std::string normalized = upper(trim(sql));
    while (!normalized.empty() && normalized[normalized.size() - 1] == ';')
      normalized = trim(normalized.substr(0, normalized.size() - 1));

    // SQLCI GET metadata commands are implemented by the local catalog
    // utility layer.  That layer historically rendered them only to the
    // SQLCI logfile, so a network client received an empty command result.
    // Return the same object list as a one-column result set for T4 clients.
    if (normalized.find("GET ") == 0)
      {
        std::string title;
        std::vector<std::string> objects;
        std::string metadataError;
        if (LiteSqlTable_getMetadata(sql.c_str(), session->env, &title,
                                          &objects, &metadataError))
          {
            *handled = true;
            if (!metadataError.empty())
              {
                result.sqlstate = "HY000";
                result.error = metadataError;
                return result;
              }

            Column column;
            column.name = "OBJECT_NAME";
            if (title.find("Tables") == 0)
              column.name = "TABLE_NAME";
            else if (title.find("Schemas") == 0)
              column.name = "SCHEMA_NAME";
            else if (title.find("Catalogs") == 0 ||
                     title.find("Databases") == 0)
              column.name = "CATALOG_NAME";
            else if (title.find("Views") == 0)
              column.name = "VIEW_NAME";
            else if (title.find("Indexes") == 0)
              column.name = "INDEX_NAME";
            else if (title.find("Sequences") == 0)
              column.name = "SEQUENCE_NAME";
            column.length = 256;
            result.columns.push_back(column);
            if (!describeOnly)
              for (size_t i = 0; i < objects.size(); i++)
                result.rows.push_back(
                    std::vector<Cell>(1, Cell(objects[i])));
            result.commandTag = "SELECT " +
                std::to_string(static_cast<unsigned long long>(objects.size()));
            return result;
          }
      }

    if (normalized == "SELECT NATIVE_LITE_HEALTH()" ||
        normalized == "SELECT NATIVELITE_HEALTH()" ||
        normalized == "SHOW NATIVE_LITE HEALTH")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_health";
        column.oid = 25;
        column.typeLength = -1;
        result.columns.push_back(column);
        result.rows.push_back(std::vector<Cell>(1, Cell("ok")));
        result.commandTag = "SELECT 1";
        return result;
      }

    if (normalized == "SELECT NATIVE_LITE_EXECUTOR_OVERLAP()" ||
        normalized == "SELECT NATIVELITE_EXECUTOR_OVERLAP()")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_executor_overlap";
        column.oid = 23;
        column.typeLength = 4;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (!describeOnly)
          {
            std::ostringstream value;
            value << maximumExecutorRequests_.load();
            result.rows.push_back(std::vector<Cell>(1, Cell(value.str())));
          }
        return result;
      }

    if (normalized == "SELECT NATIVE_LITE_COMPILER_OVERLAP()" ||
        normalized == "SELECT NATIVELITE_COMPILER_OVERLAP()")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_compiler_overlap";
        column.oid = 23;
        column.typeLength = 4;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (!describeOnly)
          {
            std::ostringstream value;
            value << maximumCompilerRequests_.load();
            result.rows.push_back(std::vector<Cell>(1, Cell(value.str())));
          }
        return result;
      }

    if (normalized == "SELECT NATIVE_LITE_OCC_METRICS()" ||
        normalized == "SELECT NATIVELITE_OCC_METRICS()")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_occ_metrics";
        column.oid = 25;
        column.typeLength = -1;
        column.length = 4096;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (!describeOnly)
          result.rows.push_back(
              std::vector<Cell>(1, Cell(LiteOccMetricsJson())));
        return result;
      }

    if (normalized == "SELECT NATIVE_LITE_OCC_METRICS_RESET()" ||
        normalized == "SELECT NATIVELITE_OCC_METRICS_RESET()")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_occ_metrics_reset";
        column.oid = 25;
        column.typeLength = -1;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (!describeOnly)
          {
            LiteOccMetricsReset();
            result.rows.push_back(std::vector<Cell>(1, Cell("ok")));
          }
        return result;
      }

    if (normalized == "SELECT NATIVE_LITE_COMPILER_METRICS()" ||
        normalized == "SELECT NATIVELITE_COMPILER_METRICS()")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_compiler_metrics";
        column.oid = 25;
        column.typeLength = -1;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (!describeOnly)
          {
            std::ostringstream value;
            value << "{\"maximum_compile_overlap\":"
                  << maximumCompilerRequests_.load() << "}";
            result.rows.push_back(std::vector<Cell>(1, Cell(value.str())));
          }
        return result;
      }

    if (normalized == "SELECT NATIVE_LITE_CHECKPOINT()" ||
        normalized == "SELECT NATIVELITE_CHECKPOINT()")
      {
        *handled = true;
        Column column;
        column.name = "native_lite_checkpoint";
        column.oid = 25;
        column.typeLength = -1;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (describeOnly)
          return result;
        const char *target = getenv("TRAF_LITE_CHECKPOINT_DIR");
        std::string checkpointError;
        bool checkpointed = false;
        if (target && target[0])
          {
            checkpointed = LiteRocksDBCheckpoint(target,
                                                       &checkpointError);
          }
        if (!checkpointed)
          {
            result.sqlstate = "58030";
            result.error = checkpointError.empty()
                ? "TRAF_LITE_CHECKPOINT_DIR is not configured"
                : checkpointError;
            return result;
          }
        result.rows.push_back(std::vector<Cell>(1, Cell("ok")));
        return result;
      }

    const std::string sleepPrefix = "SELECT NATIVE_LITE_SLEEP(";
    const std::string alternatePrefix = "SELECT NATIVELITE_SLEEP(";
    size_t prefixLength = startsWith(normalized, sleepPrefix)
        ? sleepPrefix.size()
        : startsWith(normalized, alternatePrefix) ? alternatePrefix.size() : 0;
    if (prefixLength != 0 && normalized[normalized.size() - 1] == ')')
      {
        *handled = true;
        std::string argument = trim(normalized.substr(
            prefixLength, normalized.size() - prefixLength - 1));
        char *end = NULL;
        long milliseconds = strtol(argument.c_str(), &end, 10);
        if (!end || *end != '\0' || milliseconds < 0 || milliseconds > 60000)
          {
            result.sqlstate = "22023";
            result.error = "NATIVE_LITE_SLEEP requires 0..60000 milliseconds";
            return result;
          }

        Column column;
        column.name = "native_lite_sleep";
        column.oid = 23;
        column.typeLength = 4;
        result.columns.push_back(column);
        result.commandTag = "SELECT 1";
        if (describeOnly)
          return result;

        ContextCli *context = GetCliGlobals()->currContext();
        int statementToken = 0;
        LiteTxnContext *txn = context && context->getTransaction()
            ? context->getLiteTxnContext() : NULL;
        uint64_t sequence = ++session->statementSequence;
        if (txn)
          LiteTxnManager::beginStatement(txn, &statementToken, sequence);
        long elapsed = 0;
        while (elapsed < milliseconds && !session->cancelRequested.load())
          {
            long interval = std::min<long>(10, milliseconds - elapsed);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
            elapsed += interval;
          }
        if (txn)
          LiteTxnManager::endStatement(txn, &statementToken, sequence);
        if (session->cancelRequested.load())
          {
            result.sqlstate = "57014";
            result.error = "canceling statement due to user request";
            return result;
          }
        std::ostringstream value;
        value << milliseconds;
        result.rows.push_back(std::vector<Cell>(1, Cell(value.str())));
        return result;
      }

    *handled = false;
    return result;
  }

  bool loadColumns(ExeCliInterface *cli, SqlciEnv *env,
                   std::vector<Column> *columns, QueryResult *result)
  {
    Lng32 inputCount = 0;
    Lng32 outputCount = 0;
    Lng32 rc = cli->getNumEntries(inputCount, outputCount);
    if (rc < 0)
      {
        setDiagnosticsError(cli, rc, result);
        return false;
      }
    for (Lng32 index = 1; index <= outputCount; index++)
      {
        Column column;
        char name[1024];
        Lng32 nameLength = 0;
        memset(name, 0, sizeof(name));
        cli->getHeadingAndLen(static_cast<short>(index), name, nameLength);
        if (nameLength > 0)
          column.name.assign(name, static_cast<size_t>(nameLength));
        else
          {
            std::ostringstream generated;
            generated << "column" << index;
            column.name = generated.str();
          }
        cli->getOutputDescItem(index, SQLDESC_TYPE_FS, &column.fsType);
        cli->getOutputDescItem(index, SQLDESC_OCTET_LENGTH, &column.length);
        cli->getOutputDescItem(index, SQLDESC_NULLABLE, &column.nullable);
        if (column.fsType == REC_DATETIME)
          {
            cli->getOutputDescItem(index, SQLDESC_DATETIME_CODE,
                                   &column.precision);
            cli->getOutputDescItem(index, SQLDESC_PRECISION, &column.scale);
          }
        else if (column.fsType >= REC_MIN_INTERVAL &&
                 column.fsType <= REC_MAX_INTERVAL)
          {
            cli->getOutputDescItem(index, SQLDESC_INT_LEAD_PREC,
                                   &column.precision);
            cli->getOutputDescItem(index, SQLDESC_PRECISION, &column.scale);
          }
        else
          {
            cli->getOutputDescItem(index, SQLDESC_PRECISION,
                                   &column.precision);
            cli->getOutputDescItem(index, SQLDESC_SCALE, &column.scale);
          }
        if (DFS2REC::isAnyCharacter(column.fsType))
          cli->getOutputDescItem(index, SQLDESC_CHAR_SET, &column.charset);
        column.oid = oidForType(column.fsType, column.precision, column.scale);
        column.typeLength = lengthForOid(column.oid);
        if (column.oid == 1700 && column.precision > 0 &&
            column.precision <= 0x7fff && column.scale >= 0 &&
            column.scale <= 0x7fff)
          column.typeModifier =
              ((column.precision << 16) | column.scale) + 4;
        columns->push_back(column);
      }
    return true;
  }

  Cell formatCell(ExeCliInterface *cli, SqlciEnv *env,
                  const Column &column, short entry)
  {
    Long dataAddress = 0;
    Long indicatorAddress = 0;
    cli->getOutputDescItem(entry, SQLDESC_VAR_PTR, &dataAddress);
    cli->getOutputDescItem(entry, SQLDESC_IND_PTR, &indicatorAddress);
    if (indicatorAddress != 0 &&
        *reinterpret_cast<short *>(indicatorAddress) < 0)
      return Cell();

    const char *data = reinterpret_cast<const char *>(dataAddress);
    std::ostringstream numeric;
    switch (column.fsType)
      {
      case REC_BOOLEAN:
        numeric << (*reinterpret_cast<const unsigned char *>(data) ? "t" :
                                                                     "f");
        return Cell(numeric.str());
      case REC_BIN8_SIGNED:
        if (column.scale != 0)
          break;
        numeric << static_cast<int>(*reinterpret_cast<const int8_t *>(data));
        return Cell(numeric.str());
      case REC_BIN8_UNSIGNED:
        if (column.scale != 0)
          break;
        numeric << static_cast<unsigned int>(
            *reinterpret_cast<const uint8_t *>(data));
        return Cell(numeric.str());
      case REC_BIN16_SIGNED:
        {
          if (column.scale != 0)
            break;
          int16_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_BIN16_UNSIGNED:
        {
          if (column.scale != 0)
            break;
          uint16_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_BPINT_UNSIGNED:
        {
          if (column.scale != 0)
            break;
          uint16_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_BIN32_SIGNED:
        {
          if (column.scale != 0)
            break;
          int32_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_BIN32_UNSIGNED:
        {
          if (column.scale != 0)
            break;
          uint32_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_BIN64_SIGNED:
        {
          if (column.scale != 0)
            break;
          int64_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_BIN64_UNSIGNED:
        {
          if (column.scale != 0)
            break;
          uint64_t value;
          memcpy(&value, data, sizeof(value));
          numeric << value;
          return Cell(numeric.str());
        }
      case REC_FLOAT32:
        {
          float value;
          memcpy(&value, data, sizeof(value));
          numeric << std::setprecision(std::numeric_limits<float>::max_digits10)
                  << value;
          return Cell(numeric.str());
        }
      case REC_FLOAT64:
        {
          double value;
          memcpy(&value, data, sizeof(value));
          numeric << std::setprecision(std::numeric_limits<double>::max_digits10)
                  << value;
          return Cell(numeric.str());
        }
      default:
        break;
      }

    Lng32 displayBufferLength = 0;
    Lng32 displayLength = Formatter::display_length(
        column.fsType, column.length, column.precision, column.scale,
        column.charset, static_cast<Lng32>(column.name.size()), env,
        &displayBufferLength);
    if (displayBufferLength <= 0 || displayLength <= 0)
      return Cell(std::string());
    std::vector<char> buffer(static_cast<size_t>(displayBufferLength) + 1, 0);
    Lng32 position = 0;
    Formatter::buffer_it(env, reinterpret_cast<char *>(dataAddress),
                         column.fsType, column.length, column.precision,
                         column.scale,
                         reinterpret_cast<char *>(indicatorAddress),
                         displayLength, displayBufferLength, column.nullable,
                         &buffer[0], &position, FALSE, FALSE);
    std::string value(&buffer[0], static_cast<size_t>(position));
    while (!value.empty() && value[value.size() - 1] == ' ')
      value.resize(value.size() - 1);
    size_t begin = 0;
    if (column.oid != 25 && column.oid != 1042 && column.oid != 1043 &&
        column.oid != 17)
      while (begin < value.size() && value[begin] == ' ')
        begin++;
    value = value.substr(begin);
    if (column.oid == 17)
      value = "\\x" + value;
    return Cell(value);
  }

  bool preparedParameterSupported(Lng32 fsDatatype) const
  {
    if (DFS2REC::isAnyCharacter(fsDatatype))
      return true;
    switch (fsDatatype)
      {
      case REC_BOOLEAN:
      case REC_BIN8_SIGNED:
      case REC_BIN8_UNSIGNED:
      case REC_BIN16_SIGNED:
      case REC_BIN16_UNSIGNED:
      case REC_BPINT_UNSIGNED:
      case REC_BIN32_SIGNED:
      case REC_BIN32_UNSIGNED:
      case REC_BIN64_SIGNED:
      case REC_BIN64_UNSIGNED:
      case REC_FLOAT32:
      case REC_FLOAT64:
        return true;
      default:
        return false;
      }
  }

  bool bindPreparedParameters(
      const std::shared_ptr<NativeLitePreparedPlan> &plan,
      const std::vector<std::string> &values,
      const std::vector<bool> &nulls, std::string *error)
  {
    if (!plan || !plan->cli || values.size() != plan->parameters.size() ||
        nulls.size() != values.size())
      {
        if (error)
          *error = "NativeLite prepared parameter count mismatch";
        return false;
      }
    char *input = plan->cli->inputBuf();
    const Lng32 inputLength = plan->cli->inputDatalen();
    if (inputLength > 0 && !input)
      {
        if (error)
          *error = "NativeLite prepared input buffer is unavailable";
        return false;
      }
    if (input && inputLength > 0)
      memset(input, 0, static_cast<size_t>(inputLength));

    for (size_t index = 0; index < values.size(); index++)
      {
        const NativeLitePreparedParameter &parameter =
            plan->parameters[index];
        if (parameter.indOffset >= 0)
          *reinterpret_cast<int16_t *>(input + parameter.indOffset) =
              nulls[index] ? -1 : 0;
        if (nulls[index])
          continue;
        if (parameter.varOffset < 0 || parameter.length <= 0)
          {
            if (error)
              *error = "NativeLite prepared input descriptor has no data slot";
            return false;
          }
        char *target = input + parameter.varOffset;
        const std::string &value = values[index];
        if (DFS2REC::isAnyCharacter(parameter.fsDatatype))
          {
            if (DFS2REC::isAnyVarChar(parameter.fsDatatype))
              {
                const size_t maxLength = static_cast<size_t>(parameter.length);
                if (value.size() > maxLength)
                  {
                    if (error)
                      *error = "NativeLite prepared character parameter is too long";
                    return false;
                  }
                const uint16_t length = static_cast<uint16_t>(value.size());
                memcpy(target, &length, sizeof(length));
                memcpy(target + SQL_VARCHAR_HDR_SIZE, value.data(),
                       value.size());
              }
            else
              {
                memset(target, ' ', static_cast<size_t>(parameter.length));
                memcpy(target, value.data(),
                       std::min<size_t>(value.size(), parameter.length));
              }
            continue;
          }

        char *end = NULL;
        switch (parameter.fsDatatype)
          {
          case REC_BOOLEAN:
            *reinterpret_cast<unsigned char *>(target) =
                (upper(value) == "TRUE" || value == "1") ? 1 : 0;
            break;
          case REC_BIN8_SIGNED:
            *reinterpret_cast<int8_t *>(target) =
                static_cast<int8_t>(strtoll(value.c_str(), &end, 10));
            break;
          case REC_BIN8_UNSIGNED:
            *reinterpret_cast<uint8_t *>(target) =
                static_cast<uint8_t>(strtoull(value.c_str(), &end, 10));
            break;
          case REC_BIN16_SIGNED:
            {
              int16_t converted = static_cast<int16_t>(
                  strtoll(value.c_str(), &end, 10));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_BIN16_UNSIGNED:
          case REC_BPINT_UNSIGNED:
            {
              uint16_t converted = static_cast<uint16_t>(
                  strtoull(value.c_str(), &end, 10));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_BIN32_SIGNED:
            {
              int32_t converted = static_cast<int32_t>(
                  strtoll(value.c_str(), &end, 10));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_BIN32_UNSIGNED:
            {
              uint32_t converted = static_cast<uint32_t>(
                  strtoull(value.c_str(), &end, 10));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_BIN64_SIGNED:
            {
              int64_t converted = static_cast<int64_t>(
                  strtoll(value.c_str(), &end, 10));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_BIN64_UNSIGNED:
            {
              uint64_t converted = static_cast<uint64_t>(
                  strtoull(value.c_str(), &end, 10));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_FLOAT32:
            {
              float converted = static_cast<float>(strtod(value.c_str(), &end));
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          case REC_FLOAT64:
            {
              double converted = strtod(value.c_str(), &end);
              memcpy(target, &converted, sizeof(converted));
            }
            break;
          default:
            if (error)
              *error = "NativeLite prepared parameter type requires text binding";
            return false;
          }
        if (!end || *end != '\0')
          {
            if (error)
              *error = "NativeLite prepared numeric parameter is invalid";
            return false;
          }
      }
    return true;
  }

  Lng32 compilePlan(const std::shared_ptr<Session> &session,
                    ExeCliInterface *cli, const std::string &sql)
  {
    std::lock_guard<std::mutex> sessionCompilerLock(session->compilerMutex);
    const int active = activeCompilerRequests_.fetch_add(1) + 1;
    int maximum = maximumCompilerRequests_.load();
    while (active > maximum &&
           !maximumCompilerRequests_.compare_exchange_weak(maximum, active))
      {}
    const char *hold = getenv("TRAF_LITE_COMPILER_HOLD_MS");
    if (hold && hold[0])
      usleep(static_cast<useconds_t>(strtoul(hold, NULL, 10)) * 1000U);
    const Lng32 result = cli->fetchRowsPrologue(sql.c_str(), TRUE);
    activeCompilerRequests_.fetch_sub(1);
    return result;
  }

  void prepareStatement(EngineRequest *request)
  {
    QueryResult result;
    if (!switchTo(request->session))
      {
        result.sqlstate = "08003";
        result.error = "NativeLite session is closed";
        request->result = result;
        return;
      }
    applySessionEnvironment(request->session);
    if (request->session->failedTransaction)
      {
        result.sqlstate = "25P02";
        result.error = "current transaction is aborted; ROLLBACK required";
        request->result = result;
        return;
      }

    const size_t parameterCount = countT4Parameters(request->sql);
    std::vector<std::string> statements;
    statements.push_back(request->sql);
    std::string castSql;
    if (rewriteT4ParametersAsVarchar(request->sql, &castSql) &&
        castSql != request->sql)
      statements.push_back(castSql);

    for (size_t attempt = 0; attempt < statements.size(); attempt++)
      {
        std::shared_ptr<NativeLitePreparedPlan> plan(
            new NativeLitePreparedPlan());
        plan->sql = statements[attempt];
        plan->sourceSql = request->sql;
        ContextCli *context = GetCliGlobals()->currContext();
        plan->cli.reset(new ExeCliInterface(
            context->exHeap(), SQLCHARSETCODE_UTF8, context));
        plan->cli->setNotExeUtilInternalQuery(TRUE);
        Lng32 rc = 0;
        rc = compilePlan(request->session, plan->cli.get(), plan->sql);
        if (rc < 0)
          {
            setDiagnosticsError(plan->cli.get(), rc, &result);
            plan->cli->fetchRowsEpilogue(plan->sql.c_str());
            continue;
          }

        Lng32 inputCount = 0;
        Lng32 outputCount = 0;
        if (plan->cli->getNumEntries(inputCount, outputCount) < 0 ||
            static_cast<size_t>(inputCount) != parameterCount)
          {
            result.sqlstate = "07001";
            result.error = "NativeLite prepared parameter descriptor mismatch";
            plan->cli->fetchRowsEpilogue(plan->sql.c_str());
            continue;
          }
        plan->parameters.clear();
        bool unsupported = false;
        for (Lng32 entry = 1; entry <= inputCount; entry++)
          {
            NativeLitePreparedParameter parameter;
            plan->cli->getAttributes(entry, TRUE, parameter.fsDatatype,
                                     parameter.length,
                                     &parameter.indOffset,
                                     &parameter.varOffset);
            if (!preparedParameterSupported(parameter.fsDatatype))
              unsupported = true;
            plan->parameters.push_back(parameter);
          }
        if (unsupported && attempt == 0 && statements.size() > 1)
          {
            plan->cli->fetchRowsEpilogue(plan->sql.c_str());
            continue;
          }
        if (!loadColumns(plan->cli.get(), request->session->env,
                         &plan->description.columns, &result))
          {
            plan->cli->fetchRowsEpilogue(plan->sql.c_str());
            continue;
          }
        plan->description.commandTag = commandTag(request->sql, 0, 0);
        result = plan->description;
        request->preparedPlan = plan;
        request->result = result;
        request->success = true;
        return;
      }
    if (result.error.empty())
      {
        result.sqlstate = "HY000";
        result.error = "NativeLite could not prepare statement";
      }
    request->result = result;
  }

  bool reopenPreparedPlan(const std::shared_ptr<Session> &session,
                          const std::shared_ptr<NativeLitePreparedPlan> &plan,
                          QueryResult *result)
  {
    if (!plan || !plan->cli)
      return false;
    plan->cli->fetchRowsEpilogue(plan->sql.c_str());
    ContextCli *context = GetCliGlobals()->currContext();
    plan->cli.reset(new ExeCliInterface(context->exHeap(),
                                        SQLCHARSETCODE_UTF8, context));
    plan->cli->setNotExeUtilInternalQuery(TRUE);
    Lng32 rc = 0;
    rc = compilePlan(session, plan->cli.get(), plan->sql);
    if (rc < 0)
      {
        setDiagnosticsError(plan->cli.get(), rc, result);
        return false;
      }
    Lng32 inputCount = 0;
    Lng32 outputCount = 0;
    if (plan->cli->getNumEntries(inputCount, outputCount) < 0 ||
        static_cast<size_t>(inputCount) != plan->parameters.size())
      {
        result->sqlstate = "07001";
        result->error = "NativeLite prepared parameter descriptor changed";
        return false;
      }
    for (Lng32 entry = 1; entry <= inputCount; entry++)
      plan->cli->getAttributes(entry, TRUE,
                               plan->parameters[entry - 1].fsDatatype,
                               plan->parameters[entry - 1].length,
                               &plan->parameters[entry - 1].indOffset,
                               &plan->parameters[entry - 1].varOffset);
    return true;
  }

  QueryResult executePreparedStatement(
      const std::shared_ptr<Session> &session,
      const std::shared_ptr<NativeLitePreparedPlan> &plan,
      const std::vector<std::vector<std::string> > &rows,
      const std::vector<std::vector<bool> > &nullRows)
  {
    QueryResult result;
    if (!switchTo(session) || !plan || !plan->cli)
      {
        result.sqlstate = "08003";
        result.error = "NativeLite prepared statement is unavailable";
        return result;
      }
    applySessionEnvironment(session);
    if (session->failedTransaction)
      {
        result.sqlstate = "25P02";
        result.error = "current transaction is aborted; ROLLBACK required";
        updateTransactionStatus(session);
        return result;
      }
    if (rows.empty() || rows.size() != nullRows.size())
      {
        result.sqlstate = "07001";
        result.error = "NativeLite prepared batch is empty or malformed";
        return result;
      }
    if (rows.size() > 1 && firstWord(plan->sourceSql) != "INSERT")
      {
        result.sqlstate = "0A000";
        result.error = "NativeLite prepared batching requires INSERT statements";
        return result;
      }

    // M22E keeps the bound descriptor tuple attached to the root ATP. The
    // Lite scan TCB materializes dynamic primary keys from that tuple and
    // evaluates residual/range predicates against the same bound input, so
    // prepared SELECT/UPDATE/DELETE no longer require literal SQL expansion.

    int active = activeExecutorRequests_.fetch_add(1) + 1;
    int maximum = maximumExecutorRequests_.load();
    while (active > maximum &&
           !maximumExecutorRequests_.compare_exchange_weak(maximum, active))
      {}
    uint64_t affectedTotal = 0;
      for (size_t row = 0; row < rows.size(); row++)
      {
        if (plan->executed &&
            !reopenPreparedPlan(session, plan, &result))
          break;
        std::string bindingError;
        if (!bindPreparedParameters(plan, rows[row], nullRows[row],
                                    &bindingError))
          {
            result.sqlstate = "22023";
            result.error = bindingError;
            break;
          }
        Lng32 rebindRc = plan->cli->rebindInputBuffer();
        if (rebindRc < 0)
          {
            setDiagnosticsError(plan->cli.get(), rebindRc, &result);
            break;
          }
        Lng32 rc = plan->cli->exec();
        if (rc < 0)
          {
            setDiagnosticsError(plan->cli.get(), rc, &result);
            break;
          }
        result.columns = plan->description.columns;
        while (result.ok())
          {
            rc = plan->cli->fetch();
            if (rc == 100)
              break;
            if (rc < 0)
              {
                setDiagnosticsError(plan->cli.get(), rc, &result);
                break;
              }
            std::vector<Cell> cells;
            for (size_t column = 0; column < result.columns.size(); column++)
              cells.push_back(formatCell(plan->cli.get(), session->env,
                                          result.columns[column],
                                          static_cast<short>(column + 1)));
            result.rows.push_back(cells);
          }
        Int64 affected = 0;
        plan->cli->GetRowsAffected(&affected);
        if (affected > 0)
          affectedTotal += static_cast<uint64_t>(affected);
        // End the executor statement after each bound execution so lite
        // implicit transactions publish their writes. The next execution
        // reopens the same prepared plan and rebinds its descriptors.
        if (result.ok())
          {
            plan->cli->fetchRowsEpilogue(plan->sql.c_str());
          }
        if (!result.ok())
          break;
        plan->executed = true;
      }
    if (result.ok())
      result.commandTag = commandTag(plan->sourceSql, affectedTotal,
                                     result.rows.size());
    if (session->cancelRequested.load() && result.ok())
      {
        result.sqlstate = "57014";
        result.error = "canceling statement due to user request";
      }
    if (!result.ok() && currentTransactionInProgress())
      session->failedTransaction = true;
    activeExecutorRequests_.fetch_sub(1);
    updateTransactionStatus(session);
    return result;
  }

  bool automaticPlanCandidate(const std::string &sql,
                              bool describeOnly) const
  {
    if (describeOnly || countT4Parameters(sql) != 0)
      return false;
    const std::string verb = firstWord(sql);
    return verb == "SELECT" || verb == "INSERT" || verb == "UPDATE" ||
           verb == "DELETE" || verb == "MERGE" || verb == "UPSERT";
  }

  std::shared_ptr<NativeLitePreparedPlan> findAutomaticPlan(
      const std::shared_ptr<Session> &session, const std::string &sql)
  {
    std::map<std::string, std::shared_ptr<NativeLitePreparedPlan> >::iterator
        found = session->planCache.find(sql);
    if (found == session->planCache.end())
      return std::shared_ptr<NativeLitePreparedPlan>();

    session->planCacheLru.erase(std::remove(session->planCacheLru.begin(),
                                            session->planCacheLru.end(), sql),
                                session->planCacheLru.end());
    session->planCacheLru.push_back(sql);
    return found->second;
  }

  void rememberAutomaticPlan(const std::shared_ptr<Session> &session,
                             const std::string &sql,
                             const std::shared_ptr<NativeLitePreparedPlan> &plan)
  {
    static const size_t kAutomaticPlanCacheLimit = 64;
    session->planCache[sql] = plan;
    session->planCacheLru.erase(std::remove(session->planCacheLru.begin(),
                                            session->planCacheLru.end(), sql),
                                session->planCacheLru.end());
    session->planCacheLru.push_back(sql);
    while (session->planCacheLru.size() > kAutomaticPlanCacheLimit)
      {
        const std::string evicted = session->planCacheLru.front();
        session->planCacheLru.pop_front();
        session->planCache.erase(evicted);
      }
  }

  void clearAutomaticPlans(const std::shared_ptr<Session> &session)
  {
    session->planCache.clear();
    session->planCacheLru.clear();
  }

  QueryResult executeWithAutomaticPlan(
      const std::shared_ptr<Session> &session, const std::string &sql)
  {
    std::shared_ptr<NativeLitePreparedPlan> plan =
        findAutomaticPlan(session, sql);
    if (!plan)
      {
        EngineRequest prepareRequest(REQUEST_PREPARE);
        prepareRequest.session = session;
        prepareRequest.sql = sql;
        prepareStatement(&prepareRequest);
        if (!prepareRequest.success || !prepareRequest.preparedPlan)
          return prepareRequest.result;
        plan = prepareRequest.preparedPlan;
        rememberAutomaticPlan(session, sql, plan);
      }

    std::vector<std::vector<std::string> > rows(1);
    std::vector<std::vector<bool> > nullRows(1);
    QueryResult result = executePreparedStatement(session, plan, rows, nullRows);
    if (!result.ok())
      {
        // A schema/catalog change should normally clear the cache before this
        // path. Evict on any execution error as a conservative guard against
        // an external metadata change or a stale CLI plan.
        session->planCache.erase(sql);
        session->planCacheLru.erase(std::remove(session->planCacheLru.begin(),
                                                session->planCacheLru.end(),
                                                sql),
                                    session->planCacheLru.end());
      }
    return result;
  }

  QueryResult runStatement(const std::shared_ptr<Session> &session,
                           const std::string &sql, bool describeOnly)
  {
    std::unique_lock<std::mutex> catalogLock(catalogMutex_,
                                             std::defer_lock);
    std::string sqlWord = firstWord(sql);
    if (sqlWord == "CREATE" || sqlWord == "DROP" || sqlWord == "ALTER" ||
        sqlWord == "INITIALIZE")
      {
        clearAutomaticPlans(session);
        catalogLock.lock();
      }
    QueryResult result;
    if (!switchTo(session))
      {
        result.sqlstate = "08003";
        result.error = "NativeLite session is closed";
        return result;
      }
    if (!GetCliGlobals() || !GetCliGlobals()->currContext())
      {
        std::cerr << "NativeLite missing current CLI context after switch"
                  << " session=" << session.get()
                  << " handle=" << session->contextHandle << std::endl;
        result.sqlstate = "08003";
        result.error = "NativeLite session context is unavailable";
        return result;
      }
    applySessionEnvironment(session);

    if (session->cancelRequested.load())
      {
        result.sqlstate = "57014";
        result.error = "canceling statement due to user request";
        if (currentTransactionInProgress())
          session->failedTransaction = true;
        updateTransactionStatus(session);
        return result;
      }

    if (session->failedTransaction && !isRollback(sql))
      {
        result.sqlstate = "25P02";
        result.error = "current transaction is aborted; ROLLBACK required";
        updateTransactionStatus(session);
        return result;
      }

    bool builtinHandled = false;
    result = builtin(session, sql, describeOnly, &builtinHandled);
    if (builtinHandled)
      {
        if (session->cancelRequested.load() && result.ok())
          {
            result.sqlstate = "57014";
            result.error = "canceling statement due to user request";
          }
        if (!result.ok() && currentTransactionInProgress())
          session->failedTransaction = true;
        updateTransactionStatus(session);
        return result;
      }

    if (describeOnly && LiteSqlTable_isUtilityStatement(sql.c_str()))
      {
        result.commandTag = commandTag(sql, 0, 0);
        if (session->cancelRequested.load())
          {
            result.sqlstate = "57014";
            result.error = "canceling statement due to user request";
          }
        if (!result.ok() && currentTransactionInProgress())
          session->failedTransaction = true;
        updateTransactionStatus(session);
        return result;
      }

    if (!describeOnly)
      {
        clearCapture(session->env->get_logfile());
        short utilityRc = 0;
        bool utilityHandled = false;
        {
          std::lock_guard<std::mutex> utilityLock(utilityMutex_);
          utilityHandled = LiteSqlTable_process(
              sql.c_str(), session->env, &utilityRc);
        }
        if (utilityHandled)
          {
            std::string output = readCapture(session->env->get_logfile());
            if (utilityRc != 0)
              {
                result.error = output.empty() ?
                    "NativeLite utility statement failed" : output;
                result.sqlstate = sqlstateForUtilityError(result.error);
              }
            else
              result.commandTag = commandTag(sql, 0, 0);
            if (session->cancelRequested.load() && result.ok())
              {
                result.sqlstate = "57014";
                result.error = "canceling statement due to user request";
              }
            if (!result.ok() && currentTransactionInProgress())
              session->failedTransaction = true;
            updateTransactionStatus(session);
            return result;
          }
      }

    std::string executableSql = sql;
    std::string normalizedSql = upper(trim(sql));
    if (normalizedSql == "BEGIN" || normalizedSql == "START TRANSACTION")
      executableSql = "BEGIN WORK";

    ContextCli *context = GetCliGlobals()->currContext();
    ExeCliInterface cli(context->exHeap(), SQLCHARSETCODE_UTF8, context);
    cli.setNotExeUtilInternalQuery(TRUE);
    int active = activeExecutorRequests_.fetch_add(1) + 1;
    int maximum = maximumExecutorRequests_.load();
    while (active > maximum &&
           !maximumExecutorRequests_.compare_exchange_weak(maximum, active))
      {}
    const char *holdText = getenv("TRAF_LITE_EXECUTOR_HOLD_MS");
    if (holdText && holdText[0] &&
        (sql.find("M14E_OVERLAP") != std::string::npos ||
         sql.find("M21_OVERLAP") != std::string::npos))
      {
        char *end = NULL;
        long hold = strtol(holdText, &end, 10);
        if (end && *end == '\0' && hold > 0 && hold <= 5000)
          std::this_thread::sleep_for(std::chrono::milliseconds(hold));
      }

    // Transaction control is already represented by the current CLI
    // context. Avoid compiling a one-line SQL statement through the full
    // fetchRowsPrologue path for every JDBC BEGIN/COMMIT/ROLLBACK.
    const bool ddlCommitBoundary = context->ddlStmtsExecuted() != FALSE;
    const bool beginControl = !describeOnly &&
        (normalizedSql == "BEGIN" || normalizedSql == "BEGIN WORK" ||
         normalizedSql == "START TRANSACTION" ||
         normalizedSql == "START TRANSACTION WORK");
    const bool commitControl = !describeOnly && !ddlCommitBoundary &&
        (normalizedSql == "COMMIT" || normalizedSql == "COMMIT WORK");
    const bool rollbackControl = !describeOnly && !ddlCommitBoundary &&
        (normalizedSql == "ROLLBACK" || normalizedSql == "ROLLBACK WORK");
    Lng32 rc = 0;
    if ((beginControl || commitControl || rollbackControl) &&
        context->getTransaction() != NULL)
      {
        std::string transactionError;
        ExTransaction *transaction = context->getTransaction();
        bool transactionPassed = transaction != NULL;
        if (transactionPassed && beginControl)
          {
            transactionPassed = transaction->beginLiteTransaction(
                &transactionError);
            if (transactionPassed)
              {
                transaction->disableAutoCommit();
                transaction->implicitXn() = FALSE;
              }
          }
        else if (transactionPassed && commitControl)
          {
            context->closeAllCursors(ContextCli::CLOSE_ALL,
                                     ContextCli::CLOSE_CURR_XN);
            transactionPassed = transaction->commitLiteTransaction(
                &transactionError);
            transaction->enableAutoCommit();
          }
        else if (transactionPassed && rollbackControl)
          {
            context->closeAllCursors(
                ContextCli::CLOSE_ALL_INCLUDING_ANSI_PUBSUB_HOLDABLE_WHEN_CQD,
                ContextCli::CLOSE_CURR_XN);
            transactionPassed = transaction->rollbackLiteTransaction(
                &transactionError);
            transaction->enableAutoCommit();
          }
        if (!transactionPassed)
          {
            result.error = transactionError.empty()
                ? "NativeLite transaction context is unavailable"
                : transactionError;
            const size_t stateMarker = result.error.find("SQLSTATE ");
            if (stateMarker != std::string::npos &&
                result.error.size() >= stateMarker + 14)
              result.sqlstate = result.error.substr(stateMarker + 9, 5);
            else
              result.sqlstate = "XX000";
          }
        else
          result.commandTag = commandTag(sql, 0, 0);
        if (session->cancelRequested.load() && result.ok())
          {
            result.sqlstate = "57014";
            result.error = "canceling statement due to user request";
          }
        if (!result.ok() && currentTransactionInProgress())
          session->failedTransaction = true;
        activeExecutorRequests_.fetch_sub(1);
        updateTransactionStatus(session);
        return result;
      }

    if (automaticPlanCandidate(executableSql, describeOnly))
      {
        QueryResult cached = executeWithAutomaticPlan(session, executableSql);
        activeExecutorRequests_.fetch_sub(1);
        return cached;
      }

    rc = compilePlan(session, &cli, executableSql);
    if (rc < 0)
      {
        setDiagnosticsError(&cli, rc, &result);
        cli.fetchRowsEpilogue(sql.c_str());
      }
    else if (!loadColumns(&cli, session->env, &result.columns, &result))
      cli.fetchRowsEpilogue(sql.c_str());
    else if (describeOnly)
      {
        cli.fetchRowsEpilogue(sql.c_str());
        result.commandTag = commandTag(sql, 0, 0);
      }
    else
      {
        rc = cli.exec();
        if (rc < 0)
          setDiagnosticsError(&cli, rc, &result);
        else
          {
            while (result.ok())
              {
                rc = cli.fetch();
                if (rc == 100)
                  break;
                if (rc < 0)
                  {
                    setDiagnosticsError(&cli, rc, &result);
                    break;
                  }
                std::vector<Cell> row;
                for (size_t index = 0; index < result.columns.size(); index++)
                  row.push_back(formatCell(&cli, session->env,
                                           result.columns[index],
                                           static_cast<short>(index + 1)));
                result.rows.push_back(row);
              }
          }
        Int64 affected = 0;
        cli.GetRowsAffected(&affected);
        if (result.ok())
          result.commandTag = commandTag(sql, affected, result.rows.size());
        cli.fetchRowsEpilogue(sql.c_str());
      }

    if (session->cancelRequested.load() && result.ok())
      {
        result.sqlstate = "57014";
        result.error = "canceling statement due to user request";
      }
    if (!result.ok() && currentTransactionInProgress())
      session->failedTransaction = true;
    activeExecutorRequests_.fetch_sub(1);
    updateTransactionStatus(session);
    return result;
  }

  std::thread worker_;
  std::mutex queueMutex_;
  std::condition_variable queueCondition_;
  std::condition_variable readyCondition_;
  std::atomic<bool> initialized_;
  bool initializationFailed_;
  std::string initializationError_;
  std::atomic<bool> stopping_;
  SQLCTX_HANDLE defaultContext_;
  SqlciEnv *bootstrapEnv_;
  std::mutex catalogMutex_;
  std::mutex utilityMutex_;
  std::mutex contextLifecycleMutex_;
  std::atomic<int> activeExecutorRequests_;
  std::atomic<int> maximumExecutorRequests_;
  std::atomic<int> activeCompilerRequests_;
  std::atomic<int> maximumCompilerRequests_;
  LiteRocksDBStore storeLease_;
};

std::string quoteLiteral(const std::string &value)
{
  std::string result("'");
  for (size_t i = 0; i < value.size(); i++)
    {
      if (value[i] == '\'')
        result += "''";
      else
        result.push_back(value[i]);
    }
  result.push_back('\'');
  return result;
}

bool currentTransactionInProgress()
{
  CliGlobals *globals = GetCliGlobals();
  ContextCli *context = globals ? globals->currContext() : NULL;
  if (!context)
    return false;
  ExTransaction *transaction = context->getTransaction();
  if (!transaction)
    return false;
  return transaction->xnInProgress();
}

size_t countT4Parameters(const std::string &sql)
{
  size_t count = 0;
  bool singleQuoted = false;
  bool doubleQuoted = false;
  for (size_t i = 0; i < sql.size(); i++)
    {
      if (sql[i] == '\'' && !doubleQuoted)
        {
          if (singleQuoted && i + 1 < sql.size() && sql[i + 1] == '\'')
            i++;
          else
            singleQuoted = !singleQuoted;
        }
      else if (sql[i] == '"' && !singleQuoted)
        doubleQuoted = !doubleQuoted;
      else if (sql[i] == '?' && !singleQuoted && !doubleQuoted)
        count++;
    }
  return count;
}

bool rewriteT4ParametersAsVarchar(const std::string &sql,
                                  std::string *output)
{
  output->clear();
  bool singleQuoted = false;
  bool doubleQuoted = false;
  for (size_t i = 0; i < sql.size(); i++)
    {
      if (sql[i] == '\'' && !doubleQuoted)
        {
          output->push_back(sql[i]);
          if (singleQuoted && i + 1 < sql.size() && sql[i + 1] == '\'')
            output->push_back(sql[++i]);
          else
            singleQuoted = !singleQuoted;
        }
      else if (sql[i] == '"' && !singleQuoted)
        {
          doubleQuoted = !doubleQuoted;
          output->push_back(sql[i]);
        }
      else if (sql[i] == '?' && !singleQuoted && !doubleQuoted)
        *output += "CAST(? AS VARCHAR(256))";
      else
        output->push_back(sql[i]);
    }
  return !singleQuoted && !doubleQuoted;
}

bool splitT4Statements(const std::string &sql,
                       std::vector<std::string> *statements)
{
  statements->clear();
  bool singleQuoted = false;
  bool doubleQuoted = false;
  size_t begin = 0;
  for (size_t i = 0; i < sql.size(); i++)
    {
      if (sql[i] == '\'' && !doubleQuoted)
        {
          if (singleQuoted && i + 1 < sql.size() && sql[i + 1] == '\'')
            i++;
          else
            singleQuoted = !singleQuoted;
        }
      else if (sql[i] == '"' && !singleQuoted)
        doubleQuoted = !doubleQuoted;
      else if (sql[i] == ';' && !singleQuoted && !doubleQuoted)
        {
          std::string statement = trim(sql.substr(begin, i - begin));
          if (!statement.empty())
            statements->push_back(statement);
          begin = i + 1;
        }
    }
  std::string tail = trim(sql.substr(begin));
  if (!tail.empty())
    statements->push_back(tail);
  return !singleQuoted && !doubleQuoted;
}

bool substituteT4Parameters(const std::string &sql,
                            const std::vector<std::string> &values,
                            const std::vector<bool> &nulls,
                            std::string *output)
{
  output->clear();
  bool singleQuoted = false;
  bool doubleQuoted = false;
  size_t parameter = 0;
  for (size_t i = 0; i < sql.size(); i++)
    {
      if (sql[i] == '\'' && !doubleQuoted)
        {
          output->push_back(sql[i]);
          if (singleQuoted && i + 1 < sql.size() && sql[i + 1] == '\'')
            output->push_back(sql[++i]);
          else
            singleQuoted = !singleQuoted;
        }
      else if (sql[i] == '"' && !singleQuoted)
        {
          doubleQuoted = !doubleQuoted;
          output->push_back(sql[i]);
        }
      else if (sql[i] == '?' && !singleQuoted && !doubleQuoted)
        {
          if (parameter >= values.size() || parameter >= nulls.size())
            return false;
          *output += nulls[parameter]
              ? "NULL" : quoteLiteral(values[parameter]);
          parameter++;
        }
      else
        output->push_back(sql[i]);
    }
  return parameter == values.size();
}

bool compileT4ParameterTemplate(const std::string &sql,
                                std::vector<std::string> *parts)
{
  parts->clear();
  bool singleQuoted = false;
  bool doubleQuoted = false;
  size_t begin = 0;
  for (size_t i = 0; i < sql.size(); i++)
    {
      if (sql[i] == '\'' && !doubleQuoted)
        {
          if (singleQuoted && i + 1 < sql.size() && sql[i + 1] == '\'')
            i++;
          else
            singleQuoted = !singleQuoted;
        }
      else if (sql[i] == '"' && !singleQuoted)
        doubleQuoted = !doubleQuoted;
      else if (sql[i] == '?' && !singleQuoted && !doubleQuoted)
        {
          parts->push_back(sql.substr(begin, i - begin));
          begin = i + 1;
        }
    }
  parts->push_back(sql.substr(begin));
  return !singleQuoted && !doubleQuoted;
}

bool substituteT4ParameterTemplate(
    const std::vector<std::string> &parts,
    const std::vector<std::string> &values,
    const std::vector<bool> &nulls,
    std::string *output)
{
  if (parts.empty() || parts.size() != values.size() + 1 ||
      values.size() != nulls.size())
    return false;
  output->clear();
  for (size_t i = 0; i < values.size(); i++)
    {
      *output += parts[i];
      *output += nulls[i] ? "NULL" : quoteLiteral(values[i]);
    }
  *output += parts.back();
  return true;
}

bool decodeT4ParameterRows(
    const std::string &data, size_t parameterCount, uint32_t rowCount,
    std::vector<std::vector<std::string> > *rows,
    std::vector<std::vector<bool> > *nullRows)
{
  const size_t descriptorStride = 260;
  const size_t valueStride = 258;
  if (rowCount == 0)
    rowCount = 1;
  if (parameterCount != 0 &&
      (parameterCount > std::numeric_limits<size_t>::max() /
           descriptorStride ||
       rowCount > std::numeric_limits<size_t>::max() /
           (parameterCount * descriptorStride)))
    return false;
  const size_t required = parameterCount * descriptorStride * rowCount;
  if (data.size() < required)
    return false;
  rows->assign(rowCount, std::vector<std::string>(parameterCount));
  nullRows->assign(rowCount, std::vector<bool>(parameterCount, false));
  for (size_t column = 0; column < parameterCount; column++)
    for (size_t row = 0; row < rowCount; row++)
      {
        const size_t nullOffset = column * descriptorStride * rowCount +
            row * sizeof(uint16_t);
        const size_t valueOffset = (column * descriptorStride + 2) * rowCount +
            row * valueStride;
        if (nullOffset + 2 > data.size() || valueOffset + 2 > data.size())
          return false;
        (*nullRows)[row][column] =
            getU16(data.data() + nullOffset) == 0xffffU;
        if (!(*nullRows)[row][column])
          {
            const uint16_t length = getU16(data.data() + valueOffset);
            if (length > 256 || valueOffset + 2 + length > data.size())
              return false;
            (*rows)[row][column].assign(
                data.data() + valueOffset + 2, length);
          }
      }
  return true;
}

bool buildT4BatchSql(
    const std::string &sql,
    const std::vector<std::vector<std::string> > &rows,
    const std::vector<std::vector<bool> > &nullRows,
    std::string *output,
    const std::vector<std::string> *parameterParts = NULL,
    const std::string *batchPrefix = NULL,
    const std::vector<std::string> *batchParameterParts = NULL)
{
  if (rows.empty() || rows.size() != nullRows.size())
    return false;
  if (rows.size() == 1)
    return parameterParts
        ? substituteT4ParameterTemplate(*parameterParts, rows[0],
                                         nullRows[0], output)
        : substituteT4Parameters(sql, rows[0], nullRows[0], output);
  if (firstWord(sql) != "INSERT")
    return false;
  const std::string upperSql = upper(sql);
  const size_t values = upperSql.find("VALUES");
  if (values == std::string::npos)
    return false;
  const size_t tupleOffset = values + 6;
  const std::string tupleTemplate = sql.substr(tupleOffset);
  *output = batchPrefix ? *batchPrefix : sql.substr(0, tupleOffset);
  for (size_t row = 0; row < rows.size(); row++)
    {
      std::string tuple;
      const bool substituted = batchParameterParts
          ? substituteT4ParameterTemplate(*batchParameterParts, rows[row],
                                           nullRows[row], &tuple)
          : substituteT4Parameters(tupleTemplate, rows[row], nullRows[row],
                                   &tuple);
      if (!substituted)
        return false;
      if (row != 0)
        output->push_back(',');
      *output += tuple;
    }
  return true;
}

bool t4PatternMatches(const std::string &value, const std::string &pattern,
                      size_t valueOffset = 0, size_t patternOffset = 0)
{
  while (patternOffset < pattern.size())
    {
      if (pattern[patternOffset] == '%')
        {
          while (patternOffset < pattern.size() &&
                 pattern[patternOffset] == '%')
            patternOffset++;
          if (patternOffset == pattern.size())
            return true;
          for (size_t i = valueOffset; i <= value.size(); i++)
            if (t4PatternMatches(value, pattern, i, patternOffset))
              return true;
          return false;
        }
      if (valueOffset >= value.size())
        return false;
      if (pattern[patternOffset] != '_' &&
          pattern[patternOffset] != value[valueOffset])
        return false;
      valueOffset++;
      patternOffset++;
    }
  return valueOffset == value.size();
}

bool t4Matches(const std::string &value, const std::string &pattern)
{
  return pattern.empty() || pattern == "%" ||
         t4PatternMatches(value, pattern);
}

std::string liteJdbcType(const std::string &type)
{
  std::string normalized = upper(type);
  if (startsWith(normalized, "SMALLINT")) return "5";
  if (startsWith(normalized, "TINYINT")) return "-6";
  if (startsWith(normalized, "LARGEINT") ||
      startsWith(normalized, "BIGINT")) return "-5";
  if (startsWith(normalized, "INT")) return "4";
  if (startsWith(normalized, "VARCHAR")) return "12";
  if (startsWith(normalized, "CHAR")) return "1";
  if (startsWith(normalized, "DECIMAL")) return "3";
  if (startsWith(normalized, "NUMERIC")) return "2";
  if (startsWith(normalized, "DATE")) return "91";
  if (startsWith(normalized, "TIME") &&
      !startsWith(normalized, "TIMESTAMP")) return "92";
  if (startsWith(normalized, "TIMESTAMP")) return "93";
  if (startsWith(normalized, "BOOLEAN")) return "16";
  if (startsWith(normalized, "REAL")) return "7";
  if (startsWith(normalized, "FLOAT")) return "6";
  if (startsWith(normalized, "DOUBLE")) return "8";
  if (startsWith(normalized, "VARBINARY")) return "-3";
  if (startsWith(normalized, "BINARY")) return "-2";
  return "1111";
}

std::string liteTypeSize(const std::string &type)
{
  size_t open = type.find('(');
  size_t close = type.find(')', open == std::string::npos ? 0 : open + 1);
  if (open != std::string::npos && close != std::string::npos)
    return type.substr(open + 1, close - open - 1);
  std::string jdbc = liteJdbcType(type);
  if (jdbc == "-6") return "3";
  if (jdbc == "5") return "5";
  if (jdbc == "4") return "10";
  if (jdbc == "-5") return "19";
  if (jdbc == "16") return "1";
  if (jdbc == "91") return "10";
  if (jdbc == "92") return "8";
  if (jdbc == "93") return "26";
  return "0";
}

class NativeLiteServer
{
public:
  NativeLiteServer(const std::string &host, int port,
                   const std::string &unixSocket, unsigned int maxSessions)
      : host_(host), port_(port), unixSocket_(unixSocket), listenFd_(-1),
        ownsUnixSocket_(false), unixSocketDevice_(0), unixSocketInode_(0),
        maxSessions_(maxSessions), activeSessions_(0),
        // Dialogue ids are local to this server process.  Keep them within
        // the signed 32-bit range expected by the T4 JDBC driver; using the
        // process id as a prefix overflows once WSL process ids grow past
        // INT32_MAX / 1000.
        nextDialogueId_(0)
  {
  }

  ~NativeLiteServer()
  {
    stopClients();
    if (listenFd_ >= 0)
      close(listenFd_);
    removeOwnedUnixSocket();
  }

  bool start(std::string *error)
  {
    if (!engine_.waitUntilReady(error))
      return false;
    return unixSocket_.empty() ? startTcp(error) : startUnix(error);
  }

  int run()
  {
    std::cout << "NativeLite server ready on "
              << (unixSocket_.empty() ? host_ + ":" + portString()
                                      : unixSocket_)
              << " workers=" << maxSessions_
              << std::endl;
    while (!gStopping.load())
      {
        reapClientThreads();
        int fd = accept(listenFd_, NULL, NULL);
        if (fd < 0)
          {
            if (errno == EINTR)
              continue;
            if (gStopping.load() || errno == EBADF || errno == EINVAL)
              break;
            std::cerr << "accept failed: " << strerror(errno) << std::endl;
            continue;
          }
        std::shared_ptr<std::atomic<bool> > done(
            new std::atomic<bool>(false));
        {
          std::lock_guard<std::mutex> lock(clientMutex_);
          clientFds_.push_back(fd);
        }
        clientThreadDone_.push_back(done);
        clientThreads_.push_back(std::thread(
            &NativeLiteServer::serveClientAndMark, this, fd, done));
      }
    if (gListenFd.load() < 0)
      listenFd_ = -1;
    stopClients();
    engine_.stop();
    return 0;
  }

  void cancel(int32_t dialogueId)
  {
    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      std::map<int32_t, std::weak_ptr<Session> >::iterator found =
          sessions_.find(dialogueId);
      if (found != sessions_.end())
        session = found->second.lock();
    }
    if (session)
      engine_.cancel(session);
  }

private:
  bool startTcp(std::string *error)
  {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
      return socketError("create TCP socket", error);
    int enabled = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1)
      {
        *error = "--listen must be a numeric loopback IPv4 address";
        return false;
      }
    uint32_t hostAddress = ntohl(address.sin_addr.s_addr);
    if ((hostAddress >> 24) != 127)
      {
        *error = "NativeLite trusted transport is restricted to loopback";
        return false;
      }
    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0)
      return socketError("bind TCP socket", error);
    if (listen(listenFd_, 64) != 0)
      return socketError("listen on TCP socket", error);
    gListenFd.store(listenFd_);
    return true;
  }

  bool startUnix(std::string *error)
  {
    if (unixSocket_.size() >= sizeof(((struct sockaddr_un *)0)->sun_path))
      {
        *error = "Unix socket path is too long";
        return false;
      }
    sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, unixSocket_.c_str(), sizeof(address.sun_path) - 1);

    struct stat existing;
    if (lstat(unixSocket_.c_str(), &existing) == 0)
      {
        if (!S_ISSOCK(existing.st_mode))
          {
            *error = "refusing to replace a non-socket Unix path";
            return false;
          }
        if (existing.st_uid != geteuid())
          {
            *error = "refusing to replace a Unix socket owned by another user";
            return false;
          }
        int probe = socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe < 0)
          return socketError("create Unix socket probe", error);
        int connectRc = connect(probe, reinterpret_cast<sockaddr *>(&address),
                                sizeof(address));
        int connectError = errno;
        close(probe);
        if (connectRc == 0)
          {
            *error = "Unix socket path is already accepting connections";
            return false;
          }
        if (connectError != ECONNREFUSED)
          {
            *error = std::string("inspect existing Unix socket: ") +
                     strerror(connectError);
            return false;
          }
        struct stat stale;
        if (lstat(unixSocket_.c_str(), &stale) != 0 ||
            !S_ISSOCK(stale.st_mode) || stale.st_dev != existing.st_dev ||
            stale.st_ino != existing.st_ino)
          {
            *error = "Unix socket path changed while checking stale state";
            return false;
          }
        if (unlink(unixSocket_.c_str()) != 0)
          return socketError("remove stale Unix socket", error);
      }
    else if (errno != ENOENT)
      return socketError("inspect Unix socket path", error);

    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ < 0)
      return socketError("create Unix socket", error);
    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0)
      return socketError("bind Unix socket", error);
    struct stat bound;
    if (lstat(unixSocket_.c_str(), &bound) != 0 || !S_ISSOCK(bound.st_mode))
      {
        *error = "unable to verify the bound Unix socket path";
        close(listenFd_);
        listenFd_ = -1;
        return false;
      }
    ownsUnixSocket_ = true;
    unixSocketDevice_ = bound.st_dev;
    unixSocketInode_ = bound.st_ino;
    if (chmod(unixSocket_.c_str(), 0600) != 0)
      return socketError("set Unix socket permissions", error);
    if (listen(listenFd_, 64) != 0)
      return socketError("listen on Unix socket", error);
    gListenFd.store(listenFd_);
    return true;
  }

  bool socketError(const char *operation, std::string *error)
  {
    *error = std::string(operation) + ": " + strerror(errno);
    if (listenFd_ >= 0)
      {
        close(listenFd_);
        listenFd_ = -1;
      }
    return false;
  }

  std::string portString() const
  {
    std::ostringstream value;
    value << port_;
    return value.str();
  }

  void stopClients()
  {
    std::vector<int> fds;
    {
      std::lock_guard<std::mutex> lock(clientMutex_);
      fds = clientFds_;
    }
    for (size_t i = 0; i < fds.size(); i++)
      shutdown(fds[i], SHUT_RDWR);
    for (size_t i = 0; i < clientThreads_.size(); i++)
      if (clientThreads_[i].joinable())
        clientThreads_[i].join();
    clientThreads_.clear();
    clientThreadDone_.clear();
    clientFds_.clear();
  }

  void reapClientThreads()
  {
    for (size_t i = 0; i < clientThreads_.size();)
      {
        if (!clientThreadDone_[i]->load())
          {
            i++;
            continue;
          }
        if (clientThreads_[i].joinable())
          clientThreads_[i].join();
        clientThreads_.erase(clientThreads_.begin() + i);
        clientThreadDone_.erase(clientThreadDone_.begin() + i);
      }
  }

  void removeOwnedUnixSocket()
  {
    if (!ownsUnixSocket_)
      return;
    struct stat current;
    if (lstat(unixSocket_.c_str(), &current) == 0 &&
        S_ISSOCK(current.st_mode) &&
        current.st_dev == unixSocketDevice_ &&
        current.st_ino == unixSocketInode_)
      unlink(unixSocket_.c_str());
    ownsUnixSocket_ = false;
  }

  bool readT4Request(int fd, T4Header *header, std::string *body)
  {
    char headerBytes[40];
    if (!readExact(fd, headerBytes, sizeof(headerBytes)))
      return false;
    if (!decodeT4Header(headerBytes, header))
      return false;
    body->assign(header->totalLength, '\0');
    if (!body->empty() && !readExact(fd, &(*body)[0], body->size()))
      return false;
    return true;
  }

  void serveClient(int fd)
  {
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    T4Header request;
    std::string body;
    if (!readT4Request(fd, &request, &body))
      return closeClientFd(fd);

    if (request.operation == kT4GetObjectRef)
      {
        sendT4GetObjectRef(fd, request);
        return closeClientFd(fd);
      }
    if (request.operation == kT4Cancel)
      {
        handleT4Cancel(fd, request, body);
        return closeClientFd(fd);
      }
    if (request.operation != kT4SqlConnect)
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return closeClientFd(fd);
      }

    std::string user;
    if (!readT4ConnectUser(body, &user))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return closeClientFd(fd);
      }

    std::shared_ptr<Session> session(new Session());
    session->user = upper(user.empty() ? "DB__ROOT" : user);
    session->database = "TRAFODION";
    session->backendPid = request.dialogueId;
    if (session->backendPid <= 0)
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return closeClientFd(fd);
      }
    if (!tryAcquireSessionSlot())
      {
        QueryResult capacity;
        capacity.sqlstate = "53300";
        capacity.error = "NativeLite session capacity exhausted (limit=" +
                         std::to_string(maxSessions_) + ")";
        std::string errorBody;
        appendT4ConnectError(&errorBody, capacity);
        sendT4Response(fd, request, errorBody);
        return closeClientFd(fd);
      }
    session->slotAcquired = true;
    std::string createError;
    if (!engine_.create(session, &createError))
      {
        QueryResult failure;
        failure.sqlstate = "08004";
        failure.error = createError.empty() ?
            "NativeLite could not create a session context" : createError;
        std::string errorBody;
        appendT4ConnectError(&errorBody, failure);
        sendT4Response(fd, request, errorBody);
        releaseSessionSlot(session);
        return closeClientFd(fd);
      }
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      sessions_[session->backendPid] = session;
    }

    if (!sendT4Connect(fd, request))
      return finishSession(fd, session);
    t4ProtocolLoop(fd, session);
    finishSession(fd, session);
  }

  void sendT4GetObjectRef(int fd, const T4Header &request)
  {
    int32_t dialogueId = ++nextDialogueId_;
    std::string reply;
    appendU32(&reply, 0); // exception number
    appendU32(&reply, 0); // exception detail
    appendT4String(&reply, std::string());
    appendU32(&reply, static_cast<uint32_t>(dialogueId));
    appendT4String(&reply, "TDM_Default_DataSource");
    appendU32(&reply, 0); // user SID byte string
    appendU32(&reply, 2); // version count
    appendT4Version(&reply, 2, 3, 0, 1);
    appendT4Version(&reply, 4, 3, 5, 1);
    appendU32(&reply, 15); // ISO8859-1 mapping
    appendT4String(&reply, "localhost");
    appendU32(&reply, 0); // node id
    appendU32(&reply, static_cast<uint32_t>(getpid()));
    appendT4String(&reply, "$NATIVELITE");
    appendT4String(&reply, host_);
    appendU32(&reply, static_cast<uint32_t>(port_));
    sendT4Response(fd, request, reply);
  }

  void handleT4Cancel(int fd, const T4Header &request,
                      const std::string &body)
  {
    T4Reader reader(body);
    uint32_t dialogueId = 0;
    uint32_t serverType = 0;
    uint32_t stopType = 0;
    std::string serverObject;
    std::string reply;
    if (!reader.readU32(&dialogueId) || !reader.readU32(&serverType) ||
        !reader.readString(&serverObject) || !reader.readU32(&stopType) ||
        dialogueId == 0)
      {
        appendU32(&reply, 1); // AS parameter error
        appendU32(&reply, 0);
        appendT4String(&reply, "invalid NativeLite cancel request");
      }
    else
      {
        cancel(static_cast<int32_t>(dialogueId));
        appendU32(&reply, 0);
        appendU32(&reply, 0);
      }
    sendT4Response(fd, request, reply);
  }

  bool readT4ConnectUser(const std::string &body, std::string *user)
  {
    T4Reader reader(body);
    uint32_t descriptorType = 0;
    std::string userSid;
    std::string domain;
    std::string password;
    return reader.readU32(&descriptorType) &&
           reader.readString(&userSid) && reader.readString(&domain) &&
           reader.readString(user) && reader.readString(&password);
  }

  bool sendT4Connect(int fd, const T4Header &request)
  {
    std::string reply;
    appendU32(&reply, 0); // exception number
    appendU32(&reply, 0); // exception detail
    appendU32(&reply, 2); // version count
    appendT4Version(&reply, 4, 3, 5, 1);
    appendT4Version(&reply, 3, 1, 1, 1);
    appendU16(&reply, 0); // node id
    appendU32(&reply, static_cast<uint32_t>(getpid()));
    appendT4String(&reply, "localhost");
    appendT4String(&reply, "TRAFODION");
    appendT4String(&reply, "SEABASE");
    appendU32(&reply, 1); // enforce ISO8859-1
    appendU32(&reply, 0);
    return sendT4Response(fd, request, reply);
  }

  void t4ProtocolLoop(int fd, const std::shared_ptr<Session> &session)
  {
    std::map<std::string, T4StatementState> statements;
    while (!gStopping.load())
      {
        T4Header request;
        std::string body;
        if (!readT4Request(fd, &request, &body))
          return;
        if (request.operation == kT4SqlDisconnect)
          {
            std::string reply;
            appendU32(&reply, 0);
            appendU32(&reply, 0);
            sendT4Response(fd, request, reply);
            return;
          }
        if (request.operation == kT4SetConnectionOption)
          {
            handleT4ConnectionOption(session, body);
            std::string reply;
            appendU32(&reply, 0);
            appendU32(&reply, 0);
            appendU32(&reply, 0); // warning/error descriptor count
            if (!sendT4Response(fd, request, reply))
              return;
            continue;
          }
        if (request.operation == kT4EndTransaction)
          {
            handleT4EndTransaction(fd, request, body, session);
            continue;
          }
        if (request.operation == kT4ExecuteDirect)
          {
            handleT4ExecuteDirect(fd, request, body, session, &statements);
            continue;
          }
        if (request.operation == kT4Prepare)
          {
            handleT4Prepare(fd, request, body, session, &statements);
            continue;
          }
        if (request.operation == kT4Execute ||
            request.operation == kT4Execute2)
          {
            handleT4Execute(fd, request, body, session, &statements);
            continue;
          }
        if (request.operation == kT4Fetch)
          {
            handleT4Fetch(fd, request, body, &statements);
            continue;
          }
        if (request.operation == kT4FreeStatement)
          {
            handleT4FreeStatement(fd, request, body, &statements);
            continue;
          }
        if (request.operation == kT4GetCatalogs)
          {
            handleT4GetCatalogs(fd, request, body, &statements);
            continue;
          }
        sendT4Response(fd, request, std::string(), 1, 0);
      }
  }

  void handleT4ConnectionOption(const std::shared_ptr<Session> &session,
                                const std::string &body)
  {
    T4Reader reader(body);
    uint32_t dialogue = 0;
    uint16_t option = 0;
    uint32_t value = 0;
    std::string text;
    if (!reader.readU32(&dialogue) || !reader.readU16(&option) ||
        !reader.readU32(&value) || !reader.readString(&text))
      return;
    if (option != 102)
      return;
    bool enabled = value != 0;
    if (enabled && !session->autoCommit &&
        session->transactionStatus.load() != 'I')
      engine_.execute(session, "COMMIT");
    session->autoCommit = enabled;
  }

  void appendT4Error(std::string *reply, const QueryResult &result)
  {
    appendU32(reply, static_cast<uint32_t>(-1));
    std::string error;
    appendU32(&error, 0); // row id
    appendU32(&error, static_cast<uint32_t>(-1));
    appendT4String(&error, result.error);
    std::string state = result.sqlstate.empty() ? "HY000" : result.sqlstate;
    state.resize(5, '0');
    error.append(state.data(), 5);
    error.push_back('\0');
    appendU32(reply, static_cast<uint32_t>(error.size() + 4));
    appendU32(reply, 1);
    reply->append(error);
  }

  void appendT4ConnectError(std::string *reply, const QueryResult &result)
  {
    // SQLCONNECT uses InitializeDialogueReply's SQLError list rather than
    // the execute-reply diagnostic envelope used after a session exists.
    appendU32(reply, 3); // odbc_SQLSvc_InitializeDialogue_SQLError_exn_
    appendU32(reply, 0); // exception detail
    appendU32(reply, 1); // one ERROR_DESC_def
    appendT4ErrorDescriptor(reply, result);
  }

  void appendT4ErrorDescriptor(std::string *reply, const QueryResult &result)
  {
    appendU32(reply, 0); // row id
    appendU32(reply, 0); // diagnostic id
    appendU32(reply, static_cast<uint32_t>(-1)); // SQL code
    std::string state = result.sqlstate.empty() ? "HY000" : result.sqlstate;
    state.resize(5, '0');
    reply->append(state.data(), 5);
    reply->push_back('\0');
    appendT4String(reply, result.error);
    appendU32(reply, 0); // operation abort id
    appendU32(reply, 0); // error code type
    for (size_t i = 0; i < 7; i++)
      appendT4String(reply, std::string());
  }

  uint32_t rowsAffectedForT4(const QueryResult &result) const
  {
    std::istringstream words(result.commandTag);
    std::string word;
    uint64_t last = 0;
    while (words >> word)
      {
        char *end = NULL;
        unsigned long long parsed = strtoull(word.c_str(), &end, 10);
        if (end && *end == '\0')
          last = parsed;
      }
    return static_cast<uint32_t>(std::min<uint64_t>(last, 0xffffffffU));
  }

  int32_t queryTypeForT4(const QueryResult &result) const
  {
    if (!result.columns.empty())
      return 2; // SQL_SELECT_NON_UNIQUE
    std::string word = firstWord(result.commandTag);
    if (word == "INSERT" || word == "UPSERT")
      return 4;
    if (word == "UPDATE" || word == "MERGE")
      return 6;
    if (word == "DELETE")
      return 8;
    if (word == "SET" || word == "CONTROL")
      return 9;
    return -1;
  }

  void appendT4ExecuteReply(std::string *reply, const QueryResult &result)
  {
    if (!result.ok())
      appendT4Error(reply, result);
    else
      {
        appendU32(reply, 0);
        appendU32(reply, 0);
      }

    if (result.ok() && !result.columns.empty())
      {
        appendU32(reply, 1); // a nonzero descriptor section length
        appendU32(reply, t4RowLength(result));
        appendU32(reply, static_cast<uint32_t>(result.columns.size()));
        uint32_t offset = 0;
        for (size_t i = 0; i < result.columns.size(); i++)
          {
            appendT4Descriptor(reply, result.columns[i], offset);
            offset += 2 + t4ColumnCapacity(result.columns[i]);
          }
      }
    else
      appendU32(reply, 0);
    appendU32(reply, result.ok() ? rowsAffectedForT4(result) : 0);
    appendU32(reply, static_cast<uint32_t>(queryTypeForT4(result)));
    appendU32(reply, 0); // estimated cost / high rows-affected bits
    appendU32(reply, 0); // execute output values
    appendU32(reply, 0); // result set count
    appendT4String(reply, std::string()); // proxy syntax
  }

  void handleT4ExecuteDirect(
      int fd, const T4Header &request, const std::string &body,
      const std::shared_ptr<Session> &session,
      std::map<std::string, T4StatementState> *statements)
  {
    T4Reader reader(body);
    std::string sql;
    std::string cursor;
    std::string label;
    std::string explain;
    if (!reader.skip(32) || !reader.readStringWithCharset(&sql) ||
        !reader.readStringWithCharset(&cursor) ||
        !reader.readStringWithCharset(&label) ||
        !reader.readString(&explain))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    if (!session->autoCommit && session->transactionStatus.load() == 'I' &&
        !LiteSqlTable_isUtilityStatement(sql.c_str()))
      {
        QueryResult begin = engine_.execute(session, "BEGIN");
        if (!begin.ok() && begin.error.find("already active") ==
                std::string::npos)
          {
            std::string reply;
            appendT4ExecuteReply(&reply, begin);
            sendT4Response(fd, request, reply);
            return;
          }
      }
    T4StatementState state;
    std::vector<std::string> batch;
    if (!splitT4Statements(sql, &batch))
      {
        state.result.sqlstate = "42601";
        state.result.error = "NativeLite batch has unterminated quoted text";
      }
    else
      state.result = batch.size() > 1
          ? engine_.executeBatch(session, batch)
          : engine_.execute(session, sql);
    (*statements)[label] = state;
    std::string reply;
    appendT4ExecuteReply(&reply, state.result);
    sendT4Response(fd, request, reply);
  }

  void appendT4PrepareDescriptors(std::string *reply,
                                  const QueryResult &description)
  {
    if (description.columns.empty())
      {
        appendU32(reply, 0);
        return;
      }
    appendU32(reply, 1);
    appendU32(reply, t4RowLength(description));
    appendU32(reply, static_cast<uint32_t>(description.columns.size()));
    uint32_t offset = 0;
    for (size_t i = 0; i < description.columns.size(); i++)
      {
        appendT4Descriptor(reply, description.columns[i], offset);
        offset += 2 + t4ColumnCapacity(description.columns[i]);
      }
  }

  void handleT4Prepare(
      int fd, const T4Header &request, const std::string &body,
      const std::shared_ptr<Session> &session,
      std::map<std::string, T4StatementState> *statements)
  {
    T4Reader reader(body);
    uint16_t statementType = 0;
    uint32_t sqlStatementType = 0;
    std::string label;
    std::string cursor;
    std::string module;
    std::string sql;
    if (!reader.skip(12) || !reader.readU16(&statementType) ||
        !reader.readU32(&sqlStatementType) ||
        !reader.readStringWithCharset(&label) ||
        !reader.readStringWithCharset(&cursor) ||
        !reader.readStringWithCharset(&module))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    if (!module.empty())
      {
        uint64_t timestamp = 0;
        if (!reader.readU64(&timestamp))
          {
            sendT4Response(fd, request, std::string(), 1, 0);
            return;
          }
      }
    if (!reader.readStringWithCharset(&sql))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }

    T4StatementState state;
    state.sql = sql;
    state.parameterCount = countT4Parameters(sql);
    if (!compileT4ParameterTemplate(sql, &state.parameterParts))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    if (firstWord(sql) == "INSERT")
      {
        const std::string upperSql = upper(sql);
        const size_t valuesOffset = upperSql.find("VALUES");
        if (valuesOffset != std::string::npos)
          {
            const size_t tupleOffset = valuesOffset + 6;
            state.batchPrefix = sql.substr(0, tupleOffset);
            if (!compileT4ParameterTemplate(
                    sql.substr(tupleOffset), &state.batchParameterParts))
              {
                sendT4Response(fd, request, std::string(), 1, 0);
                return;
              }
          }
      }
    std::vector<std::string> multiStatements;
    const bool isMultiStatement = splitT4Statements(sql, &multiStatements) &&
        multiStatements.size() > 1;
    if (!isMultiStatement)
      {
        state.preparedPlan = engine_.prepare(session, sql, &state.result);
        state.prepared = state.preparedPlan.get() != NULL && state.result.ok();
      }
    if (!state.prepared)
      {
        // Keep the protocol usable for utility statements and for compiler
        // forms that cannot expose a reusable CLI input descriptor. This is
        // only the compatibility path; normal DML/queries use the plan above.
        std::vector<std::string> dummy(state.parameterCount, "0");
        std::vector<bool> nulls(state.parameterCount, false);
        std::string describeSql;
        if (!substituteT4Parameters(sql, dummy, nulls, &describeSql))
          describeSql = sql;
        if (isMultiStatement && splitT4Statements(describeSql,
                                                  &multiStatements) &&
            !multiStatements.empty())
          state.result = engine_.describe(session, multiStatements.back());
        else
          state.result = engine_.describe(session, describeSql);
      }
    (*statements)[label] = state;

    std::string reply;
    if (!state.result.ok())
      {
        appendT4Error(&reply, state.result);
        sendT4Response(fd, request, reply);
        return;
      }
    appendU32(&reply, 0);
    appendU32(&reply, static_cast<uint32_t>(queryTypeForT4(state.result)));
    appendU32(&reply, 0); // statement handle is label-based here
    appendU32(&reply, 0); // estimated cost
    if (state.parameterCount == 0)
      appendU32(&reply, 0);
    else
      {
        appendU32(&reply, 1);
        appendU32(&reply,
                  static_cast<uint32_t>(state.parameterCount * 260));
        appendU32(&reply, static_cast<uint32_t>(state.parameterCount));
        uint32_t offset = 0;
        for (size_t i = 0; i < state.parameterCount; i++)
          {
            Column parameter;
            std::ostringstream name;
            name << "PARAM" << (i + 1);
            parameter.name = name.str();
            parameter.length = 256;
            appendT4Descriptor(&reply, parameter, offset);
            offset += 260;
          }
      }
    appendT4PrepareDescriptors(&reply, state.result);
    sendT4Response(fd, request, reply);
  }

  void handleT4Execute(
      int fd, const T4Header &request, const std::string &body,
      const std::shared_ptr<Session> &session,
      std::map<std::string, T4StatementState> *statements)
  {
    T4Reader reader(body);
    uint32_t fields[8];
    for (size_t i = 0; i < 8; i++)
      if (!reader.readU32(&fields[i]))
        {
          sendT4Response(fd, request, std::string(), 1, 0);
          return;
        }
    std::string unused;
    std::string label;
    if (!reader.readStringWithCharset(&unused) ||
        !reader.readStringWithCharset(&unused) ||
        !reader.readStringWithCharset(&label) ||
        !reader.readString(&unused))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    uint32_t dataLength = 0;
    std::string data;
    if (!reader.readU32(&dataLength) || !reader.readBytes(dataLength, &data))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    std::map<std::string, T4StatementState>::iterator found =
        statements->find(label);
    if (found == statements->end())
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    T4StatementState &state = found->second;
    const uint32_t rowCount = fields[3] == 0 ? 1 : fields[3];
    std::vector<std::vector<std::string> > rows;
    std::vector<std::vector<bool> > nullRows;
    bool valid = decodeT4ParameterRows(
        data, state.parameterCount, rowCount, &rows, &nullRows);
    std::string sql;
    if (!valid)
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    if (!state.prepared && !buildT4BatchSql(
            state.sql, rows, nullRows, &sql, &state.parameterParts,
            state.batchPrefix.empty() ? NULL : &state.batchPrefix,
            state.batchParameterParts.empty()
                ? NULL : &state.batchParameterParts))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    const std::string &transactionSql = state.prepared ? state.sql : sql;
    if (!session->autoCommit && session->transactionStatus.load() == 'I' &&
        !LiteSqlTable_isUtilityStatement(transactionSql.c_str()))
      {
        QueryResult begin = engine_.execute(session, "BEGIN");
        if (!begin.ok() && begin.error.find("already active") ==
                std::string::npos)
          {
            state.result = begin;
            state.rowOffset = 0;
            std::string reply;
            appendT4ExecuteReply(&reply, state.result);
            sendT4Response(fd, request, reply);
            return;
          }
      }
    if (state.prepared)
      state.result = engine_.executePrepared(session, state.preparedPlan,
                                             rows, nullRows);
    else
      {
        std::vector<std::string> batch;
        if (!splitT4Statements(sql, &batch))
          {
            state.result.sqlstate = "42601";
            state.result.error =
                "NativeLite batch has unterminated quoted text";
          }
        else
          state.result = batch.size() > 1
              ? engine_.executeBatch(session, batch)
              : engine_.execute(session, sql);
      }
    state.rowOffset = 0;
    std::string reply;
    appendT4ExecuteReply(&reply, state.result);
    sendT4Response(fd, request, reply);
  }

  void handleT4Fetch(
      int fd, const T4Header &request, const std::string &body,
      std::map<std::string, T4StatementState> *statements)
  {
    T4Reader reader(body);
    std::string label;
    uint64_t maxRows = 0;
    if (!reader.skip(16) || !reader.readStringWithCharset(&label) ||
        !reader.readU64(&maxRows))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    std::map<std::string, T4StatementState>::iterator found =
        statements->find(label);
    if (found == statements->end())
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    T4StatementState &state = found->second;
    std::string reply;
    if (state.rowOffset >= state.result.rows.size())
      {
        appendU32(&reply, 100); // SQL_NO_DATA_FOUND
        appendU32(&reply, 0);
        appendU32(&reply, 0);
        appendU32(&reply, 0);
        sendT4Response(fd, request, reply);
        return;
      }
    size_t count = maxRows == 0
        ? state.result.rows.size() - state.rowOffset
        : static_cast<size_t>(std::min<uint64_t>(
              maxRows, state.result.rows.size() - state.rowOffset));
    std::string rows = state.oldFetchFormat
        ? encodeT4OldRows(state.result, state.rowOffset,
                          state.rowOffset + count)
        : encodeT4Rows(state.result, state.rowOffset,
                       state.rowOffset + count);
    state.rowOffset += count;
    appendU32(&reply, 0);
    appendU32(&reply, static_cast<uint32_t>(count));
    appendU32(&reply, 0); // rowwise format
    appendU32(&reply, static_cast<uint32_t>(rows.size()));
    reply.append(rows);
    sendT4Response(fd, request, reply);
  }

  void addT4MetadataColumn(QueryResult *result, const std::string &name,
                           uint32_t length = 256)
  {
    Column column;
    column.name = name;
    column.length = static_cast<Lng32>(length);
    result->columns.push_back(column);
  }

  void appendT4OldDescriptor(std::string *reply, const Column &column)
  {
    uint32_t capacity = std::min<uint32_t>(t4ColumnCapacity(column), 0x7fffU);
    appendU32(reply, 0);
    appendU32(reply, static_cast<uint32_t>(-601));
    appendU32(reply, 0);
    appendU32(reply, capacity);
    appendU16(reply, static_cast<uint16_t>(capacity));
    appendU16(reply, 0);
    reply->push_back(1);
    appendT4String(reply, column.name);
    reply->push_back(0);
    appendU32(reply, 12);
    appendU16(reply, static_cast<uint16_t>(capacity));
    appendU32(reply, 15);
    appendU32(reply, 15);
    appendT4String(reply, std::string());
    appendT4String(reply, "TRAFODION");
    appendT4String(reply, "SEABASE");
    appendT4String(reply, column.name);
    appendU32(reply, 0);
    appendU32(reply, 0);
  }

  QueryResult buildT4CatalogResult(
      uint16_t api, const std::string &catalogPattern,
      const std::string &schemaPattern, const std::string &tablePattern,
      const std::string &typeList, const std::string &columnPattern)
  {
    QueryResult result;
    LiteRocksDBStore store;
    std::string error;
    if (api == 10053 && catalogPattern == "%" && schemaPattern.empty() &&
        tablePattern.empty())
      {
        addT4MetadataColumn(&result, "TABLE_CAT");
        std::vector<std::string> catalogs;
        if (!store.listCatalogs(&catalogs, &error))
          result.error = error;
        for (size_t i = 0; i < catalogs.size(); i++)
          result.rows.push_back(std::vector<Cell>(1, Cell(catalogs[i])));
      }
    else if (api == 10053 && catalogPattern.empty() &&
             schemaPattern == "%" && tablePattern.empty())
      {
        addT4MetadataColumn(&result, "TABLE_SCHEM");
        addT4MetadataColumn(&result, "TABLE_CATALOG");
        std::vector<std::string> schemas;
        if (!store.listSchemas("TRAFODION", &schemas, &error))
          result.error = error;
        for (size_t i = 0; i < schemas.size(); i++)
          {
            std::vector<Cell> row;
            row.push_back(Cell(schemas[i]));
            row.push_back(Cell("TRAFODION"));
            result.rows.push_back(row);
          }
      }
    else
      {
        std::vector<LiteTableDef> tables;
        if (!store.listTables("", "", &tables, &error))
          {
            result.error = error;
            result.sqlstate = "HY000";
            return result;
          }
        if (api == 10053)
          {
            const char *names[] = {
              "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE",
              "REMARKS", "TYPE_CAT", "TYPE_SCHEM", "TYPE_NAME",
              "SELF_REFERENCING_COL_NAME", "REF_GENERATION"
            };
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
              addT4MetadataColumn(&result, names[i]);
            for (size_t i = 0; i < tables.size(); i++)
              {
                const LiteTableDef &table = tables[i];
                std::string tableType = table.view ? "VIEW" : "TABLE";
                if (!t4Matches(table.catalog, catalogPattern) ||
                    !t4Matches(table.schema, schemaPattern) ||
                    !t4Matches(table.name, tablePattern) ||
                    (!typeList.empty() &&
                     typeList.find(tableType) == std::string::npos))
                  continue;
                std::vector<Cell> row;
                row.push_back(Cell(table.catalog));
                row.push_back(Cell(table.schema));
                row.push_back(Cell(table.name));
                row.push_back(Cell(tableType));
                while (row.size() < 10) row.push_back(Cell());
                result.rows.push_back(row);
              }
          }
        else if (api == 10039 || api == 40)
          {
            const char *names[] = {
              "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME",
              "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE", "BUFFER_LENGTH",
              "DECIMAL_DIGITS", "NUM_PREC_RADIX", "NULLABLE", "REMARKS",
              "COLUMN_DEF", "SQL_DATA_TYPE", "SQL_DATETIME_SUB",
              "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE",
              "SCOPE_CATALOG", "SCOPE_SCHEMA", "SCOPE_TABLE",
              "SOURCE_DATA_TYPE", "IS_AUTOINCREMENT", "IS_GENERATEDCOLUMN"
            };
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
              addT4MetadataColumn(&result, names[i]);
            for (size_t i = 0; i < tables.size(); i++)
              {
                const LiteTableDef &table = tables[i];
                if (!t4Matches(table.catalog, catalogPattern) ||
                    !t4Matches(table.schema, schemaPattern) ||
                    !t4Matches(table.name, tablePattern))
                  continue;
                for (size_t c = 0; c < table.columns.size(); c++)
                  {
                    const LiteColumnDef &column = table.columns[c];
                    if (!t4Matches(column.name, columnPattern)) continue;
                    std::vector<Cell> row;
                    row.push_back(Cell(table.catalog));
                    row.push_back(Cell(table.schema));
                    row.push_back(Cell(table.name));
                    row.push_back(Cell(column.name));
                    row.push_back(Cell(liteJdbcType(column.type)));
                    row.push_back(Cell(column.type));
                    row.push_back(Cell(liteTypeSize(column.type)));
                    row.push_back(Cell("0"));
                    row.push_back(Cell("0"));
                    row.push_back(Cell("10"));
                    row.push_back(Cell(column.nullable ? "1" : "0"));
                    row.push_back(Cell());
                    row.push_back(column.defaultValue.empty()
                                      ? Cell() : Cell(column.defaultValue));
                    row.push_back(Cell("0"));
                    row.push_back(Cell("0"));
                    row.push_back(Cell(liteTypeSize(column.type)));
                    row.push_back(Cell(std::to_string(c + 1)));
                    row.push_back(Cell(column.nullable ? "YES" : "NO"));
                    while (row.size() < 22) row.push_back(Cell());
                    row.push_back(Cell("NO"));
                    row.push_back(Cell("NO"));
                    result.rows.push_back(row);
                  }
              }
          }
        else if (api == 65)
          {
            const char *names[] = {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME",
                                   "COLUMN_NAME", "KEY_SEQ", "PK_NAME"};
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
              addT4MetadataColumn(&result, names[i]);
            for (size_t i = 0; i < tables.size(); i++)
              {
                const LiteTableDef &table = tables[i];
                if (!t4Matches(table.catalog, catalogPattern) ||
                    !t4Matches(table.schema, schemaPattern) ||
                    !t4Matches(table.name, tablePattern)) continue;
                for (size_t k = 0; k < table.primaryKeyColumns.size(); k++)
                  {
                    size_t c = table.primaryKeyColumns[k];
                    if (c >= table.columns.size()) continue;
                    std::vector<Cell> row;
                    row.push_back(Cell(table.catalog));
                    row.push_back(Cell(table.schema));
                    row.push_back(Cell(table.name));
                    row.push_back(Cell(table.columns[c].name));
                    row.push_back(Cell(std::to_string(k + 1)));
                    row.push_back(Cell(table.primaryKeyName));
                    result.rows.push_back(row);
                  }
              }
          }
        else
          {
            result.error = "NativeLite T4 catalog API is not supported";
            result.sqlstate = "HYC00";
          }
      }
    result.commandTag = "SELECT " + std::to_string(result.rows.size());
    return result;
  }

  void handleT4GetCatalogs(
      int fd, const T4Header &request, const std::string &body,
      std::map<std::string, T4StatementState> *statements)
  {
    T4Reader reader(body);
    uint32_t dialogue = 0;
    uint16_t api = 0;
    std::string label, catalog, schema, table, types, column;
    if (!reader.readU32(&dialogue) || !reader.readString(&label) ||
        !reader.readU16(&api) || !reader.readString(&catalog) ||
        !reader.readString(&schema) || !reader.readString(&table) ||
        !reader.readString(&types) || !reader.readString(&column))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    T4StatementState state;
    state.oldFetchFormat = true;
    state.result = buildT4CatalogResult(api, catalog, schema, table, types,
                                        column);
    (*statements)[label] = state;
    if (!state.result.ok())
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    std::string reply;
    appendU32(&reply, 0);
    appendU32(&reply, 0);
    appendT4String(&reply, label);
    appendU32(&reply, static_cast<uint32_t>(state.result.columns.size()));
    for (size_t i = 0; i < state.result.columns.size(); i++)
      appendT4OldDescriptor(&reply, state.result.columns[i]);
    appendU32(&reply, 0); // warning descriptor count
    appendT4String(&reply, std::string());
    sendT4Response(fd, request, reply);
  }

  void handleT4FreeStatement(
      int fd, const T4Header &request, const std::string &body,
      std::map<std::string, T4StatementState> *statements)
  {
    T4Reader reader(body);
    uint32_t dialogue = 0;
    std::string label;
    uint16_t option = 0;
    if (reader.readU32(&dialogue) && reader.readString(&label) &&
        reader.readU16(&option))
      {
        std::map<std::string, T4StatementState>::iterator found =
            statements->find(label);
        if (option == 1)
          statements->erase(label); // SQL_DROP
        else if (found != statements->end())
          {
            found->second.result = QueryResult(); // SQL_CLOSE
            found->second.rowOffset = 0;
          }
      }
    std::string reply;
    appendU32(&reply, 0);
    appendU32(&reply, 0);
    sendT4Response(fd, request, reply);
  }

  void handleT4EndTransaction(
      int fd, const T4Header &request, const std::string &body,
      const std::shared_ptr<Session> &session)
  {
    T4Reader reader(body);
    uint32_t dialogue = 0;
    uint16_t option = 0;
    QueryResult result;
    if (!reader.readU32(&dialogue) || !reader.readU16(&option))
      {
        sendT4Response(fd, request, std::string(), 1, 0);
        return;
      }
    result = engine_.execute(session, option == 1 ? "ROLLBACK" : "COMMIT");
    // JDBC permits commit/rollback when no work has started in the current
    // transaction.  Lite's transaction manager reports that condition as
    // -8001; expose it as the required no-op instead of a driver error.
    if (!result.ok() &&
        result.error.find("no active lite transaction") !=
            std::string::npos)
      {
        result = QueryResult();
        result.commandTag = option == 1 ? "ROLLBACK" : "COMMIT";
      }
    std::string reply;
    if (result.ok())
      {
        appendU32(&reply, 0);
        appendU32(&reply, 0);
      }
    else
      {
        appendU32(&reply, 3);
        appendU32(&reply, 0);
        appendU32(&reply, 1);
        appendT4ErrorDescriptor(&reply, result);
        appendU32(&reply, 0); // no warning list
      }
    sendT4Response(fd, request, reply);
  }

  void serveClientAndMark(
      int fd, const std::shared_ptr<std::atomic<bool> > &done)
  {
    serveClient(fd);
    done->store(true);
  }

  void finishSession(int fd, const std::shared_ptr<Session> &session)
  {
    {
      std::lock_guard<std::mutex> lock(sessionMutex_);
      sessions_.erase(session->backendPid);
    }
    engine_.destroy(session);
    releaseSessionSlot(session);
    closeClientFd(fd);
  }

  bool tryAcquireSessionSlot()
  {
    int current = activeSessions_.load();
    while (current < static_cast<int>(maxSessions_))
      if (activeSessions_.compare_exchange_weak(current, current + 1))
        return true;
    return false;
  }

  void releaseSessionSlot(const std::shared_ptr<Session> &session)
  {
    if (!session || !session->slotAcquired)
      return;
    session->slotAcquired = false;
    activeSessions_.fetch_sub(1);
  }

  void closeClientFd(int fd)
  {
    close(fd);
    std::lock_guard<std::mutex> lock(clientMutex_);
    std::vector<int>::iterator found =
        std::find(clientFds_.begin(), clientFds_.end(), fd);
    if (found != clientFds_.end())
      clientFds_.erase(found);
  }

  std::string host_;
  int port_;
  std::string unixSocket_;
  int listenFd_;
  bool ownsUnixSocket_;
  dev_t unixSocketDevice_;
  ino_t unixSocketInode_;
  NativeLiteEngine engine_;
  unsigned int maxSessions_;
  std::atomic<int> activeSessions_;
  std::atomic<int32_t> nextDialogueId_;
  std::mutex sessionMutex_;
  std::map<int32_t, std::weak_ptr<Session> > sessions_;
  std::mutex clientMutex_;
  std::vector<int> clientFds_;
  std::vector<std::thread> clientThreads_;
  std::vector<std::shared_ptr<std::atomic<bool> > > clientThreadDone_;
};

void signalHandler(int)
{
  gStopping.store(true);
  int fd = gListenFd.exchange(-1);
  if (fd >= 0)
    close(fd);
}

void usage(const char *program)
{
  std::cerr << "Usage: " << program
            << " [--listen 127.0.0.1] [--port 23400]"
               " [--unix-socket PATH] [--workers N]" << std::endl;
}

unsigned int defaultWorkerLimit()
{
  unsigned int concurrency = std::thread::hardware_concurrency();
  if (concurrency == 0)
    concurrency = 2;
  return std::min<unsigned int>(32, std::max<unsigned int>(2, concurrency));
}

} // namespace

int main(int argc, char **argv)
{
  std::string host = "127.0.0.1";
  int port = 23400;
  std::string unixSocket;
  unsigned int workers = defaultWorkerLimit();
  for (int i = 1; i < argc; i++)
    {
      std::string option = argv[i];
      if (option == "--listen" && i + 1 < argc)
        host = argv[++i];
      else if (option == "--port" && i + 1 < argc)
        {
          char *end = NULL;
          long parsed = strtol(argv[++i], &end, 10);
          if (!end || *end != '\0' || parsed < 1 || parsed > 65535)
            {
              usage(argv[0]);
              return 2;
            }
          port = static_cast<int>(parsed);
        }
      else if (option == "--unix-socket" && i + 1 < argc)
        unixSocket = argv[++i];
      else if (option == "--workers" && i + 1 < argc)
        {
          char *end = NULL;
          long parsed = strtol(argv[++i], &end, 10);
          if (!end || *end != '\0' || parsed < 1 || parsed > 256)
            {
              usage(argv[0]);
              return 2;
            }
          workers = static_cast<unsigned int>(parsed);
        }
      else if (option == "--help")
        {
          usage(argv[0]);
          return 0;
        }
      else
        {
          usage(argv[0]);
          return 2;
        }
    }

  if (LiteConfig_init() != 0)
    {
      std::cerr << "failed to initialize NativeLite environment" << std::endl;
      return 1;
    }
  atexit(my_mpi_fclose);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  NativeLiteServer server(host, port, unixSocket, workers);
  std::string error;
  if (!server.start(&error))
    {
      std::cerr << "NativeLite server startup failed: " << error << std::endl;
      return 1;
    }
  return server.run();
}

#else

#include <iostream>
int main()
{
  std::cerr << "nativelite-server requires TRAF_LITE" << std::endl;
  return 1;
}

#endif
