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

/** M14D Level 3 isolation matrix over the reduced T4 endpoint. */
public final class NativeLiteTpccIsolation {
  private static final String USER = "DB__ROOT";

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
      return result.getLong(1);
    }
  }

  private static BigDecimal balance(Connection connection, int customer)
      throws SQLException {
    try (PreparedStatement statement = connection.prepareStatement(
        "SELECT C_BALANCE FROM TPCC_CUSTOMER " +
        "WHERE C_W_ID=1 AND C_D_ID=1 AND C_ID=?")) {
      statement.setInt(1, customer);
      try (ResultSet result = statement.executeQuery()) {
        require(result.next(), "customer does not exist: " + customer);
        return result.getBigDecimal(1);
      }
    }
  }

  private static int deliveryCount(Connection connection, int customer)
      throws SQLException {
    try (PreparedStatement statement = connection.prepareStatement(
        "SELECT C_DELIVERY_CNT FROM TPCC_CUSTOMER " +
        "WHERE C_W_ID=1 AND C_D_ID=1 AND C_ID=?")) {
      statement.setInt(1, customer);
      try (ResultSet result = statement.executeQuery()) {
        require(result.next(), "customer does not exist: " + customer);
        return result.getInt(1);
      }
    }
  }

  private static void updateBalance(Connection connection, int customer,
      BigDecimal value) throws SQLException {
    try (PreparedStatement statement = connection.prepareStatement(
        "UPDATE TPCC_CUSTOMER SET C_BALANCE=? " +
        "WHERE C_W_ID=1 AND C_D_ID=1 AND C_ID=?")) {
      statement.setBigDecimal(1, value);
      statement.setInt(2, customer);
      require(statement.executeUpdate() == 1, "balance update missed customer");
    }
  }

  private static void updateDeliveryCount(Connection connection, int customer,
      int value) throws SQLException {
    try (PreparedStatement statement = connection.prepareStatement(
        "UPDATE TPCC_CUSTOMER SET C_DELIVERY_CNT=? " +
        "WHERE C_W_ID=1 AND C_D_ID=1 AND C_ID=?")) {
      statement.setInt(1, value);
      statement.setInt(2, customer);
      require(statement.executeUpdate() == 1,
          "delivery-count update missed customer");
    }
  }

  private static String expectRestart(Connection connection, String diagnostic)
      throws SQLException {
    long started = System.nanoTime();
    try {
      connection.commit();
      throw new AssertionError("conflicting commit succeeded");
    } catch (SQLException expected) {
      String message = expected.getMessage();
      require(message != null && message.contains("restart transaction"),
          "conflict was not classified: " + message);
      require(message.contains(diagnostic),
          "unexpected conflict diagnostic: " + message);
      long millis = (System.nanoTime() - started) / 1_000_000L;
      require(millis < 5000, "optimistic conflict exceeded 5 seconds");
      return message;
    }
  }

  private static void dirtyRead(String url) throws Exception {
    try (Connection writer = connect(url); Connection observer = connect(url)) {
      BigDecimal original = balance(writer, 21);
      updateBalance(writer, 21, original.add(BigDecimal.ONE));
      require(balance(observer, 21).compareTo(original) == 0,
          "observer saw an uncommitted Payment update");
      observer.rollback();
      writer.rollback();
    }
  }

  private static void dirtyWrite(String url) throws Exception {
    try (Connection first = connect(url); Connection second = connect(url)) {
      BigDecimal original = balance(first, 22);
      require(balance(second, 22).compareTo(original) == 0,
          "same-row snapshots disagree");
      updateBalance(first, 22, original.add(BigDecimal.ONE));
      updateBalance(second, 22, original.add(BigDecimal.TEN));
      first.commit();
      expectRestart(second, "serializable validation failed");
    }
  }

  private static void nonRepeatableRead(String url) throws Exception {
    try (Connection reader = connect(url); Connection writer = connect(url)) {
      BigDecimal original = balance(reader, 23);
      BigDecimal latest = balance(writer, 23);
      updateBalance(writer, 23, latest.add(BigDecimal.ONE));
      writer.commit();
      require(balance(reader, 23).compareTo(original) == 0,
          "reader snapshot changed after committed Payment");
      reader.rollback();
    }
  }

  private static void phantomAndPredicateConflict(String url) throws Exception {
    try (Connection reader = connect(url); Connection writer = connect(url)) {
      long before = queryLong(reader,
          "SELECT COUNT(*) FROM TPCC_HISTORY WHERE H_ID>=990000");
      try (PreparedStatement insert = writer.prepareStatement(
          "INSERT INTO TPCC_HISTORY VALUES (?,?,?,?,?,?,?,?,?)")) {
        insert.setLong(1, 990001L);
        insert.setInt(2, 24);
        insert.setInt(3, 1);
        insert.setInt(4, 1);
        insert.setInt(5, 1);
        insert.setInt(6, 1);
        insert.setTimestamp(7, Timestamp.valueOf("2026-08-16 01:00:00"));
        insert.setBigDecimal(8, BigDecimal.ONE);
        insert.setString(9, "M14D-PHANTOM");
        require(insert.executeUpdate() == 1, "phantom insert failed");
      }
      writer.commit();
      require(queryLong(reader,
          "SELECT COUNT(*) FROM TPCC_HISTORY WHERE H_ID>=990000") == before,
          "reader observed a predicate phantom");
      BigDecimal current = balance(reader, 24);
      updateBalance(reader, 24, current.add(BigDecimal.ONE));
      expectRestart(reader, "serializable validation failed");
    }
    try (Connection observer = connect(url)) {
      require(queryLong(observer,
          "SELECT COUNT(*) FROM TPCC_HISTORY WHERE H_ID=990001") == 1,
          "committed predicate writer effect is missing");
      observer.rollback();
    }
  }

  private static void writeSkew(String url) throws Exception {
    try (Connection first = connect(url); Connection second = connect(url)) {
      int firstBefore = deliveryCount(first, 25);
      int secondBefore = deliveryCount(first, 26);
      require(deliveryCount(second, 25) == firstBefore &&
          deliveryCount(second, 26) == secondBefore,
          "write-skew snapshots disagree");
      updateDeliveryCount(first, 25, firstBefore + 1);
      updateDeliveryCount(second, 26, secondBefore + 1);
      first.commit();
      expectRestart(second, "serializable validation failed");
    }

    // One bounded retry starts from a new snapshot and applies the logical
    // effect once; the aborted attempt was never published.
    try (Connection retry = connect(url)) {
      int current = deliveryCount(retry, 26);
      updateDeliveryCount(retry, 26, current + 1);
      retry.commit();
    }
    try (Connection observer = connect(url)) {
      require(deliveryCount(observer, 25) == 1,
          "first write-skew effect was not exactly once");
      require(deliveryCount(observer, 26) == 1,
          "retried write-skew effect was not exactly once");
      observer.rollback();
    }
  }

  public static void main(String[] args) throws Exception {
    require(args.length == 2,
        "usage: NativeLiteTpccIsolation JDBC_URL REPORT");
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    dirtyRead(args[0]);
    dirtyWrite(args[0]);
    nonRepeatableRead(args[0]);
    phantomAndPredicateConflict(args[0]);
    writeSkew(args[0]);
    String report = "{\"contract_version\":1," +
        "\"isolation_level\":\"serializable\"," +
        "\"mechanism\":\"snapshot_plus_database_sequence_validation\"," +
        "\"conflict_policy\":\"optimistic_abort\"," +
        "\"conflict_timeout_ms\":5000,\"bounded_retry_limit\":1," +
        "\"dirty_read\":\"pass\",\"dirty_write\":\"pass\"," +
        "\"non_repeatable_read\":\"pass\",\"phantom\":\"pass\"," +
        "\"predicate_conflict\":\"pass\",\"write_skew\":\"pass\"," +
        "\"exactly_once_retry\":\"pass\",\"deadlock_policy\":" +
        "\"non_blocking_avoidance\"}\n";
    Files.write(Paths.get(args[1]), report.getBytes(StandardCharsets.UTF_8));
    System.out.print(report);
  }
}
