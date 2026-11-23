#define _GNU_SOURCE

#include <security/pam_appl.h>

#include <err.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth.h"

static void free_responses(struct pam_response* reply, int count) {
    for (int i = 0; i < count; i++) {
        if (!reply[i].resp) {
            continue;
        }

        explicit_bzero(reply[i].resp, strlen(reply[i].resp));
        free(reply[i].resp);
    }

    free(reply);
}

static int converse(int count, const struct pam_message** messages, struct pam_response** responses, void* data) {
    const char* secret = data;
    if (count <= 0 || count > 16 || !secret || !messages || !responses) {
        return PAM_CONV_ERR;
    }

    struct pam_response* reply = calloc((size_t) count, sizeof(*reply));
    if (!reply) {
        return PAM_BUF_ERR;
    }

    for (int i = 0; i < count; i++) {
        if (!messages[i]) {
            free_responses(reply, i);
            return PAM_CONV_ERR;
        }

        const char* text = NULL;

        switch (messages[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
                text = secret;
                break;
            case PAM_PROMPT_ECHO_ON:
                text = "";
                break;
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                break;
            default:
                free_responses(reply, i);
                return PAM_CONV_ERR;
        }

        if (!text) {
            continue;
        }

        reply[i].resp = strdup(text);
        if (!reply[i].resp) {
            free_responses(reply, i);
            return PAM_BUF_ERR;
        }
    }

    *responses = reply;
    return PAM_SUCCESS;
}

int authenticate(const char* password, const char* service) {
    struct passwd* pw = getpwuid(getuid());
    if (!pw || !pw->pw_name || pw->pw_name[0] == '\0') {
        warnx("getpwuid");
        return -1;
    }

    size_t length = strlen(pw->pw_name);
    if (length >= LOGIN_NAME_MAX) {
        warnx("username exceeds maximum length of %d", LOGIN_NAME_MAX - 1);
        return -1;
    }

    char user[LOGIN_NAME_MAX];

    memcpy(user, pw->pw_name, length + 1);

    struct pam_conv conv = {
        .conv = converse,
        .appdata_ptr = (void*) password,
    };
    pam_handle_t* pam = NULL;
    int ret = -1;

    int status = pam_start(service, user, &conv, &pam);
    if (status != PAM_SUCCESS) {
        warnx("pam_start failed: %s (PAM status %d)", pam_strerror(pam, status), status);
        goto end;
    }

    status = pam_authenticate(pam, PAM_DISALLOW_NULL_AUTHTOK);
    if (status != PAM_SUCCESS) {
        if (status != PAM_AUTH_ERR && status != PAM_USER_UNKNOWN && status != PAM_MAXTRIES && status != PAM_PERM_DENIED) {
            warnx("pam_authenticate failed: %s (PAM status %d)", pam_strerror(pam, status), status);
        }
        goto end;
    }

    status = pam_acct_mgmt(pam, 0);
    if (status == PAM_NEW_AUTHTOK_REQD) {
        warnx("password change required");
        goto end;
    }

    if (status != PAM_SUCCESS) {
        if (status != PAM_AUTH_ERR && status != PAM_PERM_DENIED) {
            warnx("pam_acct_mgmt failed: %s (PAM status %d)", pam_strerror(pam, status), status);
        }
        goto end;
    }

    ret = 0;

end:
    if (pam) {
        pam_end(pam, status);
    }
    explicit_bzero(user, sizeof(user));
    return ret;
}
