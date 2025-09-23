#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#define MAX_NAME 64
#define MAX_ARGS 16

typedef struct {
    uint8_t opcode;
    char    name[MAX_NAME];
    int     arg_count;
    bool    arg_is_offset[MAX_ARGS];  // per-arg
    uint8_t arg_width[MAX_ARGS];      // per-arg (1,2,4). Default 2 if not set.
    bool    continues;                // true => next opcode follows; false => return to table
} OpcodeDef;

typedef struct {
    OpcodeDef *items;
    size_t count, cap;
} DefVec;

/* -------------------- tiny utils -------------------- */

static void die(const char *msg) { fprintf(stderr, "%s\n", msg); exit(1); }

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void vec_push(DefVec *v, const OpcodeDef *d) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->items = (OpcodeDef*)realloc(v->items, v->cap * sizeof(*v->items));
        if (!v->items) die("out of memory");
    }
    v->items[v->count++] = *d;
}

static bool parse_bool_ci(const char *s, bool *out) {
    char buf[16] = {0};
    size_t n = 0;
    while (*s && n < sizeof(buf)-1) buf[n++] = (char)tolower((unsigned char)*s++);
    if (!strcmp(buf, "true"))  { *out = true;  return true; }
    if (!strcmp(buf, "false")) { *out = false; return true; }
    return false;
}

static int hexbyte(const char *s) {
    unsigned x = 0;
    if (sscanf(s, "%x", &x) == 1 && x <= 0xFF) return (int)x;
    return -1;
}

static unsigned long read_ulong_prompt_anybase(const char *prompt) {
    char buf[128];
    printf("%s", prompt); fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) die("input error");
    char *end = NULL;
    unsigned long x = strtoul(buf, &end, 0); // base 0 => 0x.. or decimal
    if (end == buf) die("expected a number");
    return x;
}

/* -------------------- file reading (big-endian) -------------------- */

static uint8_t read8(FILE *f, bool *ok) {
    int b = fgetc(f);
    if (b == EOF) { *ok = false; return 0; }
    *ok = true; return (uint8_t)b;
}

static uint16_t read16_be(FILE *f, bool *ok) {
    int b0 = fgetc(f), b1 = fgetc(f);
    if (b0 == EOF || b1 == EOF) { *ok = false; return 0; }
    *ok = true; return (uint16_t)((b0<<8) | b1);
}

static uint32_t read32_be(FILE *f, bool *ok) {
    int b0 = fgetc(f), b1 = fgetc(f), b2 = fgetc(f), b3 = fgetc(f);
    if (b0 == EOF || b1 == EOF || b2 == EOF || b3 == EOF) { *ok = false; return 0; }
    *ok = true; return ((uint32_t)b0<<24) | ((uint32_t)b1<<16) | ((uint32_t)b2<<8) | (uint32_t)b3;
}

static unsigned long read_be_by_width(FILE *f, int width, bool *ok) {
    switch (width) {
        case 1: return read8(f, ok);
        case 2: return read16_be(f, ok);
        case 4: return read32_be(f, ok);
        default: *ok = false; return 0;
    }
}

/* -------------------- ultra-minimal YAML loader -------------------- */
/* Supports:
   opcodes:
     0xAB:
       name: Foo
       arg_count: 3
       arg_is_offset: [true, false, true]
       arg_widths:    [2, 1, 2]   # optional; defaults to 2 per arg if omitted
       continues: true
*/

static void set_defaults(OpcodeDef *d) {
    for (int i = 0; i < MAX_ARGS; i++) {
        d->arg_is_offset[i] = false;
        d->arg_width[i] = 2; // default to 2 bytes per your request
    }
}

static void load_defs(const char *path, DefVec *out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }

    char line[512];
    bool in_opcodes = false;
    bool have_current = false;
    OpcodeDef cur;
    memset(&cur, 0, sizeof cur);
    set_defaults(&cur);

    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;

        if (!in_opcodes) {
            if (!strcmp(p, "opcodes:")) in_opcodes = true;
            continue;
        }

        // New opcode key: "0xAB:"
        if (p[0] == '0' && (p[1]=='x' || p[1]=='X')) {
            char *colon = strchr(p, ':');
            if (!colon) continue;
            *colon = '\0';
            int hb = hexbyte(p);
            if (hb < 0) continue;

            if (have_current) vec_push(out, &cur);
            memset(&cur, 0, sizeof cur);
            set_defaults(&cur);
            cur.opcode = (uint8_t)hb;
            have_current = true;
            continue;
        }

        if (!have_current) continue;

        if (!strncmp(p, "name:", 5)) {
            char *v = trim(p + 5);
            if (*v == '"' || *v == '\'') {
                char q = *v++;
                char *end = strrchr(v, q);
                if (end) *end = '\0';
            }
            strncpy(cur.name, v, sizeof cur.name - 1);
        } else if (!strncmp(p, "arg_count:", 10)) {
            int n = 0; sscanf(p + 10, "%d", &n);
            if (n < 0) n = 0; if (n > MAX_ARGS) n = MAX_ARGS;
            cur.arg_count = n;
        } else if (!strncmp(p, "arg_is_offset:", 14)) {
            char *v = strchr(p, '['); if (!v) continue;
            v++;
            int idx = 0;
            while (*v && *v != ']' && idx < MAX_ARGS) {
                while (isspace((unsigned char)*v) || *v==',') v++;
                char tok[16] = {0};
                int tlen = 0;
                while (*v && *v!=',' && *v!=']' && !isspace((unsigned char)*v) && tlen < 15)
                    tok[tlen++] = *v++;
                tok[tlen] = 0;
                bool b;
                if (parse_bool_ci(tok, &b)) cur.arg_is_offset[idx++] = b;
                while (*v && *v!=',' && *v!=']') v++;
            }
        } else if (!strncmp(p, "arg_widths:", 11)) {
            char *v = strchr(p, '['); if (!v) continue;
            v++;
            int idx = 0;
            while (*v && *v != ']' && idx < MAX_ARGS) {
                while (isspace((unsigned char)*v) || *v==',') v++;
                char tok[16] = {0};
                int tlen = 0;
                while (*v && *v!=',' && *v!=']' && !isspace((unsigned char)*v) && tlen < 15)
                    tok[tlen++] = *v++;
                tok[tlen] = 0;
                long w = strtol(tok, NULL, 0);
                if (w == 1 || w == 2 || w == 4) cur.arg_width[idx++] = (uint8_t)w;
                while (*v && *v!=',' && *v!=']') v++;
            }
        } else if (!strncmp(p, "continues:", 10)) {
            char *v = trim(p + 10);
            bool b; if (parse_bool_ci(v, &b)) cur.continues = b;
        }
    }

    if (have_current) vec_push(out, &cur);
    fclose(f);
}

static const OpcodeDef* find_def(const DefVec *defs, uint8_t opcode) {
    for (size_t i = 0; i < defs->count; i++)
        if (defs->items[i].opcode == opcode) return &defs->items[i];
    return NULL;
}

/* -------------------- main -------------------- */

int main(void) {
    char bin_path[512], yaml_path[512];
    unsigned long pointer_base, table_off, pointer_count;

    pointer_base = read_ulong_prompt_anybase("Base offset to add to each pointer (hex/dec, e.g. 0x9A000): ");

    printf("Binary file (ROM) path: ");
    if (!fgets(bin_path, sizeof bin_path, stdin)) die("input error");
    bin_path[strcspn(bin_path, "\r\n")] = 0;

    printf("YAML opcode definition path: ");
    if (!fgets(yaml_path, sizeof yaml_path, stdin)) die("input error");
    yaml_path[strcspn(yaml_path, "\r\n")] = 0;

    table_off     = read_ulong_prompt_anybase("Pointer table file offset (hex/dec): ");
    pointer_count = read_ulong_prompt_anybase("Number of 16-bit pointers (hex/dec): ");

    FILE *f = fopen(bin_path, "rb");
    if (!f) { perror(bin_path); return 1; }

    DefVec defs = {0};
    load_defs(yaml_path, &defs);
    if (!defs.count) die("no opcode definitions loaded");

    for (unsigned long i = 0; i < pointer_count; i++) {
        unsigned long ptr_pos = table_off + i*2UL;
        if (fseek(f, (long)ptr_pos, SEEK_SET) != 0) { perror("fseek"); break; }
        bool ok = true;
        uint16_t pval = read16_be(f, &ok);
        if (!ok) { fprintf(stderr, "EOF reading pointer %lu\n", i); break; }

        unsigned long pos = pointer_base + pval;
        printf("\n=== Pointer[%lu] table@0x%08lX -> target 0x%08lX (val 0x%04X) ===\n",
               i, ptr_pos, pos, pval);

        for (;;) {
            if (fseek(f, (long)pos, SEEK_SET) != 0) {
                fprintf(stderr, "seek failed to 0x%08lX\n", pos);
                goto cleanup_fail;
            }
            bool okop = true;
            int op = fgetc(f);
            if (op == EOF) {
                fprintf(stderr, "EOF at 0x%08lX\n", pos);
                goto cleanup_fail;
            }

            const OpcodeDef *d = find_def(&defs, (uint8_t)op);
            if (!d) {
                fprintf(stderr,
                    "ERROR: Unknown opcode 0x%02X at 0x%08lX (from pointer index %lu). Aborting.\n",
                    op & 0xFF, pos, i);
                goto cleanup_fail;
            }

            printf("  @0x%08lX: OPC 0x%02X  %-32s", pos, d->opcode, d->name[0]?d->name:"(unnamed)");
            pos += 1; // advance past opcode

            if (d->arg_count > 0) {
                printf(" | args: ");
                for (int a = 0; a < d->arg_count; a++) {
                    int w = d->arg_width[a] ? d->arg_width[a] : 2; // default 2
                    bool okarg = true;
                    unsigned long raw = read_be_by_width(f, w, &okarg);
                    if (!okarg) { printf("[EOF]"); fprintf(stderr, "\nEOF in args\n"); goto cleanup_fail; }
                    if (a) printf(", ");
                    if (d->arg_is_offset[a]) {
                        unsigned long resolved = pointer_base + raw;
                        if (w == 1)      printf("0x%02lX->0x%08lX", raw & 0xFF, resolved);
                        else if (w == 2) printf("0x%04lX->0x%08lX", raw & 0xFFFF, resolved);
                        else             printf("0x%08lX->0x%08lX", raw, resolved);
                    } else {
                        if (w == 1)      printf("0x%02lX", raw & 0xFF);
                        else if (w == 2) printf("0x%04lX", raw & 0xFFFF);
                        else             printf("0x%08lX", raw);
                    }
                    pos += (unsigned)w;
                }
            }
            printf(" | continues=%s\n", d->continues ? "true" : "false");

            if (!d->continues) break; // return to pointer table
        }
    }

    free(defs.items);
    fclose(f);
    return 0;

cleanup_fail:
    free(defs.items);
    fclose(f);
    return 2;
}
