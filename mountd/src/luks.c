#define _POSIX_C_SOURCE 200809L
/*
 * luks.c — LUKS encrypted volume operations
 *
 * Uses libcryptsetup for container open/close and libblkid to
 * probe the inner filesystem type after unlocking.
 */
#include "mountd.h"

#include <stdio.h>
#include <string.h>

#ifdef __linux__

#include <libcryptsetup.h>
#include <blkid/blkid.h>

int mountd_luks_open(const char *dev_path, const char *dm_name,
                     const char *passphrase, size_t passphrase_len,
                     char *errbuf, size_t errlen)
{
    struct crypt_device *cd = NULL;
    int r;

    r = crypt_init(&cd, dev_path);
    if (r < 0) {
        snprintf(errbuf, errlen, "crypt_init: %s", strerror(-r));
        return -1;
    }

    r = crypt_load(cd, CRYPT_LUKS, NULL);
    if (r < 0) {
        snprintf(errbuf, errlen, "crypt_load: %s", strerror(-r));
        crypt_free(cd);
        return -1;
    }

    r = crypt_activate_by_passphrase(cd, dm_name, CRYPT_ANY_SLOT,
                                     passphrase, passphrase_len, 0);
    if (r < 0) {
        if (r == -1)
            snprintf(errbuf, errlen, "Wrong passphrase");
        else
            snprintf(errbuf, errlen, "crypt_activate: %s", strerror(-r));
        crypt_free(cd);
        return -1;
    }

    crypt_free(cd);
    fprintf(stderr, "isde-mountd: LUKS opened %s as %s\n", dev_path, dm_name);
    return 0;
}

int mountd_luks_close(const char *dm_name,
                      char *errbuf, size_t errlen)
{
    struct crypt_device *cd = NULL;
    int r;

    r = crypt_init_by_name(&cd, dm_name);
    if (r < 0) {
        snprintf(errbuf, errlen, "crypt_init_by_name: %s", strerror(-r));
        return -1;
    }

    r = crypt_deactivate(cd, dm_name);
    crypt_free(cd);

    if (r < 0) {
        snprintf(errbuf, errlen, "crypt_deactivate: %s", strerror(-r));
        return -1;
    }

    fprintf(stderr, "isde-mountd: LUKS closed %s\n", dm_name);
    return 0;
}

int mountd_luks_is_active(const char *dm_name)
{
    crypt_status_info ci = crypt_status(NULL, dm_name);
    return ci == CRYPT_ACTIVE || ci == CRYPT_BUSY;
}

int mountd_luks_probe_fs(const char *dm_path,
                         char *fs_type, size_t fs_len)
{
    blkid_probe pr = blkid_new_probe_from_filename(dm_path);
    if (!pr) {
        return -1;
    }

    blkid_probe_enable_superblocks(pr, 1);
    blkid_probe_set_superblocks_flags(pr, BLKID_SUBLKS_TYPE);

    if (blkid_do_safeprobe(pr) != 0) {
        blkid_free_probe(pr);
        return -1;
    }

    const char *type = NULL;
    if (blkid_probe_lookup_value(pr, "TYPE", &type, NULL) == 0 && type) {
        snprintf(fs_type, fs_len, "%s", type);
        blkid_free_probe(pr);
        return 0;
    }

    blkid_free_probe(pr);
    return -1;
}

#else /* !__linux__ */

int mountd_luks_open(const char *dev_path, const char *dm_name,
                     const char *passphrase, size_t passphrase_len,
                     char *errbuf, size_t errlen)
{
    (void)dev_path; (void)dm_name;
    (void)passphrase; (void)passphrase_len;
    snprintf(errbuf, errlen, "LUKS not supported on this platform");
    return -1;
}

int mountd_luks_close(const char *dm_name,
                      char *errbuf, size_t errlen)
{
    (void)dm_name;
    snprintf(errbuf, errlen, "LUKS not supported on this platform");
    return -1;
}

int mountd_luks_is_active(const char *dm_name)
{
    (void)dm_name;
    return 0;
}

int mountd_luks_probe_fs(const char *dm_path,
                         char *fs_type, size_t fs_len)
{
    (void)dm_path; (void)fs_type; (void)fs_len;
    return -1;
}

#endif /* __linux__ */
