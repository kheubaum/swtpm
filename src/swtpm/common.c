/*
 * common.c -- Common code for swtpm and swtpm_cuse
 *
 * (c) Copyright IBM Corporation 2014, 2015, 2019.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifdef WITH_SECCOMP
# include <seccomp.h>
#endif

#include <libtpms/tpm_error.h>

#include "common.h"
#include "options.h"
#include "key.h"
#include "locality.h"
#include "logging.h"
#include "swtpm_nvstore.h"
#include "pidfile.h"
#include "tpmstate.h"
#include "ctrlchannel.h"
#include "server.h"
#include "seccomp_profile.h"
#include "tpmlib.h"
#include "mainloop.h"
#include "profile.h"
#include "swtpm_utils.h"
#include "utils.h"

/* --log %s */
static const OptionDesc logging_opt_desc[] = {
    {
        .name = "file",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "fd",
        .type = OPT_TYPE_INT,
    }, {
        .name = "level",
        .type = OPT_TYPE_UINT,
    }, {
        .name = "prefix",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "truncate",
        .type = OPT_TYPE_BOOLEAN,
    },
    END_OPTION_DESC
};

/* --key %s */
static const OptionDesc key_opt_desc[] = {
    {
        .name = "file",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "mode",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "format",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "remove",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "pwdfile",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "kdf",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "fd",
        .type = OPT_TYPE_INT,
    }, {
        .name = "pwdfd",
        .type = OPT_TYPE_INT,
    },
    END_OPTION_DESC
};

/* --pid %s */
static const OptionDesc pid_opt_desc[] = {
    {
        .name = "file",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "fd",
        .type = OPT_TYPE_INT,
    },
    END_OPTION_DESC
};

/* --state %s */
static const OptionDesc tpmstate_opt_desc[] = {
    {
        .name = "dir",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "mode",
        .type = OPT_TYPE_MODE_T,
    }, {
        .name = "backend-uri",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "lock",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "backup",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "fsync",
        .type = OPT_TYPE_BOOLEAN,
    },
    END_OPTION_DESC
};

static const OptionDesc ctrl_opt_desc[] = {
    {
        .name = "type",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "path",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "port",
        .type = OPT_TYPE_INT,
    }, {
        .name = "bindaddr",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "ifname",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "fd",
        .type = OPT_TYPE_INT,
    }, {
        .name = "clientfd",
        .type = OPT_TYPE_INT,
    }, {
        .name = "mode",
        .type = OPT_TYPE_MODE_T,
    }, {
        .name = "uid",
        .type = OPT_TYPE_UID_T,
    }, {
        .name = "gid",
        .type = OPT_TYPE_GID_T,
    }, {
        .name = "terminate",
        .type = OPT_TYPE_BOOLEAN,
    },
    END_OPTION_DESC
};

static const OptionDesc server_opt_desc[] = {
    {
        .name = "type",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "path",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "port",
        .type = OPT_TYPE_INT,
    }, {
        .name = "bindaddr",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "ifname",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "fd",
        .type = OPT_TYPE_INT,
    }, {
        .name = "disconnect",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "mode",
        .type = OPT_TYPE_MODE_T,
    }, {
        .name = "uid",
        .type = OPT_TYPE_UID_T,
    }, {
        .name = "gid",
        .type = OPT_TYPE_GID_T,
    },
    END_OPTION_DESC
};

static const OptionDesc locality_opt_desc[] = {
    {
        .name = "reject-locality-4",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "allow-set-locality",
        .type = OPT_TYPE_BOOLEAN,
    },
    END_OPTION_DESC
};

static const OptionDesc flags_opt_desc[] = {
    {
        .name = "not-need-init",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "startup-none",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "startup-clear",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "startup-state",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "startup-deactivated",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "disable-auto-shutdown",
        .type = OPT_TYPE_BOOLEAN,
    },
    END_OPTION_DESC
};

#ifdef WITH_SECCOMP
static const OptionDesc seccomp_opt_desc[] = {
    {
        .name = "action",
        .type = OPT_TYPE_STRING,
    },
    END_OPTION_DESC
};
#endif

static const OptionDesc migration_opt_desc[] = {
    {
        .name = "incoming",
        .type = OPT_TYPE_BOOLEAN,
    }, {
        .name = "release-lock-outgoing",
        .type = OPT_TYPE_BOOLEAN,
    },
    END_OPTION_DESC
};

static const OptionDesc profile_opt_desc[] = {
    {
        .name = "name",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "profile",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "file",
        .type = OPT_TYPE_STRING,
    }, {
        .name = "fd",
        .type = OPT_TYPE_INT,
    }, {
        .name = "remove-disabled",
        .type = OPT_TYPE_STRING,
    },
    END_OPTION_DESC
};

/*
 * handle_log_options:
 * Parse and act upon the parsed log options. Initialize the logging.
 * @options: the log options
 *
 * Returns 0 on success, -1 on failure.
 */
int
handle_log_options(const char *options)
{
    char *error = NULL;
    const char *logfile = NULL, *logprefix = NULL;
    int logfd;
    unsigned int loglevel;
    bool logtruncate;
    OptionValues *ovs = NULL;

    if (!options)
        return 0;

    ovs = options_parse(options, logging_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing logging options: %s\n",
                  error);
        free(error);
        return -1;
    }
    logfile = option_get_string(ovs, "file", NULL);
    logfd = option_get_int(ovs, "fd", -1);
    loglevel = option_get_uint(ovs, "level", 0);
    logprefix = option_get_string(ovs, "prefix", NULL);
    logtruncate = option_get_bool(ovs, "truncate", false);
    if (logfile && (log_init(logfile, logtruncate) < 0)) {
        logprintf(STDERR_FILENO,
                  "Could not open logfile for writing: %s\n",
                  strerror(errno));
        goto error;
    } else if (logfd >= 0 && (log_init_fd(logfd) < 0)) {
        logprintf(STDERR_FILENO,
                  "Could not access logfile using fd %d: %s\n",
                  logfd, strerror(errno));
        goto error;
    }
    if ((logfile || logfd) && !loglevel)
        loglevel = 1;

    if (log_set_prefix(logprefix) < 0) {
        logprintf(STDERR_FILENO,
                  "Could not set logging prefix. Out of memory?\n");
        goto error;
    }
    if (log_set_level(loglevel) < 0) {
        logprintf(STDERR_FILENO,
                  "Could not set log level. Out of memory?");
        goto error;
    }

    option_values_free(ovs);

    return 0;

error:
    option_values_free(ovs);

    return -1;
}

/*
 * parse_key_options:
 * Parse and act upon the parsed key options.
 *
 * @options: the key options to parse
 * @key: buffer to hold the key
 * @maxkeylen: size of the buffer (= max. size the key can have)
 * @keylen: the length of the parsed key
 * @encmode: the encryption mode as determined from the options
 *
 * Returns 0 on success, -1 on failure.
 */
static int
parse_key_options(const char *options, unsigned char *key, size_t maxkeylen,
                  size_t *keylen, enum encryption_mode *encmode)
{
    OptionValues *ovs = NULL;
    char *error = NULL;
    const char *keyfile = NULL;
    const char *pwdfile = NULL;
    const char *tmp;
    enum key_format keyformat;
    enum kdf_identifier kdfid;
    size_t mode_keylength;
    int ret;
    int keyfile_fd = -1;
    int pwdfile_fd = -1;

    ovs = options_parse(options, key_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing key options: %s\n",
                  error);
        goto error;
    }

    keyfile = option_get_string(ovs, "file", NULL);
    keyfile_fd = option_get_int(ovs, "fd", -1);
    pwdfile = option_get_string(ovs, "pwdfile", NULL);
    pwdfile_fd = option_get_int(ovs, "pwdfd", -1);
    if (!keyfile && keyfile_fd == -1 && !pwdfile && pwdfile_fd == -1) {
        logprintf(STDERR_FILENO,
                  "Either file=, fd=, pwdfile=, or pwdfd= is required for key option\n");
        goto error;
    }

    tmp = option_get_string(ovs, "format", NULL);
    keyformat = key_format_from_string(tmp ? tmp : "hex");
    if (keyformat == KEY_FORMAT_UNKNOWN)
        goto error;

    tmp = option_get_string(ovs, "mode", NULL);
    *encmode = encryption_mode_from_string(tmp ? tmp : "aes-128-cbc",
                                           &mode_keylength);
    if (*encmode == ENCRYPTION_MODE_UNKNOWN) {
        logprintf(STDERR_FILENO, "Unknown encryption mode '%s'.\n", tmp);
        goto error;
    }

    if (mode_keylength > maxkeylen) {
        /* program error ... */
        logprintf(STDERR_FILENO,
                  "Requested key size %zu larger than supported size %zu.\n",
                  mode_keylength, maxkeylen);
        goto error;
    }

    if (keyfile != NULL) {
        if (key_load_key(keyfile, keyformat,
                         key, keylen, mode_keylength) < 0)
            goto error;
    } else if (keyfile_fd >= 0) {
        if (key_load_key_fd(keyfile_fd, keyformat,
                            key, keylen, mode_keylength) < 0)
            goto error;
    } else {
        tmp = option_get_string(ovs, "kdf", "pbkdf2");
        kdfid = kdf_identifier_from_string(tmp);
        if (kdfid == KDF_IDENTIFIER_UNKNOWN) {
            logprintf(STDERR_FILENO, "Unknown kdf '%s'.\n", tmp);
            goto error;
        }
        /* no key file, so must be pwdfile or pwdfile_fd */
        if (pwdfile) {
            if (key_from_pwdfile(pwdfile, key, keylen,
                                 mode_keylength, kdfid) < 0)
                goto error;
        } else {
            if (key_from_pwdfile_fd(pwdfile_fd, key, keylen,
                                    mode_keylength, kdfid) < 0)
                goto error;
        }
    }

    if (option_get_bool(ovs, "remove", false)) {
        if (keyfile)
            unlink(keyfile);
        if (pwdfile)
            unlink(pwdfile);
    }

    ret = 0;

exit:
    option_values_free(ovs);
    if (keyfile_fd >= 0)
        close(keyfile_fd);
    if (pwdfile_fd >= 0)
        close(pwdfile_fd);

    return ret;

error:
    ret = -1;
    free(error);

    goto exit;
}

/*
 * handle_key_options:
 * Parse and act upon the parsed key options. Set global values related
 * to the options found.
 * @options: the key options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int
handle_key_options(const char *options)
{
    enum encryption_mode encmode = ENCRYPTION_MODE_UNKNOWN;
    unsigned char key[256/8];
    size_t maxkeylen = sizeof(key);
    size_t keylen;
    int ret = 0;

    if (!options)
        return 0;

    if (parse_key_options(options, key, maxkeylen, &keylen, &encmode) < 0) {
        ret = -1;
        goto error;
    }

    if (SWTPM_NVRAM_Set_FileKey(key, keylen, encmode) != TPM_SUCCESS) {
        ret = -1;
        goto error;
    }

error:
    /* Wipe to ensure we don't leave a key on the stack */
    memset(key, 0, maxkeylen);
    return ret;
}

/*
 * handle_migration_key_options:
 * Parse and act upon the parsed key options. Set global values related
 * to the options found.
 * @options: the key options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int
handle_migration_key_options(const char *options)
{
    enum encryption_mode encmode = ENCRYPTION_MODE_UNKNOWN;
    unsigned char key[256/8];
    size_t maxkeylen = sizeof(key);
    size_t keylen;
    int ret = 0;

    if (!options)
        return 0;

    if (parse_key_options(options, key, maxkeylen, &keylen, &encmode) < 0) {
        ret = -1;
        goto error;
    }

    if (SWTPM_NVRAM_Set_MigrationKey(key, keylen, encmode) != TPM_SUCCESS) {
        ret = -1;
        goto error;
    }

error:
    /* Wipe to ensure we don't leave a key on the stack */
    memset(key, 0, maxkeylen);
    return ret;
}

/*
 * parse_pid_options:
 * Parse and act upon the parsed 'pid' options.
 *
 * @options: the 'pid' options to parse
 * @pidfile: Point to pointer for pidfile
 * @pidfilefd: Pointer to file descriptor for pidfile
 *
 * Returns 0 on success, -1 on failure.
 */
static int
parse_pid_options(const char *options, char **pidfile, int *pidfilefd)
{
    OptionValues *ovs = NULL;
    char *error = NULL;
    const char *filename = NULL;
    struct stat stat;

    ovs = options_parse(options, pid_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing pid options: %s\n",
                  error);
        goto error;
    }

    filename = option_get_string(ovs, "file", NULL);
    *pidfilefd = option_get_int(ovs, "fd", -1);
    if (!filename && *pidfilefd < 0) {
        logprintf(STDERR_FILENO,
                  "The file or fd parameter is required for the pid option.\n");
        goto error;
    }

    if (filename) {
        *pidfile = strdup(filename);
        if (!*pidfile) {
            logprintf(STDERR_FILENO, "Out of memory.");
            goto error;
        }
    } else {
        if (fstat(*pidfilefd, &stat) < 0 || !S_ISREG(stat.st_mode)) {
            logprintf(STDERR_FILENO,
                      "Bad filedescriptor %d for pid file\n", *pidfilefd);
            goto error;
        }
    }

    option_values_free(ovs);

    return 0;

error:
    option_values_free(ovs);
    if (*pidfilefd >= 0)
        close(*pidfilefd);
    free(error);

    return -1;
}

/*
 * handle_pidfile_options:
 * Parse and act upon the parse pidfile options.
 *
 * @options: the pidfile options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int
handle_pid_options(const char *options)
{
    char *pidfile = NULL;
    int pidfilefd = -1;
    int ret = 0;

    if (!options)
        return 0;

    if (parse_pid_options(options, &pidfile, &pidfilefd) < 0)
        return -1;

    if (pidfile && pidfile_set(pidfile) < 0)
        ret = -1;
    else if (pidfile_set_fd(pidfilefd) < 0)
        ret = -1;

    free(pidfile);

    return ret;
}

/*
 * parse_tpmstate_options:
 * Parse and act upon the parsed 'tpmstate' options.
 *
 * @options: the 'pid' options to parse
 * @tpmstatedir: Point to pointer for tpmstatedir
 * @mode: the mode of the TPM's state files
 * @mode_is_default: true if user did not provide mode bits but using default
 * @tpmbackend_uri: Point to pointer for backend URI
 * @do_locking: whether the backend should file-lock the storage
 * @make_backup: whether a backup file should be created
 * @do_fsync: whether to call fsync on the file and its directory
 *
 * Returns 0 on success, -1 on failure.
 */
static int
parse_tpmstate_options(const char *options, char **tpmstatedir, mode_t *mode,
                       bool *mode_is_default, char **tpmbackend_uri,
                       bool *do_locking, bool *make_backup, bool *do_fsync)
{
    OptionValues *ovs = NULL;
    char *error = NULL;
    const char *directory = NULL;
    const char *backend_uri = NULL;
    /* historically dir backend always locked, file backend did not */
    bool lock_default = true;

    ovs = options_parse(options, tpmstate_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing tpmstate options: %s\n",
                  error);
        goto error;
    }

    directory = option_get_string(ovs, "dir", NULL);
    backend_uri = option_get_string(ovs, "backend-uri", NULL);
    *make_backup = option_get_bool(ovs, "backup", false);
    *do_fsync = option_get_bool(ovs, "fsync", false);

    /* Did user provide mode bits? User can only provide <= 0777 */
    *mode = option_get_mode_t(ovs, "mode", 01000);
    *mode_is_default = (*mode == 01000);
    if (*mode_is_default)
        *mode = 0640;

    if (directory) {
        *tpmstatedir = strdup(directory);
        if (!*tpmstatedir) {
            logprintf(STDERR_FILENO, "Out of memory.");
            goto error;
        }
    } else if (backend_uri) {
        *tpmbackend_uri = strdup(backend_uri);
        if (!*tpmbackend_uri) {
            logprintf(STDERR_FILENO, "Out of memory.");
            goto error;
        }
        if (strncmp(*tpmbackend_uri, "file://", 7) == 0)
            lock_default = false;
    } else {
        logprintf(STDERR_FILENO,
                  "The dir or backend-uri parameters is required "
                  "for the tpmstate option.\n");
        goto error;
    }

    *do_locking = option_get_bool(ovs, "lock", lock_default);

    option_values_free(ovs);

    return 0;

error:
    free(error);
    option_values_free(ovs);

    return -1;
}

/*
 * handle_tpmstate_options:
 * Parse and act upon the parsed 'tpmstate' options.
 *
 * @options: the tpmstate options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int
handle_tpmstate_options(const char *options)
{
    char *tpmstatedir = NULL;
    char *tpmbackend_uri = NULL;
    char *temp_uri = NULL;
    int ret = 0;
    mode_t mode;
    bool mode_is_default = true;
    bool do_locking = false;
    bool make_backup = false;
    bool do_fsync = false;

    if (!options)
        return 0;

    if (parse_tpmstate_options(options, &tpmstatedir, &mode,
                               &mode_is_default, &tpmbackend_uri,
                               &do_locking, &make_backup, &do_fsync) < 0) {
        ret = -1;
        goto error;
    }

    if (tpmstatedir) {
        /* Default tpmstate store dir backend */
        if (asprintf(&temp_uri, "dir://%s", tpmstatedir) < 0) {
            logprintf(STDERR_FILENO,
                      "Could not asprintf TPM backend uri\n");
            ret = -1;
            temp_uri = NULL;
            goto error;
        }

        if (tpmstate_set_backend_uri(temp_uri) < 0) {
            ret = -1;
            goto error;
        }
    } else {
        if (tpmstate_set_backend_uri(tpmbackend_uri) < 0) {
            ret = -1;
            goto error;
        }
    }

    tpmstate_set_mode(mode, mode_is_default);
    tpmstate_set_locking(do_locking);
    tpmstate_set_make_backup(make_backup);
    tpmstate_set_do_fsync(do_fsync);

error:
    free(tpmstatedir);
    free(tpmbackend_uri);
    free(temp_uri);

    return ret;
}

/*
 * unixio_open_socket: Open a UnixIO socket and return file descriptor
 *
 * @path: UnixIO socket path
 * @perm: UnixIO socket permissions
 * @uid: uid to set the ownership of the UnixIO socket path to
 * @gid: gid to set the ownership of the UnixIO socket path to
 */
static int unixio_open_socket(const char *path, mode_t perm,
                              uid_t uid, gid_t gid)
{
    struct sockaddr_un su;
    int fd = -1, n;
    size_t len;

    su.sun_family = AF_UNIX;
    len = sizeof(su.sun_path);
    n = snprintf(su.sun_path, len, "%s", path);
    if (n < 0) {
        logprintf(STDERR_FILENO, "Could not nsprintf path to UnixIO socket\n");
        return -1;
    }
    if (n >= (int)len) {
        logprintf(STDERR_FILENO, "Path for UnioIO socket is too long\n");
        return -1;
    }

    unlink(su.sun_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        logprintf(STDERR_FILENO, "Could not open UnixIO socket\n");
        return -1;
    }

    len = strlen(su.sun_path) + sizeof(su.sun_family) + 1;
    n = bind(fd, (struct sockaddr *)&su, len);
    if (n < 0) {
        logprintf(STDERR_FILENO, "Could not open UnixIO socket: %s\n",
                  strerror(errno));
        goto error;
    }

    if (chmod(su.sun_path, perm) < 0) {
        logprintf(STDERR_FILENO,
                  "Could not change permissions on UnixIO socket: %s\n",
                  strerror(errno));
        goto error;
    }

    if ((uid != (uid_t)-1 || gid != (gid_t)-1) &&
        chown(su.sun_path, uid, gid) < 0) {
        logprintf(STDERR_FILENO,
                  "Could not change ownership of UnixIO socket to "
                  "%u:%u %s\n", uid, gid, strerror(errno));
        goto error;
    }

    n = listen(fd, 1);
    if (n < 0) {
        logprintf(STDERR_FILENO, "Cannot listen on UnixIO socket: %s\n",
                  strerror(errno));
        goto error;
    }

    return fd;

error:
    close(fd);

    return -1;
}

/*
 * tcp_open_socket: Open a TCP port and return the file descriptor
 *
 * @port: port number
 * @bindadddr: the address to bind the socket to
 */
static int tcp_open_socket(unsigned short port, const char *bindaddr,
                           const char *ifname)
{
    int fd = -1, n, af, opt;
    struct sockaddr_in si;
    struct sockaddr_in6 si6;
    struct sockaddr *sa;
    socklen_t sa_len;
    void *dst;

    if (index(bindaddr, ':')) {
        af = AF_INET6;

        memset(&si6, 0, sizeof(si6));
        si6.sin6_family = af;
        si6.sin6_port = htons(port);

        dst = &si6.sin6_addr.s6_addr;
        sa = (struct sockaddr *)&si6;
        sa_len = sizeof(si6);
    } else {
        af = AF_INET;

        si.sin_family = af;
        si.sin_port = htons(port);
        memset(&si.sin_zero, 0, sizeof(si.sin_zero));

        dst = &si.sin_addr.s_addr;
        sa = (struct sockaddr *)&si;
        sa_len = sizeof(si);
    }

    n = inet_pton(af, bindaddr, dst);
    if (n <= 0) {
        logprintf(STDERR_FILENO, "Could not parse the bind address '%s'\n",
                  bindaddr);
        return -1;
    }

    if (af == AF_INET6) {
        if (IN6_IS_ADDR_LINKLOCAL(&si6.sin6_addr)) {
            if (!ifname) {
                logprintf(STDERR_FILENO,
                          "Missing interface name for link local address\n");
                return -1;
            }
            n = if_nametoindex(ifname);
            if (!n) {
                logprintf(STDERR_FILENO,
                          "Could not convert interface name '%s' to "
                          "index: %s\n",
                          ifname, strerror(errno));
                return -1;
            }
            si6.sin6_scope_id = n;
        }
    }

    fd = socket(af, SOCK_STREAM, 0);
    if (fd < 0) {
        logprintf(STDERR_FILENO, "Could not open TCP socket\n");
        return -1;
    }

    opt = 1;
    n = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (n < 0) {
        logprintf(STDERR_FILENO,
                  "Could not set socket option SO_REUSEADDR: %s\n",
                  strerror(errno));
        goto error;
    }

    n = bind(fd, sa, sa_len);
    if (n < 0) {
        logprintf(STDERR_FILENO, "Could not open TCP socket: %s\n",
                  strerror(errno));
        goto error;
    }

    n = listen(fd, 1);
    if (n < 0) {
        logprintf(STDERR_FILENO, "Cannot listen on TCP socket: %s\n",
                  strerror(errno));
        goto error;
    }

    return fd;

error:
    close(fd);

    return -1;
}

/*
 * parse_ctrlchannel_options:
 * Parse the 'ctrl' (control channel) options.
 *
 * @options: the control channel options to parse
 * @cc: pointer for pointer to allocated ctrlchannel struct
 * @mainloop_flags: pointer to mainloop flags to add a flag to
 *
 * Returns 0 on success, -1 on failure.
 */
static int parse_ctrlchannel_options(const char *options,
                                     struct ctrlchannel **cc,
                                     uint32_t *mainloop_flags)
{
    OptionValues *ovs = NULL;
    char *error = NULL;
    const char *type, *path, *bindaddr, *ifname;
    int fd, clientfd, port;
    struct stat stat;
    mode_t mode;
    uid_t uid;
    gid_t gid;

    ovs = options_parse(options, ctrl_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing ctrl options: %s\n", error);
        goto error;
    }

    type = option_get_string(ovs, "type", NULL);
    if (!type) {
        logprintf(STDERR_FILENO,
                  "Missing type parameter for control channel\n");
        goto error;
    }

    if (!strcmp(type, "unixio")) {
        path = option_get_string(ovs, "path", NULL);
        fd = option_get_int(ovs, "fd", -1);
        clientfd = option_get_int(ovs, "clientfd", -1);
        mode = option_get_mode_t(ovs, "mode", 0770);
        uid = option_get_uid_t(ovs, "uid", -1);
        gid = option_get_gid_t(ovs, "gid", -1);
        if (fd >= 0) {
            if (fstat(fd, &stat) < 0 || !S_ISSOCK(stat.st_mode)) {
               logprintf(STDERR_FILENO,
                         "Bad filedescriptor %d for UnixIO control channel\n",
                         fd);
               goto error;
            }

            *cc = ctrlchannel_new(fd, false, NULL);
        } else if (clientfd >= 0) {
            if (fstat(clientfd, &stat) < 0 || !S_ISSOCK(stat.st_mode)) {
               logprintf(STDERR_FILENO,
                         "Bad filedescriptor %d for UnixIO client control"
                         " channel\n", clientfd);
               goto error;
            }

            *cc = ctrlchannel_new(clientfd, true, NULL);
        } else if (path) {
            fd = unixio_open_socket(path, mode, uid, gid);
            if (fd < 0)
                goto error;

            *cc = ctrlchannel_new(fd, false, path);
        } else {
            logprintf(STDERR_FILENO,
                      "Missing path and fd options for UnixIO "
                      "control channel\n");
            goto error;
        }
    } else if (!strcmp(type, "tcp")) {
        port = option_get_int(ovs, "port", -1);
        fd = option_get_int(ovs, "fd", -1);
        if (fd >= 0) {
            if (fstat(fd, &stat) < 0 || !S_ISSOCK(stat.st_mode)) {
               logprintf(STDERR_FILENO,
                         "Bad filedescriptor %d for TCP control channel\n", fd);
               goto error;
            }

            *cc = ctrlchannel_new(fd, false, NULL);
        } else if (port >= 0) {
            if (port >= 0x10000) {
                logprintf(STDERR_FILENO,
                          "TCP control channel port outside valid range\n");
                goto error;
            }

            bindaddr = option_get_string(ovs, "bindaddr", "127.0.0.1");
            ifname = option_get_string(ovs, "ifname", NULL);

            fd = tcp_open_socket(port, bindaddr, ifname);
            if (fd < 0)
                goto error;

            *cc = ctrlchannel_new(fd, false, NULL);
        } else {
            logprintf(STDERR_FILENO,
                      "Missing port and fd options for TCP control channel\n");
            goto error;
        }
    } else {
        logprintf(STDERR_FILENO, "Unsupported control channel type: %s\n", type);
        goto error;
    }

    if (*cc == NULL)
        goto error;

    if (option_get_bool(ovs, "terminate", false))
        *mainloop_flags |= MAIN_LOOP_FLAG_CTRL_END_ON_HUP;

    option_values_free(ovs);

    return 0;

error:
    free(error);
    option_values_free(ovs);

    return -1;
}

/*
 * handle_ctrlchannel_options:
 * Parse and act upon the parsed 'ctrl' (control channel) options.
 *
 * @options: the control channel options to parse
 * @cc: pointer for pointer to allocated ctrlchannel struct
 * @mainloop_flags: pointer to mainloop flags to add a flag to
 *
 * Returns 0 on success, -1 on failure.
 */
int handle_ctrlchannel_options(const char *options, struct ctrlchannel **cc,
                               uint32_t *mainloop_flag)
{
    if (!options)
        return 0;

    if (parse_ctrlchannel_options(options, cc, mainloop_flag) < 0)
        return -1;

    return 0;
}

/*
 * parse_server_options:
 * Parse the 'server' options.
 *
 * @options: the server options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
static int parse_server_options(const char *options, struct server **c)
{
    OptionValues *ovs = NULL;
    char *error = NULL;
    const char *bindaddr, *ifname;
    const char *type, *path;
    int fd, port;
    struct stat stat;
    unsigned int flags = 0;
    mode_t mode;
    uid_t uid;
    gid_t gid;

    *c = NULL;

    ovs = options_parse(options, server_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing server options: %s\n", error);
        goto error;
    }

    type = option_get_string(ovs, "type", "tcp");

    if (option_get_bool(ovs, "disconnect", false))
        flags |= SERVER_FLAG_DISCONNECT;

    if (!strcmp(type, "unixio")) {
        path = option_get_string(ovs, "path", NULL);
        fd = option_get_int(ovs, "fd", -1);
        mode = option_get_mode_t(ovs, "mode", 0770);
        uid = option_get_uid_t(ovs, "uid", -1);
        gid = option_get_gid_t(ovs, "gid", -1);
        if (fd >= 0) {
            if (fstat(fd, &stat) < 0 || !S_ISSOCK(stat.st_mode)) {
               logprintf(STDERR_FILENO,
                         "Bad filedescriptor %d for UnixIO control channel\n",
                         fd);
               goto error;
            }

            *c = server_new(fd, flags, NULL);
        } else if (path) {
            fd = unixio_open_socket(path, mode, uid, gid);
            if (fd < 0)
                goto error;

            *c = server_new(fd, flags, path);
        } else {
            logprintf(STDERR_FILENO,
                      "Missing path and file descriptor option for UnixIO "
                      "socket\n");
            goto error;
        }
    } else if (!strcmp(type, "tcp")) {
        fd = option_get_int(ovs, "fd", -1);
        if (fd >= 0) {
            if (fstat(fd, &stat) < 0 || !S_ISSOCK(stat.st_mode)) {
               logprintf(STDERR_FILENO,
                         "Bad filedescriptor %d for TCP socket\n", fd);
               goto error;
            }

            flags |= SERVER_FLAG_FD_GIVEN;

            *c = server_new(fd, flags, NULL);
        } else {
            port = option_get_int(ovs, "port", -1);
            if (port == -1) {
                const char *port_str = getenv("TPM_PORT");
                if (!port_str || sscanf(port_str, "%d", &port) == -1)
                    port = -1;
            }
            if (port < 0) {
                logprintf(STDERR_FILENO,
                      "No valid port number provided for TCP socket.\n");
                goto error;
            }
            if (port >= 0x10000) {
                logprintf(STDERR_FILENO,
                          "TCP socket port outside valid range\n");
                goto error;
            }

            bindaddr = option_get_string(ovs, "bindaddr", "127.0.0.1");
            ifname = option_get_string(ovs, "ifname", NULL);

            fd = tcp_open_socket(port, bindaddr, ifname);
            if (fd < 0)
                goto error;

            *c = server_new(fd, flags, NULL);
        }
    } else {
        logprintf(STDERR_FILENO, "Unsupported socket type: %s\n", type);
        goto error;
    }

    if (*c == NULL)
        goto error;

    option_values_free(ovs);

    return 0;

error:
    if (*c) {
        server_free(*c);
        *c = NULL;
    }
    option_values_free(ovs);
    free(error);

    return -1;
}

/*
 * handle_server_options:
 * Parse and act upon the parsed 'server' options.
 *
 * @options: the server options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int handle_server_options(const char *options, struct server **c)
{
    if (!options)
        return 0;

    if (parse_server_options(options, c) < 0)
        return -1;

    return 0;
}

static int parse_locality_options(const char *options, uint32_t *flags)
{
    OptionValues *ovs = NULL;
    char *error = NULL;

    ovs = options_parse(options, locality_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing locality options: %s\n", error);
        goto error;
    }

    if (option_get_bool(ovs, "reject-locality-4", false))
        *flags |= LOCALITY_FLAG_REJECT_LOCALITY_4;
    if (option_get_bool(ovs, "allow-set-locality", false))
        *flags |= LOCALITY_FLAG_ALLOW_SETLOCALITY;

    option_values_free(ovs);

    return 0;

error:
    option_values_free(ovs);
    free(error);

    return -1;
}

/*
 * handle_locality_options:
 * Parse the 'locality' options.
 *
 * @options: the locality options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int handle_locality_options(const char *options, uint32_t *flags)
{
    *flags = 0;

    if (!options)
        return 0;

    if (parse_locality_options(options, flags) < 0)
        return -1;

    return 0;
}

static int parse_flags_options(const char *options, bool *need_init_cmd,
                               uint16_t *startupType, bool *disable_auto_shutdown)
{
    OptionValues *ovs = NULL;
    char *error = NULL;

    ovs = options_parse(options, flags_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing flags options: %s\n", error);
        goto error;
    }

    if (option_get_bool(ovs, "not-need-init", false))
        *need_init_cmd = false;
    if (option_get_bool(ovs, "disable-auto-shutdown", false))
        *disable_auto_shutdown = true;

    if (option_get_bool(ovs, "startup-clear", false))
        *startupType = TPM_ST_CLEAR;
    else if (option_get_bool(ovs, "startup-state", false))
        *startupType = TPM_ST_STATE;
    else if (option_get_bool(ovs, "startup-deactivated", false))
        *startupType = TPM_ST_DEACTIVATED;
    else if (option_get_bool(ovs, "startup-none", false))
        *startupType = _TPM_ST_NONE;

    if (*startupType != _TPM_ST_NONE)
        *need_init_cmd = false;

    option_values_free(ovs);

    return 0;

error:
    option_values_free(ovs);
    free(error);

    return -1;
}

/*
 * handle_flags_options:
 * Parse the 'flags' options.
 *
 * @options: the flags options to parse
 *
 * Returns 0 on success, -1 on failure.
 */
int handle_flags_options(const char *options, bool *need_init_cmd,
                         uint16_t *startupType, bool *disable_auto_shutdown)
{
    if (!options)
        return 0;

    if (parse_flags_options(options, need_init_cmd, startupType,
                            disable_auto_shutdown) < 0)
        return -1;

    return 0;
}

#ifdef WITH_SECCOMP
static int parse_seccomp_options(const char *options, unsigned int *seccomp_action)
{
    OptionValues *ovs = NULL;
    char *error = NULL;
    const char *action;

    ovs = options_parse(options, seccomp_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing seccomp options: %s\n", error);
        goto error;
    }

    action = option_get_string(ovs, "action", "kill");
    if (!strcmp(action, "kill")) {
        *seccomp_action = SWTPM_SECCOMP_ACTION_KILL;
#ifdef SCMP_ACT_LOG
    } else if (!strcmp(action, "log")) {
        *seccomp_action = SWTPM_SECCOMP_ACTION_LOG;
#endif
    } else if (!strcmp(action, "none")) {
        *seccomp_action = SWTPM_SECCOMP_ACTION_NONE;
    } else {
        logprintf(STDERR_FILENO,
                  "Unsupported seccomp log action %s\n", action);
        goto error;
    }

    option_values_free(ovs);

    return 0;

error:
    option_values_free(ovs);
    free(error);

    return -1;
}

/*
 * handle_seccomp_options:
 * Parse the 'seccomp' options.
 *
 * @options: the flags options to parse
 * @seccomp_action: the action to take when 
 *
 * Returns 0 on success, -1 on failure.
 */
int handle_seccomp_options(const char *options, unsigned int *seccomp_action)
{
    *seccomp_action = SWTPM_SECCOMP_ACTION_KILL;

    if (!options)
        return 0;

    if (parse_seccomp_options(options, seccomp_action) < 0)
        return -1;

    return 0;
}
#endif /* WITH_SECCOMP */

static int parse_migration_options(const char *options, bool *incoming_migration,
                                   bool *release_lock_outgoing)
{
    OptionValues *ovs = NULL;
    char *error = NULL;

    ovs = options_parse(options, migration_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing migration options: %s\n", error);
        goto error;
    }

    *incoming_migration = option_get_bool(ovs, "incoming", false);
    *release_lock_outgoing = option_get_bool(ovs, "release-lock-outgoing",
                                             false);

    option_values_free(ovs);

    return 0;

error:
    option_values_free(ovs);
    free(error);

    return -1;
}

static int parse_profile_options(const char *options, char **json_profile)
{
    g_autofree gchar *buffer = NULL;
    const char *remove_disabled;
    OptionValues *ovs = NULL;
    gboolean force = false;
    GError *gerror = NULL;
    const char *filename;
    const char *profile;
    char *error = NULL;
    int profilefd = -1;
    gsize buffer_len;
    const char *name;

    ovs = options_parse(options, profile_opt_desc, &error);
    if (!ovs) {
        logprintf(STDERR_FILENO, "Error parsing profile options: %s\n", error);
        goto error;
    }

    profile = option_get_string(ovs, "profile", NULL);
    name = option_get_string(ovs, "name", NULL);
    filename = option_get_string(ovs, "file", NULL);
    profilefd = option_get_int(ovs, "fd", -1);

    if ((profile != NULL) + (name != NULL) + (filename != NULL) > 1 + (profilefd >= 0)) {
        logprintf(STDERR_FILENO, "Only one profile option parameter of 'profile', 'name', 'fd', or 'file' may be provided\n");
        goto error;
    }
    if (profile) {
        *json_profile = strdup(profile);
        if (!*json_profile)
            goto oom_error;
    } else if (name) {
        if (asprintf(json_profile, "{\"Name\":\"%s\"}", name) < 0) {
            logprintf(STDERR_FILENO, "Out of memory.\n");
            goto oom_error;
        }
    } else if (filename) {
        if (!g_file_get_contents(filename, &buffer, &buffer_len, &gerror)) {
            logprintf(STDERR_FILENO, "%s\n", gerror->message);
            goto error;
        }
        *json_profile = strndup(buffer, buffer_len);
    } else if (profilefd >= 0) {
        buffer_len = 10 * 1024;
        buffer = g_malloc(buffer_len);
        buffer_len = read_eintr(profilefd, buffer, buffer_len - 1);
        if ((ssize_t)buffer_len < 0) {
            logprintf(STDERR_FILENO, "Unable to read profile: %s\n",
                      strerror(errno));
            goto error;
        }
        SWTPM_CLOSE(profilefd);
        buffer[buffer_len] = 0;
        *json_profile = g_steal_pointer(&buffer);
    } else {
        logprintf(STDERR_FILENO,
                  "No profile option parameter given to get a profile\n");
        goto error;
    }
    /* remove leading and trailing whitespaces */
    g_strstrip(*json_profile);

    remove_disabled = option_get_string(ovs, "remove-disabled", NULL);
    if (remove_disabled) {
        if (!strcmp(remove_disabled, "check")) {
            force = false;
        } else if (!strcmp(remove_disabled, "fips-host")) {
            force = true;
        } else {
            logprintf(STDERR_FILENO, "Invalid option parameter '%s' for 'remove-disabled'\n",
                      remove_disabled);
            goto error;
        }
        if (profile_remove_fips_disabled_algorithms(json_profile, force) == -1)
            goto error;
    }

    option_values_free(ovs);

    return 0;

oom_error:
    logprintf(STDERR_FILENO,
              "Out of memory to create JSON profile\n");

error:
    if (profilefd >= 0)
        close(profilefd);
    if (gerror)
        g_error_free(gerror);
    SWTPM_G_FREE(*json_profile);
    option_values_free(ovs);
    free(error);

    return -1;
}

/*
 * handle_migration_options:
 * Parse the 'migration' options.
 *
 * @options: the migration options to parse
 * @incoming_migration: whether there's an incoming migration
 * @release_lock_outgoing: whether to release NVRAM lock on outgoing migration
 *
 * Return 0 on success, -1 on failure.
 */
int handle_migration_options(const char *options, bool *incoming_migration,
                             bool *release_lock_outgoing)
{
    *incoming_migration = false;

    if (!options)
        return 0;

    if (parse_migration_options(options, incoming_migration,
                                release_lock_outgoing) < 0)
        return -1;

    return 0;
}

/*
 * handle_profile_options:
 * Parse the 'profile' options.
 *
 * @options: the porfile options to parse
 * @json_profile: pointer to a buffer for the profile rules to pass to libtpms
 *
 * Returns 0 on success, -1 on failure.
 */
int handle_profile_options(const char *options, char **json_profile)
{
    *json_profile = NULL;

    if (!options)
        return 0;

    if (parse_profile_options(options, json_profile) < 0)
        return -1;

    return 0;
}
