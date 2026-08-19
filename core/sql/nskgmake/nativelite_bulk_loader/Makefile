#######################################################################
# LocalLite native TPCC bulk loader.
#######################################################################

CPPSRC := NativeLiteBulkLoaderMain.cpp
TARGET := nativelite-bulk-loader

ifeq ($(TRAF_LOCAL_LITE),1)
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
INCLUDE_DIRS := bin sqlci executor localstore cli common
SRCPATH := bin sqlci executor localstore cli common
