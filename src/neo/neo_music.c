/*
 * neo_music.c — your own MP3s as race music.
 *
 * Put .mp3 files in a music/ folder next to neosmk (or in the working
 * directory) and turn on Sound → "Custom race music". While a race's course
 * music plays, the game's music voices are left out of the audio output and a
 * song from music/ plays instead; the sound effects (engine, items, Lakitu)
 * stay. Each race picks a different song; the folder is read again at every
 * race start, so files can be added while the game runs.
 *
 * How SMK's sound driver is read (sound CPU RAM):
 *   $04  song slot of the loaded bank: in a race 1 = loading, 3 = countdown
 *        tune, 4 = course music
 *   $1A  voices currently owned by sound effects (bit per DSP voice); the
 *        others play music
 * Presentation only: the emulated sound chip, its echo RAM and save-states
 * are untouched, so netplay and replays are unaffected.
 */
#include "neo.h"

#include <snesrecomp/bus.h>
#include <snesrecomp/menu_overlay.h>
#include <snesrecomp/snesrecomp.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "third_party/dr_mp3.h"

#define APU_SONG        0x04
#define APU_SFX_VOICES  0x1A
#define SONG_COURSE     4
#define MAX_SONGS       256
#define FADE_IN_S       0.6f
#define FADE_OUT_S      0.8f

static int s_enabled = 0;
static int s_volume = 2;        /* 0..3 -> 50 / 75 / 100 / 125 % */
static const char *const k_volume_names[] = { "50%", "75%", "100%", "125%", NULL };
static const float k_volume[] = { 0.5f, 0.75f, 1.0f, 1.25f };

/* the song playing */
static struct {
    bool       open;
    drmp3      mp3;
    SDL_IOStream *io;
    float      buf[4096 * 2];    /* decoded stereo frames */
    int        buf_n, buf_pos;
    double     frac;             /* resampling position between buf_pos and buf_pos + 1 */
    float      gain;             /* fade */
    int        fading;           /* +1 in, -1 out, 0 steady */
} s;

static char  s_files[MAX_SONGS][512];
static int   s_nfiles = 0;
static int   s_last = -1;
static bool  s_active = false;   /* replacing the course music now */
static int   s_prev_state = -1;

/* ---- file list ---- */

static void scan_dir(const char *dir) {
    int count = 0;
    char **list = SDL_GlobDirectory(dir, "*.mp3", SDL_GLOB_CASEINSENSITIVE, &count);
    if (!list) return;
    for (int i = 0; i < count && s_nfiles < MAX_SONGS; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, list[i]);
        bool dup = false;
        for (int k = 0; k < s_nfiles && !dup; k++) dup = strcmp(s_files[k], path) == 0;
        if (!dup) SDL_strlcpy(s_files[s_nfiles++], path, sizeof(s_files[0]));
    }
    SDL_free(list);
}

static void scan(void) {
    s_nfiles = 0;
    const char *base = SDL_GetBasePath();
    if (base) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%smusic", base);
        scan_dir(dir);
    }
    scan_dir("music");
}

/* ---- decoding ---- */

static size_t io_read(void *user, void *out, size_t n) {
    return SDL_ReadIO((SDL_IOStream *)user, out, n);
}

static drmp3_bool32 io_seek(void *user, int offset, drmp3_seek_origin origin) {
    SDL_IOWhence w = origin == drmp3_seek_origin_start ? SDL_IO_SEEK_SET : SDL_IO_SEEK_CUR;
    return SDL_SeekIO((SDL_IOStream *)user, offset, w) >= 0;
}

static void close_song(void) {
    if (s.open) drmp3_uninit(&s.mp3);
    if (s.io) SDL_CloseIO(s.io);
    memset(&s, 0, sizeof(s));
}

static int s_hook_frame;
static bool open_song(int idx) {
    close_song();
    s.io = SDL_IOFromFile(s_files[idx], "rb");
    if (!s.io) return false;
    if (!drmp3_init(&s.mp3, io_read, io_seek, s.io, NULL)) {
        SDL_CloseIO(s.io);
        s.io = NULL;
        printf("neo_music: can't decode %s\n", s_files[idx]);
        return false;
    }
    s.open = true;
    s.gain = 0.0f;
    s.fading = 1;
    printf("neo_music: playing %s (%u Hz, %u ch, frame %d)\n", s_files[idx], s.mp3.sampleRate, s.mp3.channels, s_hook_frame);
    return true;
}

/* Refill the decode buffer; loops the song at its end. */
static bool refill(void) {
    float tmp[4096 * 2];
    drmp3_uint64 want = 2048;
    drmp3_uint64 got = drmp3_read_pcm_frames_f32(&s.mp3, want, tmp);
    if (got == 0) {                                   /* end: loop */
        drmp3_seek_to_pcm_frame(&s.mp3, 0);
        got = drmp3_read_pcm_frames_f32(&s.mp3, want, tmp);
        if (got == 0) return false;
    }
    int ch = (int)s.mp3.channels;
    for (drmp3_uint64 i = 0; i < got; i++) {
        s.buf[i * 2] = tmp[i * ch];
        s.buf[i * 2 + 1] = tmp[i * ch + (ch > 1 ? 1 : 0)];
    }
    s.buf_n = (int)got;
    s.buf_pos = 0;
    return true;
}

static bool next_frame(float *l, float *r) {
    if (s.buf_pos >= s.buf_n && !refill()) return false;
    *l = s.buf[s.buf_pos * 2];
    *r = s.buf[s.buf_pos * 2 + 1];
    s.buf_pos++;
    return true;
}

/* Mix the song into one frame of game audio (linear resampling). */
static void mix(int16_t *out, int frames, int rate) {
    static float cur[2], nxt[2];
    static bool primed = false;
    if (s.buf_n == 0 && s.buf_pos == 0) primed = false;
    if (!primed) {
        if (!next_frame(&cur[0], &cur[1]) || !next_frame(&nxt[0], &nxt[1])) return;
        primed = true;
    }
    double step = (double)s.mp3.sampleRate / rate;
    float vol = k_volume[s_volume < 0 ? 0 : s_volume > 3 ? 3 : s_volume];
    float fade_step = 1.0f / ((s.fading > 0 ? FADE_IN_S : FADE_OUT_S) * rate);
    for (int i = 0; i < frames; i++) {
        float t = (float)s.frac;
        float l = cur[0] + (nxt[0] - cur[0]) * t;
        float r = cur[1] + (nxt[1] - cur[1]) * t;
        if (s.fading > 0) { s.gain += fade_step; if (s.gain >= 1.0f) { s.gain = 1.0f; s.fading = 0; } }
        if (s.fading < 0) { s.gain -= fade_step; if (s.gain <= 0.0f) { s.gain = 0.0f; } }
        float g = s.gain * vol * 32767.0f * 0.8f;
        int L = out[i * 2] + (int)(l * g), R = out[i * 2 + 1] + (int)(r * g);
        out[i * 2] = (int16_t)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        out[i * 2 + 1] = (int16_t)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
        s.frac += step;
        while (s.frac >= 1.0) {
            s.frac -= 1.0;
            cur[0] = nxt[0]; cur[1] = nxt[1];
            if (!next_frame(&nxt[0], &nxt[1])) { nxt[0] = nxt[1] = 0.0f; }
        }
    }
}

static int pick(void) {
    if (s_nfiles == 0) return -1;
    if (s_nfiles == 1) return 0;
    int i;
    do { i = (int)SDL_rand(s_nfiles); } while (i == s_last);
    return i;
}

/* ---- per frame ---- */

static int s_hook_frame;

static void audio_hook(int16_t *stereo, int frames, int rate, void *user) {
    (void)user;
    s_hook_frame++;
    bool racing = bus_wram_read16(SMK_RAM_GAME_STATE) == 0x0002;
    bool course_song = snesrecomp_apu_ram(APU_SONG) == SONG_COURSE;
    int state = racing ? (course_song ? 2 : 1) : 0;

    if (state != s_prev_state && state == 1) scan();   /* a race is loading: re-read music/ */
    if (s_prev_state == -1 && state == 2) scan();
    s_prev_state = state;

    bool want = s_enabled && state == 2;
    if (want && !s_active) {
        if (s_nfiles == 0) scan();
        int idx = pick();
        if (idx >= 0 && open_song(idx)) {
            s_last = idx;
            s_active = true;
            /* leave out the music voices; voices playing effects stay */
            snesrecomp_set_music_mask(0xFF, APU_SFX_VOICES);
        }
    } else if (!want && s_active) {
        if (s.fading >= 0) s.fading = -1;          /* fade out, game music back now */
        snesrecomp_set_music_mask(0, -1);
        if (s.gain <= 0.0f) { close_song(); s_active = false; }
    }
    if (s.open) {
        mix(stereo, frames, rate);
        if (!want && s.gain <= 0.0f) { close_song(); s_active = false; printf("neo_music: stopped (frame %d)\n", s_hook_frame); }
    }
}

void neo_music_install(void) {
    snesrecomp_set_audio_hook(audio_hook, NULL);
}

void neo_music_register_options(void) {
    menu_overlay_add_option("Sound", "Custom race music", "neo_music", &s_enabled, 1, NULL,
                            "Play your own MP3s as race music: put .mp3 files in a music\n"
                            "folder next to neosmk. A different song each race; the sound\n"
                            "effects stay. New files are picked up at the next race.");
    menu_overlay_add_option("Sound", "Race music volume", "neo_music_volume", &s_volume, 3,
                            k_volume_names, "Volume of the custom race music.");
}
