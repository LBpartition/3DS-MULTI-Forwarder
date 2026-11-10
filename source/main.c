/*
 * MULTI-Forwarder: creates HOME Menu forwarders for 3DSX, GBA, SNES and NDS.
 *
 * The top screen previews the selected title before its forwarder is installed.
 *
 * Name: the app's file name. Icon: a PNG or JPEG next to the app with
 * the same name (Golf.gba -> Golf.png or Golf.jpg, any size), else a
 * cartridge with the game's name on it. Top screen banner: Golf.banner.png
 * or .jpg (best 256x128), else the icon, bigger, with the name. A picture of
 * another shape is padded to fit. The top screen shows both before anything
 * is made.
 *
 * The Installed tab lists and deletes the forwarders made by this app.
 *
 * The look (and ui.c, ui_font.c) is gpSP 3D's.
 */
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <malloc.h>
#include <zlib.h>
#include "fwdcia.h"
#include "deficon.h"
#include "ui.h"
#include "pngshrink.h"
#include "jpegshrink.h"
#include "boxart.h"
#include "netget.h"
#include "cia_patch.h"
#include "banner.h"
#include "wav.h"
#include "sha256.h"
#include "nds_forwarder_bridge.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"

#define ICONMAKER_VERSION "0.4.0"
#define MAX_ENTRIES 1024
#define MAX_ICONS   300
#define FORWARDER_UID_START 0xC0000u
#define FORWARDER_UID_END   0xFF000u
#define FORWARDER_UID_COUNT (FORWARDER_UID_END - FORWARDER_UID_START)
#define OLD_APP_DIR "sdmc:/3ds/3DSX-Forwarder"
#define APP_DIR     "sdmc:/3ds/MULTI-Forwarder"
#define LAST_DIR    APP_DIR "/iconmaker_dir.txt"
#define SETTINGS    APP_DIR "/iconmaker.cfg"
#define TITLE_IDS   APP_DIR "/title_ids.tsv"
#define CIA_TEMPLATE "romfs:/template.cia"
#define HBLAUNCHER_TITLE_ID 0x000400000D921E00ULL

/* The banner a game gets: on gpSP 3D's blue, on nothing (the HOME Menu's
 * own background shows behind the icon and the name), or 3D: the icon on a
 * tile that comes out of the screen and sways, over the blue, with a jingle
 * (SELECT on the Games tab switches; saved in iconmaker.cfg) */
enum { STYLE_BLUE, STYLE_CLEAR, STYLE_3D, STYLE_COUNT };
static const char *style_key[STYLE_COUNT] = { "blue", "none", "3d" };
static int banner_style = STYLE_3D;
static int banner_cycle;
static bool nds_custom_icon;
static bool nds_icon_default_pending = true;

/* A picture of another shape than the icon, banner or tile: shrunk to fit
 * with margins (white on the icon, as the HOME Menu draws see-through
 * parts; the tile's grey on the 3D banner), or
 * scaled to fill it with the sides cut off (X on the Games tab; saved too) */
#define BOXART_SOURCES 8
static char boxart_dir[256];
static char boxart_src[BOXART_SOURCES][256];
static int nboxart_src;
#define BOXART_SIDE      128
#define BOXART_LIST      APP_DIR "/boxart_%s.txt"
#define BOXART_LIB_LIST  APP_DIR "/boxart_%08lX_%s.txt"

/* pic[0], pic[1]: the game's icon and banner picture, as an index into
 * pic_types, or -1 if there's none (found when the folder is read) */
typedef struct { char name[256]; bool dir, has_icon; int sys; s8 pic[2]; } entry;
static const char *const pic_types[] = { "png", "jpg", "jpeg" };
/* Pictures too big to use (an icon from the name instead, and a note): a
 * side over PIC_MAX_SIDE; a progressive JPEG whose coefficients would take
 * over PIC_MAX_COEF bytes; or, where a picture must be read and decoded
 * all at once (interlaced PNGs, JPEGs libjpeg can't read), a file over
 * PIC_MAX_FILE or over PIC_MAX_FULL bytes decoded */
#define PIC_MAX_SIDE  4096
#define PIC_MAX_COEF  (16 << 20)
#define PIC_MAX_FILE  (16 << 20)
#define PIC_MAX_FULL  (8 << 20)
typedef struct
{
   u64 tid;
   char name[64], publisher[64];
   u16 icon48[48 * 48];
   int sys;
} icon_entry;

/* the emulators an icon can start (the publisher on an icon names it) */
typedef struct
{
   const char *name, *console;
   u64 tid;                    /* its CIA's title ID */
   const char *launch;         /* the file it reads the game path from ("": gpSP 3D's) */
   int style;                  /* default icon: which cartridge */
   u32 tag_rgb;                /* its name's colour on the banner */
} emulator;
enum { SYS_3DSX, SYS_GBA, SYS_SNES, SYS_COUNT, SYS_NDS = SYS_COUNT };
static const emulator systems[SYS_COUNT] = {
   { "3DSX", "3DSX", HBLAUNCHER_TITLE_ID, "", ICON_GBA, 0x23D5C8 },
   { "gpSP 3D", "GBA", 0x000400000BAC8100ULL, "", ICON_GBA, 0x23D5C8 },
   { "Snes9x 3D", "SNES", 0x00040000038A7100ULL, "sdmc:/3ds/snes9x_3ds/launch.txt",
     ICON_SNES, 0xB59CFF },
};
static bool sys_installed[SYS_COUNT];

static entry entries[MAX_ENTRIES];
static int nentries, ngames;
static icon_entry icons[MAX_ICONS];
static int nicons;
static bool installed_icons_dirty;
static int installed_icon_indices[MAX_ICONS];
static int ninstalled_icons;
#define MAX_TWL_ICONS 300
typedef struct
{
   u64 tid;
   char name[64];
   FS_MediaType media;
   u16 icon32[32 * 32];
   u8 icon32_transparent[32 * 32];
   bool has_icon, is_nds_forwarder;
} twl_icon_entry;
static twl_icon_entry twl_icons[MAX_TWL_ICONS];
static int ntwl_icons;
static bool twl_titles_dirty;
static u32 twl_installed_count = 0xFFFFFFFF;
static char cwd[512] = "sdmc:/";
typedef struct { char path[256]; u64 tid; } title_record;
static title_record title_records[MAX_ENTRIES];
static int ntitle_records;
static u64 occupied_ids[4096];
static int noccupied_ids;

/* the preview being made: when it started, and the spinner shown over the
 * top screen while a slow one is made (see preview_stop) */
static u64 preview_t0, spinner_last, spinner_ticks;

/* ------------------------------------------------------------------ */
/* timing (make TIMING=1): how long each step of the preview takes,    */
/* shown on the top screen and added to iconmaker_timing.log           */
/* ------------------------------------------------------------------ */

#ifdef ICONMAKER_TIMING
#define TIMING_LOG APP_DIR "/iconmaker_timing.log"
#define TM_MAX 20
static struct { const char *what; u32 us; int n; } tm[TM_MAX];
static int ntm;
static u64 tm_t0;
static char tm_pic[2][64];          /* the icon's and banner's file: size, bytes */
static u32 tm_dir_us, tm_dir_n, tm_bottom_us;

static u32 tm_us(u64 ticks)
{
   return (u32)(ticks / (SYSCLOCK_ARM11 / 1000000));
}

static void tm_start(void)
{
   ntm = 0;
   tm_pic[0][0] = tm_pic[1][0] = 0;
   tm_t0 = svcGetSystemTick();
}

/* the time since the last mark goes to this step (added up if it repeats) */
static void tm_mark(const char *what)
{
   u32 us = tm_us(svcGetSystemTick() - tm_t0);
   int i;
   for (i = 0; i < ntm && tm[i].what != what; i++)
      ;
   if (i < TM_MAX)
   {
      if (i == ntm)
      {
         tm[ntm].what = what;
         tm[ntm].us = tm[ntm].n = 0;
         ntm++;
      }
      tm[i].us += us;
      tm[i].n++;
   }
   tm_t0 = svcGetSystemTick();
}

static void tm_log(const char *line);

/* a session's first lines: when, and on which console */
static void tm_log_start(void)
{
   char line[160];
   time_t now = time(NULL);
   bool n3ds = false;
   APT_CheckNew3DS(&n3ds);
   strftime(line, 40, "\n=== %Y-%m-%d %H:%M:%S", localtime(&now));
   snprintf(line + strlen(line), sizeof(line) - strlen(line),
            "  3DSX Forwarder " ICONMAKER_VERSION ", %s\n", n3ds ? "New 3DS" : "Old 3DS");
   tm_log(line);
}

/* a part of the step just marked, measured inside it (not added to the total) */
static void tm_note(const char *what, u64 ticks)
{
   if (ntm < TM_MAX)
   {
      tm[ntm].what = what;
      tm[ntm].us = tm_us(ticks);
      tm[ntm].n = -1;
      ntm++;
   }
}

static void tm_log(const char *line)
{
   FILE *f;
   mkdir("sdmc:/3ds", 0777);
   mkdir(APP_DIR, 0777);
   if ((f = fopen(TIMING_LOG, "a")))
   {
      fputs(line, f);
      fclose(f);
   }
}
#else
#define tm_start()  ((void)0)
#define tm_mark(w)  ((void)(w))
#endif

/* ------------------------------------------------------------------ */
/* theme (gpSP 3D's)                                                   */
/* ------------------------------------------------------------------ */

#define C_BG      UI_RGB(0x0E, 0x11, 0x17)
#define C_BG2     UI_RGB(0x16, 0x1A, 0x23)
#define C_PANEL   UI_RGB(0x1B, 0x20, 0x2A)
#define C_ROW     UI_RGB(0x20, 0x26, 0x31)
#define C_ROW_SEL UI_RGB(0x2A, 0x33, 0x45)
#define C_ACCENT  UI_RGB(0x4E, 0x8D, 0xFF)
#define C_ACCENT2 UI_RGB(0x8F, 0x6B, 0xFF)
#define C_ORANGE  UI_RGB(0xFF, 0xAA, 0x28)
#define C_TEXT    UI_RGB(0xEC, 0xEF, 0xF4)
#define C_TEXT2   UI_RGB(0xC5, 0xCB, 0xD6)
#define C_DIM     UI_RGB(0x8B, 0x94, 0xA7)
#define C_FAINT   UI_RGB(0x4A, 0x52, 0x63)
#define C_TRACK   UI_RGB(0x37, 0x3F, 0x50)
#define C_GOOD    UI_RGB(0x5F, 0xD3, 0x93)
#define C_BAD     UI_RGB(0xFF, 0x6E, 0x6E)
#define C_WARN    UI_RGB(0xFF, 0xC1, 0x5C)

#define ROW_X     6
#define ROW_W     (UI_W - 12)
#define TAB_Y     30
#define TAB_H     26
#define LIST_Y    76
#define ROW_H     23
#define ROWS      6
#define FOOT_Y    216

enum { TAB_GAMES, TAB_ICONS, TAB_TWL };
enum { FILTER_ALL, FILTER_3DSX, FILTER_GBA, FILTER_SNES, FILTER_COUNT };
static int installed_filter = FILTER_ALL;
static const char *tab_names[3] = { "Make Forwarder", "Installed", "TWL" };

/* SMDH icon (RGB565, 8x8 tiles, Z-order) to a plain RGB565 picture */
static void smdh_icon_decode(const u8 *src, int size, u16 *out)
{
   int x, y, i = 0;
   for (y = 0; y < size; y += 8)
      for (x = 0; x < size; x += 8)
      {
         int k;
         for (k = 0; k < 64; k++, i++)
         {
            int px = 0, py = 0, b;
            for (b = 0; b < 3; b++)
            {
               px |= ((k >> (2 * b)) & 1) << b;
               py |= ((k >> (2 * b + 1)) & 1) << b;
            }

            out[(y + py) * size + x + px] = src[2 * i] | src[2 * i + 1] << 8;
         }
      }
}

static void wrap_text_two_lines(const ui_font *font, const char *text, int max_w,
                                char *first, size_t first_size,
                                char *second, size_t second_size)
{
   const char *p, *break_at = NULL, *last_fit = text;
   char line[128];
   size_t len;

   snprintf(first, first_size, "%s", text);
   second[0] = 0;
   if (ui_text_w(font, text) <= max_w)
      return;

   for (p = text; *p; p++)
   {
      if (*p == ' ' || *p == '\n')
      {
         len = (size_t)(p - text);
         while (len && (text[len - 1] == ' ' || text[len - 1] == '\n'))
            len--;
         if (len < sizeof(line))
         {
            memcpy(line, text, len);
            line[len] = 0;
            if (ui_text_w(font, line) <= max_w)
               break_at = p;
         }
      }
      else
      {
         const char *next = p;
         ui_utf8_next(&next);
         if (next > p + 1)
            p = next - 1;
      }
   }

   if (break_at)
   {
      len = (size_t)(break_at - text);
      while (len && (text[len - 1] == ' ' || text[len - 1] == '\n'))
         len--;
      if (len >= first_size) len = first_size - 1;
      memcpy(first, text, len);
      first[len] = 0;
      while (*break_at == ' ' || *break_at == '\n') break_at++;
      snprintf(second, second_size, "%s", break_at);
      return;
   }

   for (p = text; *p; )
   {
      const char *next = p;
      char candidate[128];
      size_t n;
      ui_utf8_next(&next);
      n = (size_t)(next - text);
      if (n >= sizeof(candidate)) break;
      memcpy(candidate, text, n);
      candidate[n] = 0;
      if (ui_text_w(font, candidate) > max_w)
         break;
      last_fit = next;
      p = next;
   }
   len = (size_t)(last_fit - text);
   if (len >= first_size) len = first_size - 1;
   memcpy(first, text, len);
   first[len] = 0;
   while (*last_fit == ' ' || *last_fit == '\n') last_fit++;
   snprintf(second, second_size, "%s", last_fit);
}

static u32 read_u32le(const u8 *p)
{
      return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u16 read_u16le(const u8 *p)
{
      return p[0] | (u16)p[1] << 8;
}

static bool read_app_smdh(const char *path, u8 *smdh)
{
      u8 header[0x2C];
      char sidecar[800];
      const char *dot = strrchr(path, '.');
      FILE *f = fopen(path, "rb");
      long file_size;
      u32 offset, size;
      bool ok = false;

      if (!f)
         return false;
      if (fseek(f, 0, SEEK_END) || (file_size = ftell(f)) < (long)sizeof(header) ||
          fseek(f, 0, SEEK_SET) || fread(header, 1, sizeof(header), f) != sizeof(header))
         goto done;
      if (!memcmp(header, "3DSX", 4))
      {
         offset = read_u32le(header + 0x20);
         size = read_u32le(header + 0x24);
         if (read_u16le(header + 0x04) >= sizeof(header) &&
             size >= FWDCIA_SMDH_SIZE && offset <= (u32)file_size &&
             FWDCIA_SMDH_SIZE <= (u32)file_size - offset &&
             !fseek(f, offset, SEEK_SET) &&
             fread(smdh, 1, FWDCIA_SMDH_SIZE, f) == FWDCIA_SMDH_SIZE &&
             !memcmp(smdh, "SMDH", 4))
         {
            ok = true;
            goto done;
         }
      }
      fclose(f);
      f = NULL;
      if (!dot || (size_t)(dot - path) + sizeof(".smdh") > sizeof(sidecar))
         return false;
      snprintf(sidecar, sizeof(sidecar), "%.*s.smdh", (int)(dot - path), path);
      f = fopen(sidecar, "rb");
      if (f && fread(smdh, 1, FWDCIA_SMDH_SIZE, f) == FWDCIA_SMDH_SIZE &&
          !memcmp(smdh, "SMDH", 4))
         ok = true;
done:
      if (f)
         fclose(f);
      return ok;
}

static void smdh_icon_rgba(const u8 *smdh, u8 *rgba)
{
      static u16 rgb[48 * 48];
      int i;
      smdh_icon_decode(smdh + 0x24C0, 48, rgb);
      for (i = 0; i < 48 * 48; i++)
      {
         u16 c = rgb[i];
         rgba[i * 4] = (u8)(((c >> 11) & 31) * 255 / 31);
         rgba[i * 4 + 1] = (u8)(((c >> 5) & 63) * 255 / 63);
         rgba[i * 4 + 2] = (u8)((c & 31) * 255 / 31);
         rgba[i * 4 + 3] = 255;
      }
}

/* ------------------------------------------------------------------ */
/* the icons on the HOME Menu (forwarders to gpSP 3D)                  */
/* ------------------------------------------------------------------ */

/* a title's SMDH, read from its ExeFS icon */
static bool read_title_smdh_media(u64 tid, u8 *smdh, u32 media_type)
{
   static const u32 file_path[5] = { 0, 0, 2, 0x6E6F6369 /* "icon" */, 0 };
   u32 arch_path[4] = { (u32)tid, (u32)(tid >> 32), media_type, 0 };
   FS_Path ap = { PATH_BINARY, sizeof(arch_path), arch_path };
   FS_Path fp = { PATH_BINARY, sizeof(file_path), file_path };
   Handle h;
   u32 got = 0;
   if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0)))
      return false;
   FSFILE_Read(h, &got, 0, smdh, FWDCIA_SMDH_SIZE);
   FSFILE_Close(h);
   return got == FWDCIA_SMDH_SIZE && !memcmp(smdh, "SMDH", 4);
}

static bool read_title_smdh(u64 tid, u8 *smdh)
{
   return read_title_smdh_media(tid, smdh, MEDIATYPE_SD);
}

static void utf16_to_ui(const u8 *in, int max, char *out, int outlen)
{
   int i, k = 0;
   for (i = 0; i < max && k < outlen - 1; i++)
   {
      u32 c = in[2 * i] | (u32)in[2 * i + 1] << 8;
      char encoded[3];
      int n;
      if (!c)
         break;
      if (c == '\r' || c == '\n')
      {
         if (c == '\r' && i + 1 < max &&
             (in[2 * (i + 1)] | (u32)in[2 * (i + 1) + 1] << 8) == '\n')
            i++;
         if (k && out[k - 1] != ' ')
            out[k++] = ' ';
         continue;
      }
      if (c >= 0xD800 && c <= 0xDBFF &&
               i + 1 < max &&
               (in[2 * (i + 1)] | (u32)in[2 * (i + 1) + 1] << 8) >= 0xDC00 &&
               (in[2 * (i + 1)] | (u32)in[2 * (i + 1) + 1] << 8) <= 0xDFFF)
      {
         i++;
         c = '?';
      }
      else if (c >= 0xD800 && c <= 0xDFFF)
         c = '?';
      if (c < 0x80)
      {
         encoded[0] = (char)c;
         n = 1;
      }
      else if (c < 0x800)
      {
         encoded[0] = (char)(0xC0 | (c >> 6));
         encoded[1] = (char)(0x80 | (c & 0x3F));
         n = 2;
      }
      else
      {
         encoded[0] = (char)(0xE0 | (c >> 12));
         encoded[1] = (char)(0x80 | ((c >> 6) & 0x3F));
         encoded[2] = (char)(0x80 | (c & 0x3F));
         n = 3;
      }
      if (k + n >= outlen)
         break;
      memcpy(out + k, encoded, n);
      k += n;
   }
   while (k && out[k - 1] == ' ')
      k--;
   out[k] = 0;
}

static int cmp_icon(const void *a, const void *b)
{
   return strcasecmp(((const icon_entry*)a)->name, ((const icon_entry*)b)->name);
}

static int system_of(const char *path);

static void rebuild_installed_filter(void)
{
   int i;
   ninstalled_icons = 0;
   for (i = 0; i < nicons; i++)
      if (installed_filter == FILTER_ALL || icons[i].sys == installed_filter - 1)
         installed_icon_indices[ninstalled_icons++] = i;
}

static void load_title_records(void)
{
   char line[320];
   FILE *f;
   int i;
   ntitle_records = 0;
   if (!(f = fopen(TITLE_IDS, "r")))
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *tab = strrchr(line, '\t');
      u64 tid;
      if (!tab)
         continue;
      *tab++ = 0;
      if (!*line || strlen(line) >= sizeof(title_records[0].path))
         continue;
      tid = strtoull(tab, NULL, 16);
      if (!tid)
         continue;
      for (i = 0; i < ntitle_records; i++)
         if (!strcmp(title_records[i].path, line))
         {
            title_records[i].tid = tid;
            break;
         }
      if (i < ntitle_records)
         continue;
      if (ntitle_records >= MAX_ENTRIES)
         break;
      snprintf(title_records[ntitle_records].path,
               sizeof(title_records[ntitle_records].path), "%s", line);
      title_records[ntitle_records++].tid = tid;
   }
   fclose(f);
}

static bool title_id_used(u64 tid)
{
   int i;
   for (i = 0; i < ntitle_records; i++)
      if (title_records[i].tid == tid)
         return true;
   for (i = 0; i < noccupied_ids; i++)
      if (occupied_ids[i] == tid)
         return true;
   return false;
}

static bool icon_installed_for_system(u64 tid, int sys)
{
   int i;
   for (i = 0; i < nicons; i++)
      if (icons[i].tid == tid && icons[i].sys == sys)
         return true;
   return false;
}

static bool title_id_recorded(u64 tid)
{
   int i;
   for (i = 0; i < ntitle_records; i++)
      if (title_records[i].tid == tid)
         return true;
   return false;
}

static bool path_is_nds(const char *path)
{
   const char *dot = strrchr(path, '.');
   return dot && (!strcasecmp(dot, ".nds") || !strcasecmp(dot, ".srl"));
}

static bool nds_template_category(u8 *category)
{
   static const char *paths[] = {
      "sdmc:/3ds/MULTI-Forwarder/templates/sdcard.nds",
      "romfs:/templates/sdcard.nds"
   };
   size_t i;
   if (!category)
      return false;
   for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
   {
      FILE *f = fopen(paths[i], "rb");
      if (!f)
         continue;
      if (!fseek(f, 0x234, SEEK_SET) && fread(category, 1, 1, f) == 1)
      {
         fclose(f);
         return true;
      }
      fclose(f);
      return false;
   }
   return false;
}

static u32 read_le32(const u8 *p)
{
   return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 nds_title_id_for_path(const char *path)
{
   u8 code[4], unit_code, category;
   u64 tid = 0x000480ULL;
   FILE *f = fopen(path, "rb");
   int i, start, end, step;
   if (!f)
      return 0;
   if (fseek(f, 0x12, SEEK_SET) || fread(&unit_code, 1, 1, f) != 1 ||
       fseek(f, unit_code ? 0x230 : 0x0C, SEEK_SET) ||
       fread(code, 1, sizeof(code), f) != sizeof(code))
   {
      fclose(f);
      return 0;
   }
   fclose(f);
   if (!nds_template_category(&category))
      return 0;
   tid = (tid << 8) | category;
   start = unit_code ? 3 : 0;
   end = unit_code ? -1 : 4;
   step = unit_code ? -1 : 1;
   for (i = start; i != end; i += step)
      tid = (tid << 8) | code[i];
   return tid;
}

static bool nds_bmp_icon_supported(const char *path)
{
   u8 header[54];
   FILE *f = fopen(path, "rb");
   bool supported;
   if (!f)
      return false;
   supported = fread(header, 1, sizeof(header), f) == sizeof(header) &&
      header[0] == 'B' && header[1] == 'M' &&
      read_le32(header + 14) == 40 &&
      read_le32(header + 18) == 32 && read_le32(header + 22) == 32 &&
      (header[28] | (u16)header[29] << 8) == 4 &&
      read_le32(header + 30) == 0 &&
      read_le32(header + 46) <= 16;
   fclose(f);
   return supported;
}

static bool nds_custom_icon_path(const char *rom_path, char *out, size_t out_size)
{
   const char *dot = strrchr(rom_path, '.');
   struct stat st;
   snprintf(out, out_size, "%.*s.bmp",
            dot ? (int)(dot - rom_path) : (int)strlen(rom_path), rom_path);
   if (nds_bmp_icon_supported(out))
      return true;
   {
      const char *base = strrchr(rom_path, '/');
      char name[256];
      base = base ? base + 1 : rom_path;
      snprintf(name, sizeof(name), "%s", base);
      dot = strrchr(name, '.');
      if (dot) name[dot - name] = 0;
      snprintf(out, out_size, "%s/icons/%s.bmp", APP_DIR, name);
   }
   return !stat(out, &st) && nds_bmp_icon_supported(out);
}

static bool read_nds_custom_icon_bmp(const char *path, twl_icon_entry *icon)
{
   u8 header[54], palette[16][4], pixels[32 * 16];
   u32 palette_count, palette_offset, pixel_offset;
   FILE *f = fopen(path, "rb");
   int x, y;
   bool ok = false;
   if (!f)
      return false;
   if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
       header[0] != 'B' || header[1] != 'M' ||
       read_le32(header + 14) != 40 ||
       read_le32(header + 18) != 32 || read_le32(header + 22) != 32 ||
       (header[28] | (u16)header[29] << 8) != 4 ||
       read_le32(header + 30) != 0)
      goto done;
   palette_count = read_le32(header + 46);
   if (!palette_count) palette_count = 16;
   if (palette_count > 16)
      goto done;
   palette_offset = 14 + read_le32(header + 14);
   pixel_offset = read_le32(header + 10);
   if (palette_offset > pixel_offset ||
       fseek(f, (long)palette_offset, SEEK_SET) ||
       fread(palette, 4, palette_count, f) != palette_count ||
       fseek(f, (long)pixel_offset, SEEK_SET) ||
       fread(pixels, 1, sizeof(pixels), f) != sizeof(pixels))
      goto done;
   for (y = 0; y < 32; y++)
   {
      const u8 *row = pixels + (31 - y) * 16;
      for (x = 0; x < 32; x++)
      {
         u8 packed = row[x / 2];
         u8 index = x & 1 ? packed & 15 : packed >> 4;
         const u8 *colour;
         if (index >= palette_count)
            goto done;
         colour = palette[index];
         icon->icon32[y * 32 + x] = UI_RGB(colour[2], colour[1], colour[0]);
         icon->icon32_transparent[y * 32 + x] = index == 0;
      }
   }
   icon->has_icon = true;
   ok = true;
done:
   fclose(f);
   return ok;
}

static void decode_twl_banner_icon(const u8 *banner, twl_icon_entry *icon)
{
   int x, y;
   utf16_to_ui(banner + 0x340, 0x80, icon->name, sizeof(icon->name));
   if (!icon->name[0])
      utf16_to_ui(banner + 0x240, 0x80, icon->name, sizeof(icon->name));
   for (y = 0; y < 32; y++)
      for (x = 0; x < 32; x++)
      {
         int tile = (y / 8) * 4 + x / 8;
         int pixel = (y % 8) * 8 + x % 8;
         u8 packed = banner[0x20 + tile * 32 + pixel / 2];
         u8 palette_index = pixel & 1 ? packed >> 4 : packed & 15;
         u16 colour = (u16)(banner[0x220 + palette_index * 2] |
                            (u16)banner[0x221 + palette_index * 2] << 8);
         icon->icon32[y * 32 + x] = UI_RGB((colour & 31) * 255 / 31,
                                             ((colour >> 5) & 31) * 255 / 31,
                                             ((colour >> 10) & 31) * 255 / 31);
         icon->icon32_transparent[y * 32 + x] = palette_index == 0;
      }
   icon->has_icon = true;
}

static bool nds_header_title(const u8 *header, char *out, size_t out_size)
{
   size_t length = 0;
   while (length < 12 && length + 1 < out_size &&
          header[length] >= 32 && header[length] < 127)
   {
      out[length] = header[length] == '_' ? ' ' : (char)header[length];
      length++;
   }
   while (length && out[length - 1] == ' ')
      length--;
   out[length] = 0;
   return length > 0;
}

static void draw_twl_icon_scaled(const twl_icon_entry *icon, int x, int y,
                                 int size)
{
   int dx, dy;
   for (dy = 0; dy < size; dy++)
      for (dx = 0; dx < size; dx++)
      {
         int sx = dx * 32 / size, sy = dy * 32 / size;
         int source = sy * 32 + sx;
         if (!icon->icon32_transparent[source])
            ui_canvas[(y + dy) * UI_W + x + dx] = icon->icon32[source];
      }
}

static bool read_installed_twl_banner(u64 tid, FS_MediaType media,
                                      twl_icon_entry *icon)
{
   u8 header[0x3B4], banner[0x23C0];
   bool have_header, have_banner;
   icon->name[0] = 0;
   have_header = R_SUCCEEDED(FSUSER_GetLegacyRomHeader(media, tid, header));
   have_banner = R_SUCCEEDED(FSUSER_GetLegacyBannerData(media, tid, banner));
   if (have_banner)
      decode_twl_banner_icon(banner, icon);
   if (!icon->name[0] || !ui_text_supported(icon->name))
   {
      if (!have_header || !nds_header_title(header, icon->name, sizeof(icon->name)))
         icon->name[0] = 0;
   }
   return have_banner;
}

static bool read_nds_banner_icon(const char *path, twl_icon_entry *icon,
                                 bool use_custom_icon)
{
   u8 header[0x240], banner[0x840], custom_banner[0x840];
   u32 banner_offset;
   FILE *f = fopen(path, "rb");
   const char *base = strrchr(path, '/');
   char basename[256], banner_path[800], icon_path[800], adjacent_path[800];
   struct stat custom_stat;
   const char *dot;
   bool custom_banner_loaded = false;
   if (!f)
      return false;
   if (fread(header, 1, sizeof(header), f) != sizeof(header))
   {
      fclose(f);
      return false;
   }
   banner_offset = read_le32(header + 0x68);
   if (!banner_offset || fseek(f, (long)banner_offset, SEEK_SET) ||
       fread(banner, 1, sizeof(banner), f) != sizeof(banner))
   {
      fclose(f);
      return false;
   }
   fclose(f);
   base = base ? base + 1 : path;
   snprintf(basename, sizeof(basename), "%s", base);
   dot = strrchr(basename, '.');
   if (dot) basename[dot - basename] = 0;
   dot = strrchr(path, '.');
   snprintf(adjacent_path, sizeof(adjacent_path), "%.*s.bin",
            dot ? (int)(dot - path) : (int)strlen(path), path);
   if (!stat(adjacent_path, &custom_stat) && custom_stat.st_size == 0x23C0)
   {
      f = fopen(adjacent_path, "rb");
      if (f)
      {
         if (fread(custom_banner, 1, sizeof(custom_banner), f) == sizeof(custom_banner))
         {
            memcpy(banner, custom_banner, sizeof(banner));
            custom_banner_loaded = true;
         }
         fclose(f);
      }
   }
   snprintf(banner_path, sizeof(banner_path), "%s/banners/%s.bin", APP_DIR, basename);
   if (!custom_banner_loaded && !stat(banner_path, &custom_stat) &&
       custom_stat.st_size == 0x23C0)
   {
      f = fopen(banner_path, "rb");
      if (f)
      {
         if (fread(custom_banner, 1, sizeof(custom_banner), f) == sizeof(custom_banner))
         {
            memcpy(banner, custom_banner, sizeof(banner));
            custom_banner_loaded = true;
         }
         fclose(f);
      }
   }
   decode_twl_banner_icon(banner, icon);
   if (!icon->name[0] || !ui_text_supported(icon->name))
      nds_header_title(header, icon->name, sizeof(icon->name));
   if (!icon->name[0])
      snprintf(icon->name, sizeof(icon->name), "NDS game");
   if (use_custom_icon &&
       nds_custom_icon_path(path, icon_path, sizeof(icon_path)))
      read_nds_custom_icon_bmp(icon_path, icon);
   return true;
}

static bool twl_title_installed(u64 tid)
{
   int i;
   for (i = 0; i < ntwl_icons; i++)
      if (twl_icons[i].tid == tid)
         return true;
   return false;
}

static void add_twl_title(u64 tid, FS_MediaType media)
{
   twl_icon_entry *icon;
   int record;
   static u8 smdh[FWDCIA_SMDH_SIZE];
   if (ntwl_icons >= MAX_TWL_ICONS || twl_title_installed(tid))
      return;
   icon = &twl_icons[ntwl_icons++];
   memset(icon, 0, sizeof(*icon));
   icon->tid = tid;
   icon->media = media;
   read_installed_twl_banner(tid, media, icon);
   for (record = 0; record < ntitle_records; record++)
      if (title_records[record].tid == tid)
      {
         icon->is_nds_forwarder = media == MEDIATYPE_NAND &&
                                  path_is_nds(title_records[record].path) &&
                                  title_id_recorded(tid);
         if (!icon->has_icon)
         {
            const char *base = strrchr(title_records[record].path, '/');
            char *dot;
            base = base ? base + 1 : title_records[record].path;
            snprintf(icon->name, sizeof(icon->name), "%s", base);
            dot = strrchr(icon->name, '.');
            if (dot) *dot = 0;
         }
         if (icon->is_nds_forwarder && !icon->has_icon)
            read_nds_banner_icon(title_records[record].path, icon, true);
         break;
      }
   if (!icon->has_icon && read_title_smdh_media(tid, smdh, media))
   {
      char title[64];
      utf16_to_ui(smdh + 8 + 0x200, 0x40, title, sizeof(title));
      if (!title[0])
         utf16_to_ui(smdh + 8, 0x40, title, sizeof(title));
      if (title[0] && ui_text_supported(title) &&
          !icon->name[0] && !icon->is_nds_forwarder)
         snprintf(icon->name, sizeof(icon->name), "%s", title);
      {
         u16 icon48[48 * 48];
         int x, y;
         smdh_icon_decode(smdh + 0x24C0, 48, icon48);
         for (y = 0; y < 32; y++)
            for (x = 0; x < 32; x++)
               icon->icon32[y * 32 + x] =
                  icon48[(y * 48 / 32) * 48 + x * 48 / 32];
      }
      icon->has_icon = true;
   }
   if (!icon->name[0])
      snprintf(icon->name, sizeof(icon->name), "TWL %08lX",
               (unsigned long)(u32)tid);
   if (!icon->has_icon)
   {
      int p;
      for (p = 0; p < 32 * 32; p++)
      {
         icon->icon32[p] = UI_RGB(0x35, 0x3D, 0x4B);
         icon->icon32_transparent[p] = 0;
      }
   }
}

static bool record_title_id(const char *path, u64 tid);

static void reconcile_nds_title_records(const u64 *ids, u32 count)
{
   int record;
   u32 i;
   for (record = 0; record < ntitle_records; record++)
   {
      u64 expected;
      if (!path_is_nds(title_records[record].path))
         continue;
      expected = nds_title_id_for_path(title_records[record].path);
      if (!expected || expected == title_records[record].tid)
         continue;
      for (i = 0; i < count && i < MAX_TWL_ICONS; i++)
         if (ids[i] == expected)
         {
            record_title_id(title_records[record].path, expected);
            break;
         }
   }
}

static void read_twl_titles(void)
{
   u64 ids[MAX_TWL_ICONS];
   u64 sd_ids[MAX_TWL_ICONS];
   u32 count = 0, i;
   u32 sd_count = 0;

   ntwl_icons = 0;
   twl_titles_dirty = false;
   if (multi_nds_title_ids(ids, MAX_TWL_ICONS, &count))
   {
      twl_installed_count = 0xFFFFFFFF;
      return;
   }
   twl_installed_count = count;
   reconcile_nds_title_records(ids, count);
   for (i = 0; i < count && i < MAX_TWL_ICONS; i++)
      add_twl_title(ids[i], MEDIATYPE_NAND);
   if (multi_nds_sd_title_ids(sd_ids, MAX_TWL_ICONS, &sd_count) == 0)
      for (i = 0; i < sd_count && i < MAX_TWL_ICONS; i++)
         add_twl_title(sd_ids[i], MEDIATYPE_SD);
}

static bool nds_services_ready;

static u64 title_id_for_path(const char *path, int sys)
{
   int i;
   u32 offset;
   u64 tid, prefix, candidate;
   u32 base_uid, uid;
   for (i = 0; i < ntitle_records; i++)
      if (!strcmp(title_records[i].path, path))
         return title_records[i].tid;
   tid = (sys == SYS_GBA || sys == SYS_SNES)
      ? fwdcia_iconmaker_title_id_for(path) : fwdcia_title_id_for(path);
   if (icon_installed_for_system(tid, sys))
      return tid;
   if (!title_id_used(tid))
      return tid;
   prefix = tid & 0xFFFFFFFF000000FFULL;
   base_uid = (u32)(tid >> 8) & 0xFFFFF;
   for (offset = 1; offset < FORWARDER_UID_COUNT; offset++)
   {
      uid = FORWARDER_UID_START +
         (base_uid - FORWARDER_UID_START + offset) % FORWARDER_UID_COUNT;
      candidate = prefix | (u64)uid << 8;
      if (!title_id_used(candidate))
         return candidate;
   }
   return 0;
}

static bool record_title_id(const char *path, u64 tid)
{
   FILE *f;
   int i;
   for (i = 0; i < ntitle_records; i++)
      if (!strcmp(title_records[i].path, path))
      {
         bool failed;
         if (title_records[i].tid == tid)
            return true;
         f = fopen(TITLE_IDS, "a");
         if (!f)
            return false;
         failed = fprintf(f, "%s\t%016llX\n", path,
                          (unsigned long long)tid) < 0;
         if (fclose(f) != 0)
            failed = true;
         if (failed)
            return false;
         title_records[i].tid = tid;
         return true;
      }
   if (ntitle_records >= MAX_ENTRIES || strlen(path) >= sizeof(title_records[0].path))
      return false;
   f = fopen(TITLE_IDS, "a");
   if (!f)
      return false;
   {
      bool failed = fprintf(f, "%s\t%016llX\n", path, (unsigned long long)tid) < 0;
      if (fclose(f) != 0)
         failed = true;
      if (failed)
         return false;
   }
   snprintf(title_records[ntitle_records].path, sizeof(title_records[0].path), "%s", path);
   title_records[ntitle_records].tid = tid;
   ntitle_records++;
   return true;
}

static void fwdcia_sha256(const void *data, uint32_t size, uint8_t out[32])
{
   sha256(data, size, out);
}

/* Forwarders made by this app share a publisher and use the reserved range. */
static void read_icons(void)
{
   static u8 smdh[FWDCIA_SMDH_SIZE];
   u32 count = 0, got = 0, i;
   u64 *ids;
   installed_icons_dirty = false;
   nicons = 0;
   noccupied_ids = 0;
   memset(sys_installed, 0, sizeof(sys_installed));
   if (R_FAILED(AM_GetTitleCount(MEDIATYPE_SD, &count)) || !count)
   {
      rebuild_installed_filter();
      return;
   }
   ids = malloc(count * sizeof(u64));
   if (!ids)
   {
      rebuild_installed_filter();
      return;
   }
   if (R_SUCCEEDED(AM_GetTitleList(&got, MEDIATYPE_SD, count, ids)))
      for (i = 0; i < got; i++)
      {
         u32 uid = (u32)ids[i] >> 8;
         char pub[64];
         int k, sys = -1;
         for (k = 0; k < SYS_COUNT; k++)
            if (ids[i] == systems[k].tid)
               sys_installed[k] = true;
         if ((ids[i] >> 32) == 0x00040000 &&
             uid >= FORWARDER_UID_START && uid < FORWARDER_UID_END &&
             noccupied_ids < (int)(sizeof(occupied_ids) / sizeof(occupied_ids[0])))
            occupied_ids[noccupied_ids++] = ids[i];
         if ((ids[i] >> 32) != 0x00040000 ||
             uid < FORWARDER_UID_START || uid >= FORWARDER_UID_END)
            continue;
         if (!read_title_smdh(ids[i], smdh))
            continue;
         utf16_to_ui(smdh + 8 + 0x180, 0x40, pub, sizeof(pub));   /* English publisher */
         for (k = 0; k < ntitle_records; k++)
            if (title_records[k].tid == ids[i])
            {
               sys = system_of(title_records[k].path);
               break;
            }
         if (sys < 0 && (!strcmp(pub, "3DSX Icon Maker") ||
                         !strcmp(pub, "3DSX Forwarder")))
            sys = SYS_3DSX;
         if (sys < 0 && !strcmp(pub, "gpSP 3D"))
            sys = SYS_GBA;
         if (sys < 0 && !strcmp(pub, "Snes9x 3D"))
            sys = SYS_SNES;
         if (sys < 0)
            continue;
         if (nicons >= MAX_ICONS)
            continue;
         icons[nicons].tid = ids[i];
         icons[nicons].sys = sys;
         utf16_to_ui(smdh + 8, 0x40, icons[nicons].name, sizeof(icons[0].name));
         snprintf(icons[nicons].publisher, sizeof(icons[0].publisher), "%s", pub);
         smdh_icon_decode(smdh + 0x24C0, 48, icons[nicons].icon48);
         nicons++;
      }
   free(ids);
   qsort(icons, nicons, sizeof(icon_entry), cmp_icon);
   rebuild_installed_filter();
}

static bool refresh_installed_icon(const char *path, int sys)
{
   static u8 smdh[FWDCIA_SMDH_SIZE];
   icon_entry fresh;
   u64 tid = title_id_for_path(path, sys);
   int i, index = -1;
   if (!tid || !read_title_smdh(tid, smdh))
      return false;
   memset(&fresh, 0, sizeof(fresh));
   fresh.tid = tid;
   fresh.sys = sys;
   utf16_to_ui(smdh + 8, 0x40, fresh.name, sizeof(fresh.name));
   utf16_to_ui(smdh + 8 + 0x180, 0x40, fresh.publisher, sizeof(fresh.publisher));
   smdh_icon_decode(smdh + 0x24C0, 48, fresh.icon48);
   for (i = 0; i < nicons; i++)
      if (icons[i].tid == tid)
      {
         index = i;
         break;
      }
   if (index < 0)
   {
      if (nicons >= MAX_ICONS)
         return false;
      index = nicons++;
   }
   icons[index] = fresh;
   if ((tid >> 32) == 0x00040000)
   {
      u32 uid = (u32)tid >> 8;
      if (uid >= FORWARDER_UID_START && uid < FORWARDER_UID_END &&
          noccupied_ids < (int)(sizeof(occupied_ids) / sizeof(occupied_ids[0])))
      {
         bool found = false;
         for (i = 0; i < noccupied_ids; i++)
            if (occupied_ids[i] == tid)
               found = true;
         if (!found)
            occupied_ids[noccupied_ids++] = tid;
      }
   }
   qsort(icons, nicons, sizeof(icon_entry), cmp_icon);
   rebuild_installed_filter();
   return true;
}

static void remove_installed_icon(u64 tid)
{
   int i, j;
   for (i = 0; i < nicons; i++)
      if (icons[i].tid == tid)
      {
         for (j = i + 1; j < nicons; j++)
            icons[j - 1] = icons[j];
         nicons--;
         rebuild_installed_filter();
         return;
      }
}

static void set_game_installed_badge(u64 tid, bool installed)
{
   int i, j;
   for (i = 0; i < ntitle_records; i++)
      if (title_records[i].tid == tid)
         for (j = 0; j < nentries; j++)
         {
            char path[800];
            if (entries[j].dir)
               continue;
            snprintf(path, sizeof(path), "%s%s", cwd, entries[j].name);
            if (!strcmp(path, title_records[i].path))
               entries[j].has_icon = installed;
         }
}

static bool icon_installed(u64 tid)
{
   int i;
   for (i = 0; i < nicons; i++)
      if (icons[i].tid == tid)
         return true;
   return false;
}

static bool dialog_with_default(const char *title, const char *body,
                                const char *ok, const char *cancel,
                                u16 ok_color, bool default_ok);

static bool dialog(const char *title, const char *body, const char *ok,
                   const char *cancel, u16 ok_color)
{
   return dialog_with_default(title, body, ok, cancel, ok_color, false);
}

/* ------------------------------------------------------------------ */
/* the file list                                                       */
/* ------------------------------------------------------------------ */

static int system_of_ext(const char *dot)
{
   if (!dot) return -1;
   if (!strcasecmp(dot, ".3dsx")) return SYS_3DSX;
   if (!strcasecmp(dot, ".gba")) return SYS_GBA;
   if (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc") ||
       !strcasecmp(dot, ".fig") ||
       !strcasecmp(dot, ".bs") || !strcasecmp(dot, ".bsx")) return SYS_SNES;
   if (!strcasecmp(dot, ".nds") || !strcasecmp(dot, ".srl")) return SYS_NDS;
   if (!strcasecmp(dot, ".zip")) return SYS_GBA;
   return -1;
}

/* Whether a path is a launchable homebrew executable. */
static int system_of(const char *path)
{
   const char *dot = strrchr(path, '.');
   return system_of_ext(dot);
}

/* The pictures in the folder, by name: looking for a
 * file that isn't there costs about 60 ms on the SD card, so each game's
 * are found once, here */
typedef struct { char **name; int n, room; } name_list;
static name_list pics;

static bool is_picture(const char *name)
{
   const char *dot = strrchr(name, '.');
   int i;
   for (i = 0; dot && i < 3; i++)
      if (!strcasecmp(dot + 1, pic_types[i]))
         return true;
   return false;
}

static void add_picture(name_list *l, const char *name)
{
   if (l->n == l->room)
   {
      int room = l->room ? l->room * 2 : 256;
      char **p = realloc(l->name, room * sizeof(char*));
      if (!p)
         return;
      l->name = p;
      l->room = room;
   }
   if ((l->name[l->n] = strdup(name)))
      l->n++;
}

static void clear_pictures(name_list *l)
{
   int i;
   for (i = 0; i < l->n; i++)
      free(l->name[i]);
   l->n = 0;
}

static int cmp_name(const void *a, const void *b)
{
   return strcasecmp(*(char *const*)a, *(char *const*)b);
}

static bool has_picture(const name_list *l, const char *want)
{
   return l->n && bsearch(&want, l->name, l->n, sizeof(char*), cmp_name);
}

/* each app's matching icon and banner picture, if any */
static void find_pictures(void)
{
   int i, k, t;
   qsort(pics.name, pics.n, sizeof(char*), cmp_name);
   for (i = 0; i < nentries; i++)
   {
      const char *name = entries[i].name, *dot = strrchr(name, '.');
      entries[i].pic[0] = entries[i].pic[1] = -1;
      if (entries[i].dir || !dot)
         continue;
      for (k = 0; k < 2; k++)
         for (t = 2; t >= 0; t--)          /* png wins, as when they were opened in turn */
         {
            char want[300];
            snprintf(want, sizeof(want), "%.*s%s.%s", (int)(dot - name), name, k ? ".banner" : "", pic_types[t]);
            if (has_picture(&pics, want))
               entries[i].pic[k] = (s8)t;
         }
   }
   clear_pictures(&pics);
}

static int cmp_entry(const void *a, const void *b)
{
   const entry *x = a, *y = b;
   if (x->dir != y->dir)
      return x->dir ? -1 : 1;
   return strcasecmp(x->name, y->name);
}

static void read_dir(void)
{
#ifdef ICONMAKER_TIMING
   u64 t_dir = svcGetSystemTick();
#endif
   DIR *d = opendir(cwd);
   struct dirent *e;
   nentries = ngames = 0;
   if (strcmp(cwd, "sdmc:/"))
   {
      strcpy(entries[0].name, "..");
      entries[0].dir = true;
      nentries = 1;
   }
   if (!d)
      return;
   clear_pictures(&pics);
   while ((e = readdir(d)) && nentries < MAX_ENTRIES)
   {
      bool dir = e->d_type == DT_DIR;
      char path[800];
      int sys = -1;
      if (e->d_name[0] == '.')
         continue;
      if (!dir && is_picture(e->d_name))
      {
         add_picture(&pics, e->d_name);
         continue;
      }
      snprintf(path, sizeof(path), "%s%s", cwd, e->d_name);
      if (!dir && (sys = system_of(path)) < 0)
         continue;
      snprintf(entries[nentries].name, sizeof(entries[0].name), "%s", e->d_name);
      entries[nentries].dir = dir;
      entries[nentries].sys = sys;
      entries[nentries].has_icon = false;
      if (!dir)
      {
         entries[nentries].has_icon = sys == SYS_NDS
            ? twl_title_installed(nds_title_id_for_path(path))
            : icon_installed(title_id_for_path(path, entries[nentries].sys));
         ngames++;
      }
      nentries++;
   }
   closedir(d);
   find_pictures();
   qsort(entries + (strcmp(cwd, "sdmc:/") ? 1 : 0),
         nentries - (strcmp(cwd, "sdmc:/") ? 1 : 0), sizeof(entry), cmp_entry);
#ifdef ICONMAKER_TIMING
   {
      char line[700];
      tm_dir_us = tm_us(svcGetSystemTick() - t_dir);
      tm_dir_n = nentries;
      snprintf(line, sizeof(line), "folder %s: %d entries, %d games, read in %.1f ms\n",
               cwd, nentries, ngames, tm_dir_us / 1000.0);
      tm_log(line);
   }
#endif
}

static void cache_clear(void);

static void enter(const char *name)
{
   size_t n = strlen(cwd);
   if (!strcmp(name, ".."))
   {
      if (n > 6)
      {
         cwd[n - 1] = 0;                       /* drop the trailing '/' */
         *(strrchr(cwd, '/') + 1) = 0;
      }
   }
   else if (n + strlen(name) + 2 < sizeof(cwd))
   {
      strcat(cwd, name);
      strcat(cwd, "/");
   }
   cache_clear();
   read_dir();
   nds_icon_default_pending = true;
}

static void leave_folder(int *sel)
{
   char name[sizeof(entries[0].name)];
   const char *end, *start;
   size_t n = strlen(cwd);
   int i;

   if (n <= 6)
   {
      *sel = 0;
      return;
   }
   end = cwd + n - 1;
   start = end;
   while (start > cwd + 6 && start[-1] != '/')
      start--;
   n = end - start;
   if (!n || n >= sizeof(name))
   {
      *sel = 0;
      enter("..");
      return;
   }
   memcpy(name, start, n);
   name[n] = 0;
   enter("..");
   for (i = 0; i < nentries; i++)
      if (entries[i].dir && !strcmp(entries[i].name, name))
      {
         *sel = i;
         return;
      }
   *sel = 0;
}

/* The game's picture with this ending ("" or ".banner") and type (an index
 * into pic_types, -1: none), as RGBA, or NULL. It comes at least
 * min_w x min_h (or its own size if smaller), but often not much bigger:
 * PNGs are shrunk while read (pngshrink.c), JPEGs decoded small while read
 * (jpegshrink.c); stb_image decodes all of the rest (interlaced PNGs, JPEGs
 * libjpeg can't read) if that fits. NULL with *too_big if the picture is
 * too big, *w x *h its size (if known). */
static u8 *load_picture(const char *game, const char *ext, int type, int min_w, int min_h,
                        int *w, int *h, bool *too_big)
{
   char file[600];
   const char *dot = strrchr(game, '.');
   bool banner = ext[0] != 0;
   u8 sig[8], *buf = NULL, *img = NULL;
   long size;
   int n, interlaced = 0, why = 0;
   size_t got;
   u32 mem = 0;
   FILE *f;
   *too_big = false;
   *w = *h = 0;
   if (type < 0)
      return NULL;
   snprintf(file, sizeof(file), "%.*s%s.%s", (int)(dot - game), game, ext, pic_types[type]);
   if (!(f = fopen(file, "rb")))
      return NULL;
   setvbuf(f, NULL, _IOFBF, 64 * 1024);  /* the SD card is much faster in big reads */
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   /* by what's in it, not the name: a .jpg can be a PNG */
   got = fread(sig, 1, 8, f);
   if (got == 8 && !memcmp(sig, "\x89PNG\r\n\x1a\n", 8))
   {
      fseek(f, 0, SEEK_SET);
      img = png_shrink(f, min_w, min_h, PIC_MAX_SIDE, w, h, &interlaced, &mem);
      tm_mark(banner ? "banner: read + shrink PNG (libpng)" : "icon: read + shrink PNG (libpng)");
#ifdef ICONMAKER_TIMING
      /* in that: the SD card, libpng + zlib unpacking, our averaging */
      tm_note("  of it: reading the SD card", png_shrink_read_ticks);
      tm_note("  of it: libpng + zlib", png_shrink_rows_ticks - png_shrink_read_ticks);
      tm_note("  of it: averaging rows", png_shrink_average_ticks);
      if (spinner_ticks)
         tm_note("  of it: drawing the spinner", spinner_ticks);
#endif
      if (!img && (!interlaced || *w > PIC_MAX_SIDE || *h > PIC_MAX_SIDE ||
                   (long long)*w * *h * 4 > PIC_MAX_FULL))
      {
         *too_big = *w > PIC_MAX_SIDE || *h > PIC_MAX_SIDE || interlaced;
         fclose(f);
         return NULL;
      }
   }
   else if (got >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF)
   {
#ifdef ICONMAKER_TIMING
      u64 t_jpeg = svcGetSystemTick(), spun = spinner_ticks;
#endif
      fseek(f, 0, SEEK_SET);
      img = jpeg_shrink(f, min_w, min_h, PIC_MAX_SIDE, PIC_MAX_COEF, w, h, &why, &mem);
      tm_mark(banner ? "banner: read + decode JPEG (libjpeg)" : "icon: read + decode JPEG (libjpeg)");
#ifdef ICONMAKER_TIMING
      /* in that: the SD card, libjpeg decoding (and the spinner) */
      tm_note("  of it: reading the SD card", jpeg_shrink_read_ticks);
      tm_note("  of it: libjpeg decoding", svcGetSystemTick() - t_jpeg - jpeg_shrink_read_ticks -
                                           (spinner_ticks - spun));
      if (spinner_ticks != spun)
         tm_note("  of it: drawing the spinner", spinner_ticks - spun);
#endif
      if (why)                         /* too big, or a button stopped it */
      {
         *too_big = why == JPEG_SHRINK_TOO_BIG;
         fclose(f);
         return NULL;
      }
   }
   if (!img)
   {
      buf = size > 0 && size <= PIC_MAX_FILE ? malloc(size) : NULL;
      fseek(f, 0, SEEK_SET);
      if (buf && fread(buf, 1, size, f) != (size_t)size)
      {
         free(buf);
         buf = NULL;
      }
      *too_big = size > PIC_MAX_FILE;
      tm_mark(banner ? "banner: read the file" : "icon: read the file");
   }
   fclose(f);
   if (buf && !*too_big)
   {
      /* all of it at once: only if that fits */
      int iw, ih;
      if (stbi_info_from_memory(buf, size, &iw, &ih, &n))
      {
         if (iw > PIC_MAX_SIDE || ih > PIC_MAX_SIDE || (long long)iw * ih * 4 > PIC_MAX_FULL)
         {
            *w = iw;
            *h = ih;
            *too_big = true;
         }
         else
         {
            img = stbi_load_from_memory(buf, size, w, h, &n, 4);
            mem = (u32)iw * ih * 4;
         }
      }
      tm_mark(banner ? "banner: decode (stb_image)" : "icon: decode (stb_image)");
   }
#ifdef ICONMAKER_TIMING
   if (*too_big)
      snprintf(tm_pic[banner], sizeof(tm_pic[0]), "%s, %ld KB, %dx%d: too big%s",
               pic_types[type], size / 1024, *w, *h, interlaced ? " (interlaced)" : "");
   else
      snprintf(tm_pic[banner], sizeof(tm_pic[0]), "%s, %ld KB, decoded at %dx%d, %lu KB used",
               pic_types[type], size / 1024, img ? *w : 0, img ? *h : 0, (unsigned long)(mem / 1024));
#endif
   free(buf);
   return img;
}

/* a picture to ow x oh, padded or cropped (fit_mode) */
static void fit_picture(const u8 *img, int w, int h, u8 *out, int ow, int oh)
{
   image_fit(img, w, h, out, ow, oh);
}

/* Text in one of gpSP 3D's fonts onto an RGBA picture (w wide), with a
 * soft shadow. Returns the x after it. */
static int rgba_text(u8 *img, int w, int h, const ui_font *f, int x, int y, const char *s, u32 rgb)
{
   int pass;
   int x_end = x;
   for (pass = 0; pass < 2; pass++)
   {
      int cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 2);
      u32 col = pass ? rgb : 0x0A0A1E;
      const char *p = s;
      while (*p)
      {
         const ui_glyph *g = ui_glyph_of(f, ui_utf8_next(&p));
         const unsigned char *b = f->bits + g->off;
         int i, j, n = 0;
         for (j = 0; j < g->h; j++)
            for (i = 0; i < g->w; i++, n++)
            {
               int v = (b[n >> 1] >> ((n & 1) * 4)) & 15, px = cx + g->x + i, py = cy + g->y + j, k;
               u8 *d;
               if (!v || px < 0 || py < 0 || px >= w || py >= h)
                  continue;
               d = img + (py * w + px) * 4;
               v = v * 17 * (pass ? 255 : 150) / 255;
               {
                  /* over what's there, alpha too (the banner can be transparent) */
                  int da = d[3] * (255 - v) / 255, oa = v + da;
                  for (k = 0; k < 3; k++)
                  {
                     int c = (col >> (16 - 8 * k)) & 255;
                     d[k] = (u8)((c * v + d[k] * da) / oa);
                  }
                  d[3] = (u8)oa;
               }
            }
         cx += g->adv;
      }
      x_end = cx;
   }
   return x_end;
}

/* the game's name word-wrapped into at most rows lines of font f, each at
 * most maxw wide (" - " starts a new line, "..." where it's cut). Returns the
 * number of lines. */
static int wrap_name(const char *title, int sys, const ui_font *f, int maxw, char lines[][64], int rows)
{
   char clean[128];
   const char *s;
   int nl = 0;

   clean_name(title, clean, sizeof(clean));
   memset(lines, 0, rows * 64);
   s = clean;
   while (*s && nl < rows)
   {
      char line[64] = "";
      while (*s == ' ') s++;
      while (*s)
      {
         char test[64];
         const char *e = s;
         int w;
         while (*e && *e != ' ') e++;
         if (e - s == 1 && *s == '-')        /* " - " between parts: a new line */
         {
            s = e;
            if (line[0]) break;
            continue;
         }
         snprintf(test, sizeof(test), "%s%s%.*s", line, line[0] ? " " : "", (int)(e - s), s);
         w = ui_text_w(f, test);
         if (w > maxw && line[0])
            break;
         snprintf(line, sizeof(line), "%s", test);
         s = e;
         while (*s == ' ') s++;
         if (w > maxw)
            break;
      }
      if (!line[0])
         break;
      /* a word too long for the line: cut with "..." */
      while (ui_text_w(f, line) > maxw && strlen(line) > 4)
      {
         size_t n = strlen(line);
         line[n - 4] = '.'; line[n - 3] = '.'; line[n - 2] = '.'; line[n - 1] = 0;
      }
      if (nl == rows - 1 && *s)                     /* more left over: "..." */
      {
         size_t n = strlen(line);
         while (n > 4 && ui_text_w(f, line) + ui_text_w(f, "...") > maxw)
            line[--n] = 0;
         strcat(line, "...");
      }
      snprintf(lines[nl++], sizeof(lines[0]), "%s", line);
   }
   if (!nl)
      snprintf(lines[nl++], sizeof(lines[0]), "%s game", systems[sys].console);

   return nl;
}

/* the game's name on the generated banner, right of the icon: up to three
 * lines of Inter, then the emulator's name */
static void banner_name(const char *title, int sys, u8 *banner, int maxw)
{
   char lines[3][64];
   int nl = wrap_name(title, sys, &ui_font_bold, maxw, lines, 3), i, y;

   y = (128 - (nl * 20 + 22)) / 2;
   for (i = 0; i < nl; i++)
      rgba_text(banner, 256, 128, &ui_font_bold, 120, y + i * 20, lines[i], 0xFFFFFF);
   rgba_text(banner, 256, 128, &ui_font_small, 121, y + nl * 20 + 6, systems[sys].name, systems[sys].tag_rgb);
}

/* text centred at y with a dark outline, so it reads on any HOME Menu theme */
static void outlined_text(u8 *img, int w, int h, const ui_font *f, int y, const char *s, u32 rgb)
{
   static const signed char d[8][2] = { {-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1} };
   int i, x = (w - ui_text_w(f, s)) / 2;
   for (i = 0; i < 8; i++)
      rgba_text(img, w, h, f, x + d[i][0] * 2, y + d[i][1] * 2, s, 0x0A0A1E);
   rgba_text(img, w, h, f, x, y, s, rgb);
}

/* the 3D banner's name strip (256x64, transparent): the name big on one
 * line with the system and emulator under it, or smaller on two lines */
static void name_strip(const char *title, int sys, u8 *strip)
{
   char lines[2][64], tag[64];
   int i;

   memset(strip, 0, 256 * 64 * 4);
   if (wrap_name(title, sys, &ui_font_big, 246, lines, 1) == 1 && !strstr(lines[0], "..."))
   {
      outlined_text(strip, 256, 64, &ui_font_big, 0, lines[0], 0xFFFFFF);
      snprintf(tag, sizeof(tag), "%s  \xC2\xB7  %s", systems[sys].console, systems[sys].name);
      outlined_text(strip, 256, 64, &ui_font_body, 44, tag, systems[sys].tag_rgb);
      return;
   }
   if (wrap_name(title, sys, &ui_font_title, 246, lines, 2) == 1)
   {
      outlined_text(strip, 256, 64, &ui_font_title, 8, lines[0], 0xFFFFFF);
      snprintf(tag, sizeof(tag), "%s  \xC2\xB7  %s", systems[sys].console, systems[sys].name);
      outlined_text(strip, 256, 64, &ui_font_body, 40, tag, systems[sys].tag_rgb);
      return;
   }
   for (i = 0; i < 2; i++)
      outlined_text(strip, 256, 64, &ui_font_title, 4 + i * 28, lines[i], 0xFFFFFF);
}

/* ------------------------------------------------------------------ */
/* the icon and banner a game gets                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
   u8 icon48[48 * 48 * 4];
   u8 preview_icon[96 * 96 * 4];
   u8 banner[256 * 128 * 4];     /* what the top screen previews (flat) */
   u8 name[256 * 64 * 4];        /* 3D: the name strip under the tile */
   u8 label[128 * 128 * 4];      /* 3D: the icon, on the tile */
   char title[64];
   char publisher[64];
   int sys;
   bool has_icon_png, has_smdh_icon, has_banner_png;
   bool flat;                    /* a flat banner (its .banner picture, or SELECT), not 3D */
   bool icon_too_big, banner_too_big;   /* a picture too big to use, */
   int icon_big[2], banner_big[2];      /* and its size (0 x 0: not known) */
} game_art;

static void make_art(const char *path, const entry *e, game_art *a)
{
   const char *name = strrchr(path, '/') + 1, *dot = strrchr(name, '.');
   int n = (int)(dot - name), sys = e->sys, iw, ih, bw, bh;
   u8 *icon_png, *banner_png = NULL;
   static u8 smdh[FWDCIA_SMDH_SIZE];
   static u8 smdh_rgba[48 * 48 * 4];
   bool has_smdh;
   bool big = false;

   iw = ih = 0;
   snprintf(a->title, sizeof(a->title), "%.*s", n > 63 ? 63 : n, name);
   a->publisher[0] = 0;
   a->sys = sys;
   has_smdh = read_app_smdh(path, smdh);
   a->has_smdh_icon = has_smdh;
   if (has_smdh)
   {
      int lang;
      utf16_to_ui(smdh + 8 + 0x200, 0x40, a->title, sizeof(a->title));
      for (lang = 0; !a->title[0] && lang < 16; lang++)
         utf16_to_ui(smdh + 8 + (size_t)lang * 0x200, 0x40,
                     a->title, sizeof(a->title));
      if (!a->title[0])
         snprintf(a->title, sizeof(a->title), "%.*s", n > 63 ? 63 : n, name);
      utf16_to_ui(smdh + 8 + 0x200 + 0x180, 0x40, a->publisher, sizeof(a->publisher));
      for (lang = 0; !a->publisher[0] && lang < 16; lang++)
         utf16_to_ui(smdh + 8 + (size_t)lang * 0x200 + 0x180, 0x40,
                     a->publisher, sizeof(a->publisher));
   }
   icon_png = load_picture(path, "", e->pic[0], 128, 128, &iw, &ih, &big);
   a->has_icon_png = icon_png != NULL;
   a->icon_too_big = big;
   a->icon_big[0] = iw;
   a->icon_big[1] = ih;
   if (icon_png)
   {
      fit_picture(icon_png, iw, ih, a->label, 128, 128);
      tm_mark("icon: scale to 128x128");
      fit_picture(icon_png, iw, ih, a->icon48, 48, 48);
      stbi_image_free(icon_png);
   }
   else if (has_smdh)
   {
      smdh_icon_rgba(smdh, smdh_rgba);
      image_fit(smdh_rgba, 48, 48, a->icon48, 48, 48);
      image_fit(smdh_rgba, 48, 48, a->label, 128, 128);
   }
   else
   {
      default_icon_style(a->title, systems[sys].style, a->icon48);
      image_fit(a->icon48, 48, 48, a->label, 128, 128);
   }
   tm_mark("icon: scale to 48x48");
   /* A game's own .banner picture always wins: it's shown flat, also when
    * 3D is chosen (3D is for the games without one). Without one, the 3D
    * banner needs no flat banner. */
   {
      bool use_custom_banner = e->pic[1] >= 0 && banner_cycle == 0;
      a->has_banner_png = use_custom_banner;
      a->flat = use_custom_banner || banner_style != STYLE_3D;
      a->banner_too_big = false;
      if (a->flat || use_custom_banner)
      {
         banner_png = use_custom_banner
            ? load_picture(path, ".banner", e->pic[1], 256, 128, &bw, &bh, &big)
            : NULL;
         a->has_banner_png = banner_png != NULL;
         a->banner_too_big = big;
         a->banner_big[0] = bw;
         a->banner_big[1] = bh;
         if (banner_png)
         {
            fit_picture(banner_png, bw, bh, a->banner, 256, 128);
            stbi_image_free(banner_png);
            a->flat = true;
         }
         else if (a->flat)
         {
            default_banner(a->title, a->label, 128, 128, a->banner,
                           banner_style == STYLE_CLEAR ? BANNER_CLEAR : 0);
            banner_name(a->title, sys, a->banner, 256 - 120 - 8);
         }
         tm_mark("flat banner");
      }
   }
   if (!a->flat)
   {
      name_strip(a->title, sys, a->name);
      tm_mark("3D: name strip");
      {
         /* see-through parts take the tile's grey (make_banner3d.py's Body);
          * outside a square with rounded corners (radius 0.2 of the width,
          * as make_banner3d.py's CORNER) it's transparent: the corners cut round */
         int i, k;
         static const u8 body[3] = { 0x4A, 0x4F, 0x5A };
         for (i = 0; i < 128 * 128; i++)
         {
            u8 *q = a->label + i * 4;
            int x = i % 128, y = i / 128, r = 26, dx = 0, dy = 0;
            for (k = 0; k < 3; k++)
               q[k] = (u8)((q[k] * q[3] + body[k] * (255 - q[3])) / 255);
            if (x < r) dx = r - x; else if (x > 127 - r) dx = x - (127 - r);
            if (y < r) dy = r - y; else if (y > 127 - r) dy = y - (127 - r);
            q[3] = dx * dx + dy * dy > r * r ? 0 : 255;
         }
      }
      tm_mark("3D: tile");
   }
   if (!a->flat && (sys == SYS_GBA || sys == SYS_SNES))
      image_fit(a->label, 128, 128, a->preview_icon, 96, 96);
}

static Result install_cia_buf(const u8 *cia, u32 size);

static int make_nds_forwarder(const char *path, const char *custom_title,
                              char *err, int errlen)
{
   u64 tid = nds_title_id_for_path(path);
   bool already_installed = twl_title_installed(tid);
   if (!nds_services_ready)
   {
      snprintf(err, errlen, "NDS forwarder services are unavailable.");
      return -1;
   }
   if (!tid)
   {
      snprintf(err, errlen, "Could not read the NDS game code.");
      return -1;
   }
   if (!twl_title_installed(tid) && twl_installed_count != 0xFFFFFFFF &&
       twl_installed_count >= 40)
   {
      snprintf(err, errlen, "The 40-title TWL forwarder limit has been reached.");
      return -1;
   }
   if (multi_nds_build_install(path, custom_title, err, errlen))
      return -1;
   if (!record_title_id(path, tid))
   {
      snprintf(err, errlen, "The NDS forwarder installed, but its title record could not be saved.");
      return -1;
   }
   if (!already_installed && twl_installed_count != 0xFFFFFFFF)
      twl_installed_count++;
   add_twl_title(tid, MEDIATYPE_NAND);
   return 0;
}

static int make_rom_icon(const char *path, const entry *e,
                         const char *custom_title, const char *custom_publisher,
                         char *err, int errlen)
{
   static game_art art;
   static u8 *smdh;
   char template_path[64], wav_path[800], long_title[128];
   FILE *f = NULL;
   u8 *cia = NULL, *cwav = NULL;
   long cia_len;
   size_t cwav_size = 0;
   size_t banner_reserved;
   uint32_t audio_capacity;
   int result = -1;
   u64 tid;
   cia_t parsed;
   fwdcia_params params;
   Result install_result;
   u8 *template_banner;
   const char *dot = strrchr(path, '.');

   make_art(path, e, &art);
   if (custom_title && custom_title[0])
      snprintf(art.title, sizeof(art.title), "%s", custom_title);
   tid = title_id_for_path(path, e->sys);
   if (!tid)
   {
      snprintf(err, errlen, "No available HOME Menu title IDs remain.");
      return -1;
   }
   snprintf(template_path, sizeof(template_path), art.flat
            ? "romfs:/template-gpsp3d.cia"
            : "romfs:/template-gpsp3d-3d.cia");
   if (!(f = fopen(template_path, "rb")) || fseek(f, 0, SEEK_END) ||
       (cia_len = ftell(f)) <= 0 || cia_len > UINT32_MAX || fseek(f, 0, SEEK_SET))
   {
      if (f) fclose(f);
      snprintf(err, errlen, "Could not read the ROM forwarder template.");
      return -1;
   }
   cia = malloc((size_t)cia_len);
   if (!cia || fread(cia, 1, (size_t)cia_len, f) != (size_t)cia_len)
   {
      fclose(f);
      free(cia);
      snprintf(err, errlen, "Could not load the ROM forwarder template.");
      return -1;
   }
   fclose(f);
   f = NULL;
   if (cia_open(&parsed, cia, (size_t)cia_len) != CIA_OK ||
       !(smdh = cia_icon_data(&parsed)))
   {
      free(cia);
      snprintf(err, errlen, "The ROM forwarder template is invalid.");
      return -1;
   }
   snprintf(long_title, sizeof(long_title), "%.110s", art.title);
   smdh_set(smdh, art.title, long_title,
            custom_publisher ? custom_publisher : systems[e->sys].name,
            art.icon48);

   memset(&params, 0, sizeof(params));
   params.title_id = tid;
   params.target = systems[e->sys].tid;
   params.launch = systems[e->sys].launch;
   params.path = path;
   params.smdh = smdh;
   params.banner = art.flat ? art.banner : art.name;
   params.label = art.flat ? NULL : art.label;

   if (dot && (size_t)(dot - path) + sizeof(".banner.wav") <= sizeof(wav_path))
   {
      snprintf(wav_path, sizeof(wav_path), "%.*s.banner.wav", (int)(dot - path), path);
      template_banner = cia_banner_data(&parsed, &banner_reserved);
      if (!template_banner || banner_reserved > UINT32_MAX ||
          banner_sound_capacity(template_banner, (uint32_t)banner_reserved,
                                &audio_capacity, err, errlen))
      {
         free(cia);
         return -1;
      }
      result = wav_to_cwav_file(wav_path, audio_capacity, &cwav, &cwav_size,
                                err, errlen);
      if (result < 0)
      {
         free(cia);
         return -1;
      }
      if (result == 0)
      {
         if (cwav_size > UINT32_MAX)
         {
            free(cwav);
            free(cia);
            snprintf(err, errlen, "The custom banner sound is too large.");
            return -1;
         }
         params.cwav = cwav;
         params.cwav_size = (uint32_t)cwav_size;
      }
   }

   result = fwdcia_make(cia, (uint32_t)cia_len, &params, fwdcia_sha256, err, errlen);
   free(cwav);
   if (result)
   {
      free(cia);
      return -1;
   }
   if (!record_title_id(path, tid))
   {
      free(cia);
      snprintf(err, errlen, "Could not save the title-ID record in %s.", APP_DIR);
      return -1;
   }
   install_result = install_cia_buf(cia, (u32)cia_len);
   if (R_FAILED(install_result) && R_DESCRIPTION(install_result) == RD_ALREADY_EXISTS)
   {
      AM_DeleteTitle(MEDIATYPE_SD, tid);
      AM_DeleteTicket(tid);
      install_result = install_cia_buf(cia, (u32)cia_len);
   }
   free(cia);
   if (R_FAILED(install_result))
   {
      snprintf(err, errlen, "Install failed (%08lX).\nUse Luma3DS with signature patches.",
               (unsigned long)install_result);
      return -1;
   }
   return 0;
}

/* Make and install the icon for the game at path. Returns 0, or -1 with a
 * message in err. */
/* the CIA in memory -> installed */
static Result install_cia_buf(const u8 *cia, u32 size)
{
   Handle h;
   u32 written = 0;
   Result r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
   if (R_FAILED(r))
      return r;
   r = FSFILE_Write(h, &written, 0, cia, size, 0);
   if (R_SUCCEEDED(r) && written == size)
      return AM_FinishCiaInstall(h);
   AM_CancelCIAInstall(h);
   return R_SUCCEEDED(r) ? -1 : r;
}

static int apply_custom_banner_sound(cia_t *parsed, u8 *banner,
                                     size_t banner_size, const char *path,
                                     bool has_custom_banner, char *err,
                                     int errlen)
{
   const char *extension;
   char wav_path[800];
   uint8_t *cwav = NULL, *copy = NULL;
   size_t cwav_size = 0;
   uint32_t audio_capacity;
   uint32_t banner_len;
   int result;

   if (!has_custom_banner)
      return 0;
   extension = strrchr(path, '.');
   result = extension ? snprintf(wav_path, sizeof(wav_path), "%.*s.banner.wav",
                                 (int)(extension - path), path) : -1;
   if (result < 0 || result >= (int)sizeof(wav_path))
   {
      snprintf(err, errlen, "Could not create the .banner.wav path.");
      return -1;
   }
   if (banner_size > UINT32_MAX ||
       banner_sound_capacity(banner, (uint32_t)banner_size, &audio_capacity,
                             err, errlen))
      return -1;
   result = wav_to_cwav_file(wav_path, audio_capacity, &cwav, &cwav_size,
                             err, errlen);
   if (result == 1)
      return 0;
   if (result < 0)
      return -1;
   if (banner_size > UINT32_MAX || cwav_size > UINT32_MAX)
   {
      free(cwav);
      snprintf(err, errlen, "The custom banner sound is too large.");
      return -1;
   }
   copy = malloc(banner_size);
   if (!copy)
   {
      free(cwav);
      snprintf(err, errlen, "Not enough memory to set the banner sound.");
      return -1;
   }
   memcpy(copy, banner, banner_size);
   result = banner_set_sound(copy, (uint32_t)banner_size, cwav,
                             (uint32_t)cwav_size, &banner_len, err, errlen);
   free(cwav);
   if (result || cia_set_banner(parsed, copy, banner_len) != CIA_OK)
   {
      if (!result)
         snprintf(err, errlen, "The custom banner sound does not fit the CIA.");
      free(copy);
      return -1;
   }
   free(copy);
   return 0;
}

static int make_icon(const char *path, const entry *e, const char *custom_title,
                     const char *custom_publisher, char *err, int errlen)
{
   static game_art art;
   static u8 app_smdh[FWDCIA_SMDH_SIZE];
   static u8 *smdh;
   char longt[160];
   u8 *cia = NULL, *three_d_bnr = NULL, *template_banner;
   size_t banner_reserved, template_cwav_size, three_d_bnr_size = 0;
   u32 template_cwav_off, three_d_cwav_off;
   long cia_len, bnr_len;
   FILE *f = NULL;
   cia_t parsed;
   u64 tid;
   Result r;
   bool has_app_smdh;

   if (e->sys == SYS_NDS)
      return make_nds_forwarder(path, custom_title, err, errlen);
   if (e->sys == SYS_GBA || e->sys == SYS_SNES)
      return make_rom_icon(path, e, custom_title, custom_publisher, err, errlen);

   make_art(path, e, &art);
   if (custom_title && custom_title[0])
   {
      snprintf(art.title, sizeof(art.title), "%s", custom_title);
      if (art.flat && !art.has_banner_png)
      {
         default_banner(art.title, art.label, 128, 128, art.banner,
                        banner_style == STYLE_CLEAR ? BANNER_CLEAR : 0);
         banner_name(art.title, art.sys, art.banner, 256 - 120 - 8);
      }
      if (!art.flat)
         name_strip(art.title, art.sys, art.name);
   }
   if (custom_publisher)
      snprintf(art.publisher, sizeof(art.publisher), "%s", custom_publisher);
   has_app_smdh = read_app_smdh(path, app_smdh);
   if (strlen(path) >= 256)
   {
      snprintf(err, errlen, "The SD path is too long for a CIA forwarder.");
      return -1;
   }
   tid = title_id_for_path(path, e->sys);
   if (!tid)
   {
      snprintf(err, errlen, "No available HOME Menu title IDs remain.");
      return -1;
   }
   snprintf(longt, sizeof(longt), "%.110s", art.title);

   if (!(f = fopen(CIA_TEMPLATE, "rb")) || fseek(f, 0, SEEK_END) ||
       (cia_len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET))
   {
      if (f) fclose(f);
      snprintf(err, errlen, "Could not read the built-in CIA template.");
      return -1;
   }
   cia = malloc((size_t)cia_len);
   if (!cia || fread(cia, 1, (size_t)cia_len, f) != (size_t)cia_len)
   {
      fclose(f);
      free(cia);
      snprintf(err, errlen, "Could not load the CIA template into memory.");
      return -1;
   }
   fclose(f);
   if (cia_open(&parsed, cia, (size_t)cia_len) != CIA_OK)
   {
      free(cia);
      snprintf(err, errlen, "The built-in CIA template is invalid.");
      return -1;
   }
   smdh = cia_icon_data(&parsed);
   if (!smdh)
   {
      free(cia);
      snprintf(err, errlen, "The CIA template has no HOME Menu icon.");
      return -1;
   }
   smdh_set(smdh, art.title, longt, "", art.icon48);
   if (has_app_smdh)
   {
      int lang;
      for (lang = 0; lang < 16; lang++)
      {
         size_t title_off = 8 + (size_t)lang * 0x200;
         memcpy(smdh + title_off + 0x180, app_smdh + title_off + 0x180, 0x80);
      }
   }
   if (custom_publisher &&
       cia_set_titles(&parsed, art.title, art.publisher) != CIA_OK)
   {
      free(cia);
      snprintf(err, errlen, "Could not set the custom title and publisher.");
      return -1;
   }
   if (cia_set_core_path(&parsed, path) != CIA_OK ||
       cia_set_rom_path(&parsed, "") != CIA_OK)
   {
      free(cia);
      snprintf(err, errlen, "Could not set the selected 3DSX path in the CIA.");
      return -1;
   }
   cia_set_title_id(&parsed, tid);
   cia_set_process_name(&parsed, "3DSXFW");
   cia_set_version(&parsed, 1);
   template_banner = cia_banner_data(&parsed, &banner_reserved);
   if (!template_banner || !banner_reserved)
   {
      free(cia);
      snprintf(err, errlen, "The CIA template has no banner.");
      return -1;
   }
   if (art.flat)
   {
      if (banner_set_images(template_banner, (u32)banner_reserved, art.banner,
                            NULL, err, errlen))
      {
         free(cia);
         return -1;
      }
      if (apply_custom_banner_sound(&parsed, template_banner, banner_reserved,
                                    path, e->pic[1] >= 0, err, errlen))
      {
         free(cia);
         return -1;
      }
   }
   else
   {
      if (!(f = fopen("romfs:/banner3d.bnr", "rb")) || fseek(f, 0, SEEK_END) ||
          (bnr_len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET))
      {
         if (f) fclose(f);
         free(cia);
         snprintf(err, errlen, "Could not read Icon Maker's 3D banner.");
         return -1;
      }
      three_d_bnr = malloc((size_t)bnr_len);
      if (!three_d_bnr || fread(three_d_bnr, 1, (size_t)bnr_len, f) != (size_t)bnr_len)
      {
         fclose(f);
         free(three_d_bnr);
         free(cia);
         snprintf(err, errlen, "Could not load Icon Maker's 3D banner.");
         return -1;
      }
      fclose(f);
      three_d_bnr_size = (size_t)bnr_len;
      if (banner_set_images(three_d_bnr, (u32)three_d_bnr_size, art.name,
                            art.label, err, errlen))
      {
         free(three_d_bnr);
         free(cia);
         return -1;
      }
      template_cwav_off = template_banner[0x84] | (u32)template_banner[0x85] << 8 |
                          (u32)template_banner[0x86] << 16 | (u32)template_banner[0x87] << 24;
      three_d_cwav_off = three_d_bnr[0x84] | (u32)three_d_bnr[0x85] << 8 |
                         (u32)three_d_bnr[0x86] << 16 | (u32)three_d_bnr[0x87] << 24;
      if (template_cwav_off > banner_reserved || banner_reserved - template_cwav_off < 16 ||
          three_d_cwav_off > three_d_bnr_size || three_d_bnr_size - three_d_cwav_off < 16)
      {
         free(three_d_bnr);
         free(cia);
         snprintf(err, errlen, "The CIA template's banner audio is invalid.");
         return -1;
      }
      template_cwav_size = template_banner[template_cwav_off + 0x0C] |
                           (u32)template_banner[template_cwav_off + 0x0D] << 8 |
                           (u32)template_banner[template_cwav_off + 0x0E] << 16 |
                           (u32)template_banner[template_cwav_off + 0x0F] << 24;
      if (template_cwav_size > banner_reserved - template_cwav_off ||
          template_cwav_size > banner_reserved - three_d_cwav_off)
      {
         free(three_d_bnr);
         free(cia);
         snprintf(err, errlen, "The 3D banner audio does not fit the CIA template.");
         return -1;
      }
      memcpy(three_d_bnr + three_d_cwav_off, template_banner + template_cwav_off,
             template_cwav_size);
      if (cia_set_banner(&parsed, three_d_bnr, three_d_cwav_off + template_cwav_size) != CIA_OK)
      {
         free(three_d_bnr);
         free(cia);
         snprintf(err, errlen, "The 3D Icon Maker banner does not fit the CIA.");
         return -1;
      }
      free(three_d_bnr);
   }
   cia_reseal(&parsed);

   if (!record_title_id(path, tid))
   {
      free(cia);
      snprintf(err, errlen, "Could not save the title-ID record in %s.", APP_DIR);
      return -1;
   }
   r = install_cia_buf(cia, (u32)cia_len);
   if (R_FAILED(r) && R_DESCRIPTION(r) == RD_ALREADY_EXISTS)
   {
      AM_DeleteTitle(MEDIATYPE_SD, tid);
      r = install_cia_buf(cia, (u32)cia_len);
   }
   free(cia);
   if (R_FAILED(r))
   {
      snprintf(err, errlen, "Install failed (%08lX).\nIt needs custom firmware with\nsignature patches (Luma3DS).",
               (unsigned long)r);
      return -1;
   }
   return 0;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static inline u16 rgb565(int r, int g, int b)
{
   return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* an RGBA picture onto the canvas, over what's there, scale 1 or 2 */
static void blit_rgba(const u8 *img, int w, int h, int x0, int y0, int scale)
{
   int x, y;
   for (y = 0; y < h * scale; y++)
      for (x = 0; x < w * scale; x++)
      {
         const u8 *s = img + ((y / scale) * w + x / scale) * 4;
         int cx = x0 + x, cy = y0 + y;
         u16 *d;
         if ((unsigned)cx >= UI_W || (unsigned)cy >= UI_H || !s[3])
            continue;
         d = &ui_canvas[cy * UI_W + cx];
         if (s[3] == 255)
            *d = rgb565(s[0], s[1], s[2]);
         else
         {
            int r = ((*d >> 11) << 3), g = (((*d >> 5) & 63) << 2), b = ((*d & 31) << 3);
            *d = rgb565((s[0] * s[3] + r * (255 - s[3])) / 255,
                        (s[1] * s[3] + g * (255 - s[3])) / 255,
                        (s[2] * s[3] + b * (255 - s[3])) / 255);
         }
      }
}

static void blit_565(const u16 *img, int w, int h, int x0, int y0, int scale)
{
   int x, y;
   for (y = 0; y < h * scale; y++)
      for (x = 0; x < w * scale; x++)
      {
         int cx = x0 + x, cy = y0 + y;
         if ((unsigned)cx < UI_W && (unsigned)cy < UI_H)
            ui_canvas[cy * UI_W + cx] = img[(y / scale) * w + x / scale];
      }
}

/* what the top screen shows, so the spinner can be drawn over it */
static u16 top_shown[UI_W * UI_H];
static u8 top_left_icon[48 * 48 * 4];
static int top_left_icon_y;
static bool top_left_icon_active;

/* the canvas to the top screen (400 wide: the canvas in the middle, its
 * edge colours carried out to the sides) */
static void present_top_canvas(void)
{
   u16 *fb = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
   int x, y;
   for (x = 0; x < 400; x++)
   {
      int cx = x - 40;
      u16 *col = fb + x * 240 + 239;
      if (cx < 0) cx = 0;
      if (cx >= UI_W) cx = UI_W - 1;
      for (y = 0; y < 240; y++)
         *col-- = ui_canvas[y * UI_W + cx];
   }
   if (top_left_icon_active)
   {
      int ix, iy;
      for (iy = 0; iy < 56; iy++)
         for (ix = 0; ix < 56; ix++)
         {
            int px = ix < 10 ? 10 : (ix > 45 ? 45 : ix);
            int py = iy < 10 ? 10 : (iy > 45 ? 45 : iy);
            int dx = ix - px, dy = iy - py;
            u16 color = C_PANEL;
            if ((ix < 10 || ix > 45) && (iy < 10 || iy > 45) && dx * dx + dy * dy > 100)
               continue;
            if (ix >= 4 && ix < 52 && iy >= 4 && iy < 52)
            {
               const u8 *src = top_left_icon + (((iy - 4) * 48 + ix - 4) * 4);
               if (src[3])
               {
                  int r = ((color >> 11) & 31) * 255 / 31;
                  int g = ((color >> 5) & 63) * 255 / 63;
                  int b = (color & 31) * 255 / 31;
                  r = (src[0] * src[3] + r * (255 - src[3])) / 255;
                  g = (src[1] * src[3] + g * (255 - src[3])) / 255;
                  b = (src[2] * src[3] + b * (255 - src[3])) / 255;
                  color = rgb565(r, g, b);
               }
            }
            fb[(ix + 8) * 240 + 239 - (top_left_icon_y + iy)] = color;
         }
   }
   GSPGPU_FlushDataCache(fb, 400 * 240 * 2);
   gfxScreenSwapBuffers(GFX_TOP, false);
}

/* the same, kept as what the top screen shows */
static void present_top(void)
{
   memcpy(top_shown, ui_canvas, sizeof(top_shown));
   present_top_canvas();
}

/* only the canvas's w x h at (x0, y0) to the top screen: the rest of both
 * of its framebuffers must already show the same */
static void present_top_rect(int x0, int y0, int w, int h)
{
   u16 *fb = (u16*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
   int x, y;
   for (x = x0; x < x0 + w; x++)
   {
      u16 *col = fb + (x + 40) * 240 + 239 - y0;
      for (y = y0; y < y0 + h; y++)
         *col-- = ui_canvas[y * UI_W + x];
   }
   GSPGPU_FlushDataCache(fb + (x0 + 40) * 240, w * 240 * 2);
   gfxScreenSwapBuffers(GFX_TOP, false);
}

/* a small SNES cartridge (wide, ridged sides) */
static void icon_snes(int x, int y, u16 c)
{
   ui_rrect(x, y + 1, 17, 14, 3, c);
   ui_fill(x + 4, y + 3, 9, 6, C_BG);
   ui_fill(x + 1, y + 11, 2, 1, C_BG);
   ui_fill(x + 14, y + 11, 2, 1, C_BG);
   ui_fill(x + 1, y + 13, 2, 1, C_BG);
   ui_fill(x + 14, y + 13, 2, 1, C_BG);
}

static void icon_folder(int x, int y, u16 c)
{
   ui_rrect(x, y + 2, 7, 4, 1, c);
   ui_rrect(x, y + 4, 16, 11, 2, c);
}

static void icon_cart(int x, int y, u16 c)
{
   ui_rrect(x + 1, y, 14, 15, 2, c);
   ui_fill(x + 4, y + 3, 8, 5, C_BG);
   ui_fill(x + 3, y + 11, 10, 1, C_BG);
}

/* text with \n, one line every dy; returns the y after it */
static int text_lines(const ui_font *f, int x, int y, int dy, const char *s, u16 c, int max_w)
{
   char line[128];
   while (s && *s)
   {
      const char *nl = strchr(s, '\n');
      int n = nl ? (int)(nl - s) : (int)strlen(s);
      if (n > 127) n = 127;
      memcpy(line, s, n);
      line[n] = 0;
      ui_text(f, x, y, line, c, max_w);
      y += dy;
      s += n + (nl ? 1 : 0);
      if (!nl) break;
   }
   return y;
}

static void draw_header(void)
{
   ui_vgradient(0, 0, UI_W, TAB_Y, C_BG2, C_BG);
   ui_text_center(&ui_font_title, UI_W / 2, 3, "MULTI-FORWARDER", C_TEXT);
   ui_text_right(&ui_font_small, UI_W - 10, 10, "v" ICONMAKER_VERSION, C_DIM);
}

static void tab_layout(int x[3], int w[3])
{
   int i, gap = 8, total = gap * 2;
   for (i = 0; i < 3; i++)
   {
      w[i] = ui_text_w(&ui_font_body, tab_names[i]) + 24;
      total += w[i];
   }
   x[0] = (UI_W - total) / 2;
   for (i = 1; i < 3; i++)
      x[i] = x[i - 1] + w[i - 1] + gap;
}

static void draw_tabs(int tab)
{
   int i, x[3], w[3];
   tab_layout(x, w);
   for (i = 0; i < 3; i++)
   {
      int on = i == tab;
      ui_rrect(x[i], TAB_Y, w[i], TAB_H, 10,
               on ? C_ROW_SEL : UI_RGB(0x1C, 0x20, 0x29));
      if (on)
         ui_fill(x[i] + w[i] / 2 - 14, TAB_Y + TAB_H - 3, 28, 3, C_ACCENT);
      ui_text_vc(&ui_font_body, x[i] + (w[i] - ui_text_w(&ui_font_body, tab_names[i])) / 2,
                 TAB_Y, TAB_H - 2, tab_names[i], on ? C_TEXT : C_DIM, 0);
   }
}

static void draw_control_hints_at(const char *keys, int max_width, int y, int height)
{
   const char *p = keys;
   int x = 10;
   while (*p && x < 10 + max_width)
   {
      const char *colon = strchr(p, ':');
      const char *next;
      char button[32], detail[128];
      size_t button_len, detail_len;
      int button_width, detail_width, filter_width = 0, filter_tail_width = 0;
      int colon_width;
      bool highlight_filter;
      if (!colon)
      {
         ui_text_vc(&ui_font_small, x, y, height, p, C_DIM, 0);
         break;
      }
      next = strstr(colon + 1, "   ");
      button_len = (size_t)(colon - p);
      if (button_len >= sizeof(button))
         button_len = sizeof(button) - 1;
      memcpy(button, p, button_len);
      button[button_len] = 0;
      detail_len = next ? (size_t)(next - colon - 1) : strlen(colon + 1);
      if (detail_len >= sizeof(detail))
         detail_len = sizeof(detail) - 1;
      memcpy(detail, colon + 1, detail_len);
      detail[detail_len] = 0;
      highlight_filter = !strcmp(button, "Y") && detail_len >= 7 &&
                        !strncmp(detail, " Filter", 7);
      button_width = ui_text_w(&ui_font_bold, button);
      if (highlight_filter)
      {
         filter_width = ui_text_w(&ui_font_bold, ": Filter");
         filter_tail_width = ui_text_w(&ui_font_small, detail + 7);
         detail_width = filter_width + filter_tail_width;
         colon_width = 0;
      }
      else
      {
         detail_width = ui_text_w(&ui_font_small, detail);
         colon_width = ui_text_w(&ui_font_small, ":");
      }
      if (x + button_width + colon_width + detail_width > 10 + max_width)
         break;
      ui_text_vc(&ui_font_bold, x, y, height, button, C_TEXT, 0);
      x += button_width;
      if (highlight_filter)
      {
         ui_text_vc(&ui_font_bold, x, y, height, ": Filter", C_TEXT, 0);
         x += filter_width;
         if (detail[7])
            ui_text_vc(&ui_font_small, x, y, height, detail + 7, C_DIM, 0);
      }
      else
      {
         ui_text_vc(&ui_font_small, x, y, height, ":", C_TEXT, 0);
         x += colon_width;
         ui_text_vc(&ui_font_small, x, y, height, detail, C_DIM, 0);
      }
      x += detail_width + 12;
      if (!next)
         break;
      p = next + 3;
   }
}

static void draw_control_hints(const char *keys, int max_width)
{
   draw_control_hints_at(keys, max_width, FOOT_Y, UI_H - FOOT_Y);
}

static void draw_right_control_hint(const char *button, const char *detail,
                                    int right, int y, int height)
{
   int button_width = ui_text_w(&ui_font_bold, button);
   int colon_width = ui_text_w(&ui_font_small, ":");
   int detail_width = ui_text_w(&ui_font_small, detail);
   int x = right - button_width - colon_width - detail_width;
   ui_text_vc(&ui_font_bold, x, y, height, button, C_TEXT, 0);
   x += button_width;
   ui_text_vc(&ui_font_small, x, y, height, ":", C_TEXT, 0);
   x += colon_width;
   ui_text_vc(&ui_font_small, x, y, height, detail, C_DIM, 0);
}

static void draw_footer(const char *keys)
{
   ui_fill(0, FOOT_Y, UI_W, UI_H - FOOT_Y, C_BG2);
   ui_fill(0, FOOT_Y, UI_W, 1, C_PANEL);
   draw_control_hints(keys, UI_W - 20);
}

static void draw_installed_footer(int count)
{
   char total[16];
   char filter_hint[64];
   const char *filter_name =
      installed_filter == FILTER_ALL ? "ALL" :
      installed_filter == FILTER_3DSX ? "3DSX" :
      installed_filter == FILTER_GBA ? "GBA" : "SNES";

   ui_fill(0, FOOT_Y, UI_W, UI_H - FOOT_Y, C_BG2);
   ui_fill(0, FOOT_Y, UI_W, 1, C_PANEL);
   snprintf(total, sizeof(total), "(%d)", count);
   snprintf(filter_hint, sizeof(filter_hint), "Y: Filter - %s", filter_name);
   draw_control_hints_at(filter_hint, UI_W - 110, TAB_Y + TAB_H,
                         LIST_Y - TAB_Y - TAB_H);
   draw_control_hints("L / R: Switch Tabs", UI_W - 52);
   ui_text_right(&ui_font_small, UI_W - 10, FOOT_Y + 5, total, C_DIM);
   if (count)
      draw_right_control_hint("X", "Uninstall", UI_W - 8, TAB_Y + TAB_H,
                              LIST_Y - TAB_Y - TAB_H);
}

static void draw_twl_footer(bool can_uninstall)
{
   char total[24];
   const char *keys = "L / R: Switch Tabs";
   snprintf(total, sizeof(total), twl_installed_count == 0xFFFFFFFF
            ? "(?/40)" : "(%lu/40)", (unsigned long)twl_installed_count);
   ui_fill(0, FOOT_Y, UI_W, UI_H - FOOT_Y, C_BG2);
   ui_fill(0, FOOT_Y, UI_W, 1, C_PANEL);
   draw_control_hints(keys, UI_W - 64);
   ui_text_right(&ui_font_small, UI_W - 10, FOOT_Y + 5, total, C_DIM);
   if (can_uninstall)
      draw_right_control_hint("X", "Uninstall", UI_W - 8, TAB_Y + TAB_H,
                              LIST_Y - TAB_Y - TAB_H);
}

static void draw_scrollbar(int n, int top)
{
   if (n > ROWS)
   {
      int track = ROWS * ROW_H - 4;
      int bar = track * ROWS / n;
      if (bar < 12) bar = 12;
      ui_rrect(UI_W - 4, LIST_Y + (track - bar) * top / (n - ROWS), 3, bar, 1, C_FAINT);
   }
}

static void draw_bottom(int tab, int sel, int top)
{
   int i;
   char keys[128];
   ui_clear(C_BG);
   draw_header();
   draw_tabs(tab);
   if (tab == TAB_GAMES)
   {
      char info[64];
      static const char *const style_short[STYLE_COUNT] = { "blue", "no background", "3D" };
      int iw, select_x, select_w;
      snprintf(info, sizeof(info), "%s",
               entries[sel].sys == SYS_NDS
                  ? nds_custom_icon ? "Custom" : "Default"
                  : entries[sel].pic[1] >= 0 && banner_cycle == 0
                     ? "custom" : style_short[banner_style]);
      iw = ui_text_w(&ui_font_small, info);
      select_w = ui_text_w(&ui_font_small, "SELECT:");
      select_x = UI_W - 10 - iw - 8 - select_w;
      ui_text(&ui_font_small, 10, LIST_Y - 18, cwd + 5, C_FAINT,
              select_x > 18 ? select_x - 18 : 0);
      ui_text(&ui_font_small, select_x, LIST_Y - 18, "SELECT:", C_TEXT, select_w);
      ui_text_right(&ui_font_small, UI_W - 10, LIST_Y - 18, info, C_DIM);
      if (!nentries)
         ui_text_center(&ui_font_body, UI_W / 2, 130, "No folders or supported files here", C_DIM);
      for (i = 0; i < ROWS && top + i < nentries; i++)
      {
         const entry *e = &entries[top + i];
         int y = LIST_Y + i * ROW_H, h = ROW_H - 2, s = top + i == sel, right = ROW_W - 50;
         if (s)
         {
            ui_rrect(ROW_X, y, ROW_W, h, 7, C_ROW_SEL);
            ui_fill(ROW_X, y + 5, 3, h - 10, C_ACCENT);
         }
         if (e->dir)
            icon_folder(ROW_X + 10, y + 3, s ? C_WARN : UI_RGB(0xB0, 0x8A, 0x45));
         else if (e->sys == SYS_SNES)
            icon_snes(ROW_X + 9, y + 3, s ? C_ACCENT2 : C_FAINT);
         else
            icon_cart(ROW_X + 10, y + 3, s ? C_ACCENT : C_FAINT);
         if (e->has_icon)
         {
            /* a green installed pill */
            int pw = ui_text_w(&ui_font_small, "INSTALLED") + 14;
            ui_rrect(ROW_X + ROW_W - pw - 8, y + 4, pw, h - 8, (h - 8) / 2, UI_RGB(0x1E, 0x3B, 0x2E));
            ui_text_vc(&ui_font_small, ROW_X + ROW_W - pw - 1, y + 4, h - 8, "INSTALLED", C_GOOD, 0);
            right = ROW_W - pw - 50;
         }
         ui_text_vc(&ui_font_body, ROW_X + 34, y, h, strcmp(e->name, "..") ? e->name : ".. (up a folder)",
                    s ? C_TEXT : C_TEXT2, right);
      }
      draw_scrollbar(nentries, top);
      if (strcmp(cwd, "sdmc:/"))
         snprintf(keys, sizeof(keys), !nentries || entries[sel].dir
                  ? "L / R: Switch Tabs   B: Back   A: Open"
                  : "L / R: Switch Tabs   B: Back   A: Make Forwarder");
      else
         snprintf(keys, sizeof(keys), !nentries || entries[sel].dir
                  ? "L / R: Switch Tabs   A: Open"
                  : "L / R: Switch Tabs   A: Make Forwarder");
      draw_footer(keys);
   }
   else if (tab == TAB_ICONS)
   {
      if (!ninstalled_icons)
      {
         ui_text_center(&ui_font_body, UI_W / 2, 120, "No icons yet", C_DIM);
         ui_text_center(&ui_font_small, UI_W / 2, 142, "Pick an app in Make Forwarder and press A", C_FAINT);
      }
      for (i = 0; i < ROWS && top + i < ninstalled_icons; i++)
      {
         const icon_entry *ic = &icons[installed_icon_indices[top + i]];
         int y = LIST_Y + i * ROW_H, h = ROW_H - 2, s = top + i == sel;
         if (s)
         {
            ui_rrect(ROW_X, y, ROW_W, h, 7, C_ROW_SEL);
            ui_fill(ROW_X, y + 5, 3, h - 10, C_ACCENT);
         }
         {
            int px, py;
            for (py = 0; py < 19; py++)
               for (px = 0; px < 19; px++)
                  ui_canvas[(y + 1 + py) * UI_W + ROW_X + 9 + px] =
                     ic->icon48[(py * 48 / 19) * 48 + px * 48 / 19];
         }
         {
            char single_line[sizeof(ic->name)];
            int j;
            for (j = 0; ic->name[j] && j < (int)sizeof(single_line) - 1; j++)
               single_line[j] = ic->name[j] == '\n' || ic->name[j] == '\r' ? ' ' : ic->name[j];
            single_line[j] = 0;
            ui_text_vc(&ui_font_body, ROW_X + 36, y, h, single_line,
                       s ? C_TEXT : C_TEXT2, ROW_W - 90);
         }
         ui_text_right(&ui_font_small, ROW_X + ROW_W - 10, y + (h - ui_font_small.ascent) / 2 - 1,
                       systems[ic->sys].console,
                       ic->sys == SYS_SNES ? C_ACCENT2 : C_ACCENT);
      }
      draw_scrollbar(ninstalled_icons, top);
      draw_installed_footer(ninstalled_icons);
   }
   else
   {
      if (!ntwl_icons)
      {
         ui_text_center(&ui_font_body, UI_W / 2, 120, "No TWL titles found", C_DIM);
         ui_text_center(&ui_font_small, UI_W / 2, 142, "Make an NDS forwarder from the first tab", C_FAINT);
      }
      for (i = 0; i < ROWS && top + i < ntwl_icons; i++)
      {
         const twl_icon_entry *ic = &twl_icons[top + i];
         int y = LIST_Y + i * ROW_H, h = ROW_H - 2, s = top + i == sel;
         if (s)
         {
            ui_rrect(ROW_X, y, ROW_W, h, 7, C_ROW_SEL);
            ui_fill(ROW_X, y + 5, 3, h - 10, C_ACCENT);
         }
         draw_twl_icon_scaled(ic, ROW_X + 9, y + 1, 19);
         ui_text_vc(&ui_font_body, ROW_X + 36, y, h, ic->name,
                    s ? C_TEXT : C_TEXT2, ROW_W - 90);
         ui_text_right(&ui_font_small, ROW_X + ROW_W - 10,
                       y + (h - ui_font_small.ascent) / 2 - 1,
                       ic->is_nds_forwarder ? "NDS" : "TWL",
                       ic->is_nds_forwarder ? C_ACCENT : C_DIM);
      }
      draw_scrollbar(ntwl_icons, top);
      draw_twl_footer(ntwl_icons && twl_icons[sel].is_nds_forwarder);
   }
}

/* ---- top screen: what the HOME Menu will show ---- */

static void top_bg(void)
{
   top_left_icon_active = false;
   ui_vgradient(0, 0, UI_W, UI_H, C_BG2, C_BG);
}

static void draw_top_game_metadata(const game_art *a, int icon_x, int icon_y,
                                   int text_x)
{
   char first[64], second[64];
   int text_y_next, title_lines = 0, block_height, max_w = UI_W - text_x - 8;
   if (icon_x < 0)
   {
      memcpy(top_left_icon, a->icon48, sizeof(top_left_icon));
      top_left_icon_y = icon_y;
      top_left_icon_active = true;
   }
   else
   {
      ui_rrect(icon_x, icon_y, 56, 56, 10, C_PANEL);
      blit_rgba(a->icon48, 48, 48, icon_x + 4, icon_y + 4, 1);
   }
   if (a->title[0])
   {
      wrap_text_two_lines(&ui_font_body, a->title, max_w,
                          first, sizeof(first), second, sizeof(second));
      title_lines = second[0] ? 2 : 1;
   }
   block_height = title_lines * 18 + (a->publisher[0] ? 16 : 0);
   text_y_next = icon_y + (56 - block_height) / 2;
   if (title_lines)
   {
      ui_text(&ui_font_body, text_x, text_y_next, first, C_TEXT, max_w);
      text_y_next += 18;
      if (second[0])
      {
         ui_text(&ui_font_body, text_x, text_y_next, second, C_TEXT, max_w);
         text_y_next += 18;
      }
   }
   if (a->publisher[0])
      ui_text(&ui_font_small, text_x, text_y_next, a->publisher, C_DIM, max_w);
}

static void draw_top_game(const game_art *a)
{
   top_bg();
   if (!a->flat)
   {
      char first[64], second[64];
      ui_rrect(42, 0, 236, 168, 7, C_BG);
      ui_rrect_outline(42, 0, 236, 168, 7, 1, C_FAINT);
      ui_rrect(104, 0, 112, 108, 16, C_PANEL);
      if (a->sys == SYS_GBA || a->sys == SYS_SNES)
         blit_rgba(a->preview_icon, 96, 96, 112, 4, 1);
      else
         blit_rgba(a->icon48, 48, 48, 112, 4, 2);
      wrap_text_two_lines(&ui_font_title, a->title, 216,
                          first, sizeof(first), second, sizeof(second));
      ui_text_center(&ui_font_title, UI_W / 2, 108, first, C_TEXT);
      if (second[0])
         ui_text_center(&ui_font_title, UI_W / 2, 127, second, C_TEXT);
      ui_text_center(&ui_font_small, UI_W / 2, 151, "Homebrew  ·  3DSX", C_ACCENT);
   }
   else
   {
      ui_rrect(30, 14, 260, 132, 5, C_BG);
      blit_rgba(a->banner, 256, 128, 32, 16, 1);
      ui_rrect_outline(30, 14, 260, 132, 5, 1, C_FAINT);
   }
   draw_top_game_metadata(a, 56, 170, 112);
}

static void draw_top_folder(const char *name, bool up)
{
   top_bg();
   ui_rrect(110, 40, 100, 70, 12, C_PANEL);
   ui_rrect(130, 58, 26, 12, 3, UI_RGB(0xB0, 0x8A, 0x45));
   ui_rrect(130, 64, 60, 36, 5, C_WARN);
   ui_text_center(&ui_font_bold, UI_W / 2, 124, up ? "Up a folder" : name, C_TEXT);
   if (!up)
      ui_text_center(&ui_font_small, UI_W / 2, 148, "A: open the folder", C_DIM);
}

static void draw_top_intro(void)
{
   top_bg();
   ui_text_center(&ui_font_title, UI_W / 2, 40, "MULTI-FORWARDER", C_TEXT);
   ui_text_center(&ui_font_small, UI_W / 2, 68, "Forward 3DSX, GBA, SNES and NDS", C_DIM);
   text_lines(&ui_font_small, 40, 100, 16,
              "3DSX: launches through Homebrew Launcher.\n"
              "GBA: gpSP 3D. SNES: Snes9x 3D.\n"
              "NDS: installs a TWL forwarder to NAND.\n"
              "Custom artwork sits beside the game.\n"
              "SELECT cycles banner styles for 3DSX,\n"
              "GBA and SNES forwarders.", C_TEXT2, UI_W - 60);
}

static void draw_top_icon(const icon_entry *ic)
{
   char first[64], second[64], tid[40];
   int label_y, tid_y;
   top_bg();
   ui_rrect(108, 30, 104, 104, 14, C_PANEL);
   blit_565(ic->icon48, 48, 48, 112, 34, 2);
   wrap_text_two_lines(&ui_font_bold, ic->name, 280,
                       first, sizeof(first), second, sizeof(second));
   ui_text_center(&ui_font_bold, UI_W / 2, 144, first, C_TEXT);
   if (second[0])
      ui_text_center(&ui_font_bold, UI_W / 2, 162, second, C_TEXT);
   label_y = second[0] ? 182 : 166;
   tid_y = second[0] ? 218 : 202;
   if (ic->sys == SYS_3DSX)
      ui_text_center(&ui_font_small, UI_W / 2, label_y,
                     "3DSX Homebrew Launcher", C_DIM);
   else if (ic->publisher[0])
      ui_text_center(&ui_font_small, UI_W / 2, label_y, ic->publisher, C_DIM);
   snprintf(tid, sizeof(tid), "Title ID: %016llX", (unsigned long long)ic->tid);
   ui_text_center(&ui_font_small, UI_W / 2, tid_y, tid, C_DIM);
}

static void draw_top_twl_icon(const twl_icon_entry *ic)
{
   char tid[40], first[64], second[64];
   int label_y, tid_y;
   top_bg();
   ui_rrect(108, 30, 104, 104, 14, C_PANEL);
   if (ic->has_icon)
      draw_twl_icon_scaled(ic, 112, 34, 96);
   wrap_text_two_lines(&ui_font_bold, ic->name, 280,
                       first, sizeof(first), second, sizeof(second));
   ui_text_center(&ui_font_bold, UI_W / 2, 144, first, C_TEXT);
   if (second[0])
      ui_text_center(&ui_font_bold, UI_W / 2, 160, second, C_TEXT);
   label_y = second[0] ? 182 : 166;
   tid_y = second[0] ? 218 : 202;
   ui_text_center(&ui_font_small, UI_W / 2, label_y,
                  ic->is_nds_forwarder ? "Twilight Menu" : "DSiWare",
                  C_DIM);
   snprintf(tid, sizeof(tid), "Title ID: %016llX", (unsigned long long)ic->tid);
   ui_text_center(&ui_font_small, UI_W / 2, tid_y, tid, C_DIM);
}

static void draw_top_nds_preview(const char *path, bool use_custom_icon)
{
   twl_icon_entry preview;
   char first[64], second[64];
   memset(&preview, 0, sizeof(preview));
   preview.tid = nds_title_id_for_path(path);
   preview.is_nds_forwarder = true;
   if (!read_nds_banner_icon(path, &preview, use_custom_icon))
      snprintf(preview.name, sizeof(preview.name), "NDS game");
   top_bg();
   ui_rrect(108, 30, 104, 104, 14, C_PANEL);
   if (preview.has_icon)
      draw_twl_icon_scaled(&preview, 112, 34, 96);
   wrap_text_two_lines(&ui_font_bold, preview.name, 280,
                       first, sizeof(first), second, sizeof(second));
   ui_text_center(&ui_font_bold, UI_W / 2, 144, first, C_TEXT);
   if (second[0])
      ui_text_center(&ui_font_bold, UI_W / 2, 162, second, C_TEXT);
}

/* ---- dialogs on the bottom screen ---- */

static bool touched(u32 k, touchPosition *t)
{
   if (!(k & KEY_TOUCH))
      return false;
   hidTouchRead(t);
   return true;
}

static void draw_footer(const char *keys);

/* A card over the bottom screen; returns true for OK and false for Cancel. */
static bool dialog_with_default(const char *title, const char *body,
                                const char *ok, const char *cancel,
                                u16 ok_color, bool default_cancel)
{
   int lines = 1, h, y0, bw = cancel ? 128 : 160;
   int selected = cancel && default_cancel ? 0 : 1;
   const char *p;
   for (p = body; p && *p; p++)
      if (*p == '\n') lines++;
   h = 92 + lines * 16;
   y0 = (UI_H - h) / 2;

   ui_fill_alpha(0, 0, UI_W, UI_H, 0, 170);
   ui_rrect(14, y0, UI_W - 28, h, 14, C_PANEL);
   ui_text(&ui_font_bold, 30, y0 + 12, title, C_TEXT, UI_W - 60);
   text_lines(&ui_font_small, 30, y0 + 38, 16, body, C_TEXT2, UI_W - 60);
   if (cancel)
   {
      ui_rrect(30, y0 + h - 40, bw, 28, 14, selected == 0 ? ok_color : C_ROW_SEL);
      ui_text_center(selected == 0 ? &ui_font_bold : &ui_font_body,
                     30 + bw / 2, y0 + h - 36, cancel,
                     selected == 0 ? C_TEXT : C_TEXT2);
      ui_rrect(UI_W - 30 - bw, y0 + h - 40, bw, 28, 14, selected == 1 ? ok_color : C_ROW_SEL);
      ui_text_center(selected == 1 ? &ui_font_bold : &ui_font_body,
                     UI_W - 30 - bw / 2, y0 + h - 37, ok,
                     selected == 1 ? C_TEXT : C_TEXT2);
   }
   else
   {
      ui_rrect((UI_W - bw) / 2, y0 + h - 40, bw, 28, 14, ok_color);
      ui_text_center(&ui_font_bold, UI_W / 2, y0 + h - 37, ok, C_TEXT);
   }
   if (cancel)
      draw_footer("A: Proceed   B: Cancel");
   ui_present();

   while (aptMainLoop())
   {
      touchPosition t;
      u32 k;
      gspWaitForVBlank();
      hidScanInput();
      k = hidKeysDown();
      if (cancel && (k & (KEY_DLEFT | KEY_DRIGHT | KEY_DUP | KEY_DDOWN)))
      {
         selected = !selected;
         ui_rrect(30, y0 + h - 40, bw, 28, 14, selected == 0 ? ok_color : C_ROW_SEL);
         ui_text_center(selected == 0 ? &ui_font_bold : &ui_font_body,
                        30 + bw / 2, y0 + h - 36, cancel, selected == 0 ? C_TEXT : C_TEXT2);
         ui_rrect(UI_W - 30 - bw, y0 + h - 40, bw, 28, 14, selected == 1 ? ok_color : C_ROW_SEL);
         ui_text_center(selected == 1 ? &ui_font_bold : &ui_font_body,
                        UI_W - 30 - bw / 2, y0 + h - 37, ok, selected == 1 ? C_TEXT : C_TEXT2);
         draw_footer("A: Proceed   B: Cancel");
         ui_present();
      }
      if (k & KEY_A) return cancel ? selected == 1 : true;
      if (k & KEY_B) return cancel ? false : true;
      if (touched(k, &t) && t.py >= y0 + h - 40 && t.py < y0 + h - 12)
      {
         if (!cancel && t.px >= (UI_W - bw) / 2 && t.px < (UI_W + bw) / 2) return true;
         if (cancel && t.px >= UI_W - 30 - bw && t.px < UI_W - 30) return true;
         if (cancel && t.px >= 30 && t.px < 30 + bw) return false;
      }
   }
   return false;
}

static bool same_file_contents(const char *a, const char *b)
{
   FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
   unsigned char ba[4096], bb[4096];
   bool same = false;

   if (!fa || !fb)
      goto done;
   for (;;)
   {
      size_t na = fread(ba, 1, sizeof(ba), fa);
      size_t nb = fread(bb, 1, sizeof(bb), fb);
      if (na != nb || memcmp(ba, bb, na))
         break;
      if (na < sizeof(ba))
      {
         same = !ferror(fa) && !ferror(fb);
         break;
      }
   }
done:
   if (fa) fclose(fa);
   if (fb) fclose(fb);
   return same;
}

static bool title_id_path_exists(const char *file, const char *path)
{
   char line[320];
   FILE *f = fopen(file, "r");
   bool found = false;
   if (!f)
      return false;
   while (fgets(line, sizeof(line), f))
   {
      char *tab = strrchr(line, '\t');
      if (tab)
      {
         *tab = 0;
         if (!strcmp(line, path))
         {
            found = true;
            break;
         }
      }
   }
   fclose(f);
   return found;
}

static bool merge_title_id_file(const char *src, const char *dst)
{
   char line[320], path[320];
   FILE *in = fopen(src, "r"), *out = fopen(dst, "a");
   bool ok = in != NULL && out != NULL;
   if (!ok)
   {
      if (in) fclose(in);
      if (out) fclose(out);
      return false;
   }
   while (fgets(line, sizeof(line), in))
   {
      char *tab;
      snprintf(path, sizeof(path), "%s", line);
      tab = strrchr(path, '\t');
      if (tab)
      {
         *tab = 0;
         if (title_id_path_exists(dst, path))
            continue;
      }
      if (fputs(line, out) == EOF || fflush(out))
      {
         ok = false;
         break;
      }
   }
   if (ferror(in))
      ok = false;
   if (fclose(in))
      ok = false;
   if (fclose(out))
      ok = false;
   return ok;
}

static bool make_directory(const char *path)
{
   struct stat st;
   if (!mkdir(path, 0777))
      return true;
   return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static bool copy_import_file_new(const char *src, const char *dst)
{
   char tmp[900];
   FILE *in = NULL, *out = NULL;
   unsigned char buf[8192];
   size_t n;
   bool ok = false;

   if (snprintf(tmp, sizeof(tmp), "%s.importing", dst) >= (int)sizeof(tmp))
      return false;
   remove(tmp);
   in = fopen(src, "rb");
   out = fopen(tmp, "wb");
   if (!in || !out)
      goto done;
   while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
      if (fwrite(buf, 1, n, out) != n)
         goto done;
   if (ferror(in) || fflush(out) || ferror(out))
      goto done;
   if (fclose(out))
   {
      out = NULL;
      goto done;
   }
   out = NULL;
   if (!fclose(in))
   {
      in = NULL;
      if (same_file_contents(src, tmp) && !rename(tmp, dst))
         ok = true;
   }
done:
   if (in) fclose(in);
   if (out) fclose(out);
   if (!ok) remove(tmp);
   return ok;
}

static bool copy_import_file(const char *src, const char *dst)
{
   char backup[900];
   int suffix;
   if (access(dst, F_OK))
      return copy_import_file_new(src, dst);
   if (same_file_contents(src, dst))
      return true;
   if (!strcmp(dst, TITLE_IDS))
      return merge_title_id_file(src, dst);
   for (suffix = 0; suffix < 100; suffix++)
   {
      int n = suffix == 0
         ? snprintf(backup, sizeof(backup), "%s.legacy", dst)
         : snprintf(backup, sizeof(backup), "%s.legacy.%d", dst, suffix);
      if (n < 0 || n >= (int)sizeof(backup))
         return false;
      if (access(backup, F_OK))
         return copy_import_file_new(src, backup);
      if (same_file_contents(src, backup))
         return true;
   }
   return false;
}

static bool copy_import_tree(const char *src, const char *dst)
{
   DIR *dir;
   struct dirent *entry;
   bool ok = true;

   if (!make_directory(dst))
      return false;
   dir = opendir(src);
   if (!dir)
      return false;
   while ((entry = readdir(dir)))
   {
      char from[900], to[900];
      struct stat st;

      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         continue;
      if (snprintf(from, sizeof(from), "%s/%s", src, entry->d_name) >= (int)sizeof(from) ||
          snprintf(to, sizeof(to), "%s/%s", dst, entry->d_name) >= (int)sizeof(to) ||
          stat(from, &st))
      {
         ok = false;
         break;
      }
      if (S_ISDIR(st.st_mode))
         ok = copy_import_tree(from, to);
      else
      {
         ok = copy_import_file(from, to);
      }
      if (!ok)
         break;
   }
   closedir(dir);
   return ok;
}

static bool remove_import_tree(const char *path)
{
   DIR *dir = opendir(path);
   struct dirent *entry;
   bool ok = true;

   if (!dir)
      return false;
   while ((entry = readdir(dir)))
   {
      char child[900];
      struct stat st;
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         continue;
      if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
          stat(child, &st) || (S_ISDIR(st.st_mode) ? !remove_import_tree(child) : remove(child)))
      {
         ok = false;
         break;
      }
   }
   closedir(dir);
   return ok && !rmdir(path);
}

static int has_importable_files(const char *path)
{
   DIR *dir = opendir(path);
   struct dirent *entry;
   bool found = false;
   if (!dir)
      return -1;
   while ((entry = readdir(dir)))
   {
      char child[900];
      struct stat st;
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
         continue;
      if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
          stat(child, &st))
      {
         closedir(dir);
         return -1;
      }
      if (S_ISDIR(st.st_mode))
      {
         int nested = has_importable_files(child);
         if (nested < 0)
         {
            closedir(dir);
            return -1;
         }
         found = nested > 0;
      }
      else
         found = true;
      if (found)
         break;
   }
   closedir(dir);
   return found ? 1 : 0;
}

static int import_old_database(void)
{
   struct stat st;

   if (stat(OLD_APP_DIR, &st))
      return errno == ENOENT ? 0 : -1;
   if (!S_ISDIR(st.st_mode) || !make_directory("sdmc:/3ds") ||
       !copy_import_tree(OLD_APP_DIR, APP_DIR))
      return -1;
   return 1;
}

static void progress_keys(const char *title, const char *line, int done, int n, const char *keys)
{
   int y0 = 70, w = UI_W - 76;
   ui_clear(C_BG);
   draw_header();
   ui_rrect(14, y0, UI_W - 28, 100, 14, C_PANEL);
   ui_text(&ui_font_bold, 30, y0 + 12, title, C_TEXT, UI_W - 60);
   ui_text(&ui_font_small, 30, y0 + 40, line, C_TEXT2, UI_W - 60);
   ui_rrect(38, y0 + 66, w, 8, 4, C_TRACK);
   if (n > 0 && done > 0)
      ui_rrect(38, y0 + 66, w * done / n < 8 ? 8 : w * done / n, 8, 4, C_ACCENT);
   draw_footer(keys);
   ui_present();
}

static void progress(const char *title, const char *line, int done, int n)
{
   progress_keys(title, line, done, n, n > 1 ? "B: Stop after this one" : "");
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */

static int make_one(const entry *e)
{
   const char *name = e->name;
   char path[800], err[200], body[400], missing[160] = "";
   char custom_title[64] = "", custom_publisher[64] = "";
   bool again, use_custom_publisher = false;
   snprintf(path, sizeof(path), "%s%s", cwd, name);
   again = e->sys == SYS_NDS
      ? twl_title_installed(nds_title_id_for_path(path))
      : icon_installed(title_id_for_path(path, e->sys));
   if (e->sys != SYS_NDS && !sys_installed[e->sys])
   {
      if (e->sys == SYS_3DSX)
         snprintf(missing, sizeof(missing), "\nInstall Homebrew Launcher Loader as a CIA first.");
      else if (e->sys == SYS_GBA)
         snprintf(missing, sizeof(missing), "\nInstall the gpSP 3D CIA first.");
      else if (e->sys == SYS_SNES)
         snprintf(missing, sizeof(missing), "\nInstall the Snes9x 3D CIA first.");
   }
   if (e->sys == SYS_NDS)
      snprintf(body, sizeof(body), "%s\nThis installs a TWL forwarder to NAND.\nThe NDS Bootstrap forwarder pack is required.",
               again ? "This NDS title is already installed." : "This creates an NDS forwarder.");
   else if (e->sys == SYS_3DSX)
      snprintf(body, sizeof(body), "%s\nIt launches through Homebrew Launcher.\nThe top screen shows how it will look.%s",
               again ? "It's on the HOME Menu already:\nthis makes it again." : "Its icon and banner go on the HOME Menu.",
               missing);
   else
      snprintf(body, sizeof(body), "%s\nIt launches through %s.\nThe top screen shows how it will look.%s",
               again ? "It's on the HOME Menu already:\nthis makes it again." : "Its icon and banner go on the HOME Menu.",
               systems[e->sys].name,
               missing);
   if (!dialog(again ? "Make the icon again?" : "Make an icon?", body, "Make Forwarder", "Cancel", C_ACCENT))
      return 0;
   if (dialog("Custom Title/Name?", "", "Yes", "No", C_ACCENT))
   {
      SwkbdState keyboard;
      swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, sizeof(custom_title) - 1);
      swkbdSetFeatures(&keyboard, SWKBD_MULTILINE);
      swkbdSetHintText(&keyboard, "Enter custom title");
      if (swkbdInputText(&keyboard, custom_title, sizeof(custom_title)) != SWKBD_BUTTON_CONFIRM)
         return 0;
      if (!custom_title[0])
      {
         dialog("Title required", "Enter a title, or choose No to use the standard title.",
                "OK", NULL, C_ROW_SEL);
         return 0;
      }
   }
   if (e->sys != SYS_NDS && dialog("Custom Publisher?", "", "Yes", "No", C_ACCENT))
   {
      SwkbdState keyboard;
      swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, sizeof(custom_publisher) - 1);
      swkbdSetHintText(&keyboard, "Enter custom publisher");
      if (swkbdInputText(&keyboard, custom_publisher, sizeof(custom_publisher)) != SWKBD_BUTTON_CONFIRM)
         return 0;
      use_custom_publisher = true;
   }
   progress("Making the icon...", name, 0, 1);
   if (make_icon(path, e, custom_title[0] ? custom_title : NULL,
                 use_custom_publisher ? custom_publisher : NULL, err, sizeof(err)))
   {
      dialog("Could not make it", err, "OK", NULL, C_ROW_SEL);
      return -1;
   }
   else
      dialog("Done", "It's on the HOME Menu now.", "OK", NULL, UI_RGB(0x2E, 0x8B, 0x57));
   return 1;
}

/* ---- box art (L) ---- */

#define SOC_SIZE 0x40000
static u32 *soc_buf;
/* per system: the libraries, then libretro (base NULL); each list loaded
 * once and kept till the end */
static boxart_source ba_src[SYS_COUNT][BOXART_SOURCES + 1];
static bool ba_loaded[SYS_COUNT][BOXART_SOURCES + 1];
static bool ba_skip[SYS_COUNT][BOXART_SOURCES + 1];  /* list failed: left out till the next L */
static const char *ba_name;                   /* the game being done, */
static int ba_i, ba_n;                        /* which of how many */
static u64 ba_drawn;
#ifdef ICONMAKER_TIMING
static u64 ba_dl_t;                           /* when the first download began */
#endif

/* Wi-Fi on and the network started (socInit only while it's needed, as
 * gpSP 3D's update check), or false with a message in err */
static bool net_start(char *err, int errlen)
{
   u32 st = 0;
   Result r;
   if (R_SUCCEEDED(acInit()))
   {
      if (R_FAILED(ACU_GetWifiStatus(&st)))
         st = 0;
      acExit();
   }
   if (!st)
   {
      snprintf(err, errlen, "The 3DS isn't on Wi-Fi. Connect it\nto the internet and try again.");
      return false;
   }
   if (!(soc_buf = memalign(0x1000, SOC_SIZE)))
   {
      snprintf(err, errlen, "Not enough memory.");
      return false;
   }
   if (R_FAILED(r = socInit(soc_buf, SOC_SIZE)))
   {
      snprintf(err, errlen, "The network didn't start (%08lX).", (unsigned long)r);
      free(soc_buf);
      soc_buf = NULL;
      return false;
   }
   net_begin();
   memset(ba_skip, 0, sizeof(ba_skip));
   return true;
}

static void net_stop_all(void)
{
   net_end();
   socExit();
   free(soc_buf);
   soc_buf = NULL;
}

static int ba_stop(void)
{
   hidScanInput();
   return (hidKeysHeld() & KEY_B) != 0;
}

/* the progress screen, at most every 100 ms: the bar is the games done
 * plus how far along this one's step is (step -1: just started) */
static void ba_show(int step, size_t done, size_t total)
{
   static const char *const what[] = { "Getting the game list", "Reading the game", "Downloading" };
   const char *w = step >= 0 ? what[step] : "Looking it up";
   char title[64], line[320];
   int pct = total ? (int)((u64)done * 100 / total) : 0;
   u64 now = svcGetSystemTick();
#ifdef ICONMAKER_TIMING
   if (step == BOXART_STEP_DOWNLOAD && !ba_dl_t)
      ba_dl_t = now;
#endif
   if (now - ba_drawn < SYSCLOCK_ARM11 / 10)
      return;
   ba_drawn = now;
   if (ba_n > 1)
   {
      snprintf(title, sizeof(title), "Getting box art (%d of %d)...", ba_i + 1, ba_n);
      snprintf(line, sizeof(line), "%s: %s", w, ba_name);
   }
   else
   {
      snprintf(title, sizeof(title), "Getting box art...");
      if (total)
         snprintf(line, sizeof(line), "%s... %d%%", w, pct);
      else if (done)
         snprintf(line, sizeof(line), "%s... %lu KB", w, (unsigned long)(done / 1024));
      else
         snprintf(line, sizeof(line), "%s...", w);
   }
   progress_keys(title, line, ba_i * 100 + pct, ba_n * 100, "B: Stop");
}

/* box art for the game e (in cwd): saved to boxart_dir or next to it;
 * *from: the library it came from, or "" for libretro */
static int get_boxart(const entry *e, char *found, int foundlen, const char **from, char *err, int errlen)
{
   boxart_source *src = ba_src[e->sys], use[BOXART_SOURCES + 1];
   const char *dot = strrchr(e->name, '.'), *sys = e->sys == SYS_SNES ? "snes" : "gba";
   char path[800], out[800];
   int r, i, n = nboxart_src + 1, nuse = 0, map[BOXART_SOURCES + 1], which;
#ifdef ICONMAKER_TIMING
   u64 t0 = svcGetSystemTick(), t_list = 0, t_get;
#endif
   ba_name = e->name;
   ba_drawn = 0;
   ba_show(-1, 0, 0);
   *from = "";
   mkdir("sdmc:/3ds", 0777);
   mkdir(APP_DIR, 0777);
   for (i = 0; i < n; i++)
   {
      char list[80];
      const char *base = i < nboxart_src ? boxart_src[i] : NULL;
      if (ba_skip[e->sys][i])
         continue;
      if (ba_loaded[e->sys][i])
      {
         map[nuse] = i;
         use[nuse++] = src[i];
         continue;
      }
      src[i].base = base;
      if (base)
         snprintf(list, sizeof(list), BOXART_LIB_LIST,
                  (unsigned long)crc32(0, (const Bytef*)base, strlen(base)), sys);
      else
         snprintf(list, sizeof(list), BOXART_LIST, sys);
      r = boxart_list_load(&src[i].list, base, e->sys, list, err, errlen);
      if (r == BOXART_STOPPED || (r && !base))
         return r;
      if (r)
      {
         /* a library that can't be reached now: tried again at the next L */
         ba_skip[e->sys][i] = true;
         continue;
      }
      ba_loaded[e->sys][i] = true;
      map[nuse] = i;
      use[nuse++] = src[i];
   }
#ifdef ICONMAKER_TIMING
   t_list = svcGetSystemTick() - t0;
#endif
   snprintf(path, sizeof(path), "%s%s", cwd, e->name);
   if (boxart_dir[0])
   {
      mkdir(boxart_dir, 0777);
      snprintf(out, sizeof(out), "%s/%.*s.png", boxart_dir, (int)(dot - e->name), e->name);
   }
   else
      snprintf(out, sizeof(out), "%s%.*s.png", cwd, (int)(dot - e->name), e->name);
#ifdef ICONMAKER_TIMING
   ba_dl_t = 0;
   t_get = svcGetSystemTick();
#endif
   r = boxart_get(use, nuse, path, e->sys, out, BOXART_SIDE, found, foundlen, &which, err, errlen);
   if (r == BOXART_SAVED && map[which] < nboxart_src)
      *from = boxart_src[map[which]];
#ifdef ICONMAKER_TIMING
   {
      /* finding the game (its name, or reading it for the CRC32) until the
       * first download began; downloading (all names tried) and saving */
      static const char *const res[] = { "saved", "none found", "error", "stopped" };
      u64 end = svcGetSystemTick(), dl = ba_dl_t ? ba_dl_t : end;
      char line[700];
      snprintf(line, sizeof(line), "box art %s: %s%s%s%s%s, %.1f ms (game lists %.1f, finding the game %.1f, "
               "downloading + saving %.1f)\n", e->name, res[r], r == BOXART_SAVED ? " as " : "",
               r == BOXART_SAVED ? found : "", r == BOXART_SAVED ? " from " : "",
               r == BOXART_SAVED ? (**from ? *from : "libretro") : "", tm_us(end - t0) / 1000.0,
               tm_us(t_list) / 1000.0, tm_us(dl - t_get) / 1000.0, tm_us(end - dl) / 1000.0);
      tm_log(line);
   }
#endif
   return r;
}

static void boxart_one(const entry *e)
{
   char err[200], found[256], body[400];
   const char *from;
   int r;
   if (e->pic[0] >= 0)
   {
      dialog("It has a picture", "Its own picture is used for the icon.\n"
             "Delete or rename it to get box art.", "OK", NULL, C_ROW_SEL);
      return;
   }
   if (!net_start(err, sizeof(err)))
   {
      dialog("Could not get box art", err, "OK", NULL, C_ROW_SEL);
      return;
   }
   ba_i = 0;
   ba_n = 1;
   boxart_stop = ba_stop;
   boxart_progress = ba_show;
   r = get_boxart(e, found, sizeof(found), &from, err, sizeof(err));
   boxart_stop = NULL;
   boxart_progress = NULL;
   net_stop_all();
   if (r == BOXART_SAVED)
   {
      /* where from: a library's URL without https:// */
      const char *f = strstr(from, "://") ? strstr(from, "://") + 3 : from;
      snprintf(body, sizeof(body), "%.120s\nFrom %.100s.\nThe top screen shows it on the icon.", found,
               *f ? f : "libretro-thumbnails");
      dialog("Got the box art", body, "OK", NULL, UI_RGB(0x2E, 0x8B, 0x57));
   }
   else if (r == BOXART_NONE)
      dialog("No box art found", nboxart_src ? "Neither your libraries nor libretro-\nthumbnails have a cover for it."
             : "libretro-thumbnails has no cover for\nit. Games named as in No-Intro, like\n"
             "Golf (USA).gba, are found best.", "OK", NULL, C_ROW_SEL);
   else if (r == BOXART_ERROR)
      dialog("Could not get box art", err, "OK", NULL, C_ROW_SEL);
}

static void boxart_folder(void)
{
   char err[200] = "", found[256], body[300];
   const char *from;
   int i, saved = 0, none = 0, r = BOXART_SAVED, want = 0;
   for (i = 0; i < nentries; i++)
      if (!entries[i].dir && entries[i].pic[0] < 0)
         want++;
   if (!want)
   {
      dialog(ngames ? "They all have pictures" : "No games here",
             ngames ? "Every game here has its own picture." : "This folder has no GBA or SNES games.",
             "OK", NULL, C_ROW_SEL);
      return;
   }
   snprintf(body, sizeof(body), "For the %d game%s here without a picture,\nfrom libretro-thumbnails (needs Wi-Fi).",
            want, want == 1 ? "" : "s");
   if (!dialog("Get box art?", body, "Get box art", "Cancel", C_ACCENT))
      return;
   if (!net_start(err, sizeof(err)))
   {
      dialog("Could not get box art", err, "OK", NULL, C_ROW_SEL);
      return;
   }
   ba_i = 0;
   ba_n = want;
   boxart_stop = ba_stop;
   boxart_progress = ba_show;
   for (i = 0; i < nentries && aptMainLoop(); i++)
   {
      if (entries[i].dir || entries[i].pic[0] >= 0)
         continue;
      r = get_boxart(&entries[i], found, sizeof(found), &from, err, sizeof(err));
      if (r == BOXART_SAVED)
         saved++;
      else if (r == BOXART_NONE)
         none++;
      else
         break;                        /* stopped, or the network or SD card failed */
      ba_i++;
   }
   boxart_stop = NULL;
   boxart_progress = NULL;
   net_stop_all();
   if (r == BOXART_ERROR)
      snprintf(body, sizeof(body), "%d saved, %d not found. Then:\n%.100s", saved, none, err);
   else
      snprintf(body, sizeof(body), "%d saved, %d not found%s", saved, none,
               r == BOXART_STOPPED ? ", then stopped." : ".");
   dialog(r == BOXART_ERROR ? "Box art stopped" : "Done", body, "OK", NULL,
          r == BOXART_ERROR ? C_ROW_SEL : UI_RGB(0x2E, 0x8B, 0x57));
}

static bool delete_icon(const icon_entry *ic)
{
   char body[200];
   Result r;
   u64 tid = ic->tid;
   snprintf(body, sizeof(body), "%.60s\nThe .3dsx stays on the SD card.", ic->name);
   if (!dialog("Uninstall this forwarder?", body, "Uninstall", "Cancel", UI_RGB(0xC8, 0x45, 0x45)))
      return false;
   progress_keys("Uninstalling this Forwarder,", "Please Wait...", 0, 0, "");
   gspWaitForVBlank();
   r = AM_DeleteTitle(MEDIATYPE_SD, tid);
   if (R_SUCCEEDED(r))
      AM_DeleteTicket(tid);
   if (R_FAILED(r))
   {
      snprintf(body, sizeof(body), "The system said %08lX.", (unsigned long)r);
      dialog("Could not delete it", body, "OK", NULL, C_ROW_SEL);
      return false;
   }
   remove_installed_icon(tid);
   set_game_installed_badge(tid, false);
   return true;
}

static bool delete_twl_forwarder(const twl_icon_entry *ic)
{
   char body[200];
   Result r;
   u64 tid = ic->tid;
   int i, j;
   if (!ic->is_nds_forwarder)
      return false;
   snprintf(body, sizeof(body),
            "%.60s\nOnly this NDS forwarder is removed.\nThe .nds stays on the SD card.",
            ic->name);
   if (!dialog("Uninstall this NDS forwarder?", body, "Uninstall", "Cancel",
               UI_RGB(0xC8, 0x45, 0x45)))
      return false;
   progress_keys("Uninstalling this Forwarder,", "Please Wait...", 0, 0, "");
   gspWaitForVBlank();
   r = AM_DeleteTitle(MEDIATYPE_NAND, tid);
   if (R_SUCCEEDED(r))
      AM_DeleteTicket(tid);
   if (R_FAILED(r))
   {
      snprintf(body, sizeof(body), "The system said %08lX.", (unsigned long)r);
      dialog("Could not uninstall it", body, "OK", NULL, C_ROW_SEL);
      return false;
   }
   for (i = 0; i < ntwl_icons; i++)
      if (twl_icons[i].tid == tid && twl_icons[i].media == MEDIATYPE_NAND)
      {
         for (j = i + 1; j < ntwl_icons; j++)
            twl_icons[j - 1] = twl_icons[j];
         ntwl_icons--;
         break;
      }
   if (twl_installed_count != 0xFFFFFFFF && twl_installed_count)
      twl_installed_count--;
   set_game_installed_badge(tid, false);
   return true;
}

static void save_dir(void)
{
   FILE *f;
   mkdir("sdmc:/3ds", 0777);
   mkdir(APP_DIR, 0777);
   if ((f = fopen(LAST_DIR, "w")))
   {
      fputs(cwd, f);
      fclose(f);
   }
}

static void save_settings(void)
{
   FILE *f;
   mkdir("sdmc:/3ds", 0777);
   mkdir(APP_DIR, 0777);
   if ((f = fopen(SETTINGS, "w")))
   {
      fprintf(f, "banner=%s\n", style_key[banner_style]);
      fclose(f);
   }
}

static void load_settings(void)
{
   FILE *f = fopen(SETTINGS, "r");
   char buf[320];
   if (!f)
      return;
   while (fgets(buf, sizeof(buf), f))
      if (!strncmp(buf, "banner=", 7))
      {
         int i;
         buf[strcspn(buf, "\r\n")] = 0;
         for (i = 0; i < STYLE_COUNT; i++)
            if (!strcmp(buf + 7, style_key[i]))
               banner_style = i;
      }
   fclose(f);
}

static void load_dir(void)
{
   FILE *f = fopen(LAST_DIR, "r");
   char buf[512];
   DIR *d;
   if (!f)
      return;
   if (fgets(buf, sizeof(buf), f))
   {
      buf[strcspn(buf, "\r\n")] = 0;
      if (!strncmp(buf, "sdmc:/", 6) && buf[strlen(buf) - 1] == '/' && (d = opendir(buf)))
      {
         closedir(d);
         strcpy(cwd, buf);
      }
   }
   fclose(f);
}

/* ------------------------------------------------------------------ */

/* The last previews made, so going back to a game shows it at once. Cleared
 * when the folder, the banner style or pad/crop changes. */
#define CACHE_N        16
#define PREVIEW_DELAY  18        /* frames (0.3 s) the selection must stay before a preview is made */
static struct { char path[800]; game_art *art; u32 used; } cache[CACHE_N];
static u32 cache_clock;

static void cache_clear(void)
{
   int i;
   for (i = 0; i < CACHE_N; i++)
      cache[i].path[0] = 0;
}

static game_art *cache_find(const char *path)
{
   int i;
   for (i = 0; i < CACHE_N; i++)
      if (cache[i].art && !strcmp(cache[i].path, path))
      {
         cache[i].used = ++cache_clock;
         return cache[i].art;
      }
   return NULL;
}

/* a slot for path's preview: an empty one, or the one unused longest */
static game_art *cache_slot(const char *path)
{
   int i, old = 0;
   for (i = 0; i < CACHE_N; i++)
      if (!cache[i].path[0] || cache[i].used < cache[old].used)
      {
         old = i;
         if (!cache[i].path[0])
            break;
      }
   if (!cache[old].art && !(cache[old].art = malloc(sizeof(game_art))))
      return NULL;
   snprintf(cache[old].path, sizeof(cache[0].path), "%s", path);
   cache[old].used = ++cache_clock;
   return cache[old].art;
}

/* frames to wait before the top screen shows what's selected: a game's
 * preview takes a while to make, so not while scrolling past it */
static int preview_wait(int tab, int sel)
{
   char path[800];
   if (tab != TAB_GAMES)
      return 8;
   if (!nentries || entries[sel].dir)
      return 1;
   snprintf(path, sizeof(path), "%s%s", cwd, entries[sel].name);
   return cache_find(path) ? 1 : PREVIEW_DELAY;
}

static void cache_drop(const game_art *a)
{
   int i;
   for (i = 0; i < CACHE_N; i++)
      if (cache[i].art == a)
         cache[i].path[0] = 0;
}

/* A button pressed while a preview is made stops it (a huge cover can take
 * seconds); the main loop still gets the button. Called every few rows of a
 * PNG or JPEG being read, which also turns the spinner once the preview
 * takes a while. */
static u32 preview_keys;
static bool preview_stopped;

#define SPINNER_AFTER  250       /* ms a preview takes before the spinner shows */
#define SPINNER_EVERY  100       /* ms between its frames */
#define MS_TICKS(ms)   ((u64)(ms) * (SYSCLOCK_ARM11 / 1000))

static u16 mix565(u16 a, u16 b, int t)   /* t 0..256: a .. b */
{
   int r = ((a >> 11) * (256 - t) + (b >> 11) * t) >> 8;
   int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
   int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
   return (u16)(r << 11 | g << 5 | bl);
}

/* the top screen as it is, with a small "loading" pill at the bottom: eight
 * dots going round, and that a button skips. The first frame is the whole
 * screen (so both framebuffers have it), the rest only the pill: the Old
 * 3DS spent a tenth of its time on whole frames. */
static void draw_spinner(u64 now, bool first)
{
   static const signed char dx[8] = { 6, 4, 0, -4, -6, -4, 0, 4 }, dy[8] = { 0, 4, 6, 4, 0, -4, -6, -4 };
   static const char msg[] = "Loading the cover...   press any button to skip";
   int i, w = ui_text_w(&ui_font_small, msg) + 40, x = (UI_W - w) / 2, y = UI_H - 30;
   int head = (int)((now - preview_t0) / MS_TICKS(SPINNER_EVERY)) % 8;
   if (first)
      memcpy(ui_canvas, top_shown, sizeof(top_shown));
   ui_rrect(x, y, w, 22, 11, C_PANEL);
   for (i = 0; i < 8; i++)
      ui_circle(x + 14 + dx[i], y + 11 + dy[i], 2, mix565(C_ACCENT, C_PANEL, ((head - i + 8) % 8) * 28));
   if (first)
   {
      ui_text_vc(&ui_font_small, x + 28, y, 22, msg, C_TEXT2, 0);
      present_top_canvas();
      present_top_canvas();            /* both framebuffers */
   }
   else                                /* only the dots change */
      present_top_rect(x + 4, y + 1, 21, 20);
}

static int preview_stop(void)
{
   u64 now = svcGetSystemTick();
   u32 k;
   if (now - preview_t0 > MS_TICKS(SPINNER_AFTER) && now - spinner_last > MS_TICKS(SPINNER_EVERY))
   {
      draw_spinner(now, !spinner_last);
      spinner_last = svcGetSystemTick();
      spinner_ticks += spinner_last - now;
   }
   hidScanInput();
   if (!(k = hidKeysDown()))
      return 0;
   preview_keys |= k;
   preview_stopped = true;
   return 1;
}

#ifdef ICONMAKER_TIMING
static u32 tm_total(void)
{
   u32 total = 0;
   int i;
   for (i = 0; i < ntm; i++)
      if (tm[i].n > 0)                   /* not the parts measured inside a step */
         total += tm[i].us;
   return total;
}

/* a preview's steps on a dark panel over the right of the top screen, and
 * in the log, one step a line */
static void tm_report(const char *name)
{
   char line[96], log[2000];
   int i, y = 4, n;
   u32 total = tm_total();
   ui_fill_alpha(130, 0, UI_W - 130, UI_H, C_BG, 230);
   snprintf(line, sizeof(line), "total %.0f ms", total / 1000.0);
   ui_text(&ui_font_bold, 136, y, line, C_WARN, 0);
   y += 20;
   for (i = 0; i < ntm; i++, y += 12)
   {
      snprintf(line, sizeof(line), "%6.1f  %s%s", tm[i].us / 1000.0, tm[i].what,
               tm[i].n > 1 ? " (x" : "");
      if (tm[i].n > 1)
         snprintf(line + strlen(line), sizeof(line) - strlen(line), "%d)", tm[i].n);
      ui_text(&ui_font_small, 136, y, line, C_TEXT2, UI_W - 140);
   }
   y += 4;
   for (i = 0; i < 2; i++)
      if (tm_pic[i][0])
      {
         snprintf(line, sizeof(line), "%s: %s", i ? "banner" : "icon", tm_pic[i]);
         ui_text(&ui_font_small, 136, y, line, C_DIM, UI_W - 140);
         y += 12;
      }
   snprintf(line, sizeof(line), "folder read %.0f ms (%lu)", tm_dir_us / 1000.0,
            (unsigned long)tm_dir_n);
   ui_text(&ui_font_small, 136, y, line, C_DIM, 0);
   snprintf(line, sizeof(line), "bottom redraw %.1f ms", tm_bottom_us / 1000.0);
   ui_text(&ui_font_small, 136, y + 12, line, C_DIM, 0);

   n = snprintf(log, sizeof(log), "preview %s: %.1f ms\n", name, total / 1000.0);
   for (i = 0; i < 2; i++)
      if (tm_pic[i][0])
         n += snprintf(log + n, sizeof(log) - n, "   %s: %s\n", i ? "banner" : "icon", tm_pic[i]);
   for (i = 0; i < ntm && n < (int)sizeof(log) - 80; i++)
      n += snprintf(log + n, sizeof(log) - n, "   %8.1f ms  %s%s\n", tm[i].us / 1000.0, tm[i].what,
                    tm[i].n > 1 ? " (several times)" : "");
   tm_log(log);
}

static void tm_report_stopped(const char *name)
{
   char line[400];
   snprintf(line, sizeof(line), "preview %s: stopped by a button after %.1f ms\n", name, tm_total() / 1000.0);
   tm_log(line);
}
#else
#define tm_report(name)          ((void)0)
#define tm_report_stopped(name)  ((void)0)
#endif

/* The top screen for what's selected. Returns false if a button stopped
 * it (the top screen is left as it was). */
static bool draw_top_for(int tab, int sel, bool am)
{
   static game_art art;
   if (!am)
   {
      top_bg();
      ui_text_center(&ui_font_title, UI_W / 2, 70, "Can't install", C_BAD);
      text_lines(&ui_font_small, 40, 110, 16, "No access to the install service.\n"
                 "Start 3DSX Forwarder from the HOME Menu\n(install its CIA) or the Homebrew\n"
                 "Launcher on Luma3DS.", C_TEXT2, UI_W - 60);
   }
   else if (tab == TAB_ICONS)
   {
      if (ninstalled_icons)
         draw_top_icon(&icons[installed_icon_indices[sel]]);
      else
         top_bg();
   }
   else if (tab == TAB_TWL)
   {
      if (ntwl_icons)
         draw_top_twl_icon(&twl_icons[sel]);
      else
         top_bg();
   }
   else if (!nentries)
      draw_top_intro();
   else if (entries[sel].dir)
      draw_top_folder(entries[sel].name, !strcmp(entries[sel].name, ".."));
   else if (entries[sel].sys == SYS_NDS)
   {
      char path[800];
      snprintf(path, sizeof(path), "%s%s", cwd, entries[sel].name);
      draw_top_nds_preview(path, nds_custom_icon);
   }
   else
   {
      char path[800];
      game_art *a;
      snprintf(path, sizeof(path), "%s%s", cwd, entries[sel].name);
      tm_start();
      if ((a = cache_find(path)))
         tm_mark("from the cache");
      else
      {
         if (!(a = cache_slot(path)))
            a = &art;
         preview_stopped = false;
         preview_t0 = svcGetSystemTick();
         spinner_last = spinner_ticks = 0;
         png_shrink_stop = jpeg_shrink_stop = preview_stop;
         make_art(path, &entries[sel], a);
         png_shrink_stop = jpeg_shrink_stop = NULL;
         if (preview_stopped)
         {
            cache_drop(a);
            if (spinner_last)              /* the top screen as it was, without the spinner */
            {
               memcpy(ui_canvas, top_shown, sizeof(top_shown));
               present_top_canvas();
               present_top_canvas();
            }
            tm_report_stopped(entries[sel].name);
            return false;
         }
      }
      draw_top_game(a);
      tm_mark("draw the top screen");
      tm_report(entries[sel].name);
   }
   present_top();
   return true;
}

int main(void)
{
   int tab = TAB_GAMES, sel = 0, top = 0;
   int saved_sel[3] = { 0, 0, 0 }, saved_top[3] = { 0, 0, 0 };
   int top_wait = 0;           /* frames until the top screen is redrawn */
   int import_result, old_path_result, old_has_files;
   struct stat old_data;
   bool am, fs, ps, redraw = true, romfs;

   osSetSpeedupEnable(true);           /* New 3DS: the faster CPU and its L2 cache */
   gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
   romfs = R_SUCCEEDED(romfsInit());
   old_path_result = stat(OLD_APP_DIR, &old_data);
   if (!old_path_result)
   {
      old_has_files = S_ISDIR(old_data.st_mode) ? has_importable_files(OLD_APP_DIR) : 0;
      if (old_has_files < 0)
         dialog("Could not check old data", "The old folder could not be read. No files were changed.",
                "OK", NULL, C_BAD);
      else if (old_has_files > 0 &&
          dialog_with_default("Import old database?",
                 "Saved files were found in\n/3ds/3DSX-Forwarder. Import them\ninto /3ds/MULTI-Forwarder/?",
                 "YES", "NO", C_ACCENT, false))
      {
         import_result = import_old_database();
         if (import_result < 0)
         {
            dialog("Import failed", "The old folder has been preserved. Check SD space and card access, then try again.",
                   "OK", NULL, C_BAD);
         }
         else if (import_result > 0 &&
                  dialog("Import complete", "The folder was imported.\n\nDelete the old 3DSX-Forwarder\nHomebrew folder?",
                         "YES", "NO", C_BAD) &&
                  !remove_import_tree(OLD_APP_DIR))
         {
            dialog("Could not delete the old folder",
                   "The imported database is safe, but some old files remain.",
                   "OK", NULL, C_BAD);
         }
      }
   }
   else if (errno != ENOENT)
   {
      dialog("Could not check old data", "The old database folder could not be checked. No files were changed.",
             "OK", NULL, C_BAD);
   }
   mkdir("sdmc:/3ds", 0777);
   mkdir(APP_DIR, 0777);
   am = R_SUCCEEDED(amInit());
   fs = R_SUCCEEDED(fsInit());
   ps = R_SUCCEEDED(psInit());
   nds_services_ready = am && fs && ps;
   hidSetRepeatParameters(20, 4);

   load_title_records();
   if (am)
   {
      read_icons();
      read_twl_titles();
   }
   load_settings();
#ifdef ICONMAKER_TIMING
   tm_log_start();
   png_shrink_clock = jpeg_shrink_clock = svcGetSystemTick;
#endif
   load_dir();
   read_dir();
   top_wait = 1;

   while (aptMainLoop())
   {
      u32 k;
      touchPosition t;
      int count, old_sel = sel, old_tab = tab;
      bool act = false;

      hidScanInput();
      k = hidKeysDown() | (hidKeysDownRepeat() & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT)) | preview_keys;
      preview_keys = 0;
      count = tab == TAB_GAMES ? nentries :
              tab == TAB_ICONS ? ninstalled_icons : ntwl_icons;
      if (k & KEY_START)
         break;
      if (k & KEY_DDOWN && sel < count - 1) sel++;
      if (k & KEY_DUP && sel > 0) sel--;
      if (k & KEY_DRIGHT) sel = sel + ROWS < count ? sel + ROWS : count - 1;
      if (k & KEY_DLEFT) sel = sel > ROWS ? sel - ROWS : 0;

      /* touch: tabs, rows (tap the selected row again to use it) */
      if (touched(k, &t))
      {
         if (t.py >= TAB_Y && t.py < TAB_Y + TAB_H)
         {
            int xs[3], ws[3], target;
            tab_layout(xs, ws);
            for (target = 0; target < 3; target++)
               if (t.px >= xs[target] && t.px < xs[target] + ws[target])
                  break;
            if (target != tab)
            {
               if (target < 3)
               {
                  saved_sel[tab] = sel;
                  saved_top[tab] = top;
                  tab = target;
                  sel = saved_sel[tab];
                  top = saved_top[tab];
                  redraw = true;
               }
            }
         }
         else if (t.py >= LIST_Y && t.py < LIST_Y + ROWS * ROW_H)
         {
            int idx = top + (t.py - LIST_Y) / ROW_H;
            if (idx < count)
            {
               if (idx == sel)
                  act = true;
               sel = idx;
            }
         }
      }

      if (k & KEY_R)
      {
         saved_sel[tab] = sel;
         saved_top[tab] = top;
         tab = (tab + 1) % 3;
         sel = saved_sel[tab];
         top = saved_top[tab];
         redraw = true;
      }
      else if (k & KEY_L)
      {
         saved_sel[tab] = sel;
         saved_top[tab] = top;
         tab = (tab + 2) % 3;
         sel = saved_sel[tab];
         top = saved_top[tab];
         redraw = true;
      }
      else if (tab == TAB_GAMES)
      {
         if (k & KEY_B && strcmp(cwd, "sdmc:/"))
         {
            leave_folder(&sel);
         }
         else if (k & KEY_SELECT)
         {
            if (nentries && !entries[sel].dir && entries[sel].sys == SYS_NDS)
            {
               char path[800], icon_path[800];
               snprintf(path, sizeof(path), "%s%s", cwd, entries[sel].name);
               nds_custom_icon = nds_custom_icon_path(path, icon_path, sizeof(icon_path))
                  ? !nds_custom_icon : false;
            }
            else if (nentries && !entries[sel].dir && entries[sel].pic[1] >= 0)
            {
               banner_cycle = (banner_cycle + 1) % 4;
               if (banner_cycle == 1) banner_style = STYLE_BLUE;
               else if (banner_cycle == 2) banner_style = STYLE_CLEAR;
               else if (banner_cycle == 3) banner_style = STYLE_3D;
            }
            else
               banner_style = (banner_style + 1) % STYLE_COUNT;
            if (!nentries || entries[sel].dir || entries[sel].sys != SYS_NDS)
            if (!nentries || entries[sel].dir || entries[sel].sys != SYS_NDS)
            {
               cache_clear();
               save_settings();
            }
            top_wait = 1;
            redraw = true;
         }
         else if ((k & KEY_A || act) && nentries)
         {
            if (entries[sel].dir)
            {
               if (!strcmp(entries[sel].name, ".."))
                  leave_folder(&sel);
               else
               {
                  enter(entries[sel].name);
                  sel = top = 0;
               }
            }
            else if (am)
            {
               int result = make_one(&entries[sel]);
               if (result > 0)
               {
                  char path[800];
                  snprintf(path, sizeof(path), "%s%s", cwd, entries[sel].name);
                  entries[sel].has_icon = true;
                  if (entries[sel].sys != SYS_NDS &&
                      !refresh_installed_icon(path, entries[sel].sys))
                     installed_icons_dirty = true;
                  top_wait = 1;
               }
               else if (result < 0)
               {
                  if (entries[sel].sys == SYS_NDS)
                     twl_titles_dirty = true;
                  else
                     installed_icons_dirty = true;
               }
            }
            redraw = true;
         }
      }
      else if (tab == TAB_ICONS)
      {
         if (k & KEY_Y)
         {
            installed_filter = (installed_filter + 1) % FILTER_COUNT;
            rebuild_installed_filter();
            sel = top = 0;
            redraw = true;
         }
         else if ((k & KEY_X || act) && ninstalled_icons)
         {
            if (delete_icon(&icons[installed_icon_indices[sel]]))
               top_wait = 1;
            redraw = true;
         }
      }
      else if (tab == TAB_TWL && (k & KEY_X) && sel >= 0 &&
               sel < ntwl_icons &&
               twl_icons[sel].is_nds_forwarder)
      {
         if (delete_twl_forwarder(&twl_icons[sel]))
            top_wait = 1;
         redraw = true;
      }

      if (tab == TAB_ICONS && installed_icons_dirty)
         read_icons();
      if (tab == TAB_TWL && twl_titles_dirty)
         read_twl_titles();

      saved_sel[tab] = sel;
      saved_top[tab] = top;
      if (sel != old_sel || tab != old_tab)
         banner_cycle = 0;
      count = tab == TAB_GAMES ? nentries :
              tab == TAB_ICONS ? ninstalled_icons : ntwl_icons;
      if (sel >= count) sel = count - 1;
      if (sel < 0) sel = 0;
      if (sel != old_sel || tab != old_tab || nds_icon_default_pending)
      {
         nds_icon_default_pending = false;
         nds_custom_icon = false;
         if (tab == TAB_GAMES && nentries && !entries[sel].dir &&
             entries[sel].sys == SYS_NDS)
         {
            char path[800], icon_path[800];
            snprintf(path, sizeof(path), "%s%s", cwd, entries[sel].name);
            nds_custom_icon = nds_custom_icon_path(path, icon_path,
                                                    sizeof(icon_path));
         }
      }
      if (sel < top) top = sel;
      if (sel >= top + ROWS) top = sel - ROWS + 1;
      if (top > count - ROWS) top = count - ROWS > 0 ? count - ROWS : 0;

      if (sel != old_sel || tab != old_tab || redraw || k)
      {
#ifdef ICONMAKER_TIMING
         u64 t_bottom = svcGetSystemTick();
#endif
         draw_bottom(tab, sel, top);
         ui_present();
#ifdef ICONMAKER_TIMING
         tm_bottom_us = tm_us(svcGetSystemTick() - t_bottom);
         if (tm_bottom_us > 16700)             /* longer than a frame */
         {
            char line[80];
            snprintf(line, sizeof(line), "bottom screen redraw: %.1f ms\n", tm_bottom_us / 1000.0);
            tm_log(line);
         }
#endif
         if (sel != old_sel || tab != old_tab)
            top_wait = preview_wait(tab, sel); /* wait for the selection to settle */
         else if (redraw && !top_wait)
            top_wait = preview_wait(tab, sel);
         redraw = false;
      }
      /* the top preview, once the selection stops moving */
      if (top_wait && !--top_wait)
      {
         /* stopped by a button: made again once the selection settles */
         if (!draw_top_for(tab, sel, am))
            top_wait = preview_wait(tab, sel);
         draw_bottom(tab, sel, top);       /* the canvas is shared: draw it back */
         ui_present();
      }
      gspWaitForVBlank();
   }

   save_dir();
   if (am)
      amExit();
   if (fs)
      fsExit();
   if (ps)
      psExit();
   if (romfs)
      romfsExit();
   gfxExit();
   return 0;
}
