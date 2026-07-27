// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_ROW_CODEC_H
#define LOCAL_LITE_ROW_CODEC_H

#ifdef TRAF_LOCAL_LITE

#include <string>
#include <vector>

class ExpTupleDesc;
struct LocalLiteTableDef;

bool LocalLiteEncodeBinaryRow(const LocalLiteTableDef &table,
                              const std::vector<std::string> &fields,
                              std::string *encoded,
                              std::string *error);

bool LocalLiteWrapBinaryRow(const char *row,
                            size_t rowLen,
                            std::string *encoded,
                            std::string *error);

bool LocalLiteNormalizeBinaryRow(const LocalLiteTableDef &table,
                                 ExpTupleDesc *srcTd,
                                 const char *srcRow,
                                 size_t srcRowLen,
                                 std::string *encoded,
                                 std::string *error);

bool LocalLiteProjectBinaryRow(const LocalLiteTableDef &table,
                               const std::string &encoded,
                               const std::vector<size_t> &sourceIndexes,
                               ExpTupleDesc *destTd,
                               char *destRow,
                               size_t destRowLen,
                               unsigned int *formattedLen,
                               std::string *error);

bool LocalLiteBuildPrimaryKey(const LocalLiteTableDef &table,
                              const std::string &encoded,
                              std::string *key,
                              std::string *error);

bool LocalLiteBuildPrimaryKeyFromTextFields(
    const LocalLiteTableDef &table,
    const std::vector<std::string> &keyFields,
    std::string *key,
    std::string *error);

bool LocalLiteBuildUniqueKey(const LocalLiteTableDef &table,
                             const std::string &encoded,
                             const std::vector<size_t> &keyColumns,
                             size_t keyOrdinal,
                             std::string *key,
                             bool *hasKey,
                             std::string *error);

bool LocalLiteBuildUniqueKeyFromTextFields(
    const LocalLiteTableDef &table,
    const std::vector<size_t> &keyColumns,
    size_t keyOrdinal,
    const std::vector<std::string> &keyFields,
    std::string *key,
    bool *hasKey,
    std::string *error);

#endif

#endif
