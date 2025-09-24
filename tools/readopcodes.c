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
    bool    arg_is_offset[MAX_ARGS];     // print/resolve as base+value
    uint8_t arg_width[MAX_ARGS];         // 1,2,4 (default 2)
    bool    follow_targets[MAX_ARGS];    // follow control-flow target
    bool    arg_is_text_offset[MAX_ARGS];// if true: decode text at base+value using table
    bool    continues;
} OpcodeDef;

typedef struct { OpcodeDef *items; size_t count, cap; } DefVec;
typedef struct { unsigned long *items; size_t count, cap; } ULongVec;

/* ---------- text table (1- and 2-byte keys => UTF-8 string) ---------- */

typedef struct { uint8_t  key;  char *val; } Map1;
typedef struct { uint16_t key;  char *val; } Map2;

typedef struct {
    Map1 *m1; size_t n1, c1;
    Map2 *m2; size_t n2, c2;
} TextMap;

static void tmap_add1(TextMap *tm, uint8_t k, const char *v) {
    if (tm->n1 == tm->c1) {
        tm->c1 = tm->c1 ? tm->c1*2 : 128;
        tm->m1 = (Map1*)realloc(tm->m1, tm->c1*sizeof(Map1));
        if (!tm->m1) { perror("realloc"); exit(1); }
    }
    tm->m1[tm->n1].key = k;
    tm->m1[tm->n1].val = strdup(v?v:"");
    tm->n1++;
}
static void tmap_add2(TextMap *tm, uint16_t k, const char *v) {
    if (tm->n2 == tm->c2) {
        tm->c2 = tm->c2 ? tm->c2*2 : 128;
        tm->m2 = (Map2*)realloc(tm->m2, tm->c2*sizeof(Map2));
        if (!tm->m2) { perror("realloc"); exit(1); }
    }
    tm->m2[tm->n2].key = k;
    tm->m2[tm->n2].val = strdup(v?v:"");
    tm->n2++;
}
static const char* tmap_find2(const TextMap *tm, uint16_t k) {
    for (size_t i=0;i<tm->n2;i++) if (tm->m2[i].key==k) return tm->m2[i].val;
    return NULL;
}
static const char* tmap_find1(const TextMap *tm, uint8_t k) {
    for (size_t i=0;i<tm->n1;i++) if (tm->m1[i].key==k) return tm->m1[i].val;
    return NULL;
}
static void tmap_free(TextMap *tm) {
    for (size_t i=0;i<tm->n1;i++) free(tm->m1[i].val);
    for (size_t i=0;i<tm->n2;i++) free(tm->m2[i].val);
    free(tm->m1); free(tm->m2);
    memset(tm, 0, sizeof *tm);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}
static int hexbyte(const char *s) { unsigned x=0; if (sscanf(s,"%x",&x)==1 && x<=0xFF) return (int)x; return -1; }
static unsigned long read_ulong_prompt_anybase(const char *prompt) {
    char buf[128]; printf("%s", prompt); fflush(stdout);
    if (!fgets(buf, sizeof buf, stdin)) { fprintf(stderr,"input error\n"); exit(1); }
    char *end=NULL; unsigned long x=strtoul(buf,&end,0);
    if (end==buf) { fprintf(stderr,"expected a number\n"); exit(1); }
    return x;
}
static void die(const char *msg) { fprintf(stderr, "%s\n", msg); exit(1); }

/* ---------- file reading (big-endian) ---------- */

static uint8_t read8(FILE *f, bool *ok){ int b=fgetc(f); if(b==EOF){*ok=false; return 0;} *ok=true; return (uint8_t)b; }
static uint16_t read16_be(FILE *f, bool *ok){
    int b0=fgetc(f), b1=fgetc(f); if(b0==EOF||b1==EOF){*ok=false; return 0;} *ok=true; return (uint16_t)((b0<<8)|b1);
}
static uint32_t read32_be(FILE *f, bool *ok){
    int b0=fgetc(f), b1=fgetc(f), b2=fgetc(f), b3=fgetc(f);
    if(b0==EOF||b1==EOF||b2==EOF||b3==EOF){*ok=false; return 0;}
    *ok=true; return ((uint32_t)b0<<24)|((uint32_t)b1<<16)|((uint32_t)b2<<8)|(uint32_t)b3;
}
static unsigned long read_be_by_width(FILE *f, int w, bool *ok){
    switch (w){ case 1: return read8(f,ok); case 2: return read16_be(f,ok); case 4: return read32_be(f,ok); default:*ok=false;return 0; }
}

/* ---------- YAML loader ---------- */
/* Supports:
   opcodes:
     0xAB:
       name: Foo
       arg_count: 3
       arg_is_offset:   [true, false, true]
       arg_widths:      [2, 1, 2]
       follow_targets:  [false, true, false]
       arg_is_text_offset: [false, false, true]
       continues: true
*/

static void vec_push(DefVec *v, const OpcodeDef *d) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->items = (OpcodeDef*)realloc(v->items, v->cap * sizeof(*v->items));
        if (!v->items) die("out of memory");
    }
    v->items[v->count++] = *d;
}
static bool parse_bool_ci(const char *s, bool *out) {
    char buf[16]={0}; size_t n=0; while(*s && n<sizeof(buf)-1) buf[n++]=(char)tolower((unsigned char)*s++);
    if(!strcmp(buf,"true")){*out=true; return true;} if(!strcmp(buf,"false")){*out=false; return true;} return false;
}
static void set_defaults(OpcodeDef *d) {
    for (int i=0;i<MAX_ARGS;i++){
        d->arg_is_offset[i]=false;
        d->arg_is_text_offset[i]=false;
        d->arg_width[i]=2;
        d->follow_targets[i]=false;
    }
    d->continues=false;
}
static void load_defs(const char *path, DefVec *out) {
    FILE *f=fopen(path,"rb"); if(!f){ perror(path); exit(1); }
    char line[512]; bool in_opcodes=false, have=false; OpcodeDef cur; memset(&cur,0,sizeof cur); set_defaults(&cur);
    while (fgets(line,sizeof line,f)){
        char *p=trim(line); if(!*p || *p=='#') continue;
        if(!in_opcodes){ if(!strcmp(p,"opcodes:")) in_opcodes=true; continue; }

        if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) {
            char *colon=strchr(p,':'); if(!colon) continue; *colon='\0';
            int hb=hexbyte(p); if(hb<0) continue;
            if(have) vec_push(out,&cur);
            memset(&cur,0,sizeof cur); set_defaults(&cur); cur.opcode=(uint8_t)hb; have=true; continue;
        }
        if(!have) continue;

        if (!strncmp(p,"name:",5)) {
            char *v=trim(p+5);
            if(*v=='"'||*v=='\''){ char q=*v++; char *e=strrchr(v,q); if(e) *e='\0'; }
            strncpy(cur.name,v,sizeof cur.name-1);
        } else if (!strncmp(p,"arg_count:",10)) {
            int n=0; sscanf(p+10,"%d",&n); if(n<0)n=0; if(n>MAX_ARGS)n=MAX_ARGS; cur.arg_count=n;
        } else if (!strncmp(p,"arg_is_offset:",14)) {
            char *v=strchr(p,'['); if(!v) continue; v++; int idx=0;
            while(*v && *v!=']' && idx<MAX_ARGS){
                while(isspace((unsigned char)*v)||*v==',') v++;
                char tok[16]={0}; int t=0; while(*v&&*v!=','&&*v!=']'&&!isspace((unsigned char)*v)&&t<15) tok[t++]=*v++;
                bool b; if(parse_bool_ci(tok,&b)) cur.arg_is_offset[idx++]=b; while(*v&&*v!=','&&*v!=']') v++;
            }
        } else if (!strncmp(p,"arg_is_text_offset:",19)) {
            char *v=strchr(p,'['); if(!v) continue; v++; int idx=0;
            while(*v && *v!=']' && idx<MAX_ARGS){
                while(isspace((unsigned char)*v)||*v==',') v++;
                char tok[16]={0}; int t=0; while(*v&&*v!=','&&*v!=']'&&!isspace((unsigned char)*v)&&t<15) tok[t++]=*v++;
                bool b; if(parse_bool_ci(tok,&b)) cur.arg_is_text_offset[idx++]=b; while(*v&&*v!=','&&*v!=']') v++;
            }
        } else if (!strncmp(p,"arg_widths:",11)) {
            char *v=strchr(p,'['); if(!v) continue; v++; int idx=0;
            while(*v && *v!=']' && idx<MAX_ARGS){
                while(isspace((unsigned char)*v)||*v==',') v++;
                char tok[16]={0}; int t=0; while(*v&&*v!=','&&*v!=']'&&!isspace((unsigned char)*v)&&t<15) tok[t++]=*v++;
                long w=strtol(tok,NULL,0); if(w==1||w==2||w==4) cur.arg_width[idx++]=(uint8_t)w; while(*v&&*v!=','&&*v!=']') v++;
            }
        } else if (!strncmp(p,"follow_targets:",15)) {
            char *v=strchr(p,'['); if(!v) continue; v++; int idx=0;
            while(*v && *v!=']' && idx<MAX_ARGS){
                while(isspace((unsigned char)*v)||*v==',') v++;
                char tok[16]={0}; int t=0; while(*v&&*v!=','&&*v!=']'&&!isspace((unsigned char)*v)&&t<15) tok[t++]=*v++;
                bool b; if(parse_bool_ci(tok,&b)) cur.follow_targets[idx++]=b; while(*v&&*v!=','&&*v!=']') v++;
            }
        } else if (!strncmp(p,"continues:",10)) {
            char *v=trim(p+10); bool b; if(parse_bool_ci(v,&b)) cur.continues=b;
        }
    }
    if(have) vec_push(out,&cur);
    fclose(f);
}

static const OpcodeDef* find_def(const DefVec *defs, uint8_t opcode){
    for(size_t i=0;i<defs->count;i++) if(defs->items[i].opcode==opcode) return &defs->items[i];
    return NULL;
}

/* ---------- table loader (XX=YY or XXXX=YY) ---------- */

static void load_text_table(const char *path, TextMap *tm) {
    memset(tm,0,sizeof *tm);
    FILE *f=fopen(path,"rb"); if(!f){ perror(path); exit(1); }
    char line[1024];
    while (fgets(line,sizeof line,f)) {
        char *p=trim(line);
        if(!*p || *p=='#') continue;
        char *eq=strchr(p,'=');
        if(!eq) continue;
        *eq='\0';
        char *kstr=trim(p);
        char *vstr=trim(eq+1);
        // strip trailing newline already via trim
        // uppercase hex for key
        for(char *q=kstr; *q; ++q) *q=(char)toupper((unsigned char)*q);
        size_t klen=strlen(kstr);
        if (klen==2) {
            unsigned k; if (sscanf(kstr,"%2x",&k)==1) tmap_add1(tm,(uint8_t)k,vstr);
        } else if (klen==4) {
            unsigned k; if (sscanf(kstr,"%4x",&k)==1) tmap_add2(tm,(uint16_t)k,vstr);
        } else {
            // ignore malformed keys
        }
    }
    fclose(f);
}

/* ---------- tiny containers ---------- */

static void uvec_push(ULongVec *v, unsigned long x){
    if(v->count==v->cap){ v->cap = v->cap? v->cap*2:64; v->items=(unsigned long*)realloc(v->items, v->cap*sizeof(unsigned long));
        if(!v->items){ perror("realloc"); exit(1);} }
    v->items[v->count++]=x;
}
static bool uvec_contains(const ULongVec *v, unsigned long x){ for(size_t i=0;i<v->count;i++) if(v->items[i]==x) return true; return false; }

/* ---------- processing ---------- */

typedef struct {
    FILE *f;
    const DefVec *defs;
    unsigned long base;          // pointer_base
    size_t ptr_index;
    const TextMap *tmap;         // for text decoding
} Ctx;

static void print_indent(int depth){ for(int i=0;i<depth;i++) putchar(' '); }

/* Decode and print text starting at addr, stop on 0xDB */
static void print_text_block(Ctx *ctx, unsigned long addr, int depth) {
    FILE *f=ctx->f;
    if (fseek(f, (long)addr, SEEK_SET)!=0) { print_indent(depth); printf("  [text @0x%08lX] <seek error>\n", addr); return; }
    print_indent(depth);
    printf("  [text @0x%08lX] \"", addr);

    bool ok=true;
    while (1) {
        long here = ftell(f);
        uint8_t b1 = read8(f,&ok);
        if(!ok){ printf("\" <EOF>\n"); return; }
        if (b1 == 0xDB) { break; } // end of text

        // Try 2-byte match first
        long pos_after_b1 = ftell(f);
        bool ok2=true;
        uint8_t b2 = read8(f,&ok2);
        if (ok2) {
            uint16_t k2 = ((uint16_t)b1<<8) | b2;
            const char *s2 = tmap_find2(ctx->tmap, k2);
            if (s2) {
                fputs(s2, stdout);
                continue; // consumed both bytes
            }
            // no 2-byte match -> rewind to after b1 to try 1-byte
            fseek(f, pos_after_b1, SEEK_SET);
        } else {
            // couldn't read second byte -> rewind to after b1 for 1-byte try
            fseek(f, pos_after_b1, SEEK_SET);
        }

        // Try 1-byte
        const char *s1 = tmap_find1(ctx->tmap, b1);
        if (s1) {
            fputs(s1, stdout);
        } else {
            // unknown byte: show hex placeholder
            printf("{%02X}", b1);
        }
    }

    printf("\"\n");
}

/* forward decl */
static int process_stream(Ctx *ctx, unsigned long start_pos, ULongVec *visited, int depth);

static int process_one(Ctx *ctx, unsigned long *pos_io, ULongVec *visited, int depth) {
    unsigned long pos = *pos_io;

    if (fseek(ctx->f, (long)pos, SEEK_SET) != 0) {
        fprintf(stderr, "seek failed to 0x%08lX\n", pos);
        return 2;
    }

    int op = fgetc(ctx->f);
    if (op == EOF) {
        fprintf(stderr, "EOF at 0x%08lX\n", pos);
        return 2;
    }

    const OpcodeDef *d = find_def(ctx->defs, (uint8_t)op);
    if (!d) {
        fprintf(stderr, "ERROR: Unknown opcode 0x%02X at 0x%08lX (from pointer index %zu). Aborting.\n",
                op & 0xFF, pos, ctx->ptr_index);
        return 2;
    }

    print_indent(depth);
    printf("  @0x%08lX: OPC 0x%02X  %-32s", pos, d->opcode, d->name[0]?d->name:"(unnamed)");
    pos += 1;

    unsigned long follow_targets[MAX_ARGS]; int follow_count=0;
    unsigned long text_targets[MAX_ARGS];   int text_count=0;

    if (d->arg_count > 0) {
        printf(" | args: ");
        if (fseek(ctx->f, (long)pos, SEEK_SET) != 0) {
            fprintf(stderr, "seek failed to arg start 0x%08lX\n", pos);
            return 2;
        }
        for (int a=0; a<d->arg_count; a++) {
            int w = d->arg_width[a] ? d->arg_width[a] : 2;
            bool okarg=true;
            unsigned long raw = read_be_by_width(ctx->f, w, &okarg);
            if (!okarg) { printf("[EOF]"); fprintf(stderr,"\nEOF in args\n"); return 2; }
            if (a) printf(", ");

            if (d->arg_is_offset[a] || d->arg_is_text_offset[a]) {
                unsigned long resolved = ctx->base + raw;
                if (w==1)      printf("0x%02lX->0x%08lX", raw & 0xFF, resolved);
                else if (w==2) printf("0x%04lX->0x%08lX", raw & 0xFFFF, resolved);
                else           printf("0x%08lX->0x%08lX", raw, resolved);
                if (d->follow_targets[a]) follow_targets[follow_count++] = resolved;
                if (d->arg_is_text_offset[a]) text_targets[text_count++] = resolved;
            } else {
                if (w==1)      printf("0x%02lX", raw & 0xFF);
                else if (w==2) printf("0x%04lX", raw & 0xFFFF);
                else           printf("0x%08lX", raw);
            }
            pos += (unsigned)w;
        }
    }
    printf(" | continues=%s\n", d->continues ? "true" : "false");

    // If there are text targets, print their contents now (indented)
    for (int i=0;i<text_count;i++) {
        print_text_block(ctx, text_targets[i], depth+2);
    }

    // Depth-first follow of jump targets, then fall-through
    for (int i=0;i<follow_count;i++) {
        unsigned long tgt = follow_targets[i];
        if (uvec_contains(visited, tgt)) {
            print_indent(depth);
            printf("    -> follow 0x%08lX (already visited)\n", tgt);
        } else {
            print_indent(depth);
            printf("    -> follow 0x%08lX\n", tgt);
            uvec_push(visited, tgt);
            int rc = process_stream(ctx, tgt, visited, depth+2);
            if (rc) return rc;
        }
    }

    *pos_io = pos;
    return d->continues ? 0 : 1;
}

static int process_stream(Ctx *ctx, unsigned long start_pos, ULongVec *visited, int depth) {
    unsigned long pos = start_pos;
    for (;;) {
        int rc = process_one(ctx, &pos, visited, depth);
        if (rc == 0) continue;      // keep scanning this stream
        if (rc == 1) return 0;      // reached continues=false
        return rc;                   // error
    }
}

/* ---------- main ---------- */

int main(void) {
    char bin_path[512], yaml_path[512], table_path[512];
    unsigned long pointer_base, table_off, pointer_count;

    pointer_base = read_ulong_prompt_anybase("Base offset to add to each pointer (hex/dec, e.g. 0x9A000): ");

    printf("Binary file (ROM) path: ");
    if (!fgets(bin_path, sizeof bin_path, stdin)) die("input error");
    bin_path[strcspn(bin_path, "\r\n")] = 0;

    printf("YAML opcode definition path: ");
    if (!fgets(yaml_path, sizeof yaml_path, stdin)) die("input error");
    yaml_path[strcspn(yaml_path, "\r\n")] = 0;

    printf("Text table path (e.g. mapping.txt): ");
    if (!fgets(table_path, sizeof table_path, stdin)) die("input error");
    table_path[strcspn(table_path, "\r\n")] = 0;

    pointer_count = read_ulong_prompt_anybase("Number of 16-bit pointers (hex/dec): ");

    FILE *f = fopen(bin_path, "rb");
    if (!f) { perror(bin_path); return 1; }

    // Derive pointer table offset: read BE16 at pointer_base and add it
    bool ok=true;
    if (fseek(f, (long)pointer_base, SEEK_SET)!=0) { perror("fseek base"); fclose(f); return 2; }
    uint16_t table_rel = read16_be(f,&ok);
    if (!ok) { fprintf(stderr,"EOF reading table-relative offset at 0x%08lX\n", pointer_base); fclose(f); return 2; }
    table_off = pointer_base + table_rel;

    printf("\nPointer table start derived: base 0x%08lX + BE16(0x%04X) = 0x%08lX\n",
           pointer_base, table_rel, table_off);

    DefVec defs={0}; load_defs(yaml_path,&defs);
    printf("Loaded %zu opcode definitions from %s\n", defs.count, yaml_path);
    if (!defs.count) die("no opcode definitions loaded");

    TextMap tmap; load_text_table(table_path, &tmap);
    printf("Loaded text table: %zu one-byte entries, %zu two-byte entries from %s\n",
           tmap.n1, tmap.n2, table_path);

    Ctx ctx = {.f=f, .defs=&defs, .base=pointer_base, .ptr_index=0, .tmap=&tmap};

    for (unsigned long i=0;i<pointer_count;i++) {
        unsigned long ptr_pos = table_off + i*2UL;
        if (fseek(f, (long)ptr_pos, SEEK_SET)!=0) { perror("fseek pointer entry"); break; }
        bool okp=true; uint16_t pval = read16_be(f,&okp);
        if (!okp) { fprintf(stderr, "EOF reading pointer %lu\n", i); break; }

        unsigned long pos = pointer_base + pval;
        printf("\n=== Pointer[%lu] table@0x%08lX -> target 0x%08lX (val 0x%04X) ===\n",
               i, ptr_pos, pos, pval);

        ULongVec visited={0};
        uvec_push(&visited, pos);
        ctx.ptr_index = i;

        int rc = process_stream(&ctx, pos, &visited, 0);
        free(visited.items);
        if (rc) { tmap_free(&tmap); free(defs.items); fclose(f); return rc; }
    }

    tmap_free(&tmap);
    free(defs.items);
    fclose(f);
    return 0;
}
