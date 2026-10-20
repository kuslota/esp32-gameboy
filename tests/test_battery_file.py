#!/usr/bin/env python3
"""Battery SRAM replacements retain the prior complete save on write failure."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gb-battery-") as tmp:
    work = Path(tmp)
    library = work / "battery.so"
    subprocess.run([
        "cc", "-std=c11", "-D_DEFAULT_SOURCE", "-shared", "-fPIC", "-Wall", "-Wextra",
        str(root / "src/storage/save_file.c"), str(root / "src/storage/rom_file.c"),
        "-o", str(library),
    ], check=True)
    files = C.CDLL(str(library))
    ptr = C.POINTER(C.c_uint8)
    files.save_file_replace.argtypes = [C.c_char_p, ptr, C.c_size_t]
    files.save_file_replace.restype = C.c_bool
    files.rom_file_read.argtypes = [C.c_char_p, ptr, C.c_size_t]
    files.rom_file_read.restype = C.c_int
    old = bytes((i * 7) & 255 for i in range(8192))
    new = bytes((i * 13 + 3) & 255 for i in range(8192))
    path = work / "game.sav"
    old_buffer = (C.c_uint8 * len(old)).from_buffer_copy(old)
    new_buffer = (C.c_uint8 * len(new)).from_buffer_copy(new)
    output = (C.c_uint8 * len(old))()
    assert files.save_file_replace(str(path).encode(), old_buffer, len(old))
    assert files.save_file_replace(str(path).encode(), new_buffer, len(new))
    assert path.read_bytes() == new
    assert Path(str(path) + ".bak").read_bytes() == old
    assert files.rom_file_read(str(path).encode(), output, len(old)) == 0
    assert bytes(output) == new
    assert files.rom_file_read(str(path).encode(), output, len(old) - 1) == 2
    assert files.rom_file_read(str(work / "absent.sav").encode(), output, len(old)) == 1
    # Bounded reads accept any non-empty file up to the capacity and report its length.
    files.rom_file_read_upto.argtypes = [C.c_char_p, ptr, C.c_size_t, C.POINTER(C.c_size_t)]
    files.rom_file_read_upto.restype = C.c_int
    short = work / "short.sav"; short.write_bytes(new[:100])
    big = (C.c_uint8 * (len(new) + 48))(); length = C.c_size_t()
    assert files.rom_file_read_upto(str(path).encode(), big, len(big), C.byref(length)) == 0
    assert length.value == len(new) and bytes(big[:len(new)]) == new
    assert files.rom_file_read_upto(str(short).encode(), big, len(big), C.byref(length)) == 0 and length.value == 100
    assert files.rom_file_read_upto(str(path).encode(), big, 100, C.byref(length)) == 2
    (work / "empty.sav").write_bytes(b"")
    assert files.rom_file_read_upto(str(work / "empty.sav").encode(), big, len(big), C.byref(length)) == 2
    assert files.rom_file_read_upto(str(work / "absent.sav").encode(), big, len(big), C.byref(length)) == 1
    # The temporary file cannot be opened, so the current save and backup survive.
    Path(str(path) + ".tmp").mkdir()
    assert not files.save_file_replace(str(path).encode(), old_buffer, len(old))
    assert path.read_bytes() == new
    assert Path(str(path) + ".bak").read_bytes() == old
print("PASS: exact and bounded battery reads, atomic replacement, backup, and failed-write preservation")
