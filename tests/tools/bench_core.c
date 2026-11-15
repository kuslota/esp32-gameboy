#include "gb_emulator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t hash_bytes(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* Saved state embeds host pointers that move with ASLR; BENCH_MASK is a byte map of
 * offsets to zero before hashing, built by tests/tools/make_state_mask.py. */
static uint8_t *state_mask;
static size_t state_mask_size;

static void mask_pointers(uint8_t *buf, size_t size)
{
    for (size_t i = 0; i < size && i < state_mask_size; ++i) if (state_mask[i]) buf[i] = 0;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static uint8_t buttons_for(unsigned frame)
{
    if (frame < 120) return 0;
    if (frame < 600) return (frame / 8) & 1 ? GB_BTN_START : (frame / 5) & 1 ? GB_BTN_A : 0;
    uint32_t seed = (frame / 12) * 2654435761u;
    seed ^= seed >> 13; seed *= 0x5bd1e995u; seed ^= seed >> 15;
    uint8_t pad = 0;
    if (seed & 1) pad |= GB_BTN_A;
    if (seed & 6) pad |= GB_BTN_B * ((seed >> 1) & 1);
    pad |= (uint8_t[]){0, GB_BTN_LEFT, GB_BTN_RIGHT, GB_BTN_UP, GB_BTN_DOWN, 0, GB_BTN_LEFT, GB_BTN_RIGHT}[(seed >> 4) & 7];
    if (((seed >> 8) & 31) == 0) pad |= GB_BTN_START;
    return pad;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const unsigned frames = argc > 2 ? (unsigned)atoi(argv[2]) : 20000;
    const unsigned reps = argc > 3 ? (unsigned)atoi(argv[3]) : 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t rom[GB_MAX_ROM_BYTES];
    size_t size = fread(rom, 1, sizeof(rom), f);
    fclose(f);
    static int16_t audio[1024];
    if (getenv("BENCH_MASK")) {
        FILE *m = fopen(getenv("BENCH_MASK"), "rb");
        state_mask = malloc(1 << 20);
        state_mask_size = m ? fread(state_mask, 1, 1 << 20, m) : 0;
        if (m) fclose(m);
    }

    double best_total = 1e18, best_render = 0, best_skip = 0;
    uint64_t fb = 0, au = 0, st = 0, first_fb = 0, first_au = 0, first_st = 0;
    unsigned rendered = 0, skipped = 0, audio_total = 0, changes = 0;
    uint64_t last = 0;
    for (unsigned rep = 0; rep < reps; ++rep) {
        if (!gb_emulator_init(rom, size)) { fprintf(stderr, "init failed\n"); return 1; }
        fb = au = st = 0xcbf29ce484222325ull;
        double t_render = 0, t_skip = 0;
        rendered = skipped = audio_total = changes = 0;
        for (unsigned n = 0; n < frames; ++n) {
            const bool skip = n & 1;
            gb_emulator_buttons(buttons_for(n));
            double t0 = now_us();
            bool ok = gb_emulator_frame(skip);
            double dt = now_us() - t0;
            if (!ok) { fprintf(stderr, "frame %u failed err=%d\n", n, gb_emulator_error()); return 1; }
            size_t count = gb_emulator_audio_samples(audio, sizeof(audio) / sizeof(audio[0]));
            au = hash_bytes(au, audio, count * sizeof(int16_t));
            audio_total += (unsigned)count;
            if (skip) { t_skip += dt; ++skipped; }
            else {
                t_render += dt; ++rendered;
                uint64_t one = hash_bytes(0xcbf29ce484222325ull, gb_emulator_pixels(), GB_PACKED_BYTES);
                changes += one != last; last = one;
                fb = hash_bytes(fb, gb_emulator_pixels(), GB_PACKED_BYTES);
            }
            if (rep == 0 && n % 1000 == 999) {
                size_t sz = gb_emulator_state_size();
                uint8_t *buf = malloc(sz);
                gb_emulator_state_copy(buf, sz);
                if (getenv("BENCH_DUMP")) { FILE *d = fopen(getenv("BENCH_DUMP"), "ab"); fwrite(buf, 1, sz, d); fclose(d); }
                mask_pointers(buf, sz);
                st = hash_bytes(st, buf, sz);
                free(buf);
            }
        }
        if (rep == 0) { first_fb = fb; first_au = au; first_st = st; }
        else if (fb != first_fb || au != first_au) { fprintf(stderr, "rep %u differs from rep 0\n", rep); return 1; }
        if (t_render + t_skip < best_total) { best_total = t_render + t_skip; best_render = t_render; best_skip = t_skip; }
    }
    printf("frames=%u rendered=%u skipped=%u audio_samples=%u fb_changes=%u\n", frames, rendered, skipped, audio_total, changes);
    printf("fb_hash=%016llx\naudio_hash=%016llx\nstate_hash=%016llx\n",
           (unsigned long long)first_fb, (unsigned long long)first_au, (unsigned long long)first_st);
    printf("us_per_frame_rendered=%.2f us_per_frame_skipped=%.2f us_per_frame_avg=%.2f\n",
           best_render / rendered, best_skip / skipped, best_total / frames);
    return 0;
}
