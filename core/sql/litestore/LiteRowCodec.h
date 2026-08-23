// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information.
// @@@ END COPYRIGHT @@@

#ifndef LITE_ROW_CODEC_H
#define LITE_ROW_CODEC_H

#ifdef TRAF_LITE

#include <stdint.h>
#include <string>
#include <vector>

class ExpTupleDesc;
struct LiteTableDef;
struct LiteIndexDef;

bool LiteEncodeBinaryRow(const LiteTableDef &table,
                              const std::vector<std::string> &fields,
                              std::string *encoded,
                              std::string *error);

bool LiteWrapBinaryRow(const char *row,
                            size_t rowLen,
                            std::string *encoded,
                            std::string *error);

bool LiteNormalizeBinaryRow(const LiteTableDef &table,
                                 ExpTupleDesc *srcTd,
                                 const char *srcRow,
                                 size_t srcRowLen,
                                 std::string *encoded,
                                 std::string *error);

bool LiteApplyBinaryUpdate(
    const LiteTableDef &table,
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
bool LiteRebuildBinaryRow(
    const LiteTableDef &oldTable,
    const LiteTableDef &newTable,
    const std::string &original,
    const std::vector<int> &newToOldColumn,
    const std::vector<std::string> &addedValues,
    std::string *encoded,
    std::string *error);

bool LiteProjectBinaryRow(const LiteTableDef &table,
                               const std::string &encoded,
                               uint64_t syntheticRowId,
                               const std::vector<size_t> &sourceIndexes,
                               ExpTupleDesc *destTd,
                               char *destRow,
                               size_t destRowLen,
                               unsigned int *formattedLen,
                               std::string *error);

bool LiteBuildPrimaryKey(const LiteTableDef &table,
                              const std::string &encoded,
                              std::string *key,
                              std::string *error);

bool LiteBuildPrimaryKeyFromTextFields(
    const LiteTableDef &table,
    const std::vector<std::string> &keyFields,
    std::string *key,
    std::string *error);

bool LiteBuildPrimaryKeyFromExecutorTuple(
    const LiteTableDef &table,
    ExpTupleDesc *keyTd,
    const char *keyRow,
    size_t keyRowLen,
    std::string *key,
    std::string *error);

bool LiteBuildPrimaryKeyPrefixFromTextFields(
    const LiteTableDef &table,
    const std::vector<std::string> &leadingKeyFields,
    std::string *keyPrefix,
    std::string *error);

bool LiteBuildUniqueKey(const LiteTableDef &table,
                             const std::string &encoded,
                             const std::vector<size_t> &keyColumns,
                             size_t keyOrdinal,
                             std::string *key,
                             bool *hasKey,
                             std::string *error);

bool LiteBinaryRowIsNull(const LiteTableDef &table,
                              const std::string &encoded,
                              size_t columnIndex,
                              bool *isNull,
                              std::string *error);

// Build an ordinal-independent key for comparing RI columns across tables.
// A NULL component returns hasKey=false, implementing MATCH SIMPLE.
bool LiteBuildConstraintKey(const LiteTableDef &table,
                                 const std::string &encoded,
                                 const std::vector<size_t> &keyColumns,
                                 std::string *key,
                                 bool *hasKey,
                                 std::string *error);

bool LiteBuildUniqueKeyFromTextFields(
    const LiteTableDef &table,
    const std::vector<size_t> &keyColumns,
    size_t keyOrdinal,
    const std::vector<std::string> &keyFields,
    std::string *key,
    bool *hasKey,
    std::string *error);

bool LiteBuildSecondaryIndexPrefixFromTextFields(
    const LiteTableDef &table,
    const std::vector<size_t> &keyColumns,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error);

bool LiteSecondaryIndexSupportsOrderedKeys(
    const LiteTableDef &table,
    const std::vector<size_t> &keyColumns);

bool LiteBuildOrderedSecondaryKeyPayload(
    const LiteTableDef &table,
    const LiteIndexDef &index,
    const std::string &encodedRow,
    std::string *payload,
    bool *hasKey,
    bool *containsNull,
    std::string *error);

bool LiteBuildOrderedSecondaryKeyPrefixFromTextFields(
    const LiteTableDef &table,
    const LiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error);

bool LiteBuildOrderedSecondaryNullPrefixFromTextFields(
    const LiteTableDef &table,
    const LiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    std::string *payloadPrefix,
    std::string *error);

bool LiteBuildOrderedSecondaryNullablePrefixFromTextFields(
    const LiteTableDef &table,
    const LiteIndexDef &index,
    const std::vector<std::string> &leadingKeyFields,
    const std::vector<bool> &nullFields,
    std::string *payloadPrefix,
    std::string *error);

#endif

#endif
