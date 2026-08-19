// @@@ START COPYRIGHT @@@
// Licensed to the Apache Software Foundation (ASF) under one or more
// contributor license agreements. See the NOTICE file distributed with
// this work for additional information regarding copyright ownership.
// @@@ END COPYRIGHT @@@

#include "Platform.h"

#ifdef TRAF_LOCAL_LITE

#include "LocalLiteConfig.h"
#include "LocalLiteRocksDBStore.h"
#include "LocalLiteRowCodec.h"

#include <signal.h>
#include <stdlib.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern void my_mpi_fclose();

// sqlcilib expects the standalone executable to provide the per-thread SQLCI
// environment, just like nativelite-server and sqlci do.
class SqlciEnv;
THREAD_P SqlciEnv *global_sqlci_env = NULL;

namespace {

const char *kCatalog = "TRAFODION";
const char *kSchema = "SEABASE";
const char *kTimestamp = "2026-08-15 00:00:00";

// Warehouse producers may encode rows concurrently, but the unified
// TransactionDB publication path is intentionally serialized.  This keeps
// the loader's worker/session isolation while avoiding lock-order inversion
// between concurrent table/index publication batches.
std::mutex gBulkPublicationMutex;

struct Config
{
  int warehouses;
  int districts;
  int customers;
  int orders;
  int newOrders;
  int items;
  int commitRows;
  int parallelWarehouses;
  long long seed;
  std::string report;

  Config()
    : warehouses(0), districts(0), customers(0), orders(0), newOrders(0),
      items(0), commitRows(100000), parallelWarehouses(1), seed(0)
  {
  }
};

std::string trim(const std::string &value)
{
  size_t begin = 0;
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n'))
    begin++;
  size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n'))
    end--;
  return value.substr(begin, end - begin);
}

bool loadProperties(const std::string &path,
                    std::map<std::string, std::string> *properties,
                    std::string *error)
{
  std::ifstream input(path.c_str());
  if (!input)
    {
      *error = "cannot open properties file: " + path;
      return false;
    }
  std::string line;
  while (std::getline(input, line))
    {
      line = trim(line);
      if (line.empty() || line[0] == '#')
        continue;
      size_t equal = line.find('=');
      if (equal == std::string::npos)
        continue;
      (*properties)[trim(line.substr(0, equal))] =
          trim(line.substr(equal + 1));
    }
  return true;
}

int propertyInt(const std::map<std::string, std::string> &properties,
                const std::string &name, int fallback, std::string *error)
{
  std::map<std::string, std::string>::const_iterator found =
      properties.find(name);
  if (found == properties.end())
    return fallback;
  char *end = NULL;
  long value = strtol(found->second.c_str(), &end, 10);
  if (!end || *end != '\0' || value <= 0 || value > 1000000000L)
    {
      *error = "invalid integer property " + name + "=" + found->second;
      return 0;
    }
  return static_cast<int>(value);
}

long long propertyLong(const std::map<std::string, std::string> &properties,
                       const std::string &name, long long fallback,
                       std::string *error)
{
  std::map<std::string, std::string>::const_iterator found =
      properties.find(name);
  if (found == properties.end())
    return fallback;
  char *end = NULL;
  long long value = strtoll(found->second.c_str(), &end, 10);
  if (!end || *end != '\0')
    {
      *error = "invalid long property " + name + "=" + found->second;
      return 0;
    }
  return value;
}

std::string number(long long value)
{
  std::ostringstream out;
  out << value;
  return out.str();
}

std::string decimal(long long cents)
{
  std::ostringstream out;
  out << (cents / 100) << '.' << std::setfill('0') << std::setw(2)
      << std::llabs(cents % 100);
  return out.str();
}

std::string decimal4(long long tenThousandths)
{
  std::ostringstream out;
  out << (tenThousandths / 10000) << '.' << std::setfill('0')
      << std::setw(4) << std::llabs(tenThousandths % 10000);
  return out.str();
}

std::string tableName(const char *name)
{
  return name;
}

struct Worker
{
  LocalLiteRocksDBStore store;
  LocalLiteTxnContext *context;
  std::string error;

  Worker() : context(NULL) {}

  bool open()
  {
    if (!store.open(&error))
      return false;
    context = LocalLiteTxnManager::createContext();
    return context != NULL;
  }

  void close()
  {
    if (context)
      {
        LocalLiteTxnManager::destroyContext(context);
        context = NULL;
      }
    store.close();
  }

  ~Worker() { close(); }
};

class BulkWriter
{
public:
  BulkWriter(Worker *worker, const Config &config, const std::string &name)
    : worker_(worker), config_(config), name_(name), rows_(0), committed_(0)
  {
  }

  bool open()
  {
    if (!worker_->store.loadTable(kCatalog, kSchema, name_, &table_,
                                  &worker_->error))
      return false;
    encoded_.reserve(static_cast<size_t>(config_.commitRows));
    return true;
  }

  bool add(const std::vector<std::string> &fields)
  {
    std::string encoded;
    if (!LocalLiteEncodeBinaryRow(table_, fields, &encoded,
                                  &worker_->error))
      return false;
    encoded_.push_back(encoded);
    rows_++;
    if (static_cast<int>(encoded_.size()) >= config_.commitRows)
      return flush();
    return true;
  }

  bool finish()
  {
    if (!flush())
      return false;
    std::cout << "bulk table=" << name_ << " rows=" << rows_
              << " commits=" << committed_ << std::endl;
    return true;
  }

  uint64_t rows() const { return rows_; }

private:
  bool flush()
  {
    if (encoded_.empty())
      return true;

    std::lock_guard<std::mutex> publicationLock(gBulkPublicationMutex);
    if (!LocalLiteTxnManager::begin(worker_->context, &worker_->error))
      return false;
    LocalLiteTxn transaction(&worker_->store, worker_->context);
    if (!transaction.insertRows(table_, encoded_, &worker_->error))
      {
        LocalLiteTxnManager::rollback(worker_->context, &worker_->error);
        return false;
      }
    if (!LocalLiteTxnManager::commit(worker_->context, &worker_->error))
      return false;
    encoded_.clear();
    committed_++;
    return true;
  }

  Worker *worker_;
  const Config &config_;
  std::string name_;
  LocalLiteTableDef table_;
  std::vector<std::string> encoded_;
  uint64_t rows_;
  uint64_t committed_;
};

bool addWarehouse(BulkWriter *writer, int warehouse)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back("W" + number(warehouse));
  row.push_back("Street 1");
  row.push_back("Street 2");
  row.push_back("NativeLite");
  row.push_back("NL");
  row.push_back("123456789");
  row.push_back("0.1000");
  row.push_back("300000.00");
  return writer->add(row);
}

bool addDistrict(BulkWriter *writer, int warehouse, int district,
                 const Config &config)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back(number(district));
  row.push_back("District" + number(district));
  row.push_back("Street 1");
  row.push_back("Street 2");
  row.push_back("NativeLite");
  row.push_back("NL");
  row.push_back("123456789");
  row.push_back("0.1000");
  row.push_back("30000.00");
  row.push_back(number(config.orders + 1));
  return writer->add(row);
}

bool addItem(BulkWriter *writer, int item)
{
  std::vector<std::string> row;
  row.push_back(number(item));
  row.push_back(number(item % 10000 + 1));
  row.push_back("ITEM-" + number(item));
  row.push_back(decimal((item * 37) % 9901 + 100));
  row.push_back(item % 10 == 0 ? "ITEM-ORIGINAL" : "ITEM-DATA");
  return writer->add(row);
}

bool addCustomer(BulkWriter *writer, int warehouse, int district, int customer)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back(number(district));
  row.push_back(number(customer));
  row.push_back("FIRST-" + number(customer));
  row.push_back("OE");
  row.push_back("LAST-" + number((customer - 1) % 1000));
  row.push_back("STREET-1");
  row.push_back("STREET-2");
  row.push_back("NATIVELITE");
  row.push_back("NL");
  row.push_back("123456789");
  row.push_back(number(customer));
  row.push_back(kTimestamp);
  row.push_back(customer % 10 == 0 ? "BC" : "GC");
  row.push_back("50000.00");
  row.push_back(decimal4((customer * 17) % 5000));
  row.push_back("-10.00");
  row.push_back("10.00");
  row.push_back("1");
  row.push_back("0");
  row.push_back("CUSTOMER-DATA");
  return writer->add(row);
}

bool addHistory(BulkWriter *writer, int warehouse, int district, int customer,
                const Config &config)
{
  long long base = (static_cast<long long>(warehouse - 1) * config.districts +
                    district - 1) * config.customers;
  std::vector<std::string> row;
  row.push_back(number(base + customer));
  row.push_back(number(customer));
  row.push_back(number(district));
  row.push_back(number(warehouse));
  row.push_back(number(district));
  row.push_back(number(warehouse));
  row.push_back(kTimestamp);
  row.push_back("10.00");
  row.push_back("HISTORY-DATA");
  return writer->add(row);
}

int orderLineCount(const Config &config, int warehouse, int district, int order)
{
  long long value = static_cast<long long>(order) * 37 + warehouse * 17 +
                    district * 13 + (config.seed % 11);
  return 5 + static_cast<int>((value % 11 + 11) % 11);
}

bool addOrder(BulkWriter *writer, int warehouse, int district, int order,
              const Config &config)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back(number(district));
  row.push_back(number(order));
  row.push_back(number((order * 37 + district * 17) % config.customers + 1));
  row.push_back(kTimestamp);
  row.push_back(order <= config.orders - config.newOrders
                    ? number(order % 10 + 1) : "");
  row.push_back(number(orderLineCount(config, warehouse, district, order)));
  row.push_back("1");
  return writer->add(row);
}

bool addNewOrder(BulkWriter *writer, int warehouse, int district, int order)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back(number(district));
  row.push_back(number(order));
  return writer->add(row);
}

bool addStock(BulkWriter *writer, int warehouse, int item)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back(number(item));
  row.push_back(number((item * 37) % 91 + 10));
  for (int district = 1; district <= 10; district++)
    row.push_back("DIST-" + number(district));
  row.push_back("0");
  row.push_back("0");
  row.push_back("0");
  row.push_back(item % 10 == 0 ? "STOCK-ORIGINAL" : "STOCK-DATA");
  return writer->add(row);
}

bool addOrderLine(BulkWriter *writer, int warehouse, int district, int order,
                  int line, const Config &config)
{
  std::vector<std::string> row;
  row.push_back(number(warehouse));
  row.push_back(number(district));
  row.push_back(number(order));
  row.push_back(number(line));
  row.push_back(number((order * 37 + line * 13 + district * 17) %
                       config.items + 1));
  row.push_back(number(warehouse));
  row.push_back(order <= config.orders - config.newOrders ? kTimestamp : "");
  row.push_back("5");
  long long cents = order <= config.orders - config.newOrders
      ? 0 : (order * 37 + line * 13) % 999999 + 1;
  row.push_back(decimal(cents));
  row.push_back("DIST-" + number(district));
  return writer->add(row);
}

bool runSingle(const Config &config,
               const std::string &name,
               const std::function<bool(BulkWriter &)> &producer,
               std::string *error)
{
  Worker worker;
  if (!worker.open())
    {
      *error = worker.error;
      return false;
    }
  BulkWriter writer(&worker, config, name);
  if (!writer.open() || !producer(writer) || !writer.finish())
    {
      *error = worker.error.empty() ? "native bulk writer failed" : worker.error;
      return false;
    }
  return true;
}

bool runWarehouses(const Config &config,
                   const std::string &name,
                   const std::function<bool(BulkWriter &, int)> &producer,
                   std::string *error)
{
  const int parallelism = std::max(1, std::min(config.parallelWarehouses,
                                                config.warehouses));
  std::atomic<int> nextWarehouse(1);
  std::atomic<bool> failed(false);
  std::mutex errorMutex;
  std::string firstError;
  std::vector<std::thread> threads;
  for (int slot = 0; slot < parallelism; slot++)
    threads.push_back(std::thread([&]() {
      Worker worker;
      if (!worker.open())
        {
          std::lock_guard<std::mutex> lock(errorMutex);
          if (firstError.empty()) firstError = worker.error;
          failed.store(true);
          return;
        }
      for (;;)
        {
          if (failed.load()) break;
          int warehouse = nextWarehouse.fetch_add(1);
          if (warehouse > config.warehouses) break;
          BulkWriter writer(&worker, config, name);
          if (!writer.open() || !producer(writer, warehouse) ||
              !writer.finish())
            {
              std::lock_guard<std::mutex> lock(errorMutex);
              if (firstError.empty())
                firstError = worker.error.empty()
                    ? "native warehouse bulk writer failed" : worker.error;
              failed.store(true);
              break;
            }
        }
    }));
  for (size_t i = 0; i < threads.size(); i++) threads[i].join();
  if (failed.load())
    {
      *error = firstError.empty() ? "native warehouse bulk loader failed"
                                  : firstError;
      return false;
    }
  return true;
}

bool run(const Config &config, std::string *error)
{
  if (!runSingle(config, tableName("TPCC_WAREHOUSE"),
                 [&](BulkWriter &writer) {
                   for (int w = 1; w <= config.warehouses; w++)
                     if (!addWarehouse(&writer, w)) return false;
                   return true;
                 }, error)) return false;

  if (!runSingle(config, tableName("TPCC_DISTRICT"),
                 [&](BulkWriter &writer) {
                   for (int w = 1; w <= config.warehouses; w++)
                     for (int d = 1; d <= config.districts; d++)
                       if (!addDistrict(&writer, w, d, config)) return false;
                   return true;
                 }, error)) return false;

  if (!runSingle(config, tableName("TPCC_ITEM"),
                 [&](BulkWriter &writer) {
                   for (int i = 1; i <= config.items; i++)
                     if (!addItem(&writer, i)) return false;
                   return true;
                 }, error)) return false;

  if (!runWarehouses(config, tableName("TPCC_CUSTOMER"),
                     [&](BulkWriter &writer, int w) {
                       for (int d = 1; d <= config.districts; d++)
                         for (int c = 1; c <= config.customers; c++)
                           if (!addCustomer(&writer, w, d, c)) return false;
                       return true;
                     }, error)) return false;

  if (!runWarehouses(config, tableName("TPCC_HISTORY"),
                     [&](BulkWriter &writer, int w) {
                       for (int d = 1; d <= config.districts; d++)
                         for (int c = 1; c <= config.customers; c++)
                           if (!addHistory(&writer, w, d, c, config)) return false;
                       return true;
                     }, error)) return false;

  if (!runWarehouses(config, tableName("TPCC_ORDERS"),
                     [&](BulkWriter &writer, int w) {
                       for (int d = 1; d <= config.districts; d++)
                         for (int o = 1; o <= config.orders; o++)
                           if (!addOrder(&writer, w, d, o, config)) return false;
                       return true;
                     }, error)) return false;

  if (!runWarehouses(config, tableName("TPCC_NEW_ORDER"),
                     [&](BulkWriter &writer, int w) {
                       for (int d = 1; d <= config.districts; d++)
                         for (int o = config.orders - config.newOrders + 1;
                              o <= config.orders; o++)
                           if (!addNewOrder(&writer, w, d, o)) return false;
                       return true;
                     }, error)) return false;

  if (!runWarehouses(config, tableName("TPCC_STOCK"),
                     [&](BulkWriter &writer, int w) {
                       for (int i = 1; i <= config.items; i++)
                         if (!addStock(&writer, w, i)) return false;
                       return true;
                     }, error)) return false;

  if (!runWarehouses(config, tableName("TPCC_ORDER_LINE"),
                     [&](BulkWriter &writer, int w) {
                       for (int d = 1; d <= config.districts; d++)
                         for (int o = 1; o <= config.orders; o++)
                           for (int l = 1;
                                l <= orderLineCount(config, w, d, o); l++)
                             if (!addOrderLine(&writer, w, d, o, l, config))
                               return false;
                       return true;
                     }, error)) return false;
  return true;
}

void usage(const char *program)
{
  std::cerr << "Usage: " << program
            << " --properties FILE [--scale qualification|multi]"
               " [--commit-rows N] [--report FILE]" << std::endl;
}

bool parseConfig(int argc, char **argv, Config *config, std::string *error)
{
  std::string propertiesPath;
  std::string scale = "qualification";
  int commitRowsOverride = 0;
  for (int i = 1; i < argc; i++)
    {
      std::string option = argv[i];
      if (option == "--properties" && i + 1 < argc)
        propertiesPath = argv[++i];
      else if (option == "--scale" && i + 1 < argc)
        scale = argv[++i];
      else if (option == "--commit-rows" && i + 1 < argc)
        {
          char *end = NULL;
          long value = strtol(argv[++i], &end, 10);
          if (!end || *end != '\0' || value <= 0 || value > 1000000000L)
            {
              *error = "invalid --commit-rows value";
              return false;
            }
          commitRowsOverride = static_cast<int>(value);
        }
      else if (option == "--report" && i + 1 < argc)
        config->report = argv[++i];
      else
        {
          *error = "unknown or incomplete option: " + option;
          return false;
        }
    }
  if (propertiesPath.empty())
    {
      *error = "--properties is required";
      return false;
    }
  std::map<std::string, std::string> properties;
  if (!loadProperties(propertiesPath, &properties, error))
    return false;
  const std::string prefix = scale == "multi" ? "performance." : "";
  config->warehouses = propertyInt(properties, prefix + "warehouses", 0, error);
  if (scale == "smoke")
    {
      config->districts = 2;
      config->customers = 100;
      config->orders = 100;
      config->newOrders = 30;
      config->items = 1000;
    }
  else
    {
      config->districts = propertyInt(properties,
          prefix + "districts.per.warehouse", 0, error);
      config->customers = propertyInt(properties,
          prefix + "customers.per.district", 0, error);
      config->orders = propertyInt(properties,
          prefix + "orders.per.district", 0, error);
      config->newOrders = propertyInt(properties,
          prefix + "new.orders.per.district", 0, error);
      config->items = propertyInt(properties, prefix + "items", 0, error);
    }
  config->commitRows = propertyInt(properties, "loader.commit.rows", 100000,
                                   error);
  if (commitRowsOverride > 0)
    config->commitRows = commitRowsOverride;
  config->parallelWarehouses = propertyInt(properties,
      "loader.parallel.warehouses", 1, error);
  config->seed = propertyLong(properties, "data.seed", 2026081501, error);
  if (error->empty() && (config->warehouses <= 0 || config->districts <= 0 ||
                         config->customers <= 0 || config->orders <= 0 ||
                         config->newOrders <= 0 || config->items <= 0 ||
                         config->newOrders > config->orders))
    *error = "invalid TPCC bulk-loader cardinality configuration";
  return error->empty();
}

} // namespace

int main(int argc, char **argv)
{
  Config config;
  std::string error;
  if (!parseConfig(argc, argv, &config, &error))
    {
      usage(argv[0]);
      std::cerr << "NativeLite bulk-loader configuration failed: " << error
                << std::endl;
      return 2;
    }
  if (LocalLiteConfig_init() != 0)
    {
      std::cerr << "NativeLite bulk-loader environment initialization failed"
                << std::endl;
      return 1;
    }
  atexit(my_mpi_fclose);

  const std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  if (!run(config, &error))
    {
      std::cerr << "NativeLite bulk-loader failed: " << error << std::endl;
      return 1;
    }
  const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started).count();
  std::ostringstream report;
  report << "{\"contract_version\":1,\"loader\":\"native\",\"warehouses\":"
         << config.warehouses << ",\"elapsed_ms\":" << elapsed
         << ",\"consistency\":\"deferred_to_sql_verifier\"}\n";
  std::cout << report.str();
  if (!config.report.empty())
    {
      std::ofstream output(config.report.c_str());
      if (!output)
        {
          std::cerr << "NativeLite bulk-loader cannot write report: "
                    << config.report << std::endl;
          return 1;
        }
      output << report.str();
    }
  return 0;
}

#else

#include <iostream>
int main()
{
  std::cerr << "nativelite-bulk-loader requires TRAF_LOCAL_LITE" << std::endl;
  return 1;
}

#endif
