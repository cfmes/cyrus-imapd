/* lcb_append.c -- replication-based backup api - append functions
 *
 * Copyright (c) 1994-2016 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include <assert.h>
#include <syslog.h>

#include "lib/exitcodes.h"
#include "lib/sqldb.h"
#include "lib/xmalloc.h"
#include "lib/xsha1.h"

#include "imap/imap_err.h"

#include "backup/backup.h"

#define LIBCYRUS_BACKUP_SOURCE /* this file is part of libcyrus_backup */
#include "backup/lcb_internal.h"
#include "backup/lcb_sqlconsts.h"

/* FIXME make it xsha1_file and do it properly */
#define SHA1_LIMIT_WHOLE_FILE ((size_t) -1)
extern const char *_sha1_file(int fd, const char *fname, size_t limit,
                              char buf[2 * SHA1_DIGEST_LENGTH + 1]);
/***********************************************/

HIDDEN int backup_real_append_start(struct backup *backup,
                                    time_t ts, off_t offset,
                                    const char *file_sha1,
                                    int index_only,
                                    enum backup_append_flush flush)
{
    if (backup->append_state != NULL
        && backup->append_state->mode != BACKUP_APPEND_INACTIVE) {
        fatal("backup append already started", EC_SOFTWARE);
    }

    if (!backup->append_state)
        backup->append_state = xzmalloc(sizeof(*backup->append_state));

    if (index_only) backup->append_state->mode |= BACKUP_APPEND_INDEXONLY;

    backup->append_state->wrote = 0;
    SHA1_Init(&backup->append_state->sha_ctx);

    char header[80];
    snprintf(header, sizeof(header), "# cyrus backup: chunk start\r\n");

    if (!index_only) {
        if (!backup->append_state->gzfile) {
            backup->append_state->gzfile = gzdopen(backup->fd, "ab");
            if (!backup->append_state->gzfile) {
                fprintf(stderr, "%s: gzdopen fd %i failed: %s\n",
                        __func__, backup->fd, strerror(errno));
                goto error;
            }
        }

        // FIXME check for error return
        gzwrite(backup->append_state->gzfile, header, strlen(header));
        if (flush)
            gzflush(backup->append_state->gzfile, Z_FULL_FLUSH);
    }

    SHA1_Update(&backup->append_state->sha_ctx, header, strlen(header));
    backup->append_state->wrote += strlen(header);

    struct sqldb_bindval bval[] = {
        { ":ts_start",  SQLITE_INTEGER, { .i = ts           } },
        { ":offset",    SQLITE_INTEGER, { .i = offset       } },
        { ":file_sha1", SQLITE_TEXT,    { .s = file_sha1    } },
        { NULL,         SQLITE_NULL,    { .s = NULL         } },
    };

    sqldb_begin(backup->db, "backup_append"); // FIXME what if this fails

    int r = sqldb_exec(backup->db, backup_index_start_sql, bval, NULL, NULL);
    if (r) {
        // FIXME handle this sensibly
        fprintf(stderr, "%s: something went wrong: %i\n", __func__, r);
        sqldb_rollback(backup->db, "backup_append");
        goto error;
    }

    backup->append_state->chunk_id = sqldb_lastid(backup->db);

    backup->append_state->mode |= BACKUP_APPEND_ACTIVE;
    return 0;

error:
    backup->append_state->mode = BACKUP_APPEND_INACTIVE;
    return -1;
}

EXPORTED int backup_append_start(struct backup *backup,
                                 enum backup_append_flush flush)
{
    char file_sha1[2 * SHA1_DIGEST_LENGTH + 1];
    off_t offset = lseek(backup->fd, 0, SEEK_END);

    _sha1_file(backup->fd, backup->data_fname, SHA1_LIMIT_WHOLE_FILE, file_sha1);

    return backup_real_append_start(backup, time(0), offset, file_sha1, 0, flush);
}

EXPORTED int backup_append(struct backup *backup, struct dlist *dlist, time_t ts,
                           enum backup_append_flush flush)
{
    int r;
    if (!backup->append_state || backup->append_state->mode == BACKUP_APPEND_INACTIVE)
        fatal("backup append not started", EC_SOFTWARE);

    off_t start = backup->append_state->wrote;
    size_t len;

    /* build a buffer containing the data to be written */
    struct buf buf = BUF_INITIALIZER, ts_buf = BUF_INITIALIZER;
    dlist_printbuf(dlist, 1, &buf);
    buf_printf(&ts_buf, "%ld APPLY ", (int64_t) ts);
    buf_insert(&buf, 0, &ts_buf);
    buf_appendcstr(&buf, "\r\n");

    /* track the sha1sum */
    SHA1_Update(&backup->append_state->sha_ctx, buf_cstring(&buf), buf_len(&buf));

    /* if we're not in index-only mode, write the data out */
    if (!(backup->append_state->mode & BACKUP_APPEND_INDEXONLY)) {
        /* gzprintf's internal buffer is limited to about 8K, which
         * dlist will exceed if there's a message in it, so use gzwrite
         * rather than gzprintf for writing the dlist contents.
         */
        const char *p = buf_cstring(&buf);
        size_t left = buf_len(&buf);

        while (left) {
            int n = MIN(left, INT32_MAX);
            int wrote = gzwrite(backup->append_state->gzfile, p, n);
            if (wrote > 0) {
                left -= wrote;
                p += wrote;
            }
            else {
                const char *err = gzerror(backup->append_state->gzfile, &r);
                syslog(LOG_ERR, "IOERROR: %s gzwrite %s: %s", __func__, backup->data_fname, err);

                if (r == Z_STREAM_ERROR)
                    fatal("gzwrite: invalid stream", EC_IOERR);
                else if (r == Z_MEM_ERROR)
                    fatal("gzwrite: out of memory", EC_TEMPFAIL);

                goto error;
            }
        }

        if (flush) {
            r = gzflush(backup->append_state->gzfile, Z_FULL_FLUSH);
            if (r != Z_OK) {
                syslog(LOG_ERR, "IOERROR: %s gzflush %s: %i %i", __func__, backup->data_fname, r, errno);
                goto error;
            }
        }
    }

    /* count the written bytes */
    len = buf_len(&buf);
    backup->append_state->wrote += buf_len(&buf);

    buf_free(&buf);

    /* update the index */
    return backup_index(backup, dlist, ts, start, len);

error:
    buf_free(&buf);
    return IMAP_INTERNAL;
}

HIDDEN int backup_real_append_end(struct backup *backup, time_t ts)
{
    int r;

    if (!backup->append_state)
        fatal("backup append not started", EC_SOFTWARE);
    if (backup->append_state->mode == BACKUP_APPEND_INACTIVE)
        fatal("backup append not started", EC_SOFTWARE);

    if (!(backup->append_state->mode & BACKUP_APPEND_INDEXONLY)) {
        r = gzflush(backup->append_state->gzfile, Z_FINISH);
        if (r != Z_OK) {
            fprintf(stderr, "%s: gzflush failed: %i\n", __func__, r);
            // FIXME handle this sensibly
        }
    }

    unsigned char sha1_raw[SHA1_DIGEST_LENGTH];
    char data_sha1[2 * SHA1_DIGEST_LENGTH + 1];
    SHA1_Final(sha1_raw, &backup->append_state->sha_ctx);
    r = bin_to_hex(sha1_raw, SHA1_DIGEST_LENGTH, data_sha1, BH_LOWER);
    assert(r == 2 * SHA1_DIGEST_LENGTH);

    struct sqldb_bindval bval[] = {
        { ":id",        SQLITE_INTEGER, { .i = backup->append_state->chunk_id } },
        { ":ts_end",    SQLITE_INTEGER, { .i = ts                             } },
        { ":length",    SQLITE_INTEGER, { .i = backup->append_state->wrote    } },
        { ":data_sha1", SQLITE_TEXT,    { .s = data_sha1                      } },
        { NULL,         SQLITE_NULL,    { .s = NULL                           } },
    };

    r = sqldb_exec(backup->db, backup_index_end_sql, bval, NULL, NULL);
    if (r) {
        // FIXME handle this sensibly
        fprintf(stderr, "%s: something went wrong: %i\n", __func__, r);
        sqldb_rollback(backup->db, "backup_append");
    }
    else {
        sqldb_commit(backup->db, "backup_append");
    }

    backup->append_state->mode = BACKUP_APPEND_INACTIVE;
    backup->append_state->wrote = 0;

    return r;
}

EXPORTED int backup_append_end(struct backup *backup)
{
    return backup_real_append_end(backup, time(NULL));
}

EXPORTED int backup_append_abort(struct backup *backup)
{
    if (!backup->append_state)
        fatal("backup append not started", EC_SOFTWARE);
    if (backup->append_state == BACKUP_APPEND_INACTIVE)
        fatal("backup append not started", EC_SOFTWARE);

    sqldb_rollback(backup->db, "backup_append");

    // FIXME
    // can we truncate back to the length we started this append at?
    // ftruncate(2) says nothing about behaviour on descriptors
    // opened with O_APPEND...
    // seems like it might work, but test it first.

    // FIXME at least z_finish the damn file...

    backup->append_state->mode = BACKUP_APPEND_INACTIVE;
    return 0;
}
