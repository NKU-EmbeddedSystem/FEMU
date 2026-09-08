# sdk/Makefile -- plain gcc, no meson/ninja dependency (guest-side builds).
# Two build tiers mirror the experiment protocol (em_p2_bringup STAGE=.sh):
#   scalar tier: -O2 -fno-tree-vectorize   -> libcylon.so + cpu_search
#   avx tier:    -O3 -mavx2 (CYLON_BUILD_AVX=1) -> libcylon_avx.so 5 cpu_search_vec
# The CLI (cpu_search_main.c) compiles at its tier and links its tier's lib;
# the avx tier defines CYLON_BUILD_AVX (EAVX poison-point guard).
# PNM_INC points at pnm_uapi.h (repo default, falls back to the sdk dir --
# deployments mirror pnm_uapi.h next to the Makefile).

CC       ?= gcc
PNM_INC  ?= ../hw/femu/cylon
ifeq ($(wildcard $(PNM_INC)/pnm_uapi.h),)
PNM_INC := .
endif
CFLAGS   = -O2 -fno-tree-vectorize -fPIC -pthread -Wall -Wextra
AVXCFLAGS = -O3 -mavx2 -fPIC -pthread -Wall -Wextra -DCYLON_BUILD_AVX=1
INC      = -I include -I src -I $(PNM_INC)
LDLIBS   = -lpthread -lm

all: libcylon.so libcylon_avx.so cpu_search cpu_search_vec c_min

libcylon.so: src/cylon.c src/cylon_api.c src/cylon_internal.h include/cylon.h $(PNM_INC)/pnm_uapi.h
	$(CC) $(CFLAGS) $(INC) -shared src/cylon.c src/cylon_api.c -o $@ $(LDLIBS)

libcylon_avx.so: src/cylon.c src/cylon_api.c src/cylon_internal.h include/cylon.h $(PNM_INC)/pnm_uapi.h
	$(CC) $(AVXCFLAGS) $(INC) -shared src/cylon.c src/cylon_api.c -o $@ $(LDLIBS)

cpu_search: src/cpu_search_main.c libcylon.so
	$(CC) $(CFLAGS) $(INC) src/cpu_search_main.c libcylon.so -o $@ $(LDLIBS) -Wl,-rpath,'$$ORIGIN'

cpu_search_vec: src/cpu_search_main.c libcylon_avx.so
	$(CC) $(AVXCFLAGS) $(INC) src/cpu_search_main.c libcylon_avx.so -o $@ $(LDLIBS) -Wl,-rpath,'$$ORIGIN'

c_min: examples/c_min.c libcylon.so
	$(CC) $(CFLAGS) $(INC) examples/c_min.c libcylon.so -o $@ $(LDLIBS) -Wl,-rpath,'$$ORIGIN'

clean:
	rm -f libcylon.so libcylon_avx.so cpu_search cpu_search_vec c_min

.PHONY: all clean
