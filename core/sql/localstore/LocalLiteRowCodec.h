// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LOCAL_LITE_ROW_CODEC_H
#define LOCAL_LITE_ROW_CODEC_H

#ifdef TRAF_LOCAL_LITE

#include <stdint.h>
#include <string>
#include <vector>

class ExpTupleDesc;
struct LocalLiteTableDef;
struct LocalLiteIndexDef;

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

bool LocalLiteApplyBinaryUpdate(
    const LocalLiteTableDef &table,
    const std::string &original,
    ExpTupleDesc *srcTd,
    const char *srcRow,
    size_t srcRowLen,
    const std::vector<size_t> &updatedColumnIndexes,
    std::string *encoded,
    std::string *error);

// Rebuild a stored row after a catalog-only column change.  Each entry in
// newToOldColumn maps a column in newTable to its oldTable column, or -1 for
// a newly added column.  Added columns are initialized from addedValues.
bool LocalLiteRebuildBinaryRow(
    const LocalLiteTableDef &oldTable,
    const LocalLiteTableDef &newTable,
    const std::string &original,
    const std::vector<int> &newToOldColumn,
    const std::vector<std::string> &addedValues,
    std::string *encoded,
    std::string *error);

bool LocalLiteProjectBinaryRow(const LocalLiteTableDef &table,
                               const std::string &encoded,
                               uint64_t syntheticRowId,
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

// Build an ordinal-independent key for comparing RI columns across tables.
// A NULL component returns hasKey=false, implementing MATCH SIMPLE.
bool LocalLiteBuildConstraintKey(const LocalLiteTableDef &table,
                                 const std::string &encoded,
                                 const std::vector<size_t> &keyColumns,
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

bool LocalLiteBuildSecondaryIndexPrefixFromTextFields(
    const LocalLiteTableDef &table,
    const std::vector<size_t> &keyColumns,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error);

bool LocalLiteSecondaryIndexSupportsOrderedKeys(
    const LocalLiteTableDef &table,
    const std::vector<size_t> &keyColumns);

bool LocalLiteBuildOrderedSecondaryKeyPayload(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::string &encodedRow,
    std::string *payload,
    bool *hasKey,
    bool *containsNull,
    std::string *error);

bool LocalLiteBuildOrderedSecondaryKeyPrefixFromTextFields(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error);

bool LocalLiteBuildOrderedSecondaryNullPrefixFromTextFields(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error);

bool LocalLiteBuildOrderedSecondaryNullablePrefixFromTextFields(
    const LocalLiteTableDef &table,
    const LocalLiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    const std::vector<bool> &nullFields,
    std::string *payloadPrefix,
    std::string *error);

#endif

#endif
