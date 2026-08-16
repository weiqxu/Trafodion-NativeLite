import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.ReentrantLock;

/** M14F deterministic multi-warehouse TPC-C-like workload and operations. */
public final class NativeLiteTpccWorkload {
  private static final String USER = "DB__ROOT";
  private static int retryLimit;
  private static int retryBackoffMillis;
  private static final ReentrantLock TRANSACTION_ADMISSION =
      new ReentrantLock(true);
  private static final String[] MIX = {
      "new_order", "payment", "new_order", "payment", "new_order",
      "payment", "new_order", "payment", "new_order", "payment",
      "new_order", "payment", "new_order", "payment", "new_order",
      "payment", "new_order", "order_status", "delivery", "stock_level"
  };

  private static final class ProfileStats {
    long committed;
    long aborted;
    long retried;
    final List<Long> latencyMicros = new ArrayList<>();

    synchronized void commit(long micros, int retries) {
      committed++;
      aborted += retries;
      retried += retries;
      latencyMicros.add(micros);
    }
  }

  private static final class RunStats {
    final Map<String, ProfileStats> profiles = new LinkedHashMap<>();
    final List<Double> repetitionTps = new ArrayList<>();
    long measuredNanos;

    RunStats() {
      for (String profile : Arrays.asList(
          "new_order", "payment", "order_status", "delivery", "stock_level"))
        profiles.put(profile, new ProfileStats());
    }
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
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery(sql)) {
      require(result.next(), "query returned no row: " + sql);
      return result.getLong(1);
    }
  }

  private static String queryString(Connection connection, String sql)
      throws SQLException {
    try (Statement statement = connection.createStatement();
         ResultSet result = statement.executeQuery(sql)) {
      require(result.next(), "query returned no row: " + sql);
      return result.getString(1);
    }
  }

  private static void executeProfile(NativeLiteTpccTransactions.Terminal terminal,
      String profile, int district, int customer) throws SQLException {
    if ("new_order".equals(profile)) terminal.newOrder(district, customer);
    else if ("payment".equals(profile)) terminal.payment(district, customer);
    else if ("order_status".equals(profile))
      terminal.orderStatus(district, customer);
    else if ("delivery".equals(profile)) terminal.delivery(district, 7);
    else if ("stock_level".equals(profile)) terminal.stockLevel(district, 50);
    else throw new IllegalArgumentException("unknown profile: " + profile);
  }

  private static boolean retryable(SQLException failure) {
    return failure.getMessage() != null &&
        failure.getMessage().contains("restart transaction");
  }

  private static void runLogical(
      NativeLiteTpccTransactions.Terminal terminal, String profile,
      int district, int customer, ProfileStats stats) throws Exception {
    long started = System.nanoTime();
    TRANSACTION_ADMISSION.lockInterruptibly();
    try {
      for (int attempt = 0; ; attempt++) {
        try {
          executeProfile(terminal, profile, district, customer);
          if (stats != null)
            stats.commit((System.nanoTime() - started) / 1000L, attempt);
          return;
        } catch (SQLException failure) {
          if (!retryable(failure) || attempt >= retryLimit) throw failure;
          Thread.sleep((long) retryBackoffMillis * (attempt + 1));
        }
      }
    } finally {
      TRANSACTION_ADMISSION.unlock();
    }
  }

  private static void runTerminal(String url, int terminalId, int warehouse,
      int transactionCount, RunStats stats) throws Exception {
    try (NativeLiteTpccTransactions.Terminal terminal =
             new NativeLiteTpccTransactions.Terminal(
                 url, terminalId, warehouse)) {
      for (int index = 0; index < transactionCount; index++) {
        String profile = MIX[index % MIX.length];
        int district = 1 + (index % 2);
        int customer = 2 + (index * 7 % 90);
        runLogical(terminal, profile, district, customer,
            stats == null ? null : stats.profiles.get(profile));
      }
    }
  }

  private static long runRepetition(String url, int repetition,
      int terminals, int transactionCount, RunStats stats) throws Exception {
    CountDownLatch ready = new CountDownLatch(terminals);
    CountDownLatch start = new CountDownLatch(1);
    AtomicReference<Throwable> failure = new AtomicReference<>();
    List<Thread> workers = new ArrayList<>();
    for (int warehouse = 1; warehouse <= terminals; warehouse++) {
      final int selectedWarehouse = warehouse;
      Thread worker = new Thread(() -> {
        try {
          ready.countDown();
          start.await();
          runTerminal(url, repetition * 100 + selectedWarehouse,
              selectedWarehouse, transactionCount, stats);
        } catch (Throwable problem) {
          failure.compareAndSet(null, problem);
        }
      }, "m14f-terminal-" + warehouse);
      workers.add(worker);
      worker.start();
    }
    ready.await();
    long started = System.nanoTime();
    start.countDown();
    for (Thread worker : workers) worker.join(120000);
    long elapsed = System.nanoTime() - started;
    for (Thread worker : workers)
      require(!worker.isAlive(), worker.getName() + " exceeded 120 seconds");
    if (failure.get() != null)
      throw new AssertionError("M14F terminal failed", failure.get());
    return elapsed;
  }

  private static long percentile(List<Long> values, double percentile) {
    require(!values.isEmpty(), "latency sample is empty");
    List<Long> sorted = new ArrayList<>(values);
    Collections.sort(sorted);
    int index = (int) Math.ceil(percentile * sorted.size()) - 1;
    return sorted.get(Math.max(0, Math.min(index, sorted.size() - 1)));
  }

  private static String json(RunStats stats, int terminals, int warmup,
      int measured, int repetitions, double maxVariance) {
    long commits = 0;
    for (ProfileStats profile : stats.profiles.values())
      commits += profile.committed;
    double throughput = commits * 1_000_000_000.0 / stats.measuredNanos;
    double min = Collections.min(stats.repetitionTps);
    double max = Collections.max(stats.repetitionTps);
    double variance = min == 0.0 ? 0.0 : (max - min) / min;
    require(variance <= maxVariance,
        "throughput variance " + variance + " exceeds " + maxVariance);
    StringBuilder out = new StringBuilder();
    out.append("{\"contract_version\":1,\"claim\":\"tpc-c-like\",")
        .append("\"warehouses\":2,\"terminals\":").append(terminals)
        .append(",\"mix_percent\":{\"new_order\":45,\"payment\":40,")
        .append("\"order_status\":5,\"delivery\":5,\"stock_level\":5}")
        .append(",\"transaction_admission\":")
        .append("\"fair_client_serialized_writers\"")
        .append(",\"terminal_pacing\":\"none\"")
        .append(",\"latency_scope\":")
        .append("\"client_end_to_end_including_admission\"")
        .append(",\"warmup_transactions_per_terminal\":").append(warmup)
        .append(",\"measured_transactions_per_terminal\":").append(measured)
        .append(",\"repetitions\":").append(repetitions)
        .append(",\"throughput_tps\":").append(
            String.format(Locale.ROOT, "%.3f", throughput))
        .append(",\"throughput_variance_ratio\":")
        .append(String.format(Locale.ROOT, "%.6f", variance))
        .append(",\"profiles\":{");
    boolean first = true;
    for (Map.Entry<String, ProfileStats> entry : stats.profiles.entrySet()) {
      if (!first) out.append(',');
      first = false;
      ProfileStats value = entry.getValue();
      out.append('\"').append(entry.getKey()).append("\":{")
          .append("\"committed\":").append(value.committed)
          .append(",\"aborted\":").append(value.aborted)
          .append(",\"retried\":").append(value.retried)
          .append(",\"latency_us\":{")
          .append("\"p50\":").append(percentile(value.latencyMicros, 0.50))
          .append(",\"p95\":").append(percentile(value.latencyMicros, 0.95))
          .append(",\"p99\":").append(percentile(value.latencyMicros, 0.99))
          .append(",\"max\":").append(percentile(value.latencyMicros, 1.0))
          .append("}}");
    }
    out.append("},\"server_metrics\":{")
        .append("\"queue_time\":\"unavailable_direct_dispatch\",")
        .append("\"compile_time\":\"unavailable_reduced_t4\",")
        .append("\"wal_fsync_latency\":\"unavailable_rocksdb_c_api\",")
        .append("\"compaction\":\"unavailable_rocksdb_c_api\",")
        .append("\"write_stalls\":\"unavailable_rocksdb_c_api\",")
        .append("\"cache\":\"unavailable_rocksdb_c_api\"},")
        .append("\"unclassified_errors\":0,\"consistency\":\"pass\"}\n");
    return out.toString();
  }

  private static void verify(String url, Path report) throws Exception {
    try (Connection connection = connect(url)) {
      require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_WAREHOUSE") == 2,
          "multi-warehouse store lost a warehouse");
      require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_DISTRICT") == 4,
          "multi-warehouse store lost a district");
      require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_ORDER_LINE L " +
          "LEFT JOIN TPCC_ORDERS O ON L.OL_W_ID=O.O_W_ID AND " +
          "L.OL_D_ID=O.O_D_ID AND L.OL_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
          "multi-warehouse store has orphan order lines");
      require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_NEW_ORDER N " +
          "LEFT JOIN TPCC_ORDERS O ON N.NO_W_ID=O.O_W_ID AND " +
          "N.NO_D_ID=O.O_D_ID AND N.NO_O_ID=O.O_ID WHERE O.O_ID IS NULL") == 0,
          "multi-warehouse store has orphan new orders");
      connection.rollback();
    }
    Files.write(report, "{\"multi_warehouse_consistency\":\"pass\"}\n"
        .getBytes(StandardCharsets.UTF_8));
  }

  public static void main(String[] args) throws Exception {
    require(args.length == 4,
        "usage: NativeLiteTpccWorkload JDBC_URL MODE PROPERTIES REPORT");
    Class.forName("org.trafodion.jdbc.t4.T4Driver");
    Path report = Paths.get(args[3]);
    if ("checkpoint".equals(args[1])) {
      try (Connection connection = connect(args[0])) {
        require("ok".equals(queryString(connection,
            "SELECT NATIVE_LITE_CHECKPOINT()")), "checkpoint did not return ok");
        connection.rollback();
      }
      Files.write(report, "{\"online_checkpoint\":\"pass\"}\n"
          .getBytes(StandardCharsets.UTF_8));
      return;
    }
    if ("verify".equals(args[1])) {
      verify(args[0], report);
      return;
    }
    if ("watermark".equals(args[1])) {
      try (NativeLiteTpccTransactions.Terminal terminal =
               new NativeLiteTpccTransactions.Terminal(args[0], 999, 1)) {
        try {
          terminal.payment(1, 2);
          throw new AssertionError("disk-watermark transaction succeeded");
        } catch (SQLException expected) {
          require(expected.getMessage() != null &&
              expected.getMessage().contains("watermark"),
              "unexpected watermark diagnostic: " + expected.getMessage());
        }
      }
      Files.write(report, "{\"disk_watermark_rejection\":\"pass\"}\n"
          .getBytes(StandardCharsets.UTF_8));
      return;
    }
    require("run".equals(args[1]), "unknown mode: " + args[1]);
    Properties properties = new Properties();
    try (java.io.Reader reader = Files.newBufferedReader(Paths.get(args[2]),
        StandardCharsets.UTF_8)) {
      properties.load(reader);
    }
    int terminals = Integer.parseInt(properties.getProperty(
        "performance.terminals"));
    require(terminals == Integer.parseInt(properties.getProperty(
        "performance.warehouses")),
        "M14F requires one terminal session per warehouse");
    int warmup = Integer.parseInt(properties.getProperty(
        "performance.warmup.transactions.per.terminal"));
    int measured = Integer.parseInt(properties.getProperty(
        "performance.measure.transactions.per.terminal"));
    int repetitions = Integer.parseInt(properties.getProperty(
        "performance.repetitions"));
    retryLimit = Integer.parseInt(properties.getProperty(
        "performance.retry.limit"));
    retryBackoffMillis = Integer.parseInt(properties.getProperty(
        "performance.retry.backoff.millis"));
    double maxVariance = Double.parseDouble(properties.getProperty(
        "performance.max.throughput.variance.ratio"));
    runRepetition(args[0], 0, terminals, warmup, null);
    RunStats stats = new RunStats();
    for (int repetition = 1; repetition <= repetitions; repetition++) {
      long elapsed = runRepetition(args[0], repetition, terminals, measured,
          stats);
      stats.measuredNanos += elapsed;
      stats.repetitionTps.add(
          terminals * measured * 1_000_000_000.0 / elapsed);
    }
    verify(args[0], Paths.get(args[3] + ".verify"));
    String json = json(stats, terminals, warmup, measured, repetitions,
        maxVariance);
    Files.write(report, json.getBytes(StandardCharsets.UTF_8));
    System.out.print(json);
  }
}
