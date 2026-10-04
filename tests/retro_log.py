"""Readable core logs for libretro.py: cores that log printf-style (PPSSPP does) reach
Python through a small C shim that formats each line first. Import, then call enable()
before creating a Session."""
import ctypes
import os
import subprocess

from libretro.api.log import LogLevel, retro_log_callback, retro_log_printf_t
from libretro.drivers.environment import composite

HERE = os.path.dirname(os.path.abspath(__file__))
SINK = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)
_shim = None
_sinks = []


def enable():
    global _shim
    if _shim is None:
        so = os.path.join(HERE, '..', 'build', 'logshim.so')
        os.makedirs(os.path.dirname(so), exist_ok=True)
        if not os.path.exists(so):
            subprocess.check_call(['gcc', '-shared', '-fPIC', '-O2', '-o', so, os.path.join(HERE, 'logshim.c')])
        _shim = ctypes.CDLL(so)
        _shim.shim_set_sink.argtypes = [SINK]

    def get_log_interface(self, interface):
        if self._log_driver is None or not interface:
            return False
        drv = self._log_driver

        def forward(level, message):
            try:
                drv.log(LogLevel(level) if level in LogLevel else LogLevel.INFO, message)
            except Exception:
                pass

        sink = SINK(forward)
        _sinks.append(sink)  # keep it alive
        _shim.shim_set_sink(sink)
        interface[0] = retro_log_callback(log=ctypes.cast(_shim.shim_log, retro_log_printf_t))
        return True

    composite.CompositeEnvironmentDriver._get_log_interface = get_log_interface
