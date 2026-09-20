#pragma once

#include <stdio.h>

/* Flushes buffered output and durably syncs file without closing it. */
int storage_sync_file(FILE *file);

/* Commit calls require distinct same-filesystem paths and serialized writers.
 * They consume/null temporary_file. New-file commits retain an unpublished temp
 * on every failure; replacement commits discard a failed stream because the
 * previous complete final remains available. */
int storage_commit_new_file(FILE **temporary_file,
                            const char *temporary_path,
                            const char *final_path);

/* Replaces final_path and keeps its previous contents at backup_path. */
int storage_commit_replace_file(FILE **temporary_file,
                                const char *temporary_path,
                                const char *final_path,
                                const char *backup_path);

/* Restores backup_path only when final_path is absent. */
int storage_recover_replace(const char *final_path, const char *backup_path);

/* Removes an incomplete final CSV row. A missing file is already repaired. */
int storage_repair_csv_tail(const char *path);
