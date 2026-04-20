// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // Step 1: Build header
    const char *type_str;
    if      (type == OBJ_BLOB)   type_str = "blob";
    else if (type == OBJ_TREE)   type_str = "tree";
    else                          type_str = "commit";

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    // Step 2: Build full object = header + data
    size_t full_len = (size_t)header_len + len;
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, header_len);
    if (len > 0) memcpy(full + header_len, data, len);

    // Step 3: Compute hash
    compute_hash(full, full_len, id_out);

    // Step 4: Deduplication check
    if (object_exists(id_out)) {
        free(full);
        return 0;
    }

    // Step 5: Create shard directory
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);

    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755);

    // Step 6: Write to temp file then rename
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/tmp_XXXXXX", OBJECTS_DIR, hex);

    int fd = mkstemp(tmp_path);
    if (fd < 0) { free(full); return -1; }

    ssize_t written = write(fd, full, full_len);
    free(full);

    if (written != (ssize_t)full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    fsync(fd);
    close(fd);

    // Step 7: Rename to final path
    char final_path[256];
    snprintf(final_path, sizeof(final_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);

    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // Step 8: fsync directory
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_len_long = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_len_long < 0) { fclose(f); return -1; }
    size_t file_len = (size_t)file_len_long;

    uint8_t *buf = malloc(file_len + 1);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, file_len, f) != file_len) {
        fclose(f); free(buf); return -1;
    }
    fclose(f);

    // Verify integrity
    ObjectID computed;
    compute_hash(buf, file_len, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf); return -1;
    }

    // Find null separator
    uint8_t *null_pos = memchr(buf, '\0', file_len);
    if (!null_pos) { free(buf); return -1; }

    // Parse type
    if      (strncmp((char*)buf, "blob ",   5) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree ",   5) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Extract data
    size_t header_len = (size_t)(null_pos - buf) + 1;
    size_t data_len   = file_len - header_len;

    *data_out = malloc(data_len + 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, buf + header_len, data_len);
    *len_out = data_len;

    free(buf);
    return 0;
}