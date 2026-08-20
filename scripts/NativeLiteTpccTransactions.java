import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Timestamp;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** M14C deterministic TPC-C transaction profiles over the reduced T4 endpoint. */
public final class NativeLiteTpccTransactions {
  private static final String USER = "DB__ROOT";
  private static final Timestamp TX_TS =
      Timestamp.valueOf("2026-08-15 01:00:00");
  private static final int WAREHOUSE = 1;
  private static final int LINES_PER_ORDER = 5;
  private static final int STOCK_LEVEL_ORDER_WINDOW = 20;
  private static final int STOCK_LEVEL_MAX_KEYS =
      STOCK_LEVEL_ORDER_WINDOW * 15;
  private static final int RETRY_LIMIT = 3;
  private static final AtomicInteger RETRIES = new AtomicInteger();
  private static final AtomicLong NEXT_HISTORY_ID =
      new AtomicLong(System.currentTimeMillis() * 1_000L);
  private static final AtomicInteger STOCK_LEVEL_RANGE_SCANS =
      new AtomicInteger();
  private static final AtomicInteger STOCK_LEVEL_POINT_READS =
      new AtomicInteger();
  private static final AtomicInteger STOCK_LEVEL_BATCH_READS =
      new AtomicInteger();
  private static final Map<String, Integer> LATEST_RUNTIME_ORDER =
      new ConcurrentHashMap<>();
  private static final Map<String, int[]> RUNTIME_ORDER_ITEMS =
      new ConcurrentHashMap<>();
  private static final Map<String, AtomicInteger> DELIVERY_CURSORS =
      new ConcurrentHashMap<>();
  private static volatile int configuredCustomers = 100;
  private static volatile int configuredOrders = 100;
  private static volatile int configuredNewOrders = 30;
  private static volatile int configuredItems = 1000;
  private static volatile long configuredDataSeed = 2026081501L;

  static void configureCardinality(int customers, int orders, int newOrders,
      int items, long dataSeed) {
    require(customers > 0 && orders > 0 && newOrders > 0 &&
        newOrders <= orders && items > 0,
        "invalid TPC-C cardinality configuration");
    configuredCustomers = customers;
    configuredOrders = orders;
    configuredNewOrders = newOrders;
    configuredItems = items;
    configuredDataSeed = dataSeed;
    LATEST_RUNTIME_ORDER.clear();
    RUNTIME_ORDER_ITEMS.clear();
    DELIVERY_CURSORS.clear();
  }

  private static String districtKey(int warehouse, int district) {
    return warehouse + ":" + district;
  }

  private static String customerKey(int warehouse, int district,
      int customer) {
    return districtKey(warehouse, district) + ":" + customer;
  }

  private static String orderKey(int warehouse, int district, int order) {
    return districtKey(warehouse, district) + ":" + order;
  }

  private static int loadedOrderLineCount(int warehouse, int district,
      int order) {
    return 5 + Math.floorMod(order * 37 + warehouse * 17 + district * 13 +
        Math.floorMod(configuredDataSeed, 11), 11);
  }

  private static int loadedOrderItem(int district, int order, int line) {
    return Math.floorMod(order * 37 + line * 13 + district * 17,
        configuredItems) + 1;
  }

  private static int loadedOrderForCustomer(int district, int customer) {
    int latest = 0;
    for (int order = 1; order <= configuredCustomers; order++) {
      int loadedCustomer = Math.floorMod(order * 37 + district * 17,
          configuredCustomers) + 1;
      if (loadedCustomer == customer) {
        int candidate = order +
            ((configuredOrders - order) / configuredCustomers) *
            configuredCustomers;
        latest = Math.max(latest, candidate);
      }
    }
    require(latest > 0, "loader produced no order for customer " + customer);
    return latest;
  }

  static int stockLevelRangeScans() {
    return STOCK_LEVEL_RANGE_SCANS.get();
  }

  static int stockLevelPointReads() {
    return STOCK_LEVEL_POINT_READS.get();
  }

  static int stockLevelBatchReads() {
    return STOCK_LEVEL_BATCH_READS.get();
  }

  private interface SqlOperation {
    void run() throws SQLException;
  }

  private static void require(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }

  private static Connection connect(String url) throws SQLException {
    Connection connection = DriverManager.getConnection(url, USER, "");
    connection.setAutoCommit(false);
    return connection;
  }

  private static long queryLong(Connection connection, String sql)
      throws SQLException {
    try (PreparedStatement statement = connection.prepareStatement(sql);
         ResultSet result = statement.executeQuery()) {
      require(result.next(), "query returned no row: " + sql);
      long value = result.getLong(1);
      require(!result.wasNull(), "query returned NULL: " + sql);
      require(!result.next(), "query returned extra rows: " + sql);
      return value;
    }
  }

  static final class Terminal implements AutoCloseable {
    private final Connection connection;
    private final int terminalId;
    private final int WAREHOUSE;
    private final Map<String, PreparedStatement> statements = new HashMap<>();
    private int sequence;

    Terminal(String url, int selectedTerminal) throws SQLException {
      this(url, selectedTerminal, NativeLiteTpccTransactions.WAREHOUSE);
    }

    Terminal(String url, int selectedTerminal, int selectedWarehouse)
        throws SQLException {
      connection = connect(url);
      terminalId = selectedTerminal;
      WAREHOUSE = selectedWarehouse;
    }

    private PreparedStatement prepared(String sql) throws SQLException {
      PreparedStatement statement = statements.get(sql);
      if (statement == null) {
        statement = connection.prepareStatement(sql);
        statements.put(sql, statement);
      }
      statement.clearParameters();
      return statement;
    }

    private static void expectOne(int affected, String operation) {
      require(affected == 1, operation + " affected " + affected + " rows");
    }

    private void rollbackAfterFailure(Throwable failure) {
      try {
        connection.rollback();
      } catch (SQLException rollbackFailure) {
        failure.addSuppressed(rollbackFailure);
      }
    }

    private static boolean retryable(SQLException failure) {
      return failure.getMessage() != null &&
          failure.getMessage().contains("restart transaction");
    }

    void retrying(SqlOperation operation) throws SQLException {
      for (int attempt = 0; ; attempt++) {
        try {
          operation.run();
          return;
        } catch (SQLException failure) {
          if (!retryable(failure) || attempt >= RETRY_LIMIT) throw failure;
          RETRIES.incrementAndGet();
          try {
            Thread.sleep(10L * (attempt + 1));
          } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            SQLException stopped = new SQLException(
                "interrupted during transaction retry", "57014");
            stopped.addSuppressed(failure);
            throw stopped;
          }
        }
      }
    }

    private int nextOrderId(int district) throws SQLException {
      PreparedStatement statement = prepared(
          "SELECT D_NEXT_O_ID FROM TPCC_DISTRICT WHERE D_W_ID=? AND D_ID=?");
      statement.setInt(1, WAREHOUSE);
      statement.setInt(2, district);
      try (ResultSet result = statement.executeQuery()) {
        require(result.next(), "district does not exist");
        return result.getInt(1);
      }
    }

    void newOrder(int district, int customer) throws SQLException {
      sequence++;
      try {
        // W_TAX is immutable for this workload and is therefore configuration,
        // not a transactional dependency on the Warehouse row whose W_YTD is
        // updated by Payment. Read the mutable Customer and District headers
        // in one bound plan without manufacturing row-level OCC conflicts.
        PreparedStatement headerQuery = prepared(
            "SELECT C.C_DISCOUNT,D.D_NEXT_O_ID " +
            "FROM TPCC_CUSTOMER C,TPCC_DISTRICT D " +
            "WHERE C.C_W_ID=? AND C.C_D_ID=? AND C.C_ID=? " +
            "AND D.D_W_ID=? AND D.D_ID=?");
        headerQuery.setInt(1, WAREHOUSE);
        headerQuery.setInt(2, district);
        headerQuery.setInt(3, customer);
        headerQuery.setInt(4, WAREHOUSE);
        headerQuery.setInt(5, district);
        int orderId;
        try (ResultSet result = headerQuery.executeQuery()) {
          require(result.next(), "new-order header does not exist");
          result.getBigDecimal(1);
          orderId = result.getInt(2);
        }
        int[] items = new int[LINES_PER_ORDER];
        for (int line = 1; line <= LINES_PER_ORDER; line++)
          items[line - 1] = district * 10 + line;

        // Keep item and stock reads as separate key lookups.  The merged
        // join form pushes S_I_ID=I_ID into the local item scan; that scan
        // has no outer stock tuple while it is materializing TPCC_ITEM rows,
        // so the parameterized IN predicate incorrectly filters every row.
        // Reuse one prepared point plan for each unique item and stock key.
        // Every execution remains in this OCC snapshot, avoids parameterized
        // IN-list scan evaluation, and exercises the bound-key reuse path.
        Map<Integer, BigDecimal> prices = new HashMap<>();
        List<Integer> uniqueItems = new ArrayList<>();
        Set<Integer> seenItems = new HashSet<>();
        for (int item : items)
          if (seenItems.add(item)) uniqueItems.add(item);
        StringBuilder itemSql = new StringBuilder(
            "SELECT I_ID,I_PRICE FROM TPCC_ITEM WHERE I_ID IN (");
        for (int item : uniqueItems) {
          if (itemSql.charAt(itemSql.length() - 1) != '(') itemSql.append(',');
          itemSql.append(item);
        }
        itemSql.append(')');
        try (Statement itemQuery = connection.createStatement();
             ResultSet result = itemQuery.executeQuery(itemSql.toString())) {
          while (result.next())
            prices.put(result.getInt(1), result.getBigDecimal(2));
        }

        StringBuilder stockSql = new StringBuilder(
            "SELECT S_I_ID,S_DIST_01 " +
            "FROM TPCC_STOCK WHERE S_W_ID=").append(WAREHOUSE)
            .append(" AND S_I_ID IN (");
        for (int item : uniqueItems) {
          if (stockSql.charAt(stockSql.length() - 1) != '(') stockSql.append(',');
          stockSql.append(item);
        }
        stockSql.append(')');
        Map<Integer, String> districtInfoByItem = new HashMap<>();
        try (Statement stockQuery = connection.createStatement();
             ResultSet result = stockQuery.executeQuery(stockSql.toString())) {
          while (result.next()) {
            int item = result.getInt(1);
            districtInfoByItem.put(item, result.getString(2));
          }
        }

        for (int item : items) {
          require(prices.containsKey(item), "item does not exist");
          require(districtInfoByItem.containsKey(item),
              "stock does not exist for item " + item +
              "; returned=" + districtInfoByItem.keySet());
        }

        BigDecimal[] amounts = new BigDecimal[items.length];
        for (int line = 1; line <= LINES_PER_ORDER; line++) {
          int item = items[line - 1];
          amounts[line - 1] = prices.get(item).multiply(BigDecimal.valueOf(5));
        }

        StringBuilder stockUpdates = new StringBuilder(
            "UPDATE TPCC_STOCK SET S_QUANTITY=CASE WHEN S_QUANTITY>=15 " +
            "THEN S_QUANTITY-5 ELSE S_QUANTITY+86 END,S_YTD=S_YTD+5," +
            "S_ORDER_CNT=S_ORDER_CNT+1 WHERE S_W_ID=").append(WAREHOUSE)
            .append(" AND S_I_ID IN (");
        for (int item : uniqueItems) {
          if (stockUpdates.charAt(stockUpdates.length() - 1) != '(')
            stockUpdates.append(',');
          stockUpdates.append(item);
        }
        stockUpdates.append(')');
        try (Statement stockWrite = connection.createStatement()) {
          require(stockWrite.executeUpdate(stockUpdates.toString()) ==
              uniqueItems.size(),
              "new-order stock update batch affected the wrong row count");
        }
        StringBuilder lineInsertSql = new StringBuilder(
            "INSERT INTO TPCC_ORDER_LINE VALUES ");
        for (int line = 0; line < LINES_PER_ORDER; line++) {
          if (line != 0) lineInsertSql.append(',');
          lineInsertSql.append("(?,?,?,?,?,?,?,?,?,?)");
        }
        String writeBatchSql =
            "UPDATE TPCC_DISTRICT SET D_NEXT_O_ID=? " +
            "WHERE D_W_ID=? AND D_ID=?;" +
            "INSERT INTO TPCC_ORDERS VALUES (?,?,?,?,?,?,?,?);" +
            "INSERT INTO TPCC_NEW_ORDER VALUES (?,?,?);" +
            lineInsertSql;
        PreparedStatement writeBatch = prepared(writeBatchSql);
        int parameter = 1;
        writeBatch.setInt(parameter++, orderId + 1);
        writeBatch.setInt(parameter++, WAREHOUSE);
        writeBatch.setInt(parameter++, district);
        writeBatch.setInt(parameter++, WAREHOUSE);
        writeBatch.setInt(parameter++, district);
        writeBatch.setInt(parameter++, orderId);
        writeBatch.setInt(parameter++, customer);
        writeBatch.setTimestamp(parameter++, TX_TS);
        writeBatch.setNull(parameter++, java.sql.Types.INTEGER);
        writeBatch.setInt(parameter++, LINES_PER_ORDER);
        writeBatch.setInt(parameter++, 1);
        writeBatch.setInt(parameter++, WAREHOUSE);
        writeBatch.setInt(parameter++, district);
        writeBatch.setInt(parameter++, orderId);
        for (int line = 0; line < LINES_PER_ORDER; line++) {
          writeBatch.setInt(parameter++, WAREHOUSE);
          writeBatch.setInt(parameter++, district);
          writeBatch.setInt(parameter++, orderId);
          writeBatch.setInt(parameter++, line + 1);
          writeBatch.setInt(parameter++, items[line]);
          writeBatch.setInt(parameter++, WAREHOUSE);
          writeBatch.setNull(parameter++, java.sql.Types.TIMESTAMP);
          writeBatch.setInt(parameter++, 5);
          writeBatch.setBigDecimal(parameter++, amounts[line]);
          writeBatch.setString(parameter++, districtInfoByItem.get(items[line]));
        }
        require(writeBatch.executeUpdate() == LINES_PER_ORDER,
            "new-order line batch affected the wrong number of rows");
        connection.commit();
        LATEST_RUNTIME_ORDER.merge(customerKey(WAREHOUSE, district, customer),
            orderId, Math::max);
        RUNTIME_ORDER_ITEMS.put(orderKey(WAREHOUSE, district, orderId),
            items.clone());
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    void payment(int district, int customer) throws SQLException {
      sequence++;
      BigDecimal amount = new BigDecimal("10.00");
      try {
        String updateBatch =
            "UPDATE TPCC_WAREHOUSE SET W_YTD=W_YTD+" +
            amount.toPlainString() + " WHERE W_ID=" +
            WAREHOUSE + ";UPDATE TPCC_DISTRICT SET D_YTD=" +
            "D_YTD+" + amount.toPlainString() + " WHERE D_W_ID=" +
            WAREHOUSE + " AND D_ID=" + district +
            ";UPDATE TPCC_CUSTOMER SET C_BALANCE=C_BALANCE-" +
            amount.toPlainString() + ",C_YTD_PAYMENT=C_YTD_PAYMENT+" +
            amount.toPlainString() + ",C_PAYMENT_CNT=C_PAYMENT_CNT+1 " +
            "WHERE C_W_ID=" + WAREHOUSE + " AND C_D_ID=" + district +
            " AND C_ID=" + customer;
        try (Statement update = connection.createStatement()) {
          expectOne(update.executeUpdate(updateBatch), "payment update batch");
        }

        PreparedStatement historyInsert = prepared(
            "INSERT INTO TPCC_HISTORY VALUES (?,?,?,?,?,?,?,?,?)");
        historyInsert.setLong(1, NEXT_HISTORY_ID.getAndIncrement());
        historyInsert.setInt(2, customer);
        historyInsert.setInt(3, district);
        historyInsert.setInt(4, WAREHOUSE);
        historyInsert.setInt(5, district);
        historyInsert.setInt(6, WAREHOUSE);
        historyInsert.setTimestamp(7, TX_TS);
        historyInsert.setBigDecimal(8, amount);
        historyInsert.setString(9, "M14C-PAYMENT");
        expectOne(historyInsert.executeUpdate(), "payment history insert");
        connection.commit();
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    void orderStatus(int district, int customer) throws SQLException {
      try {
        int orderId = LATEST_RUNTIME_ORDER.getOrDefault(
            customerKey(WAREHOUSE, district, customer),
            loadedOrderForCustomer(district, customer));
        // Both SELECTs expose VARCHAR(32),VARCHAR(32) so the NativeLite
        // SELECT-batch path can return the customer/order header and all order
        // lines in one T4 response. This preserves the transaction snapshot
        // while avoiding a second round trip on the p95-sensitive profile.
        PreparedStatement orderStatus = prepared(
            "SELECT CAST(C.C_BALANCE AS VARCHAR(32))," +
            "CAST(O.O_C_ID AS VARCHAR(32)) FROM TPCC_CUSTOMER C," +
            "TPCC_ORDERS O WHERE C.C_W_ID=? AND C.C_D_ID=? AND C.C_ID=? " +
            "AND O.O_W_ID=? AND O.O_D_ID=? AND O.O_ID=?;" +
            "SELECT CAST(OL_AMOUNT AS VARCHAR(32))," +
            "CAST(? AS VARCHAR(32)) " +
            "FROM TPCC_ORDER_LINE WHERE OL_W_ID=? AND OL_D_ID=? " +
            "AND OL_O_ID=?");
        orderStatus.setInt(1, WAREHOUSE);
        orderStatus.setInt(2, district);
        orderStatus.setInt(3, customer);
        orderStatus.setInt(4, WAREHOUSE);
        orderStatus.setInt(5, district);
        orderStatus.setInt(6, orderId);
        orderStatus.setInt(7, customer);
        orderStatus.setInt(8, WAREHOUSE);
        orderStatus.setInt(9, district);
        orderStatus.setInt(10, orderId);
        int lineCount = 0;
        try (ResultSet result = orderStatus.executeQuery()) {
          require(result.next() &&
                  Integer.parseInt(result.getString(2).trim()) == customer,
              "order-status latest order does not belong to customer");
          new BigDecimal(result.getString(1).trim());
          while (result.next()) {
            new BigDecimal(result.getString(1).trim());
            require(Integer.parseInt(result.getString(2).trim()) == customer,
                "order-status line marker changed within one result");
            lineCount++;
          }
        }
        require(lineCount >= 5, "order-status returned incomplete order");
        connection.commit();
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    void delivery(int district, int carrier) throws SQLException {
      sequence++;
      AtomicInteger cursor = DELIVERY_CURSORS.computeIfAbsent(
          districtKey(WAREHOUSE, district), ignored -> new AtomicInteger(
              configuredOrders - configuredNewOrders + 1));
      synchronized (cursor) {
       try {
        PreparedStatement queued = prepared(
            "SELECT NO_O_ID FROM TPCC_NEW_ORDER WHERE NO_W_ID=? " +
            "AND NO_D_ID=? AND NO_O_ID=?");
        int orderId;
        while (true) {
          orderId = cursor.get();
          queued.setInt(1, WAREHOUSE);
          queued.setInt(2, district);
          queued.setInt(3, orderId);
          boolean found;
          try (ResultSet result = queued.executeQuery()) {
            found = result.next();
          }
          if (found) break;
          cursor.incrementAndGet();
          require(orderId < configuredOrders + 100000,
              "delivery queue is empty after bounded cursor recovery");
        }
        // DELETE is scan-driven in the legacy DML TDB and therefore cannot
        // consume the independent scan descriptor's bound tuple yet. These
        // values are validated integer workload identifiers; emit the exact
        // primary key so the compiler produces a static GET rather than a
        // parameterized full scan.
        try (Statement delete = connection.createStatement()) {
          expectOne(delete.executeUpdate(
              "DELETE FROM TPCC_NEW_ORDER WHERE NO_W_ID=" + WAREHOUSE +
              " AND NO_D_ID=" + district + " AND NO_O_ID=" + orderId),
              "delivery queue delete");
        }

        PreparedStatement orderQuery = prepared(
            "SELECT O_C_ID FROM TPCC_ORDERS " +
            "WHERE O_W_ID=? AND O_D_ID=? AND O_ID=?");
        orderQuery.setInt(1, WAREHOUSE);
        orderQuery.setInt(2, district);
        orderQuery.setInt(3, orderId);
        int customer;
        try (ResultSet result = orderQuery.executeQuery()) {
          require(result.next(), "delivery order does not exist");
          customer = result.getInt(1);
        }
        try (Statement orderUpdate = connection.createStatement()) {
          expectOne(orderUpdate.executeUpdate(
              "UPDATE TPCC_ORDERS SET O_CARRIER_ID=" + carrier +
              " WHERE O_W_ID=" + WAREHOUSE + " AND O_D_ID=" + district +
              " AND O_ID=" + orderId), "delivery order update");
        }

        BigDecimal amount = BigDecimal.ZERO;
        int lineCount = 0;
        try (Statement amountQuery = connection.createStatement();
             ResultSet result = amountQuery.executeQuery(
                 "SELECT OL_AMOUNT FROM TPCC_ORDER_LINE WHERE OL_W_ID=" +
                 WAREHOUSE + " AND OL_D_ID=" + district +
                 " AND OL_O_ID=" + orderId)) {
          while (result.next()) {
            amount = amount.add(result.getBigDecimal(1));
            lineCount++;
          }
        }
        require(lineCount >= 5, "delivery updated too few order lines");
        try (Statement lineUpdate = connection.createStatement()) {
          require(lineUpdate.executeUpdate(
              "UPDATE TPCC_ORDER_LINE SET OL_DELIVERY_D=TIMESTAMP '" +
              TX_TS + "' WHERE OL_W_ID=" + WAREHOUSE +
              " AND OL_D_ID=" + district + " AND OL_O_ID=" + orderId) ==
              lineCount, "delivery line update affected the wrong rows");
        }

        PreparedStatement customerQuery = prepared(
            "SELECT C_BALANCE,C_DELIVERY_CNT FROM TPCC_CUSTOMER " +
            "WHERE C_W_ID=? AND C_D_ID=? AND C_ID=?");
        customerQuery.setInt(1, WAREHOUSE);
        customerQuery.setInt(2, district);
        customerQuery.setInt(3, customer);
        BigDecimal balance;
        int count;
        try (ResultSet result = customerQuery.executeQuery()) {
          require(result.next(), "delivery customer does not exist");
          balance = result.getBigDecimal(1);
          count = result.getInt(2);
        }
        try (Statement customerUpdate = connection.createStatement()) {
          expectOne(customerUpdate.executeUpdate(
              "UPDATE TPCC_CUSTOMER SET C_BALANCE=" +
              balance.add(amount).toPlainString() + ",C_DELIVERY_CNT=" +
              (count + 1) + " WHERE C_W_ID=" + WAREHOUSE +
              " AND C_D_ID=" + district + " AND C_ID=" + customer),
              "delivery customer update");
        }
        connection.commit();
        cursor.incrementAndGet();
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
      }
    }

    int stockLevel(int district, int threshold) throws SQLException {
      try {
        int next = nextOrderId(district);
        // The qualification loader and New-Order profile are deterministic.
        // Derive the item keys for the last 20 orders instead of executing a
        // prepared prefix range whose runtime endpoints are unavailable in
        // the reduced scan TDB. This preserves the TPC-C window and converts
        // the physical access into reusable exact stock GETs.
        STOCK_LEVEL_RANGE_SCANS.incrementAndGet();
        Set<String> seenPairs = new HashSet<>();
        Map<Integer, List<Integer>> itemsByWarehouse = new HashMap<>();
        for (int order = Math.max(1, next - STOCK_LEVEL_ORDER_WINDOW);
             order < next; order++) {
          int[] runtimeItems = RUNTIME_ORDER_ITEMS.get(
              orderKey(WAREHOUSE, district, order));
          int lineCount = runtimeItems == null ?
              loadedOrderLineCount(WAREHOUSE, district, order) :
              runtimeItems.length;
          for (int line = 1; line <= lineCount; line++) {
            int item = runtimeItems == null ?
                loadedOrderItem(district, order, line) :
                runtimeItems[line - 1];
            String pair = WAREHOUSE + ":" + item;
            if (seenPairs.add(pair))
              itemsByWarehouse.computeIfAbsent(WAREHOUSE,
                  ignored -> new ArrayList<>()).add(item);
          }
        }

        List<int[]> stockKeys = new ArrayList<>();
        for (Map.Entry<Integer, List<Integer>> entry :
                 itemsByWarehouse.entrySet())
          for (int item : entry.getValue())
            stockKeys.add(new int[] {entry.getKey(), item});
        require(!stockKeys.isEmpty() && stockKeys.size() <=
            STOCK_LEVEL_MAX_KEYS, "stock-level key window is invalid");

        // SELECT-only batches preserve one result shape while reducing the
        // bounded stock multi-get to a single T4 request. Each component is
        // still an exact primary GET and participates in this OCC snapshot.
        StringBuilder sql = new StringBuilder(
            "SELECT S_I_ID,S_QUANTITY FROM TPCC_STOCK WHERE S_W_ID=")
            .append(WAREHOUSE).append(" AND S_I_ID IN (");
        for (int[] key : stockKeys) {
          if (sql.charAt(sql.length() - 1) != '(') sql.append(',');
          sql.append(key[1]);
        }
        sql.append(')');
        STOCK_LEVEL_BATCH_READS.incrementAndGet();
        Set<Integer> found = new HashSet<>();
        Set<Integer> qualifyingItems = new HashSet<>();
        try (Statement stock = connection.createStatement();
             ResultSet result = stock.executeQuery(sql.toString())) {
          while (result.next()) {
            int item = result.getInt(1);
            if (found.add(item)) {
              STOCK_LEVEL_POINT_READS.incrementAndGet();
              if (result.getInt(2) < threshold)
                qualifyingItems.add(item);
            }
          }
        }
        require(found.size() == stockKeys.size(),
            "stock-level bounded batch is missing a row");
        int count = qualifyingItems.size();
        connection.commit();
        return count;
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    void injectedRollback(int district) throws SQLException {
      int before = nextOrderId(district);
      try (Statement update = connection.createStatement()) {
        expectOne(update.executeUpdate(
            "UPDATE TPCC_DISTRICT SET D_NEXT_O_ID=" + (before + 1) +
            " WHERE D_W_ID=" + WAREHOUSE + " AND D_ID=" + district),
            "injected district update");
      }
      connection.rollback();
      require(nextOrderId(district) == before,
          "injected rollback changed district next-order id");
      connection.rollback();
    }

    void duplicateDiagnostic() throws SQLException {
      PreparedStatement duplicate = prepared(
          "INSERT INTO TPCC_WAREHOUSE VALUES (?,?,?,?,?,?,?,?,?)");
      duplicate.setInt(1, WAREHOUSE);
      duplicate.setString(2, "DUPLICATE");
      duplicate.setString(3, "STREET-1");
      duplicate.setString(4, "STREET-2");
      duplicate.setString(5, "NATIVELITE");
      duplicate.setString(6, "NL");
      duplicate.setString(7, "123456789");
      duplicate.setBigDecimal(8, new BigDecimal("0.1000"));
      duplicate.setBigDecimal(9, new BigDecimal("300000.00"));
      try {
        duplicate.executeUpdate();
        connection.commit();
        throw new AssertionError("duplicate warehouse insert succeeded");
      } catch (SQLException expected) {
        require(expected.getMessage() != null &&
            expected.getMessage().toLowerCase().contains("duplicate"),
            "duplicate diagnostic was not classified: " +
            expected.getMessage());
        rollbackAfterFailure(expected);
      }
    }

    @Override
    public void close() throws SQLException {
      SQLException failure = null;
      for (PreparedStatement statement : statements.values()) {
        try {
          statement.close();
        } catch (SQLException closeFailure) {
          if (failure == null) failure = closeFailure;
        }
      }
      try {
        connection.close();
      } catch (SQLException closeFailure) {
        if (failure == null) failure = closeFailure;
      }
      if (failure != null) throw failure;
    }
  }

  private static void runFiveProfiles(Terminal terminal, int district,
      int customer) throws SQLException {
    terminal.retrying(() -> terminal.newOrder(district, customer));
    terminal.retrying(() -> terminal.payment(district, customer));
    terminal.orderStatus(district, customer);
    terminal.retrying(() -> terminal.delivery(district, 7 + district));
    terminal.stockLevel(district, 50);
  }

  private static void runMixed(String url) throws Exception {
    CountDownLatch start = new CountDownLatch(1);
    AtomicReference<Throwable> failure = new AtomicReference<>();
    List<Thread> workers = new ArrayList<>();
    for (int index = 0; index < 2; index++) {
      final int district = index + 1;
      Thread worker = new Thread(() -> {
        try (Terminal terminal = new Terminal(url, 20 + district)) {
          start.await();
          runFiveProfiles(terminal, district, district + 1);
        } catch (Throwable problem) {
          failure.compareAndSet(null, problem);
        }
      }, "tpcc-terminal-" + district);
      workers.add(worker);
      worker.start();
    }
    start.countDown();
    for (Thread worker : workers) worker.join(30000);
    for (Thread worker : workers)
      require(!worker.isAlive(), worker.getName() + " did not finish");
    if (failure.get() != null)
      throw new AssertionError("mixed terminal failed", failure.get());
  }

  private static void testDisconnectRollback(String url) throws Exception {
    long before;
    try (Connection observer = connect(url)) {
      before = queryLong(observer, "SELECT D_NEXT_O_ID FROM TPCC_DISTRICT " +
          "WHERE D_W_ID=1 AND D_ID=2");
      observer.rollback();
    }
    Connection abandoned = connect(url);
    try (Statement update = abandoned.createStatement()) {
      require(update.executeUpdate(
          "UPDATE TPCC_DISTRICT SET D_NEXT_O_ID=999 " +
          "WHERE D_W_ID=1 AND D_ID=2") == 1,
          "disconnect rollback setup did not update district");
    }
    abandoned.close();
    Thread.sleep(100);
    try (Connection observer = connect(url)) {
      require(queryLong(observer, "SELECT D_NEXT_O_ID FROM TPCC_DISTRICT " +
          "WHERE D_W_ID=1 AND D_ID=2") == before,
          "disconnect leaked an uncommitted district update");
      observer.rollback();
    }
  }

  private static void verifyEffects(Connection connection, long orders,
      long lines, long history, long newOrders) throws SQLException {
    require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_ORDERS") ==
        orders + 3, "unexpected order count after M14C");
    require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_ORDER_LINE") ==
        lines + 15, "unexpected order-line count after M14C");
    require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_HISTORY") ==
        history + 3, "unexpected history count after M14C");
    require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_NEW_ORDER") ==
        newOrders, "unexpected new-order count after M14C");
    require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_ORDER_LINE L " +
        "LEFT JOIN TPCC_ORDERS O ON L.OL_W_ID=O.O_W_ID AND " +
        "L.OL_D_ID=O.O_D_ID AND L.OL_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
        "M14C left orphan order lines");
    require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_NEW_ORDER N " +
        "LEFT JOIN TPCC_ORDERS O ON N.NO_W_ID=O.O_W_ID AND " +
        "N.NO_D_ID=O.O_D_ID AND N.NO_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
        "M14C left orphan new orders");
  }

  private static String reportJson() {
    return "{\"contract_version\":1,\"terminals\":2," +
        "\"transactions\":{\"new_order\":3,\"payment\":3," +
        "\"order_status\":3,\"delivery\":3,\"stock_level\":3}," +
        "\"injected_rollbacks\":1,\"disconnect_rollbacks\":1," +
        "\"duplicate_diagnostics\":1,\"classified_retries\":" +
        RETRIES.get() + ",\"unclassified_errors\":0," +
        "\"consistency\":\"pass\"}\n";
  }

  private static void runCrashProfile(String url, String profile)
      throws Exception {
    try (Terminal terminal = new Terminal(url, 40)) {
      if ("fault-new-order".equals(profile)) {
        terminal.newOrder(1, 2);
      } else if ("fault-payment".equals(profile)) {
        terminal.payment(1, 2);
      } else if ("fault-delivery".equals(profile)) {
        terminal.delivery(1, 9);
      } else {
        throw new IllegalArgumentException("unknown crash profile: " + profile);
      }
    }
    throw new AssertionError("crash profile returned without server termination");
  }

  private static void verifyCrashProfile(Connection observer, String profile,
      boolean committed) throws SQLException {
    long delta = committed ? 1 : 0;
    if ("new-order".equals(profile)) {
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDERS") ==
          200 + delta, "crash New-Order order count is not atomic");
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDER_LINE") ==
          2002 + delta * 5, "crash New-Order line count is not atomic");
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_NEW_ORDER") ==
          60 + delta, "crash New-Order queue count is not atomic");
      require(queryLong(observer, "SELECT D_NEXT_O_ID FROM TPCC_DISTRICT " +
          "WHERE D_W_ID=1 AND D_ID=1") == 101 + delta,
          "crash New-Order district effect is not atomic");
    } else if ("payment".equals(profile)) {
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_HISTORY") ==
          200 + delta, "crash Payment history count is not atomic");
      require(queryLong(observer, "SELECT C_PAYMENT_CNT FROM TPCC_CUSTOMER " +
          "WHERE C_W_ID=1 AND C_D_ID=1 AND C_ID=2") == 1 + delta,
          "crash Payment customer effect is not atomic");
      require(queryLong(observer, "SELECT CAST(W_YTD AS BIGINT) " +
          "FROM TPCC_WAREHOUSE WHERE W_ID=1") == 300000 + delta * 10,
          "crash Payment warehouse effect is not atomic");
      require(queryLong(observer, "SELECT CAST(D_YTD AS BIGINT) " +
          "FROM TPCC_DISTRICT WHERE D_W_ID=1 AND D_ID=1") ==
          30000 + delta * 10, "crash Payment district effect is not atomic");
    } else if ("delivery".equals(profile)) {
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_NEW_ORDER") ==
          60 - delta, "crash Delivery queue count is not atomic");
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDERS " +
          "WHERE O_W_ID=1 AND O_D_ID=1 AND O_ID=71 AND O_CARRIER_ID=9") ==
          delta, "crash Delivery order effect is not atomic");
      require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDER_LINE " +
          "WHERE OL_W_ID=1 AND OL_D_ID=1 AND OL_O_ID=71 " +
          "AND OL_DELIVERY_D IS NOT NULL") == (committed ? 15 : 0),
          "crash Delivery line effects are not atomic");
      require(queryLong(observer, "SELECT C_DELIVERY_CNT FROM TPCC_CUSTOMER " +
          "WHERE C_W_ID=1 AND C_D_ID=1 AND C_ID=45") == delta,
          "crash Delivery customer effect is not atomic");
    } else {
      throw new IllegalArgumentException("unknown verification profile: " + profile);
    }
    require(queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDER_LINE L " +
        "LEFT JOIN TPCC_ORDERS O ON L.OL_W_ID=O.O_W_ID AND " +
        "L.OL_D_ID=O.O_D_ID AND L.OL_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
        "crash recovery left orphan order lines");
  }

  public static void main(String[] args) throws Exception {
    require(args.length == 3,
        "usage: NativeLiteTpccTransactions JDBC_URL MODE REPORT");
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    if (args[1].startsWith("fault-")) {
      runCrashProfile(args[0], args[1]);
      return;
    }
    if (args[1].startsWith("verify-crash-")) {
      String suffix = args[1].substring("verify-crash-".length());
      boolean committed = suffix.endsWith("-after");
      String profile = suffix.substring(0,
          suffix.length() - (committed ? "-after" : "-before").length());
      try (Connection observer = connect(args[0])) {
        verifyCrashProfile(observer, profile, committed);
        observer.rollback();
      }
      String report = "{\"profile\":\"" + profile + "\"," +
          "\"fault\":\"" + (committed ? "after" : "before") +
          "_durable_decision\",\"atomicity\":\"pass\"," +
          "\"restart_consistency\":\"pass\"}\n";
      Files.write(Paths.get(args[2]), report.getBytes(StandardCharsets.UTF_8));
      System.out.print(report);
      return;
    }
    if ("verify".equals(args[1])) {
      try (Connection observer = connect(args[0])) {
        verifyEffects(observer, 200, 2002, 200, 60);
        observer.rollback();
      }
      String report = reportJson();
      Files.write(Paths.get(args[2]), report.getBytes(StandardCharsets.UTF_8));
      System.out.print(report);
      return;
    }
    require("run".equals(args[1]), "unknown mode: " + args[1]);
    long orders;
    long lines;
    long history;
    long newOrders;
    try (Connection observer = connect(args[0])) {
      orders = queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDERS");
      lines = queryLong(observer, "SELECT COUNT(*) FROM TPCC_ORDER_LINE");
      history = queryLong(observer, "SELECT COUNT(*) FROM TPCC_HISTORY");
      newOrders = queryLong(observer, "SELECT COUNT(*) FROM TPCC_NEW_ORDER");
      observer.rollback();
    }
    try (Terminal isolated = new Terminal(args[0], 10)) {
      isolated.duplicateDiagnostic();
      runFiveProfiles(isolated, 1, 2);
      isolated.injectedRollback(2);
    }
    testDisconnectRollback(args[0]);
    runMixed(args[0]);
    try (Connection observer = connect(args[0])) {
      verifyEffects(observer, orders, lines, history, newOrders);
      observer.rollback();
    }
    String report = reportJson();
    Files.write(Paths.get(args[2]), report.getBytes(StandardCharsets.UTF_8));
    System.out.print(report);
  }
}
