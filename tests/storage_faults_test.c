/* Complete storage_io.c with real owned files and controlled I/O faults. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define fixture_pid _getpid
#else
#include <unistd.h>
#define fixture_pid getpid
#endif
enum point { NONE, FLUSH, SYNC, CLOSE, STAT, REMOVE, RENAME, POINTS };
enum operation { DIRECT, NEW_FILE, REPLACE, RECOVER };
static enum point fault;
static unsigned fail_call, calls[POINTS], handles, publications, cases, failures;
static int fault_errno;
static bool stale_success, flagged, close_error, remove_error, rollback_error, flush_flag;
static FILE *active;
static char paths[3][128];
static bool owned[3];
static unsigned slot(const char *path)
{ for (unsigned i = 0; i < 3; i++) if (!strcmp(path, paths[i])) return i; fprintf(stderr, "Unknown path: %s\n", path); abort(); }
static bool inject(enum point point)
{
    calls[point]++;
    if (fault != point || calls[point] != fail_call) return false;
    if (fault_errno) errno = fault_errno;
    return true;
}
static void stale(void) { if (stale_success) errno = ENOENT; }
static int fixture_flush(FILE *file)
{
    assert(file == active); int result = fflush(file);
    if (inject(FLUSH)) return EOF;
    if (flush_flag) { flagged = true; if (fault_errno) errno = fault_errno; }
    else if (!result) stale();
    return result;
}
static int fixture_sync(int fd)
{
#ifdef _WIN32
    int result = _commit(fd);
#else
    int result = fsync(fd);
#endif
    if (inject(SYNC)) return -1;
    if (!result) stale();
    return result;
}
static int fixture_close(FILE *file)
{
    assert(file == active && handles == 1); int result = fclose(file); active = NULL; handles--;
    if (inject(CLOSE) || close_error) { if (close_error) errno = EPERM; return EOF; }
    if (!result) stale();
    return result;
}
static int fixture_error(FILE *file) { assert(file == active); return flagged || ferror(file); }
static int fixture_stat(const char *path, struct stat *info)
{
    if (inject(STAT)) return -1;
    int result = stat(paths[slot(path)], info);
    if (!result) stale();
    return result;
}
static int fixture_remove(const char *path)
{
    unsigned i = slot(path);
    if (inject(REMOVE) || remove_error) { if (remove_error) errno = EACCES; return -1; }
    if (!owned[i]) { errno = ENOENT; return -1; }
    int result = remove(paths[i]);
    if (!result) { owned[i] = false; stale(); }
    return result;
}
static int fixture_rename(const char *from, const char *to)
{
    unsigned a = slot(from), b = slot(to); assert(owned[a] && a != b);
    if (inject(RENAME)) return -1;
    if (rollback_error && a == 2 && b == 1) { errno = EPERM; return -1; }
    int result = rename(paths[a], paths[b]);
    if (!result) { owned[a] = false; owned[b] = true; publications += a == 0; stale(); }
    return result;
}
#define ferror fixture_error
#define fflush fixture_flush
#define fclose fixture_close
#define stat(path, info) fixture_stat(path, info)
#define remove fixture_remove
#define rename fixture_rename
#ifdef _WIN32
#define _commit fixture_sync
#else
#define fsync fixture_sync
#endif
#include "storage_source.inc"
#undef ferror
#undef fflush
#undef fclose
#undef stat
#undef remove
#undef rename
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
static const char *new_bytes = "new complete\n", *old_bytes = "previous complete\n", *backup_bytes = "older complete\n";
static void seed(unsigned i, const char *bytes)
{ assert(!owned[i]); FILE *file = fopen(paths[i], "wb"); assert(file); owned[i] = true; assert(fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes)); assert(fclose(file) == 0); }
static bool matches(unsigned i, const char *expected)
{
    FILE *file = fopen(paths[i], "rb");
    if (!expected) { if (file) fclose(file); return !file && errno == ENOENT; }
    if (!file) return false;
    char bytes[128]; size_t length = fread(bytes, 1, sizeof(bytes), file);
    bool ok = !ferror(file) && length == strlen(expected) && !memcmp(bytes, expected, length);
    return fclose(file) == 0 && ok;
}
static bool generations(const char *temp, const char *final, const char *backup)
{ return matches(0, temp) && matches(1, final) && matches(2, backup); }
static void clean(void)
{ assert(!handles && !active); for (unsigned i = 0; i < 3; i++) if (owned[i]) { assert(remove(paths[i]) == 0); owned[i] = false; } }
static void prepare(bool final, bool backup)
{
    clean(); seed(0, new_bytes); if (final) seed(1, old_bytes); if (backup) seed(2, backup_bytes);
    fault = NONE; fault_errno = 0; fail_call = 1; memset(calls, 0, sizeof(calls)); publications = 0;
    stale_success = true; flagged = close_error = remove_error = rollback_error = flush_flag = false; errno = ENOENT;
}
static FILE *open_temp(void)
{ assert(!active && !handles); active = fopen(paths[0], "r+b"); assert(active); handles++; return active; }
static void result(const char *name, bool ok, bool bytes_ok, int error)
{
    ok = ok && bytes_ok && !handles && !active;
    printf("%s %s bytes_verified=%u error=%d published=%u flushes=%u syncs=%u closes=%u stats=%u removes=%u renames=%u handles=%u\n",
           ok ? "PASS" : "FAIL", name, bytes_ok, error, publications, calls[FLUSH], calls[SYNC], calls[CLOSE], calls[STAT], calls[REMOVE], calls[RENAME], handles);
    cases++; failures += !ok;
}
static void direct_case(const char *name, enum point point, int error, bool preexisting_flag, bool new_flush_flag)
{
    prepare(false, false); FILE *file = open_temp(); fault = point; fault_errno = error; flagged = preexisting_flag; flush_flag = new_flush_flag;
    errno = preexisting_flag ? error : ENOENT;
    int status = storage_sync_file(file), cause = errno;
    bool expected_error = point != NONE || preexisting_flag || new_flush_flag;
    bool ok = status == (expected_error ? -1 : 0) && (!expected_error || cause == (error ? error : EIO));
    if (preexisting_flag) ok = ok && !calls[FLUSH] && !calls[SYNC];
    if (new_flush_flag || point == FLUSH) ok = ok && !calls[SYNC];
    fixture_close(file);
    result(name, ok, generations(new_bytes, NULL, NULL), expected_error ? cause : 0);
}
static void new_case(const char *name, enum point point, int error, bool existing, bool cleanup_error)
{
    prepare(existing, false); FILE *file = open_temp(); fault = point; fault_errno = error; close_error = cleanup_error;
    int status = storage_commit_new_file(&file, paths[0], paths[1]), cause = errno;
    bool failed = point != NONE || existing;
    bool ok = !file && !handles && status == (failed ? -1 : 0) && (!failed || cause == (existing ? EEXIST : error ? error : EIO));
    if (point == STAT) ok = ok && !calls[RENAME];
    if (point == FLUSH || point == SYNC || point == CLOSE) ok = ok && !calls[STAT] && !calls[RENAME];
    result(name, ok, failed ? generations(new_bytes, existing ? old_bytes : NULL, NULL) : generations(NULL, new_bytes, NULL), failed ? cause : 0);
}
static void replace_case(const char *name, enum point point, unsigned at, int error, bool exists, bool cleanup_error, bool rollback_failure)
{
    prepare(exists, exists); FILE *file = open_temp(); fault = point; fail_call = at; fault_errno = error;
    remove_error = cleanup_error; rollback_error = rollback_failure;
    int status = storage_commit_replace_file(&file, paths[0], paths[1], paths[2]), cause = errno;
    bool failed = point != NONE;
    bool ok = !file && !handles && status == (failed ? -1 : 0) && (!failed || cause == (error ? error : EIO));
    const char *temp = failed ? new_bytes : NULL, *final = failed ? (exists ? old_bytes : NULL) : new_bytes;
    const char *backup = exists ? (failed ? backup_bytes : old_bytes) : NULL;
    if (point == FLUSH || point == SYNC || point == CLOSE) { temp = cleanup_error ? new_bytes : NULL; ok = ok && !calls[STAT] && !calls[RENAME]; }
    if (point == STAT || point == REMOVE) ok = ok && !calls[RENAME];
    if (point == RENAME && at == 1 && exists) backup = NULL;
    if (point == RENAME && at == 2) { backup = rollback_failure ? old_bytes : NULL; final = rollback_failure ? NULL : old_bytes; }
    result(name, ok, generations(temp, final, backup), failed ? cause : 0);
}
static void recover_case(const char *name, enum point point, unsigned at, int error, bool final, bool backup)
{
    prepare(final, backup); fault = point; fail_call = at; fault_errno = error;
    int status = storage_recover_replace(paths[1], paths[2]), cause = errno;
    bool failed = point != NONE;
    bool ok = status == (failed ? -1 : 0) && (!failed || cause == (error ? error : EIO));
    if (point == STAT) ok = ok && !calls[RENAME];
    result(name, ok, generations(new_bytes, !failed && !final && backup ? backup_bytes : final ? old_bytes : NULL,
                                 !failed && !final ? NULL : backup ? backup_bytes : NULL), failed ? cause : 0);
}
static void repeat_recovery(void)
{
    bool ok = true, bytes_ok = true; int cause = 0;
    for (unsigned i = 0; i < 25; i++) {
        prepare(true, true); fault = RENAME; fail_call = 2; fault_errno = 0; rollback_error = true; FILE *file = open_temp();
        int status = storage_commit_replace_file(&file, paths[0], paths[1], paths[2]); cause = errno;
        ok = status == -1 && cause == EIO && !file && !handles && ok;
        bytes_ok = generations(new_bytes, NULL, old_bytes) && bytes_ok;
        fault = NONE; rollback_error = false;
        ok = storage_recover_replace(paths[1], paths[2]) == 0 && ok; bytes_ok = generations(new_bytes, old_bytes, NULL) && bytes_ok;
        file = open_temp(); ok = storage_commit_replace_file(&file, paths[0], paths[1], paths[2]) == 0 && !file && !handles && ok;
        bytes_ok = generations(NULL, new_bytes, old_bytes) && bytes_ok;
    }
    result("rollback-recover-republish-25", ok, bytes_ok, cause);
}
int main(void)
{
    const char *extensions[] = {"TMP", "CSV", "BAK"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(paths[i], sizeof(paths[i]), ".storage_faults_%d.%s", fixture_pid(), extensions[i]);
        struct stat info; if (stat(paths[i], &info) == 0 || errno != ENOENT) { fputs("Refusing existing fixture file\n", stderr); return 2; }
    }
    direct_case("sync-success", NONE, 0, false, false);
    direct_case("sync-flush-error", FLUSH, EACCES, false, false); direct_case("sync-flush-no-errno", FLUSH, 0, false, false);
    direct_case("sync-descriptor-error", SYNC, EACCES, false, false); direct_case("sync-descriptor-no-errno", SYNC, 0, false, false);
    direct_case("sync-existing-stream-error", NONE, ENOSPC, true, false); direct_case("sync-existing-stream-no-errno", NONE, 0, true, false);
    direct_case("sync-flush-stream-error", NONE, ENOSPC, false, true); direct_case("sync-flush-stream-no-errno", NONE, 0, false, true);
    new_case("new-success", NONE, 0, false, false); new_case("new-existing-final", NONE, 0, true, false);
    const enum point new_points[] = {FLUSH, SYNC, CLOSE, STAT, RENAME};
    const char *new_names[] = {"flush", "sync", "close", "probe", "publish"};
    for (unsigned i = 0; i < 5; i++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[64]; snprintf(name, sizeof(name), "new-%s-%s", new_names[i], missing ? "no-errno" : "error");
        new_case(name, new_points[i], missing ? 0 : EACCES, false, false);
    }
    new_case("new-first-sync-error", SYNC, EACCES, false, true); new_case("new-first-flush-error", FLUSH, EACCES, false, true);
    replace_case("replace-success", NONE, 0, 0, true, false, false); replace_case("replace-first-creation", NONE, 0, 0, false, false, false);
    const enum point replace_points[] = {FLUSH, SYNC, CLOSE, STAT, REMOVE, RENAME, RENAME};
    const unsigned replace_calls[] = {1, 1, 1, 1, 1, 1, 2};
    const char *replace_names[] = {"flush", "sync", "close", "probe", "remove-backup", "backup", "publish"};
    for (unsigned i = 0; i < 7; i++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[64]; snprintf(name, sizeof(name), "replace-%s-%s", replace_names[i], missing ? "no-errno" : "error");
        replace_case(name, replace_points[i], replace_calls[i], missing ? 0 : EACCES, true, false, false);
    }
    replace_case("replace-first-error-discard", SYNC, 1, ENOSPC, true, true, false);
    replace_case("replace-first-error-rollback", RENAME, 2, EACCES, true, false, true);
    replace_case("replace-no-errno-rollback", RENAME, 2, 0, true, false, true);
    recover_case("recover-existing-final", NONE, 0, 0, true, true);
    recover_case("recover-backup", NONE, 0, 0, false, true); recover_case("recover-missing", NONE, 0, 0, false, false);
    for (unsigned stage = 0; stage < 3; stage++) for (unsigned missing = 0; missing < 2; missing++) {
        char name[64]; snprintf(name, sizeof(name), "recover-%s-%s", stage == 0 ? "final-probe" : stage == 1 ? "backup-probe" : "rename", missing ? "no-errno" : "error");
        recover_case(name, stage == 2 ? RENAME : STAT, stage == 1 ? 2 : 1, missing ? 0 : EACCES, false, true);
    }
    const char *invalid_names[] = {"null-sync", "null-new", "null-replace", "null-recover", "null-repair"};
    for (unsigned i = 0; i < 5; i++) {
        prepare(false, false); int status = i == 0 ? storage_sync_file(NULL) : i == 1 ? storage_commit_new_file(NULL, paths[0], paths[1]) :
            i == 2 ? storage_commit_replace_file(NULL, paths[0], paths[1], paths[2]) : i == 3 ? storage_recover_replace(NULL, paths[2]) : storage_repair_csv_tail(NULL);
        int cause = errno; result(invalid_names[i], status == -1 && cause == EINVAL && !calls[STAT] && !calls[RENAME], generations(new_bytes, NULL, NULL), cause);
    }
    repeat_recovery(); clean();
    printf("%s %u storage fault cases failures=%u handles=%u (native files; controlled I/O faults)\n", failures ? "FAIL" : "PASS", cases, failures, handles);
    return failures ? 1 : 0;
}
