/* doomgeneric_psp.c – Batman Doom on PSP
 * Ported from doomgeneric_vita.c
 * Uses PSP GU for hardware-accelerated scaling (320x200 -> 480x272)
 * Audio: sceAudio with OPL3 music + 16-channel SFX mixer
 */
#include "d_event.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_argv.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

extern void D_PostEvent(event_t *ev);
extern char *savegamedir;
extern void G_SaveGame(int slot, char *description);
extern void G_LoadGame(char *name);
extern char *P_SaveGameFile(int slot);
extern int gamestate;
extern int gameaction;
extern int usergame;

#include "opl3.h"
#include <math.h>
#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#include <psputility.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* PSP module info - required for homebrew */
PSP_MODULE_INFO("BatmanDoom", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(256); /* 256KB stack for main thread */
PSP_HEAP_SIZE_KB(20 * 1024);        /* 20MB heap — leaves room for stack+OS */

#define DATA_PATH "ms0:/PSP/GAME/BATDOOM/"

#define TICRATE 35
#define DOOM_W 320
#define DOOM_H 200
#define SCR_W 480
#define SCR_H 272
#define PSP_STRIDE 512
#define TEX_W 512 /* next power-of-2 >= 320 */
#define TEX_H 256 /* next power-of-2 >= 200 */
#define OUTPUT_RATE 44100
#define AUDIO_GRANULARITY 512
#define MIX_CHANNELS 16

byte *I_VideoBuffer = NULL;
int screenvisible = 1, vanilla_keyboard_mapping = 0;
boolean screensaver_mode = false;
int usegamma = 0, usemouse = 0, snd_musicdevice = 3;
float mouse_acceleration = 0;
int mouse_threshold = 0;

/* ---- Display ---- */
static uint32_t __attribute__((
    aligned(16))) gu_list[4096]; /* GU command list – 16KB is plenty */
static uint32_t __attribute__((aligned(16))) tex_buf[TEX_W * TEX_H];
static uint32_t cmap[256];
static int display_ready = 0;

typedef struct {
  float u, v;
  float x, y, z;
} tex_vertex_t;

static void init_display(void) {
  sceGuInit();
  sceGuStart(GU_DIRECT, gu_list);

  /* GU buffers are VRAM-relative offsets, NOT absolute addresses */
  sceGuDrawBuffer(GU_PSM_8888, (void *)0, PSP_STRIDE);
  sceGuDispBuffer(SCR_W, SCR_H, (void *)(PSP_STRIDE * SCR_H * 4), PSP_STRIDE);
  /* No depth buffer needed for 2D blitting */

  sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
  sceGuViewport(2048, 2048, SCR_W, SCR_H);
  sceGuScissor(0, 0, SCR_W, SCR_H);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_BLEND);
  sceGuDisable(GU_ALPHA_TEST);
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_8888, 0, 0, 0);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuFinish();
  sceGuSync(0, 0);
  sceGuDisplay(GU_TRUE);
  display_ready = 1;
}

static uint32_t get_ms(void) {
  uint64_t tick;
  sceRtcGetCurrentTick(&tick);
  static uint64_t base = 0;
  if (!base)
    base = tick;
  return (uint32_t)((tick - base) / (sceRtcGetTickResolution() / 1000));
}
static uint32_t base_time = 0;

static void debug_log(const char *msg) {
  FILE *f = fopen(DATA_PATH "debug.log", "a");
  if (f) {
    fprintf(f, "%s\n", msg);
    fclose(f);
  }
}
static void debug_logf(const char *fmt, ...) {
  char buf[512];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof(buf), fmt, a);
  va_end(a);
  debug_log(buf);
}

/* ---- Doom callbacks ---- */
void DG_Init(void) {
  I_VideoBuffer = (byte *)malloc(DOOM_W * DOOM_H);
  if (!I_VideoBuffer) {
    debug_log("FATAL: VideoBuffer alloc failed");
    sceKernelExitGame();
  }
  memset(tex_buf, 0, sizeof(tex_buf));
  debug_log("DG_Init OK");
}

void DG_DrawFrame(void) {
  int x, y;
  if (!I_VideoBuffer || !display_ready)
    return;

  /* Apply palette to doom's indexed framebuffer -> RGBA texture */
  const byte *src = I_VideoBuffer;
  for (y = 0; y < DOOM_H; y++) {
    uint32_t *row = tex_buf + y * TEX_W;
    for (x = 0; x < DOOM_W; x++)
      row[x] = cmap[src[y * DOOM_W + x]];
  }
  sceKernelDcacheWritebackAll();

  sceGuStart(GU_DIRECT, gu_list);
  sceGuClearColor(0xFF000000);
  sceGuClear(GU_COLOR_BUFFER_BIT);

  sceGuTexImage(0, TEX_W, TEX_H, TEX_W, tex_buf);
  sceGuTexSync();

  /* Draw scaled quad: 320x200 portion of TEX (512x256) -> 480x272 screen */
  {
    tex_vertex_t *v = (tex_vertex_t *)sceGuGetMemory(2 * sizeof(tex_vertex_t));
    v[0].u = 0.0f;
    v[0].v = 0.0f;
    v[0].x = 0;
    v[0].y = 0;
    v[0].z = 0;
    v[1].u = (float)DOOM_W;
    v[1].v = (float)DOOM_H;
    v[1].x = SCR_W;
    v[1].y = SCR_H;
    v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2,
                   NULL, v);
  }

  sceGuFinish();
  sceGuSync(0, 0);
  sceGuSwapBuffers();
}

void DG_SleepMs(uint32_t ms) { sceKernelDelayThread(ms * 1000); }

uint32_t DG_GetTicksMs(void) { return get_ms() - base_time; }

void DG_SetWindowTitle(const char *title) { (void)title; }

/* ---- Input ---- */
#define KQUEUE_SZ 64
#define DEADZONE 40
static struct {
  int pressed;
  unsigned char key;
} kq[KQUEUE_SZ];
static int kq_r = 0, kq_w = 0;
static SceCtrlData pad_prev;
static int input_init = 0,
           analog_held[4]; /* PSP has 1 nub: fwd/back + strafe */
static int current_weapon = 1, weapon_cycle_cooldown = 0;
static unsigned char pending_weapon_release = 0;
static int quicksave_cooldown = 0, quickload_cooldown = 0;

static void kq_push(int p, unsigned char k) {
  int n = (kq_w + 1) % KQUEUE_SZ;
  if (n == kq_r)
    return;
  kq[kq_w].pressed = p;
  kq[kq_w].key = k;
  kq_w = n;
}
static void analog_axis(int val, int nk, int pk, int *nh, int *ph) {
  int wn = val<-DEADZONE, wp = val> DEADZONE;
  if (wn && !*nh) {
    kq_push(1, nk);
    *nh = 1;
  }
  if (!wn && *nh) {
    kq_push(0, nk);
    *nh = 0;
  }
  if (wp && !*ph) {
    kq_push(1, pk);
    *ph = 1;
  }
  if (!wp && *ph) {
    kq_push(0, pk);
    *ph = 0;
  }
}

static void do_poll_input(void) {
  SceCtrlData pad;
  int i;
  if (!input_init) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    memset(&pad_prev, 0, sizeof(pad_prev));
    memset(analog_held, 0, sizeof(analog_held));
    input_init = 1;
  }
  sceCtrlPeekBufferPositive(&pad, 1);

  if (weapon_cycle_cooldown > 0)
    weapon_cycle_cooldown--;
  if (quicksave_cooldown > 0)
    quicksave_cooldown--;
  if (quickload_cooldown > 0)
    quickload_cooldown--;

  /* Face buttons + triggers
   * PSP layout: Cross=use, Square=fire, Triangle=automap, Circle=strafe
   * R=fire, L=run, Start=menu, Select=confirm
   * D-pad: left/right = turn, up/down = move (when no nub available)
   * L+R+START = exit
   */
  struct {
    unsigned btn;
    unsigned char key;
  } bm[] = {{PSP_CTRL_CROSS, KEY_USE},
            {PSP_CTRL_SQUARE, KEY_FIRE},
            {PSP_CTRL_CIRCLE, KEY_RALT},  /* strafe modifier */
            {PSP_CTRL_TRIANGLE, KEY_TAB}, /* automap */
            {PSP_CTRL_RTRIGGER, KEY_FIRE},
            {PSP_CTRL_LTRIGGER, KEY_RSHIFT}, /* run */
            {PSP_CTRL_START, KEY_ESCAPE},
            {PSP_CTRL_SELECT, KEY_ENTER},
            {0, 0}};
  for (i = 0; bm[i].btn; i++) {
    int now = (pad.Buttons & bm[i].btn) != 0;
    int was = (pad_prev.Buttons & bm[i].btn) != 0;
    if (now && !was)
      kq_push(1, bm[i].key);
    if (!now && was)
      kq_push(0, bm[i].key);
  }

  /* Exit combo: L+R+Start */
  if ((pad.Buttons & PSP_CTRL_LTRIGGER) && (pad.Buttons & PSP_CTRL_RTRIGGER) &&
      (pad.Buttons & PSP_CTRL_START)) {
    debug_log("Exit combo pressed");
    sceKernelExitGame();
  }

  /* Release pending weapon key */
  if (pending_weapon_release) {
    kq_push(0, pending_weapon_release);
    pending_weapon_release = 0;
  }

  /* D-Pad: turn left/right; up/down = move if no nub input */
  {
    int u = (pad.Buttons & PSP_CTRL_UP) != 0;
    int d = (pad.Buttons & PSP_CTRL_DOWN) != 0;
    int l = (pad.Buttons & PSP_CTRL_LEFT) != 0;
    int r = (pad.Buttons & PSP_CTRL_RIGHT) != 0;
    int uw = (pad_prev.Buttons & PSP_CTRL_UP) != 0;
    int dw = (pad_prev.Buttons & PSP_CTRL_DOWN) != 0;
    int lw = (pad_prev.Buttons & PSP_CTRL_LEFT) != 0;
    int rw = (pad_prev.Buttons & PSP_CTRL_RIGHT) != 0;

    /* L+D-UP = quick save */
    if ((pad.Buttons & PSP_CTRL_LTRIGGER) && u && !uw &&
        quicksave_cooldown == 0 && gamestate == 0 && usergame) {
      G_SaveGame(0, "PSP SAVE");
      debug_log("QuickSave slot 0");
      quicksave_cooldown = TICRATE * 2;
    }
    /* L+D-DOWN = quick load */
    else if ((pad.Buttons & PSP_CTRL_LTRIGGER) && d && !dw &&
             quickload_cooldown == 0 && gamestate == 0 && usergame) {
      char *path = P_SaveGameFile(0);
      if (path) {
        G_LoadGame(path);
        debug_logf("QuickLoad: %s", path);
      }
      quickload_cooldown = TICRATE * 2;
    }
    /* D-pad turn (when L not held) */
    else if (!(pad.Buttons & PSP_CTRL_LTRIGGER)) {
      if (l && !lw)
        kq_push(1, KEY_LEFTARROW);
      if (!l && lw)
        kq_push(0, KEY_LEFTARROW);
      if (r && !rw)
        kq_push(1, KEY_RIGHTARROW);
      if (!r && rw)
        kq_push(0, KEY_RIGHTARROW);
    }

    /* L+D-LEFT/RIGHT = cycle weapons */
    if ((pad.Buttons & PSP_CTRL_LTRIGGER)) {
      if (r && !rw && weapon_cycle_cooldown == 0) {
        current_weapon++;
        if (current_weapon > 7)
          current_weapon = 1;
        kq_push(1, '0' + current_weapon);
        pending_weapon_release = '0' + current_weapon;
        weapon_cycle_cooldown = 10;
      }
      if (l && !lw && weapon_cycle_cooldown == 0) {
        current_weapon--;
        if (current_weapon < 1)
          current_weapon = 7;
        kq_push(1, '0' + current_weapon);
        pending_weapon_release = '0' + current_weapon;
        weapon_cycle_cooldown = 10;
      }
    }
  }

  /* Analog nub: lx = strafe, ly = forward/back */
  analog_axis((int)pad.Lx - 128, KEY_STRAFE_L, KEY_STRAFE_R, &analog_held[0],
              &analog_held[1]);
  analog_axis((int)pad.Ly - 128, KEY_UPARROW, KEY_DOWNARROW, &analog_held[2],
              &analog_held[3]);

  pad_prev = pad;
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
  if (kq_r == kq_w) {
    do_poll_input();
  }
  if (kq_r == kq_w)
    return 0;
  *pressed = kq[kq_r].pressed;
  *doomKey = kq[kq_r].key;
  kq_r = (kq_r + 1) % KQUEUE_SZ;
  return 1;
}

/* ---- SFX ENGINE ---- */
typedef struct {
  const byte *data;
  int length, pos_fixed, step_fixed;
  int vol_left, vol_right, handle, active, lumpnum;
} mix_channel_t;

static mix_channel_t mix_ch[MIX_CHANNELS];
static int sfx_port = -1;
static SceUID sfx_thread_id = -1, sfx_sema = -1;
static volatile int sfx_running = 0;
static volatile int sfx_master_vol = 15;
static int next_handle = 1, audio_ready = 0;
static int16_t __attribute__((aligned(64))) sfx_buf[2][AUDIO_GRANULARITY * 2];
static int sfx_buf_idx = 0;

#define SFX_CACHE_MAX 128
typedef struct {
  int lumpnum;
  const byte *samples;
  int length, samplerate;
} sfx_cache_entry_t;
static sfx_cache_entry_t sfx_cache[SFX_CACHE_MAX];
static int sfx_cache_count = 0;

static sfx_cache_entry_t *sfx_cache_get(int lumpnum) {
  int i;
  byte *raw;
  int rawlen, format, rate, nsamples;
  for (i = 0; i < sfx_cache_count; i++)
    if (sfx_cache[i].lumpnum == lumpnum)
      return &sfx_cache[i];
  rawlen = W_LumpLength(lumpnum);
  if (rawlen < 8)
    return NULL;
  raw = W_CacheLumpNum(lumpnum, PU_STATIC);
  if (!raw)
    return NULL;
  format = raw[0] | (raw[1] << 8);
  rate = raw[2] | (raw[3] << 8);
  nsamples = raw[4] | (raw[5] << 8) | (raw[6] << 16) | (raw[7] << 24);
  if (format != 3)
    return NULL;
  if (rate < 4000 || rate > 48000)
    rate = 11025;
  if (nsamples > rawlen - 8)
    nsamples = rawlen - 8;
  if (nsamples <= 0)
    return NULL;
  {
    const byte *pcm = raw + 8;
    int pcm_len = nsamples;
    if (pcm_len > 32) {
      pcm += 16;
      pcm_len -= 32;
    }
    if (sfx_cache_count >= SFX_CACHE_MAX)
      return NULL;
    i = sfx_cache_count++;
    sfx_cache[i].lumpnum = lumpnum;
    sfx_cache[i].samples = pcm;
    sfx_cache[i].length = pcm_len;
    sfx_cache[i].samplerate = rate;
  }
  return &sfx_cache[i];
}

static int32_t __attribute__((aligned(16))) sfx_accum[AUDIO_GRANULARITY * 2];

static void sfx_mix(int16_t *out, int nframes) {
  int i, ch, step, idx, s, mv;
  memset(sfx_accum, 0, nframes * 2 * sizeof(int32_t));
  if (sfx_sema >= 0)
    sceKernelWaitSema(sfx_sema, 1, NULL);
  for (ch = 0; ch < MIX_CHANNELS; ch++) {
    mix_channel_t *c = &mix_ch[ch];
    if (!c->active || !c->data)
      continue;
    step = c->step_fixed;
    for (i = 0; i < nframes; i++) {
      idx = c->pos_fixed >> 16;
      if (idx >= c->length) {
        c->active = 0;
        break;
      }
      s = ((int)c->data[idx] - 128) << 8;
      sfx_accum[i * 2] += (s * c->vol_left) / 127;
      sfx_accum[i * 2 + 1] += (s * c->vol_right) / 127;
      c->pos_fixed += step;
    }
  }
  if (sfx_sema >= 0)
    sceKernelSignalSema(sfx_sema, 1);
  mv = sfx_master_vol;
  for (i = 0; i < nframes * 2; i++) {
    int32_t val = (sfx_accum[i] * mv) / 15;
    if (val > 32767)
      val = 32767;
    if (val < -32768)
      val = -32768;
    out[i] = (int16_t)val;
  }
}

/* ---- OPL3 Music Engine (same as Vita, mutex replaced with PSP sema) ---- */
#define GENMIDI_NUM_INSTRS 175
#define GENMIDI_HEADER "#OPL_II#"
#define GENMIDI_FLAG_FIXED 0x0001
#define OPL_NUM_VOICES 9
#define PERCUSSION_CHAN 15

#pragma pack(push, 1)
typedef struct {
  uint8_t tremolo, attack, sustain, waveform, scale, level;
} genmidi_op_t;
typedef struct {
  genmidi_op_t modulator;
  uint8_t feedback;
  genmidi_op_t carrier;
  uint8_t unused;
  int16_t base_note_offset;
} genmidi_voice_t;
typedef struct {
  uint16_t flags;
  uint8_t fine_tuning, fixed_note;
  genmidi_voice_t voices[2];
} genmidi_instr_t;
#pragma pack(pop)

static const uint8_t opl_mod_offset[9] = {0x00, 0x01, 0x02, 0x08, 0x09,
                                          0x0A, 0x10, 0x11, 0x12};
static const uint8_t opl_car_offset[9] = {0x03, 0x04, 0x05, 0x0B, 0x0C,
                                          0x0D, 0x13, 0x14, 0x15};
static const uint16_t opl_freq_table[12] = {0x157, 0x16B, 0x181, 0x198,
                                            0x1B0, 0x1CA, 0x1E5, 0x202,
                                            0x220, 0x241, 0x263, 0x287};

typedef struct {
  int active, mus_channel, note, volume;
  uint32_t age;
} opl_voice_t;
typedef struct {
  int volume, patch, pitch_bend;
} opl_mus_chan_t;
typedef struct {
  opl3_chip chip;
  genmidi_instr_t *genmidi;
  int genmidi_loaded;
  opl_voice_t voices[OPL_NUM_VOICES];
  opl_mus_chan_t channels[16];
  uint32_t voice_age;
  const byte *mus_data;
  int mus_len, mus_pos, score_start, score_len;
  int playing, looping, delay_left, tick_samples, tick_counter, music_volume;
} opl_music_t;

static opl_music_t opl_music;
static SceUID mus_sema = -1;
static uint8_t opl_reg_b0[9];

static void opl_write(uint16_t reg, uint8_t val) {
  OPL3_WriteReg(&opl_music.chip, reg, val);
}

static void load_genmidi(void) {
  int lump;
  byte *data;
  int len;
  opl_music.genmidi_loaded = 0;
  lump = W_CheckNumForName("GENMIDI");
  if (lump < 0) {
    debug_log("GENMIDI not found");
    return;
  }
  len = W_LumpLength(lump);
  data = W_CacheLumpNum(lump, PU_STATIC);
  if (len < 8 + (int)sizeof(genmidi_instr_t) * GENMIDI_NUM_INSTRS)
    return;
  if (memcmp(data, GENMIDI_HEADER, 8) != 0)
    return;
  opl_music.genmidi = (genmidi_instr_t *)(data + 8);
  opl_music.genmidi_loaded = 1;
}

static void opl_write_op(int slot, genmidi_op_t *op, int vol) {
  int l, fl;
  opl_write(0x20 + slot, op->tremolo);
  if (vol >= 0) {
    l = 0x3F - (op->level & 0x3F);
    l = (l * vol) / 127;
    fl = 0x3F - l;
    if (fl < 0)
      fl = 0;
    if (fl > 0x3F)
      fl = 0x3F;
    opl_write(0x40 + slot, (op->scale & 0xC0) | fl);
  } else
    opl_write(0x40 + slot, (op->scale & 0xC0) | (op->level & 0x3F));
  opl_write(0x60 + slot, op->attack);
  opl_write(0x80 + slot, op->sustain);
  opl_write(0xE0 + slot, op->waveform & 0x07);
}
static void opl_set_instrument(int v, genmidi_voice_t *gv, int volume) {
  int add = gv->feedback & 0x01;
  opl_write(0xC0 + v, (gv->feedback & 0x0F) | 0x30);
  opl_write_op(opl_mod_offset[v], &gv->modulator, add ? volume : -1);
  opl_write_op(opl_car_offset[v], &gv->carrier, volume);
}
static void opl_update_volume(int v, int volume, genmidi_voice_t *gv) {
  int l, fl;
  l = 0x3F - (gv->carrier.level & 0x3F);
  l = (l * volume) / 127;
  fl = 0x3F - l;
  if (fl < 0)
    fl = 0;
  if (fl > 0x3F)
    fl = 0x3F;
  opl_write(0x40 + opl_car_offset[v], (gv->carrier.scale & 0xC0) | fl);
  if (gv->feedback & 0x01) {
    l = 0x3F - (gv->modulator.level & 0x3F);
    l = (l * volume) / 127;
    fl = 0x3F - l;
    if (fl < 0)
      fl = 0;
    if (fl > 0x3F)
      fl = 0x3F;
    opl_write(0x40 + opl_mod_offset[v], (gv->modulator.scale & 0xC0) | fl);
  }
}
static void opl_key_on(int v, int note) {
  int oct, fn;
  uint16_t freq;
  if (note < 0)
    note = 0;
  if (note > 127)
    note = 127;
  oct = (note / 12) - 1;
  fn = note % 12;
  if (oct < 0)
    oct = 0;
  if (oct > 7)
    oct = 7;
  freq = opl_freq_table[fn];
  opl_write(0xA0 + v, freq & 0xFF);
  opl_reg_b0[v] = 0x20 | ((oct & 7) << 2) | ((freq >> 8) & 3);
  opl_write(0xB0 + v, opl_reg_b0[v]);
}
static void opl_key_off(int v) {
  opl_reg_b0[v] &= ~0x20;
  opl_write(0xB0 + v, opl_reg_b0[v]);
}
static void opl_silence_voice(int v) {
  opl_reg_b0[v] = 0;
  opl_write(0xB0 + v, 0);
  opl_write(0xA0 + v, 0);
}

static int opl_alloc_voice(int ch, int pri) {
  int i, best;
  uint32_t oldest;
  (void)pri;
  (void)ch;
  for (i = 0; i < OPL_NUM_VOICES; i++)
    if (!opl_music.voices[i].active)
      return i;
  best = 0;
  oldest = 0xFFFFFFFF;
  for (i = 0; i < OPL_NUM_VOICES; i++)
    if (opl_music.voices[i].age < oldest) {
      oldest = opl_music.voices[i].age;
      best = i;
    }
  opl_key_off(best);
  opl_music.voices[best].active = 0;
  return best;
}
static genmidi_voice_t *get_voice_instr(int vi) {
  int ch, patch;
  if (!opl_music.genmidi_loaded)
    return NULL;
  ch = opl_music.voices[vi].mus_channel;
  patch = opl_music.channels[ch].patch;
  if (ch == PERCUSSION_CHAN) {
    int n = opl_music.voices[vi].note;
    if (n >= 35 && n <= 81)
      patch = 128 + n - 35;
    else
      return NULL;
  }
  if (patch < 0 || patch >= GENMIDI_NUM_INSTRS)
    return NULL;
  return &opl_music.genmidi[patch].voices[0];
}
static void mus_opl_note_on(int ch, int note, int vol) {
  int voice, patch, mn;
  genmidi_instr_t *inst;
  genmidi_voice_t *gv;
  if (!opl_music.genmidi_loaded)
    return;
  patch = opl_music.channels[ch].patch;
  if (ch == PERCUSSION_CHAN) {
    if (note < 35 || note > 81)
      return;
    patch = 128 + note - 35;
  }
  if (patch < 0 || patch >= GENMIDI_NUM_INSTRS)
    return;
  inst = &opl_music.genmidi[patch];
  gv = &inst->voices[0];
  voice = opl_alloc_voice(ch, vol >= 0 ? vol : 64);
  if (inst->flags & GENMIDI_FLAG_FIXED)
    mn = inst->fixed_note;
  else {
    mn = note;
    int off = (int)(int16_t)gv->base_note_offset;
    if (off > -48 && off < 48)
      mn += off;
  }
  if (mn < 0)
    mn = 0;
  if (mn > 127)
    mn = 127;
  if (vol < 0)
    vol = opl_music.channels[ch].volume;
  if (vol > 127)
    vol = 127;
  opl_set_instrument(voice, gv, vol);
  opl_key_on(voice, mn);
  opl_music.voices[voice].active = 1;
  opl_music.voices[voice].mus_channel = ch;
  opl_music.voices[voice].note = note;
  opl_music.voices[voice].volume = vol;
  opl_music.voices[voice].age = opl_music.voice_age++;
}
static void mus_opl_note_off(int ch, int note) {
  int i;
  for (i = 0; i < OPL_NUM_VOICES; i++)
    if (opl_music.voices[i].active && opl_music.voices[i].mus_channel == ch &&
        opl_music.voices[i].note == note) {
      opl_key_off(i);
      opl_music.voices[i].active = 0;
    }
}
static void mus_opl_all_off(int ch) {
  int i;
  for (i = 0; i < OPL_NUM_VOICES; i++)
    if (opl_music.voices[i].mus_channel == ch && opl_music.voices[i].active) {
      opl_key_off(i);
      opl_music.voices[i].active = 0;
    }
}
static byte mus_rb(void) {
  if (opl_music.mus_pos >= opl_music.mus_len)
    return 0;
  return opl_music.mus_data[opl_music.mus_pos++];
}
static void mus_process_event(void) {
  byte ev, channel, type;
  int last, i;
  if (!opl_music.playing)
    return;
  if (opl_music.mus_pos >= opl_music.score_start + opl_music.score_len) {
    if (opl_music.looping) {
      opl_music.mus_pos = opl_music.score_start;
      for (i = 0; i < OPL_NUM_VOICES; i++) {
        opl_silence_voice(i);
        opl_music.voices[i].active = 0;
      }
    } else
      opl_music.playing = 0;
    return;
  }
  ev = mus_rb();
  channel = ev & 0x0F;
  type = (ev >> 4) & 0x07;
  last = ev & 0x80;
  switch (type) {
  case 0: {
    byte n = mus_rb();
    mus_opl_note_off(channel, n & 0x7F);
    break;
  }
  case 1: {
    byte nb = mus_rb();
    int n = nb & 0x7F, v = -1;
    if (nb & 0x80) {
      v = mus_rb() & 0x7F;
      opl_music.channels[channel].volume = v;
    }
    mus_opl_note_on(channel, n, v);
    break;
  }
  case 2: {
    opl_music.channels[channel].pitch_bend = mus_rb();
    break;
  }
  case 3: {
    byte sys = mus_rb();
    if (sys == 10 || sys == 11 || sys == 14)
      mus_opl_all_off(channel);
    break;
  }
  case 4: {
    byte ctrl = mus_rb(), val = mus_rb();
    if (ctrl == 0)
      opl_music.channels[channel].patch = val;
    else if (ctrl == 3) {
      opl_music.channels[channel].volume = val & 0x7F;
      for (i = 0; i < OPL_NUM_VOICES; i++) {
        if (opl_music.voices[i].active &&
            opl_music.voices[i].mus_channel == channel) {
          genmidi_voice_t *gv = get_voice_instr(i);
          if (gv) {
            int cv = (opl_music.voices[i].volume * (val & 0x7F)) / 127;
            if (cv > 127)
              cv = 127;
            opl_update_volume(i, cv, gv);
          }
        }
      }
    }
    break;
  }
  case 5:
  case 6:
    if (opl_music.looping) {
      opl_music.mus_pos = opl_music.score_start;
      for (i = 0; i < OPL_NUM_VOICES; i++) {
        opl_silence_voice(i);
        opl_music.voices[i].active = 0;
      }
    } else
      opl_music.playing = 0;
    return;
  default:
    break;
  }
  if (last) {
    int delay = 0;
    byte db;
    do {
      db = mus_rb();
      delay = (delay << 7) | (db & 0x7F);
    } while (db & 0x80);
    opl_music.delay_left = delay;
  }
}
static void mus_opl_tick(void) {
  if (!opl_music.playing)
    return;
  while (opl_music.delay_left <= 0 && opl_music.playing)
    mus_process_event();
  if (opl_music.delay_left > 0)
    opl_music.delay_left--;
}

/* Mix OPL3 output into accumulator */
static int16_t __attribute__((aligned(16))) opl_tmp_buf[AUDIO_GRANULARITY * 2];

static void opl_mix_into(int32_t *accum, int ns) {
  int s, mv, tc, ts, l, r;
  if (!opl_music.playing)
    return;
  tc = opl_music.tick_counter;
  ts = opl_music.tick_samples;
  for (s = 0; s < ns; s++)
    OPL3_GenerateResampled(&opl_music.chip, opl_tmp_buf + s * 2);
  mv = opl_music.music_volume;
  for (s = 0; s < ns; s++) {
    tc--;
    if (tc <= 0) {
      mus_opl_tick();
      tc = ts;
    }
    l = (int)opl_tmp_buf[s * 2] * mv / 15;
    r = (int)opl_tmp_buf[s * 2 + 1] * mv / 15;
    if (l > 32767)
      l = 32767;
    if (l < -32768)
      l = -32768;
    if (r > 32767)
      r = 32767;
    if (r < -32768)
      r = -32768;
    accum[s * 2] += l;
    accum[s * 2 + 1] += r;
  }
  opl_music.tick_counter = tc;
}

/* ---- Audio thread ---- */
static int32_t __attribute__((aligned(16))) mus_accum[AUDIO_GRANULARITY * 2];

static int audio_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  int ch, i;
  int32_t s;
  ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, AUDIO_GRANULARITY,
                         PSP_AUDIO_FORMAT_STEREO);
  if (ch < 0) {
    debug_logf("sceAudioChReserve failed: 0x%08X", ch);
    return -1;
  }
  sfx_port = ch;
  sfx_running = 1;
  while (sfx_running) {
    int16_t *buf = sfx_buf[sfx_buf_idx ^= 1];
    /* Mix SFX */
    sfx_mix(buf, AUDIO_GRANULARITY);
    /* Add OPL3 music on top */
    if (mus_sema >= 0)
      sceKernelWaitSema(mus_sema, 1, NULL);
    memset(mus_accum, 0, sizeof(mus_accum));
    opl_mix_into(mus_accum, AUDIO_GRANULARITY);
    for (i = 0; i < AUDIO_GRANULARITY * 2; i++) {
      s = (int32_t)buf[i] + mus_accum[i];
      if (s > 32767)
        s = 32767;
      if (s < -32768)
        s = -32768;
      buf[i] = (int16_t)s;
    }
    if (mus_sema >= 0)
      sceKernelSignalSema(mus_sema, 1);
    sceAudioOutputPannedBlocking(ch, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX,
                                 buf);
  }
  sceAudioChRelease(ch);
  return 0;
}

static void init_audio(void) {
  int i;
  memset(mix_ch, 0, sizeof(mix_ch));
  memset(&opl_music, 0, sizeof(opl_music));
  sfx_sema = sceKernelCreateSema("sfx_sema", 0, 1, 1, NULL);
  mus_sema = sceKernelCreateSema("mus_sema", 0, 1, 1, NULL);
  opl_music.tick_samples = OUTPUT_RATE / TICRATE;
  opl_music.tick_counter = opl_music.tick_samples;
  opl_music.music_volume = 10;
  for (i = 0; i < 16; i++) {
    opl_music.channels[i].volume = 100;
    opl_music.channels[i].pitch_bend = 64;
  }
  OPL3_Reset(&opl_music.chip, OUTPUT_RATE);
  /* Enable OPL3 mode */
  OPL3_WriteReg(&opl_music.chip, 0x105, 0x01);
  for (i = 0; i < 9; i++)
    opl_reg_b0[i] = 0;
  sfx_thread_id = sceKernelCreateThread("audio_thread", audio_thread, 0x12,
                                        64 * 1024, PSP_THREAD_ATTR_USER, NULL);
  if (sfx_thread_id >= 0)
    sceKernelStartThread(sfx_thread_id, 0, NULL);
  audio_ready = 1;
  debug_logf("init_audio: sema=%d thread=%d", sfx_sema, sfx_thread_id);
}

/* ---- I_Sound interface ---- */
int I_GetSfxLumpNum(sfxinfo_t *sfx) {
  char namebuf[9];
  snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
  return W_CheckNumForName(namebuf);
}
void I_InitSound(boolean use_sfx_prefix) {
  (void)use_sfx_prefix;
  debug_log("I_InitSound");
  load_genmidi();
  debug_logf("I_InitSound: genmidi_loaded=%d", opl_music.genmidi_loaded);
}
void I_ShutdownSound(void) { sfx_running = 0; }
void I_UpdateSoundParams(int handle, int vol, int sep) {
  int i;
  if (sfx_sema >= 0)
    sceKernelWaitSema(sfx_sema, 1, NULL);
  for (i = 0; i < MIX_CHANNELS; i++) {
    if (mix_ch[i].active && mix_ch[i].handle == handle) {
      mix_ch[i].vol_left = (vol * (255 - sep)) / 255;
      mix_ch[i].vol_right = (vol * sep) / 255;
      break;
    }
  }
  if (sfx_sema >= 0)
    sceKernelSignalSema(sfx_sema, 1);
}
int I_StartSound(sfxinfo_t *sfx, int cnum, int vol, int sep) {
  int lumpnum, i, best;
  sfx_cache_entry_t *e;
  (void)cnum;
  if (!audio_ready)
    return -1;
  lumpnum = I_GetSfxLumpNum(sfx);
  if (lumpnum < 0)
    return -1;
  e = sfx_cache_get(lumpnum);
  if (!e)
    return -1;
  if (sfx_sema >= 0)
    sceKernelWaitSema(sfx_sema, 1, NULL);
  best = -1;
  for (i = 0; i < MIX_CHANNELS; i++)
    if (!mix_ch[i].active) {
      best = i;
      break;
    }
  if (best < 0)
    best = 0;
  {
    int rate = e->samplerate;
    if (rate < 1)
      rate = 11025;
    int step = (int)((float)rate / OUTPUT_RATE * 65536.0f);
    int handle = next_handle++;
    if (next_handle <= 0)
      next_handle = 1;
    mix_ch[best].data = e->samples;
    mix_ch[best].length = e->length;
    mix_ch[best].pos_fixed = (16 << 16); /* skip header */
    mix_ch[best].step_fixed = step;
    mix_ch[best].vol_left = (vol * (255 - sep)) / 255;
    mix_ch[best].vol_right = (vol * sep) / 255;
    mix_ch[best].handle = handle;
    mix_ch[best].active = 1;
    mix_ch[best].lumpnum = lumpnum;
    if (sfx_sema >= 0)
      sceKernelSignalSema(sfx_sema, 1);
    return handle;
  }
}
void I_StopSound(int handle) {
  int i;
  if (sfx_sema >= 0)
    sceKernelWaitSema(sfx_sema, 1, NULL);
  for (i = 0; i < MIX_CHANNELS; i++)
    if (mix_ch[i].active && mix_ch[i].handle == handle) {
      mix_ch[i].active = 0;
      break;
    }
  if (sfx_sema >= 0)
    sceKernelSignalSema(sfx_sema, 1);
}
boolean I_SoundIsPlaying(int handle) {
  int i;
  boolean r = false;
  if (sfx_sema >= 0)
    sceKernelWaitSema(sfx_sema, 1, NULL);
  for (i = 0; i < MIX_CHANNELS; i++)
    if (mix_ch[i].active && mix_ch[i].handle == handle) {
      r = true;
      break;
    }
  if (sfx_sema >= 0)
    sceKernelSignalSema(sfx_sema, 1);
  return r;
}
void I_UpdateSound(void) {}
void I_SubmitSound(void) {}
void I_SetChannels(void) {}
void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) {
  (void)sounds;
  (void)num_sounds;
}

/* ---- Music interface ---- */
void I_InitMusic(void) {
  debug_log("I_InitMusic");
  load_genmidi();
  debug_logf("I_InitMusic: genmidi_loaded=%d", opl_music.genmidi_loaded);
}
void I_ShutdownMusic(void) {
  int i;
  if (mus_sema >= 0) {
    sceKernelWaitSema(mus_sema, 1, NULL);
    opl_music.playing = 0;
    for (i = 0; i < OPL_NUM_VOICES; i++) {
      opl_silence_voice(i);
      opl_music.voices[i].active = 0;
    }
    sceKernelSignalSema(mus_sema, 1);
  }
}
void I_SetMusicVolume(int v) {
  if (mus_sema >= 0) {
    sceKernelWaitSema(mus_sema, 1, NULL);
    opl_music.music_volume = (v * 15) / 127;
    if (opl_music.music_volume < 1 && v > 0)
      opl_music.music_volume = 1;
    if (opl_music.music_volume > 15)
      opl_music.music_volume = 15;
    sceKernelSignalSema(mus_sema, 1);
  }
}
void I_PauseSong(void) {
  if (mus_sema >= 0) {
    sceKernelWaitSema(mus_sema, 1, NULL);
    opl_music.playing = 0;
    sceKernelSignalSema(mus_sema, 1);
  }
}
void I_ResumeSong(void) {
  if (mus_sema >= 0) {
    sceKernelWaitSema(mus_sema, 1, NULL);
    if (opl_music.mus_data)
      opl_music.playing = 1;
    sceKernelSignalSema(mus_sema, 1);
  }
}
void I_StopSong(void) {
  int i;
  if (mus_sema >= 0) {
    sceKernelWaitSema(mus_sema, 1, NULL);
    opl_music.playing = 0;
    for (i = 0; i < OPL_NUM_VOICES; i++) {
      opl_silence_voice(i);
      opl_music.voices[i].active = 0;
    }
    sceKernelSignalSema(mus_sema, 1);
  }
}
boolean I_MusicIsPlaying(void) {
  boolean r;
  if (mus_sema >= 0) {
    sceKernelWaitSema(mus_sema, 1, NULL);
    r = opl_music.playing ? true : false;
    sceKernelSignalSema(mus_sema, 1);
  } else
    r = opl_music.playing ? true : false;
  return r;
}
void *I_RegisterSong(void *data, int len) {
  byte *d = (byte *)data, *md;
  int so, sl, i;
  debug_logf("I_RegisterSong: data=%p len=%d", data, len);
  if (!data || len < 16)
    return NULL;
  if (d[0] != 'M' || d[1] != 'U' || d[2] != 'S' || d[3] != 0x1A) {
    debug_log("not MUS");
    return (void *)1;
  }
  sl = d[4] | (d[5] << 8);
  so = d[6] | (d[7] << 8);
  if (so >= len || so < 12)
    return (void *)1;
  if (sl <= 0 || so + sl > len)
    sl = len - so;
  md = (byte *)malloc(len);
  if (!md)
    return (void *)1;
  memcpy(md, data, len);
  if (mus_sema >= 0)
    sceKernelWaitSema(mus_sema, 1, NULL);
  if (opl_music.mus_data) {
    free((void *)opl_music.mus_data);
    opl_music.mus_data = NULL;
  }
  opl_music.playing = 0;
  for (i = 0; i < OPL_NUM_VOICES; i++) {
    opl_silence_voice(i);
    opl_music.voices[i].active = 0;
  }
  opl_music.mus_data = md;
  opl_music.mus_len = len;
  opl_music.score_start = so;
  opl_music.score_len = sl;
  opl_music.mus_pos = so;
  opl_music.delay_left = 0;
  opl_music.tick_counter = opl_music.tick_samples;
  opl_music.voice_age = 0;
  for (i = 0; i < 16; i++) {
    opl_music.channels[i].volume = 100;
    opl_music.channels[i].patch = 0;
    opl_music.channels[i].pitch_bend = 64;
  }
  if (mus_sema >= 0)
    sceKernelSignalSema(mus_sema, 1);
  return (void *)md;
}
void I_UnRegisterSong(void *handle) {
  int i;
  if (!handle || handle == (void *)1)
    return;
  if (mus_sema >= 0)
    sceKernelWaitSema(mus_sema, 1, NULL);
  opl_music.playing = 0;
  for (i = 0; i < OPL_NUM_VOICES; i++) {
    opl_silence_voice(i);
    opl_music.voices[i].active = 0;
  }
  if (opl_music.mus_data == (const byte *)handle) {
    opl_music.mus_data = NULL;
    opl_music.mus_len = 0;
  }
  if (mus_sema >= 0)
    sceKernelSignalSema(mus_sema, 1);
  free(handle);
}
void I_PlaySong(void *handle, boolean looping) {
  int i;
  debug_logf("I_PlaySong: handle=%p looping=%d", handle, looping);
  if (!handle || handle == (void *)1)
    return;
  if (mus_sema >= 0)
    sceKernelWaitSema(mus_sema, 1, NULL);
  if (opl_music.mus_data == (const byte *)handle) {
    opl_music.mus_pos = opl_music.score_start;
    opl_music.delay_left = 0;
    opl_music.looping = looping ? 1 : 0;
    opl_music.tick_counter = opl_music.tick_samples;
    for (i = 0; i < OPL_NUM_VOICES; i++) {
      opl_silence_voice(i);
      opl_music.voices[i].active = 0;
    }
    opl_music.playing = 1;
  }
  if (mus_sema >= 0)
    sceKernelSignalSema(mus_sema, 1);
}

/* ---- CD/ENDOOM stubs ---- */
int I_CDMusInit(void) { return 0; }
void I_CDMusShutdown(void) {}
void I_CDMusUpdate(void) {}
void I_CDMusStop(void) {}
int I_CDMusPlay(int t) {
  (void)t;
  return 0;
}
void I_CDMusSetVolume(int v) { (void)v; }
int I_CDMusFirstTrack(void) { return 0; }
int I_CDMusLastTrack(void) { return 0; }
int I_CDMusTrackLength(int t) {
  (void)t;
  return 0;
}
void I_Endoom(byte *d) { (void)d; }
char *gus_patch_path = "";
int gus_ram_kb = 0;

/* ---- Exit callback ---- */
static int exit_callback(int arg1, int arg2, void *common) {
  (void)arg1;
  (void)arg2;
  (void)common;
  sfx_running = 0;
  sceKernelExitGame();
  return 0;
}
static int callback_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  int cbid = sceKernelCreateCallback("exit_cb", exit_callback, NULL);
  sceKernelRegisterExitCallback(cbid);
  sceKernelSleepThreadCB();
  return 0;
}

/* ---- MAIN ---- */
int main(int argc, char **argv) {
  int i;
  const char *iwad = NULL, *pwad = NULL;
  const char *iwad_paths[] = {DATA_PATH "doom2.wad", DATA_PATH "DOOM2.WAD",
                              NULL};
  const char *pwad_paths[] = {DATA_PATH "batman.wad", DATA_PATH "BATMAN.WAD",
                              NULL};
  (void)argc;
  (void)argv;

  /* Setup exit callback */
  SceUID cb_thid =
      sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
  if (cb_thid >= 0)
    sceKernelStartThread(cb_thid, 0, NULL);

  /* Overclock PSP to 333MHz for smooth gameplay */
  scePowerSetClockFrequency(333, 333, 166);

  /* Create data directory */
  mkdir(DATA_PATH, 0777);

  /* Remove old log */
  remove(DATA_PATH "debug.log");
  debug_log("=== Batman Doom PSP v1.0 ===");

  debug_log("Step 1: init_display");
  init_display();
  if (!display_ready) {
    debug_log("FATAL: no display");
    sceKernelDelayThread(3000000);
    sceKernelExitGame();
    return 1;
  }
  debug_log("Step 2: display OK");
  base_time = get_ms();

  debug_log("Step 3: init_audio");
  init_audio();

  /* Set save directory */
  savegamedir = strdup(DATA_PATH);
  debug_logf("savegamedir: %s", savegamedir);

  debug_log("Step 4: searching WADs");
  for (i = 0; iwad_paths[i]; i++) {
    FILE *f = fopen(iwad_paths[i], "rb");
    if (f) {
      fclose(f);
      iwad = iwad_paths[i];
      break;
    }
  }
  for (i = 0; pwad_paths[i]; i++) {
    FILE *f = fopen(pwad_paths[i], "rb");
    if (f) {
      fclose(f);
      pwad = pwad_paths[i];
      break;
    }
  }
  if (!iwad) {
    debug_log("FATAL: no doom2.wad!");
    sceKernelDelayThread(5000000);
    sceKernelExitGame();
    return 1;
  }
  debug_logf("IWAD: %s", iwad);
  if (pwad)
    debug_logf("PWAD: %s", pwad);
  else
    debug_log("WARNING: no batman.wad, running vanilla Doom 2");

  base_time = get_ms();
  debug_log("Step 5: doomgeneric_Create");
  if (pwad) {
    char *nargv[] = {"BatmanDoom", "-iwad",      (char *)iwad,
                     "-file",      (char *)pwad, NULL};
    doomgeneric_Create(5, nargv);
  } else {
    char *nargv[] = {"BatmanDoom", "-iwad", (char *)iwad, NULL};
    doomgeneric_Create(3, nargv);
  }

  savegamedir = strdup(DATA_PATH);
  mkdir(DATA_PATH, 0777);
  debug_logf("savegamedir (post-init): %s", savegamedir);

  debug_log("Entering main loop");
  while (1) {
    doomgeneric_Tick();
  }
  return 0;
}

/* =========================================================
 * Missing platform functions required by doomgeneric
 * ========================================================= */

#include <stdarg.h>

/* Fatal error handler — log to file then exit */
void I_Error(const char *error, ...) {
  va_list argptr;
  FILE *f = fopen(DATA_PATH "error.log", "a");
  if (f) {
    va_start(argptr, error);
    vfprintf(f, error, argptr);
    va_end(argptr);
    fprintf(f, "\n");
    fclose(f);
  }
  /* Also print to stderr for debug builds */
  va_start(argptr, error);
  vfprintf(stderr, error, argptr);
  va_end(argptr);
  fprintf(stderr, "\n");

  sceKernelExitGame();
  /* Unreachable */
  for (;;)
    sceKernelDelayThread(100000);
}

/* Palette: the engine calls I_SetPalette with a raw RGB palette.
 * We convert it to ABGR for PSP GU_PSM_8888 and store in cmap[]. */
void I_SetPalette(unsigned char *palette) {
  int i;
  if (!palette)
    return;
  for (i = 0; i < 256; i++) {
    unsigned char r = palette[i * 3 + 0];
    unsigned char g = palette[i * 3 + 1];
    unsigned char b = palette[i * 3 + 2];
    /* PSP GU_PSM_8888 = ABGR in memory (R in low byte) */
    cmap[i] = 0xFF000000 | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
  }
}

int I_GetPaletteIndex(int r, int g, int b) {
  int best = 0, best_diff = 0x7FFFFFFF, i;
  for (i = 0; i < 256; i++) {
    int cr = (cmap[i]) & 0xFF;
    int cg = (cmap[i] >> 8) & 0xFF;
    int cb = (cmap[i] >> 16) & 0xFF;
    int diff = (r - cr) * (r - cr) + (g - cg) * (g - cg) + (b - cb) * (b - cb);
    if (diff < best_diff) {
      best_diff = diff;
      best = i;
    }
    if (diff == 0)
      break;
  }
  return best;
}

/* Disk read activity indicators — no-op on PSP (no HDD LED) */
void I_BeginRead(void) {}
void I_EndRead(void) {}

/* Memory zone base — use malloc from PSP heap (set by PSP_HEAP_SIZE_KB).
 * sceKernelAllocPartitionMemory requires kernel mode — use malloc instead. */
#define ZONE_SIZE_MAIN (12 * 1024 * 1024) /* 12 MB zone */
#define ZONE_SIZE_FALL (8 * 1024 * 1024)  /*  8 MB fallback */

static void *zone_ptr = NULL;
static int zone_actual_size = 0;

void *I_ZoneBase(int *size) {
  if (!zone_ptr) {
    zone_ptr = malloc(ZONE_SIZE_MAIN);
    if (zone_ptr) {
      zone_actual_size = ZONE_SIZE_MAIN;
    } else {
      zone_ptr = malloc(ZONE_SIZE_FALL);
      if (zone_ptr) {
        zone_actual_size = ZONE_SIZE_FALL;
      } else {
        debug_log("FATAL: cannot allocate zone memory");
        sceKernelExitGame();
      }
    }
    debug_logf("I_ZoneBase: allocated %d bytes", zone_actual_size);
  }
  *size = zone_actual_size;
  return zone_ptr;
}

/* =========================================================
 * Additional platform stubs for newer doomgeneric API
 * ========================================================= */

/* --- Timer --- */
/* I_GetTime: doom tics at 35Hz */
int I_GetTime(void) {
  return (int)(sceKernelGetSystemTimeWide() * 35 / 1000000ULL);
}

/* I_GetTimeMS: milliseconds */
int I_GetTimeMS(void) { return (int)(sceKernelGetSystemTimeWide() / 1000ULL); }

void I_Sleep(int ms) { sceKernelDelayThread(ms * 1000); }

void I_InitTimer(void) {}

/* --- Display extras --- */
void I_UpdateNoBlit(void) {}
void I_StartFrame(void) {}
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_GraphicsCheckCommandLine(void) {}

void I_SetGrabMouseCallback(void *cb) { (void)cb; }
void I_InitGraphics(void) {} /* PSP init done in DG_Init */

void I_ReadScreen(unsigned char *scr) {
  /* Copy the 8-bit indexed framebuffer (I_VideoBuffer) */
  if (scr && I_VideoBuffer)
    memcpy(scr, I_VideoBuffer, DOOMGENERIC_RESX * DOOMGENERIC_RESY);
}

/* --- Input extras --- */
void I_InitJoystick(void) {}
void I_BindJoystickVariables(void) {}

/* --- Sound extras --- */
void I_BindSoundVariables(void) {}

/* --- Misc --- */
void I_Quit(void) {
  sceKernelExitGame();
  for (;;)
    sceKernelDelayThread(100000);
}

void I_WaitVBL(int count) {
  sceDisplayWaitVblankStart();
  (void)count;
}

void I_Tactile(int on, int off, int total) {
  (void)on;
  (void)off;
  (void)total;
}

/* at-exit handler list */
#define MAX_ATEXIT 32
static void (*atexit_funcs[MAX_ATEXIT])(void);
static int atexit_count = 0;
void I_AtExit(void (*func)(void), int run_on_error) {
  (void)run_on_error;
  if (atexit_count < MAX_ATEXIT)
    atexit_funcs[atexit_count++] = func;
}

void I_PrintBanner(const char *s) { printf("%s\n", s); }
void I_PrintDivider(void) { printf("----\n"); }
void I_PrintStartupBanner(const char *s) { printf("%s\n", s); }
void I_CheckIsScreensaver(void) {}
void I_DisplayFPSDots(int b) { (void)b; }

int I_ConsoleStdout(void) { return 1; }

/* Memory probe for Vanilla compatibility checks */
unsigned int I_GetMemoryValue(unsigned int offs, int size) {
  (void)offs;
  (void)size;
  return 0;
}

/* I_StartTic: collect input events each tic (PSP input polled in DG_GetKey) */
void I_StartTic(void) {
  /* PSP input is polled in DG_GetKey; nothing needed here */
}

/* I_FinishUpdate: blit screen — delegate to DG_DrawFrame which does the GU blit
 */
void I_FinishUpdate(void) { DG_DrawFrame(); }
