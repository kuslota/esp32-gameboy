#!/usr/bin/env python3
"""Build the BENCH_MASK byte map: make_state_mask.py OUT DUMP... with STATE_SIZE set to gb_emulator_state_size().
Pointers sit at offset 4 mod 8 in the saved state and vary with ASLR, so a differing byte masks its whole word."""
import os
import sys
dumps = [open(p, 'rb').read() for p in sys.argv[2:]]
state = int(os.environ["STATE_SIZE"])
mask = bytearray(state)
for i in range(len(dumps[0])):
    if len({d[i] for d in dumps}) > 1:
        start = (i % state - 4) // 8 * 8 + 4
        mask[start:start + 8] = b'\1' * 8
open(sys.argv[1], 'wb').write(mask)
print('masked bytes', sum(mask))
