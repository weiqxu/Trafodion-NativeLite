#######################################################################
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
#######################################################################

CPPSRC := SqlciMain.cpp 

ifeq ($(TRAF_LOCAL_LITE),1)
# Keep all DEP_LIBS in the NEEDED list so BIND_NOW can resolve
# transitive symbols across shared libraries.
EARLY_DLLS := -Wl,--no-as-needed
SYS_LIBS += -Wl,--as-needed
else
EARLY_DLLS := -ljsig
endif

DEP_LIBS += \
	sqlcilib \
	sqlmsg \
	executor \
	optimizer \
	generator \
	sqlcomp \
	cli \
	comexe \
	common \
	eh \
	sort \
	tdm_sqlerrormsg \
	tdm_sqlexport \
	tdm_sqlshare \
	tdm_sqlmxevents \
	ustat \
	sqlcat \
	parser \
	arkcmplib \
	tdm_sqlcli \
	tdm_sqlexp \
	porting_layer \
	lmcomp \
	smdio \
	qmscommon \
	arkcmp_dll

EXTERN_LIBS := \
	$(LIBROOT)/libtdm_tmfuser.so

CPPSRC += vprocsqlci.cpp


SRCPATH := sqlci bin
