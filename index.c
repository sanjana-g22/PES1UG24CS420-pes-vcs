// index.c — Staging area implementation

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry*)a)->path, ((const IndexEntry*)b)->path);
}

int index_save(const Index *index) {
    Index sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), compare_index_entries);

    char tmp_path[] = ".pes/index.tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted.entries[i].hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                sorted.entries[i].mode,
                hex,
                (unsigned long long)sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp_path, INDEX_FILE);
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  // No index file = empty index, not an error

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    unsigned long long mtime;
    unsigned int size;
    char path[512];

    while (index->count < MAX_INDEX_ENTRIES) {
        int r = fscanf(f, "%o %64s %llu %u %511s\n",
                       &mode, hex, &mtime, &size, path);
        if (r == EOF || r != 5) break;

        IndexEntry *e = &index->entries[index->count];
        e->mode     = mode;
        e->mtime_sec = (uint64_t)mtime;
        e->size     = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        if (hex_to_hash(hex, &e->hash) != 0) { fclose(f); return -1; }
        index->count++;
    }

    fclose(f);
    return 0;
}

int index_add(Index *index, const char *path) {
    // Open and read file
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    size_t file_size = (size_t)sz;

    // Allocate buffer (at least 1 byte to avoid malloc(0))
    unsigned char *buf = malloc(file_size + 1);
    if (!buf) { fclose(f); return -1; }

    if (file_size > 0) {
        size_t n = fread(buf, 1, file_size, f);
        if (n != file_size) { free(buf); fclose(f); return -1; }
    }
    fclose(f);

    // Write blob
    ObjectID hash;
    if (object_write(OBJ_BLOB, buf, file_size, &hash) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    // Get file metadata
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    // Update or create index entry
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    entry->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size      = (uint32_t)st.st_size;
    memcpy(entry->hash.hash, hash.hash, HASH_SIZE);
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';

    return index_save(index);
}