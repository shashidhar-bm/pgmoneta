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
 * Azurite backend driver.
 *
 * Azurite is a local Azure Blob Storage emulator; it stands in for Azure so
 * the real se_azure code path is exercised. The container lifecycle is handled
 * by MCTF_START_CONTAINER; this driver adds the provisioning step (create the
 * blob container) and the server configuration lines.
 *
 * Azurite well-known credentials (public, for local development only):
 *   Account : devstoreaccount1
 *   Key     : Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/
 *             K1SZFPTOtr/KBHBeksoGMGw==
 *
 * Container creation uses the SharedKey REST API via a Python3 stdlib script
 * (no external SDK required) because Azurite has no in-container CLI for this.
 */

#include <pgmoneta.h>
#include <logging.h>
#include <mctf_container.h>
#include <mctf_se.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AZURITE_ACCOUNT   "devstoreaccount1"
#define AZURITE_KEY       "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw=="
#define AZURITE_CONTAINER "pgmoneta-test-container"
#define AZURITE_BLOB_PORT 10000

/*
 * Create the blob container in Azurite using SharedKey authentication.
 * Python3 stdlib only — no external packages needed.
 *
 * Azurite uses path-style URLs (/<account>/<container>) instead of the
 * host-style URLs used by production Azure.
 *
 * Container creation uses the azure-storage-blob SDK to avoid re-implementing
 * the per-version quirks of the SharedKey canonical string; the SDK handles
 * those transparently. Install it with: pip3 install azure-storage-blob
 */
static int
provision(struct mctf_se* s)
{
   char script_path[256];
   FILE* f;
   int rc;

   snprintf(script_path, sizeof(script_path), "/tmp/mctf-azurite-provision-%d.py", (int)getpid());
   f = fopen(script_path, "w");
   if (f == NULL)
   {
      pgmoneta_log_error("azurite: cannot write provisioning script");
      return MCTF_FAIL;
   }

   fprintf(f,
      "import sys\n"
      "try:\n"
      "    from azure.storage.blob import BlobServiceClient\n"
      "except ImportError:\n"
      "    print('azure-storage-blob not installed; run: pip3 install azure-storage-blob', file=sys.stderr)\n"
      "    sys.exit(1)\n"
      "\n"
      "ACCOUNT   = '%s'\n"
      "KEY       = '%s'\n"
      "CONTAINER = '%s'\n"
      "PORT      = %d\n"
      "\n"
      "conn_str = (\n"
      "    'DefaultEndpointsProtocol=http;'\n"
      "    f'AccountName={ACCOUNT};'\n"
      "    f'AccountKey={KEY};'\n"
      "    f'BlobEndpoint=http://127.0.0.1:{PORT}/{ACCOUNT};'\n"
      ")\n"
      "client = BlobServiceClient.from_connection_string(conn_str)\n"
      "try:\n"
      "    client.create_container(CONTAINER)\n"
      "    print(f'container {CONTAINER!r} created')\n"
      "except Exception as e:\n"
      "    if 'ContainerAlreadyExists' in str(e):\n"
      "        print(f'container {CONTAINER!r} already exists')\n"
      "    else:\n"
      "        print(f'error: {e}', file=sys.stderr)\n"
      "        sys.exit(1)\n",
      AZURITE_ACCOUNT, AZURITE_KEY, AZURITE_CONTAINER, AZURITE_BLOB_PORT);

   fclose(f);

   rc = mctf_sh(NULL, "python3 %s", script_path);
   unlink(script_path);

   return (rc == 0) ? MCTF_OK : MCTF_FAIL;
}

static int
azurite_start(struct mctf_se* s)
{
   int rc;

   rc = MCTF_START_CONTAINER(&s->container, MCTF_CONTAINER_AZURITE);
   if (rc != MCTF_OK)
   {
      return rc;
   }
   fprintf(stderr, "    - container started\n");
   fflush(stderr);

   if (provision(s) != MCTF_OK)
   {
      return MCTF_FAIL;
   }
   fprintf(stderr, "    - blob container provisioned\n");
   fflush(stderr);

   snprintf(s->endpoint,   sizeof(s->endpoint),   "127.0.0.1");
   snprintf(s->access_key, sizeof(s->access_key),  "%s", AZURITE_ACCOUNT);
   snprintf(s->secret_key, sizeof(s->secret_key),  "%s", AZURITE_KEY);
   snprintf(s->bucket,     sizeof(s->bucket),      "%s", AZURITE_CONTAINER);
   s->port    = AZURITE_BLOB_PORT;
   s->use_tls = false;

   return MCTF_OK;
}

static void
azurite_stop(struct mctf_se* s)
{
   MCTF_STOP_CONTAINER(&s->container);
}

static int
azurite_write_global_conf(struct mctf_se* s, FILE* f)
{
   /* Azure settings are global (main_configuration), not per-server. */
   fprintf(f, "azure_storage_account = %s\n", s->access_key);
   fprintf(f, "azure_shared_key = %s\n",      s->secret_key);
   fprintf(f, "azure_container = %s\n",        s->bucket);
   fprintf(f, "azure_base_dir = pgmoneta\n");
   fprintf(f, "azure_endpoint = %s\n",         s->endpoint);
   fprintf(f, "azure_port = %d\n",             s->port);
   fprintf(f, "azure_use_tls = %s\n",          s->use_tls ? "on" : "off");
   return MCTF_OK;
}

const struct mctf_se_driver mctf_azurite_driver = {
   .name              = "azurite",
   .storage_engine    = "azure",
   .start             = azurite_start,
   .stop              = azurite_stop,
   .write_global_conf = azurite_write_global_conf,
   .write_server_conf = NULL,
};
