#include <rocksdb/c.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

void check(char *error, const char *operation)
{
  if (!error)
    return;
  std::cerr << "FAIL: " << operation << ": " << error << std::endl;
  rocksdb_free(error);
  std::exit(1);
}

bool startsWith(const std::string &value, const std::string &prefix)
{
  return value.compare(0, prefix.size(), prefix) == 0;
}

std::string hexEncode(const std::string &value)
{
  static const char digits[] = "0123456789abcdef";
  std::string result;
  for (size_t i = 0; i < value.size(); i++)
    {
      const unsigned char byte = static_cast<unsigned char>(value[i]);
      result += digits[byte >> 4];
      result += digits[byte & 15];
    }
  return result;
}

int hexDigit(char value)
{
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return -1;
}

bool hexDecode(const std::string &value, std::string *decoded)
{
  decoded->clear();
  if ((value.size() & 1) != 0)
    return false;
  for (size_t i = 0; i < value.size(); i += 2)
    {
      const int high = hexDigit(value[i]);
      const int low = hexDigit(value[i + 1]);
      if (high < 0 || low < 0)
        return false;
      decoded->push_back(static_cast<char>((high << 4) | low));
    }
  return true;
}

std::string physicalKey(const std::string &logicalKey)
{
  return "catalog/" + hexEncode(logicalKey);
}

rocksdb_t *openCatalog(const char *path)
{
  rocksdb_options_t *options = rocksdb_options_create();
  char *error = NULL;
  rocksdb_t *db = rocksdb_open(options, path, &error);
  rocksdb_options_destroy(options);
  check(error, "open catalog");
  return db;
}

void downgrade(const char *path)
{
  rocksdb_t *db = openCatalog(path);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
  const char *prefixes[] = {"md|COLUMNS|", "md|KEYS|"};
  for (size_t p = 0; p < 2; p++)
    {
      const std::string prefix(prefixes[p]);
      const std::string storedPrefix = physicalKey(prefix);
      rocksdb_iterator_t *iterator =
          rocksdb_create_iterator(db, readOptions);
      bool copiedLegacyValue = false;
      for (rocksdb_iter_seek(iterator, storedPrefix.data(), storedPrefix.size());
           rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator))
        {
          size_t keyLength = 0;
          size_t valueLength = 0;
          const char *rawKey = rocksdb_iter_key(iterator, &keyLength);
          const std::string storedKey(rawKey, keyLength);
          if (!startsWith(storedKey, storedPrefix))
            break;
          std::string key;
          if (!hexDecode(storedKey.substr(std::string("catalog/").size()),
                         &key))
            {
              std::cerr << "FAIL: invalid unified catalog key" << std::endl;
              std::exit(1);
            }
          const char *rawValue = rocksdb_iter_value(iterator, &valueLength);
          rocksdb_writebatch_delete(batch, storedKey.data(), storedKey.size());
          // The pre-M11 fixed buffer could collapse distinct logical keys.
          // Preserve one representative value under a deliberately colliding
          // legacy key; startup must regenerate the complete set from table
          // definitions rather than trusting this lossy key.
          if (!copiedLegacyValue)
            {
              const std::string legacyKey = prefix + "legacy-truncated";
              const std::string storedLegacyKey = physicalKey(legacyKey);
              rocksdb_writebatch_put(batch, storedLegacyKey.data(),
                                     storedLegacyKey.size(),
                                     rawValue, valueLength);
              copiedLegacyValue = true;
            }
        }
      char *error = NULL;
      rocksdb_iter_get_error(iterator, &error);
      rocksdb_iter_destroy(iterator);
      check(error, "scan v2 metadata keys");
    }
  const std::string marker("format|metadata-key");
  const std::string storedMarker = physicalKey(marker);
  rocksdb_writebatch_delete(batch, storedMarker.data(), storedMarker.size());
  rocksdb_writeoptions_t *writeOptions = rocksdb_writeoptions_create();
  rocksdb_writeoptions_set_sync(writeOptions, 1);
  char *error = NULL;
  rocksdb_write(db, writeOptions, batch, &error);
  check(error, "install legacy metadata keys");
  rocksdb_writeoptions_destroy(writeOptions);
  rocksdb_writebatch_destroy(batch);
  rocksdb_readoptions_destroy(readOptions);
  rocksdb_close(db);
}

void verify(const char *path)
{
  rocksdb_t *db = openCatalog(path);
  rocksdb_readoptions_t *readOptions = rocksdb_readoptions_create();
  const std::string marker("format|metadata-key");
  const std::string storedMarker = physicalKey(marker);
  char *error = NULL;
  size_t valueLength = 0;
  char *rawValue = rocksdb_get(db, readOptions, storedMarker.data(),
                               storedMarker.size(), &valueLength, &error);
  check(error, "read metadata-key marker");
  if (!rawValue || std::string(rawValue, valueLength) != "2")
    {
      if (rawValue)
        rocksdb_free(rawValue);
      std::cerr << "FAIL: metadata-key marker was not migrated" << std::endl;
      std::exit(1);
    }
  rocksdb_free(rawValue);

  const char *prefixes[] = {"md|COLUMNS|", "md|KEYS|"};
  size_t counts[] = {0, 0};
  for (size_t p = 0; p < 2; p++)
    {
      const std::string prefix(prefixes[p]);
      const std::string storedPrefix = physicalKey(prefix);
      rocksdb_iterator_t *iterator =
          rocksdb_create_iterator(db, readOptions);
      for (rocksdb_iter_seek(iterator, storedPrefix.data(), storedPrefix.size());
           rocksdb_iter_valid(iterator); rocksdb_iter_next(iterator))
        {
          size_t keyLength = 0;
          const char *rawKey = rocksdb_iter_key(iterator, &keyLength);
          const std::string storedKey(rawKey, keyLength);
          if (!startsWith(storedKey, storedPrefix))
            break;
          std::string key;
          if (!hexDecode(storedKey.substr(std::string("catalog/").size()),
                         &key))
            {
              std::cerr << "FAIL: invalid unified catalog key" << std::endl;
              std::exit(1);
            }
          if (!startsWith(key, prefix + "v2|"))
            {
              std::cerr << "FAIL: legacy metadata key survived migration"
                        << std::endl;
              std::exit(1);
            }
          counts[p]++;
        }
      rocksdb_iter_destroy(iterator);
    }
  rocksdb_readoptions_destroy(readOptions);
  rocksdb_close(db);
  if (counts[0] != 2 || counts[1] != 3)
    {
      std::cerr << "FAIL: migrated metadata key counts are columns="
                << counts[0] << " keys=" << counts[1] << std::endl;
      std::exit(1);
    }
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 3 ||
      (std::string(argv[1]) != "downgrade" &&
       std::string(argv[1]) != "verify"))
    {
      std::cerr << "usage: metadata-key-migration-test "
                   "downgrade|verify CATALOG"
                << std::endl;
      return 2;
    }
  if (std::string(argv[1]) == "downgrade")
    downgrade(argv[2]);
  else
    verify(argv[2]);
  return 0;
}
