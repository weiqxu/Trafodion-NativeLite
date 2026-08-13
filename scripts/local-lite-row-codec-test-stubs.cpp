// Test-only fallbacks for standalone LocalLiteRocksDBStore probes.
//
// The focused store probes provide their own primary/UNIQUE key codecs. They
// do not link the executor-dependent LocalLiteRowCodec implementation, but the
// store object still references the broader codec surface. Fail loudly if a
// probe unexpectedly reaches one of those unsupported paths.

#include "LocalLiteRocksDBStore.h"

static bool unsupportedCodec(std::string *error, const char *operation)
{
  if (error)
    *error = std::string(operation) +
             " is not available in the standalone local-lite store probe";
  return false;
}

bool LocalLiteEncodeBinaryRow(const LocalLiteTableDef &,
                              const std::vector<std::string> &,
                              std::string *,
                              std::string *error)
{
  return unsupportedCodec(error, "binary row encoding");
}

bool LocalLiteRebuildBinaryRow(const LocalLiteTableDef &,
                               const LocalLiteTableDef &,
                               const std::string &,
                               const std::vector<int> &,
                               const std::vector<std::string> &,
                               std::string *,
                               std::string *error)
{
  return unsupportedCodec(error, "binary row rebuild");
}

bool LocalLiteBuildConstraintKey(const LocalLiteTableDef &,
                                 const std::string &,
                                 const std::vector<size_t> &,
                                 std::string *,
                                 bool *hasKey,
                                 std::string *error)
{
  if (hasKey)
    *hasKey = false;
  return unsupportedCodec(error, "constraint key encoding");
}

bool LocalLiteBinaryRowIsNull(const LocalLiteTableDef &,
                              const std::string &,
                              size_t,
                              bool *,
                              std::string *error)
{
  return unsupportedCodec(error, "binary NULL inspection");
}
