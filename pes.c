// pes.c — CLI entry point

#include "pes.h"
#include "index.h"
#include "commit.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/types.h>

// Forward declarations
int commit_create(const char *message, ObjectID *commit_id_out);
int commit_walk(commit_walk_fn callback, void *ctx);

// ─── cmd_init ────────────────────────────────────────────────────────────────
void cmd_init(void) {
    // provided by pes.c original — but we rewrite it here
    // Create .pes directory structure
    struct stat st;
    if (stat(".pes", &st) == 0) {
        printf("Already initialized PES repository in .pes/\n");
        return;
    }
    mkdir(".pes", 0755);
    mkdir(".pes/objects", 0755);
    mkdir(".pes/refs", 0755);
    mkdir(".pes/refs/heads", 0755);

    FILE *f = fopen(".pes/HEAD", "w");
    if (f) { fprintf(f, "ref: refs/heads/main\n"); fclose(f); }

    printf("Initialized empty PES repository in .pes/\n");
}

// ─── cmd_add ─────────────────────────────────────────────────────────────────
void cmd_add(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: pes add <file>...\n");
        return;
    }
    Index *index = malloc(sizeof(Index));
    if (!index) { fprintf(stderr, "error: out of memory\n"); return; }
    index->count = 0;
    if (index_load(index) != 0) {
        fprintf(stderr, "error: failed to load index\n");
        free(index);
        return;
    }
    for (int i = 2; i < argc; i++) {
        if (index_add(index, argv[i]) != 0) {
            fprintf(stderr, "error: failed to add '%s'\n", argv[i]);
        }
    }
    free(index);
}

// ─── cmd_status ──────────────────────────────────────────────────────────────
void cmd_status(void) {
    Index *index = malloc(sizeof(Index));
    if (!index) { fprintf(stderr, "error: out of memory\n"); return; }
    index->count = 0;
    if (index_load(index) != 0) {
        fprintf(stderr, "error: failed to load index\n");
        free(index);
        return;
    }
    index_status(index);
    free(index);
}

// ─── cmd_commit ──────────────────────────────────────────────────────────────
void cmd_commit(int argc, char *argv[]) {
    const char *message = NULL;
    for (int i = 2; i < argc - 1; i++) {
        if (strcmp(argv[i], "-m") == 0) {
            message = argv[i + 1];
            break;
        }
    }
    if (!message) {
        fprintf(stderr, "error: commit requires a message (-m \"message\")\n");
        return;
    }

    ObjectID commit_id;
    if (commit_create(message, &commit_id) != 0) {
        fprintf(stderr, "error: commit failed\n");
        return;
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit_id, hex);
    printf("Committed: %.12s... %s\n", hex, message);
}

// ─── cmd_log ─────────────────────────────────────────────────────────────────
static void print_commit(const ObjectID *id, const Commit *c, void *ctx) {
    (void)ctx;
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    printf("commit %s\n", hex);
    printf("Author: %s\n", c->author);
    printf("Date:   %" PRIu64 "\n", c->timestamp);
    printf("\n    %s\n\n", c->message);
}

void cmd_log(void) {
    if (commit_walk(print_commit, NULL) != 0) {
        fprintf(stderr, "error: no commits yet\n");
    }
}

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: pes <command> [args]\n");
        fprintf(stderr, "\nCommands:\n");
        fprintf(stderr, "  init              Create a new PES repository\n");
        fprintf(stderr, "  add <file>...     Stage files for commit\n");
        fprintf(stderr, "  status            Show working directory status\n");
        fprintf(stderr, "  commit -m <msg>   Create a commit from staged files\n");
        fprintf(stderr, "  log               Show commit history\n");
        return 1;
    }

    const char *cmd = argv[1];

    if      (strcmp(cmd, "init")   == 0) cmd_init();
    else if (strcmp(cmd, "add")    == 0) cmd_add(argc, argv);
    else if (strcmp(cmd, "status") == 0) cmd_status();
    else if (strcmp(cmd, "commit") == 0) cmd_commit(argc, argv);
    else if (strcmp(cmd, "log")    == 0) cmd_log();
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    return 0;
}