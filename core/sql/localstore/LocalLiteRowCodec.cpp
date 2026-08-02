// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteRowCodec.h"

#include "BigNumHelper.h"
#include "DatetimeType.h"
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
  bool unsignedNumeric;
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
                    bool *decimalStorage,
                    bool *unsignedNumeric)
{
  *scale = 0;
  *precisionOut = 0;
  *decimalStorage = false;
  std::string typeName = upper(typeText);
  *unsignedNumeric = typeName.find("UNSIGNED") != std::string::npos;
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
  if (startsWithWord(typeName, "REAL"))
    {
      *type = LL_TYPE_FLOAT32;
      *length = 4;
      *precisionOut = *length;
      return true;
    }
  if (startsWithWord(typeName, "FLOAT"))
    {
      size_t precision = typeArg(typeName, 54);
      if (precision < 1 || precision > 54)
        return false;
      *type = precision <= 22 ? LL_TYPE_FLOAT32 : LL_TYPE_FLOAT64;
      *length = precision <= 22 ? 4 : 8;
      *precisionOut = precision;
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
      *scale = typeArg(typeName, 0);
      return true;
    }
  if (startsWithWord(typeName, "TIMESTAMP"))
    {
      *type = LL_TYPE_DATETIME;
      *length = 11;
      *precisionOut = *length;
      *scale = typeArg(typeName, 6);
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
                   &col.precision, &col.scale, &col.decimalStorage,
                   &col.unsignedNumeric))
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
        rec_datetime_field startField = col.length == 8
            ? REC_DATE_HOUR : REC_DATE_YEAR;
        rec_datetime_field endField = col.length == 4
            ? REC_DATE_DAY : REC_DATE_SECOND;
        UInt32 actualFractionPrecision = 0;
        DatetimeValue datetime(value.c_str(), startField, endField,
                               actualFractionPrecision, FALSE);
        if (!datetime.isValid())
          {
            setError(error, "invalid local-lite datetime literal");
            return false;
          }
        const unsigned char *source = datetime.getValue();
        size_t baseLength = startField == REC_DATE_YEAR ? 7 : 3;
        if (endField == REC_DATE_DAY)
          baseLength = 4;
        if (datetime.getValueLen() < baseLength ||
            baseLength > col.length)
          {
            setError(error, "invalid local-lite datetime storage");
            return false;
          }
        memcpy(target, source, baseLength);
        if (endField == REC_DATE_SECOND && col.length >= baseLength + 4)
          {
            UInt32 fraction = 0;
            if (datetime.getValueLen() >= baseLength + 4)
              memcpy(&fraction, source + baseLength, sizeof(fraction));
            while (actualFractionPrecision < col.scale)
              {
                fraction *= 10;
                actualFractionPrecision++;
              }
            while (actualFractionPrecision > col.scale)
              {
                fraction /= 10;
                actualFractionPrecision--;
              }
            memcpy(target + baseLength, &fraction, sizeof(fraction));
          }
        return true;
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

static bool hasRange(size_t rowLen, size_t offset, size_t length)
{
  return offset <= rowLen && length <= rowLen - offset;
}

static bool hasRange(const std::string &row, size_t offset, size_t length)
{
  return hasRange(row.size(), offset, length);
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
      // In SQLMX_ALIGNED_FORMAT only the first VARCHAR has a direct data
      // offset. Later VARCHAR attributes are indirect and keep their actual
      // length-indicator offset in the row's VOA. Resolve both forms through
      // the tuple helper instead of treating ExpOffsetMax as a data offset.
      if (srcAttr->getOffset() == ExpOffsetMax &&
          !hasRange(srcRowLen, srcAttr->getVoaOffset(),
                    ExpVoaSize))
        {
          setError(error, "truncated local-lite executor row VOA");
          return false;
        }
      srcOffset = ExpTupleDesc::getVarOffset(
          const_cast<char *>(srcRow),
          srcAttr->getOffset(),
          srcAttr->getVoaOffset(),
          srcAttr->getVCIndicatorLength(),
          srcAttr->getNullIndicatorLength());
      if (!hasRange(srcRowLen, srcOffset,
                    srcAttr->getVCIndicatorLength()))
        {
          setError(error, "truncated local-lite executor row");
          return false;
        }
      srcLen = srcAttr->getLength(srcRow + srcOffset);
      srcOffset += srcAttr->getVCIndicatorLength();
    }

  if (!hasRange(srcRowLen, srcOffset, srcLen))
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
  UInt32 sourceOffset = 0;
  if (table.primaryKeyColumns.empty() &&
      srcTd->numAttrs() == table.columns.size() + 1)
    // Storage allocates SYSKEY, so LLBR1 contains catalog user columns only.
    sourceOffset = 1;
  else if (srcTd->numAttrs() != table.columns.size())
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

  for (UInt32 i = 0; i < table.columns.size(); i++)
    {
      if (!copyAttrToCanonical(srcRow, srcRowLen,
                               srcTd->getAttr(i + sourceOffset), srcTd,
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

bool LocalLiteApplyBinaryUpdate(
    const LocalLiteTableDef &table,
    const std::string &original,
    ExpTupleDesc *srcTd,
    const char *srcRow,
    size_t srcRowLen,
    const std::vector<size_t> &updatedColumnIndexes,
    std::string *encoded,
    std::string *error)
{
  if (!srcTd || !srcRow || !encoded)
    {
      setError(error, "local-lite update missing executor row");
      return false;
    }
  if (srcTd->numAttrs() != updatedColumnIndexes.size())
    {
      setError(error,
               "local-lite update row does not match updated column count");
      return false;
    }
  if (original.size() < sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1 ||
      memcmp(original.data(), LOCAL_LITE_BINARY_ROW_MAGIC,
             sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1) != 0)
    {
      setError(error, "invalid local-lite binary row payload");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t fullRowLen = 0;
  if (!computeLayout(table, &columns, &fullRowLen, error))
    return false;

  std::string row =
    original.substr(sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  if (row.size() < fullRowLen)
    {
      setError(error, "truncated local-lite binary row payload");
      return false;
    }

  for (UInt32 i = 0; i < srcTd->numAttrs(); i++)
    {
      size_t columnIndex = updatedColumnIndexes[i];
      if (columnIndex >= columns.size())
        {
          setError(error, "local-lite updated column index out of range");
          return false;
        }
      if (!copyAttrToCanonical(srcRow, srcRowLen, srcTd->getAttr(i), srcTd,
                               &row[0], columns[columnIndex], error))
        return false;
    }

  UInt32 adjustedLen = ExpAlignedFormat::adjustDataLength(
      &row[0], static_cast<UInt32>(row.size()),
      ExpAlignedFormat::ALIGNMENT, TRUE);
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

bool LocalLiteBuildSecondaryIndexPrefixFromTextFields(
    const LocalLiteTableDef &table,
    const std::vector<size_t> &keyColumns,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error)
{
  if (!payloadPrefix)
    {
      setError(error, "missing local-lite secondary index prefix output");
      return false;
    }
  if (keyColumns.empty() || leadingKeyFields.empty() ||
      leadingKeyFields.size() > keyColumns.size())
    {
      setError(error, "invalid local-lite secondary index prefix fields");
      return false;
    }

  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;

  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      size_t columnIndex = keyColumns[i];
      if (columnIndex >= columns.size())
        {
          setError(error, "local-lite secondary index column out of range");
          return false;
        }
      if (!writeValue(&row[0], columns[columnIndex], leadingKeyFields[i],
                      error))
        return false;
    }

  payloadPrefix->clear();
  appendKeyUint64(payloadPrefix, static_cast<uint64_t>(keyColumns.size()));
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      size_t sourceIndex = keyColumns[i];
      const LocalLiteStoredColumn &col = columns[sourceIndex];
      const char *src = NULL;
      size_t len = 0;
      if (col.type == LL_TYPE_VARCHAR)
        {
          len = ExpAlignedFormat::getVarLength(
              &row[0] + col.vcLenIndOffset);
          src = row.data() + col.offset;
        }
      else
        {
          len = col.length;
          src = row.data() + col.offset;
        }
      appendKeyUint64(payloadPrefix, static_cast<uint64_t>(sourceIndex));
      appendKeyUint64(payloadPrefix, static_cast<uint64_t>(len));
      if (len > 0)
        payloadPrefix->append(src, len);
    }
  return true;
}

static bool orderedIndexColumnSupported(const LocalLiteStoredColumn &column)
{
  switch (column.type)
    {
    case LL_TYPE_INT8:
    case LL_TYPE_INT16:
    case LL_TYPE_INT32:
    case LL_TYPE_INT64:
    case LL_TYPE_FLOAT32:
    case LL_TYPE_FLOAT64:
    case LL_TYPE_CHAR:
    case LL_TYPE_VARCHAR:
    case LL_TYPE_DATETIME:
      return true;
    case LL_TYPE_NUMERIC:
      return true;
    default:
      return false;
    }
}

static void appendOrderedEscapedByte(std::string *out, unsigned char value)
{
  if (value == 0)
    {
      out->push_back('\0');
      out->push_back(static_cast<char>(0xff));
    }
  else
    out->push_back(static_cast<char>(value));
}

static bool appendOrderedDecimalDigits(const std::string &storedRow,
                                       const LocalLiteStoredColumn &column,
                                       std::string *component,
                                       std::string *error)
{
  std::string digits;
  bool negative = false;
  if (column.decimalStorage)
    {
      if (!hasRange(storedRow, column.offset, column.length) ||
          column.length != column.precision)
        {
          setError(error, "invalid local-lite ordered decimal value");
          return false;
        }
      digits.assign(storedRow.data() + column.offset, column.length);
      negative = (static_cast<unsigned char>(digits[0]) & 0x80) != 0;
      digits[0] = static_cast<char>(
          static_cast<unsigned char>(digits[0]) & 0x7f);
    }
  else
    {
      if (!hasRange(storedRow, column.offset, column.length) ||
          column.precision <= 18)
        {
          setError(error, "invalid local-lite ordered BigNum value");
          return false;
        }
      std::string ascii(column.precision + 1, '0');
      if (BigNumHelper::ConvBigNumWithSignToAsciiHelper(
              static_cast<Lng32>(column.length),
              static_cast<Lng32>(ascii.size()),
              const_cast<char *>(storedRow.data()) + column.offset,
              &ascii[0], NULL) != 0)
        {
          setError(error, "invalid local-lite ordered BigNum value");
          return false;
        }
      negative = ascii[0] == '-';
      digits.assign(ascii.data() + 1, column.precision);
    }

  bool zero = true;
  for (size_t i = 0; i < digits.size(); i++)
    if (digits[i] != '0')
      zero = false;
  if (zero)
    negative = false;
  appendOrderedEscapedByte(component, negative ? 1 : 2);
  for (size_t i = 0; i < digits.size(); i++)
    {
      unsigned char digit = static_cast<unsigned char>(digits[i]);
      if (digit < '0' || digit > '9')
        {
          setError(error, "invalid local-lite ordered decimal digit");
          return false;
        }
      if (negative)
        digit = static_cast<unsigned char>('9' - (digit - '0'));
      appendOrderedEscapedByte(component, digit);
    }
  return true;
}

static bool appendOrderedDatetime(const std::string &storedRow,
                                  const LocalLiteStoredColumn &column,
                                  std::string *component,
                                  std::string *error)
{
  if (!hasRange(storedRow, column.offset, column.length) ||
      (column.length != 4 && column.length != 8 && column.length != 11))
    {
      setError(error, "invalid local-lite ordered datetime value");
      return false;
    }
  const char *source = storedRow.data() + column.offset;
  size_t pos = 0;
  if (column.length != 8)
    {
      UInt16 year = 0;
      memcpy(&year, source, sizeof(year));
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>((year >> 8) & 0xff));
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>(year & 0xff));
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>(source[2]));
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>(source[3]));
      pos = 4;
    }
  if (column.length != 4)
    {
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>(source[pos]));
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>(source[pos + 1]));
      appendOrderedEscapedByte(component,
          static_cast<unsigned char>(source[pos + 2]));
      UInt32 fraction = 0;
      memcpy(&fraction, source + pos + 3, sizeof(fraction));
      for (int shift = 24; shift >= 0; shift -= 8)
        appendOrderedEscapedByte(component,
            static_cast<unsigned char>((fraction >> shift) & 0xff));
    }
  return true;
}

static bool appendOrderedIndexComponent(
    const std::string &storedRow,
    const LocalLiteStoredColumn &column,
    bool descending,
    std::string *out,
    bool *hasKey,
    bool *containsNull,
    std::string *error)
{
  if (storedColumnIsNull(storedRow, column))
    {
      std::string component(1, '\0');
      if (descending)
        component[0] = static_cast<char>(0xff);
      *out += component;
      *hasKey = true;
      if (containsNull)
        *containsNull = true;
      return true;
    }
  if (!orderedIndexColumnSupported(column))
    {
      setError(error, "unsupported ordered local-lite index column type");
      return false;
    }

  std::string component;
  component.push_back('\1');
  if (column.type == LL_TYPE_VARCHAR || column.type == LL_TYPE_CHAR)
    {
      size_t len = column.type == LL_TYPE_VARCHAR
          ? ExpAlignedFormat::getVarLength(
                const_cast<char *>(storedRow.data()) + column.vcLenIndOffset)
          : column.length;
      if (!hasRange(storedRow, column.offset, len))
        {
          setError(error, "truncated local-lite ordered index value");
          return false;
        }
      for (size_t i = 0; i < len; i++)
        appendOrderedEscapedByte(
            &component,
            static_cast<unsigned char>(storedRow[column.offset + i]));
    }
  else if (column.type == LL_TYPE_FLOAT32 ||
           column.type == LL_TYPE_FLOAT64)
    {
      if (!hasRange(storedRow, column.offset, column.length) ||
          (column.length != 4 && column.length != 8))
        {
          setError(error, "invalid local-lite ordered floating index value");
          return false;
        }
      uint64_t bits = 0;
      memcpy(&bits, storedRow.data() + column.offset, column.length);
      uint64_t signBit = static_cast<uint64_t>(1)
          << (static_cast<unsigned int>(column.length * 8) - 1);
      uint64_t valueMask = column.length == 8
          ? ~static_cast<uint64_t>(0)
          : (static_cast<uint64_t>(1) << 32) - 1;
      if ((bits & ~signBit) == 0)
        bits = 0;
      bits = (bits & signBit) ? (~bits & valueMask) : (bits ^ signBit);
      for (size_t i = column.length; i > 0; i--)
        appendOrderedEscapedByte(
            &component,
            static_cast<unsigned char>((bits >> ((i - 1) * 8)) & 0xff));
    }
  else if (column.type == LL_TYPE_DATETIME)
    {
      if (!appendOrderedDatetime(storedRow, column, &component, error))
        return false;
    }
  else if (column.type == LL_TYPE_NUMERIC &&
           (column.decimalStorage || column.precision > 18))
    {
      if (!appendOrderedDecimalDigits(storedRow, column, &component, error))
        return false;
    }
  else
    {
      if (!hasRange(storedRow, column.offset, column.length) ||
          column.length == 0 || column.length > 8)
        {
          setError(error, "invalid local-lite ordered numeric index value");
          return false;
        }
      uint64_t bits = 0;
      memcpy(&bits, storedRow.data() + column.offset, column.length);
      unsigned int width = static_cast<unsigned int>(column.length * 8);
      if (!column.unsignedNumeric)
        bits ^= static_cast<uint64_t>(1) << (width - 1);
      for (size_t i = column.length; i > 0; i--)
        appendOrderedEscapedByte(
            &component,
            static_cast<unsigned char>((bits >> ((i - 1) * 8)) & 0xff));
    }
  component.push_back('\0');
  component.push_back('\0');
  if (descending)
    for (size_t i = 0; i < component.size(); i++)
      component[i] = static_cast<char>(
          ~static_cast<unsigned char>(component[i]));
  *out += component;
  *hasKey = true;
  return true;
}

bool LocalLiteSecondaryIndexSupportsOrderedKeys(
    const LocalLiteTableDef &table,
    const std::vector<size_t> &keyColumns)
{
  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  std::string error;
  if (keyColumns.empty() ||
      !computeLayout(table, &columns, &rowLen, &error))
    return false;
  for (size_t i = 0; i < keyColumns.size(); i++)
    if (keyColumns[i] >= columns.size() ||
        !orderedIndexColumnSupported(columns[keyColumns[i]]))
      return false;
  return true;
}

bool LocalLiteBuildOrderedSecondaryKeyPayload(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::string &encodedRow,
    std::string *payload,
    bool *hasKey,
    bool *containsNull,
    std::string *error)
{
  if (!payload || !hasKey || !containsNull ||
      encodedRow.size() < sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1 ||
      memcmp(encodedRow.data(), LOCAL_LITE_BINARY_ROW_MAGIC,
             sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1) != 0)
    {
      setError(error, "invalid local-lite ordered index input");
      return false;
    }
  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;
  std::string storedRow =
      encodedRow.substr(sizeof(LOCAL_LITE_BINARY_ROW_MAGIC) - 1);
  payload->clear();
  *hasKey = true;
  *containsNull = false;
  for (size_t i = 0; i < index.keyColumns.size(); i++)
    {
      size_t column = index.keyColumns[i];
      bool componentHasKey = true;
      if (column >= columns.size() ||
          !appendOrderedIndexComponent(
              storedRow, columns[column],
              i < index.descending.size() && index.descending[i],
              payload, &componentHasKey, containsNull, error))
        return false;
      if (!componentHasKey)
        {
          payload->clear();
          *hasKey = false;
          return true;
        }
    }
  return true;
}

bool LocalLiteBuildOrderedSecondaryKeyPrefixFromTextFields(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error)
{
  if (!payloadPrefix || leadingKeyFields.empty() ||
      leadingKeyFields.size() > index.keyColumns.size())
    {
      setError(error, "invalid local-lite ordered index prefix fields");
      return false;
    }
  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;
  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      size_t column = index.keyColumns[i];
      if (column >= columns.size() ||
          !writeValue(&row[0], columns[column], leadingKeyFields[i], error))
        return false;
    }

  payloadPrefix->clear();
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      bool hasKey = true;
      size_t column = index.keyColumns[i];
      if (!appendOrderedIndexComponent(
              row, columns[column],
              i < index.descending.size() && index.descending[i],
              payloadPrefix, &hasKey, NULL, error) || !hasKey)
        return false;
    }
  return true;
}

bool LocalLiteBuildOrderedSecondaryNullPrefixFromTextFields(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error)
{
  if (!payloadPrefix ||
      leadingKeyFields.size() >= index.keyColumns.size())
    {
      setError(error, "invalid local-lite ordered NULL index prefix");
      return false;
    }
  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;
  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      size_t column = index.keyColumns[i];
      if (column >= columns.size() ||
          !writeValue(&row[0], columns[column], leadingKeyFields[i], error))
        return false;
    }
  size_t nullColumn = index.keyColumns[leadingKeyFields.size()];
  if (nullColumn >= columns.size() || !columns[nullColumn].nullable ||
      !writeValue(&row[0], columns[nullColumn], std::string(), error))
    return false;

  payloadPrefix->clear();
  for (size_t i = 0; i <= leadingKeyFields.size(); i++)
    {
      bool hasKey = true;
      size_t column = index.keyColumns[i];
      if (!appendOrderedIndexComponent(
              row, columns[column],
              i < index.descending.size() && index.descending[i],
              payloadPrefix, &hasKey, NULL, error) || !hasKey)
        return false;
    }
  return true;
}

bool LocalLiteBuildOrderedSecondaryNullablePrefixFromTextFields(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    const std::vector<bool> &nullFields,
    std::string *payloadPrefix,
    std::string *error)
{
  if (!payloadPrefix || leadingKeyFields.empty() ||
      leadingKeyFields.size() != nullFields.size() ||
      leadingKeyFields.size() > index.keyColumns.size())
    {
      setError(error, "invalid local-lite ordered nullable index prefix");
      return false;
    }
  std::vector<LocalLiteStoredColumn> columns;
  size_t rowLen = 0;
  if (!computeLayout(table, &columns, &rowLen, error))
    return false;
  std::string row(rowLen, '\0');
  initializeCanonicalRow(columns, &row[0], row.size());
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      size_t column = index.keyColumns[i];
      if (column >= columns.size() ||
          (nullFields[i] && !columns[column].nullable) ||
          !writeValue(&row[0], columns[column],
                      nullFields[i] ? std::string() : leadingKeyFields[i],
                      error))
        return false;
    }

  payloadPrefix->clear();
  for (size_t i = 0; i < leadingKeyFields.size(); i++)
    {
      bool hasKey = true;
      size_t column = index.keyColumns[i];
      if (!appendOrderedIndexComponent(
              row, columns[column],
              i < index.descending.size() && index.descending[i],
              payloadPrefix, &hasKey, NULL, error) || !hasKey)
        return false;
    }
  return true;
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
                               uint64_t syntheticRowId,
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
      Attributes *dest = destTd->getAttr(i);
      if (table.primaryKeyColumns.empty() &&
          sourceIndex == columns.size())
        {
          if (!dest || dest->getLength() < sizeof(Int64))
            {
              setError(error, "local-lite SYSKEY destination is invalid");
              return false;
            }
          Int64 syskey = static_cast<Int64>(syntheticRowId);
          str_cpy_all(destRow + dest->getOffset(),
                      reinterpret_cast<const char *>(&syskey),
                      sizeof(syskey));
          continue;
        }
      // Logical local-lite UNIQUE indexes are scanned from the base row.
      // Generator metadata can still describe the covering index tuple, whose
      // trailing base-column ordinals are one slot past the base row layout.
      if (sourceIndex >= columns.size() && sourceIndex > 0)
        sourceIndex--;
      if (sourceIndex >= columns.size())
        {
          char msg[160];
          snprintf(msg, sizeof(msg),
                   "local-lite binary projection source index out of range: %lu of %lu",
                   static_cast<unsigned long>(sourceIndex),
                   static_cast<unsigned long>(columns.size()));
          setError(error, msg);
          return false;
        }
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
