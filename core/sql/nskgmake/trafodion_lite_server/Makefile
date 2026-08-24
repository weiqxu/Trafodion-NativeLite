#######################################################################
# Lite standalone Trafodion Type 4 protocol server.
#######################################################################

CPPSRC := TrafodionLiteServerMain.cpp
TARGET := trafodion-lite-server

# Keep the complete dependency set visible to BIND_NOW, matching sqlci.
ifeq ($(TRAF_LITE),1)
EARLY_DLLS := -Wl,--no-as-needed
SYS_LIBS += -Wl,--as-needed
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

EXTERN_LIBS := $(LIBROOT)/libtdm_tmfuser.so
SYS_LIBS += -pthread
INCLUDE_DIRS := bin sqlci executor litestore cli common
SRCPATH := bin sqlci
