#include "hiveHook.h"

NABoolean hive_sd_desc::isOrcFile() const
{
  return inputFormat_ && outputFormat_ &&
    strstr(inputFormat_, "Orc") && strstr(outputFormat_, "Orc");
}

NABoolean hive_sd_desc::isParquetFile() const
{
  return inputFormat_ && outputFormat_ &&
    strstr(inputFormat_, "Parquet") && strstr(outputFormat_, "Parquet");
}

NABoolean hive_sd_desc::isSequenceFile() const
{
  return inputFormat_ && outputFormat_ &&
    strstr(inputFormat_, "Sequence") && strstr(outputFormat_, "Sequence");
}

NABoolean hive_sd_desc::isTextFile() const
{
  return inputFormat_ && outputFormat_ &&
    strstr(inputFormat_, "Text") && strstr(outputFormat_, "Text");
}

hive_tbl_desc::hive_tbl_desc(NAHeap *heap,
                             Int32 tblID,
                             const char *name,
                             const char *schName,
                             const char *owner,
                             const char *tableType,
                             Int64 creationTS,
                             const char *viewOriginalText,
                             const char *viewExpandedText,
                             hive_sd_desc *sd,
                             hive_pkey_desc *pk,
                             hive_tblparams_desc *tp)
  : heap_(heap),
    tblID_(tblID),
    tblName_(strduph(name, heap_)),
    schName_(strduph(schName, heap_)),
    owner_(owner ? strduph(owner, heap_) : NULL),
    tableType_(tableType ? strduph(tableType, heap_) : NULL),
    creationTS_(creationTS),
    redefineTS_(-1),
    viewOriginalText_(NULL),
    viewExpandedText_(NULL),
    sd_(sd),
    pkey_(pk),
    tblParams_(tp),
    next_(NULL)
{
  if (isView())
    {
      viewOriginalText_ = viewOriginalText ? strduph(viewOriginalText, heap_) : NULL;
      viewExpandedText_ = viewExpandedText ? strduph(viewExpandedText, heap_) : NULL;
    }
}

struct hive_column_desc* hive_tbl_desc::getColumns()
{
  return sd_ ? sd_->column_ : NULL;
}

struct hive_bkey_desc* hive_tbl_desc::getBucketingKeys()
{
  return sd_ ? sd_->bkey_ : NULL;
}

struct hive_skey_desc* hive_tbl_desc::getSortKeys()
{
  return sd_ ? sd_->skey_ : NULL;
}

Int32 hive_tbl_desc::getNumOfCols()
{
  Int32 result = 0;
  for (hive_column_desc *cd = getColumns(); cd; cd = cd->next_)
    result++;
  return result;
}

Int32 hive_tbl_desc::getNumOfPartCols() const
{
  Int32 result = 0;
  for (hive_pkey_desc *pk = pkey_; pk; pk = pk->next_)
    result++;
  return result;
}

Int32 hive_tbl_desc::getNumOfSortCols()
{
  Int32 result = 0;
  for (hive_skey_desc *sk = getSortKeys(); sk; sk = sk->next_)
    result++;
  return result;
}

Int32 hive_tbl_desc::getNumOfBucketCols()
{
  Int32 result = 0;
  for (hive_bkey_desc *bc = getBucketingKeys(); bc; bc = bc->next_)
    result++;
  return result;
}

Int32 hive_tbl_desc::getPartColNum(const char *name)
{
  Int32 num = 0;
  for (hive_pkey_desc *desc = getPartKey(); desc; desc = desc->next_, num++)
    if (strcmp(name, desc->name_) == 0)
      return num;
  return -1;
}

Int32 hive_tbl_desc::getBucketColNum(const char *name)
{
  Int32 num = 0;
  for (hive_bkey_desc *desc = getBucketingKeys(); desc; desc = desc->next_, num++)
    if (strcmp(name, desc->name_) == 0)
      return num;
  return -1;
}

Int32 hive_tbl_desc::getSortColNum(const char *name)
{
  Int32 num = 0;
  for (hive_skey_desc *desc = getSortKeys(); desc; desc = desc->next_, num++)
    if (strcmp(name, desc->name_) == 0)
      return num;
  return -1;
}

Int64 hive_tbl_desc::redeftime()
{
  Int64 result = creationTS_ * 1000;
  if (redefineTS_ != -1 && redefineTS_ > result)
    result = redefineTS_;
  return result;
}

Int64 hive_tbl_desc::setRedeftime(Int64 redefineTS)
{
  redefineTS_ = redefineTS;
  return redeftime();
}
