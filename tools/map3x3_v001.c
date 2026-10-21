/*
 * map3x3.c - MAP file viewer/exporter for the Sega Mega CD game
 *            Seima Densetsu 3x3 Eyes (00MAPxx.DAT files).
 *
 * Decodes the MAP files and writes BMP images of
 *   - each map layer (rendered with the map's own tileset, block table, palette)
 *   - the field tileset sheet
 *   - the 32x18-tile backdrop pictures stored in the file
 *
 * Plain C99, no external libraries. Builds with MinGW, MSVC or any Unix cc.
 * BSD-3-Clause style: use freely, no warranty.
 *
 * ---------------------------------------------------------------------------
 * File format as reverse-engineered from the sample files (all values big
 * endian; "file offset" = Mega CD memory address - 0x30000):
 *
 *   0x0000  u16  W0 = file offset of the section table
 *   0x0018  u32[] list of pointers (memory addr) to "backdrop sets", ends at 0
 *   W0      u32[] section table, 10-11 entries (memory addr; file = a-0x30000)
 *             e0..e1  event/object data
 *             e1..e2  (unknown, usually 0x100-0x200 bytes)
 *             e2..e3  map layer 1  (first 0x200 bytes are zero padding)
 *             e3..e4  map layer 2  (first 0x200 bytes are zero padding)
 *             e5..e6  block table: 1024 blocks x 4 VDP name-table words
 *                     (2x2 tiles, order TL,TR,BL,BR)
 *             e7..e8  (unknown, 0x1000 bytes, probably block attributes)
 *             e8      Kosinski compressed field tileset (4bpp, 512 tiles)
 *             e9      palette: 64 x 0BGR words (4 lines of 16). Often only
 *                     line 0 is filled in; empty lines fall back to line 0.
 *   Map layer: sequence of 16x16-block pages (0x200 bytes each, 1 word per
 *             block, low 10 bits = block number). Pages are stored row-major;
 *             pages-per-row is auto-detected by edge matching (or -w N).
 *   Backdrop set (at each pointer from 0x0018):
 *             u16 ?, u16 tilemapOffset, u16 paletteOffset (relative to set),
 *             Kosinski tiles at set+6, Kosinski 32x18 name table at
 *             set+tilemapOffset, 16 palette words at set+paletteOffset.
 *             The name table uses VRAM tile numbers; the compressed tile
 *             stream holds a blank tile followed by the distinct used tile
 *             numbers in ascending order.
 * ---------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* ------------------------------------------------------------------ util */
static u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static u8 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc((size_t)n + 16);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    memset(buf + n, 0, 16);
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* ------------------------------------------------------- Kosinski decoder */
typedef struct { const u8 *in; size_t len, pos; u16 desc; int bits; int err; } KosState;

static u16 kos_rd16(KosState *k)
{
    if (k->pos + 2 > k->len) { k->err = 1; return 0; }
    u16 v = (u16)(k->in[k->pos] | (k->in[k->pos + 1] << 8));
    k->pos += 2;
    return v;
}
static int kos_bit(KosState *k)
{
    int b = k->desc & 1;
    k->desc >>= 1;
    if (--k->bits == 0) { k->desc = kos_rd16(k); k->bits = 16; }
    return b;
}

/* returns decoded length, or -1 on error. out must hold outcap bytes. */
static long kosinski(const u8 *in, size_t inlen, size_t pos, u8 *out, size_t outcap, size_t *endpos)
{
    KosState k = { in, inlen, pos, 0, 16, 0 };
    size_t n = 0;
    k.desc = kos_rd16(&k);
    for (;;) {
        if (k.err) return -1;
        if (kos_bit(&k)) {
            if (k.pos >= k.len || n >= outcap) return -1;
            out[n++] = in[k.pos++];
        } else {
            int cnt, off;
            if (kos_bit(&k)) {
                if (k.pos + 2 > k.len) return -1;
                int lo = in[k.pos], hi = in[k.pos + 1];
                k.pos += 2;
                off = (int)(0xE000 | ((hi & 0xF8) << 5) | lo) - 0x10000;
                cnt = hi & 7;
                if (cnt) cnt += 2;
                else {
                    if (k.pos >= k.len) return -1;
                    int c = in[k.pos++];
                    if (c == 0) break;
                    if (c == 1) continue;
                    cnt = c + 1;
                }
            } else {
                cnt = kos_bit(&k) << 1;
                cnt |= kos_bit(&k);
                cnt += 2;
                if (k.pos >= k.len) return -1;
                off = (int)in[k.pos++] - 256;
            }
            if ((long)n + off < 0 || n + (size_t)cnt > outcap) return -1;
            while (cnt--) { out[n] = out[(long)n + off]; n++; }
        }
    }
    if (endpos) *endpos = k.pos;
    return (long)n;
}

/* ------------------------------------------------------------- image/BMP */
typedef struct { int w, h; u8 *rgb; } Image;   /* rgb: w*h*3, top-down */

static Image img_new(int w, int h, u8 r, u8 g, u8 b)
{
    Image im; im.w = w; im.h = h;
    im.rgb = (u8 *)malloc((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) { im.rgb[i*3] = r; im.rgb[i*3+1] = g; im.rgb[i*3+2] = b; }
    return im;
}

static int write_bmp(const char *path, const Image *im)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int rowbytes = (im->w * 3 + 3) & ~3;
    u32 datasize = (u32)rowbytes * im->h, filesize = 54 + datasize;
    u8 hdr[54] = { 'B', 'M' };
    hdr[2] = filesize; hdr[3] = filesize >> 8; hdr[4] = filesize >> 16; hdr[5] = filesize >> 24;
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = im->w; hdr[19] = im->w >> 8; hdr[20] = im->w >> 16; hdr[21] = im->w >> 24;
    hdr[22] = im->h; hdr[23] = im->h >> 8; hdr[24] = im->h >> 16; hdr[25] = im->h >> 24;
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = datasize; hdr[35] = datasize >> 8; hdr[36] = datasize >> 16; hdr[37] = datasize >> 24;
    fwrite(hdr, 1, 54, f);
    u8 *row = (u8 *)calloc(1, rowbytes);
    for (int y = im->h - 1; y >= 0; y--) {
        const u8 *src = im->rgb + (size_t)y * im->w * 3;
        for (int x = 0; x < im->w; x++) { row[x*3] = src[x*3+2]; row[x*3+1] = src[x*3+1]; row[x*3+2] = src[x*3]; }
        fwrite(row, 1, rowbytes, f);
    }
    free(row);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------- Mega Drive bits */
typedef struct { u8 r, g, b; } Rgb;

static Rgb md_color(u16 c)
{
    Rgb o;
    o.r = (u8)(((c >> 1) & 7) * 255 / 7);
    o.g = (u8)(((c >> 5) & 7) * 255 / 7);
    o.b = (u8)(((c >> 9) & 7) * 255 / 7);
    return o;
}

/* 4bpp tiles -> one byte per pixel, 64 bytes per tile */
static u8 *unpack_tiles(const u8 *data, size_t len, int *ntiles)
{
    int n = (int)((len + 31) / 32);
    u8 *px = (u8 *)calloc((size_t)n, 64);
    for (int t = 0; t < n; t++)
        for (int i = 0; i < 32; i++) {
            u8 b = (t * 32 + i < (int)len) ? data[t * 32 + i] : 0;
            px[t * 64 + i * 2]     = b >> 4;
            px[t * 64 + i * 2 + 1] = b & 15;
        }
    *ntiles = n;
    return px;
}

static void blit_tile(Image *im, int x0, int y0, const u8 *tilepx, int hflip, int vflip,
                      const Rgb *pal /*16*/, Rgb bg)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int sx = hflip ? 7 - x : x, sy = vflip ? 7 - y : y;
            int c = tilepx[sy * 8 + sx];
            Rgb col = c ? pal[c] : bg;
            u8 *d = im->rgb + ((size_t)(y0 + y) * im->w + (x0 + x)) * 3;
            d[0] = col.r; d[1] = col.g; d[2] = col.b;
        }
}

static int is_zero_buf(const u8 *p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i]) return 0; return 1; }
#define is_zero is_zero_buf
static Rgb g_bg = { 32, 32, 32 };
static u16 *g_grid = NULL; static int g_gw = 0, g_gh = 0;   /* layer-1 block ids (blocks) */
static int g_ox = -1, g_oy = 0;       /* block origin of the cropped layer-1 image */
/* ----------------------------------------------------------- MAP loading */
#define MAX_TILES 2048               /* 11-bit tile numbers in name table words */
static u8 *g_vram = NULL;            /* optional 64 KB VRAM dump (-v) */
typedef struct {
    u8 *d; size_t size;
    u32 ent[16]; int nent;
    u8 *tiles; int ntiles;           /* unpacked field tileset (ntiles tiles from the MAP) */
    u8 have[MAX_TILES];              /* tile number has graphics (from MAP or VRAM dump) */
    Rgb pal[4][16]; int palFilled[4];
    Rgb fallback;                    /* flat colour for tiles that have no graphics */
} MapFile;

static int load_map(MapFile *m, const char *path)
{
    memset(m, 0, sizeof *m);
    m->d = read_file(path, &m->size);
    if (!m->d || m->size < 0x200) { fprintf(stderr, "  cannot read %s\n", path); return -1; }

    u32 w0 = be16(m->d);
    if (w0 + 44 > m->size) { fprintf(stderr, "  bad header\n"); return -1; }
    while (m->nent < 16 && w0 + 4u * (m->nent + 1) <= m->size) {
        u32 v = be32(m->d + w0 + 4 * m->nent);
        u32 hi = v >> 16;
        if (v == 0 || hi < 3 || hi > 5) break;
        m->ent[m->nent++] = v - 0x30000;
    }
    if (m->nent < 10) { fprintf(stderr, "  section table too short (%d)\n", m->nent); return -1; }

    /* field tileset */
    u8 *buf = (u8 *)malloc(0x20000);
    size_t endp;
    long n = kosinski(m->d, m->size, m->ent[8], buf, 0x20000, &endp);
    if (n < 0) { fprintf(stderr, "  tileset decompression failed\n"); free(buf); return -1; }
    {
        int nt; u8 *up = unpack_tiles(buf, (size_t)n, &nt);
        if (nt > MAX_TILES) nt = MAX_TILES;
        m->tiles = (u8 *)calloc(MAX_TILES, 64);
        memcpy(m->tiles, up, (size_t)nt * 64);
        m->ntiles = nt;
        for (int t = 0; t < nt; t++) m->have[t] = 1;
        free(up);
    }
    free(buf);
    /* Tiles above the MAP's own tileset (e.g. the animated water frames at
     * 0x480-0x49F) are not stored in the MAP file. Take them from a VRAM dump
     * if one was given. */
    if (g_vram)
        for (int t = m->ntiles; t < MAX_TILES && t * 32 + 32 <= 65536; t++)
            if (!is_zero_buf(g_vram + t * 32, 32)) {
                for (int i = 0; i < 32; i++) {
                    m->tiles[t * 64 + i * 2]     = g_vram[t * 32 + i] >> 4;
                    m->tiles[t * 64 + i * 2 + 1] = g_vram[t * 32 + i] & 15;
                }
                m->have[t] = 1;
            }

    /* palette */
    u32 po = m->ent[9];
    for (int l = 0; l < 4; l++) {
        int nz = 0;
        for (int c = 0; c < 16; c++) {
            u32 o = po + (u32)(l * 16 + c) * 2;
            u16 v = (o + 2 <= m->size) ? be16(m->d + o) : 0;
            if (v) nz++;
            m->pal[l][c] = md_color(v);
        }
        /* a line only counts as real if it has several colours; the files
         * often hold a single stray entry (e.g. 0x0280 at index 15) in the
         * unused lines. */
        m->palFilled[l] = nz >= 4;
    }
    if (!m->palFilled[0]) {   /* nothing at all: grey ramp */
        for (int c = 0; c < 16; c++) { u8 g = (u8)(c * 17); m->pal[0][c].r = m->pal[0][c].g = m->pal[0][c].b = g; }
    }
    /* Blocks carry palette-line bits (always line 3 in the samples) but only
     * line 0 holds the real colours, so unused lines mirror line 0. */
    for (int l = 1; l < 4; l++)
        if (!m->palFilled[l]) memcpy(m->pal[l], m->pal[0], sizeof m->pal[0]);

    m->fallback.r = m->fallback.g = m->fallback.b = 0;   /* missing tiles are drawn black */
    return 0;
}

static void free_map(MapFile *m) { free(m->d); free(m->tiles); }

/* detect pages-per-row by matching page edges against adjacency pairs that
 * occur inside pages */
static int detect_ppr(const u8 *layer, int npages)
{
    if (npages <= 2) return npages < 1 ? 1 : npages;
    static u8 h[1024 * 1024 / 8 + 1], v[1024 * 1024 / 8 + 1];
    memset(h, 0, sizeof h); memset(v, 0, sizeof v);
#define ID(p, r, c) (be16(layer + (size_t)(p) * 512 + ((r) * 16 + (c)) * 2) & 0x3FF)
#define SETB(a, i) ((a)[(i) >> 3] |= (u8)(1 << ((i) & 7)))
#define GETB(a, i) (((a)[(i) >> 3] >> ((i) & 7)) & 1)
    for (int p = 0; p < npages; p++)
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                if (c < 15) { u32 k = (u32)(ID(p, r, c) << 10 | ID(p, r, c + 1)); SETB(h, k); }
                if (r < 15) { u32 k = (u32)(ID(p, r, c) << 10 | ID(p, r + 1, c)); SETB(v, k); }
            }
    int best = 1; double bestsc = -1;
    int maxw = npages < 40 ? npages : 40;
    for (int w = 1; w <= maxw; w++) {
        long hits = 0, tot = 0;
        for (int p = 0; p + 1 < npages; p++) {
            if ((p + 1) % w == 0) continue;
            for (int r = 0; r < 16; r++) { u32 k = (u32)(ID(p, r, 15) << 10 | ID(p + 1, r, 0)); hits += GETB(h, k); tot++; }
        }
        for (int p = 0; p + w < npages; p++)
            for (int c = 0; c < 16; c++) { u32 k = (u32)(ID(p, 15, c) << 10 | ID(p + w, 0, c)); hits += GETB(v, k); tot++; }
        double sc = tot ? (double)hits / (double)tot : 0;
        if (sc > bestsc) { bestsc = sc; best = w; }
    }
    return best;
}


/* keep != NULL: the rendered image is handed back in *keep (caller frees) */
static int render_layer(const MapFile *m, int sec, int pprOverride, const char *outpath, Rgb bg, Image *keep)
{
    u32 a = m->ent[sec], b = m->ent[sec + 1];
    if (b <= a + 0x200 || b > m->size) return 1;                 /* empty layer (blank page only) */
    size_t nbytes = b - a;
    int npages = (int)(nbytes / 0x200);                          /* including blank page 0 */
    const u8 *layer = m->d + a;
    if (is_zero(layer + 0x200, (size_t)(npages - 1) * 0x200)) return 1;

    /* The two 0x100-byte sections before the layers are page directories:
     * 16x16 bytes, 0 = no page, N = N-th stored page (1-based). Layer 1 uses
     * directory 0 and layer 2 directory 1, except that a map whose only layer
     * sits in slot 2 (00MAP00) uses directory 0. Pick the directory whose
     * page count matches the stored pages. */
    int npg = npages - 1;                 /* page 0 (first 0x200 bytes) is blank */
    u8 dirmap[256]; int haveDir = 0;
    if (pprOverride <= 0 && npg > 0) {
        int order[2] = { sec - 2, 1 - (sec - 2) };
        for (int k = 0; k < 2 && !haveDir; k++) {
            u32 da = m->ent[order[k]];
            if (da + 256 > m->size) continue;
            int cnt = 0, mx = 0;
            for (int i = 0; i < 256; i++) { int v = m->d[da + i]; if (v) cnt++; if (v > mx) mx = v; }
            if (cnt == npg && mx == npg) { memcpy(dirmap, m->d + da, 256); haveDir = 1; }
        }
    }
    int ppr, prow, gx0 = 0, gy0 = 0;
    if (haveDir) {
        int x1 = 0, y1 = 0; gx0 = 16; gy0 = 16;
        for (int i = 0; i < 256; i++) if (dirmap[i]) {
            int x = i % 16, y = i / 16;
            if (x < gx0) gx0 = x;
            if (y < gy0) gy0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
        ppr = x1 - gx0 + 1; prow = y1 - gy0 + 1;
    } else {
        ppr = pprOverride > 0 ? pprOverride : detect_ppr(layer + 0x200, npg);
        prow = (npg + ppr - 1) / ppr;
    }
    int wb = ppr * 16, hb = prow * 16;              /* size in 16x16 blocks */
    if (sec == 2 || (sec == 3 && g_ox < 0)) {
        g_ox = gx0 * 16; g_oy = gy0 * 16;
        free(g_grid); g_gw = wb; g_gh = hb; g_grid = (u16 *)calloc((size_t)wb * hb, sizeof(u16));
        if (g_grid) for (int i = 0; i < wb * hb; i++) g_grid[i] = 0xFFFF;
    }
    int isL1 = (g_grid && g_gw == wb && g_gh == hb && (sec == 2 || (sec == 3 && g_ox >= 0)));
    printf("  layer %d: %d pages, %dx%d pages (%s) -> %dx%d px\n",
           sec - 1, npg, ppr, prow, haveDir ? "page directory" : "guessed", wb * 16, hb * 16);

    const u8 *blocks = m->d + m->ent[5];
    int nblocks = (int)((m->ent[6] - m->ent[5]) / 8);
    Image im = img_new(wb * 16, hb * 16, bg.r, bg.g, bg.b);
    long missingCells = 0; static int missingTile[MAX_TILES];
    memset(missingTile, 0, sizeof missingTile);
    for (int p = 1; p < npages; p++) {          /* stored page p (1-based) */
        int px0, py0;
        if (haveDir) {
            int cell = -1;
            for (int i = 0; i < 256; i++) if (dirmap[i] == p) { cell = i; break; }
            if (cell < 0) continue;
            px0 = (cell % 16 - gx0) * 16; py0 = (cell / 16 - gy0) * 16;
        } else { px0 = ((p - 1) % ppr) * 16; py0 = ((p - 1) / ppr) * 16; }
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                int id = be16(layer + (size_t)p * 512 + (r * 16 + c) * 2) & 0x3FF;
                if (isL1) g_grid[(py0 + r) * wb + px0 + c] = (u16)id;
                if (id >= nblocks) continue;
                for (int q = 0; q < 4; q++) {
                    u16 w = be16(blocks + id * 8 + q * 2);
                    int t = w & 0x7FF;
                    int dx = (px0 + c) * 16 + (q & 1) * 8, dy = (py0 + r) * 16 + (q >> 1) * 8;
                    if (!m->have[t]) {          /* no graphics for this tile: flat colour */
                        static const u8 flat[64] = { 0 };
                        Rgb f = m->fallback; Rgb pl[16];
                        for (int i = 0; i < 16; i++) pl[i] = f;
                        blit_tile(&im, dx, dy, flat, 0, 0, pl, f);
                        missingTile[t] = 1; missingCells++;
                        continue;
                    }
                    blit_tile(&im, dx, dy, m->tiles + t * 64, (w >> 11) & 1, (w >> 12) & 1,
                              m->pal[(w >> 13) & 3], bg);
                }
            }
    }
    if (missingCells) {
        int nm = 0, lo = MAX_TILES, hi = 0;
        for (int t = 0; t < MAX_TILES; t++) if (missingTile[t]) { nm++; if (t < lo) lo = t; if (t > hi) hi = t; }
        printf("  note: %d tiles (0x%X-0x%X, %ld cells) are not stored in the MAP file "
               "(animated water?); drawn black. Use -v VRAMDUMP for real graphics.\n",
               nm, lo, hi, missingCells);
    }
    int rc = write_bmp(outpath, &im);
    if (keep) *keep = im; else free(im.rgb);
    return rc;
}


/* ---- layer-2 switch / interior-area records (originally thought to be doors):
 * tag 1 and tag 2 rows lie on either side of a building entrance; crossing them
 * most likely shows/hides layer 2 (which one does which is not determined). The
 * destination rectangle (green) is the interior area. Original notes:
 * ---- door / event records -------------------------------------------------
 * Type A (cities, interiors): header words 1,2 give a list of 12-byte records
 *   tag x0 0 y0 x1 0 y1 destX 0 destY destW destH   (bytes; all in 16x16 blocks, x1/y1 exclusive)
 *   i.e. a door rectangle and the destination room rectangle (same map). tag 1/2.
 * Type B (world map): 12-byte records  80 x y 00 kind ff dest.. = a trigger point.
 * Field meanings beyond the rectangles are inferred. */
static void rect_outline(Image *im, int x0, int y0, int x1, int y1, Rgb c, int th)
{
    for (int t = 0; t < th; t++)
        for (int x = x0; x < x1; x++)
            for (int k = 0; k < 2; k++) {
                int y = k ? y1 - 1 - t : y0 + t;
                if (x >= 0 && y >= 0 && x < im->w && y < im->h) { u8 *p = im->rgb + ((size_t)y * im->w + x) * 3; p[0] = c.r; p[1] = c.g; p[2] = c.b; }
            }
    for (int t = 0; t < th; t++)
        for (int y = y0; y < y1; y++)
            for (int k = 0; k < 2; k++) {
                int x = k ? x1 - 1 - t : x0 + t;
                if (x >= 0 && y >= 0 && x < im->w && y < im->h) { u8 *p = im->rgb + ((size_t)y * im->w + x) * 3; p[0] = c.r; p[1] = c.g; p[2] = c.b; }
            }
}

static int write_doors(const MapFile *m, const Image *l1, const char *outpath)
{
    if (!l1->rgb || m->size < 24) return 1;
    Image im = img_new(l1->w, l1->h, 0, 0, 0);
    for (size_t i = 0; i < (size_t)l1->w * l1->h * 3; i++) im.rgb[i] = (u8)(l1->rgb[i] / 3);   /* dimmed layer 1 */
    int ox = g_ox < 0 ? 0 : g_ox, oy = g_oy;
    int n = 0;
    u32 a = be16(m->d + 2), b = be16(m->d + 4);
    int typeA = b > a && b <= m->size && (b - a) / 12 >= 2;
    if (typeA) b = a + (b - a) / 12 * 12;      /* whole records only (list may end with padding) */
    for (u32 o = a; typeA && o < b; o += 12) {
        const u8 *r = m->d + o;
        int tag = r[0], x0 = r[1], y0 = r[3], x1 = r[4], y1 = r[6];
        if ((tag != 1 && tag != 2) || x1 <= x0 || y1 <= y0) typeA = 0;
    }
    if (typeA) {
        for (u32 o = a; o < b; o += 12) {
            const u8 *r = m->d + o;
            int tag = r[0], x0 = r[1], y0 = r[3], x1 = r[4], y1 = r[6];
            int dx = r[7], dy = r[9], dw = r[10], dh = r[11];
            rect_outline(&im, (dx - ox) * 16, (dy - oy) * 16, (dx + dw - ox) * 16, (dy + dh - oy) * 16, (Rgb){ 40, 200, 40 }, 3);
            Rgb c = tag == 1 ? (Rgb){ 255, 40, 40 } : (Rgb){ 40, 255, 255 };
            rect_outline(&im, (x0 - ox) * 16, (y0 - oy) * 16, (x1 - ox) * 16, (y1 - oy) * 16, c, 4);
            n++;
        }
    } else {
        for (u32 o = 0x100; o + 12 <= m->size && o < 0x400; o++) {
            const u8 *r = m->d + o;
            if (r[0] == 0x80 && r[3] == 0 && (r[4] == 1 || r[4] == 2) && r[5] == 0xFF) {
                int x = r[1], y = r[2];
                rect_outline(&im, (x - ox) * 16, (y - oy) * 16, (x - ox + 1) * 16, (y - oy + 1) * 16, (Rgb){ 255, 40, 255 }, 4);
                n++;
            }
        }
    }
    int rc = n ? write_bmp(outpath, &im) : 1;
    free(im.rgb);
    if (n) printf("  %d records marked (%s)\n", n, typeA ? "layer-2 switch rows + interior areas" : "world-map trigger points");
    return rc;
}

/* collision layer: per-block attribute table (section 7, 4 bytes/block, byte 3).
 * 1 = solid (red), 5 = counter-like (orange), other non-zero (yellow); lane 1 flag
 * 32 (roof/facade) is shown as a faint blue tint. Values are inferred, not documented. */

static void draw_num(Image *im, int x, int y, int v, Rgb c, int sc);
/* ---- world objects (chest / item slots): list at MAP header word 0x0E, 16-byte records
 *   +0 room, +1 x (blocks), +2 y (word, blocks), +4 index (word), +12 word "C" (unknown, probably contents)
 * First word of the list = base ID; object ID = base + index. Loader: _000PRG.DAT 0x8C70, slots at $BD000. */
static int write_objects(const MapFile *m, const Image *l1, const char *outpath, const char *txtpath)
{
    if (m->size < 0x12) return 1;
    u32 o = be16(m->d + 0x0E);
    if (!o || o + 2 > m->size) return 1;
    int ox = g_ox < 0 ? 0 : g_ox, oy = g_oy;
    Image im = img_new(l1->w, l1->h, 0, 0, 0);
    for (size_t i = 0; i < (size_t)l1->w * l1->h * 3; i++) im.rgb[i] = (u8)(l1->rgb[i] * 2 / 3);
    u16 base = be16(m->d + o);
    static const Rgb col[8] = { {255,0,255},{255,160,0},{0,255,255},{255,255,0},{120,255,120},{255,90,90},{140,160,255},{255,255,255} };
    FILE *f = fopen(txtpath, "w");
    if (f) fprintf(f, "World objects of this MAP (header word 0x0E = 0x%04X, base ID 0x%04X)\nmap image origin (blocks): %d,%d\nn  room  x   y   index  id     wordC\n", (unsigned)o, base, ox, oy);
    int n = 0, drawn = 0;
    for (size_t q = o + 2; q + 16 <= m->size && m->d[q] != 0xFF && n < 400; q += 16, n++) {
        const u8 *r = m->d + q;
        int room = r[0], x = r[1], y = be16(r + 2), idx = be16(r + 4), wc = be16(r + 12);
        if (f) fprintf(f, "%-3d %-4d %-3d %-3d %-5d 0x%04X 0x%04X\n", n, room, x, y, idx, base + idx, wc);
        int px = (x - ox) * 16, py = (y - oy) * 16;
        if (px < 0 || py < 0 || px + 16 > im.w || py + 16 > im.h) continue;
        Rgb c = col[room & 7];
        for (int yy = py; yy < py + 16; yy++) for (int xx = px; xx < px + 16; xx++) {
            u8 *p = im.rgb + ((size_t)yy * im.w + xx) * 3;
            p[0] = (u8)((p[0] + c.r) / 2); p[1] = (u8)((p[1] + c.g) / 2); p[2] = (u8)((p[2] + c.b) / 2);
        }
        rect_outline(&im, px, py, px + 16, py + 16, c, 1);
        draw_num(&im, px + 1, py + 1, n, (Rgb){ 255, 255, 255 }, 1);   /* number = row in _objects.txt */
        drawn++;
    }
    if (f) fclose(f);
    int rc = write_bmp(outpath, &im);
    free(im.rgb);
    printf("  objects: %d records, %d drawn (box colour = room number mod 8, number = row in the .txt)\n", n, drawn);
    return rc;
}

static int write_collision(const MapFile *m, const Image *l1, const char *outpath)
{
    if (!l1->rgb || !g_grid || g_gw * 16 != l1->w || g_gh * 16 != l1->h || m->ent[8] <= m->ent[7]) return 1;
    const u8 *at = m->d + m->ent[7];
    int nat = (int)((m->ent[8] - m->ent[7]) / 4); if (nat > 1024) nat = 1024;
    Image im = img_new(l1->w, l1->h, 0, 0, 0);
    for (size_t i = 0; i < (size_t)l1->w * l1->h * 3; i++) im.rgb[i] = (u8)(l1->rgb[i] / 2);
    long cnt[256] = { 0 };
    for (int by = 0; by < g_gh; by++)
        for (int bx = 0; bx < g_gw; bx++) {
            int id = g_grid[by * g_gw + bx];
            if (id == 0xFFFF || id >= nat) continue;
            int cv = at[id * 4 + 3], fl = at[id * 4 + 1];
            Rgb c; int a;
            if (cv == 1) { c = (Rgb){ 255, 0, 0 }; a = 150; }
            else if (cv == 5) { c = (Rgb){ 255, 150, 0 }; a = 150; }
            else if (cv) { c = (Rgb){ 255, 255, 0 }; a = 150; }
            else if (fl & 32) { c = (Rgb){ 60, 60, 255 }; a = 70; }
            else continue;
            cnt[cv]++;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    u8 *p = im.rgb + ((size_t)(by * 16 + y) * im.w + bx * 16 + x) * 3;
                    p[0] = (u8)((p[0] * (255 - a) + c.r * a) / 255);
                    p[1] = (u8)((p[1] * (255 - a) + c.g * a) / 255);
                    p[2] = (u8)((p[2] * (255 - a) + c.b * a) / 255);
                    if (x == 0 || y == 0) { p[0] = p[0] / 2; p[1] = p[1] / 2; p[2] = p[2] / 2; }
                }
        }
    int rc = write_bmp(outpath, &im);
    free(im.rgb);
    printf("  collision: %ld solid, %ld counter, other flags marked\n", cnt[1], cnt[5]);
    return rc;
}

/* both layers in one picture, side by side or stacked (whichever is squarer),
 * NOT blended: layer 2 is switched off in the game e.g. inside buildings */
static int write_combined(const Image *a, const Image *b, const char *outpath, Rgb bg)
{
    const int gap = 32;
    int sw = a->w + gap + b->w, sh = a->h > b->h ? a->h : b->h;
    int tw = a->w > b->w ? a->w : b->w, th = a->h + gap + b->h;
    double rs = sw > sh ? (double)sw / sh : (double)sh / sw;
    double rt = tw > th ? (double)tw / th : (double)th / tw;
    int side = rs <= rt;
    Image im = side ? img_new(sw, sh, bg.r, bg.g, bg.b) : img_new(tw, th, bg.r, bg.g, bg.b);
    int bx = side ? a->w + gap : 0, by = side ? 0 : a->h + gap;
    for (int y = 0; y < a->h; y++) memcpy(im.rgb + (size_t)y * im.w * 3, a->rgb + (size_t)y * a->w * 3, (size_t)a->w * 3);
    for (int y = 0; y < b->h; y++) memcpy(im.rgb + ((size_t)(by + y) * im.w + bx) * 3, b->rgb + (size_t)y * b->w * 3, (size_t)b->w * 3);
    /* grey divider in the gap */
    for (int y = 0; y < im.h; y++)
        for (int x = 0; x < im.w; x++) {
            int ina = x < a->w && y < a->h, inb = x >= bx && x < bx + b->w && y >= by && y < by + b->h;
            if (!ina && !inb) { u8 *d = im.rgb + ((size_t)y * im.w + x) * 3; d[0] = d[1] = d[2] = 96; }
        }
    int rc = write_bmp(outpath, &im);
    free(im.rgb);
    return rc;
}

static int render_tileset(const MapFile *m, const char *outpath)
{
    int cols = 32, rows = (m->ntiles + cols - 1) / cols;
    Image im = img_new(cols * 8, rows * 8, 32, 32, 32);
    for (int t = 0; t < m->ntiles; t++)
        blit_tile(&im, (t % cols) * 8, (t / cols) * 8, m->tiles + t * 64, 0, 0, m->pal[0], (Rgb){ 32, 32, 32 });
    int rc = write_bmp(outpath, &im);
    free(im.rgb);
    return rc;
}

/* backdrop sets */
static int render_backdrop(const MapFile *m, u32 P, const char *outpath)
{
    if (P + 8 > m->size) return -1;
    u16 tmOff = be16(m->d + P + 2), palOff = be16(m->d + P + 4);
    u8 *tb = (u8 *)malloc(0x20000), *mb = (u8 *)malloc(0x20000);
    int rc = -1;
    long nt = kosinski(m->d, m->size, P + 6, tb, 0x20000, NULL);
    long nm = kosinski(m->d, m->size, P + tmOff, mb, 0x20000, NULL);
    if (nt < 0 || nm < 1152) goto done;
    int ntiles; u8 *tpx = unpack_tiles(tb, (size_t)nt, &ntiles);
    Rgb pal[16];
    for (int c = 0; c < 16; c++) pal[c] = md_color(P + palOff + c * 2 + 2 <= m->size ? be16(m->d + P + palOff + c * 2) : 0);

    /* rank of each used VRAM tile number */
    static int rank[0x800];
    memset(rank, 0, sizeof rank);
    for (int i = 0; i < 576; i++) { int t = be16(mb + i * 2) & 0x7FF; if (t) rank[t] = 1; }
    int r = 1;
    for (int t = 1; t < 0x800; t++) if (rank[t]) rank[t] = r++;

    Image im = img_new(256, 144, 0, 0, 0);
    for (int i = 0; i < 576; i++) {
        u16 w = be16(mb + i * 2);
        int t = w & 0x7FF; if (!t) continue;
        int idx = rank[t];
        if (idx >= ntiles) continue;
        blit_tile(&im, (i % 32) * 8, (i / 32) * 8, tpx + idx * 64, (w >> 11) & 1, (w >> 12) & 1, pal, pal[0]);
    }
    rc = write_bmp(outpath, &im);
    free(im.rgb); free(tpx);
done:
    free(tb); free(mb);
    return rc;
}

/* ----------------------------------------------------------------- driver */
static const char *g_outdir = NULL;
static int g_ppr = 0;

static void base_name(const char *path, char *out, size_t cap)
{
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') s = p + 1;
    snprintf(out, cap, "%s", s);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}


/* ---- automatic MAP -> EV association -------------------------------------------------------
 * The MAP header word at 0x12 points to a table of 28 words: the EV file number for each stage index
 * ($BE80C in the game). The distinct numbers are read from the MAP itself; EV file name = EV%05X.DAT. */
static const char *g_evdir = NULL;      /* -E DIR: extra folder to look for EV files */

static int path_dir(const char *path, char *dir, size_t cap)
{
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') s = p + 1;
    size_t n = (size_t)(s - path); if (n >= cap) n = cap - 1;
    memcpy(dir, path, n); dir[n] = 0;
    return (int)n;
}

static int find_ev_file(const char *mappath, int ev, char *out, size_t cap)
{
    char dirs[3][600]; int nd = 0;
    if (g_evdir) snprintf(dirs[nd++], 600, "%s%s", g_evdir, (g_evdir[strlen(g_evdir) - 1] == '/' || g_evdir[strlen(g_evdir) - 1] == '\\') ? "" : "/");
    path_dir(mappath, dirs[nd++], 600);
    dirs[nd][0] = 0; nd++;
    for (int d = 0; d < nd; d++) {
        const char *fmt[3] = { "%sEV%05X.DAT", "%sev%05x.dat", "%sEV%05x.DAT" };
        for (int f = 0; f < 3; f++) {
            char t[700]; snprintf(t, sizeof t, fmt[f], dirs[d], ev);
            FILE *fp = fopen(t, "rb");
            if (fp) { fclose(fp); snprintf(out, cap, "%s", t); return 1; }
        }
    }
    return 0;
}

static void make_out(char *dst, size_t cap, const char *base, const char *suffix)
{
    if (g_outdir && *g_outdir) snprintf(dst, cap, "%s/%s%s.bmp", g_outdir, base, suffix);
    else snprintf(dst, cap, "%s%s.bmp", base, suffix);
}


/* ======================================================================
 * EV file interactive tile data  (-e EVxxxxx.DAT)
 * ----------------------------------------------------------------------
 * The EV file's event block (file offset 0xC800, loaded by the game at 0x92000)
 * starts with a table of 16-bit offsets (relative to the block start):
 *   +0x00 event script table  (word offsets of scripts; script 0 = area init)
 *   +0x02 list A  "door" tiles   (script type 1 / 5 = Door_TryOpenAndDisplayText)
 *   +0x04 door records, 4 bytes each:  [b0 b1] [b2] [b3 = condition set]
 *   +0x06 condition sets  (word offset table, then {b,b,word,flag words..,FFFF,text})
 *   +0x08 (effect script table, not decoded)
 *   +0x0A movement pattern table: word offsets, patterns of (anim, dir, time) triples,
 *         terminated by FF (loop) or FE (stop)
 *   +0x0C list C  "examine" tiles (script type 2 = TileScript_MessageGateByFacing):
 *         type word high byte = allowed facing mask (bits 01/02/04/08), index = message
 *         number in the "door/chest" message table of the text block (text block +6)
 *   +0x0E spawn records: word offsets (from block start); each record is condition words
 *         ending in FFFF, then 4 bytes: actor slot, type, state, script index
 *   +0x10 list D  "object spawn" tiles (script type 3 = MapObject_SpawnIfConditionsMet)
 * Every list is a stream of entries, terminated by an FF byte:
 *   point:     u16 x (bit15 clear), u8 y,                       u16 type, u16 index   (7 bytes)
 *   rectangle: u16 x|0x8000, u8 y, u16 x1, u8 y1 (exclusive),  u16 type, u16 index  (10 bytes)
 * x,y are in 16x16 blocks of the MAP (same coordinates as the layer images).
 * (Tile_FindScript, 0x25846 in _000PRG.DAT, is the matching routine.)
 * ====================================================================== */
typedef struct { int x0, y0, x1, y1, type, idx, rect; } EvEnt;
#define EV_MAXENT 256
static const char *g_evpath = NULL;

static int ev_list(const u8 *b, size_t sz, u32 o, EvEnt *out)
{
    int n = 0;
    while (o + 3 <= sz && b[o] != 0xFF && n < EV_MAXENT) {
        u16 xw = be16(b + o);
        EvEnt *e = &out[n];
        e->rect = (xw & 0x8000) != 0; e->x0 = xw & 0x7FFF; e->y0 = b[o + 2];
        if (!e->rect) {
            if (o + 7 > sz) break;
            e->x1 = e->x0 + 1; e->y1 = e->y0 + 1; e->type = be16(b + o + 3); e->idx = be16(b + o + 5); o += 7;
        } else {
            if (o + 10 > sz) break;
            e->x1 = be16(b + o + 3); e->y1 = b[o + 5]; e->type = be16(b + o + 6); e->idx = be16(b + o + 8); o += 10;
        }
        /* the game (Tile_FindScript, _000PRG.DAT 0x24FB8) stores each entry as [Y word][X byte]: swap into x/y */
        { int t = e->x0; e->x0 = e->y0; e->y0 = t; t = e->x1; e->x1 = e->y1; e->y1 = t; }
        n++;
    }
    return n;
}

/* 3x5 digit font for labels */
static const u16 g_digit[10] = { 0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF };
static void draw_num(Image *im, int x, int y, int v, Rgb c, int sc)
{
    char t[12]; snprintf(t, sizeof t, "%d", v);
    for (int k = 0; t[k]; k++) {
        u16 g = g_digit[t[k] - '0'];
        for (int r = 0; r < 5; r++) for (int q = 0; q < 3; q++)
            if (g & (1 << (14 - (r * 3 + q))))
                for (int yy = 0; yy < sc; yy++) for (int xx = 0; xx < sc; xx++) {
                    int px = x + (k * 4 + q) * sc + xx, py = y + r * sc + yy;
                    if (px >= 0 && py >= 0 && px < im->w && py < im->h) { u8 *p = im->rgb + ((size_t)py * im->w + px) * 3; p[0] = c.r; p[1] = c.g; p[2] = c.b; }
                }
    }
}

/* true if the 16x16 block at pixel (px,py) of the image is one flat colour (= empty filler block) */
static int blk_flat(const Image *im, int px, int py)
{
    px &= ~15; py &= ~15;
    if (px < 0 || py < 0 || px + 16 > im->w || py + 16 > im->h) return 1;
    const u8 *f = im->rgb + ((size_t)py * im->w + px) * 3;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++)
        if (memcmp(im->rgb + ((size_t)(py + y) * im->w + px + x) * 3, f, 3)) return 0;
    return 1;
}
static int g_flatcnt;
static int g_obj[200][4], g_nobj;
static char g_warptxt[16384];

static void ev_mark(Image *im, const EvEnt *e, Rgb c, int ox, int oy, int *drawn, int *outside)
{
    int x0 = (e->x0 - ox) * 16, y0 = (e->y0 - oy) * 16, x1 = (e->x1 - ox) * 16, y1 = (e->y1 - oy) * 16;
    if (x1 <= 0 || y1 <= 0 || x0 >= im->w || y0 >= im->h) { (*outside)++; return; }
    if (blk_flat(im, (x0 + x1) / 2, (y0 + y1) / 2)) {   /* no real map tile here: grey outline only */
        rect_outline(im, x0, y0, x1, y1, (Rgb){ 140, 140, 140 }, 1);
        g_flatcnt++; (*drawn)++; return;
    }
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++)
        if (x >= 0 && y >= 0 && x < im->w && y < im->h) {
            u8 *p = im->rgb + ((size_t)y * im->w + x) * 3;
            p[0] = (u8)((p[0] + c.r) / 2); p[1] = (u8)((p[1] + c.g) / 2); p[2] = (u8)((p[2] + c.b) / 2);
        }
    rect_outline(im, x0, y0, x1, y1, c, 2);
    draw_num(im, x0 + 3, y0 + 3, e->idx, (Rgb){ 255, 255, 255 }, 2);
    (*drawn)++;
}

static int write_events(const MapFile *m, const Image *l1, const char *evpath, const char *outpath, const char *txtpath)
{
    size_t evs; u8 *ev = read_file(evpath, &evs);
    if (!ev) { printf("  cannot read %s\n", evpath); return 1; }
    /* find the event block: 0xC800 normally; validate the offset table */
    static const size_t cand[2] = { 0xC800, 0x4800 };   /* 0x4800 for EV files without a 0xC800 block */
    size_t base = 0xC800; int ok = 0;
    for (int ci = 0; ci < 2 && !ok; ci++) {
        size_t bb = cand[ci];
        if (evs > bb + 0x40) {
            const u8 *b0 = ev + bb;
            u16 w0 = be16(b0), w1 = be16(b0 + 2), w2 = be16(b0 + 4), w3 = be16(b0 + 6);
            ok = w0 >= 0x14 && w0 < 0x200 && w1 >= w0 && w2 >= w1 && w3 >= w2 && w3 < 0x4000;   /* equal offsets = empty lists */
            if (ok) base = bb;
        }
    }
    if (!ok) { printf("  %s: no event block at 0xC800 or 0x4800 (EV file without event script, e.g. EV00000)\n", evpath); free(ev); return 1; }
    if (base != 0xC800) printf("  %s: event block found at 0x%zX\n", evpath, base);
    const u8 *b = ev + base; size_t bsz = evs - base; if (bsz > 0x8000) bsz = 0x8000;
    u16 H[9]; for (int i = 0; i < 9; i++) H[i] = be16(b + 2 * i);
    EvEnt *A = (EvEnt *)calloc(EV_MAXENT, sizeof(EvEnt)), *C = (EvEnt *)calloc(EV_MAXENT, sizeof(EvEnt)), *D = (EvEnt *)calloc(EV_MAXENT, sizeof(EvEnt));
    int nA = ev_list(b, bsz, H[1], A), nC = ev_list(b, bsz, be16(b + 0x0C), C), nD = ev_list(b, bsz, be16(b + 0x10), D);
    int ox = g_ox < 0 ? 0 : g_ox, oy = g_oy;
    Image im = img_new(l1->w, l1->h, 0, 0, 0);
    for (size_t i = 0; i < (size_t)l1->w * l1->h * 3; i++) im.rgb[i] = (u8)(l1->rgb[i] * 2 / 3);
    int drawn = 0, outside = 0;
    for (int i = 0; i < nA; i++) ev_mark(&im, &A[i], (Rgb){ 255, 50, 50 }, ox, oy, &drawn, &outside);
    for (int i = 0; i < nA; i++) {          /* the door tile itself (door record x,y) as a filled mark */
        size_t ro = (size_t)H[2] + (size_t)A[i].idx * 4;
        if (ro + 4 > bsz) continue;
        int dx = (b[ro + 2] - ox) * 16, dy = (be16(b + ro) - oy) * 16;   /* record = [Y word][X byte] */
        for (int y = dy + 4; y < dy + 12; y++) for (int x = dx + 4; x < dx + 12; x++)
            if (x >= 0 && y >= 0 && x < im.w && y < im.h) { u8 *p = im.rgb + ((size_t)y * im.w + x) * 3; p[0] = 255; p[1] = 255; p[2] = 255; }
    }
    for (int i = 0; i < nC; i++) ev_mark(&im, &C[i], (Rgb){ 40, 230, 230 }, ox, oy, &drawn, &outside);
    for (int i = 0; i < nD; i++) ev_mark(&im, &D[i], (Rgb){ 255, 230, 40 }, ox, oy, &drawn, &outside);
    /* ---- warp triggers (stairs / building entrances / map exits): MAP header +8, _000PRG.DAT 0x25344
     *   single tile  (b0 < 0x80, 10 bytes): [Y hi][Y lo][X][dest area|0xFF][camY hi/lo][camX][destY hi/lo][destX]
     *      dest area 0xFF = leave this MAP: [map id][spawn id][..]
     *   rectangle    (b0 bit7, 12 bytes):   [0x80|Y hi][Y lo][X][Y extent][X extent][dest area|0xFF]
     *      [camY hi/lo][camX][destY hi/lo][destX]   (camera / destination are RELATIVE to the position inside the rectangle) */
    g_warptxt[0] = 0; int nwarp = 0;
    if (m->size > 0x40) {
        u32 o = be16(m->d + 8);
        char *wt = g_warptxt;
        wt += sprintf(wt, "\n== warp triggers (MAP header +8): stairs / entrances / exits; W<n> in the picture ==\n");
        while (o + 10 <= m->size && m->d[o] != 0xFF && nwarp < 100) {
            const u8 *r = m->d + o; int rect = (r[0] & 0x80) != 0, ysz = 0, xsz = 0, ln = rect ? 12 : 10;
            if (o + ln > m->size) break;
            int ty = ((r[0] & 0x7F) << 8) | r[1], tx = r[2], da, cy, cx, dy, dx;
            if (rect) { ysz = r[3]; xsz = r[4]; da = r[5]; cy = ((r[6] << 8) | r[7]) & 0x1FF; cx = r[8]; dy = ((r[9] << 8) | r[10]) & 0x1FF; dx = r[11]; }
            else      { da = r[3]; cy = ((r[4] << 8) | r[5]) & 0x1FF; cx = r[6]; dy = ((r[7] << 8) | r[8]) & 0x1FF; dx = r[9]; }
            int px0 = (tx - ox) * 16, py0 = (ty - oy) * 16, px1 = px0 + (xsz + 1) * 16, py1 = py0 + (ysz + 1) * 16;
            Rgb bc = { 70, 130, 255 };
            if (px1 > 0 && py1 > 0 && px0 < im.w && py0 < im.h) {
                rect_outline(&im, px0, py0, px1, py1, bc, 2);
                draw_num(&im, px0 + 3, py0 + 3, nwarp, (Rgb){ 255, 255, 255 }, 2);
            }
            if (da == 0xFF) {
                int mapid = rect ? r[6] : r[4], spawn = rect ? r[7] : r[5];
                int third = rect ? r[8] : r[6];      /* -> $BFE86 when bit 7 is clear (_000PRG.DAT 0x254DE / 0x25668) */
                wt += sprintf(wt, "W%-2d %s Y %3d X %3d%s -> EXIT to map id %d spawn %d", nwarp, rect ? "rect" : "tile", ty, tx,
                              rect ? "" : "", mapid, spawn);
                if (third & 0x80) wt += sprintf(wt, "   [memory: $3FE82=%04X $3FE84=%04X]\n", mapid, spawn);
                else wt += sprintf(wt, "   [memory: $3FE82=%04X $3FE84=%04X $3FE86=%04X]\n", mapid, spawn, third);
                if (rect) wt += 0;
            } else {
                /* destination: for rectangles the offset inside the rectangle is added -> show the first tile */
                int fy = rect ? cy : cy, fx = cx; (void)fy; (void)fx;
                int ddy = dy, ddx = dx;
                wt += sprintf(wt, "W%-2d %s Y %3d X %3d%s -> area %d  player Y %d X %d  camera Y %d X %d%s   [area byte 0x%02X: $3FE80=%04X, sub-area $3E812=%04X]\n", nwarp, rect ? "rect" : "tile", ty, tx,
                              rect ? "" : "", da & 0x1F, ddy, ddx, cy, cx, rect ? "  (+ offset inside the rectangle)" : "",
                              da, da & 0x1F, ((da >> 5) & 7) ? ((da >> 5) & 7) - 1 : 0);
                int qx = (ddx - ox) * 16 + 8, qy = (ddy - oy) * 16 + 8, sx = (px0 + px1) / 2, sy = (py0 + py1) / 2;
                if (qx >= 0 && qy >= 0 && qx < im.w && qy < im.h) {
                    for (int d = -6; d <= 6; d++) for (int e2 = -6; e2 <= 6; e2++)
                        if (abs(d) + abs(e2) <= 6 && abs(d) + abs(e2) >= 4) {
                            int X = qx + d, Y = qy + e2;
                            if (X >= 0 && Y >= 0 && X < im.w && Y < im.h) { u8 *p = im.rgb + ((size_t)Y * im.w + X) * 3; p[0] = 70; p[1] = 130; p[2] = 255; }
                        }
                    draw_num(&im, qx - 3, qy - 3, nwarp, (Rgb){ 70, 130, 255 }, 1);
                    /* connecting line */
                    int x0 = sx, y0 = sy, x1 = qx, y1 = qy, dxl = abs(x1 - x0), dyl = -abs(y1 - y0), sxl = x0 < x1 ? 1 : -1, syl = y0 < y1 ? 1 : -1, er = dxl + dyl, st = 0;
                    for (;;) {
                        if (x0 >= 0 && y0 >= 0 && x0 < im.w && y0 < im.h && (st++ % 6) < 3) { u8 *p = im.rgb + ((size_t)y0 * im.w + x0) * 3; p[0] = 120; p[1] = 170; p[2] = 255; }
                        if (x0 == x1 && y0 == y1) break;
                        int e2 = 2 * er; if (e2 >= dyl) { er += dyl; x0 += sxl; } if (e2 <= dxl) { er += dxl; y0 += syl; }
                    }
                }
            }
            nwarp++; o += ln;
        }
    }
    printf("  warp triggers (blue, W<n>; dotted line + diamond = destination): %d\n", nwarp);
    /* ---- tile objects (chests / items): MAP header +0x0E -> [u16 base] then 16-byte entries
     *      [area][X byte][Y word][index word][...], FF terminated (loader at _000PRG.DAT 0x8C70) */
    int nobj = 0;
    if (m->size > 0x40) {
        u32 o = be16(m->d + 0x0E);
        if (o > 0x10 && o + 18 <= m->size) {
            for (o += 2; o + 16 <= m->size && m->d[o] != 0xFF && nobj < 200; o += 16, nobj++) {
                int ar = m->d[o], ox2 = m->d[o + 1], oy2 = (m->d[o + 2] << 8) | m->d[o + 3], ix = be16(m->d + o + 4);
                g_obj[nobj][0] = ix; g_obj[nobj][1] = ar; g_obj[nobj][2] = ox2; g_obj[nobj][3] = oy2;
                int x0 = (ox2 - ox) * 16, y0 = (oy2 - oy) * 16;
                if (x0 < 0 || y0 < 0 || x0 + 16 > im.w || y0 + 16 > im.h) continue;
                Rgb c = { 255, 140, 0 };
                for (int y = y0; y < y0 + 16; y++) for (int x = x0; x < x0 + 16; x++) {
                    u8 *p = im.rgb + ((size_t)y * im.w + x) * 3; p[0] = (u8)((p[0] + c.r) / 2); p[1] = (u8)((p[1] + c.g) / 2); p[2] = (u8)((p[2] + c.b) / 2);
                }
                rect_outline(&im, x0, y0, x0 + 16, y0 + 16, c, 2);
                draw_num(&im, x0 + 4, y0 + 5, ix, (Rgb){ 255, 255, 255 }, 1);
            }
        }
    }
    g_nobj = nobj;
    printf("  tile objects (chests/items, orange): %d\n", nobj);
    /* ---- actors: MAP area records (0x1C + n*16): +0xA = actor list, 6-byte records
     *      [type][script index][u16 y|flags][u16 x|flags], FF terminated (FUN_00008ea6 of _000PRG.DAT).
     *      y/x are pixels (flags: y bits 13-14 = facing, bit 15/12 and x bit 15 = misc). The script
     *      index selects the actor's script in the EV event table (header +0); its first opcode
     *      (0x1C / 0x1F = actor setup) gives the appearance id, text message and wander box. */
    typedef struct { int type, sidx, x, y, dir, area; } Act;
    static Act acts[256]; int nact = 0;
    u32 lists[8]; int nlists = 0;
    if (m->size > 0x40) {
        u32 at = be16(m->d + 6);
        for (int a = 0; a < 8; a++) {
            u32 r = at + (u32)a * 16;
            if (r + 16 > m->size) break;
            u32 lp = be16(m->d + r + 10);
            if (lp < 0x40 || lp + 6 > m->size) break;
            int dup = 0; for (int k = 0; k < nlists; k++) if (lists[k] == lp) dup = 1;
            if (dup) continue;
            lists[nlists++] = lp;
            for (u32 o = lp; o + 6 <= m->size && m->d[o] != 0xFF && nact < 256; o += 6) {
                if (m->d[o] == 0) continue;
                Act *ac = &acts[nact++];
                /* FUN_00008ea6: first word = X (+flags in bits 12-15, facing bits 13-14), second word = Y (bit 15 flag) */
                u16 xw = be16(m->d + o + 2), yw = be16(m->d + o + 4);
                ac->type = m->d[o]; ac->sidx = m->d[o + 1]; ac->x = xw & 0xFFF; ac->y = yw & 0x7FFF;
                ac->dir = (xw >> 13) & 3; ac->area = a;
            }
        }
    }
    int actDrawn = 0;
    for (int i = 0; i < nact; i++) {
        Act *ac = &acts[i];
        int cx = (ac->x + 8 - ox * 16), cy = (ac->y + 8 - oy * 16);
        /* wander box from the first script opcode */
        u32 so = be16(b + be16(b) + 2 * ac->sidx);
        if (be16(b) + 2 * (size_t)ac->sidx + 2 <= bsz && so + 24 < bsz && (b[so] == 0x1C || b[so] == 0x1F)) {
            const u8 *p = b + so + 1; int pat = (b[so] == 0x1C) ? p[3] : 0;
            if (pat == 0) {
                const u8 *q = p + (b[so] == 0x1C ? 7 : 8);
                int dx0 = (int16_t)be16(q), dy0 = (int16_t)be16(q + 2), dx1 = (int16_t)be16(q + 4), dy1 = (int16_t)be16(q + 6);
                int xa = cx + dx0 * 16, xb = cx + dx1 * 16, ya = cy + dy0 * 16, yb = cy + dy1 * 16;
                if (xa > xb) { int t = xa; xa = xb; xb = t; }
                if (ya > yb) { int t = ya; ya = yb; yb = t; }
                rect_outline(&im, xa, ya, xb + 1, yb + 1, (Rgb){ 60, 255, 60 }, 1);
            }
        }
        if (cx < -8 || cy < -8 || cx >= im.w + 8 || cy >= im.h + 8) continue;
        int flat = blk_flat(&im, cx, cy); if (flat) g_flatcnt++;
        for (int y = -5; y <= 5; y++) for (int x = -5; x <= 5; x++)
            if (x * x + y * y <= 25 && cx + x >= 0 && cy + y >= 0 && cx + x < im.w && cy + y < im.h) {
                u8 *p = im.rgb + ((size_t)(cy + y) * im.w + cx + x) * 3;
                if (flat && x * x + y * y > 12) { p[0] = p[1] = p[2] = 140; } else if (!flat) { p[0] = 40; p[1] = 220; p[2] = 40; }
            }
        draw_num(&im, cx - 3, cy - 3, ac->sidx, (Rgb){ 0, 0, 0 }, 1);
        actDrawn++;
    }
    /* spawn / arrival points: MAP header word at +0x0C -> table of word offsets -> 8-byte records
     * [area][id][Y hi][Y lo][X][camY hi][camY lo][camX] (Map_LoadSpawnAndViewport, _000PRG.DAT 0x796C); blocks */
    if (m->size > 0x40) {
        u32 t = be16(m->d + 0x0C);
        if (t + 2 < m->size) {
            int n = be16(m->d + t) / 2;
            for (int i = 0; i < n && i < 32; i++) {
                u32 r = t + be16(m->d + t + 2 * i);
                if (r + 8 > m->size) break;
                int sy = (m->d[r + 2] << 8) | m->d[r + 3], sx = m->d[r + 4];
                int px = (sx - ox) * 16 + 8, py = (sy - oy) * 16 + 8;
                printf("  spawn %d: area %d  x %d y %d%s\n", i, m->d[r], sx, sy, blk_flat(&im, px, py) ? "  (empty tile!)" : "");
                for (int d = -9; d <= 9; d++) for (int e2 = -9; e2 <= 9; e2++)
                    if (abs(d) + abs(e2) >= 7 && abs(d) + abs(e2) <= 9 && px + d >= 0 && py + e2 >= 0 && px + d < im.w && py + e2 < im.h) {
                        u8 *p = im.rgb + ((size_t)(py + e2) * im.w + px + d) * 3; p[0] = 255; p[1] = 0; p[2] = 255;
                    }
                draw_num(&im, px - 3, py - 3, i, (Rgb){ 255, 0, 255 }, 1);
            }
        }
    }
    int rc = write_bmp(outpath, &im);
    free(im.rgb);
    printf("  events: %d door tiles (red), %d examine/chest tiles (cyan), %d object-spawn tiles (yellow); %d inside this map image, %d outside it\n",
           nA, nC, nD, drawn, outside);
    printf("  %d markers sit on empty filler tiles / outside the layer-1 rooms (drawn grey, hollow): probably entries for areas this MAP does not contain\n", g_flatcnt);
    printf("  actors: %d (green dots, number = script index), %d inside the image\n", nact, actDrawn);

    FILE *f = fopen(txtpath, "w");
    if (f) {
        fprintf(f, "EV event block of %s (file offset 0x%zX)\nheader words:", evpath, base);
        for (int i = 0; i < 9; i++) fprintf(f, " %04X", H[i]);
        fprintf(f, "\nmap image origin (blocks): %d,%d   coordinates below are 16x16 blocks, rectangles x1/y1 exclusive\n", ox, oy);
        const u8 *rec = b + H[2], *cs = b + H[3];
        fprintf(f, "\n== list A: door tiles (type 1/5), record = door record + condition set ==\n");
        for (int i = 0; i < nA; i++) {
            fprintf(f, "A%-2d x %3d-%3d y %3d-%3d type %04X idx %d", i, A[i].x0, A[i].x1, A[i].y0, A[i].y1, A[i].type, A[i].idx);
            if (H[2] + (size_t)A[i].idx * 4 + 4 <= bsz) {
                const u8 *r = rec + A[i].idx * 4;
                fprintf(f, "  record %02X %02X %02X %02X", r[0], r[1], r[2], r[3]);
                u32 so = H[3] + be16(cs + 2 * r[3]);
                if (so + 8 < bsz) {
                    fprintf(f, "  cond-set: %02X %02X +%04X flags", b[so], b[so + 1], be16(b + so + 2));
                    u32 p = so + 4; int g = 0;
                    while (p + 2 <= bsz && be16(b + p) != 0xFFFF && g++ < 8) { fprintf(f, " %04X", be16(b + p)); p += 2; }
                    if (p + 4 <= bsz) fprintf(f, " | text msg %d", be16(b + p + 2));
                }
            }
            fprintf(f, "\n");
        }
        fprintf(f, "\n== list C: examine tiles / chests / furniture (type 0x802 = facing mask in high byte, message index in the door/chest message table) ==\n");
        for (int i = 0; i < nC; i++)
            fprintf(f, "C%-2d x %3d-%3d y %3d-%3d type %04X (facing mask %02X, script %02X) message %d\n", i, C[i].x0, C[i].x1, C[i].y0, C[i].y1, C[i].type, C[i].type >> 8, C[i].type & 255, C[i].idx);
        fprintf(f, "\n== list D: object spawn tiles (type 3), record at block+offset: conditions, FFFF, slot type state script ==\n");
        u16 tb = be16(b + 0x0E);
        for (int i = 0; i < nD; i++) {
            fprintf(f, "D%-2d x %3d-%3d y %3d-%3d type %04X idx %d", i, D[i].x0, D[i].x1, D[i].y0, D[i].y1, D[i].type, D[i].idx);
            if (tb + (size_t)D[i].idx * 2 + 2 <= bsz) {
                u32 ro = be16(b + tb + D[i].idx * 2), p = ro; int g = 0;
                fprintf(f, "  record @%04X conditions", ro);
                while (p + 2 <= bsz && be16(b + p) != 0xFFFF && g++ < 8) { fprintf(f, " %04X", be16(b + p)); p += 2; }
                if (p + 6 <= bsz) fprintf(f, "  -> actor slot %02X type %02X state %02X script %02X", b[p + 2], b[p + 3], b[p + 4], b[p + 5]);
            }
            fprintf(f, "\n");
        }
        fprintf(f, "\n== actors (MAP area records at 0x%X, lists:", be16(m->d + 6));
        for (int k = 0; k < nlists; k++) fprintf(f, " %04X", lists[k]);
        fprintf(f, ") ==\n");
        for (int i = 0; i < nact; i++) {
            Act *ac = &acts[i];
            fprintf(f, "actor script %2d type %02X  pixel x %4d y %4d  block %3d,%3d  facing %d", ac->sidx, ac->type, ac->x + 8, ac->y + 8, (ac->x + 8) / 16, (ac->y + 8) / 16, ac->dir);
            u32 so = be16(b + be16(b) + 2 * ac->sidx);
            if (so + 24 < bsz && (b[so] == 0x1C || b[so] == 0x1F || b[so] == 0x1B || b[so] == 0x44)) {
                const u8 *p = b + so + 1;
                if (b[so] == 0x1F) fprintf(f, "  | op1F appearance %02X sprite %02X state %02X msg %d box dx %d dy %d / dx %d dy %d (blocks, relative)", p[0], p[1], p[2], be16(p + 6), (int16_t)be16(p + 8), (int16_t)be16(p + 10), (int16_t)be16(p + 12), (int16_t)be16(p + 14));
                else {
                    fprintf(f, "  | op%02X appearance %02X sprite %02X anim %02X pattern %d msg %d", b[so], p[0], p[1], p[2], p[3], be16(p + 5));
                    if (p[3] == 0) fprintf(f, " box dx %d dy %d / dx %d dy %d (blocks, relative)", (int16_t)be16(p + 7), (int16_t)be16(p + 9), (int16_t)be16(p + 11), (int16_t)be16(p + 13));
                    else fprintf(f, " (follows movement pattern %d)", p[3]);
                }
            } else fprintf(f, "  | first opcode %02X (not decoded)", so < bsz ? b[so] : 0);
            fprintf(f, "\n");
        }
        fprintf(f, "\n== movement patterns (header +0x0A): triples anim,dir,time; FF = loop, FE = stop ==\n");
        u32 mp = be16(b + 0x0A);
        if (mp + 2 < bsz) {
            int np = be16(b + mp) / 2; if (np > 32) np = 32;
            for (int i = 0; i < np; i++) {
                u32 o = mp + be16(b + mp + 2 * i);
                fprintf(f, "pattern %d @%04X:", i, o);
                for (int k = 0; k < 40 && o + 3 <= bsz && b[o] < 0xFE; k++, o += 3) fprintf(f, " (%d,%d,%d)", b[o], b[o + 1], b[o + 2]);
                if (o < bsz) fprintf(f, " %s", b[o] == 0xFF ? "loop" : b[o] == 0xFE ? "stop" : "...");
                fprintf(f, "\n");
            }
        }
        fputs(g_warptxt, f);
    fprintf(f, "\n== tile objects (chests/items, MAP header +0x0E; objects load only when 'area' = current area) ==\n");
    for (int k = 0; k < g_nobj; k++) fprintf(f, "obj %2d  area %d  x %3d  y %3d\n", g_obj[k][0], g_obj[k][1], g_obj[k][2], g_obj[k][3]);
    fclose(f);
        printf("  wrote %s\n", txtpath);
    }
    free(A); free(C); free(D); free(ev);
    return rc;
}

static void process_file(const char *path)
{
    printf("%s\n", path);
    MapFile m; g_ox = -1; g_oy = 0; free(g_grid); g_grid = NULL;
    if (load_map(&m, path) != 0) { free_map(&m); return; }
    char base[260], out[600];
    base_name(path, base, sizeof base);

    printf("  tiles: %d, palette lines filled: %d%d%d%d\n", m.ntiles,
           m.palFilled[0], m.palFilled[1], m.palFilled[2], m.palFilled[3]);

    Image lay[2] = { { 0, 0, NULL }, { 0, 0, NULL } };
    for (int layer = 0; layer < 2; layer++) {
        char suf[16]; snprintf(suf, sizeof suf, "_layer%d", layer + 1);
        make_out(out, sizeof out, base, suf);
        int rc = render_layer(&m, 2 + layer, g_ppr, out, g_bg, &lay[layer]);
        if (rc == 0) printf("  wrote %s\n", out);
        else if (rc == 1) printf("  layer %d empty\n", layer + 1);
        else printf("  layer %d: write error\n", layer + 1);
    }
    if (lay[0].rgb && lay[1].rgb) {
        make_out(out, sizeof out, base, "_both");
        if (write_combined(&lay[0], &lay[1], out, g_bg) == 0) printf("  wrote %s (both layers, not blended)\n", out);
    }
    if (lay[0].rgb || lay[1].rgb) {
        make_out(out, sizeof out, base, "_doors");
        if (write_doors(&m, lay[0].rgb ? &lay[0] : &lay[1], out) == 0) printf("  wrote %s\n", out);
        if (g_evpath) {
            char txt[600];
            make_out(out, sizeof out, base, "_events");
            snprintf(txt, sizeof txt, "%.*s.txt", (int)strlen(out) - 4, out);
            if (write_events(&m, lay[0].rgb ? &lay[0] : &lay[1], g_evpath, out, txt) == 0) printf("  wrote %s\n", out);
        } else {
            /* automatic: one overlay per EV file (stage) that belongs to this MAP */
            int evl[28], nev = 0;
            u32 st = m.size >= 0x14 ? be16(m.d + 0x12) : 0;
            if (st >= 0x1C && st + 56 <= m.size) {
                for (int q = 0; q < 28; q++) {
                    int v = be16(m.d + st + 2 * q), dup = 0;
                    for (int r = 0; r < nev; r++) if (evl[r] == v) dup = 1;
                    if (!dup) evl[nev++] = v;
                }
            }
            if (!nev) printf("  ERROR: no valid EV stage table in the MAP header (word 0x12 = 0x%04X) - use -e EVFILE\n", (unsigned)st);
            for (int j = 0; j < nev; j++) {
                int ev = evl[j]; char evf[700], suf[40], txt[600];
                if (!find_ev_file(path, ev, evf, sizeof evf)) {
                    printf("  ERROR: EV%05X.DAT (named in this MAP's stage table) not found - looked in %s%s%s\n", ev,
                           g_evdir ? g_evdir : "", g_evdir ? ", " : "", "the MAP's folder and the current folder");
                    continue;
                }
                snprintf(suf, sizeof suf, "_EV%05X_events", ev);
                make_out(out, sizeof out, base, suf);
                snprintf(txt, sizeof txt, "%.*s.txt", (int)strlen(out) - 4, out);
                printf("  EV%05X.DAT:\n", ev);
                if (write_events(&m, lay[0].rgb ? &lay[0] : &lay[1], evf, out, txt) == 0) printf("  wrote %s\n", out);
            }
        }
        {
            char txt[600];
            make_out(out, sizeof out, base, "_objects");
            snprintf(txt, sizeof txt, "%.*s.txt", (int)strlen(out) - 4, out);
            if (write_objects(&m, lay[0].rgb ? &lay[0] : &lay[1], out, txt) == 0) printf("  wrote %s\n", out);
        }
        make_out(out, sizeof out, base, "_collision");
        if (write_collision(&m, lay[0].rgb ? &lay[0] : &lay[1], out) == 0) printf("  wrote %s\n", out);
    }
    free(lay[0].rgb); free(lay[1].rgb);
    make_out(out, sizeof out, base, "_tileset");
    if (render_tileset(&m, out) == 0) printf("  wrote %s\n", out);

    /* backdrop sets listed from 0x18 */
    for (int i = 0; i < 8 && 0x18 + 4 * i + 4 <= (int)m.size; i++) {
        u32 v = be32(m.d + 0x18 + 4 * i);
        if (v == 0 || (v >> 16) < 3 || (v >> 16) > 5) break;
        char suf[24]; snprintf(suf, sizeof suf, "_backdrop%d", i + 1);
        make_out(out, sizeof out, base, suf);
        if (render_backdrop(&m, v - 0x30000, out) == 0) printf("  wrote %s\n", out);
        else printf("  backdrop %d: decode failed\n", i + 1);
    }
    free_map(&m);
}

#ifdef _WIN32
static int has_wild(const char *s) { return strchr(s, '*') || strchr(s, '?'); }
#endif

static void process_arg(const char *arg)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(arg);
    char pattern[600], dir[600];
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(dir, sizeof dir, "%s", arg);
        snprintf(pattern, sizeof pattern, "%s\\*MAP*.DAT", arg);
    } else if (has_wild(arg)) {
        snprintf(pattern, sizeof pattern, "%s", arg);
        snprintf(dir, sizeof dir, "%s", arg);
        char *s = strrchr(dir, '\\'); char *s2 = strrchr(dir, '/');
        if (s2 > s) s = s2;
        if (s) *s = 0; else dir[0] = 0;
    } else { process_file(arg); return; }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no files match %s\n", pattern); return; }
    do {
        char full[900];
        if (dir[0]) snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        else snprintf(full, sizeof full, "%s", fd.cFileName);
        process_file(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    struct stat st;
    if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(arg);
        struct dirent *e;
        if (!d) return;
        while ((e = readdir(d))) {
            if (strstr(e->d_name, "MAP") && strstr(e->d_name, ".DAT")) {
                char full[900]; snprintf(full, sizeof full, "%s/%s", arg, e->d_name);
                process_file(full);
            }
        }
        closedir(d);
    } else process_file(arg);
#endif
}

static void usage(void)
{
    puts("map3x3 - 3x3 Eyes (Mega CD) MAP file to BMP exporter\n"
         "usage: map3x3 [options] file-or-folder [more files...]\n"
         "  -o DIR    output folder (must exist; default: current folder)\n"
         "  -w N      force N pages (16 blocks each) per map row instead of auto-detect\n"
         "  -b RRGGBB background colour for colour 0 pixels (default 202020)\n"
         "  -v FILE   64 KB raw VRAM dump from an emulator taken while the map is shown;\n"
         "            supplies tiles that are not stored in the MAP (animated water)\n"
         "  -E DIR    folder with the EV files (default: the MAP's folder, then the current folder). Without -e, the EV files\n"
         "            belonging to each MAP (stage table of _000PRG.DAT) are found automatically as EVxxxxx.DAT and one\n"
         "            _EVxxxxx_events.bmp/.txt is written per EV file; missing files are reported as ERROR.\n"
         "  -e EVFILE EV file that belongs to the MAP (e.g. -e EV00006.DAT for 00MAP03.DAT): writes\n"
         "            _events.bmp (green dots = actors (number = event script index, green box = wander area), red = door trigger areas with a white dot on the door tile, cyan = examine/chest tiles, yellow = object spawns,\n"
         "            numbers = index) and _events.txt (decoded lists, conditions, movement patterns)\n"
         "Drag & drop MAP files or a folder onto the exe also works.\n"
         "Outputs per MAP: _objects (world object/chest slots from the MAP header, box colour = room, number = row in _objects.txt), _layer1/_layer2 (map), _doors (layer-2 on/off trigger rows + interior areas), _collision (red solid, orange counter, yellow other), _both (both layers in one picture, not blended),\n"
         "                 _tileset, _backdrop1..n");
}

int main(int argc, char **argv)
{
    int any = 0;
    if (argc < 2) { usage(); return 1; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) g_outdir = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) g_ppr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            unsigned v = (unsigned)strtoul(argv[++i], NULL, 16);
            g_bg.r = (u8)(v >> 16); g_bg.g = (u8)(v >> 8); g_bg.b = (u8)v;
        }
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            size_t vs; g_vram = read_file(argv[++i], &vs);
            if (!g_vram || vs < 65536) { fprintf(stderr, "VRAM dump must be 65536 bytes\n"); return 1; }
        }
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) g_evpath = argv[++i];
        else if (!strcmp(argv[i], "-E") && i + 1 < argc) g_evdir = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { process_arg(argv[i]); any = 1; }
    }
    if (!any) usage();
    return 0;
}
