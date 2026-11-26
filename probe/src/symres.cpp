#include "symres.h"

#include <elf.h>
#include <stdlib.h>
#include <string.h>

#include "xz.h"

#define MAX_DEBUGDATA ((size_t)64 * 1024 * 1024)

/* A suffix such as ".llvm.123" or ".__uniq.5" is the same function; ".cold" fragments are not. */
static int suffix_ok(const char *s) {
    if (*s == '\0') return 1;
    if (*s != '.') return 0;
    return !strstr(s, ".cold") && !strstr(s, ".cfi_jt");
}

/* Reads one <length><identifier>. Returns the position after it, or NULL. */
static const char *read_ident(const char *p, const char **id, size_t *n) {
    size_t len = 0;
    if (*p < '1' || *p > '9') return NULL;
    while (*p >= '0' && *p <= '9') {
        len = len * 10 + (size_t)(*p++ - '0');
        if (len > 4096) return NULL;
    }
    if (strnlen(p, len) < len) return NULL;
    *id = p;
    *n = len;
    return p + len;
}

#define MAX_COMP 8

int sr_symbol_matches(const char *symbol, const char *name) {
    size_t name_len = strlen(name);

    /* Split "a::b::c" into components (trailing part of the qualified name). */
    const char *cn[MAX_COMP];
    size_t cl[MAX_COMP];
    int comps = 0;
    for (const char *q = name;;) {
        const char *sep = strstr(q, "::");
        size_t n = sep ? (size_t)(sep - q) : strlen(q);
        if (n == 0 || comps == MAX_COMP) return 0;
        cn[comps] = q;
        cl[comps++] = n;
        if (!sep) break;
        q = sep + 2;
    }

    if (strncmp(symbol, "_Z", 2) != 0) /* C symbol */
        return comps == 1 && strncmp(symbol, name, name_len) == 0 && suffix_ok(symbol + name_len);

    const char *p = symbol + 2;
    int nested = 0;
    if (*p == 'N') {
        nested = 1;
        p++;
        while (*p && strchr("rVKRO", *p)) p++;
    } else if (*p == 'L') {
        p++;
    }

    const char *ids[16];
    size_t idn[16];
    int nid = 0;
    for (;;) {
        const char *id;
        size_t n;
        if (nested && *p == 'L') p++;
        const char *next = read_ident(p, &id, &n);
        if (!next) return 0;
        if (nid == 16) { /* keep the last 16: shift */
            memmove(ids, ids + 1, sizeof(ids) - sizeof(ids[0]));
            memmove(idn, idn + 1, sizeof(idn) - sizeof(idn[0]));
            nid--;
        }
        ids[nid] = id;
        idn[nid++] = n;
        p = next;
        if (!nested) break;
        if (*p == 'E') {
            p++;
            break;
        }
    }
    /* p now points at the parameter list (possibly followed by a dot suffix). */
    if (*p == '\0') return 0; /* a variable, not a function */
    const char *dot = strchr(p, '.');
    if (dot && !suffix_ok(dot)) return 0;

    /* The trailing identifiers must equal the requested components, in order. */
    if (comps > nid) return 0;
    for (int k = 0; k < comps; k++) {
        int j = nid - comps + k;
        if (idn[j] != cl[k] || memcmp(ids[j], cn[k], cl[k]) != 0) return 0;
    }
    return 1;
}

static int in_range(size_t len, uint64_t off, uint64_t size) { return off <= len && size <= len - off; }

static void add_match(struct sr_target *t, const char *sym, uint64_t value, uint64_t size) {
    for (int i = 0; i < t->matches && i < SR_MAX_MATCH; i++)
        if (t->match[i].value == value) return; /* alias or listed twice */
    if (t->matches < SR_MAX_MATCH) {
        t->match[t->matches].value = value;
        t->match[t->matches].size = size;
        strncpy(t->match[t->matches].symbol, sym, SR_NAME_MAX - 1);
        t->match[t->matches].symbol[SR_NAME_MAX - 1] = '\0';
    }
    t->matches++;
}

static int read_shdr(const uint8_t *f, size_t len, const Elf64_Ehdr *eh, unsigned i, Elf64_Shdr *out) {
    uint64_t off = eh->e_shoff + (uint64_t)i * sizeof(Elf64_Shdr);
    if (off < eh->e_shoff || !in_range(len, off, sizeof(*out))) return 0;
    memcpy(out, f + off, sizeof(*out));
    return 1;
}

static int unxz(const uint8_t *in, size_t in_len, uint8_t **out, size_t *out_len) {
    xz_crc32_init();
    xz_crc64_init();
    struct xz_dec *dec = xz_dec_init(XZ_DYNALLOC, 1u << 26);
    if (!dec) return SR_NO_MEMORY;
    size_t cap = 1 << 20;
    uint8_t *buf = (uint8_t *)malloc(cap);
    struct xz_buf b = {};
    b.in = in;
    b.in_size = in_len;
    int rc = SR_NO_MEMORY;
    while (buf) {
        b.out = buf;
        b.out_size = cap;
        enum xz_ret r = xz_dec_run(dec, &b);
        if (r == XZ_STREAM_END) {
            rc = SR_OK;
            break;
        }
        if (r != XZ_OK && r != XZ_UNSUPPORTED_CHECK) {
            rc = SR_BAD_XZ;
            break;
        }
        if (b.out_pos == b.out_size) {
            if (cap >= MAX_DEBUGDATA) break;
            cap *= 2;
            uint8_t *bigger = (uint8_t *)realloc(buf, cap);
            if (!bigger) break;
            buf = bigger;
        } else if (b.in_pos == b.in_size) {
            rc = SR_BAD_XZ; /* truncated */
            break;
        }
    }
    xz_dec_end(dec);
    if (rc != SR_OK) {
        free(buf);
        return rc;
    }
    *out = buf;
    *out_len = b.out_pos;
    return SR_OK;
}

/* depth 0: the library itself; depth 1: the decompressed .gnu_debugdata image. */
static int scan_image(const uint8_t *f, size_t len, struct sr_target *t, size_t count, int depth, int *tables) {
    Elf64_Ehdr eh;
    if (len < sizeof(eh)) return SR_BAD_ELF;
    memcpy(&eh, f, sizeof(eh));
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_shentsize != sizeof(Elf64_Shdr))
        return SR_BAD_ELF;

    Elf64_Shdr names;
    if (!read_shdr(f, len, &eh, eh.e_shstrndx, &names) || !in_range(len, names.sh_offset, names.sh_size))
        return SR_BAD_ELF;

    for (unsigned i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh;
        if (!read_shdr(f, len, &eh, i, &sh)) return SR_BAD_ELF;
        if (sh.sh_type == SHT_NOBITS || !in_range(len, sh.sh_offset, sh.sh_size)) continue;

        if (sh.sh_type == SHT_SYMTAB) {
            Elf64_Shdr str;
            if (!read_shdr(f, len, &eh, sh.sh_link, &str) || !in_range(len, str.sh_offset, str.sh_size))
                return SR_BAD_ELF;
            (*tables)++;
            for (uint64_t k = 0; k < sh.sh_size / sizeof(Elf64_Sym); k++) {
                Elf64_Sym s;
                memcpy(&s, f + sh.sh_offset + k * sizeof(s), sizeof(s));
                if (ELF64_ST_TYPE(s.st_info) != STT_FUNC || s.st_shndx == SHN_UNDEF || !s.st_value) continue;
                if (s.st_name >= str.sh_size) continue;
                const char *nm = (const char *)f + str.sh_offset + s.st_name;
                if (!memchr(nm, 0, str.sh_size - s.st_name)) continue;
                for (size_t j = 0; j < count; j++)
                    if (sr_symbol_matches(nm, t[j].name)) add_match(&t[j], nm, s.st_value, s.st_size);
            }
        } else if (depth == 0 && sh.sh_name < names.sh_size) {
            const char *sn = (const char *)f + names.sh_offset + sh.sh_name;
            size_t room = names.sh_size - sh.sh_name;
            if (room < sizeof(".gnu_debugdata") || memcmp(sn, ".gnu_debugdata", sizeof(".gnu_debugdata")) != 0)
                continue;
            uint8_t *img = NULL;
            size_t img_len = 0;
            int rc = unxz(f + sh.sh_offset, sh.sh_size, &img, &img_len);
            if (rc != SR_OK) return rc;
            rc = scan_image(img, img_len, t, count, 1, tables);
            free(img);
            if (rc != SR_OK) return rc;
        }
    }
    return SR_OK;
}

int sr_scan(const uint8_t *file, size_t len, struct sr_target *targets, size_t count) {
    int tables = 0;
    for (size_t j = 0; j < count; j++) targets[j].matches = 0;
    int rc = scan_image(file, len, targets, count, 0, &tables);
    if (rc != SR_OK) return rc;
    return tables ? SR_OK : SR_NO_SYMBOLS;
}
