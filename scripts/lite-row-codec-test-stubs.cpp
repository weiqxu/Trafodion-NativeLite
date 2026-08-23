// Test-only fallbacks for standalone LiteRocksDBStore probes.
//
// The focused store probes provide their own primary/UNIQUE key codecs. They
// do not link the executor-dependent LiteRowCodec implementation, but the
// store object still references the broader codec surface. Fail loudly if a
// probe unexpectedly reaches one of those unsupported paths.

#include "LiteRocksDBStore.h"

static bool unsupportedCodec(std::string *error, const char *operation)
{
  if (error)
    *error = std::string(operation) +
             " is not available in the standalone lite store probe";
  return false;
}

bool LiteEncodeBinaryRow(const LiteTableDef &,
                              const std::vector<std::string> &,
                              std::string *,
                              std::string *error)
{
  return unsupportedCodec(error, "binary row encoding");
}

bool LiteRebuildBinaryRow(const LiteTableDef &,
                               const LiteTableDef &,
                               const std::string &,
                               const std::vector<int> &,
                               const std::vector<std::string> &,
                               std::string *,
                               std::string *error)
{
  return unsupportedCodec(error, "binary row rebuild");
}

bool LiteBuildConstraintKey(const LiteTableDef &,
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

bool LiteBinaryRowIsNull(const LiteTableDef &,
                              const std::string &,
                              size_t,
                              bool *,
                              std::string *error)
{
  return unsupportedCodec(error, "binary NULL inspection");
}
