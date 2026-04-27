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

PROGS          = $(OUTDIR)/tm $(OUTDIR)/idtmsrv
###########################################################################

include ../../macros.gmk
include ../macros.gmk
include macros.gmk

HBASE_TRX_LOC    = $(TRAF_HOME)/src/seatrans/hbase-trx

DEBUG		=
DEFINES		= -DDEBUG -DxUSE_FILE_AUDIT 

ifeq ($(TRAF_LOCAL_LITE),1)
DEFINES		+= -DTRAF_LOCAL_LITE
LIBJVM		=
TM_HBASE_PREREQ	=
else
   LIBJVM        = -I. -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -L$(JAVA_HOME)/jre/lib/amd64/server -ljvm
TM_HBASE_PREREQ	= $(LIBEXPDIR)/libshbasetmlib.so
endif
   LIBSBMS       = -L$(LIBEXPDIR) -lsbms

ifeq ($(USE_THREADED_IO),1)
DEFINES         += -DMULTITHREADED_TM 
endif
ifeq ($(SQ_MTYPE),64)
DEFINES += -DNA_64BIT
endif

DTMOBJS		= -DTMOBJS


#gcc 4.8 need explicit set this flag to allow auto search of dependent shared object during link
LNK_FLGS       += -Xlinker --copy-dt-needed-entries

LIBSTMOBJS = $(OUTDIR)/tmtransid.o \
			 $(OUTDIR)/tmtransaction.o \
			 $(OUTDIR)/tmlibtxn.o \
			 $(OUTDIR)/tmlib.o \
			 $(OUTDIR)/tmlogging.o \
			 $(OUTDIR)/tmmap.o \
			 $(OUTDIR)/tmmutex.o \
			 $(OUTDIR)/tmregisterregion.o \
             $(OUTDIR)/tmddlrequests.o\
			 $(OUTDIR)/versstmlib.o \
		         $(OUTDIR)/CommonLogger.o 
ifeq ($(TRAF_LOCAL_LITE),1)
LIBSTMOBJS := $(filter-out $(OUTDIR)/tmregisterregion.o $(OUTDIR)/tmddlrequests.o,$(LIBSTMOBJS))
else
LIBSTMOBJS += $(OUTDIR)/javaobjectinterfacetm.o
endif
LIBSXATMOBJS  = $(OUTDIR)/tmmap.o \
				$(OUTDIR)/tmmmap.o \
                $(OUTDIR)/tmmutex.o \
				$(OUTDIR)/tmdeque.o \
				$(OUTDIR)/tmtransid.o \
				$(OUTDIR)/xatmlib.o \
				$(OUTDIR)/xatmmsg.o \
				$(OUTDIR)/xatmapi.o \
				$(OUTDIR)/tmlogging.o \
				$(OUTDIR)/tmregistry.o \
				$(OUTDIR)/tmeventq.o \
				$(OUTDIR)/tmthread.o  \
				$(OUTDIR)/tmtimer.o \
				$(OUTDIR)/tmglobals.o \
				$(OUTDIR)/verssxatmlib.o
LIBXARMOBJS	= $(OUTDIR)/tmmap.o \
			  $(OUTDIR)/tmmmap.o \
			  $(OUTDIR)/tmmutex.o \
		      $(OUTDIR)/tmdeque.o \
		      $(OUTDIR)/tmtransid.o \
		      $(OUTDIR)/tmpoolelement.o \
		      $(OUTDIR)/xatmlib.o \
		      $(OUTDIR)/xatmmsg.o \
		      $(OUTDIR)/xarmapi.o \
			  $(OUTDIR)/xatmapi.o \
		      $(OUTDIR)/tmregistry.o \
              $(OUTDIR)/tmtimer.o \
		      $(OUTDIR)/verssxarmlib.o
RAOBJS		= $(OUTDIR)/read_audit.o 
TMOBJS		= $(OUTDIR)/tmregistry.o \
		      $(OUTDIR)/tmlogging.o \
		      $(OUTDIR)/tmmap.o \
		      $(OUTDIR)/tmmutex.o \
		      $(OUTDIR)/tmdeque.o \
		      $(OUTDIR)/tmtxkey.o \
		      $(OUTDIR)/tmrecovstate.o \
		      $(OUTDIR)/tmpoolelement.o \
		      $(OUTDIR)/tmtxbase.o \
		      $(OUTDIR)/tmtx.o \
		      $(OUTDIR)/tmxatxn.o\
		      $(OUTDIR)/tmtxmsg.o \
		      $(OUTDIR)/tminfo.o \
		      $(OUTDIR)/tmaudit.o \
		      $(OUTDIR)/tmrecov.o \
		      $(OUTDIR)/tmshutdown.o \
		      $(OUTDIR)/tmrm.o \
		      $(OUTDIR)/tmrmtse.o \
			  $(OUTDIR)/tmrmtsebranch.o \
		      $(OUTDIR)/tmrmhbase.o \
		      $(OUTDIR)/tmtxbranches.o \
		      $(OUTDIR)/tmevent.o \
		      $(OUTDIR)/tmeventq.o \
		      $(OUTDIR)/tmthread.o \
		      $(OUTDIR)/tmauditobj.o \
		      $(OUTDIR)/tmtxthread.o \
		      $(OUTDIR)/tmtimermain.o \
		      $(OUTDIR)/tmmmap.o \
		      $(OUTDIR)/tmtxstats.o \
		      $(OUTDIR)/tmstats.o \
		      $(OUTDIR)/tmthreadeg.o \
		      $(OUTDIR)/verstm.o   \
	    	  $(OUTDIR)/xaglob.o \
		      $(OUTDIR)/tmxidmap.o \
		      $(OUTDIR)/tmxarmmain.o \
		      $(OUTDIR)/tm.o \
		      $(OUTDIR)/CommonLogger.o  
ifeq ($(TRAF_LOCAL_LITE),1)
TMOBJS += $(OUTDIR)/LocalLiteHBaseTmStubs.o
endif

ifeq ($(SUPPORT_TM_SYNC),1)
DEFINES         += -DSUPPORT_TM_SYNC
TMOBJS          += $(OUTDIR)/tmsync.o
endif


IDSRVOBJS	= $(OUTDIR)/idtmsrv.o \
		      $(OUTDIR)/versidtmsrv.o
ALLOBJS	= $(LIBSTMOBJS) \
		  $(LIBSXATMOBJS) \
		  $(TMOBJS) \
		  $(IDSRVOBJS) \
		  $(LIBXARMOBJS) \

CFILES		= `ls *.c`

ifeq ($(USE_THREADED_IO),1)
LIBSTM		+= $(LIBSTHX)
endif

all:  $(LIBEXPDIR)/libstmlib.so cp_trx_jar

ifeq ($(TRAF_LOCAL_LITE),1)
cp_trx_jar:
	@echo "Skipping transaction jar for local-lite"
else
cp_trx_jar:
	cd $(HBASE_TRX_LOC); make all
endif

ifeq ($(TRAF_LOCAL_LITE),1)
all:  $(LIBEXPDIR)/libsxatmlib.so $(LIBEXPDIR)/libstmlib.so $(PROGS) $(LIBEXPDIR)/libxarm.so
	@echo "Skipping TM tools for local-lite"
else
all:  $(LIBEXPDIR)/libsxatmlib.so $(LIBEXPDIR)/libstmlib.so $(PROGS) $(LIBEXPDIR)/libxarm.so
	cd tools; $(MAKE)
endif

$(OUTDIR)/tm: $(TMOBJS) $(LIBEXPDIR)/libsxatmlib.so $(TM_HBASE_PREREQ) $(PROGS) $(LIBEXPDIR)/libxarm.so cp_trx_jar
	@echo "USE_THREADED_IO = " $(USE_THREADED_IO)
	$(CXX) $(LNK_FLGS) $(LIBJVM) -o $@ $(TMOBJS) $(LIBSTM) -L$(LIBEXPDIR) -llog4cxx -DTM_BUILD_

	cp -fup $(OUTDIR)/tm $(BINEXPDIR)

$(OUTDIR)/ra: $(RAOBJS) 
	$(CXX) $(LNK_FLGS) -o $@ $(RAOBJS) $(LIBSTM) 

$(OUTDIR)/idtmsrv: $(IDSRVOBJS)
	$(CXX) $(LNK_FLGS) -o $@ $(IDSRVOBJS) $(LIBSBMSX)
	cp -fup $(OUTDIR)/idtmsrv $(BINEXPDIR)

$(LIBEXPDIR)/libstmlib.so: $(LIBSTMOBJS) 
	$(CXX) $(DTMOBJS) $(LNK_FLGS) $(LIBSBMSX) $(LIBJVM) -L$(LIBEXPDIR) -llog4cxx -shared -o $@ $(LIBSTMOBJS)  

$(LIBEXPDIR)/libsxatmlib.so: $(LIBSXATMOBJS) 
	$(CXX) $(LNK_FLGS) -shared -o $@ $(LIBSXATMOBJS) $(LIBSTMX) -L$(LIBEXPDIR) -llog4cxx

ifneq ($(TRAF_LOCAL_LITE),1)
$(LIBEXPDIR)/libshbasetmlib.so: $(LIBSXATMOBJS) cp_trx_jar
	cd $(HBASETMLIB); $(MAKE)
endif


$(LIBEXPDIR)/libxarm.so: $(LIBXARMOBJS) 
	$(CXX) $(LNK_FLGS) -shared -o $@ $(LIBXARMOBJS) -L$(LIBEXPDIR) -llog4cxx -DXARM_BUILD_

$(OUTDIR)/CommonLogger.o: $(TRAF_HOME)/commonLogger/CommonLogger.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(OUTDIR)/javaobjectinterfacetm.o: $(TRAF_HOME)/src/seatrans/tm/hbasetmlib2/javaobjectinterfacetm.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

hbasetmlib:
	cd $(HBASETMLIB); $(MAKE)

setup:
	cd tools; $(MAKE) setup
ifeq ($(TRAF_LOCAL_LITE),1)
	@echo "Skipping TM Java setup for local-lite"
else
	@echo "HBASETMLIB = " $(HBASETMLIB)
	cd $(HBASETMLIB); $(MAKE) setup
endif

clean:
	$(RM) $(LIBEXPDIR)/libsxatmlib.so $(LIBEXPDIR)/libxarm.so $(LIBEXPDIR)/libstmlib.so *.gcda *.gcno *.gcov
	$(RM)  $(BINEXPDIR)/tm
	$(RM)  $(BINEXPDIR)/idtmsrv
	$(RM)  $(OUTDIR)/tm
	$(RM)  $(OUTDIR)/idtmsrv
	$(RM)  $(ALLOBJS)
	$(RM)  $(CDEPFILES)
	cd $(HBASE_TRX_LOC); make clean
	@echo "PWD = " `pwd`
	cd tools; $(MAKE) clean
	@echo "PWD = " `pwd`
	@echo "HBASETMLIB = " $(HBASETMLIB)
	cd $(HBASETMLIB); make clean

cleanall: clean
	$(RM) -rf $(BUILD_PLAT)
	cd tools; $(MAKE) cleanall
	cd $(HBASELIBTM); $(MAKE) cleanall

cleaner: clean
	$(RM) *~
	cd tools; $(MAKE) cleaner
	cd $(HBASELIBTM); $(MAKE) cleaner

-include $(CDEP_DIR)/*.dep
_dummy := $(shell if [ ! -d $(CDEP_DIR) ]; then mkdir -p $(CDEP_DIR); fi)
