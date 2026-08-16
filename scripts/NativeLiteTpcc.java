import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Properties;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/** Deterministic, repository-owned loader and verifier for M14 TPC-C-like data. */
public final class NativeLiteTpcc {
  private static final String USER = "DB__ROOT";
  private static final String FIXED_TS = "2026-08-15 00:00:00";

  private static final class Config {
    int warehouses;
    final int districts;
    final int customers;
    final int orders;
    final int newOrders;
    final int items;
    final int batchRows;
    final int commitRows;
    final int loaderParallelism;
    final long seed;
    final String scale;

    Config(Properties properties, String selectedScale) {
      scale = selectedScale;
      seed = Long.parseLong(required(properties, "data.seed"));
      warehouses = Integer.parseInt(required(properties, "warehouses"));
      batchRows = Integer.parseInt(required(properties, "loader.batch.rows"));
      commitRows = Integer.parseInt(required(properties, "loader.commit.rows"));
      loaderParallelism = Integer.parseInt(properties.getProperty(
          "loader.parallel.warehouses", "1"));
      if (batchRows <= 0 || commitRows <= 0 || commitRows < batchRows) {
        throw new IllegalArgumentException(
            "loader row bounds must satisfy 0 < batch.rows <= commit.rows");
      }
      if (loaderParallelism <= 0) {
        throw new IllegalArgumentException("loader parallelism must be positive");
      }
      if ("qualification".equals(scale)) {
        districts = Integer.parseInt(required(properties,
            "districts.per.warehouse"));
        customers = Integer.parseInt(required(properties,
            "customers.per.district"));
        orders = Integer.parseInt(required(properties, "orders.per.district"));
        newOrders = Integer.parseInt(required(properties,
            "new.orders.per.district"));
        items = Integer.parseInt(required(properties, "items"));
      } else if ("smoke".equals(scale)) {
        districts = 2;
        customers = 100;
        orders = 100;
        newOrders = 30;
        items = 1000;
      } else if ("multi".equals(scale)) {
        warehouses = Integer.parseInt(required(properties,
            "performance.warehouses"));
        districts = Integer.parseInt(required(properties,
            "performance.districts.per.warehouse"));
        customers = Integer.parseInt(required(properties,
            "performance.customers.per.district"));
        orders = Integer.parseInt(required(properties,
            "performance.orders.per.district"));
        newOrders = Integer.parseInt(required(properties,
            "performance.new.orders.per.district"));
        items = Integer.parseInt(required(properties, "performance.items"));
      } else {
        throw new IllegalArgumentException("unknown scale: " + scale);
      }
    }
  }

  private interface RowFactory {
    String row(long index);
  }

  private interface RowBinder {
    void bind(PreparedStatement statement, long index) throws SQLException;
  }

  private interface WarehouseLoader {
    void load(NativeLiteTpcc worker, int warehouse) throws SQLException;
  }

  private interface ParallelLoader {
    void load(NativeLiteTpcc worker, int task) throws SQLException;
  }

  private final Connection connection;
  private final String jdbcUrl;
  private final Config config;
  private final Path schemaPath;
  private int committedBatches;
  private final int failAfterBatches;

  private NativeLiteTpcc(Connection connection, String jdbcUrl, Config config,
      Path schemaPath) {
    this.connection = connection;
    this.jdbcUrl = jdbcUrl;
    this.config = config;
    this.schemaPath = schemaPath;
    this.failAfterBatches = Integer.parseInt(
        System.getenv().getOrDefault("TPCC_FAIL_AFTER_BATCHES", "0"));
  }

  private void loadWarehouses(String table, WarehouseLoader loader)
      throws SQLException {
    loadParallel(table, config.warehouses, (worker, task) -> {
      int warehouse = task + 1;
      loader.load(worker, warehouse);
      System.out.println("loader table=" + table + " warehouse=" + warehouse +
          " action=loaded");
    });
  }

  private void loadParallel(String table, int taskCount, ParallelLoader loader)
      throws SQLException {
    // The coordinator must not retain its pre-load MVCC snapshot while worker
    // sessions commit disjoint warehouse partitions.
    connection.commit();
    int parallelism = Math.min(config.loaderParallelism, taskCount);
    if (parallelism == 1) {
      for (int task = 0; task < taskCount; task++) loader.load(this, task);
      return;
    }
    ExecutorService pool = Executors.newFixedThreadPool(parallelism);
    List<Future<?>> futures = new ArrayList<>();
    try {
      for (int task = 0; task < taskCount; task++) {
        final int selectedTask = task;
        futures.add(pool.submit(() -> {
          try (Connection workerConnection = DriverManager.getConnection(
              jdbcUrl, USER, "")) {
            workerConnection.setAutoCommit(false);
            NativeLiteTpcc worker = new NativeLiteTpcc(workerConnection,
                jdbcUrl, config, schemaPath);
            loader.load(worker, selectedTask);
            return null;
          }
        }));
      }
      for (Future<?> future : futures) future.get();
    } catch (InterruptedException interrupted) {
      Thread.currentThread().interrupt();
      throw new SQLException("interrupted while loading " + table,
          interrupted);
    } catch (ExecutionException failed) {
      Throwable cause = failed.getCause();
      if (cause instanceof SQLException) throw (SQLException) cause;
      throw new SQLException("parallel loader failed for " + table, cause);
    } finally {
      pool.shutdownNow();
    }
  }

  private static void require(boolean condition, String message) {
    if (!condition) throw new IllegalStateException(message);
  }

  private static String required(Properties properties, String name) {
    String value = properties.getProperty(name);
    if (value == null || value.isEmpty()) {
      throw new IllegalArgumentException("missing property: " + name);
    }
    return value;
  }

  private static Properties loadProperties(Path path) throws IOException {
    Properties properties = new Properties();
    try (java.io.Reader reader = Files.newBufferedReader(
        path, StandardCharsets.UTF_8)) {
      properties.load(reader);
    }
    return properties;
  }

  private boolean tableExists(String name) throws SQLException {
    try (ResultSet tables = connection.getMetaData().getTables(
        "TRAFODION", "SEABASE", name, new String[] {"TABLE"})) {
      return tables.next();
    }
  }

  private void ensureSchema() throws IOException, SQLException {
    if (tableExists("TPCC_WAREHOUSE")) return;
    String source = new String(Files.readAllBytes(schemaPath),
        StandardCharsets.UTF_8);
    StringBuilder cleaned = new StringBuilder();
    for (String line : source.split("\\r?\\n")) {
      int comment = line.indexOf("--");
      cleaned.append(comment < 0 ? line : line.substring(0, comment)).append('\n');
    }
    try (Statement statement = connection.createStatement()) {
      for (String sql : cleaned.toString().split(";")) {
        if (!sql.trim().isEmpty()) statement.execute(sql.trim());
      }
    }
    connection.commit();
  }

  private long count(String table) throws SQLException {
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery("SELECT COUNT(*) FROM " + table)) {
      require(result.next(), "COUNT returned no row for " + table);
      return result.getLong(1);
    }
  }

  private void resetPartial(String table, long expected) throws SQLException {
    long current = count(table);
    if (current == expected) return;
    if (current != 0) {
      try (Statement statement = connection.createStatement()) {
        statement.executeUpdate("DELETE FROM " + table);
      }
      connection.commit();
    }
  }

  private void insertRows(String table, long count, RowFactory factory)
      throws SQLException {
    resetPartial(table, count);
    if (count(table) == count) {
      System.out.println("loader table=" + table + " rows=" + count + " action=skip");
      return;
    }
    for (long first = 0; first < count; first += config.batchRows) {
      long end = Math.min(count, first + config.batchRows);
      StringBuilder sql = new StringBuilder("INSERT INTO ")
          .append(table).append(" VALUES ");
      for (long index = first; index < end; index++) {
        if (index != first) sql.append(',');
        sql.append(factory.row(index));
      }
      try (Statement statement = connection.createStatement()) {
        int affected = statement.executeUpdate(sql.toString());
        require(affected == end - first,
            table + " batch affected " + affected + " rows, expected " +
            (end - first));
      }
      connection.commit();
      afterBatch(table);
    }
    System.out.println("loader table=" + table + " rows=" + count + " action=loaded");
  }

  private void insertPrepared(String table, long rowCount, String sql,
      RowBinder binder) throws SQLException {
    resetPartial(table, rowCount);
    if (count(table) == rowCount) {
      System.out.println("loader table=" + table + " rows=" + rowCount +
          " action=skip");
      return;
    }
    try (PreparedStatement statement = connection.prepareStatement(sql)) {
      int pending = 0;
      for (long index = 0; index < rowCount; index++) {
        binder.bind(statement, index);
        statement.addBatch();
        pending++;
        if (pending == config.batchRows) {
          executePreparedBatch(table, statement, pending);
          pending = 0;
        }
      }
      if (pending != 0) executePreparedBatch(table, statement, pending);
    }
    System.out.println("loader table=" + table + " rows=" + rowCount +
        " action=loaded");
  }

  private void executePreparedBatch(String table, PreparedStatement statement,
      int expected) throws SQLException {
    int[] results = statement.executeBatch();
    require(results.length == expected,
        table + " batch returned " + results.length + " entries, expected " +
        expected);
    for (int result : results) {
      require(result == 1 || result == Statement.SUCCESS_NO_INFO,
          table + " batch row returned " + result);
    }
    connection.commit();
    afterBatch(table);
  }

  private void afterBatch(String table) {
    committedBatches++;
    if (failAfterBatches > 0 && committedBatches >= failAfterBatches) {
      throw new IllegalStateException("injected loader interruption after " +
          committedBatches + " batches while loading " + table);
    }
  }

  private static String quote(String value) {
    return "'" + value.replace("'", "''") + "'";
  }

  private int orderLineCount(int warehouse, int district, int order) {
    return 5 + Math.floorMod(order * 37 + warehouse * 17 + district * 13 +
        Math.floorMod(config.seed, 11), 11);
  }

  private long expectedOrderLines() {
    long rows = 0;
    for (int warehouse = 1; warehouse <= config.warehouses; warehouse++)
      for (int district = 1; district <= config.districts; district++)
        for (int order = 1; order <= config.orders; order++)
          rows += orderLineCount(warehouse, district, order);
    return rows;
  }

  private void executeCommitted(String sql) throws SQLException {
    try (Statement statement = connection.createStatement()) {
      statement.execute(sql);
    }
    connection.commit();
  }

  private void ensureNumberDomain() throws SQLException {
    if (!tableExists("TPCC_LOAD_SEED")) {
      executeCommitted("CREATE TABLE TPCC_LOAD_SEED " +
          "(N INT NOT NULL PRIMARY KEY)");
    }
    insertPrepared("TPCC_LOAD_SEED", 1000,
        "INSERT INTO TPCC_LOAD_SEED VALUES (?)", (statement, index) ->
            statement.setInt(1, (int) index));
    if (!tableExists("TPCC_LOAD_NUMBER")) {
      executeCommitted("CREATE TABLE TPCC_LOAD_NUMBER " +
          "(N INT NOT NULL PRIMARY KEY)");
    }
    long expected = Math.max(config.items,
        Math.max(config.customers, config.orders));
    resetPartial("TPCC_LOAD_NUMBER", expected);
    if (count("TPCC_LOAD_NUMBER") != expected) {
      int chunks = (int) ((expected + config.commitRows - 1) /
          config.commitRows);
      loadParallel("TPCC_LOAD_NUMBER", chunks, (worker, task) -> {
        long first = (long) task * config.commitRows + 1;
        long last = Math.min(expected, first + config.commitRows - 1L);
        String expression = "A.N * 1000 + B.N + 1";
        String sql = "INSERT INTO TPCC_LOAD_NUMBER SELECT " + expression +
            " FROM TPCC_LOAD_SEED A CROSS JOIN TPCC_LOAD_SEED B WHERE " +
            expression + " BETWEEN " + first + " AND " + last;
        try (Statement statement = worker.connection.createStatement()) {
          int affected = statement.executeUpdate(sql);
          require(affected == last - first + 1,
              "number domain chunk affected-row mismatch");
        }
        worker.connection.commit();
        worker.afterBatch("TPCC_LOAD_NUMBER");
      });
      System.out.println("loader table=TPCC_LOAD_NUMBER rows=" + expected +
          " action=loaded");
    }
  }

  private void loadSetBasedTables() throws SQLException {
    ensureNumberDomain();
    resetPartial("TPCC_ITEM", config.items);
    if (count("TPCC_ITEM") != config.items) {
      int chunks = (config.items + config.commitRows - 1) / config.commitRows;
      loadParallel("TPCC_ITEM", chunks, (worker, task) -> {
        int first = task * config.commitRows + 1;
        int last = Math.min(config.items, first + config.commitRows - 1);
        worker.executeSetChunk("TPCC_ITEM",
            "SELECT N, MOD(N, 10000) + 1, 'ITEM-' || CAST(N AS VARCHAR(10)), " +
            "CAST(MOD(N * 37, 9901) + 100 AS DECIMAL(7,2)) / 100, " +
            "CASE WHEN MOD(N,10)=0 THEN 'ITEM-ORIGINAL' ELSE 'ITEM-DATA' END " +
            "FROM TPCC_LOAD_NUMBER WHERE N BETWEEN " + first + " AND " + last);
      });
      requireCount("TPCC_ITEM", config.items);
      System.out.println("loader table=TPCC_ITEM rows=" + config.items +
          " action=loaded");
    }

    long customerTotal = (long) config.warehouses * config.districts *
        config.customers;
    resetPartial("TPCC_CUSTOMER", customerTotal);
    if (count("TPCC_CUSTOMER") != customerTotal) {
      loadWarehouses("TPCC_CUSTOMER", (worker, w) -> {
        for (int d = 1; d <= config.districts; d++) {
          for (int first = 1; first <= config.customers;
               first += config.commitRows) {
            int last = Math.min(config.customers,
                first + config.commitRows - 1);
            String select = "SELECT " + w + "," + d + ",N," +
                "'FIRST-' || CAST(N AS VARCHAR(10)),'OE'," +
                "'LAST-' || CAST(MOD(N-1,1000) AS VARCHAR(10))," +
                "'STREET-1','STREET-2','NATIVELITE','NL','123456789'," +
                "CAST(N AS CHAR(16)),TIMESTAMP '" + FIXED_TS + "'," +
                "CASE WHEN MOD(N,10)=0 THEN 'BC' ELSE 'GC' END,50000.00," +
                "CAST(MOD(N * 17,5000) AS DECIMAL(8,4)) / 10000," +
                "-10.00,10.00,1,0,'CUSTOMER-DATA' FROM TPCC_LOAD_NUMBER " +
                "WHERE N BETWEEN " + first + " AND " + last;
            worker.executeSetChunk("TPCC_CUSTOMER", select);
          }
        }
      });
      requireCount("TPCC_CUSTOMER", customerTotal);
      System.out.println("loader table=TPCC_CUSTOMER rows=" + customerTotal +
          " action=loaded");
    }

    resetPartial("TPCC_HISTORY", customerTotal);
    if (count("TPCC_HISTORY") != customerTotal) {
      loadWarehouses("TPCC_HISTORY", (worker, w) -> {
        for (int d = 1; d <= config.districts; d++) {
          long base = ((long) (w - 1) * config.districts + d - 1) *
              config.customers;
          for (int first = 1; first <= config.customers;
               first += config.commitRows) {
            int last = Math.min(config.customers,
                first + config.commitRows - 1);
            String select = "SELECT " + base + "+N,N," + d + "," + w +
                "," + d + "," + w + ",TIMESTAMP '" + FIXED_TS +
                "',10.00,'HISTORY-DATA' FROM TPCC_LOAD_NUMBER WHERE N " +
                "BETWEEN " + first + " AND " + last;
            worker.executeSetChunk("TPCC_HISTORY", select);
          }
        }
      });
      requireCount("TPCC_HISTORY", customerTotal);
      System.out.println("loader table=TPCC_HISTORY rows=" + customerTotal +
          " action=loaded");
    }

    long orderTotal = (long) config.warehouses * config.districts * config.orders;
    resetPartial("TPCC_ORDERS", orderTotal);
    if (count("TPCC_ORDERS") != orderTotal) {
      loadWarehouses("TPCC_ORDERS", (worker, w) -> {
        for (int d = 1; d <= config.districts; d++) {
          String carrier = "CASE WHEN N <= " + (config.orders - config.newOrders) +
              " THEN MOD(N,10)+1 ELSE NULL END";
          String lineCount = "5 + MOD(N*37+" + (w * 17 + d * 13 +
              Math.floorMod(config.seed, 11)) + ",11)";
          for (int first = 1; first <= config.orders;
               first += config.commitRows) {
            int last = Math.min(config.orders,
                first + config.commitRows - 1);
            String select = "SELECT " + w + "," + d + ",N," +
                "MOD(N*37+" + (d * 17) + "," + config.customers + ")+1," +
                "TIMESTAMP '" + FIXED_TS + "'," + carrier + "," + lineCount +
                ",1 FROM TPCC_LOAD_NUMBER WHERE N BETWEEN " + first +
                " AND " + last;
            worker.executeSetChunk("TPCC_ORDERS", select);
          }
        }
      });
      requireCount("TPCC_ORDERS", orderTotal);
      System.out.println("loader table=TPCC_ORDERS rows=" + orderTotal +
          " action=loaded");
    }

    long newOrderTotal = (long) config.warehouses * config.districts *
        config.newOrders;
    resetPartial("TPCC_NEW_ORDER", newOrderTotal);
    if (count("TPCC_NEW_ORDER") != newOrderTotal) {
      loadWarehouses("TPCC_NEW_ORDER", (worker, w) -> {
        for (int d = 1; d <= config.districts; d++) {
          String select = "SELECT " + w + "," + d + ",N FROM " +
              "TPCC_LOAD_NUMBER WHERE N > " + (config.orders - config.newOrders) +
              " AND N <= " + config.orders;
          worker.executeSetChunk("TPCC_NEW_ORDER", select);
        }
      });
      requireCount("TPCC_NEW_ORDER", newOrderTotal);
      System.out.println("loader table=TPCC_NEW_ORDER rows=" + newOrderTotal +
          " action=loaded");
    }

    long stockTotal = (long) config.warehouses * config.items;
    resetPartial("TPCC_STOCK", stockTotal);
    if (count("TPCC_STOCK") != stockTotal) {
      loadWarehouses("TPCC_STOCK", (worker, w) -> {
        for (int first = 1; first <= config.items; first += config.commitRows) {
          int last = Math.min(config.items, first + config.commitRows - 1);
          StringBuilder select = new StringBuilder("SELECT ").append(w)
              .append(",N,MOD(N*37,91)+10");
          for (int district = 1; district <= 10; district++)
            select.append(",'DIST-").append(district).append("'");
          select.append(",0,0,0,CASE WHEN MOD(N,10)=0 THEN 'STOCK-ORIGINAL' ")
              .append("ELSE 'STOCK-DATA' END FROM TPCC_LOAD_NUMBER WHERE N ")
              .append("BETWEEN ").append(first).append(" AND ").append(last);
          worker.executeSetChunk("TPCC_STOCK", select.toString());
        }
      });
      requireCount("TPCC_STOCK", stockTotal);
      System.out.println("loader table=TPCC_STOCK rows=" + stockTotal +
          " action=loaded");
    }

    long orderLineTotal = expectedOrderLines();
    resetPartial("TPCC_ORDER_LINE", orderLineTotal);
    if (count("TPCC_ORDER_LINE") != orderLineTotal) {
      loadWarehouses("TPCC_ORDER_LINE", (worker, w) -> {
        for (int d = 1; d <= config.districts; d++) {
          int constant = w * 17 + d * 13 + Math.floorMod(config.seed, 11);
          int ordersPerCommit = Math.max(1, config.commitRows / 15);
          for (int first = 1; first <= config.orders;
               first += ordersPerCommit) {
            int last = Math.min(config.orders, first + ordersPerCommit - 1);
            String select = "SELECT " + w + "," + d + ",O.N,L.N+1," +
                "MOD(O.N*37+(L.N+1)*13+" + (d * 17) + "," + config.items +
                ")+1," + w + ",CASE WHEN O.N <= " +
                (config.orders - config.newOrders) + " THEN TIMESTAMP '" +
                FIXED_TS + "' ELSE NULL END,5,CASE WHEN O.N <= " +
                (config.orders - config.newOrders) +
                " THEN 0.00 ELSE CAST(MOD(O.N*37+(L.N+1)*13,999999)+1 " +
                "AS DECIMAL(10,2))/100 END,'DIST-' || CAST(" + d +
                " AS VARCHAR(2)) FROM TPCC_LOAD_NUMBER O CROSS JOIN " +
                "TPCC_LOAD_SEED L WHERE O.N BETWEEN " + first + " AND " + last +
                " AND L.N < 5 + MOD(O.N*37+" + constant + ",11)";
            worker.executeSetChunk("TPCC_ORDER_LINE", select);
          }
        }
      });
      requireCount("TPCC_ORDER_LINE", orderLineTotal);
      System.out.println("loader table=TPCC_ORDER_LINE rows=" + orderLineTotal +
          " action=loaded");
    }
    executeCommitted("DROP TABLE TPCC_LOAD_NUMBER");
    executeCommitted("DROP TABLE TPCC_LOAD_SEED");
  }

  private void executeSetChunk(String table, String select) throws SQLException {
    try (Statement statement = connection.createStatement()) {
      int affected = statement.executeUpdate("INSERT INTO " + table + " " + select);
      require(affected > 0, table + " set chunk inserted no rows");
    }
    connection.commit();
    afterBatch(table);
  }

  private void load() throws Exception {
    ensureSchema();
    insertRows("TPCC_WAREHOUSE", config.warehouses, index -> {
      int w = (int) index + 1;
      return "(" + w + "," + quote("W" + w) + "," +
          quote("Street 1") + "," + quote("Street 2") + "," +
          quote("NativeLite") + ",'NL','123456789',0.1000,300000.00)";
    });
    insertRows("TPCC_DISTRICT", (long) config.warehouses * config.districts,
        index -> {
          int w = (int) (index / config.districts) + 1;
          int d = (int) (index % config.districts) + 1;
          return "(" + w + "," + d + "," + quote("District" + d) + "," +
              quote("Street 1") + "," + quote("Street 2") + "," +
              quote("NativeLite") + ",'NL','123456789',0.1000,30000.00," +
              (config.orders + 1) + ")";
        });
    loadSetBasedTables();
    verify();
  }

  private void requireCount(String table, long expected) throws SQLException {
    long actual = count(table);
    require(actual == expected, table + " count " + actual + " != " + expected);
  }

  private long queryLong(String sql) throws SQLException {
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery(sql)) {
      require(result.next(), "query returned no row: " + sql);
      return result.getLong(1);
    }
  }

  private void verify() throws SQLException {
    System.out.println("verifier phase=cardinality status=started");
    requireCount("TPCC_WAREHOUSE", config.warehouses);
    requireCount("TPCC_DISTRICT", (long) config.warehouses * config.districts);
    long customers = (long) config.warehouses * config.districts * config.customers;
    requireCount("TPCC_CUSTOMER", customers);
    requireCount("TPCC_HISTORY", customers);
    requireCount("TPCC_ORDERS",
        (long) config.warehouses * config.districts * config.orders);
    requireCount("TPCC_NEW_ORDER",
        (long) config.warehouses * config.districts * config.newOrders);
    requireCount("TPCC_ITEM", config.items);
    requireCount("TPCC_STOCK", (long) config.warehouses * config.items);
    requireCount("TPCC_ORDER_LINE", expectedOrderLines());
    System.out.println("verifier phase=cardinality status=passed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_DISTRICT WHERE D_NEXT_O_ID <> " +
        (config.orders + 1)) == 0, "district next-order invariant failed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_CUSTOMER C LEFT JOIN " +
        "TPCC_HISTORY H ON C.C_W_ID=H.H_C_W_ID AND C.C_D_ID=H.H_C_D_ID " +
        "AND C.C_ID=H.H_C_ID WHERE H.H_ID IS NULL") == 0,
        "customer/history invariant failed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_ORDERS O LEFT JOIN " +
        "TPCC_ORDER_LINE L ON O.O_W_ID=L.OL_W_ID AND O.O_D_ID=L.OL_D_ID " +
        "AND O.O_ID=L.OL_O_ID WHERE L.OL_NUMBER IS NULL") == 0,
        "orders/order-line invariant failed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_ORDER_LINE L LEFT JOIN " +
        "TPCC_ORDERS O ON L.OL_W_ID=O.O_W_ID AND L.OL_D_ID=O.O_D_ID " +
        "AND L.OL_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
        "order-line/orders invariant failed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_NEW_ORDER N LEFT JOIN " +
        "TPCC_ORDERS O ON N.NO_W_ID=O.O_W_ID AND N.NO_D_ID=O.O_D_ID " +
        "AND N.NO_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
        "new-order/orders invariant failed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_STOCK S LEFT JOIN TPCC_ITEM I " +
        "ON S.S_I_ID=I.I_ID WHERE I.I_ID IS NULL") == 0,
        "item/stock invariant failed");
    require(queryLong("SELECT COUNT(*) FROM TPCC_ORDER_LINE L LEFT JOIN " +
        "TPCC_STOCK S ON L.OL_SUPPLY_W_ID=S.S_W_ID AND L.OL_I_ID=S.S_I_ID " +
        "WHERE S.S_I_ID IS NULL") == 0,
        "order-line/stock invariant failed");
    System.out.println("verifier phase=relationships status=passed");
    System.out.println(reportJson());
  }

  private String reportJson() {
    return "{\"contract_version\":1,\"scale\":\"" + config.scale +
        "\",\"seed\":" + config.seed + ",\"warehouses\":" +
        config.warehouses + ",\"rows\":{\"WAREHOUSE\":" + config.warehouses +
        ",\"DISTRICT\":" + ((long) config.warehouses * config.districts) +
        ",\"CUSTOMER\":" + ((long) config.warehouses * config.districts *
        config.customers) + ",\"HISTORY\":" +
        ((long) config.warehouses * config.districts * config.customers) +
        ",\"NEW_ORDER\":" + ((long) config.warehouses * config.districts *
        config.newOrders) + ",\"ORDER\":" +
        ((long) config.warehouses * config.districts * config.orders) +
        ",\"ORDER_LINE\":" + expectedOrderLines() + ",\"ITEM\":" +
        config.items + ",\"STOCK\":" + ((long) config.warehouses * config.items) +
        "},\"consistency\":\"pass\"}";
  }

  public static void main(String[] args) throws Exception {
    require(args.length == 6,
        "usage: NativeLiteTpcc URL load|verify qualification|smoke PROPERTIES SCHEMA REPORT");
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    Config config = new Config(loadProperties(Paths.get(args[3])), args[2]);
    try (Connection connection = DriverManager.getConnection(args[0], USER, "")) {
      connection.setAutoCommit(false);
      NativeLiteTpcc tpcc = new NativeLiteTpcc(connection, args[0], config,
          Paths.get(args[4]));
      if ("load".equals(args[1])) tpcc.load();
      else if ("verify".equals(args[1])) tpcc.verify();
      else throw new IllegalArgumentException("unknown command: " + args[1]);
      Files.write(Paths.get(args[5]), (tpcc.reportJson() + "\n").getBytes(
          StandardCharsets.UTF_8));
    }
  }
}
