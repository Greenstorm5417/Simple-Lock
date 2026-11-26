#define _GNU_SOURCE

#include <security/pam_appl.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth.h"

#define PAM_MAX_MESSAGES 16
#define PASSWD_BUFFER_SIZE 16384

struct pam_data {
    const char* password;
    const char* username;
};

static void free_responses(struct pam_response* reply, int count) {
    if (!reply) {
        return;
    }

    for (int i = 0; i < count; i++) {
        if (reply[i].resp) {
            explicit_bzero(reply[i].resp, strlen(reply[i].resp));
            free(reply[i].resp);
        }
    }

    free(reply);
}

static int converse(int count, const struct pam_message** messages, struct pam_response** responses, void* data) {
    const struct pam_data* ctx = data;

    if (count <= 0 || count > PAM_MAX_MESSAGES || !messages || !responses || !ctx || !ctx->password || !ctx->username) {
        return PAM_CONV_ERR;
    }

    *responses = NULL;

    struct pam_response* reply = calloc((size_t) count, sizeof(*reply));
    if (!reply) {
        return PAM_BUF_ERR;
    }

    for (int i = 0; i < count; i++) {
        if (!messages[i]) {
            free_responses(reply, i);
            return PAM_CONV_ERR;
        }

        const char* text;

        switch (messages[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF:
                text = ctx->password;
                break;
            case PAM_PROMPT_ECHO_ON:
                text = ctx->username;
                break;
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO:
                continue;
            default:
                free_responses(reply, i);
                return PAM_CONV_ERR;
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

int authenticate(const char* password) {
    if (!password) {
        return -1;
    }

    struct passwd pw;
    struct passwd* result = NULL;
    char pwbuf[PASSWD_BUFFER_SIZE];

    int ret = getpwuid_r(getuid(), &pw, pwbuf, sizeof(pwbuf), &result);
    if (ret != 0 || !result || !pw.pw_name || !pw.pw_name[0]) {
        warnx("getpwuid_r failed: %s", ret ? strerror(ret) : "user not found");
        return -1;
    }

    size_t length = strlen(pw.pw_name);
    if (length >= LOGIN_NAME_MAX) {
        warnx("username exceeds maximum length");
        explicit_bzero(pwbuf, sizeof(pwbuf));
        return -1;
    }

    char user[LOGIN_NAME_MAX];
    memcpy(user, pw.pw_name, length + 1);
    explicit_bzero(pwbuf, sizeof(pwbuf));

    struct pam_data data = {
        .password = password,
        .username = user,
    };

    struct pam_conv conv = {
        .conv = converse,
        .appdata_ptr = &data,
    };

    pam_handle_t* pam = NULL;

    ret = -1;

    int status = pam_start("kisslock", user, &conv, &pam);
    if (status != PAM_SUCCESS) {
        warnx("pam_start failed (PAM status %d)", status);
        goto end;
    }

    status = pam_authenticate(pam, PAM_DISALLOW_NULL_AUTHTOK);
    if (status != PAM_SUCCESS) {
        if (status != PAM_AUTH_ERR && status != PAM_USER_UNKNOWN && status != PAM_MAXTRIES && status != PAM_PERM_DENIED) {
            warnx("pam_authenticate failed: %s", pam_strerror(pam, status));
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
            warnx("pam_acct_mgmt failed: %s",
                    pam_strerror(pam, status));
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
