/*
 * Copyright (C) 2026 The pgmoneta community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Storage-engine integration tests: S3 via Garage.
 *
 * The entire backend lifecycle (start the Garage container, provision it,
 * configure and start a dedicated pgmoneta, tear everything down) is handled
 * by mctf_se. A test only states intent.
 */

#include <mctf.h>
#include <mctf_container.h>
#include <mctf_se.h>
#include <tscommon.h>

#include <stdlib.h>

static int storage_status = MCTF_FAIL;

MCTF_MODULE_SETUP(s3)
{
   storage_status = mctf_se_up(MCTF_BACKEND_GARAGE);
}

MCTF_MODULE_TEARDOWN(s3)
{
   mctf_se_down();
}

/*
 * Backup to S3 succeeds and the local catalog records it. Local metadata
 * must always be present and authoritative, even with a remote-only engine.
 */
MCTF_INTEGRATION_TEST(test_s3_backup_keeps_local_metadata)
{
   char* listing = NULL;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_backup("primary") == 0, cleanup, "backup to S3 failed");

   MCTF_ASSERT(mctf_se_has_local_metadata("primary"), cleanup,
               "local metadata missing after S3 backup");

   MCTF_ASSERT(mctf_se_list_backup("primary", &listing) == 0, cleanup, "list-backup failed");
   MCTF_ASSERT_PTR_NONNULL(listing, cleanup, "empty list-backup response");

cleanup:
   free(listing);
   MCTF_FINISH();
}

/*
 * Full round trip: back up to S3, then restore FROM S3 and verify the data
 * directory was genuinely reconstructed (not just an exit-0 with no files).
 */
MCTF_INTEGRATION_TEST(test_s3_backup_restore_roundtrip)
{
   char* out = NULL;
   char label[256] = {0};
   char cmd[2 * MAX_PATH];

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_backup("primary") == 0, cleanup, "backup to S3 failed");

   /* resolve the backup label from the local catalog */
   mctf_sh(&out, "ls %s/backup/primary/backup | head -1 | tr -d '\\n'", mctf_se_run_dir());
   MCTF_ASSERT_PTR_NONNULL(out, cleanup, "could not read backup label");
   snprintf(label, sizeof(label), "%s", out);
   free(out);
   out = NULL;
   MCTF_ASSERT(label[0] != '\0', cleanup, "empty backup label");

   /* download from S3 */
   snprintf(cmd, sizeof(cmd), "s3 restore primary %s %s", label, TEST_RESTORE_DIR);
   MCTF_ASSERT(mctf_se_cli(cmd, NULL) == 0, cleanup, "s3 restore failed");

   /* verify the cluster actually came back (guards against a false-positive
    * restore that returns 0 but writes nothing) */
   mctf_sh(&out, "find %s -name PG_VERSION | head -1 | tr -d '\\n'", TEST_RESTORE_DIR);
   MCTF_ASSERT_PTR_NONNULL(out, cleanup, "restore produced no output");
   MCTF_ASSERT(out[0] != '\0', cleanup, "restore produced no PG_VERSION (empty restore)");

cleanup:
   free(out);
   MCTF_FINISH();
}
