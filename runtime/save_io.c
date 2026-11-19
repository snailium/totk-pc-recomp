// Save data import and export, the CLI modes --save-export and --save-import (main.c). They run while the game is
// not running, so they see the committed save and no transaction: <profile>/save/<uid> is the save the next run
// mounts, and an import that replaced it mid-run would be overwritten by the working copy's next commit.
//
// The export copies that tree into a folder the player names, with a manifest.json that names the title and the
// account, so an import can check what it is getting. The import accepts, without configuration, everything a
// player is likely to hand it: our own export (the manifest), a console or JKSV dump (the save's folders directly,
// as the Switch stores them), and a Ryujinx or yuzu save directory (the same files nested somewhere inside the
// emulator's own tree, bis/user/save/<account>/<title id> and nand/user/save/<account>/<title id>). It finds the
// save by shape, not by path: a directory whose contents look like this title's current save — the same folder
// names at the top, or a manifest — is a candidate, wherever the emulator put it. The best candidate wins; every
// candidate is printed, and --save-dry-run stops before anything is written.
//
// The save is always imported into this install's own account directory: the game's save files do not carry the
// account, it only names the folder on the Switch, so no account.uid has to travel with the data. The current
// save is copied into save-backup/import-N first, so an import is one directory copy away from undone.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "target.h"

#define PATH_CAP 1100
#define SCAN_DEPTH 8

// ---- small local directory helpers (fs_host.c keeps its own; the save tool runs before the game starts)
typedef struct { char *name; int kind; uint64_t size; } Entry;
typedef struct { Entry *e; int n, cap; } Entries;

static int entry_add(const char *name, int kind, uint64_t size, void *ctx) {
    Entries *l = ctx;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        Entry *e = realloc(l->e, l->cap * sizeof *e);
        if (!e) return 0;
        l->e = e;
    }
    char *dup = malloc(strlen(name) + 1);
    if (!dup) return 0;
    strcpy(dup, name);
    l->e[l->n] = (Entry){dup, kind, size};
    l->n++;
    return 1;
}

// The directory's entries (every one but "." and ".."), PLAT_FS_OK (0) when read, the error otherwise. Free with
// entries_free.
static int entries_read(const char *dir, Entries *l) {
    memset(l, 0, sizeof *l);
    return plat_fs_list(dir, entry_add, l);
}

static void entries_free(Entries *l) {
    for (int i = 0; i < l->n; i++) free(l->e[i].name);
    free(l->e);
    memset(l, 0, sizeof *l);
}

static int mkdir_one(const char *p) {
    int e = plat_fs_mkdir(p);
    return !e || (e == PLAT_FS_EXISTS && plat_fs_kind(p) == PLAT_FS_DIR);
}

static void mkdir_p(const char *p) {
    char buf[PATH_CAP];
    if (snprintf(buf, sizeof buf, "%s", p) >= (int)sizeof buf) rt_fatal("save: path too long: %s", p);
    for (char *q = buf + 1; *q; q++)
        if (*q == '/' || *q == '\\') {
            char c = *q;
            *q = 0;
            mkdir_one(buf);
            *q = c;
        }
    if (!mkdir_one(buf)) rt_fatal("save: cannot create directory %s", buf);
}

static char *joined(const char *a, const char *b) {
    char *p = malloc(strlen(a) + strlen(b) + 2);
    if (!p) rt_fatal("save: path allocation");
    sprintf(p, "%s/%s", a, b);
    return p;
}

// Copy the file, or the directory tree, at src to a path of the same name under dst. 0 on success.
static int tree_copy(const char *src, const char *dst) {
    int kind = plat_fs_kind(src);
    if (kind == PLAT_FS_FILE) {
        uint64_t n;
        uint8_t *b = plat_file_read(src, &n);
        if (!b) return -1;
        int ok = plat_file_write_atomic(dst, b, (size_t)n);
        free(b);
        return ok ? 0 : -1;
    }
    if (kind != PLAT_FS_DIR) return -1;
    mkdir_p(dst);
    Entries l;
    if (entries_read(src, &l)) {
        entries_free(&l);
        return -1;
    }
    int bad = 0;
    for (int i = 0; i < l.n && !bad; i++) {
        char *s = joined(src, l.e[i].name), *d = joined(dst, l.e[i].name);
        bad = l.e[i].kind == PLAT_FS_LINK ? 0 : tree_copy(s, d);   // links are not save data
        free(s), free(d);
    }
    entries_free(&l);
    return bad;
}

static int tree_remove(const char *path) {
    int kind = plat_fs_kind(path);
    if (kind == PLAT_FS_MISSING) return 0;
    if (kind == PLAT_FS_FILE || kind == PLAT_FS_LINK) return plat_fs_unlink(path) ? -1 : 0;   // 0 is PLAT_FS_OK
    if (kind != PLAT_FS_DIR) return -1;
    Entries l;
    if (entries_read(path, &l)) {
        entries_free(&l);
        return -1;
    }
    int bad = 0;
    for (int i = 0; i < l.n; i++) {
        char *c = joined(path, l.e[i].name);
        bad |= tree_remove(c);
        free(c);
    }
    entries_free(&l);
    return plat_fs_rmdir(path) ? -1 : bad;   // 0 is PLAT_FS_OK
}

// ---- this install's save paths
static void save_dir(char out[PATH_CAP]) {
    uint64_t uid[2];
    account_uid(uid);
    if (snprintf(out, PATH_CAP, "%s/save/%016llx%016llx", rt_config()->profile, (unsigned long long)uid[1],
                 (unsigned long long)uid[0]) >= PATH_CAP)
        rt_fatal("save: save path too long");
}

static void read_dir_of(char out[PATH_CAP], const char *root) {
    if (snprintf(out, PATH_CAP, "%s/%s", rt_config()->profile, root) >= PATH_CAP) rt_fatal("save: path too long");
}

// ---- export
static void tree_count(const char *path, int *files, uint64_t *bytes) {
    Entries l;
    if (entries_read(path, &l)) {
        entries_free(&l);
        return;
    }
    for (int i = 0; i < l.n; i++) {
        char *c = joined(path, l.e[i].name);
        if (l.e[i].kind == PLAT_FS_FILE) (*files)++, *bytes += l.e[i].size;
        else if (l.e[i].kind == PLAT_FS_DIR) tree_count(c, files, bytes);
        free(c);
    }
    entries_free(&l);
}

int save_export(const char *out) {
    char save[PATH_CAP];
    save_dir(save);
    Entries l;
    if (entries_read(save, &l) || !l.n) {
        printf("save-export: no save data in %s yet (play the game once first)\n", save);
        entries_free(&l);
        return 1;
    }
    int files = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < l.n; i++) {
        char *c = joined(save, l.e[i].name);
        if (l.e[i].kind == PLAT_FS_FILE) files++, bytes += l.e[i].size;
        else if (l.e[i].kind == PLAT_FS_DIR) tree_count(c, &files, &bytes);
        free(c);
    }
    mkdir_p(out);
    if (tree_copy(save, out)) {
        printf("save-export: copying into %s failed\n", out);
        entries_free(&l);
        return 1;
    }
    entries_free(&l);
    char playtime[64] = "";
    char pt_path[PATH_CAP];
    if (snprintf(pt_path, sizeof pt_path, "%s/playtime.txt", rt_config()->profile) < (int)sizeof pt_path) {
        uint64_t n;
        uint8_t *b = plat_file_read(pt_path, &n);
        if (b && n < sizeof playtime) {
            memcpy(playtime, b, (size_t)n);
            playtime[n] = 0;
            for (char *q = playtime; *q; q++)   // keep the manifest one line: no newline inside the string
                if (*q == '\n' || *q == '\r') *q = ' ';
        }
        free(b);
    }
    char manifest_path[PATH_CAP], manifest[512], when[32];
    uint64_t uid[2];
    account_uid(uid);
    struct tm tm;
    plat_local_time(&tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    if (snprintf(manifest_path, sizeof manifest_path, "%s/manifest.json", out) >= (int)sizeof manifest_path ||
        snprintf(manifest, sizeof manifest,
                 "{\"format\": 1, \"title\": \"%s\", \"account\": \"%016llx%016llx\", \"exported\": \"%s\", "
                 "\"files\": %d, \"bytes\": %llu, \"playtime\": \"%s\"}\n",
                 rt_target()->name, (unsigned long long)uid[1], (unsigned long long)uid[0], when, files,
                 (unsigned long long)bytes, playtime) >= (int)sizeof manifest ||
        !plat_file_write_atomic(manifest_path, manifest, strlen(manifest))) {
        printf("save-export: cannot write %s/manifest.json\n", out);
        return 1;
    }
    printf("save-export: %d file(s), %llu bytes -> %s (with manifest.json; the folder is the export, zip it to move it)\n",
           files, (unsigned long long)bytes, out);
    return 0;
}

// ---- import: finding the save by shape
typedef struct {
    char path[PATH_CAP];
    int score, files;
} Candidate;

typedef struct {
    const Entries *current;   // the top level of this install's save, NULL when there is none yet
    Candidate c[32];
    int n;
} Scan;

static int has_child(const Entries *l, const char *name) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].name, name)) return 1;
    return 0;
}

// Is dir a plausible save root, and how good a one? The strong sign is our manifest.json; without one, the folder
// names of the current save are: a console dump, a JKSV export and an emulator's save directory all hold the game's
// own folders (Ryza 2's GameData00, SystemData, AutoSave), while the emulator's tree around them - bis, user, save,
// the account and title-id folders - does not. With no current save to compare against, a folder whose children are
// all directories is the best guess: that is the shape of every save root, and the leaves around it hold files.
static int consider(const char *dir, const Entries *children, Scan *s) {
    int score = 0, files = 0, dirs = 0, manifest = 0;
    for (int i = 0; i < children->n; i++) {
        if (children->e[i].kind == PLAT_FS_DIR) dirs++;
        else if (children->e[i].kind == PLAT_FS_FILE) files++;
        if (!strcmp(children->e[i].name, "manifest.json")) manifest = 1;
        if (s->current && has_child(s->current, children->e[i].name))
            score += children->e[i].kind == PLAT_FS_DIR ? 3 : 1;
    }
    if (manifest) score += 100;
    else if (s->current && score < 3) return 0;   // not this game's folder names
    else if (!s->current && dirs < 2) return 0;   // no reference: a save root is folders (a stray file beside them
                                                 // does not disqualify - the copy step leaves such files out)
    if (s->n < (int)(sizeof s->c / sizeof *s->c)) {
        snprintf(s->c[s->n].path, sizeof s->c[s->n].path, "%s", dir);
        s->c[s->n].score = score ? score : 1;
        s->c[s->n].files = files;
        s->n++;
    }
    return 1;
}

static void scan(const char *dir, int depth, Scan *s) {
    if (depth > SCAN_DEPTH) return;
    Entries l;
    if (entries_read(dir, &l)) {
        entries_free(&l);
        return;
    }
    consider(dir, &l, s);
    for (int i = 0; i < l.n; i++) {
        if (l.e[i].kind != PLAT_FS_DIR) continue;
        char *child = joined(dir, l.e[i].name);
        scan(child, depth + 1, s);
        free(child);
        if (s->n >= (int)(sizeof s->c / sizeof *s->c)) break;
    }
    entries_free(&l);
}

static int better(const Candidate *a, const Candidate *b) {
    if (a->score != b->score) return a->score > b->score;
    int da = 0, db = 0;
    for (const char *q = a->path; *q; q++) da += *q == '/';
    for (const char *q = b->path; *q; q++) db += *q == '/';
    if (da != db) return da < db;   // shallower first: the emulator trees nest the save under their own folders
    return strcmp(a->path, b->path) < 0;
}

int save_import(const char *in, int dry_run) {
    if (plat_fs_kind(in) != PLAT_FS_DIR) {
        printf("save-import: %s is not a folder (unzip an archive export first)\n", in);
        return 1;
    }
    char tx[PATH_CAP];
    read_dir_of(tx, "save-tx/new");
    if (plat_fs_kind(tx) == PLAT_FS_DIR) {
        printf("save-import: %s holds an unfinished save transaction; start the game once and let it recover, "
               "then import\n", tx);
        return 1;
    }
    char save[PATH_CAP];
    save_dir(save);
    Entries current;
    int have_save = entries_read(save, &current) == 0 && current.n > 0;
    Scan s = {.current = have_save ? &current : NULL, .n = 0};
    scan(in, 0, &s);
    if (!s.n) {
        printf("save-import: nothing that looks like save data under %s\n", in);
        return 1;
    }
    int best = 0;
    for (int i = 1; i < s.n; i++)
        if (better(&s.c[i], &s.c[best])) best = i;
    for (int i = 0; i < s.n; i++)
        printf("save-import: %s%s: %d point(s), %d file(s)\n", s.c[i].path, i == best ? "  [chosen]" : "",
               s.c[i].score, s.c[i].files);
    if (dry_run) {
        printf("save-import: dry run, nothing written (the chosen folder would replace %s)\n", save);
        return 0;
    }
    // The backup: save-backup/import-N, the first free N, so a failed import never overwrites an earlier one.
    char backup[PATH_CAP];
    read_dir_of(backup, "save-backup");
    mkdir_p(backup);
    int slot = 1;
    char try[PATH_CAP];
    do {
        if (snprintf(try, sizeof try, "%s/import-%d", backup, slot) >= (int)sizeof try) rt_fatal("save: path too long");
        slot++;
    } while (plat_fs_kind(try) != PLAT_FS_MISSING && slot < 1000);
    if (plat_fs_kind(save) == PLAT_FS_DIR && tree_copy(save, try)) {
        printf("save-import: cannot back up the current save into %s\n", try);
        return 1;
    }
    // Replace: out with the old save, in with the chosen one. manifest.json stays out: it describes the export,
    // not the save. account.uid is untouched - the save files do not carry the account, only their folder does.
    if (tree_remove(save)) {
        printf("save-import: cannot clear %s; the previous save is in %s\n", save, try);
        return 1;
    }
    mkdir_p(save);
    Entries src;
    if (entries_read(s.c[best].path, &src)) {
        entries_free(&src);
        if (have_save) entries_free(&current);
        return 1;
    }
    int dirs = 0;
    for (int i = 0; i < src.n; i++)
        if (src.e[i].kind == PLAT_FS_DIR) dirs++;
    int bad = 0;
    for (int i = 0; i < src.n && !bad; i++) {
        // manifest.json describes the export, not the save; a link is not save data; a top-level file beside the
        // save's folders is a stray the source folder carried (an archive of its own, a readme) - with a current
        // save only its own file names copy, without one a folder-shaped save takes no loose files at all. The
        // save's own additions are folders (new save slots), which all copy.
        if (!strcmp(src.e[i].name, "manifest.json") || src.e[i].kind == PLAT_FS_LINK) continue;
        if (src.e[i].kind == PLAT_FS_FILE && ((dirs > 0 && !have_save) ||
                                              (have_save && (dirs > 0 || !has_child(&current, src.e[i].name)))))
            continue;
        char *a = joined(s.c[best].path, src.e[i].name), *b = joined(save, src.e[i].name);
        bad = tree_copy(a, b);
        free(a), free(b);
    }
    entries_free(&src);
    if (have_save) entries_free(&current);
    if (bad) {
        printf("save-import: copying failed; the complete previous save is in %s\n", try);
        return 1;
    }
    printf("save-import: %s -> %s (%d file(s)); the previous save is in %s\n", s.c[best].path, save,
           s.c[best].files, try);
    return 0;
}
