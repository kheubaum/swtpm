/*
 * tpm_funcs.c -- interface with libtpms
 *
 * (c) Copyright IBM Corporation 2015.
 *
 * Author: Stefan Berger <stefanb@us.ibm.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the names of the IBM Corporation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "sys_dependencies.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include <libtpms/tpm_library.h>
#include <libtpms/tpm_error.h>
#include <libtpms/tpm_nvfilename.h>
#include <libtpms/tpm_memory.h>

#include <json-glib/json-glib.h>

#include "tpmlib.h"
#include "logging.h"
#include "tpm_ioctl.h"
#include "swtpm_nvstore.h"
#include "locality.h"
#ifdef WITH_VTPM_PROXY
#include "vtpm_proxy.h"
#endif
#include "utils.h"
#include "compiler_dependencies.h"
#include "swtpm_utils.h"
#include "fips.h"
#include "check_algos.h"
#include "tpmstate.h"

/*
 * convert the blobtype integer into a string that libtpms
 * understands
 */
const char *tpmlib_get_blobname(uint32_t blobtype)
{
    switch (blobtype) {
    case PTM_BLOB_TYPE_PERMANENT:
        return TPM_PERMANENT_ALL_NAME;
    case PTM_BLOB_TYPE_VOLATILE:
        return TPM_VOLATILESTATE_NAME;
    case PTM_BLOB_TYPE_SAVESTATE:
        return TPM_SAVESTATE_NAME;
    default:
        return NULL;
    }
}

TPM_RESULT tpmlib_register_callbacks(struct libtpms_callbacks *cbs)
{
    TPM_RESULT res;

    if ((res = TPMLIB_RegisterCallbacks(cbs)) != TPM_SUCCESS) {
        logprintf(STDERR_FILENO,
                  "Error: Could not register the callbacks.\n");
    }
    return res;
}

TPM_RESULT tpmlib_choose_tpm_version(TPMLIB_TPMVersion tpmversion)
{
    TPM_RESULT res;

    if ((res = TPMLIB_ChooseTPMVersion(tpmversion)) != TPM_SUCCESS) {
        const char *version = "TPM 1.2";

        if (tpmversion == TPMLIB_TPM_VERSION_2)
            version = "TPM 2";
        logprintf(STDERR_FILENO,
                  "Error: %s is not supported by libtpms.\n", version);
    }
    return res;
}

static int tpmlib_check_disabled_algorithms(unsigned int *fix_flags,
                                            unsigned int disabled_filter,
                                            bool stop_on_first_disabled)
{
    char *info_data = TPMLIB_GetInfo(TPMLIB_INFO_RUNTIME_ALGORITHMS);
    g_autofree gchar *enabled = NULL;
    gchar **algorithms;
    int ret;

    *fix_flags = 0;

    ret = json_get_submap_value(info_data, "RuntimeAlgorithms", "Enabled",
                                &enabled);
    if (ret)
        goto error;

    algorithms = g_strsplit(enabled, ",", -1);

    *fix_flags = check_ossl_algorithms_are_disabled((const gchar * const *)algorithms,
                                                    disabled_filter,
                                                    stop_on_first_disabled);

    g_strfreev(algorithms);
error:
    free(info_data);

    return ret;
}

/*
 * Check the enabled RuntimeAttributes whether either fips-host or
 * the pair no-sha1-signing & no-sha1-verification are enabled since these
 * disable sha1 signature support by the TPM 2. Return '1' if this is the
 * case, '0' otherwise.
 */
static int tpmlib_check_attributes_disable_sha1_signatures(void)
{
    char *info_data = TPMLIB_GetInfo(TPMLIB_INFO_RUNTIME_ATTRIBUTES);
    g_autofree gchar *enabled = NULL;
    g_auto(GStrv) attributes = NULL;
    int ret;

    ret = json_get_submap_value(info_data, "RuntimeAttributes", "Enabled",
                                &enabled);
    if (ret) {
        ret = 0;
        goto error;
    }

    attributes = g_strsplit(enabled, ",", -1);

    ret = 0;
    if (strv_contains_all((const gchar *const*)attributes,
                          (const char*[]){"fips-host", NULL}) ||
        strv_contains_all((const gchar *const*)attributes,
                          (const char*[]){"no-sha1-signing", "no-sha1-verification", NULL}))
        ret = 1;

error:
    free(info_data);

    return ret;
}

/*
 * This function only applies to TPM2: If FIPS mode was enabled on the host,
 * determine whether OpenSSL needs to deactivate FIPS mode (FIX_DISABLE_FIPS is
 * set). It doesn't need to deactivate it if a profile was chosen that has no
 * algorithms that FIPS deactivates, otherwise it has to deactivate FIPS mode in
 * the OpenSSL instance being used.
 */
static int tpmlib_check_need_disable_fips_mode(unsigned int *fix_flags)
{
     return tpmlib_check_disabled_algorithms(fix_flags,
                                             DISABLED_BY_FIPS,
                                             true);
}

/* Check whether SHA1 signatures need to be enabled. */
static int tpmlib_check_need_enable_sha1_signatures(unsigned int *fix_flags)
{
    *fix_flags = 0;

    if (tpmlib_check_attributes_disable_sha1_signatures())
        return 0;
    return tpmlib_check_disabled_algorithms(fix_flags,
                                            DISABLED_SHA1_SIGNATURES,
                                            true);
}

/*
 * Check whether swtpm would have to be started with a modified config so that
 * libtpms can use the algorithms given by its profile.
 * Again also check the functioning of SHA1 signatures since this is enabled
 * with an OpenSSL patch and we want to detect now that
 * OPENSSL_ENABLE_SHA1_SIGNATURES=1 has the desired effect.
 */
static int tpmlib_check_need_modify_ossl_config(unsigned int *fix_flags,
                                                bool check_sha1_signatures)
{
     unsigned int disabled_filter = DISABLED_BY_CONFIG;

     if (check_sha1_signatures)
         disabled_filter |= DISABLED_SHA1_SIGNATURES;
     return tpmlib_check_disabled_algorithms(fix_flags,
                                             disabled_filter,
                                             false);
}

/* Determine wheter FIPS mode is enabled in the crypto library. If FIPS mode is
 * enabled check whether any of the algorithms that the TPM 2 uses would need
 * OpenSSL FIPS mode to be disabled for the TPM 2 to work and then try to disable
 * it.
 * Check whether signing and/or verifying SHA1 signatures is prevented and to
 * work around it set OPENSSL_ENABLE_SHA1_SIGNATURES=1.
 */
static int tpmlib_maybe_configure_openssl(TPMLIB_TPMVersion tpmversion)
{
    bool check_sha1_signatures = false;
    unsigned int fix_flags = 0;
    int ret;

    if (fips_mode_enabled()) {
        switch (tpmversion) {
        case TPMLIB_TPM_VERSION_1_2:
            fix_flags = FIX_DISABLE_FIPS;
            break;
        case TPMLIB_TPM_VERSION_2:
            ret = tpmlib_check_need_disable_fips_mode(&fix_flags);
            if (ret)
                return -1;
            break;
        }
        if ((fix_flags & FIX_DISABLE_FIPS) && fips_mode_disable())
            return -1;
    }

    if (tpmversion == TPMLIB_TPM_VERSION_2) {
        ret = tpmlib_check_need_enable_sha1_signatures(&fix_flags);
        if (ret)
            return -1;
        if (fix_flags & FIX_ENABLE_SHA1_SIGNATURES) {
            if (!g_setenv("OPENSSL_ENABLE_SHA1_SIGNATURES", "1", true)) {
                logprintf(STDERR_FILENO,
                          "Error: Could not set OPENSSL_ENABLE_SHA1_SIGNATURES=1\n");
                return -1;
            }
            logprintf(STDOUT_FILENO,
                      "Warning: Setting OPENSSL_ENABLE_SHA1_SIGNATURES=1\n");
            check_sha1_signatures = true;
        }

        ret = tpmlib_check_need_modify_ossl_config(&fix_flags, check_sha1_signatures);
        if (ret)
            return -1;
        if (fix_flags) {
            logprintf(STDERR_FILENO,
                     "Error: Need to start with modified OpenSSL config file to enable all needed algorithms.\n");
            return -1;
        }
    }

    return 0;
}

TPM_RESULT tpmlib_start(uint32_t flags, TPMLIB_TPMVersion tpmversion,
                        bool lock_nvram, const char *json_profile)
{
    TPM_RESULT res;

    if ((res = tpmlib_choose_tpm_version(tpmversion)) != TPM_SUCCESS)
        return res;

    if (json_profile != NULL && tpmversion == TPMLIB_TPM_VERSION_2 &&
        (res = TPMLIB_SetProfile(json_profile)) != TPM_SUCCESS) {
        logprintf(STDERR_FILENO,
                  "Error: Could not set profile for TPM2: '%s'\n",
                  json_profile);
        return res;
    }

    if ((res = TPMLIB_MainInit()) != TPM_SUCCESS) {
        /* if wanted, try to restore the permanent state backup */
        if (tpmstate_get_make_backup() &&
            (res = SWTPM_NVRAM_RestoreBackup()) == TPM_SUCCESS) {

            logprintf(STDOUT_FILENO,
                      "Attempting to start with backup state file.\n");
            res = TPMLIB_MainInit();

            if (res != TPM_SUCCESS) {
                /* 2nd call to RestoreBackup reverts file renamings */
                SWTPM_NVRAM_RestoreBackup();
            }
        }

        if (res != TPM_SUCCESS) {
            logprintf(STDERR_FILENO,
                      "Error: Could not initialize libtpms.\n");
            return res;
        }
    }

    if (json_profile != NULL && tpmversion == TPMLIB_TPM_VERSION_2 &&
        !TPMLIB_WasManufactured()) {
        logprintf(STDERR_FILENO,
                  "Error: Profile could not be applied to an existing TPM 2 instance.\n");
        return TPM_FAIL;
    }

    if (lock_nvram && (res = SWTPM_NVRAM_Lock_Storage(0)) != TPM_SUCCESS)
        goto error_terminate;

    if (flags & PTM_INIT_FLAG_DELETE_VOLATILE) {
        uint32_t tpm_number = 0;
        const char *name = TPM_VOLATILESTATE_NAME;
        res = SWTPM_NVRAM_DeleteName(tpm_number,
                                     name,
                                     FALSE);
        if (res != TPM_SUCCESS) {
            logprintf(STDERR_FILENO,
                      "Error: Could not delete the volatile "
                      "state of the TPM.\n");
            goto error_terminate;
        }
    }

    if (tpmlib_maybe_configure_openssl(tpmversion)) {
        res = TPM_FAIL;
        goto error_terminate;
    }

    return TPM_SUCCESS;

error_terminate:
    TPMLIB_Terminate();

    return res;
}

int tpmlib_get_tpm_property(enum TPMLIB_TPMProperty prop)
{
    int result = 0;
    TPM_RESULT res;

    res = TPMLIB_GetTPMProperty(prop, &result);

    assert(res == TPM_SUCCESS);

    return result;
}

uint32_t tpmlib_get_cmd_ordinal(const unsigned char *request, size_t req_len)
{
    struct tpm_req_header *hdr;

    if (req_len < sizeof(struct tpm_req_header))
        return TPM_ORDINAL_NONE;

    hdr = (struct tpm_req_header *)request;
    return be32toh(hdr->ordinal);
}

bool tpmlib_is_request_cancelable(TPMLIB_TPMVersion tpmversion,
                                  const unsigned char *request, size_t req_len)
{

    uint32_t ordinal = tpmlib_get_cmd_ordinal(request, req_len);

    if (ordinal == TPM_ORDINAL_NONE)
        return false;

    if (tpmversion == TPMLIB_TPM_VERSION_2)
        return (ordinal == TPMLIB_TPM2_CC_CreatePrimary ||
                ordinal == TPMLIB_TPM2_CC_Create);

    return (ordinal == TPMLIB_TPM_ORD_TakeOwnership ||
            ordinal == TPMLIB_TPM_ORD_CreateWrapKey);
}

static void tpmlib_write_error_response(unsigned char **rbuffer,
                                        uint32_t *rlength,
                                        uint32_t *rTotal,
                                        TPM_RESULT errcode,
                                        TPMLIB_TPMVersion tpmversion)
{
    struct tpm_resp_header errresp = {
        .tag = (tpmversion == TPMLIB_TPM_VERSION_2)
               ? htobe16(0x8001)
               : htobe16(0xc4),
        .size = htobe32(sizeof(errresp)),
        .errcode = htobe32(errcode),
    };

    if (*rbuffer == NULL ||
        *rTotal < sizeof(errresp)) {
        free(*rbuffer);

        *rTotal = sizeof(errresp);
        *rbuffer = malloc(*rTotal);
        if (*rbuffer == NULL)
            *rTotal = 0;
    }
    if (*rbuffer) {
        *rlength = sizeof(errresp);
        memcpy(*rbuffer, &errresp, sizeof(errresp));
    }
}

void tpmlib_write_fatal_error_response(unsigned char **rbuffer,
                                       uint32_t *rlength,
                                       uint32_t *rTotal,
                                       TPMLIB_TPMVersion tpmversion)
{
    TPM_RESULT errcode = (tpmversion == TPMLIB_TPM_VERSION_2)
                         ? TPM_RC_FAILURE
                         : TPM_FAIL;

    tpmlib_write_error_response(rbuffer, rlength, rTotal, errcode,
                                tpmversion);
}

void tpmlib_write_locality_error_response(unsigned char **rbuffer,
                                          uint32_t *rlength,
                                          uint32_t *rTotal,
                                          TPMLIB_TPMVersion tpmversion)
{
    TPM_RESULT errcode = (tpmversion == TPMLIB_TPM_VERSION_2)
                         ? TPM_RC_LOCALITY
                         : TPM_BAD_LOCALITY;

    tpmlib_write_error_response(rbuffer, rlength, rTotal, errcode,
                                tpmversion);
}

void tpmlib_write_success_response(unsigned char **rbuffer,
                                   uint32_t *rlength,
                                   uint32_t *rTotal,
                                   TPMLIB_TPMVersion tpmversion)
{
    tpmlib_write_error_response(rbuffer, rlength, rTotal, 0,
                                tpmversion);
}

#ifdef WITH_VTPM_PROXY
static void tpmlib_write_shortmsg_error_response(unsigned char **rbuffer,
                                                 uint32_t *rlength,
                                                 uint32_t *rTotal,
                                                 TPMLIB_TPMVersion tpmversion)
{
    TPM_RESULT errcode = (tpmversion == TPMLIB_TPM_VERSION_2)
                         ? TPM_RC_INSUFFICIENT
                         : TPM_BAD_PARAM_SIZE;

    tpmlib_write_error_response(rbuffer, rlength, rTotal, errcode,
                                tpmversion);
}

static TPM_RESULT tpmlib_process_setlocality(unsigned char **rbuffer,
                                             uint32_t *rlength,
                                             uint32_t *rTotal,
                                             unsigned char *command,
                                             uint32_t command_length,
                                             TPMLIB_TPMVersion tpmversion,
                                             uint32_t locality_flags,
                                             TPM_MODIFIER_INDICATOR *locality)
{
    TPM_MODIFIER_INDICATOR new_locality;

    if (command_length >= sizeof(struct tpm_req_header) + sizeof(char)) {
        if (!(locality_flags & LOCALITY_FLAG_ALLOW_SETLOCALITY)) {
            /* SETLOCALITY command is not allowed */
            tpmlib_write_fatal_error_response(rbuffer,
                                              rlength, rTotal,
                                              tpmversion);
        } else {
            new_locality = command[sizeof(struct tpm_req_header)];
            if (new_locality >=5 ||
                (new_locality == 4 &&
                 locality_flags & LOCALITY_FLAG_REJECT_LOCALITY_4)) {
                tpmlib_write_locality_error_response(rbuffer,
                                                     rlength, rTotal,
                                                    tpmversion);
            } else {
                tpmlib_write_success_response(rbuffer,
                                              rlength, rTotal,
                                              tpmversion);
                *locality = new_locality;
            }
        }
    } else {
        tpmlib_write_shortmsg_error_response(rbuffer,
                                             rlength, rTotal,
                                             tpmversion);
    }
    return TPM_SUCCESS;
}

TPM_RESULT tpmlib_process(unsigned char **rbuffer,
                          uint32_t *rlength,
                          uint32_t *rTotal,
                          unsigned char *command,
                          uint32_t command_length,
                          uint32_t locality_flags,
                          TPM_MODIFIER_INDICATOR *locality,
                          TPMLIB_TPMVersion tpmversion)
{
    /* process those commands we need to handle, e.g. SetLocality */
    struct tpm_req_header *req = (struct tpm_req_header *)command;
    uint32_t ordinal;

    if (command_length < sizeof(*req)) {
        tpmlib_write_shortmsg_error_response(rbuffer,
                                             rlength, rTotal,
                                             tpmversion);
        return TPM_SUCCESS;
    }

    ordinal = be32toh(req->ordinal);

    switch (tpmversion) {
    case TPMLIB_TPM_VERSION_1_2:
        switch (ordinal) {
        case TPM_CC_SET_LOCALITY:
            return tpmlib_process_setlocality(rbuffer, rlength, rTotal,
                                              command, command_length,
                                              tpmversion, locality_flags,
                                              locality);
        }
        break;

    case TPMLIB_TPM_VERSION_2:
        switch (ordinal) {
        case TPM2_CC_SET_LOCALITY:
            return tpmlib_process_setlocality(rbuffer, rlength, rTotal,
                                              command, command_length,
                                              tpmversion, locality_flags,
                                              locality);
        }
        break;
    }
    return TPM_SUCCESS;
}

#else

TPM_RESULT tpmlib_process(unsigned char **rbuffer SWTPM_ATTR_UNUSED,
                          uint32_t *rlength SWTPM_ATTR_UNUSED,
                          uint32_t *rTotal SWTPM_ATTR_UNUSED,
                          unsigned char *command SWTPM_ATTR_UNUSED,
                          uint32_t command_length SWTPM_ATTR_UNUSED,
                          uint32_t locality_flags SWTPM_ATTR_UNUSED,
                          TPM_MODIFIER_INDICATOR *locality SWTPM_ATTR_UNUSED,
                          TPMLIB_TPMVersion tpmversion SWTPM_ATTR_UNUSED)
{
    return TPM_SUCCESS;
}

#endif /* WITH_VTPM_PROXY */

enum TPMLIB_StateType tpmlib_blobtype_to_statetype(uint32_t blobtype)
{
    switch (blobtype) {
    case PTM_BLOB_TYPE_PERMANENT:
        return TPMLIB_STATE_PERMANENT;
    case PTM_BLOB_TYPE_VOLATILE:
        return TPMLIB_STATE_VOLATILE;
    case PTM_BLOB_TYPE_SAVESTATE:
        return TPMLIB_STATE_SAVE_STATE;
    }
    return 0;
}

/*
 * tpmlib_handle_tcg_tpm2_cmd_header
 *
 * Determine whether the given byte stream is a raw TPM 2 command or
 * whether it has a tcg_tpm2_cmd_header prefixed and if so return
 * the offset after the header where the actual command is. In all
 * other cases return 0.
 */
off_t tpmlib_handle_tcg_tpm2_cmd_header(const unsigned char *command,
                                        uint32_t command_length,
                                        TPM_MODIFIER_INDICATOR *locality)
{
    struct tpm_req_header *hdr = (struct tpm_req_header *)command;
    struct tpm2_send_command_prefix *tcgprefix;
    off_t ret = 0;

    /* return 0 for short packets or plain TPM 2 command */
    if (command_length < sizeof(*hdr) ||
        be16toh(hdr->tag) == TPM2_ST_NO_SESSION ||
        be16toh(hdr->tag) == TPM2_ST_SESSIONS ||
        command_length < sizeof(*tcgprefix))
        return 0;

    tcgprefix = (struct tpm2_send_command_prefix *)command;
    if (be32toh(tcgprefix->cmd) == TPM2_SEND_COMMAND) {
        ret = sizeof(*tcgprefix);
        *locality = tcgprefix->locality;
    }

    return ret;
}

/*
 * Create a Startup command with the given startupType for the
 * given TPM version.
 */
uint32_t tpmlib_create_startup_cmd(uint16_t startupType,
                                   TPMLIB_TPMVersion tpmversion,
                                   unsigned char *buffer,
                                   uint32_t buffersize)
{
    struct tpm_startup ts;
    uint32_t tocopy = min(sizeof(ts), buffersize);

    ts.hdr.size = htobe32(sizeof(ts));

    switch (tpmversion) {
    case TPMLIB_TPM_VERSION_1_2:
        ts.hdr.tag = htobe16(TPM_TAG_RQU_COMMAND);
        ts.hdr.ordinal = htobe32(TPMLIB_TPM_ORD_Startup);
        ts.startupType = htobe16(startupType);
        break;
    case TPMLIB_TPM_VERSION_2:
        ts.hdr.tag = htobe16(TPM2_ST_NO_SESSION);
        ts.hdr.ordinal = htobe32(TPMLIB_TPM2_CC_Startup);
        switch (startupType) {
        case TPM_ST_CLEAR:
            ts.startupType = htobe16(TPM2_SU_CLEAR);
            break;
        case TPM_ST_STATE:
            ts.startupType = htobe16(TPM2_SU_STATE);
            break;
        case TPM_ST_DEACTIVATED:
            tocopy = 0;
            logprintf(STDERR_FILENO,
                      "TPM 2 does not support startup deactivated.\n");
            break;
        default:
            tocopy = 0;
            logprintf(STDERR_FILENO,
                      "%s: internal error; unsupported startup type for TPM 2\n", __func__);
            break;
        }
        break;
    default:
        tocopy = 0;
        logprintf(STDERR_FILENO,
                  "%s: internal error; invalid TPM version\n", __func__);
        break;
    }

    if (tocopy)
        memcpy(buffer, &ts, tocopy);
    return tocopy;
}

/*
 * tpmlib_maybe_send_tpm2_shutdown: Send a TPM2_Shutdown() if necesssary
 *
 * @tpmversion: version of TPM
 * @lastCommand: last command that was sent to the TPM 2 before
 *               this will be updated if TPM2_Shutdown was sent
 *
 * Send a TPM2_Shutdown(SU_STATE) to a TPM 2 if the last-processed command was
 * not TPM2_Shutdown. If the command fails, send TPM2_Shutdown(SU_CLEAR).
 */
void tpmlib_maybe_send_tpm2_shutdown(TPMLIB_TPMVersion tpmversion,
                                     uint32_t *lastCommand)
{
    TPM_RESULT res;
    unsigned char *rbuffer = NULL;
    uint32_t rlength = 0;
    uint32_t rTotal = 0;
    struct tpm2_shutdown {
        struct tpm_req_header hdr;
        uint16_t shutdownType;
    } tpm2_shutdown = {
        .hdr = {
            .tag = htobe16(TPM2_ST_NO_SESSION),
            .size = htobe32(sizeof(tpm2_shutdown)),
            .ordinal = htobe32(TPMLIB_TPM2_CC_Shutdown),
        },
    };
    uint16_t shutdownTypes[2] = {
        TPM2_SU_STATE, TPM2_SU_CLEAR,
    };
    struct tpm_resp_header *rsp;
    uint32_t errcode;
    size_t i;

    /* Only send TPM2_Shutdown for a TPM 2 and only if TPM2_Shutdown()
     * was not already sent. Send a TPM2_Shutdown(SU_STATE) first since
     * this is preserves additional state that will not matter if the
     * VM starts with TPM2_Startup(SU_CLEAR). Only if this command fails
     * send TPM2_Shutdown(SU_CLEAR).
     */
    if (tpmversion != TPMLIB_TPM_VERSION_2 ||
        *lastCommand == TPMLIB_TPM2_CC_Shutdown)
        return;

    for (i = 0; i < ARRAY_LEN(shutdownTypes); i++) {
#if 0
        logprintf(STDERR_FILENO,
                  "Need to send TPM2_Shutdown(%s); previous command: 0x%08x\n",
                  shutdownTypes[i] == TPM2_SU_STATE ? "SU_STATE" : "SU_CLEAR",
                  *lastCommand);
#endif
        tpm2_shutdown.shutdownType = htobe16(shutdownTypes[i]);

        res = TPMLIB_Process(&rbuffer, &rlength, &rTotal,
                             (unsigned char *)&tpm2_shutdown,
                             sizeof(tpm2_shutdown));

        if (res || rlength < sizeof(struct tpm_resp_header))
            continue;
        rsp = (struct tpm_resp_header *)rbuffer;

        errcode = be32toh(rsp->errcode);
        if (errcode == TPM_SUCCESS) {
            *lastCommand = TPMLIB_TPM2_CC_Shutdown;
            break;
        } else if (errcode == TPM_RC_INITIALIZE) {
            /* TPM not initialized by TPM2_Startup - won't work */
            break;
        }
    }
    free(rbuffer);
}
