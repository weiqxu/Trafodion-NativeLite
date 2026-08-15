import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Types;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

public final class NativeLiteT4JdbcTest {
  private static final String USER = "DB__ROOT";
  private static final String TABLE = "M11_T4_T";

  private static void require(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }

  private static Connection connect(String url) throws SQLException {
    return DriverManager.getConnection(url, USER, "");
  }

  private static void sendDriverCancel(Connection connection) throws Exception {
    // Trafodion's current T4 Statement.cancel() implementation does not send
    // STOPSRVR while its request is in progress. Invoke the driver's own
    // InterfaceConnection cancel path so this gate still exercises the real
    // T4_Dcs_Cancel marshal, transport, and reply code.
    Field field = connection.getClass().getDeclaredField("ic_");
    field.setAccessible(true);
    Object interfaceConnection = field.get(connection);
    Method cancel = interfaceConnection.getClass().getDeclaredMethod(
        "cancel", long.class);
    cancel.setAccessible(true);
    cancel.invoke(interfaceConnection, -1L);
  }

  private static void execute(Connection connection, String sql)
      throws SQLException {
    try (Statement statement = connection.createStatement()) {
      statement.execute(sql);
    }
  }

  private static int queryInt(Connection connection, String sql)
      throws SQLException {
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery(sql)) {
      require(result.next(), "query returned no row: " + sql);
      int value = result.getInt(1);
      require(!result.wasNull(), "query returned NULL: " + sql);
      require(!result.next(), "query returned extra rows: " + sql);
      return value;
    }
  }

  private static String queryString(Connection connection, String sql)
      throws SQLException {
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery(sql)) {
      require(result.next(), "query returned no row: " + sql);
      String value = result.getString(1);
      require(!result.next(), "query returned extra rows: " + sql);
      return value;
    }
  }

  private static void testPrepared(Connection connection) throws SQLException {
    try (PreparedStatement insert = connection.prepareStatement(
        "INSERT INTO " + TABLE + " VALUES (?, ?)")) {
      insert.setString(1, "1");
      insert.setString(2, "prepared-one");
      require(insert.executeUpdate() == 1, "prepared INSERT affected != 1");
      insert.setString(1, "2");
      insert.setString(2, "prepared-two");
      require(insert.executeUpdate() == 1,
          "reused prepared INSERT affected != 1");
    }
    try (PreparedStatement select = connection.prepareStatement(
        "SELECT note FROM " + TABLE + " WHERE id = ?")) {
      require(select.getMetaData().getColumnType(1) == Types.VARCHAR,
          "prepared SELECT did not expose VARCHAR metadata");
      select.setString(1, "2");
      try (ResultSet result = select.executeQuery()) {
        require(result.next() && "prepared-two".equals(result.getString(1)),
            "prepared SELECT returned the wrong row");
      }
    }
  }

  private static void testPreparedBatch(Connection connection)
      throws SQLException {
    try (PreparedStatement insert = connection.prepareStatement(
        "INSERT INTO " + TABLE + " VALUES (?, ?)")) {
      insert.setInt(1, 50);
      insert.setString(2, "batch-one");
      insert.addBatch();
      insert.setInt(1, 51);
      insert.setNull(2, Types.VARCHAR);
      insert.addBatch();
      int[] results = insert.executeBatch();
      require(results.length == 2, "prepared batch returned wrong row count");
      for (int result : results) {
        require(result == 1 || result == Statement.SUCCESS_NO_INFO,
            "prepared batch returned " + result);
      }
    }
    require(queryInt(connection, "SELECT COUNT(*) FROM " + TABLE +
        " WHERE id IN (50, 51)") == 2,
        "prepared batch rows were not stored");
    require(queryInt(connection, "SELECT COUNT(*) FROM " + TABLE +
        " WHERE id = 51 AND note IS NULL") == 1,
        "prepared batch NULL was not preserved");
  }

  private static void testOverlappingTransactions(String url)
      throws SQLException {
    try (Connection first = connect(url);
         Connection second = connect(url);
         Connection observer = connect(url)) {
      first.setAutoCommit(false);
      second.setAutoCommit(false);
      execute(first, "INSERT INTO " + TABLE + " VALUES (10, 'first')");
      execute(second, "INSERT INTO " + TABLE + " VALUES (20, 'second')");
      require(queryInt(observer, "SELECT COUNT(*) FROM " + TABLE +
          " WHERE id IN (10, 20)") == 0, "observer saw uncommitted rows");
      second.commit();
      require(queryInt(observer, "SELECT COUNT(*) FROM " + TABLE +
          " WHERE id = 20") == 1, "independent commit was not visible");
      first.rollback();
      require(queryInt(observer, "SELECT COUNT(*) FROM " + TABLE +
          " WHERE id = 10") == 0, "rollback leaked a pending row");
    }
  }

  private static void testCancellationWithPeer(String url) throws Exception {
    try (Connection slow = connect(url);
         Connection peer = connect(url);
         Statement statement = slow.createStatement()) {
      peer.setAutoCommit(false);
      execute(peer, "INSERT INTO " + TABLE + " VALUES (40, 'peer')");
      final SQLException[] cancelled = new SQLException[1];
      Thread worker = new Thread(() -> {
        try {
          statement.executeQuery("SELECT NATIVE_LITE_SLEEP(10000)");
        } catch (SQLException expected) {
          cancelled[0] = expected;
        }
      });
      worker.start();
      Thread.sleep(200);
      sendDriverCancel(slow);
      worker.join(5000);
      require(!worker.isAlive(), "T4 cancellation did not stop the statement");
      require(cancelled[0] != null, "cancelled T4 statement did not fail");
      require("57014".equals(cancelled[0].getSQLState()),
          "cancel returned SQLSTATE " + cancelled[0].getSQLState());
      peer.commit();
      require(queryInt(peer, "SELECT COUNT(*) FROM " + TABLE +
          " WHERE id = 40") == 1, "cancellation damaged the peer session");
    }
  }

  private static void testDisconnectRollback(String url) throws Exception {
    Connection abandoned = connect(url);
    abandoned.setAutoCommit(false);
    execute(abandoned, "INSERT INTO " + TABLE + " VALUES (30, 'pending')");
    abandoned.close();
    Thread.sleep(100);
    try (Connection observer = connect(url)) {
      require(queryInt(observer, "SELECT COUNT(*) FROM " + TABLE +
          " WHERE id = 30") == 0, "disconnect did not roll back");
    }
  }

  private static void testMetadata(Connection connection) throws SQLException {
    DatabaseMetaData metadata = connection.getMetaData();
    try (ResultSet tables = metadata.getTables(
        "TRAFODION", "SEABASE", TABLE, new String[]{"TABLE"})) {
      require(tables.next(), "getTables did not return the NativeLite table");
      require(TABLE.equals(tables.getString("TABLE_NAME")),
          "getTables returned the wrong table");
      require(!tables.next(), "getTables returned duplicate rows");
    }
    int columns = 0;
    try (ResultSet result = metadata.getColumns(
        "TRAFODION", "SEABASE", TABLE, "%")) {
      while (result.next()) columns++;
    }
    require(columns == 2, "getColumns returned " + columns + " columns");
    try (ResultSet keys = metadata.getPrimaryKeys(
        "TRAFODION", "SEABASE", TABLE)) {
      require(keys.next() && "ID".equals(keys.getString("COLUMN_NAME")),
          "getPrimaryKeys did not return ID");
      require(!keys.next(), "getPrimaryKeys returned duplicate rows");
    }
  }

  private static void testResultTypes(Connection connection)
      throws SQLException {
    try (ResultSet result = connection.createStatement().executeQuery(
        "SELECT id, note FROM " + TABLE + " WHERE id = 1")) {
      require(result.next() && result.getInt(1) == 1,
          "typed integer result was not preserved");
      require(result.getMetaData().getColumnType(1) == Types.INTEGER,
          "integer descriptor did not map to JDBC INTEGER");
    }
  }

  private static void runMain(String url) throws Exception {
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    try (Connection connection = connect(url)) {
      require("ok".equals(queryString(connection,
          "SELECT NATIVE_LITE_HEALTH()")), "health query failed");
      execute(connection, "CREATE TABLE " + TABLE +
          "(id INT NOT NULL PRIMARY KEY, note VARCHAR(80))");
      testPrepared(connection);
      testPreparedBatch(connection);
      testResultTypes(connection);
      testMetadata(connection);
      execute(connection, "INSERT INTO " + TABLE + " VALUES (99, 'restart')");
    }
    testOverlappingTransactions(url);
    testCancellationWithPeer(url);
    testDisconnectRollback(url);
  }

  private static void verifyRestart(String url) throws Exception {
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    try (Connection connection = connect(url)) {
      require(queryInt(connection, "SELECT COUNT(*) FROM " + TABLE +
          " WHERE id = 99") == 1, "committed row did not survive restart");
      testMetadata(connection);
    }
  }

  public static void main(String[] args) throws Exception {
    require(args.length == 2, "usage: NativeLiteT4JdbcTest URL main|restart");
    if ("main".equals(args[1])) runMain(args[0]);
    else if ("restart".equals(args[1])) verifyRestart(args[0]);
    else throw new IllegalArgumentException("unknown mode: " + args[1]);
  }
}
