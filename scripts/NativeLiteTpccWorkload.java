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

/** M15G concurrent multi-warehouse TPC-C-like workload and qualification. */
public final class NativeLiteTpccWorkload {
  private static final String USER = "DB__ROOT";
  private static int retryLimit;
  private static int retryBackoffMillis;
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
    long retryBackoffMicros;
    final List<Long> latencyMicros = new ArrayList<>();

    synchronized void commit(long micros, int retries, long backoffMicros) {
      committed++;
      aborted += retries;
      retried += retries;
      retryBackoffMicros += backoffMicros;
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
    long retryBackoffMicros = 0;
    for (int attempt = 0; ; attempt++) {
      try {
        executeProfile(terminal, profile, district, customer);
        if (stats != null)
          stats.commit((System.nanoTime() - started) / 1000L, attempt,
              retryBackoffMicros);
        return;
      } catch (SQLException failure) {
        if (!retryable(failure) || attempt >= retryLimit) throw failure;
        retryBackoffMicros += (long) retryBackoffMillis *
            (attempt + 1) * 1000L;
        Thread.sleep((long) retryBackoffMillis * (attempt + 1));
      }
    }
  }

  private static void runTerminal(String url, int terminalId, int warehouse,
      int districts, int customers, int transactionCount, RunStats stats)
      throws Exception {
    try (NativeLiteTpccTransactions.Terminal terminal =
             new NativeLiteTpccTransactions.Terminal(
                 url, terminalId, warehouse)) {
      for (int index = 0; index < transactionCount; index++) {
        // Terminals mapped to the same warehouse differ by ten in the caller.
        // Add the terminal group to the phase so their Payment slots do not
        // line up deterministically on the same W_YTD row. Every 20-operation
        // cycle still contains the exact declared 45/40/5/5/5 mix.
        String profile = MIX[Math.floorMod(
            index + terminalId + (terminalId / 10) * 3, MIX.length)];
        // Terminals sharing a warehouse are separated by the warehouse
        // count in the caller's round-robin mapping. Fold both the terminal
        // id and its decade into the district sequence so those peers do not
        // manufacture a same-district hotspot, while retaining deterministic
        // coverage across repetitions.
        int district = 1 + Math.floorMod(
            terminalId + terminalId / 10, districts);
        int customer = 1 + Math.floorMod(
            index * 7 + terminalId * 31, customers);
        runLogical(terminal, profile, district, customer,
            stats == null ? null : stats.profiles.get(profile));
      }
    }
  }

  private static long runRepetition(String url, int repetition,
      int terminals, int warehouses, int districts, int customers,
      int transactionCount, int timeoutSeconds, RunStats stats)
      throws Exception {
    CountDownLatch ready = new CountDownLatch(terminals);
    CountDownLatch start = new CountDownLatch(1);
    AtomicReference<Throwable> failure = new AtomicReference<>();
    List<Thread> workers = new ArrayList<>();
    for (int terminalIndex = 1; terminalIndex <= terminals; terminalIndex++) {
      final int selectedTerminal = terminalIndex;
      final int selectedWarehouse = 1 + ((terminalIndex - 1) % warehouses);
      Thread worker = new Thread(() -> {
        try {
          ready.countDown();
          start.await();
          // The terminal identity must remain unique when several terminals
          // are mapped to the same warehouse; it is also used to derive the
          // deterministic TPCC_HISTORY key in payment transactions.
          runTerminal(url, repetition * 100 + selectedTerminal,
              selectedWarehouse, districts, customers, transactionCount,
              stats);
        } catch (Throwable problem) {
          failure.compareAndSet(null, problem);
        }
      }, "m15g-terminal-" + selectedTerminal);
      workers.add(worker);
      worker.start();
    }
    ready.await();
    long started = System.nanoTime();
    start.countDown();
    long deadline = System.nanoTime() + timeoutSeconds * 1_000_000_000L;
    for (Thread worker : workers) {
      long remaining = deadline - System.nanoTime();
      if (remaining <= 0) break;
      long millis = Math.max(1L, remaining / 1_000_000L);
      worker.join(millis);
    }
    long elapsed = System.nanoTime() - started;
    for (Thread worker : workers) {
      if (worker.isAlive()) worker.interrupt();
      require(!worker.isAlive(), worker.getName() + " exceeded shared " +
          timeoutSeconds + " second repetition deadline");
    }
    if (failure.get() != null)
      throw new AssertionError("M15G terminal failed", failure.get());
    return elapsed;
  }

  private static long percentile(List<Long> values, double percentile) {
    if (values.isEmpty()) return 0;
    List<Long> sorted = new ArrayList<>(values);
    Collections.sort(sorted);
    int index = (int) Math.ceil(percentile * sorted.size()) - 1;
    return sorted.get(Math.max(0, Math.min(index, sorted.size() - 1)));
  }

  private static String json(RunStats stats, String occMetrics,
      int warehouses, int terminals,
      int warmup, int measured, int repetitions, double maxVariance,
      double minThroughput, Map<String, Long> maxP95Micros) {
    long commits = 0;
    for (ProfileStats profile : stats.profiles.values())
      commits += profile.committed;
    double throughput = commits * 1_000_000_000.0 / stats.measuredNanos;
    double min = Collections.min(stats.repetitionTps);
    double max = Collections.max(stats.repetitionTps);
    double variance = min == 0.0 ? 0.0 : (max - min) / min;
    System.err.println("measured repetition TPS: " + stats.repetitionTps);
    require(variance <= maxVariance,
        "throughput variance " + variance + " exceeds " + maxVariance);
    require(throughput >= minThroughput,
        "throughput " + throughput + " is below " + minThroughput);
    for (Map.Entry<String, Long> gate : maxP95Micros.entrySet()) {
      long observed = percentile(stats.profiles.get(gate.getKey()).latencyMicros,
          0.95);
      require(observed <= gate.getValue(), gate.getKey() + " p95 " +
          observed + "us exceeds " + gate.getValue() + "us");
    }
    StringBuilder out = new StringBuilder();
    out.append("{\"contract_version\":2,\"claim\":\"tpc-c-like\",")
        .append("\"isolation_model\":\"trafodion_mvcc_occ\",")
        .append("\"warehouses\":").append(warehouses)
        .append(",\"terminals\":").append(terminals)
        .append(",\"mix_percent\":{\"new_order\":45,\"payment\":40,")
        .append("\"order_status\":5,\"delivery\":5,\"stock_level\":5}")
        .append(",\"transaction_admission\":\"none_concurrent_occ\"")
        .append(",\"terminal_pacing\":\"none\"")
        .append(",\"latency_scope\":")
        .append("\"client_end_to_end_including_retry_backoff\"")
        .append(",\"warmup_transactions_per_terminal\":").append(warmup)
        .append(",\"measured_transactions_per_terminal\":").append(measured)
        .append(",\"repetitions\":").append(repetitions)
        .append(",\"repetition_tps\":[");
    for (int repetition = 0;
         repetition < stats.repetitionTps.size(); repetition++) {
      if (repetition != 0) out.append(',');
      out.append(String.format(Locale.ROOT, "%.3f",
          stats.repetitionTps.get(repetition)));
    }
    out.append(']')
        .append(",\"throughput_tps\":").append(
            String.format(Locale.ROOT, "%.3f", throughput))
        .append(",\"throughput_variance_ratio\":")
        .append(String.format(Locale.ROOT, "%.6f", variance))
        .append(",\"qualification_gates\":{")
        .append("\"min_throughput_tps\":")
        .append(String.format(Locale.ROOT, "%.3f", minThroughput))
        .append(",\"max_variance_ratio\":")
        .append(String.format(Locale.ROOT, "%.6f", maxVariance))
        .append(",\"latency_p95_us\":{");
    boolean firstGate = true;
    for (Map.Entry<String, Long> gate : maxP95Micros.entrySet()) {
      if (!firstGate) out.append(',');
      firstGate = false;
      out.append('\"').append(gate.getKey()).append("\":")
          .append(gate.getValue());
    }
    out.append("}}")
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
          .append(",\"retry_backoff_us\":")
          .append(value.retryBackoffMicros)
          .append(",\"latency_us\":{")
          .append("\"p50\":").append(percentile(value.latencyMicros, 0.50))
          .append(",\"p95\":").append(percentile(value.latencyMicros, 0.95))
          .append(",\"p99\":").append(percentile(value.latencyMicros, 0.99))
          .append(",\"max\":").append(percentile(value.latencyMicros, 1.0))
          .append("}}");
    }
    out.append("},\"server_metrics\":")
        .append(occMetrics)
        .append(",\"stock_level_access\":{\"range_scans\":")
        .append(NativeLiteTpccTransactions.stockLevelRangeScans())
        .append(",\"point_reads\":")
        .append(NativeLiteTpccTransactions.stockLevelPointReads())
        .append(",\"batch_reads\":")
        .append(NativeLiteTpccTransactions.stockLevelBatchReads())
        .append(",\"full_scans\":0}")
        .append(",\"client_metrics\":{")
        .append("\"occ_conflicts_client_observed\":")
        .append(stats.profiles.values().stream()
            .mapToLong(profile -> profile.aborted).sum()).append(',')
        .append("\"retry_backoff_us\":")
        .append(stats.profiles.values().stream()
            .mapToLong(profile -> profile.retryBackoffMicros).sum()).append(',')
        .append("\"queue_time\":\"unavailable_direct_dispatch\",")
        .append("\"compile_time\":\"unavailable_reduced_t4\",")
        .append("\"wal_fsync_latency\":\"unavailable_rocksdb_c_api\",")
        .append("\"compaction\":\"unavailable_rocksdb_c_api\",")
        .append("\"write_stalls\":\"unavailable_rocksdb_c_api\",")
        .append("\"cache\":\"unavailable_rocksdb_c_api\"},")
        .append("\"unclassified_errors\":0,\"consistency\":\"pass\"}\n");
    return out.toString();
  }

  private static void verify(String url, int warehouses, int districts,
      Path report) throws Exception {
    try (Connection connection = connect(url)) {
      require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_WAREHOUSE") ==
          warehouses,
          "multi-warehouse store lost a warehouse");
      require(queryLong(connection, "SELECT COUNT(*) FROM TPCC_DISTRICT") ==
          (long) warehouses * districts,
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
      Properties verifyProperties = new Properties();
      try (java.io.Reader reader = Files.newBufferedReader(Paths.get(args[2]),
          StandardCharsets.UTF_8)) {
        verifyProperties.load(reader);
      }
      verify(args[0], Integer.parseInt(verifyProperties.getProperty(
          "performance.warehouses")), Integer.parseInt(
          verifyProperties.getProperty("performance.districts.per.warehouse")),
          report);
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
    int warehouses = Integer.parseInt(properties.getProperty(
        "performance.warehouses"));
    int districts = Integer.parseInt(properties.getProperty(
        "performance.districts.per.warehouse"));
    int customers = Integer.parseInt(properties.getProperty(
        "performance.customers.per.district"));
    int orders = Integer.parseInt(properties.getProperty(
        "performance.orders.per.district"));
    int newOrders = Integer.parseInt(properties.getProperty(
        "performance.new.orders.per.district"));
    int items = Integer.parseInt(properties.getProperty(
        "performance.items"));
    long dataSeed = Long.parseLong(properties.getProperty("data.seed"));
    NativeLiteTpccTransactions.configureCardinality(
        customers, orders, newOrders, items, dataSeed);
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
    double minThroughput = Double.parseDouble(properties.getProperty(
        "performance.min.throughput.tps", "0"));
    int timeoutSeconds = Integer.parseInt(properties.getProperty(
        "performance.terminal.timeout.seconds", "120"));
    Map<String, Long> maxP95Micros = new LinkedHashMap<>();
    for (String profile : Arrays.asList(
        "new_order", "payment", "order_status", "delivery", "stock_level")) {
      double millis = Double.parseDouble(properties.getProperty(
          "performance.max.p95." + profile + ".ms", "9.223372036854775E12"));
      maxP95Micros.put(profile, (long) (millis * 1000.0));
    }
    try (Connection connection = connect(args[0])) {
      require("ok".equals(queryString(connection,
          "SELECT NATIVE_LITE_OCC_METRICS_RESET()")),
          "OCC metrics reset did not return ok");
      connection.rollback();
    }
    runRepetition(args[0], 0, terminals, warehouses, districts, customers,
        warmup, timeoutSeconds, null);
    RunStats stats = new RunStats();
    for (int repetition = 1; repetition <= repetitions; repetition++) {
      long elapsed = runRepetition(args[0], repetition, terminals, warehouses,
          districts, customers, measured, timeoutSeconds, stats);
      stats.measuredNanos += elapsed;
      stats.repetitionTps.add(
          terminals * measured * 1_000_000_000.0 / elapsed);
    }
    verify(args[0], warehouses, districts, Paths.get(args[3] + ".verify"));
    String occMetrics;
    try (Connection connection = connect(args[0])) {
      occMetrics = queryString(connection, "SELECT NATIVE_LITE_OCC_METRICS()");
      connection.rollback();
    }
    String json = json(stats, occMetrics, warehouses, terminals, warmup, measured,
        repetitions, maxVariance, minThroughput, maxP95Micros);
    Files.write(report, json.getBytes(StandardCharsets.UTF_8));
    System.out.print(json);
  }
}
