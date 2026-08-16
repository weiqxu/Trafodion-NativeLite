import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Timestamp;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** M14C deterministic TPC-C transaction profiles over the reduced T4 endpoint. */
public final class NativeLiteTpccTransactions {
  private static final String USER = "DB__ROOT";
  private static final Timestamp TX_TS =
      Timestamp.valueOf("2026-08-15 01:00:00");
  private static final int WAREHOUSE = 1;
  private static final int LINES_PER_ORDER = 5;
  private static final int STOCK_LEVEL_ORDER_WINDOW = 20;
  private static final int RETRY_LIMIT = 3;
  private static final AtomicInteger RETRIES = new AtomicInteger();
  private static final AtomicInteger STOCK_LEVEL_RANGE_SCANS =
      new AtomicInteger();
  private static final AtomicInteger STOCK_LEVEL_POINT_READS =
      new AtomicInteger();

  static int stockLevelRangeScans() {
    return STOCK_LEVEL_RANGE_SCANS.get();
  }

  static int stockLevelPointReads() {
    return STOCK_LEVEL_POINT_READS.get();
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
        int orderId = nextOrderId(district);
        PreparedStatement warehouse = prepared(
            "SELECT W_TAX FROM TPCC_WAREHOUSE WHERE W_ID=?");
        warehouse.setInt(1, WAREHOUSE);
        try (ResultSet result = warehouse.executeQuery()) {
          require(result.next(), "warehouse does not exist");
          result.getBigDecimal(1);
        }
        PreparedStatement customerQuery = prepared(
            "SELECT C_DISCOUNT FROM TPCC_CUSTOMER " +
            "WHERE C_W_ID=? AND C_D_ID=? AND C_ID=?");
        customerQuery.setInt(1, WAREHOUSE);
        customerQuery.setInt(2, district);
        customerQuery.setInt(3, customer);
        try (ResultSet result = customerQuery.executeQuery()) {
          require(result.next(), "customer does not exist");
          result.getBigDecimal(1);
        }
        PreparedStatement districtUpdate = prepared(
            "UPDATE TPCC_DISTRICT SET D_NEXT_O_ID=? " +
            "WHERE D_W_ID=? AND D_ID=?");
        districtUpdate.setInt(1, orderId + 1);
        districtUpdate.setInt(2, WAREHOUSE);
        districtUpdate.setInt(3, district);
        expectOne(districtUpdate.executeUpdate(), "new-order district update");

        PreparedStatement orderInsert = prepared(
            "INSERT INTO TPCC_ORDERS VALUES (?,?,?,?,?,?,?,?)");
        orderInsert.setInt(1, WAREHOUSE);
        orderInsert.setInt(2, district);
        orderInsert.setInt(3, orderId);
        orderInsert.setInt(4, customer);
        orderInsert.setTimestamp(5, TX_TS);
        orderInsert.setNull(6, java.sql.Types.INTEGER);
        orderInsert.setInt(7, LINES_PER_ORDER);
        orderInsert.setInt(8, 1);
        expectOne(orderInsert.executeUpdate(), "new-order order insert");

        PreparedStatement newOrderInsert = prepared(
            "INSERT INTO TPCC_NEW_ORDER VALUES (?,?,?)");
        newOrderInsert.setInt(1, WAREHOUSE);
        newOrderInsert.setInt(2, district);
        newOrderInsert.setInt(3, orderId);
        expectOne(newOrderInsert.executeUpdate(), "new-order queue insert");

        for (int line = 1; line <= LINES_PER_ORDER; line++) {
          int item = district * 10 + line;
          PreparedStatement itemQuery = prepared(
              "SELECT I_PRICE FROM TPCC_ITEM WHERE I_ID=?");
          itemQuery.setInt(1, item);
          BigDecimal price;
          try (ResultSet result = itemQuery.executeQuery()) {
            require(result.next(), "item does not exist");
            price = result.getBigDecimal(1);
          }
          PreparedStatement stockQuery = prepared(
              "SELECT S_QUANTITY,S_YTD,S_ORDER_CNT,S_DIST_01 FROM TPCC_STOCK " +
              "WHERE S_W_ID=? AND S_I_ID=?");
          stockQuery.setInt(1, WAREHOUSE);
          stockQuery.setInt(2, item);
          int quantity;
          int ytd;
          int orderCount;
          String districtInfo;
          try (ResultSet result = stockQuery.executeQuery()) {
            require(result.next(), "stock does not exist");
            quantity = result.getInt(1);
            ytd = result.getInt(2);
            orderCount = result.getInt(3);
            districtInfo = result.getString(4);
          }
          int adjusted = quantity >= 15 ? quantity - 5 : quantity + 86;
          PreparedStatement stockUpdate = prepared(
              "UPDATE TPCC_STOCK SET S_QUANTITY=?,S_YTD=?,S_ORDER_CNT=? " +
              "WHERE S_W_ID=? AND S_I_ID=?");
          stockUpdate.setInt(1, adjusted);
          stockUpdate.setInt(2, ytd + 5);
          stockUpdate.setInt(3, orderCount + 1);
          stockUpdate.setInt(4, WAREHOUSE);
          stockUpdate.setInt(5, item);
          expectOne(stockUpdate.executeUpdate(), "new-order stock update");

          PreparedStatement lineInsert = prepared(
              "INSERT INTO TPCC_ORDER_LINE VALUES (?,?,?,?,?,?,?,?,?,?)");
          lineInsert.setInt(1, WAREHOUSE);
          lineInsert.setInt(2, district);
          lineInsert.setInt(3, orderId);
          lineInsert.setInt(4, line);
          lineInsert.setInt(5, item);
          lineInsert.setInt(6, WAREHOUSE);
          lineInsert.setNull(7, java.sql.Types.TIMESTAMP);
          lineInsert.setInt(8, 5);
          lineInsert.setBigDecimal(9, price.multiply(BigDecimal.valueOf(5)));
          lineInsert.setString(10, districtInfo);
          expectOne(lineInsert.executeUpdate(), "new-order line insert");
        }
        connection.commit();
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    void payment(int district, int customer) throws SQLException {
      sequence++;
      BigDecimal amount = new BigDecimal("10.00");
      try {
        addYtd("TPCC_WAREHOUSE", "W_YTD", "W_ID=?", amount,
            WAREHOUSE, 0);
        addYtd("TPCC_DISTRICT", "D_YTD", "D_W_ID=? AND D_ID=?", amount,
            WAREHOUSE, district);
        PreparedStatement customerQuery = prepared(
            "SELECT C_BALANCE,C_YTD_PAYMENT,C_PAYMENT_CNT FROM TPCC_CUSTOMER " +
            "WHERE C_W_ID=? AND C_D_ID=? AND C_ID=?");
        customerQuery.setInt(1, WAREHOUSE);
        customerQuery.setInt(2, district);
        customerQuery.setInt(3, customer);
        BigDecimal balance;
        BigDecimal ytd;
        int count;
        try (ResultSet result = customerQuery.executeQuery()) {
          require(result.next(), "payment customer does not exist");
          balance = result.getBigDecimal(1);
          ytd = result.getBigDecimal(2);
          count = result.getInt(3);
        }
        PreparedStatement customerUpdate = prepared(
            "UPDATE TPCC_CUSTOMER SET C_BALANCE=?,C_YTD_PAYMENT=?," +
            "C_PAYMENT_CNT=? WHERE C_W_ID=? AND C_D_ID=? AND C_ID=?");
        customerUpdate.setBigDecimal(1, balance.subtract(amount));
        customerUpdate.setBigDecimal(2, ytd.add(amount));
        customerUpdate.setInt(3, count + 1);
        customerUpdate.setInt(4, WAREHOUSE);
        customerUpdate.setInt(5, district);
        customerUpdate.setInt(6, customer);
        expectOne(customerUpdate.executeUpdate(), "payment customer update");

        PreparedStatement historyInsert = prepared(
            "INSERT INTO TPCC_HISTORY VALUES (?,?,?,?,?,?,?,?,?)");
        historyInsert.setLong(1, 900000L + terminalId * 1000L + sequence);
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

    private void addYtd(String table, String column, String predicate,
        BigDecimal amount, int firstKey, int secondKey) throws SQLException {
      PreparedStatement query = prepared("SELECT " + column + " FROM " +
          table + " WHERE " + predicate);
      query.setInt(1, firstKey);
      if (secondKey != 0) query.setInt(2, secondKey);
      BigDecimal current;
      try (ResultSet result = query.executeQuery()) {
        require(result.next(), table + " row does not exist");
        current = result.getBigDecimal(1);
      }
      PreparedStatement update = prepared("UPDATE " + table + " SET " +
          column + "=? WHERE " + predicate);
      update.setBigDecimal(1, current.add(amount));
      update.setInt(2, firstKey);
      if (secondKey != 0) update.setInt(3, secondKey);
      expectOne(update.executeUpdate(), "payment " + table + " update");
    }

    void orderStatus(int district, int customer) throws SQLException {
      try {
        PreparedStatement customerQuery = prepared(
            "SELECT C_BALANCE FROM TPCC_CUSTOMER " +
            "WHERE C_W_ID=? AND C_D_ID=? AND C_ID=?");
        customerQuery.setInt(1, WAREHOUSE);
        customerQuery.setInt(2, district);
        customerQuery.setInt(3, customer);
        try (ResultSet result = customerQuery.executeQuery()) {
          require(result.next(), "order-status customer does not exist");
          result.getBigDecimal(1);
        }
        PreparedStatement latest = prepared(
            "SELECT MAX(O_ID) FROM TPCC_ORDERS " +
            "WHERE O_W_ID=? AND O_D_ID=? AND O_C_ID=?");
        latest.setInt(1, WAREHOUSE);
        latest.setInt(2, district);
        latest.setInt(3, customer);
        int orderId;
        try (ResultSet result = latest.executeQuery()) {
          require(result.next(), "order-status latest query returned no row");
          orderId = result.getInt(1);
          require(!result.wasNull(), "order-status customer has no order");
        }
        PreparedStatement lines = prepared(
            "SELECT COUNT(*) FROM TPCC_ORDER_LINE " +
            "WHERE OL_W_ID=? AND OL_D_ID=? AND OL_O_ID=?");
        lines.setInt(1, WAREHOUSE);
        lines.setInt(2, district);
        lines.setInt(3, orderId);
        try (ResultSet result = lines.executeQuery()) {
          require(result.next() && result.getInt(1) >= 5,
              "order-status returned incomplete order");
        }
        connection.commit();
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    void delivery(int district, int carrier) throws SQLException {
      sequence++;
      try {
        PreparedStatement oldest = prepared(
            "SELECT MIN(NO_O_ID) FROM TPCC_NEW_ORDER " +
            "WHERE NO_W_ID=? AND NO_D_ID=?");
        oldest.setInt(1, WAREHOUSE);
        oldest.setInt(2, district);
        int orderId;
        try (ResultSet result = oldest.executeQuery()) {
          require(result.next(), "delivery queue query returned no row");
          orderId = result.getInt(1);
          require(!result.wasNull(), "delivery queue is empty");
        }
        PreparedStatement delete = prepared(
            "DELETE FROM TPCC_NEW_ORDER WHERE NO_W_ID=? AND NO_D_ID=? " +
            "AND NO_O_ID=?");
        delete.setInt(1, WAREHOUSE);
        delete.setInt(2, district);
        delete.setInt(3, orderId);
        expectOne(delete.executeUpdate(), "delivery queue delete");

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
        PreparedStatement orderUpdate = prepared(
            "UPDATE TPCC_ORDERS SET O_CARRIER_ID=? " +
            "WHERE O_W_ID=? AND O_D_ID=? AND O_ID=?");
        orderUpdate.setInt(1, carrier);
        orderUpdate.setInt(2, WAREHOUSE);
        orderUpdate.setInt(3, district);
        orderUpdate.setInt(4, orderId);
        expectOne(orderUpdate.executeUpdate(), "delivery order update");

        PreparedStatement amountQuery = prepared(
            "SELECT SUM(OL_AMOUNT) FROM TPCC_ORDER_LINE " +
            "WHERE OL_W_ID=? AND OL_D_ID=? AND OL_O_ID=?");
        amountQuery.setInt(1, WAREHOUSE);
        amountQuery.setInt(2, district);
        amountQuery.setInt(3, orderId);
        BigDecimal amount;
        try (ResultSet result = amountQuery.executeQuery()) {
          require(result.next(), "delivery amount query returned no row");
          amount = result.getBigDecimal(1);
          require(amount != null, "delivery order has no lines");
        }
        PreparedStatement linesUpdate = prepared(
            "UPDATE TPCC_ORDER_LINE SET OL_DELIVERY_D=? " +
            "WHERE OL_W_ID=? AND OL_D_ID=? AND OL_O_ID=?");
        linesUpdate.setTimestamp(1, TX_TS);
        linesUpdate.setInt(2, WAREHOUSE);
        linesUpdate.setInt(3, district);
        linesUpdate.setInt(4, orderId);
        require(linesUpdate.executeUpdate() >= 5,
            "delivery updated too few order lines");

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
        PreparedStatement customerUpdate = prepared(
            "UPDATE TPCC_CUSTOMER SET C_BALANCE=?,C_DELIVERY_CNT=? " +
            "WHERE C_W_ID=? AND C_D_ID=? AND C_ID=?");
        customerUpdate.setBigDecimal(1, balance.add(amount));
        customerUpdate.setInt(2, count + 1);
        customerUpdate.setInt(3, WAREHOUSE);
        customerUpdate.setInt(4, district);
        customerUpdate.setInt(5, customer);
        expectOne(customerUpdate.executeUpdate(), "delivery customer update");
        connection.commit();
      } catch (SQLException | RuntimeException failure) {
        rollbackAfterFailure(failure);
        throw failure;
      }
    }

    int stockLevel(int district, int threshold) throws SQLException {
      try {
        int next = nextOrderId(district);
        PreparedStatement orderLines = prepared(
            "SELECT OL_SUPPLY_W_ID,OL_I_ID FROM TPCC_ORDER_LINE " +
            "WHERE OL_W_ID=? AND OL_D_ID=? AND OL_O_ID>=? AND OL_O_ID<?");
        orderLines.setInt(1, WAREHOUSE);
        orderLines.setInt(2, district);
        orderLines.setInt(3, Math.max(1, next - STOCK_LEVEL_ORDER_WINDOW));
        orderLines.setInt(4, next);
        STOCK_LEVEL_RANGE_SCANS.incrementAndGet();
        Set<String> seenPairs = new HashSet<>();
        List<int[]> stockKeys = new ArrayList<>();
        try (ResultSet result = orderLines.executeQuery()) {
          while (result.next()) {
            int supplyWarehouse = result.getInt(1);
            int item = result.getInt(2);
            String pair = supplyWarehouse + ":" + item;
            if (seenPairs.add(pair)) {
              stockKeys.add(new int[] {supplyWarehouse, item});
            }
          }
        }

        PreparedStatement stock = prepared(
            "SELECT S_QUANTITY FROM TPCC_STOCK " +
            "WHERE S_W_ID=? AND S_I_ID=?");
        Set<Integer> qualifyingItems = new HashSet<>();
        for (int[] key : stockKeys) {
          STOCK_LEVEL_POINT_READS.incrementAndGet();
          stock.setInt(1, key[0]);
          stock.setInt(2, key[1]);
          try (ResultSet result = stock.executeQuery()) {
            require(result.next(), "stock-level stock row does not exist");
            if (result.getInt(1) < threshold) {
              qualifyingItems.add(key[1]);
            }
          }
        }
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
      PreparedStatement update = prepared(
          "UPDATE TPCC_DISTRICT SET D_NEXT_O_ID=? " +
          "WHERE D_W_ID=? AND D_ID=?");
      update.setInt(1, before + 1);
      update.setInt(2, WAREHOUSE);
      update.setInt(3, district);
      expectOne(update.executeUpdate(), "injected district update");
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
    try (PreparedStatement update = abandoned.prepareStatement(
        "UPDATE TPCC_DISTRICT SET D_NEXT_O_ID=? " +
        "WHERE D_W_ID=? AND D_ID=?")) {
      update.setInt(1, 999);
      update.setInt(2, WAREHOUSE);
      update.setInt(3, 2);
      require(update.executeUpdate() == 1,
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
