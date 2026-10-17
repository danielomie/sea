/*
 * Copyright (c) 2026 Daniel
 * SPDX-License-Identifier: MIT
 */

/* sea_cover.c — line coverage records (see sea_cover.h) */
#include "sea_cover.h"
#include "sea_vm.h"
#include "sea_chunk.h"
#include "sea_compiler.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *out_path;
static SeaVM      *root_vm;
static SeaChunk   *root_chunk;
static int         finished;

typedef struct { const char *src; unsigned char *lines; int len; } File;
static File *files;
static int   n_files, cap_files;

static void at_exit(void) { sea_cover_finish(); }

void sea_cover_start(const char *path) {
    out_path = path;
    sea_cover_enabled = true;
    atexit(at_exit);
}

void sea_cover_set_root(SeaVM *vm, SeaChunk *root) {
    root_vm = vm;
    root_chunk = root;
}

static File *file_for(const char *src) {
    for (int i = 0; i < n_files; i++)
        if (strcmp(files[i].src, src) == 0) return &files[i];
    if (n_files == cap_files) {
        cap_files = cap_files ? cap_files * 2 : 16;
        files = realloc(files, sizeof *files * (size_t)cap_files);
    }
    File *f = &files[n_files++];
    f->src = src;
    f->lines = NULL;
    f->len = 0;
    return f;
}

static void add_chunk(SeaChunk *c, const char *src) {
    if (!c) return;
    if (!src) src = c->source ? c->source : c->name;
    if (src && c->cover_len > 0) {
        File *f = file_for(src);
        if (c->cover_len > f->len) {
            f->lines = realloc(f->lines, (size_t)c->cover_len);
            memset(f->lines + f->len, 0, (size_t)(c->cover_len - f->len));
            f->len = c->cover_len;
        }
        for (int l = 0; l < c->cover_len; l++)
            if (c->cover[l] > f->lines[l]) f->lines[l] = c->cover[l];
    }
    for (int i = 0; i < c->proto_len; i++) add_chunk(c->protos[i], src);
}

void sea_cover_finish(void) {
    if (!out_path || finished) return;
    finished = 1;
    add_chunk(root_chunk, NULL);
    if (root_vm) for (SeaChunk *c = root_vm->module_chunks; c; c = c->next_owned) add_chunk(c, NULL);

    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    for (int i = 0; i < n_files; i++) {
        File *f = &files[i];
        int lf = 0, lh = 0;
        for (int l = 0; l < f->len; l++) if (f->lines[l]) { lf++; if (f->lines[l] == 2) lh++; }
        size_t need = strlen(f->src) + 64 + (size_t)lf * 24;
        if (n + need > cap) { while (n + need > cap) cap *= 2; buf = realloc(buf, cap); }
        n += (size_t)sprintf(buf + n, "SF:%s\n", f->src);
        for (int l = 0; l < f->len; l++)
            if (f->lines[l]) n += (size_t)sprintf(buf + n, "DA:%d,%d\n", l, f->lines[l] == 2);
        n += (size_t)sprintf(buf + n, "LF:%d\nLH:%d\nend_of_record\n", lf, lh);
    }
    int fd = open(out_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        size_t done = 0;
        while (done < n) {
            ssize_t w = write(fd, buf + done, n - done);
            if (w <= 0) break;
            done += (size_t)w;
        }
        close(fd);
    } else {
        fprintf(stderr, "sea: cannot write coverage to %s\n", out_path);
    }
    free(buf);
}
