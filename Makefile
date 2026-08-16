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

.PHONY: all local-lite local-lite-regress local-lite-metadata local-lite-legacy-audit local-lite-regress-inventory local-lite-m10 local-lite-m11a local-lite-m11b local-lite-m11c local-lite-m11 local-lite-m12a local-lite-m12b local-lite-m12c local-lite-m12 local-lite-m13 local-lite-m14a local-lite-m14b local-lite-m14c local-lite-m14d local-lite-m14e local-lite-m14f local-lite-m14 local-lite-m15a local-lite-m15b local-lite-m15c local-lite-m15d
SRCDIR = $(shell echo $(TRAFODION_VER_PROD) | sed -e 's/ /-/g' | tr 'A-Z' 'a-z')
M14_REPORT_DIR ?= /tmp/traf-local-lite-m14-report

all:
	@echo "Building all Trafodion components"
	cd core && $(MAKE) all 

local-lite:
	@echo "Building local-lite Trafodion native core"
	cd core && $(MAKE) local-lite

local-lite-regress:
	@echo "Running local-lite SQL regressions"
	core/sql/regress/localLite/runregr $(LOCAL_LITE_REGR_TESTS)

local-lite-metadata:
	@echo "Running local-lite metadata SQL check"
	scripts/test-local-lite-metadata.sh

local-lite-legacy-audit:
	@echo "Auditing local-lite legacy regression inventory"
	scripts/audit-local-lite-legacy-regress.sh --report

local-lite-regress-inventory:
	@echo "Auditing complete upstream regression assets"
	scripts/audit-local-lite-upstream-regress.sh --report

local-lite-m10:
	@echo "Running bounded RocksDB-only legacy convergence gate"
	scripts/test-local-lite-legacy-convergence.sh

local-lite-m11a:
	@echo "Running M11A session-owned transaction context checks"
	scripts/test-local-lite-runtime.sh
	scripts/test-local-lite-context-transactions.sh
	scripts/test-local-lite-statement-snapshot.sh
	scripts/test-local-lite-transaction-snapshot.sh
	scripts/test-local-lite-store-concurrency.sh
	scripts/test-local-lite-session-transactions.sh

local-lite-m11b:
	@echo "Running M11B standalone server and restart checks"
	scripts/test-local-lite-server.sh

local-lite-m11c:
	@echo "Running M11C Trafodion Type 4 JDBC checks"
	scripts/test-local-lite-t4jdbc.sh

local-lite-m11: local-lite-m11a local-lite-m11b local-lite-m11c
	@echo "M11 sessionized server and client protocol checks passed"

local-lite-m12a:
	@echo "Running M12A backend-neutral storage contract checks"
	scripts/test-local-lite-storage-contract.sh
	scripts/test-local-lite-metadata-key-migration.sh

local-lite-m12b: local-lite-m12a
	@echo "Running M12B transactional backend comparison"
	scripts/test-local-lite-storage-backends.sh

local-lite-m12c: local-lite-m12b
	@echo "Running M12C recovery and operations checks"
	scripts/test-local-lite-sql-commit-recovery.sh

local-lite-m12: local-lite-m12c
	@echo "M12 transactional storage and recovery checks passed"

local-lite-m13: local-lite-m12
	@echo "Running M13 exclusive unified storage checks"
	scripts/test-local-lite-storage-cutover.sh
	@echo "M13 exclusive unified TransactionDB checks passed"

local-lite-m14a:
	@echo "Running M14A TPC-C specification and baseline checks"
	scripts/test-local-lite-tpcc-baseline.sh

local-lite-m14b: local-lite-m14a
	@echo "Running M14B TPC-C schema, loader, and integrity checks"
	scripts/test-local-lite-tpcc-loader.sh

local-lite-m14c: local-lite-m14b
	@echo "Running M14C five-profile T4 JDBC transaction checks"
	scripts/test-local-lite-tpcc-transactions.sh

local-lite-m14d: local-lite-m14c
	@echo "Running M14D isolation and crash-recovery checks"
	scripts/test-local-lite-tpcc-isolation.sh

local-lite-m14e: local-lite-m14d
	@echo "Running M14E concurrent compiler/executor checks"
	scripts/test-local-lite-tpcc-concurrency.sh
	@echo "Running M14E cancellation, disconnect, and peer-survival regression"
	scripts/test-local-lite-t4jdbc.sh

local-lite-m14f: local-lite-m14e
	@echo "Running M14F multi-warehouse performance and operations checks"
	TPCC_M14F_ARTIFACT_DIR="$(TPCC_M14F_ARTIFACT_DIR)" scripts/test-local-lite-tpcc-performance.sh

local-lite-m14: TPCC_M14F_ARTIFACT_DIR := $(M14_REPORT_DIR)
local-lite-m14: local-lite-m10 local-lite-m11 local-lite-m12 local-lite-m13 local-lite-m14f
	@echo "Composing M14 aggregate qualification report"
	scripts/test-local-lite-tpcc-qualification.sh "$(M14_REPORT_DIR)"

local-lite-m15a:
	@echo "Running M15A Trafodion MVCC/OCC contract checks"
	scripts/test-local-lite-occ-contract.sh

local-lite-m15b: local-lite-m15a
	@echo "Running M15B transaction-wide unified snapshot checks"
	scripts/test-local-lite-statement-snapshot.sh
	scripts/test-local-lite-transaction-snapshot.sh

local-lite-m15c: local-lite-m15b
	@echo "Running M15C OCC read/write-set checks"
	scripts/test-local-lite-transaction-snapshot.sh

local-lite-m15d: local-lite-m15c
	@echo "Running M15D Trafodion OCC validation matrix"
	scripts/test-local-lite-occ-validation.sh

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
