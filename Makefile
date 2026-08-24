## Top-level Makefile for building Trafodion components

# @@@ START COPYRIGHT @@@
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# @@@ END COPYRIGHT @@@

.PHONY: all lite lite-release lite-client lite-regress lite-metadata lite-legacy-audit lite-regress-inventory lite-m10 lite-m11a lite-m11b lite-m11c lite-m11 lite-m12a lite-m12b lite-m12c lite-m12 lite-m13 lite-m14a lite-m14b lite-m14c lite-m14d lite-m14e lite-m14f lite-m14 lite-m15a lite-m15b lite-m15c lite-m15d lite-m15e lite-m15f lite-m15g lite-m15 lite-m16a lite-m16b lite-m16c lite-m16d lite-m16e lite-m16f lite-m16g lite-m16 lite-m17a lite-m17b lite-m17c lite-m17 lite-m18 lite-m19 lite-m20 lite-m21 lite-m21-tpcc lite-m22a lite-m22b lite-m22c lite-m22d lite-m22e lite-m22f lite-m22g lite-m22h lite-m22
SRCDIR = $(shell echo $(TRAFODION_VER_PROD) | sed -e 's/ /-/g' | tr 'A-Z' 'a-z')
M14_REPORT_DIR ?= /tmp/traf-lite-m14-report
M15_REPORT_DIR ?= /tmp/traf-lite-m15-report
M16_REPORT_DIR ?= /tmp/traf-lite-m16-report
M17_REPORT_DIR ?= /tmp/traf-lite-m17-report
M21_REPORT_DIR ?= /tmp/traf-lite-m21-full-report
M22_REPORT_DIR ?= /tmp/traf-lite-m22-full-report

all:
	@echo "Building all Trafodion components"
	cd core && $(MAKE) all 

lite:
	@echo "Building Trafodion native core with Lite Storage"
	cd core && $(MAKE) lite

lite-release:
	@echo "Building Release Trafodion native core with Lite Storage"
	$(MAKE) SQ_BUILD_TYPE=release lite

lite-client:
	@echo "Running Lite Storage SQLCI-style client checks"
	scripts/test-lite-client.sh

lite-regress:
	@echo "Running Lite Storage SQL regressions"
	core/sql/regress/lite/runregr $(LITE_REGR_TESTS)

lite-metadata:
	@echo "Running lite metadata SQL check"
	scripts/test-lite-metadata.sh

lite-legacy-audit:
	@echo "Auditing lite legacy regression inventory"
	scripts/audit-lite-legacy-regress.sh --report

lite-regress-inventory:
	@echo "Auditing complete upstream regression assets"
	scripts/audit-lite-upstream-regress.sh --report

lite-m10:
	@echo "Running bounded RocksDB-only legacy convergence gate"
	scripts/test-lite-legacy-convergence.sh

lite-m11a:
	@echo "Running M11A session-owned transaction context checks"
	scripts/test-lite-runtime.sh
	scripts/test-lite-context-transactions.sh
	scripts/test-lite-statement-snapshot.sh
	scripts/test-lite-transaction-snapshot.sh
	scripts/test-lite-store-concurrency.sh
	scripts/test-lite-session-transactions.sh

lite-m11b:
	@echo "Running M11B standalone server and restart checks"
	scripts/test-lite-server.sh

lite-m11c:
	@echo "Running M11C Trafodion Type 4 JDBC checks"
	scripts/test-lite-t4jdbc.sh

lite-m11: lite-m11a lite-m11b lite-m11c
	@echo "M11 sessionized server and client protocol checks passed"

lite-m12a:
	@echo "Running M12A backend-neutral storage contract checks"
	scripts/test-lite-storage-contract.sh
	scripts/test-lite-metadata-key-migration.sh

lite-m12b: lite-m12a
	@echo "Running M12B transactional backend comparison"
	scripts/test-lite-storage-backends.sh

lite-m12c: lite-m12b
	@echo "Running M12C recovery and operations checks"
	scripts/test-lite-sql-commit-recovery.sh

lite-m12: lite-m12c
	@echo "M12 transactional storage and recovery checks passed"

lite-m13: lite-m12
	@echo "Running M13 exclusive unified storage checks"
	scripts/test-lite-storage-cutover.sh
	@echo "M13 exclusive unified TransactionDB checks passed"

lite-m14a:
	@echo "Running M14A TPC-C specification and baseline checks"
	scripts/test-lite-tpcc-baseline.sh

lite-m14b: lite-m14a
	@echo "Running M14B TPC-C schema, loader, and integrity checks"
	scripts/test-lite-tpcc-loader.sh

lite-m14c: lite-m14b
	@echo "Running M14C five-profile T4 JDBC transaction checks"
	scripts/test-lite-tpcc-transactions.sh

lite-m14d: lite-m14c
	@echo "Running M14D isolation and crash-recovery checks"
	scripts/test-lite-tpcc-isolation.sh

lite-m14e: lite-m14d
	@echo "Running M14E concurrent compiler/executor checks"
	scripts/test-lite-tpcc-concurrency.sh
	@echo "Running M14E cancellation, disconnect, and peer-survival regression"
	scripts/test-lite-t4jdbc.sh

lite-m14f: lite-m14e
	@echo "Running M14F multi-warehouse performance and operations checks"
	TPCC_M14F_ARTIFACT_DIR="$(TPCC_M14F_ARTIFACT_DIR)" scripts/test-lite-tpcc-performance.sh

lite-m14: TPCC_M14F_ARTIFACT_DIR := $(M14_REPORT_DIR)
lite-m14: lite-m10 lite-m11 lite-m12 lite-m13 lite-m14f
	@echo "Composing M14 aggregate qualification report"
	scripts/test-lite-tpcc-qualification.sh "$(M14_REPORT_DIR)"

lite-m15a:
	@echo "Running M15A Trafodion MVCC/OCC contract checks"
	scripts/test-lite-occ-contract.sh

lite-m15b: lite-m15a
	@echo "Running M15B transaction-wide unified snapshot checks"
	scripts/test-lite-statement-snapshot.sh
	scripts/test-lite-transaction-snapshot.sh

lite-m15c: lite-m15b
	@echo "Running M15C OCC read/write-set checks"
	scripts/test-lite-transaction-snapshot.sh

lite-m15d: lite-m15c
	@echo "Running M15D Trafodion OCC validation matrix"
	scripts/test-lite-occ-validation.sh

lite-m15e: lite-m15d
	@echo "Running M15E transactional secondary-index checks"
	scripts/test-lite-occ-validation.sh

lite-m15f: lite-m15e
	@echo "Running M15F atomic delta-commit checks"
	scripts/test-lite-occ-validation.sh
	scripts/test-lite-sql-commit-recovery.sh

lite-m15g: lite-m15f lite-release
	@echo "Running M15G Release 32-warehouse concurrent OCC qualification"
	TPCC_PROPERTIES="$(CURDIR)/benchmarks/tpcc/m15-production.properties" \
	TPCC_SCALE=multi LITE_BUILD_TYPE=release \
	TPCC_ARTIFACT_DIR="$(M15_REPORT_DIR)" \
		scripts/test-lite-tpcc-performance.sh
	scripts/test-lite-tpcc-occ-qualification.sh "$(M15_REPORT_DIR)"

lite-m15: lite-m15g
	@echo "M15 Trafodion MVCC/OCC and Release TPC-C-like checks passed"

lite-m16a:
	@echo "Running M16A Stock-Level optimization contract checks"
	@test -s benchmarks/tpcc/stock-level-contract.tsv
	@test -s benchmarks/tpcc/m16-stock-level.properties
	@awk -F '\t' '$$1 == "full_scan_policy" && $$2 == "stock_level_zero" { found=1 } END { exit !found }' benchmarks/tpcc/stock-level-contract.tsv

lite-m16b: lite-m16a
	@echo "Running M16B Stock-Level index checks"
	scripts/test-lite-m16b.sh

lite-m16c: lite-m16b
	@echo "Running M16C Stock-Level transaction source checks"
	scripts/test-lite-m16c.sh

lite-m16d: lite-m16c
	@echo "Running M16D optimizer range and correctness checks"
	scripts/test-lite-m16d.sh

lite-m16e: lite-m16d
	@echo "Running M16E telemetry and qualification contract checks"
	scripts/test-lite-m16e.sh

lite-m16f: lite-m16e lite-release
	@echo "Running M16F Release Stock-Level qualification"
	TPCC_PROPERTIES="$(CURDIR)/benchmarks/tpcc/m15-production.properties" \
	TPCC_SCALE=multi LITE_BUILD_TYPE=release \
	TPCC_ARTIFACT_DIR="$(M16_REPORT_DIR)" \
		scripts/test-lite-m16f.sh

lite-m16g: lite-m16f
	@echo "Running M16G final evidence and regression checks"
	scripts/test-lite-m16g.sh "$(M16_REPORT_DIR)"

lite-m16: lite-m16g
	@echo "M16 Stock-Level range aggregation and index optimization checks passed"

lite-m17a:
	@echo "Running M17 transaction path and OCC index contract checks"
	scripts/test-lite-m17.sh
	scripts/test-lite-new-order-batch.sh

lite-m17b: lite-m17a
	@echo "Running M17 T4 and transaction regression checks"
	scripts/test-lite-t4jdbc.sh
	scripts/test-lite-tpcc-transactions.sh

lite-m17c: lite-m17b lite-release
	@echo "Running M17 Release TPCC-like qualification"
	TPCC_PROPERTIES="$(CURDIR)/benchmarks/tpcc/m15-production.properties" \
	TPCC_SCALE=multi LITE_BUILD_TYPE=release \
	TPCC_ARTIFACT_DIR="$(M17_REPORT_DIR)" \
		scripts/test-lite-tpcc-performance.sh

lite-m17: lite-m17c
	@echo "M17 New-Order execution and OCC validation optimization checks passed"

lite-m18: lite
	@echo "Running M18 T4 transaction-control and durable publication checks"
	scripts/test-lite-t4jdbc.sh
	scripts/test-lite-sql-commit-recovery.sh

lite-m19: lite-m18
	@echo "Running M19 execution-path, cache, and rowset checks"
	scripts/test-lite-new-order-batch.sh
	scripts/test-lite-storage-contract.sh

lite-m20: lite-m19
	@echo "Running M20 retained-plan and server-batch transaction checks"
	scripts/test-lite-tpcc-transactions.sh

lite-m21: lite
	@echo "Running M21 multi-worker/session isolation and capacity checks"
	scripts/test-lite-m21-concurrency.sh

lite-m21-tpcc: lite-m21
	@echo "Running M21 complete 10-warehouse/32-terminal TPCC-like qualification"
	TPCC_PROPERTIES="$(CURDIR)/benchmarks/tpcc/m21-10w32c.properties" \
	TPCC_SCALE=qualification TPCC_NATIVE_BULK_LOAD=1 TPCC_NATIVE_COMMIT_ROWS=10000 TRAFODION_LITE_WORKERS=64 \
	TPCC_ARTIFACT_DIR="$(M21_REPORT_DIR)" \
		scripts/test-lite-tpcc-performance.sh

lite-m22a:
	scripts/test-lite-m22a.sh

lite-m22b: lite-m22a
	scripts/test-lite-m22b.sh

lite-m22c: lite-m22b
	scripts/test-lite-m22c.sh

lite-m22d: lite-m22c
	scripts/test-lite-m22d.sh

lite-m22e: lite-m22d
	scripts/test-lite-m22e.sh

lite-m22f: lite-m22e lite-release
	M22_REPORT_DIR="$(M22_REPORT_DIR)" scripts/test-lite-m22f.sh

lite-m22g: lite-m22f
	scripts/test-lite-m22g.sh "$(M22_REPORT_DIR)"

lite-m22h: lite-m22g
	scripts/test-lite-m22h.sh "$(M22_REPORT_DIR)"

lite-m22: lite-m18 lite-m19 lite-m20 lite-m21 lite-m22h
	@echo "M22 concurrent commit and full-scale qualification checks passed"

package: 
	@echo "Packaging Trafodion components"
	cd core && $(MAKE) package 

package-all: 
	@echo "Packaging all Trafodion components"
	cd core && $(MAKE) package-all 

package-src: $(SRCDIR)-${TRAFODION_VER}/LICENSE
	@echo "Packaging source for $(TRAFODION_VER_PROD) $(TRAFODION_VER)"
	mkdir -p distribution
	git archive --format tar --prefix $(SRCDIR)-${TRAFODION_VER}/ HEAD > distribution/$(SRCDIR)-${TRAFODION_VER}-src.tar
	tar rf distribution/$(SRCDIR)-${TRAFODION_VER}-src.tar $^
	gzip distribution/$(SRCDIR)-${TRAFODION_VER}-src.tar
	rm -rf $(SRCDIR)-${TRAFODION_VER} LICENSE

$(SRCDIR)-${TRAFODION_VER}/LICENSE:
	cd licenses && $(MAKE) LICENSE-src
	mkdir -p $(@D)
	cp licenses/LICENSE-src $@

eclipse: 
	@echo "Making eclipse projects for Trafodion components"
	cd core && $(MAKE) eclipse 

clean:
	@echo "Removing Trafodion objects"
	cd core && $(MAKE) clean 
	cd licenses && $(MAKE) clean
	rm -rf $(SRCDIR)-${TRAFODION_VER} LICENSE

cleanall:
	@echo "Removing all Trafodion objects"
	cd core && $(MAKE) cleanall 

trafinstall:
	@echo "Installing Trafodion components"
	cd core && $(MAKE) trafinstall
