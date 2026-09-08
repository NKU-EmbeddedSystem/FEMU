"""cylon.py -- ctypes binding for libcylon (C ABI in cylon.h).

Zero-dependency (stdlib + numpy for the search convenience layer). This is
the Python story from CYLON-SDK.md 5.x: numpy fp32 (nq, dim) arrays in,
(distances, labels) out; the F32 entry converts to the device storage
precision (one RNE rounding per vector, disclosed via info()).

Padding convention: empty result slots carry label 0xffffffff (4294967295)
/ distance 0.0, passed through verbatim -- count non-sentinel labels to
recover m (real CYH1 labels are < 2**24 and never collide with it).

Usage:
    from cylon import Cylon
    with Cylon(blob_path="/var/tmp/anns_wiki/cyh1_wiki.blob") as c:
        D, L = c.search(np_queries_fp32, k=10, ef=100)
"""

import ctypes as C
import os

import numpy as np

# status / prec / notify enums (mirror cylon.h)
OK, EINVAL, ENODEV, ETO, EAVX, EPREC, EBUSY = range(7)
F32, F16, BF16, F64, I8 = range(5)
NOTIFY_AUTO, NOTIFY_POLL, NOTIFY_DOORBELL = range(3)

CYLON_KMAX = 64

_STATNAMES = {OK: "ok", EINVAL: "invalid argument/config",
              ENODEV: "device/window unreachable",
              ETO: "engine timeout (close+open to recover)",
              EAVX: "avx build at f=0.25 poison point",
              EPREC: "requested input precision not in info.in_prec_mask",
              EBUSY: "device busy (another ctx holds the window)"}


class CylonConfig(C.Structure):
    _fields_ = [("window_dev", C.c_char_p),
                ("blob_path", C.c_char_p),
                ("cpu_frac", C.c_double),
                ("n_cpu_threads", C.c_uint32),
                ("ef", C.c_uint32),
                ("notify", C.c_int),
                ("stage_bps", C.c_uint32)]


class CylonInfo(C.Structure):
    _fields_ = [("dim", C.c_uint32),
                ("ntotal", C.c_uint64),
                ("storage_prec", C.c_int),
                ("accum_prec", C.c_int),
                ("metric", C.c_char_p),
                ("in_prec_mask", C.c_uint32)]


class CylonStats(C.Structure):
    _fields_ = [("n_dist", C.c_uint64),
                ("n_hops", C.c_uint64),
                ("n_pages", C.c_uint64),
                ("engine_ns", C.c_uint64)]


def _bind(path):
    lib = C.CDLL(path)
    lib.cylon_open.argtypes = [C.POINTER(C.c_void_p), C.POINTER(CylonConfig)]
    lib.cylon_open.restype = C.c_int
    lib.cylon_load.argtypes = [C.c_void_p]
    lib.cylon_load.restype = C.c_int
    lib.cylon_search.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_uint32,
                                 C.c_uint32, C.c_uint32,
                                 C.POINTER(C.c_uint32), C.POINTER(C.c_float),
                                 C.POINTER(CylonStats)]
    lib.cylon_search.restype = C.c_int
    lib.cylon_close.argtypes = [C.c_void_p]
    lib.cylon_close.restype = C.c_int
    lib.cylon_get_info.argtypes = [C.c_void_p, C.POINTER(CylonInfo)]
    lib.cylon_get_info.restype = C.c_int
    lib.cylon_dim.argtypes = [C.c_void_p]
    lib.cylon_dim.restype = C.c_uint32
    lib.cylon_ntotal.argtypes = [C.c_void_p]
    lib.cylon_ntotal.restype = C.c_uint64
    lib.cylon_strerror.argtypes = [C.c_int]
    lib.cylon_strerror.restype = C.c_char_p
    return lib


class Cylon:
    """One ctx = one device window owner (single-ctx semantics)."""

    def __init__(self, blob_path, cpu_frac=0.5, ef=100, notify=NOTIFY_POLL,
                 window_dev=None, libpath=None):
        self._lib = _bind(libpath or os.environ.get(
            "CYLON_LIB",
            os.path.join(os.path.dirname(__file__), "..", "..", "libcylon.so")))
        self._ctx = C.c_void_p()
        cfg = CylonConfig()
        cfg.window_dev = None                    # NULL -> "/dev/dax0.0"
        cfg.blob_path = blob_path.encode()
        cfg.cpu_frac = cpu_frac
        cfg.ef = ef
        cfg.notify = notify
        cfg.stage_bps = 0
        st = self._lib.cylon_open(C.byref(self._ctx), C.byref(cfg))
        if st != OK:
            raise RuntimeError("cylon_open: " + _STATNAMES.get(st, str(st)))
        st = self._load()
        if st != OK:
            self._lib.cylon_close(self._ctx)
            raise RuntimeError("cylon_load: " + self._strerror(st))

    def _strerror(self, st):
        return self._lib.cylon_strerror(st).decode()

    def close(self):
        if getattr(self, "_ctx", None) and self._ctx:
            self._lib.cylon_close(self._ctx)
            self._ctx = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _load(self):
        return self._lib.cylon_load(self._ctx)

    def info(self):
        inf = CylonInfo()
        st = self._lib.cylon_get_info(self._ctx, C.byref(inf))
        if st != OK:
            raise RuntimeError("cylon_get_info: " + self._strerror(st))
        return {"dim": inf.dim, "ntotal": inf.ntotal,
                "storage_prec": inf.storage_prec,   # 0=F32 1=F16
                "accum_prec": inf.accum_prec,
                "metric": inf.metric.decode(),
                "in_prec_mask": inf.in_prec_mask}

    def search(self, queries, k=10, ef=0):
        """queries: numpy fp32 (nq, dim) row-major contiguous.
        Returns (distances fp32 (nq,k), labels u32 (nq,k) with 0xffffffff
        padding passed through verbatim)."""
        q = np.ascontiguousarray(queries, dtype=np.float32)
        if q.ndim != 2:
            raise ValueError("queries must be (nq, dim)")
        nq, dim = q.shape
        D = np.empty((nq, k), dtype=np.float32)
        L = np.empty((nq, k), dtype=np.uint32)
        stats = CylonStats()
        st = self._lib.cylon_search(self._ctx, F32, q.ctypes.data, nq, k, ef,
                                    L.ctypes.data_as(C.POINTER(C.c_uint32)),
                                    D.ctypes.data_as(C.POINTER(C.c_float)),
                                    C.byref(stats))
        if st != OK:
            raise RuntimeError("cylon_search: " + self._strerror(st))
        self._last_stats = {"n_dist": stats.n_dist, "n_hops": stats.n_hops,
                            "n_pages": stats.n_pages,
                            "engine_ns": stats.engine_ns}
        return D, L

    @property
    def last_stats(self):
        return getattr(self, "_last_stats", None)
