import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** M14E proof that independent T4 sessions enter compiler/executor together. */
public final class NativeLiteTpccConcurrency {
  private static final String USER = "DB__ROOT";

  private static void require(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }

  private static Connection connect(String url) throws SQLException {
    Connection connection = DriverManager.getConnection(url, USER, "");
    connection.setAutoCommit(false);
    return connection;
  }

  private static int queryInt(Connection connection, String sql)
      throws SQLException {
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery(sql)) {
      require(result.next(), "query returned no row: " + sql);
      int value = result.getInt(1);
      require(!result.next(), "query returned extra rows: " + sql);
      return value;
    }
  }

  private static void execute(Connection connection, String sql)
      throws SQLException {
    try (Statement statement = connection.createStatement()) {
      statement.execute(sql);
    }
  }

  private static void sessionSchemaIsolation(String url) throws Exception {
    try (Connection first = connect(url); Connection second = connect(url)) {
      execute(first, "CREATE SCHEMA M14EA");
      execute(first, "SET SCHEMA M14EA");
      execute(first, "CREATE TABLE SESSION_STATE(ID INT NOT NULL PRIMARY KEY)");
      first.rollback();
      execute(first, "INSERT INTO SESSION_STATE VALUES (101)");
      first.commit();
      execute(second, "CREATE SCHEMA M14EB");
      execute(second, "SET SCHEMA M14EB");
      execute(second, "CREATE TABLE SESSION_STATE(ID INT NOT NULL PRIMARY KEY)");
      second.rollback();
      execute(second, "INSERT INTO SESSION_STATE VALUES (202)");
      second.commit();
      require(queryInt(first, "SELECT ID FROM SESSION_STATE") == 101,
          "first session schema leaked");
      require(queryInt(second, "SELECT ID FROM SESSION_STATE") == 202,
          "second session schema leaked");
      first.rollback();
      second.rollback();
    }
  }

  private static void concurrentExecutor(String url) throws Exception {
    try (Connection first = connect(url); Connection second = connect(url)) {
      CountDownLatch ready = new CountDownLatch(2);
      CountDownLatch start = new CountDownLatch(1);
      AtomicReference<Throwable> failure = new AtomicReference<>();
      Thread[] workers = new Thread[2];
      Connection[] connections = {first, second};
      for (int index = 0; index < workers.length; index++) {
        final Connection connection = connections[index];
        workers[index] = new Thread(() -> {
          try {
            ready.countDown();
            start.await();
            require(queryInt(connection,
                "SELECT COUNT(*) FROM TPCC_ORDER_LINE " +
                "WHERE 1=1 /* M14E_OVERLAP */") == 2002,
                "concurrent executor returned the wrong count");
            connection.rollback();
          } catch (Throwable problem) {
            failure.compareAndSet(null, problem);
          }
        }, "m14e-executor-" + index);
        workers[index].start();
      }
      ready.await();
      long started = System.nanoTime();
      start.countDown();
      for (Thread worker : workers) worker.join(10000);
      long elapsedMillis = (System.nanoTime() - started) / 1_000_000L;
      for (Thread worker : workers)
        require(!worker.isAlive(), worker.getName() + " did not finish");
      if (failure.get() != null)
        throw new AssertionError("concurrent executor failed", failure.get());
      require(queryInt(first, "SELECT NATIVE_LITE_EXECUTOR_OVERLAP()") >= 2,
          "server did not observe compiler/executor overlap");
      require(elapsedMillis < 10000,
          "overlap probe did not complete within 10 seconds");
      first.rollback();
    }
  }

  private static void diagnosticAndPeerSurvival(String url) throws Exception {
    try (Connection broken = connect(url); Connection peer = connect(url)) {
      try {
        queryInt(broken, "SELECT M14E_MISSING FROM TPCC_CUSTOMER");
        throw new AssertionError("invalid query unexpectedly succeeded");
      } catch (SQLException expected) {
        require(expected.getMessage() != null &&
            expected.getMessage().contains("M14E_MISSING"),
            "diagnostic leaked or lost statement identity: " +
            expected.getMessage());
      }
      require(queryInt(peer, "SELECT COUNT(*) FROM TPCC_CUSTOMER") == 200,
          "peer session failed after independent diagnostic");
      peer.rollback();
      broken.rollback();
    }
  }

  public static void main(String[] args) throws Exception {
    require(args.length == 2,
        "usage: NativeLiteTpccConcurrency JDBC_URL REPORT");
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    sessionSchemaIsolation(args[0]);
    for (int iteration = 0; iteration < 5; iteration++)
      concurrentExecutor(args[0]);
    diagnosticAndPeerSurvival(args[0]);
    String report = "{\"contract_version\":1," +
        "\"compiler_executor_overlap\":\"pass\"," +
        "\"minimum_observed_overlap\":2,\"race_iterations\":5," +
        "\"session_schema_isolation\":\"pass\"," +
        "\"diagnostic_isolation\":\"pass\"," +
        "\"peer_survival\":\"pass\",\"ddl_policy\":\"serialized\"," +
        "\"utility_policy\":\"serialized\"," +
        "\"request_state\":\"session_thread_owned\"}\n";
    Files.write(Paths.get(args[1]), report.getBytes(StandardCharsets.UTF_8));
    System.out.print(report);
  }
}
