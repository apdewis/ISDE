#define _POSIX_C_SOURCE 200809L
/*
 * auth.c — PAM authentication for isde-dm
 */
#include "dm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <security/pam_appl.h>

/* ---------- PAM conversation ---------- */

/* We store the password in the appdata_ptr field of pam_conv. */

static int pam_conversation(int num_msg, const struct pam_message **msg,
                            struct pam_response **resp, void *appdata_ptr)
{
    const char *password = (const char *)appdata_ptr;

    struct pam_response *reply = calloc(num_msg, sizeof(*reply));
    if (!reply) {
        return PAM_BUF_ERR;
    }

    for (int i = 0; i < num_msg; i++) {
        switch (msg[i]->msg_style) {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
            reply[i].resp = strdup(password ? password : "");
            if (!reply[i].resp) {
                /* Free already-allocated responses */
                for (int j = 0; j < i; j++) {
                    free(reply[j].resp);
                }
                free(reply);
                return PAM_BUF_ERR;
            }
            break;
        case PAM_ERROR_MSG:
        case PAM_TEXT_INFO:
            /* No response needed */
            break;
        default:
            free(reply);
            return PAM_CONV_ERR;
        }
    }

    *resp = reply;
    return PAM_SUCCESS;
}

/* ---------- public API ---------- */

int dm_auth_check(const char *username, const char *password,
                  char *errbuf, size_t errlen)
{
    pam_handle_t *pamh = NULL;
    int ret;

    /* We need a mutable copy of the password for the conversation.
     * Zero it after PAM is done. */
    char *pass_copy = strdup(password ? password : "");
    if (!pass_copy) {
        snprintf(errbuf, errlen, "Out of memory");
        return -1;
    }

    struct pam_conv conv = {
        .conv = pam_conversation,
        .appdata_ptr = pass_copy,
    };

    ret = pam_start("isde-dm", username, &conv, &pamh);
    if (ret != PAM_SUCCESS) {
        snprintf(errbuf, errlen, "%s", pam_strerror(pamh, ret));
        goto fail;
    }

    ret = pam_authenticate(pamh, 0);
    if (ret != PAM_SUCCESS) {
        snprintf(errbuf, errlen, "%s", pam_strerror(pamh, ret));
        goto fail;
    }

    ret = pam_acct_mgmt(pamh, 0);
    if (ret != PAM_SUCCESS) {
        snprintf(errbuf, errlen, "%s", pam_strerror(pamh, ret));
        goto fail;
    }

    /* Zero the password copy */
    memset(pass_copy, 0, strlen(pass_copy));
    free(pass_copy);
    pam_end(pamh, PAM_SUCCESS);
    return 0;

fail:
    memset(pass_copy, 0, strlen(pass_copy));
    free(pass_copy);
    if (pamh) {
        pam_end(pamh, ret);
    }
    return -1;
}

void *dm_auth_start_session(const char *username)
{
    pam_handle_t *pamh = NULL;
    int ret;

    /* Open a fresh PAM handle for the session (no password needed) */
    struct pam_conv conv = {
        .conv = pam_conversation,
        .appdata_ptr = NULL,
    };

    ret = pam_start("isde-dm", username, &conv, &pamh);
    if (ret != PAM_SUCCESS) {
        return NULL;
    }

    ret = pam_setcred(pamh, PAM_ESTABLISH_CRED);
    if (ret != PAM_SUCCESS) {
        pam_end(pamh, ret);
        return NULL;
    }

    ret = pam_open_session(pamh, 0);
    if (ret != PAM_SUCCESS) {
        pam_setcred(pamh, PAM_DELETE_CRED);
        pam_end(pamh, ret);
        return NULL;
    }

    return pamh;
}

void dm_auth_end_session(void *pam_handle)
{
    pam_handle_t *pamh = (pam_handle_t *)pam_handle;
    if (!pamh) {
        return;
    }
    pam_close_session(pamh, 0);
    pam_setcred(pamh, PAM_DELETE_CRED);
    pam_end(pamh, PAM_SUCCESS);
}
