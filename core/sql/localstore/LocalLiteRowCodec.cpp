// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteRowCodec.h"

#include "BigNumHelper.h"
#include "LocalLiteRocksDBStore.h"
#include "Platform.h"
#include "NABoolean.h"
#include "str.h"
#include "ExpAlignedFormat.h"
#include "exp_attrs.h"
#include "exp_tuple_desc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char LOCAL_LITE_BINARY_ROW_MAGIC[] = "LLBR1";

enum LocalLiteCodecType
{
  LL_TYPE_INT8,
  LL_TYPE_INT16,
  LL_TYPE_INT32,
  LL_TYPE_INT64,
  LL_TYPE_FLOAT32,
  LL_TYPE_FLOAT64,
  LL_TYPE_NUMERIC,
  LL_TYPE_DATETIME,
  LL_TYPE_CHAR,
  LL_TYPE_VARCHAR
};

struct LocalLiteStoredColumn
{
  LocalLiteCodecType type;
  size_t length;
  size_t precision;
  size_t scale;
  bool decimalStorage;
  bool nullable;
  size_t voaOffset;
  size_t nullIndOffset;
  int nullBitIndex;
  size_t vcLenIndOffset;
  size_t offset;
};

static void setError(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
}

static std::string upper(const std::string &s)
{
  std::string out = s;
  for (size_t i = 0; i < out.size(); i++)
    out[i] = static_cast<char>(toupper(static_cast<unsigned char>(out[i])));
  return out;
}

static bool startsWithWord(const std::string &s, const char *word)
{
  size_t len = strlen(word);
  return s.size() >= len &&
         s.compare(0, len, word) == 0 &&
         (s.size() == len || !isalnum(static_cast<unsigned char>(s[len])));
}

static size_t typeArg(const std::string &type, size_t defaultValue)
{
  size_t lparen = type.find('(');
  if (lparen == std::string::npos)
    return defaultValue;
  const char *p = type.c_str() + lparen + 1;
  char *end = NULL;
  long value = strtol(p, &end, 10);
  return value > 0 ? static_cast<size_t>(value) : defaultValue;
}

static size_t secondTypeArg(const std::string &type, size_t defaultValue)
{
  size_t lparen = type.find('(');
  if (lparen == std::string::npos)
    return defaultValue;
  size_t comma = type.find(',', lparen + 1);
  if (comma == std::string::npos)
    return defaultValue;
  const char *p = type.c_str() + comma + 1;
  char *end = NULL;
  long value = strtol(p, &end, 10);
  return value >= 0 ? static_cast<size_t>(value) : defaultValue;
}

static size_t numericStorageSize(size_t precision)
{
  if (precision <= 2)
    return 1;
  if (precision <= 4)
    return 2;
  if (precision <= 9)
    return 4;
  return 8;
}

static size_t bigNumStorageSize(size_t precision)
{
  Lng32 len =
    BigNumHelper::ConvPrecisionToStorageLengthHelper(static_cast<Lng32>(precision));
  return len > 0 ? static_cast<size_t>(len) : 0;
}

static bool mapType(const std::string &typeText,
                    LocalLiteCodecType *type,
                    size_t *length,
                    size_t *precisionOut,
                    size_t *scale,
                    bool *decimalStorage)
{
  *scale = 0;
  *precisionOut = 0;
  *decimalStorage = false;
  std::string typeName = upper(typeText);
  if (startsWithWord(typeName, "TINYINT"))
    {
      *type = LL_TYPE_INT8;
      *length = 1;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "SMALLINT"))
    {
      *type = LL_TYPE_INT16;
      *length = 2;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "INT") ||
      startsWithWord(typeName, "INTEGER"))
    {
      *type = LL_TYPE_INT32;
      *length = 4;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "LARGEINT") ||
      startsWithWord(typeName, "BIGINT"))
    {
      *type = LL_TYPE_INT64;
      *length = 8;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "REAL") ||
      startsWithWord(typeName, "FLOAT"))
    {
      *type = LL_TYPE_FLOAT32;
      *length = 4;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "DOUBLE"))
    {
      *type = LL_TYPE_FLOAT64;
      *length = 8;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "NUMERIC"))
    {
      size_t precision = typeArg(typeName, 18);
      size_t typeScale = secondTypeArg(typeName, 0);
      if (precision < 1 || typeScale > precision)
        return false;
      *type = LL_TYPE_NUMERIC;
      *scale = typeScale;
      *precisionOut = precision;
      *decimalStorage = false;
      *length = precision > 18 ? bigNumStorageSize(precision)
                               : numericStorageSize(precision);
      if (*length == 0)
        return false;
      return true;
    }
  if (startsWithWord(typeName, "DECIMAL"))
    {
      size_t precision = typeArg(typeName, 18);
      size_t typeScale = secondTypeArg(typeName, 0);
      if (precision < 1 || typeScale > precision)
        return false;
      *type = LL_TYPE_NUMERIC;
      *scale = typeScale;
      *precisionOut = precision;
      *decimalStorage = true;
      *length = precision;
      return true;
    }
  if (startsWithWord(typeName, "DATE"))
    {
      *type = LL_TYPE_DATETIME;
      *length = 4;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "TIME"))
    {
      *type = LL_TYPE_DATETIME;
      *length = 8;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "TIMESTAMP"))
    {
      *type = LL_TYPE_DATETIME;
      *length = 11;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "VARCHAR") ||
      startsWithWord(typeName, "CHARACTER VARYING"))
    {
      *type = LL_TYPE_VARCHAR;
      *length = typeArg(typeName, 1);
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "CHAR") ||
      startsWithWord(typeName, "CHARACTER"))
    {
      *type = LL_TYPE_CHAR;
      *length = typeArg(typeName, 1);
      *precisionOut = *length;
      return true;
    }
  return false;
}

static size_t alignTo(size_t offset, size_t alignment)
{
  if (alignment <= 1)
    return offset;
  size_t rem = offset % alignment;
  return rem ? offset + alignment - rem : offset;
}

static size_t typeAlignment(LocalLiteCodecType type)
{
  switch (type)
    {
    case LL_TYPE_INT8:
    case LL_TYPE_CHAR:
    case LL_TYPE_VARCHAR:
      return 1;
    case LL_TYPE_INT16:
      return 2;
    case LL_TYPE_INT32:
    case LL_TYPE_FLOAT32:
      return 4;
    case LL_TYPE_INT64:
    case LL_TYPE_FLOAT64:
    case LL_TYPE_DATETIME:
    case LL_TYPE_NUMERIC:
      return 8;
    }
  return 1;
}

static bool computeLayout(const LocalLiteTableDef &table,
                          std::vector<LocalLiteStoredColumn> *columns,
                          size_t *rowLen,
                          std::string *error)
{
  columns->clear();
  columns->resize(table.columns.size());

  size_t nullableCount = 0;
  size_t variableCount = 0;
  for (size_t i = 0; i < table.columns.size(); i++)
    {
      LocalLiteStoredColumn &col = (*columns)[i];
      if (!mapType(table.columns[i].type, &col.type, &col.length,
                   &col.precision, &col.scale, &col.decimalStorage))
        {
          setError(error, "unsupported local-lite binary row column type: " +
                  table.columns[i].type);
          return false;
        }
      col.nullable = table.columns[i].nullable;
      col.voaOffset = 0;
      col.nullIndOffset = 0;
      col.nullBitIndex = -1;
      col.vcLenIndOffset = 0;
      col.offset = 0;
      if (col.nullable)
        nullableCount++;
      if (col.type == LL_TYPE_VARCHAR)
        variableCount++;
    }

  size_t hdrSize = ExpAlignedFormat::getHdrSize();
  size_t voaOffset = hdrSize;
  for (size_t i = 0; i < columns->size(); i++)
    {
      if ((*columns)[i].type == LL_TYPE_VARCHAR)
        {
          (*columns)[i].voaOffset = voaOffset;
          voaOffset += ExpAlignedFormat::OFFSET_SIZE;
        }
    }

  size_t bitmapOffset = 0;
  size_t firstFixed = voaOffset;
  if (nullableCount > 0)
    {
      bitmapOffset = firstFixed;
      firstFixed += ExpAlignedFormat::getNeededBitmapSize(nullableCount);
    }

  size_t firstFixedAlign = 1;
  for (size_t i = 0; i < columns->size(); i++)
    {
      if ((*columns)[i].type != LL_TYPE_VARCHAR)
        {
          firstFixedAlign = typeAlignment((*columns)[i].type);
          break;
        }
    }
  firstFixed = alignTo(firstFixed, firstFixedAlign);

  int nullBit = 0;
  size_t fixedOffset = firstFixed;
  for (size_t i = 0; i < columns->size(); i++)
    {
      LocalLiteStoredColumn &col = (*columns)[i];
      if (col.nullable)
        {
          col.nullIndOffset = bitmapOffset;
          col.nullBitIndex = nullBit++;
        }
      if (col.type == LL_TYPE_VARCHAR)
        continue;
      fixedOffset = alignTo(fixedOffset, typeAlignment(col.type));
      col.offset = fixedOffset;
      fixedOffset += col.length;
    }

  size_t varOffset = fixedOffset;
  for (size_t i = 0; i < columns->size(); i++)
    {
      LocalLiteStoredColumn &col = (*columns)[i];
      if (col.type != LL_TYPE_VARCHAR)
        continue;
      col.vcLenIndOffset = varOffset;
      col.offset = varOffset + ExpAlignedFormat::VARIABLE_LEN_SIZE;
      varOffset = col.offset + col.length;
    }

  (void)variableCount;
  *rowLen = alignTo(varOffset, ExpAlignedFormat::ALIGNMENT);
  return true;
}

static bool isNullValue(const std::string &value)
{
  return value.empty();
}

static bool parseScaledInt64(const std::string &value,
                             size_t scale,
                             Int64 *out)
{
  const char *p = value.c_str();
  bool negative = false;
  if (*p == '-' || *p == '+')
    {
      negative = (*p == '-');
      p++;
    }

  UInt64 magnitude = 0;
  size_t fracDigits = 0;
  bool seenDigit = false;
  bool seenPoint = false;

  for (; *p; p++)
    {
      if (*p == '.')
        {
          if (seenPoint)
            return false;
          seenPoint = true;
          continue;
        }

      if (*p < '0' || *p > '9')
        return false;

      seenDigit = true;
      magnitude = magnitude * 10 + static_cast<UInt64>(*p - '0');
      if (seenPoint)
        fracDigits++;
    }

  if (!seenDigit || fracDigits > scale)
    return false;

  while (fracDigits < scale)
    {
      magnitude *= 10;
      fracDigits++;
    }

  if (negative)
    *out = static_cast<Int64>(static_cast<UInt64>(0) - magnitude);
  else
    *out = static_cast<Int64>(magnitude);
  return true;
}

static bool parseScaledDigits(const std::string &value,
                              size_t scale,
                              size_t precision,
                              bool *negativeOut,
                              std::string *digitsOut)
{
  const char *p = value.c_str();
  bool negative = false;
  if (*p == '-' || *p == '+')
    {
      negative = (*p == '-');
      p++;
    }

  std::string digits;
  size_t fracDigits = 0;
  bool seenDigit = false;
  bool seenPoint = false;

  for (; *p; p++)
    {
      if (*p == '.')
        {
          if (seenPoint)
            return false;
          seenPoint = true;
          continue;
        }

      if (*p < '0' || *p > '9')
        return false;

      seenDigit = true;
      digits.push_back(*p);
      if (seenPoint)
        fracDigits++;
    }

  if (!seenDigit || fracDigits > scale)
    return false;

  while (fracDigits < scale)
    {
      digits.push_back('0');
      fracDigits++;
    }

  size_t firstNonZero = digits.find_first_not_of('0');
  if (firstNonZero == std::string::npos)
    {
      digits.assign(1, '0');
      negative = false;
    }
  else if (firstNonZero > 0)
    digits.erase(0, firstNonZero);

  if (digits.size() > precision)
    return false;

  digits.insert(digits.begin(), precision - digits.size(), '0');
  *negativeOut = negative;
  *digitsOut = digits;
  return true;
}

static bool writeDecimalValue(char *target,
                              const LocalLiteStoredColumn &col,
                              const std::string &value,
                              std::string *error)
{
  bool negative = false;
  std::string digits;
  if (!parseScaledDigits(value, col.scale, col.precision,
                         &negative, &digits))
    {
      setError(error, "invalid local-lite decimal literal");
      return false;
    }
  if (digits.size() != col.length)
    {
      setError(error, "local-lite decimal storage length mismatch");
      return false;
    }

  str_cpy_all(target, digits.data(), digits.size());
  if (negative)
    target[0] = static_cast<char>(target[0] | 0200);
  return true;
}

static bool writeBigNumValue(char *target,
                             const LocalLiteStoredColumn &col,
                             const std::string &value,
                             std::string *error)
{
  bool negative = false;
  std::string digits;
  if (!parseScaledDigits(value, col.scale, col.precision,
                         &negative, &digits))
    {
      setError(error, "invalid local-lite BigNum literal");
      return false;
    }

  std::string ascii;
  ascii.reserve(digits.size() + 1);
  ascii.push_back(negative ? '-' : '+');
  ascii.append(digits);

  if (BigNumHelper::ConvAsciiToBigNumWithSignHelper(
          static_cast<Lng32>(ascii.size()),
          static_cast<Lng32>(col.length),
          &ascii[0],
          target) != 0)
    {
      setError(error, "invalid local-lite BigNum literal");
      return false;
    }
  return true;
}

static bool writeValue(char *row,
                       const LocalLiteStoredColumn &col,
                       const std::string &value,
                       std::string *error)
{
  if (isNullValue(value) && col.nullable)
    {
      ExpTupleDesc::setNullValue(row + col.nullIndOffset, col.nullBitIndex,
                                 ExpTupleDesc::SQLMX_ALIGNED_FORMAT);
      return true;
    }
  if (isNullValue(value) && !col.nullable)
    {
      setError(error, "NULL is not allowed for local-lite NOT NULL column");
      return false;
    }
  if (col.nullable)
    ExpTupleDesc::clearNullValue(row + col.nullIndOffset, col.nullBitIndex,
                                 ExpTupleDesc::SQLMX_ALIGNED_FORMAT);

  char *target = row + col.offset;
  switch (col.type)
    {
    case LL_TYPE_INT8:
      {
        Int8 v = static_cast<Int8>(strtol(value.c_str(), NULL, 10));
        str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
        return true;
      }
    case LL_TYPE_INT16:
      {
        Int16 v = static_cast<Int16>(strtol(value.c_str(), NULL, 10));
        str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
        return true;
      }
    case LL_TYPE_INT32:
      {
        Int32 v = static_cast<Int32>(strtol(value.c_str(), NULL, 10));
        str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
        return true;
      }
    case LL_TYPE_INT64:
      {
        Int64 v = static_cast<Int64>(strtoll(value.c_str(), NULL, 10));
        str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
        return true;
      }
    case LL_TYPE_FLOAT32:
      {
        float v = static_cast<float>(strtod(value.c_str(), NULL));
        str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
        return true;
      }
    case LL_TYPE_FLOAT64:
      {
        double v = strtod(value.c_str(), NULL);
        str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
        return true;
      }
    case LL_TYPE_NUMERIC:
      {
        if (col.precision > 18)
          return writeBigNumValue(target, col, value, error);
        if (col.decimalStorage)
          return writeDecimalValue(target, col, value, error);

        Int64 scaled = 0;
        if (!parseScaledInt64(value, col.scale, &scaled))
          {
            setError(error, "invalid local-lite numeric literal");
            return false;
          }
        switch (col.length)
          {
          case 1:
            {
              Int8 v = static_cast<Int8>(scaled);
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              return true;
            }
          case 2:
            {
              Int16 v = static_cast<Int16>(scaled);
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              return true;
            }
          case 4:
            {
              Int32 v = static_cast<Int32>(scaled);
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              return true;
            }
          case 8:
            {
              Int64 v = scaled;
              str_cpy_all(target, reinterpret_cast<char *>(&v), sizeof(v));
              return true;
            }
          default:
            setError(error, "local-lite numeric key requires binary numeric storage");
            return false;
          }
      }
    case LL_TYPE_DATETIME:
      {
        setError(error, "local-lite datetime values require executor expression encoding");
        return false;
      }
    case LL_TYPE_CHAR:
      {
        str_pad(target, col.length, ' ');
        size_t len = value.size() > col.length ? col.length : value.size();
        if (len > 0)
          str_cpy_all(target, value.data(), len);
        return true;
      }
    case LL_TYPE_VARCHAR:
      {
        size_t len = value.size() > col.length ? col.length : value.size();
        ExpAlignedFormat::setVarLength(row + col.vcLenIndOffset,
                                       static_cast<UInt32>(len));
        if (len > 0)
          str_cpy_all(target, value.data(), len);
        return true;
      }
    }
  setError(error, "unsupported local-lite binary row column type");
  return false;
}

bool LocalLiteEncodeBinaryRow(const LocalLiteTableDef &table,
                              const std::vector<std::string> &fields,
                              std::string *encoded,
                              std::string *error)
{
  if (fields.size() != table.columns.size())
    {
      setError(error, "local-lite binary row field count does not match table");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;

  std::string row(rowLen, '\0');
  ExpTupleDesc::setFirstFixedOffset(&row[0],
                                    columns.empty() ? ExpAlignedFormat::getHdrSize()
                                                    : 0,
                                    ExpTupleDesc::SQLMX_ALIGNED_FORMAT);

  size_t firstFixed = rowLen;
  size_t bitmapOffset = 0;
  for (size_t i = 0; i < columns.size(); i++)
    {
      const LocalLiteStoredColumn &col = columns[i];
      if (col.type != LL_TYPE_VARCHAR && col.offset < firstFixed)
        firstFixed = col.offset;
      if (col.nullable)
        bitmapOffset = col.nullIndOffset;
    }
  if (firstFixed == rowLen)
    firstFixed = ExpAlignedFormat::getHdrSize();

  ExpTupleDesc::setFirstFixedOffset(&row[0], static_cast<UInt32>(firstFixed),
                                    ExpTupleDesc::SQLMX_ALIGNED_FORMAT);
  ExpAlignedFormat::setBitmapOffset(&row[0] + ExpAlignedFormat::OFFSET_SIZE,
                                    static_cast<UInt32>(bitmapOffset));

  for (size_t i = 0; i < columns.size(); i++)
    {
      if (columns[i].type == LL_TYPE_VARCHAR)
        ExpTupleDesc::setVoaValue(&row[0], columns[i].voaOffset,
                                  columns[i].vcLenIndOffset,
                                  ExpTupleDesc::SQLMX_ALIGNED_FORMAT);
      if (!writeValue(&row[0], columns[i], fields[i], error))
        return false;
    }

  UInt32 adjustedLen = ExpAlignedFormat::adjustDataLength(&row[0],
                                                          static_cast<UInt32>(rowLen),
                                                          ExpAlignedFormat::ALIGNMENT,
                                                          TRUE);
  row.resize(adjustedLen);

  encoded->assign(LOCAL_LITE_BINARY_ROW_MAGIC,
                  sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  encoded->append(row);
  return true;
}

bool LocalLiteWrapBinaryRow(const char *row,
                            size_t rowLen,
                            std::string *encoded,
                            std::string *error)
{
  if (!row && rowLen > 0)
    {
      setError(error, "missing local-lite binary row");
      return false;
    }
  encoded->assign(LOCAL_LITE_BINARY_ROW_MAGIC,
                  sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  if (rowLen > 0)
    encoded->append(row, rowLen);
  return true;
}

static bool hasRange(const std::string &row, size_t offset, size_t length)
{
  return offset <= row.size() && length <= row.size() - offset;
}

static bool initializeCanonicalRow(const std::vector<LocalLiteStoredColumn> &columns,
                                   char *row,
                                   size_t rowLen)
{
  str_pad(row, rowLen, '\0');

  size_t firstFixed = rowLen;
  size_t bitmapOffset = 0;
  for (size_t i = 0; i < columns.size(); i++)
    {
      const LocalLiteStoredColumn &col = columns[i];
      if (col.type != LL_TYPE_VARCHAR && col.offset < firstFixed)
        firstFixed = col.offset;
      if (col.nullable)
        bitmapOffset = col.nullIndOffset;
    }
  if (firstFixed == rowLen)
    firstFixed = ExpAlignedFormat::getHdrSize();

  ExpTupleDesc::setFirstFixedOffset(row, static_cast<UInt32>(firstFixed),
                                    ExpTupleDesc::SQLMX_ALIGNED_FORMAT);
  ExpAlignedFormat::setBitmapOffset(row + ExpAlignedFormat::OFFSET_SIZE,
                                    static_cast<UInt32>(bitmapOffset));

  for (size_t i = 0; i < columns.size(); i++)
    {
      if (columns[i].type == LL_TYPE_VARCHAR)
        ExpTupleDesc::setVoaValue(row, columns[i].voaOffset,
                                  columns[i].vcLenIndOffset,
                                  ExpTupleDesc::SQLMX_ALIGNED_FORMAT);
    }
  return true;
}

static bool attrIsNull(const char *row, Attributes *attr,
                       ExpTupleDesc *td)
{
  if (!attr || !attr->getNullFlag())
    return false;
  return ExpTupleDesc::isNullValue(
      const_cast<char *>(row) + attr->getNullIndOffset(),
      attr->getNullBitIndex(),
      td->getTupleDataFormat());
}

static bool copyAttrToCanonical(const char *srcRow,
                                size_t srcRowLen,
                                Attributes *srcAttr,
                                ExpTupleDesc *srcTd,
                                char *destRow,
                                const LocalLiteStoredColumn &destCol,
                                std::string *error)
{
  if (!srcAttr)
    {
      setError(error, "local-lite insert missing source attribute");
      return false;
    }

  if (attrIsNull(srcRow, srcAttr, srcTd))
    {
      if (!destCol.nullable)
        {
          setError(error, "NULL is not allowed for local-lite NOT NULL column");
          return false;
        }
      ExpTupleDesc::setNullValue(destRow + destCol.nullIndOffset,
                                 destCol.nullBitIndex,
                                 ExpTupleDesc::SQLMX_ALIGNED_FORMAT);
      return true;
    }
  if (destCol.nullable)
    ExpTupleDesc::clearNullValue(destRow + destCol.nullIndOffset,
                                 destCol.nullBitIndex,
                                 ExpTupleDesc::SQLMX_ALIGNED_FORMAT);

  size_t srcOffset = srcAttr->getOffset();
  size_t srcLen = srcAttr->getLength();
  if (srcAttr->getVCIndicatorLength() > 0)
    {
      if (!hasRange(std::string(srcRow, srcRowLen),
                    srcAttr->getVCLenIndOffset(),
                    srcAttr->getVCIndicatorLength()))
        {
          setError(error, "truncated local-lite executor row");
          return false;
        }
      srcLen = srcAttr->getLength(srcRow + srcAttr->getVCLenIndOffset());
    }
  if (srcAttr->getVCIndicatorLength() > 0)
    srcOffset = srcAttr->getOffset();

  if (!hasRange(std::string(srcRow, srcRowLen), srcOffset, srcLen))
    {
      setError(error, "truncated local-lite executor row");
      return false;
    }

  const char *src = srcRow + srcOffset;
  switch (destCol.type)
    {
    case LL_TYPE_INT8:
    case LL_TYPE_INT16:
    case LL_TYPE_INT32:
    case LL_TYPE_INT64:
    case LL_TYPE_FLOAT32:
    case LL_TYPE_FLOAT64:
    case LL_TYPE_DATETIME:
    case LL_TYPE_NUMERIC:
      {
        size_t len = srcLen < destCol.length ? srcLen : destCol.length;
        str_cpy_all(destRow + destCol.offset, src, len);
        return true;
      }
    case LL_TYPE_CHAR:
      {
        str_pad(destRow + destCol.offset, destCol.length, ' ');
        size_t len = srcLen < destCol.length ? srcLen : destCol.length;
        if (len > 0)
          str_cpy_all(destRow + destCol.offset, src, len);
        return true;
      }
    case LL_TYPE_VARCHAR:
      {
        size_t len = srcLen < destCol.length ? srcLen : destCol.length;
        ExpAlignedFormat::setVarLength(destRow + destCol.vcLenIndOffset,
                                       static_cast<UInt32>(len));
        if (len > 0)
          str_cpy_all(destRow + destCol.offset, src, len);
        return true;
      }
    }
  setError(error, "unsupported local-lite binary row column type");
  return false;
}

bool LocalLiteNormalizeBinaryRow(const LocalLiteTableDef &table,
                                 ExpTupleDesc *srcTd,
                                 const char *srcRow,
                                 size_t srcRowLen,
                                 std::string *encoded,
                                 std::string *error)
{
  if (!srcTd || !srcRow)
    {
      setError(error, "local-lite insert missing executor row");
      return false;
    }
  if (srcTd->numAttrs() != table.columns.size())
    {
      setError(error, "local-lite insert row does not match table column count");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;

  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());

  for (UInt32 i = 0; i < srcTd->numAttrs(); i++)
    {
      if (!copyAttrToCanonical(srcRow, srcRowLen, srcTd->getAttr(i), srcTd,
                               &row[0], columns[i], error))
        return false;
    }

  UInt32 adjustedLen = ExpAlignedFormat::adjustDataLength(&row[0],
                                                          static_cast<UInt32>(row.size()),
                                                          ExpAlignedFormat::ALIGNMENT,
                                                          TRUE);
  row.resize(adjustedLen);
  encoded->assign(LOCAL_LITE_BINARY_ROW_MAGIC,
                  sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  encoded->append(row);
  return true;
}

static bool storedColumnIsNull(const std::string &row,
                               const LocalLiteStoredColumn &col)
{
  if (!col.nullable)
    return false;
  if (col.nullIndOffset >= row.size())
    return false;
  return ExpAlignedFormat::isNullValue(
      const_cast<char *>(row.data()) + col.nullIndOffset,
      static_cast<UInt16>(col.nullBitIndex));
}

static void appendKeyUint64(std::string *s, uint64_t v)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    *s += static_cast<char>((v >> shift) & 0xff);
}

bool LocalLiteBuildPrimaryKey(const LocalLiteTableDef &table,
                              const std::string &encoded,
                              std::string *key,
                              std::string *error)
{
  if (!key)
    {
      setError(error, "missing local-lite primary key output");
      return false;
    }
  if (table.primaryKeyColumns.empty())
    {
      setError(error, "local-lite table has no primary key");
      return false;
    }
  if (encoded.size() < sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1 ||
      memcmp(encoded.data(), LOCAL_LITE_BINARY_ROW_MAGIC,
             sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1) != 0)
    {
      setError(error, "invalid local-lite binary row payload");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t fullRowLen = 0;
  if (!computeLayout(table, &columns, &fullRowLen, error))
    return false;

  std::string storedRow =
    encoded.substr(sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  if (storedRow.empty() && fullRowLen > 0)
    {
      setError(error, "truncated local-lite binary row payload");
      return false;
    }

  key->clear();
  key->push_back('P');
  appendKeyUint64(key, static_cast<uint64_t>(table.primaryKeyColumns.size()));
  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    {
      size_t sourceIndex = table.primaryKeyColumns[i];
      if (sourceIndex >= columns.size())
        {
          setError(error, "local-lite primary key column index out of range");
          return false;
        }

      const LocalLiteStoredColumn &col = columns[sourceIndex];
      if (storedColumnIsNull(storedRow, col))
        {
          setError(error, "NULL is not allowed for local-lite primary key column");
          return false;
        }

      const char *src = NULL;
      size_t len = 0;
      if (col.type == LL_TYPE_VARCHAR)
        {
          if (!hasRange(storedRow, col.vcLenIndOffset,
                        ExpAlignedFormat::VARIABLE_LEN_SIZE))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          len = ExpAlignedFormat::getVarLength(
              const_cast<char *>(storedRow.data()) + col.vcLenIndOffset);
          if (!hasRange(storedRow, col.offset, len))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          src = storedRow.data() + col.offset;
        }
      else
        {
          len = col.length;
          if (!hasRange(storedRow, col.offset, len))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          src = storedRow.data() + col.offset;
        }

      appendKeyUint64(key, static_cast<uint64_t>(sourceIndex));
      appendKeyUint64(key, static_cast<uint64_t>(len));
      if (len > 0)
        key->append(src, len);
    }
  return true;
}

bool LocalLiteBuildPrimaryKeyFromTextFields(
    const LocalLiteTableDef &table,
    const std::vector<std::string> &keyFields,
    std::string *key,
    std::string *error)
{
  if (keyFields.size() != table.primaryKeyColumns.size())
    {
      setError(error, "local-lite primary key field count mismatch");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;

  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());

  for (size_t i = 0; i < table.primaryKeyColumns.size(); i++)
    {
      size_t columnIndex = table.primaryKeyColumns[i];
      if (columnIndex >= columns.size())
        {
          setError(error, "local-lite primary key column index out of range");
          return false;
        }
      if (!writeValue(&row[0], columns[columnIndex], keyFields[i], error))
        return false;
    }

  UInt32 adjustedLen = ExpAlignedFormat::adjustDataLength(
      &row[0],
      static_cast<UInt32>(row.size()),
      ExpAlignedFormat::ALIGNMENT,
      TRUE);
  row.resize(adjustedLen);

  std::string encoded;
  encoded.assign(LOCAL_LITE_BINARY_ROW_MAGIC,
                 sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  encoded.append(row);
  return LocalLiteBuildPrimaryKey(table, encoded, key, error);
}

bool LocalLiteBuildUniqueKey(const LocalLiteTableDef &table,
                             const std::string &encoded,
                             const std::vector<size_t> &keyColumns,
                             size_t keyOrdinal,
                             std::string *key,
                             bool *hasKey,
                             std::string *error)
{
  if (!key || !hasKey)
    {
      setError(error, "missing local-lite unique key output");
      return false;
    }
  if (keyColumns.empty())
    {
      setError(error, "local-lite unique key has no columns");
      return false;
    }
  if (encoded.size() < sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1 ||
      memcmp(encoded.data(), LOCAL_LITE_BINARY_ROW_MAGIC,
             sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1) != 0)
    {
      setError(error, "invalid local-lite binary row payload");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t fullRowLen = 0;
  if (!computeLayout(table, &columns, &fullRowLen, error))
    return false;

  std::string storedRow =
    encoded.substr(sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  if (storedRow.empty() && fullRowLen > 0)
    {
      setError(error, "truncated local-lite binary row payload");
      return false;
    }

  key->clear();
  key->push_back('U');
  appendKeyUint64(key, static_cast<uint64_t>(keyOrdinal));
  appendKeyUint64(key, static_cast<uint64_t>(keyColumns.size()));
  for (size_t i = 0; i < keyColumns.size(); i++)
    {
      size_t sourceIndex = keyColumns[i];
      if (sourceIndex >= columns.size())
        {
          setError(error, "local-lite unique key column index out of range");
          return false;
        }

      const LocalLiteStoredColumn &col = columns[sourceIndex];
      if (storedColumnIsNull(storedRow, col))
        {
          *hasKey = false;
          key->clear();
          return true;
        }

      const char *src = NULL;
      size_t len = 0;
      if (col.type == LL_TYPE_VARCHAR)
        {
          if (!hasRange(storedRow, col.vcLenIndOffset,
                        ExpAlignedFormat::VARIABLE_LEN_SIZE))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          len = ExpAlignedFormat::getVarLength(
              const_cast<char *>(storedRow.data()) + col.vcLenIndOffset);
          if (!hasRange(storedRow, col.offset, len))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          src = storedRow.data() + col.offset;
        }
      else
        {
          len = col.length;
          if (!hasRange(storedRow, col.offset, len))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          src = storedRow.data() + col.offset;
        }

      appendKeyUint64(key, static_cast<uint64_t>(sourceIndex));
      appendKeyUint64(key, static_cast<uint64_t>(len));
      if (len > 0)
        key->append(src, len);
    }

  *hasKey = true;
  return true;
}

bool LocalLiteBuildUniqueKeyFromTextFields(
    const LocalLiteTableDef &table,
    const std::vector<size_t> &keyColumns,
    size_t keyOrdinal,
    const std::vector<std::string> &keyFields,
    std::string *key,
    bool *hasKey,
    std::string *error)
{
  if (keyFields.size() != keyColumns.size())
    {
      setError(error, "local-lite unique key field count mismatch");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;

  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());

  for (size_t i = 0; i < keyColumns.size(); i++)
    {
      size_t columnIndex = keyColumns[i];
      if (columnIndex >= columns.size())
        {
          setError(error, "local-lite unique key column index out of range");
          return false;
        }
      if (!writeValue(&row[0], columns[columnIndex], keyFields[i], error))
        return false;
    }

  UInt32 adjustedLen = ExpAlignedFormat::adjustDataLength(
      &row[0],
      static_cast<UInt32>(row.size()),
      ExpAlignedFormat::ALIGNMENT,
      TRUE);
  row.resize(adjustedLen);

  std::string encoded;
  encoded.assign(LOCAL_LITE_BINARY_ROW_MAGIC,
                 sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  encoded.append(row);
  return LocalLiteBuildUniqueKey(table, encoded, keyColumns, keyOrdinal,
                                 key, hasKey, error);
}

static bool copyStoredToDest(const std::string &storedRow,
                             const LocalLiteStoredColumn &source,
                             Attributes *dest,
                             char *destRow,
                             UInt32 *rowLen,
                             UInt32 *voaOffset,
                             UInt32 *lengthOffset,
                             UInt32 *dataOffset,
                             bool *firstVar,
                             ExpTupleDesc *destTd,
                             std::string *error)
{
  if (dest->getVCIndicatorLength() > 0 && *firstVar)
    {
      *voaOffset = dest->getVoaOffset();
      *lengthOffset = dest->getVCLenIndOffset();
      *dataOffset = *lengthOffset + dest->getVCIndicatorLength();
      *firstVar = false;
    }

  if (storedColumnIsNull(storedRow, source))
    {
      if (dest->getNullFlag())
        ExpTupleDesc::setNullValue(destRow + dest->getNullIndOffset(),
                                   dest->getNullBitIndex(),
                                   destTd->getTupleDataFormat());
      if (dest->getVCIndicatorLength() > 0)
        {
          ExpTupleDesc::setVoaValue(destRow, *voaOffset, *lengthOffset,
                                    dest->getVCIndicatorLength());
          dest->setVarLength(0, destRow + *lengthOffset);
          *lengthOffset = *dataOffset;
          *dataOffset = *lengthOffset + dest->getVCIndicatorLength();
          *rowLen = *lengthOffset;
          ExpAlignedFormat::incrVoaOffset(*voaOffset);
        }
      return true;
    }
  if (dest->getNullFlag())
    ExpTupleDesc::clearNullValue(destRow + dest->getNullIndOffset(),
                                 dest->getNullBitIndex(),
                                 destTd->getTupleDataFormat());

  if (dest->getVCIndicatorLength() > 0)
    {
      if (source.type != LL_TYPE_VARCHAR && source.type != LL_TYPE_CHAR)
        {
          setError(error, "local-lite binary projection type mismatch");
          return false;
        }
      size_t len = 0;
      const char *src = NULL;
      if (source.type == LL_TYPE_VARCHAR)
        {
          if (!hasRange(storedRow, source.vcLenIndOffset,
                        ExpAlignedFormat::VARIABLE_LEN_SIZE))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          len = ExpAlignedFormat::getVarLength(
              const_cast<char *>(storedRow.data()) + source.vcLenIndOffset);
          if (!hasRange(storedRow, source.offset, len))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          src = storedRow.data() + source.offset;
        }
      else
        {
          len = source.length;
          if (!hasRange(storedRow, source.offset, len))
            {
              setError(error, "truncated local-lite binary row payload");
              return false;
            }
          src = storedRow.data() + source.offset;
        }
      if (len > static_cast<size_t>(dest->getLength()))
        len = dest->getLength();
      ExpTupleDesc::setVoaValue(destRow, *voaOffset, *lengthOffset,
                                dest->getVCIndicatorLength());
      dest->setVarLength(static_cast<UInt32>(len), destRow + *lengthOffset);
      if (len > 0)
        str_cpy_all(destRow + *dataOffset, src, len);
      *lengthOffset = *dataOffset + static_cast<UInt32>(len);
      *dataOffset = *lengthOffset + dest->getVCIndicatorLength();
      *rowLen = *lengthOffset;
      ExpAlignedFormat::incrVoaOffset(*voaOffset);
      return true;
    }

  char *target = destRow + dest->getOffset();
  if (source.type != LL_TYPE_VARCHAR &&
      !hasRange(storedRow, source.offset, source.length))
    {
      setError(error, "truncated local-lite binary row payload");
      return false;
    }
  const char *src = storedRow.data() + source.offset;
  switch (dest->getDatatype())
    {
    case REC_BIN8_SIGNED:
    case REC_BIN8_UNSIGNED:
    case REC_BIN16_SIGNED:
    case REC_BIN16_UNSIGNED:
    case REC_BIN32_SIGNED:
    case REC_BIN32_UNSIGNED:
    case REC_BIN64_SIGNED:
    case REC_BIN64_UNSIGNED:
    case REC_FLOAT32:
    case REC_FLOAT64:
    case REC_DATETIME:
    case REC_DECIMAL_UNSIGNED:
    case REC_DECIMAL_LSE:
    case REC_NUM_BIG_UNSIGNED:
    case REC_NUM_BIG_SIGNED:
      {
        size_t len = source.length < static_cast<size_t>(dest->getLength())
                       ? source.length
                       : static_cast<size_t>(dest->getLength());
        str_cpy_all(target, src, len);
        return true;
      }
    case REC_BYTE_F_ASCII:
      {
        str_pad(target, dest->getLength(), ' ');
        size_t len = 0;
        if (source.type == LL_TYPE_VARCHAR)
          {
            if (!hasRange(storedRow, source.vcLenIndOffset,
                          ExpAlignedFormat::VARIABLE_LEN_SIZE))
              {
                setError(error, "truncated local-lite binary row payload");
                return false;
              }
            len = ExpAlignedFormat::getVarLength(
                const_cast<char *>(storedRow.data()) + source.vcLenIndOffset);
            if (!hasRange(storedRow, source.offset, len))
              {
                setError(error, "truncated local-lite binary row payload");
                return false;
              }
          }
        else
          len = source.length;
        if (len > static_cast<size_t>(dest->getLength()))
          len = dest->getLength();
        if (len > 0)
          str_cpy_all(target, src, len);
        return true;
      }
    default:
      setError(error, "unsupported local-lite binary projection column type");
      return false;
    }
}

bool LocalLiteProjectBinaryRow(const LocalLiteTableDef &table,
                               const std::string &encoded,
                               const std::vector<size_t> &sourceIndexes,
                               ExpTupleDesc *destTd,
                               char *destRow,
                               size_t destRowLen,
                               unsigned int *formattedLen,
                               std::string *error)
{
  if (!destTd || !destRow)
    {
      setError(error, "local-lite binary projection missing destination row");
      return false;
    }
  if (encoded.size() < sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1 ||
      memcmp(encoded.data(), LOCAL_LITE_BINARY_ROW_MAGIC,
             sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1) != 0)
    {
      setError(error, "invalid local-lite binary row payload");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t fullRowLen = 0;
  if (!computeLayout(table, &columns, &fullRowLen, error))
    return false;

  std::string storedRow =
    encoded.substr(sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  if (storedRow.empty() && fullRowLen > 0)
    {
      setError(error, "truncated local-lite binary row payload");
      return false;
    }

  if (sourceIndexes.size() != destTd->numAttrs())
    {
      setError(error, "local-lite binary projection does not match tuple descriptor");
      return false;
    }

  str_pad(destRow, destRowLen, '\0');
  UInt32 firstFixed = destRowLen;
  UInt32 bitmap = 0;
  for (UInt32 i = 0; i < destTd->numAttrs(); i++)
    {
      Attributes *attr = destTd->getAttr(i);
      if (!attr)
        continue;
      if (attr->getVCIndicatorLength() == 0 &&
          static_cast<UInt32>(attr->getOffset()) < firstFixed)
        firstFixed = attr->getOffset();
      if (attr->getNullFlag() && attr->getNullIndOffset() > 0)
        bitmap = attr->getNullIndOffset();
    }
  if (firstFixed == destRowLen)
    firstFixed = ExpAlignedFormat::getHdrSize();
  ExpTupleDesc::setFirstFixedOffset(destRow, firstFixed,
                                    destTd->getTupleDataFormat());
  ExpAlignedFormat::setBitmapOffset(destRow + ExpAlignedFormat::OFFSET_SIZE,
                                    bitmap);

  UInt32 rowLen = destTd->tupleDataLength();
  UInt32 voaOffset = 0;
  UInt32 lengthOffset = 0;
  UInt32 dataOffset = 0;
  bool firstVar = true;
  for (UInt32 i = 0; i < destTd->numAttrs(); i++)
    {
      size_t sourceIndex = sourceIndexes[i];
      if (sourceIndex >= columns.size())
        {
          setError(error, "local-lite binary projection source index out of range");
          return false;
        }
      Attributes *dest = destTd->getAttr(i);
      if (!copyStoredToDest(storedRow, columns[sourceIndex], dest, destRow,
                            &rowLen, &voaOffset, &lengthOffset, &dataOffset,
                            &firstVar, destTd, error))
        return false;
    }

  rowLen = ExpAlignedFormat::adjustDataLength(destRow, rowLen,
                                              ExpAlignedFormat::ALIGNMENT,
                                              TRUE);
  *formattedLen = rowLen;
  return true;
}

#endif
