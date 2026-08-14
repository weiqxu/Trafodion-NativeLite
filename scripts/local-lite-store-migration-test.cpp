#include "LocalLiteStorage.h"
#include "LocalLiteStorageMigration.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
  if (argc != 4)
    return 2;
  LocalLiteStorageStatus status;
  LocalLiteStorageOptions options;
  options.synchronousCommit = true;
  LocalLiteStorageEngine *engine = LocalLiteCreateRocksDBTransactionEngine();
  if (!engine->open(argv[3], options, &status))
    {
      std::cerr << "FAIL: open migration target: " << status.message
                << std::endl;
      return 1;
    }
  bool ok = false;
  if (std::string(argv[1]) == "migrate")
    ok = LocalLiteMigrateLegacyStore(argv[2], engine, &status);
  else if (std::string(argv[1]) == "verify")
    ok = LocalLiteVerifyMigratedStore(engine, &status);
  else
    return 2;
  engine->close();
  delete engine;
  if (!ok)
    {
      std::cerr << "FAIL: " << argv[1] << " legacy store: "
                << status.message << std::endl;
      return 1;
    }
  return 0;
}
