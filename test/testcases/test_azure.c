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
 * Storage-engine integration tests: Azure Blob Storage via Azurite.
 *
 * The entire backend lifecycle (start the Azurite container, provision the
 * blob container, configure and start a dedicated pgmoneta, tear everything
 * down) is handled by mctf_se. A test only states intent.
 *
 * Azure has no "azure ls" CLI command (unlike S3), so blob-side verification
 * uses the local catalog and local filesystem checks:
 *   - local metadata (backup.info) is always written regardless of storage engine
 *   - STATUS=1 in backup.info confirms the upload completed successfully
 *   - the local data/ subdirectory is removed by azure_storage_teardown when
 *     STORAGE_ENGINE_LOCAL is not set — this proves the teardown ran
 */

#include <mctf.h>
#include <mctf_container.h>
#include <mctf_se.h>
#include <tscommon.h>
#include <utils.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int storage_status = MCTF_FAIL;
static char shared_label[256];

/* Return the lexicographically largest (newest) backup label for primary. */
static int
newest_backup_label(char* out, size_t size)
{
   char backup_dir[MAX_PATH];
   char** dirs = NULL;
   int ndir = 0;
   int best = -1;

   snprintf(backup_dir, sizeof(backup_dir), "%s/backup/primary/backup", mctf_se_run_dir());
   pgmoneta_get_directories(backup_dir, &ndir, &dirs);
   if (ndir <= 0 || dirs == NULL)
      return MCTF_FAIL;

   for (int i = 0; i < ndir; i++)
   {
      if (best < 0 || strcmp(dirs[i], dirs[best]) > 0)
         best = i;
   }
   snprintf(out, size, "%s", dirs[best]);

   for (int i = 0; i < ndir; i++)
      free(dirs[i]);
   free(dirs);

   return out[0] != '\0' ? MCTF_OK : MCTF_FAIL;
}

MCTF_MODULE_SETUP(azure)
{
   memset(shared_label, 0, sizeof(shared_label));
   storage_status = mctf_se_up(MCTF_BACKEND_AZURITE);
   if (storage_status == MCTF_OK)
   {
      if (mctf_se_backup("primary") != 0 ||
          newest_backup_label(shared_label, sizeof(shared_label)) != MCTF_OK)
      {
         storage_status = MCTF_FAIL;
      }
   }
}

MCTF_MODULE_TEARDOWN(azure)
{
   mctf_se_down();
}

/*
 * Backup to Azure succeeds and the local catalog records it. Local metadata
 * must always be present and authoritative, even with a remote-only engine.
 */
MCTF_INTEGRATION_TEST(test_azure_backup_keeps_local_metadata)
{
   char* listing = NULL;

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_has_local_metadata("primary"), cleanup,
               "local metadata missing after Azure backup");

   MCTF_ASSERT(mctf_se_list_backup("primary", &listing) == 0, cleanup, "list-backup failed");
   MCTF_ASSERT_PTR_NONNULL(listing, cleanup, "empty list-backup response");

cleanup:
   free(listing);
   MCTF_FINISH();
}

/*
 * After a successful Azure upload, backup.info must contain STATUS=1.
 * This is the authoritative signal that the engine completed the transfer.
 */
MCTF_INTEGRATION_TEST(test_azure_backup_info_is_valid)
{
   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");
   MCTF_ASSERT(shared_label[0] != '\0', cleanup, "no backup label available");

   MCTF_ASSERT(
      mctf_sh(NULL, "grep -q 'STATUS=1' %s/backup/primary/backup/%s/backup.info",
              mctf_se_run_dir(), shared_label) == 0,
      cleanup, "backup.info does not contain STATUS=1 (upload may have failed)");

cleanup:
   MCTF_FINISH();
}

/*
 * azure_storage_teardown must remove the local data/ subdirectory after a
 * successful upload when STORAGE_ENGINE_LOCAL is not set. Leaving it behind
 * would waste disk and break the azure-only contract.
 */
MCTF_INTEGRATION_TEST(test_azure_backup_removes_local_data)
{
   char data_path[MAX_PATH];

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");
   MCTF_ASSERT(shared_label[0] != '\0', cleanup, "no backup label available");

   snprintf(data_path, sizeof(data_path), "%s/backup/primary/backup/%s/data",
            mctf_se_run_dir(), shared_label);

   MCTF_ASSERT(access(data_path, F_OK) != 0, cleanup,
               "local data/ directory still exists after azure-only backup (teardown failed)");

cleanup:
   MCTF_FINISH();
}

/*
 * A second backup must also succeed. This guards against WAL slot or
 * connection state issues that only manifest on repeated runs.
 */
MCTF_INTEGRATION_TEST(test_azure_second_backup_succeeds)
{
   char label2[256] = {0};

   if (storage_status == MCTF_SKIPPED)
   {
      MCTF_SKIP("no container engine / test environment");
   }
   MCTF_ASSERT(storage_status == MCTF_OK, cleanup, "storage backend setup failed");

   MCTF_ASSERT(mctf_se_backup("primary") == 0, cleanup, "second backup to Azure failed");
   MCTF_ASSERT(newest_backup_label(label2, sizeof(label2)) == MCTF_OK, cleanup,
               "could not resolve second backup label");

   /* The new label must be different from (and lexicographically after) the first. */
   MCTF_ASSERT(strcmp(label2, shared_label) > 0, cleanup,
               "second backup label is not newer than first");

   MCTF_ASSERT(
      mctf_sh(NULL, "grep -q 'STATUS=1' %s/backup/primary/backup/%s/backup.info",
              mctf_se_run_dir(), label2) == 0,
      cleanup, "second backup.info does not contain STATUS=1");

cleanup:
   MCTF_FINISH();
}
