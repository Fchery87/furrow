"""Python access to tools/jsfx_host/libfurrow_jsfx_host.so. Build it with build.sh first."""

import ctypes
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIBRARY = HERE / "libfurrow_jsfx_host.so"
BRIDGE = HERE / "gmem_bridge.jsfx"

PLAYING = 1
STOPPED = 0

_lib = None


def available():
    return LIBRARY.exists()


def lib():
    global _lib
    if _lib is None:
        _lib = ctypes.CDLL(str(LIBRARY))
        _lib.fh_new.restype = ctypes.c_void_p
        _lib.fh_new.argtypes = [ctypes.c_char_p, ctypes.c_double, ctypes.c_int]
        _lib.fh_log.restype = ctypes.c_char_p
        _lib.fh_free.argtypes = [ctypes.c_void_p]
        _lib.fh_init.argtypes = [ctypes.c_void_p]
        _lib.fh_time.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_int, ctypes.c_double, ctypes.c_double]
        _lib.fh_process.restype = ctypes.c_int
        _lib.fh_process.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int), ctypes.c_int]
        _lib.fh_slider.restype = ctypes.c_double
        _lib.fh_slider.argtypes = [ctypes.c_void_p, ctypes.c_int]
        _lib.fh_set_slider.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_double]
        _lib.fh_var.restype = ctypes.c_double
        _lib.fh_var.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        _lib.fh_read_mem.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_double), ctypes.c_int]
        _lib.fh_write_mem.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_double), ctypes.c_int]
        _lib.fh_save.restype = ctypes.c_int
        _lib.fh_save.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        _lib.fh_load.restype = ctypes.c_int
        _lib.fh_load.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    return _lib


class Effect:
    def __init__(self, path, srate=44100.0, block=1024):
        self.handle = lib().fh_new(str(path).encode(), srate, block)
        if not self.handle:
            raise RuntimeError("ysfx could not load %s\n%s" % (path, lib().fh_log().decode()))
        self.srate = srate
        self.block = block

    def close(self):
        if self.handle:
            lib().fh_free(self.handle)
            self.handle = None

    def init(self):
        lib().fh_init(self.handle)

    def time(self, tempo, state, beat, seconds=0.0):
        lib().fh_time(self.handle, tempo, state, beat, seconds)

    def process(self, frames=None):
        frames = frames or self.block
        capacity = 4096
        buffer = (ctypes.c_int * (capacity * 5))()
        count = lib().fh_process(self.handle, frames, buffer, capacity)
        if count > capacity:
            raise RuntimeError("more than %d MIDI events in one block" % capacity)
        return [tuple(buffer[i * 5:i * 5 + 5]) for i in range(count)]

    def send_midi(self, status, data1, data2, offset=0, bus=0):
        """Queues one MIDI message for the next process() call."""
        data = (ctypes.c_uint8 * 3)(status, data1, data2)

        class Event(ctypes.Structure):
            _fields_ = [("bus", ctypes.c_uint32), ("offset", ctypes.c_uint32),
                        ("size", ctypes.c_uint32), ("data", ctypes.POINTER(ctypes.c_uint8))]

        send = lib().ysfx_send_midi
        send.argtypes = [ctypes.c_void_p, ctypes.POINTER(Event)]
        send.restype = ctypes.c_bool
        event = Event(bus, offset, 3, ctypes.cast(data, ctypes.POINTER(ctypes.c_uint8)))
        return send(self.handle, ctypes.byref(event))

    def slider(self, index):
        return lib().fh_slider(self.handle, index)

    def set_slider(self, index, value):
        lib().fh_set_slider(self.handle, index, value)

    def var(self, name):
        return lib().fh_var(self.handle, name.encode())

    def read_mem(self, addr, count):
        out = (ctypes.c_double * count)()
        lib().fh_read_mem(self.handle, addr, out, count)
        return list(out)

    def write_mem(self, addr, values):
        data = (ctypes.c_double * len(values))(*values)
        lib().fh_write_mem(self.handle, addr, data, len(values))

    def save(self):
        size = lib().fh_save(self.handle, None, 0)
        out = ctypes.create_string_buffer(max(size, 1))
        lib().fh_save(self.handle, out, size)
        return out.raw[:size]

    def load(self, data):
        return lib().fh_load(self.handle, data, len(data)) == 1


class Gmem:
    """Reads and writes the named gmem Furrow through a bridge JSFX in the same process."""

    CHUNK = 65536

    def __init__(self):
        self.bridge = Effect(BRIDGE, block=1)

    def close(self):
        self.bridge.close()

    def write(self, index, values):
        for start in range(0, len(values), self.CHUNK):
            part = values[start:start + self.CHUNK]
            self.bridge.write_mem(0, [1, index + start, len(part)] + list(part))
            self.bridge.process(1)

    def read(self, index, count=1):
        out = []
        for start in range(0, count, self.CHUNK):
            n = min(self.CHUNK, count - start)
            self.bridge.write_mem(0, [2, index + start, n])
            self.bridge.process(1)
            out.extend(self.bridge.read_mem(3, n))
        return out
