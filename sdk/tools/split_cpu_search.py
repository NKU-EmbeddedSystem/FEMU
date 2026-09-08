#!/usr/bin/env python3
# split_cpu_search.py -- mechanical, verbatim-preserving split of the frozen
# 1028-line cpu_search.c monolith into:
#   src/cylon.c            library TU (platform + traversal + collab)
#   src/cpu_search_main.c  experiment CLI TU (main() verbatim)
#   src/cylon_internal.h   shared header (includes + moved struct defs +
#                          externs + prototypes)
# Every transform is an exact-match replace asserted to hit exactly once;
# the script aborts WITHOUT writing output on any mismatch. No logic is
# retyped; renames are cosmetic (anonymous struct -> named; static removal).
import sys

SRC = '/var/tmp/cylon/anns/tools/cpu_search.c'
LIB = '/home/liz/FEMU/sdk/src/cylon.c'
MAIN = '/home/liz/FEMU/sdk/src/cpu_search_main.c'
HDR = '/home/liz/FEMU/sdk/src/cylon_internal.h'

text = open(SRC).read()
def rep(s, old, new, what):
    n = s.count(old)
    if n != 1:
        sys.exit('ABORT: anchor %r hit %d times (expect 1)' % (what, n))
    return s.replace(old, new)

# ---------- 1) split off main() ----------
mi = text.index('\nint main(')
head = text[:mi]
main = text[mi+1:]

# ---------- 2) move small struct defs from lib body into shared header ----------
# struct pnm_cnd (verbatim, lines 75-78 of the monolith)
PNM_CND = 'struct pnm_cnd {\n    float d;\n    uint32_t id;\n};\n'
head = rep(head, PNM_CND, '', 'pnm_cnd block')

# struct collab_ctx (verbatim, lines 607-612)
i = head.index('struct collab_ctx {')
j = head.index('};', i) + 2
COLLAB_CTX = head[i:j] + '\n'
head = rep(head, COLLAB_CTX, '', 'collab_ctx block')

# anonymous traversal struct -> named + moved to header (body verbatim)
i = head.index('/* ---- state (graph points INTO the CXL window;')
j = head.index('} st;', i) + len('} st;')
ST_BLOCK = head[i:j].replace('static struct {', 'struct cylon_st {') + '\n'
head = rep(head, head[i:j], '', 'cylon_st block')

# COLLAB_KMAX define must live in the shared header (the externs there use
# it); removed from the lib TU, added to the header below.
head = rep(head, '#define COLLAB_KMAX 64\n', '', 'COLLAB_KMAX define removal')

# ---------- 3) de-static the symbols shared with the CLI TU ----------
# line-wise: if a line strips to 'static <tok>...' for one of these tokens,
# drop the leading 'static '. Short tokens only; no full-line retyping.
TOKENS = ['uint8_t *win;', 'uint64_t win_blob_bytes;', 'uint64_t win_sz;',
          'struct pnm_mb_s *mb;', 'uint32_t g_poll_us', 'int g_db_fd',
          'uint32_t gen_ctr;', 'uint32_t jid;', 'int g_trace;',
          'uint64_t g_dist, g_hops;', 'uint64_t g_e_dist,',
          'uint32_t (*g_all_ids)', 'float (*g_all_d)', 'uint32_t *g_all_m;',
          'uint64_t now_ns(void)', 'void mb_reset(void)',
          'uint32_t mb_submit(', 'uint64_t stage_file(', 'uint32_t stage_device(',
          'int ping(void)', 'int flush_cache(void)', 'int bind_index(void)',
          'int verify_window(', 'int engine_feeder(', 'void *cpu_worker(',
          'int cmp_u64(']
lines = head.split('\n')
out, hits = [], {}

# fix: redo the pass mutating lines in place, then verify each token hit once
for i, ln in enumerate(lines):
    s = ln.strip()
    if s.startswith('static '):
        rest = s[len('static '):]
        for t in TOKENS:
            if rest.startswith(t):
                lines[i] = ln.replace('static ', '', 1)
                hits[t] = hits.get(t, 0) + 1
                break
head = '\n'.join(lines)
for t in TOKENS:
    if hits.get(t, 0) != 1:
        sys.exit('ABORT: de-static token %r hit %d times (expect 1)' % (t, hits.get(t, 0)))

# ---------- 4) assemble shared header ----------
st_hdr = ST_BLOCK.replace('} st;', '};')
hdr = []
hdr.append('''/*
 * cylon_internal.h -- shared declarations for libcylon TUs + the experiment
 * CLI. GENERATED (mechanically) by tools/split_cpu_search.py from the frozen
 * cpu_search.c monolith; struct bodies are verbatim extractions. Do not edit
 * the moved blocks by hand -- rerun the splitter instead.
 */''')
hdr.append('#ifndef CYLON_INTERNAL_H')
hdr.append('#define CYLON_INTERNAL_H')
hdr.append('#include <stdio.h>')
hdr.append('#include <stdlib.h>')
hdr.append('#include <string.h>')
hdr.append('#include <stdint.h>')
hdr.append('#include <errno.h>')
hdr.append('#include <time.h>')
hdr.append('#include <stdbool.h>')
hdr.append('#include <unistd.h>')
hdr.append('#include <fcntl.h>')
hdr.append('#include <poll.h>')
hdr.append('#include <pthread.h>')
hdr.append('#include <sys/mman.h>')
hdr.append('#include <sys/stat.h>')
hdr.append('#include <sys/file.h>')
hdr.append('')
hdr.append('#include "pnm_uapi.h"')
hdr.append('')
hdr.append('#ifdef __cplusplus')
hdr.append('extern "C" {')
hdr.append('#endif')
hdr.append('')
hdr.append(PNM_CND)
hdr.append(COLLAB_CTX)
hdr.append(st_hdr)
hdr.append('')
hdr.append('extern struct cylon_st st;')
hdr.append('')
hdr.append('/* set -DCYLON_BUILD_AVX=1 when building libcylon_avx.so; guards the')
hdr.append(' * known avx x f=0.25 client crash family (CYLON_ST_EAVX refusal) */')
hdr.append('#ifndef CYLON_BUILD_AVX')
hdr.append('#define CYLON_BUILD_AVX 0')
hdr.append('#endif')
hdr.append('')
hdr.append('/* collab merged-result capacity (k <= this) */')
hdr.append('#define COLLAB_KMAX 64')
hdr.append('')
hdr.append('/* platform state (cylon.c) */')
hdr.append('extern uint8_t *win;')
hdr.append('extern uint64_t win_blob_bytes, win_sz;')
hdr.append('extern struct pnm_mb_s *mb;')
hdr.append('extern uint32_t g_poll_us;')
hdr.append('extern int g_db_fd;')
hdr.append('extern uint32_t gen_ctr, jid;')
hdr.append('')
hdr.append('/* traversal + collab state (cylon.c) */')
hdr.append('extern int g_trace;')
hdr.append('extern uint64_t g_dist, g_hops;')
hdr.append('extern uint64_t g_e_dist, g_e_hops, g_e_pages, g_e_ns;')
hdr.append('extern uint32_t (*g_all_ids)[COLLAB_KMAX];')
hdr.append('extern float (*g_all_d)[COLLAB_KMAX];')
hdr.append('extern uint32_t *g_all_m;')
hdr.append('')
hdr.append('uint64_t now_ns(void);')
hdr.append('void mb_reset(void);')
hdr.append('uint32_t mb_submit(uint32_t op, uint64_t a0, uint64_t a1,')
hdr.append('                          uint32_t k, uint32_t ef, uint64_t timeout_ns,')
hdr.append('                          struct pnm_resp_s *r);')
hdr.append('uint64_t stage_file(const char *path, uint64_t off);')
hdr.append('uint32_t stage_device(uint64_t blob_bytes, uint32_t *staged_pages);')
hdr.append('int ping(void);')
hdr.append('int flush_cache(void);')
hdr.append('int bind_index(void);')
hdr.append('int verify_window(const char *blob);')
hdr.append('int engine_feeder(uint64_t qoff, uint32_t n, uint32_t n_cpu,')
hdr.append('                  uint32_t k, uint32_t ef, uint64_t *lat);')
hdr.append('void *cpu_worker(void *arg);')
hdr.append('int cmp_u64(const void *a, const void *b);')
hdr.append('')
hdr.append('#ifdef __cplusplus')
hdr.append('}')
hdr.append('#endif')
hdr.append('#endif /* CYLON_INTERNAL_H */')

# ---------- 5) write outputs ----------
head = rep(head, '#include "pnm_uapi.h"',
           '#include "pnm_uapi.h"\n#include "cylon_internal.h"\n\nstruct cylon_st st;\n',
           'cylon_internal.h include insertion')
open(LIB, 'w').write(head)
main_tu = ('/* cpu_search_main.c -- experiment CLI on top of libcylon.\n'
           ' * GENERATED by tools/split_cpu_search.py: main() is verbatim from the\n'
           ' * frozen cpu_search.c monolith (same flags, same prints, same flow).\n'
           ' * Tiers: libcylon.so = O2 -fno-tree-vectorize (scalar),\n'
           ' * libcylon_avx.so = O3 -mavx2 (avx). See CYLON-SDK.md M1. */\n'
           '#include "cylon_internal.h"\n\n' + main)
open(MAIN, 'w').write(main_tu)
open(HDR, 'w').write('\n'.join(hdr) + '\n')
print('split done: lib=%d lines, main=%d lines, header=%d lines'
      % (head.count('\n'), main.count('\n'), len(hdr)))
