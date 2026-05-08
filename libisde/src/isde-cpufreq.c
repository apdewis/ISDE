#define _POSIX_C_SOURCE 200809L
/*
 * isde-cpufreq.c — CPU performance profile get/set via sysfs
 */
#include "isde/isde-cpufreq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <glob.h>

#define CPUFREQ_BASE "/sys/devices/system/cpu/cpufreq"

static const char *epp_for_profile(IsdeCpuProfile p)
{
    switch (p) {
    case ISDE_PROFILE_POWER_SAVER:  return "power";
    case ISDE_PROFILE_BALANCED:     return "balance_performance";
    case ISDE_PROFILE_PERFORMANCE:  return "performance";
    }
    return "balance_performance";
}

static const char *governor_for_profile(IsdeCpuProfile p)
{
    switch (p) {
    case ISDE_PROFILE_POWER_SAVER:  return "powersave";
    case ISDE_PROFILE_BALANCED:     return "schedutil";
    case ISDE_PROFILE_PERFORMANCE:  return "performance";
    }
    return "schedutil";
}

static IsdeCpuProfile profile_from_epp(const char *epp)
{
    if (strstr(epp, "power")) {
        return ISDE_PROFILE_POWER_SAVER;
    }
    if (strcmp(epp, "performance") == 0) {
        return ISDE_PROFILE_PERFORMANCE;
    }
    return ISDE_PROFILE_BALANCED;
}

static IsdeCpuProfile profile_from_governor(const char *gov)
{
    if (strcmp(gov, "powersave") == 0) {
        return ISDE_PROFILE_POWER_SAVER;
    }
    if (strcmp(gov, "performance") == 0) {
        return ISDE_PROFILE_PERFORMANCE;
    }
    return ISDE_PROFILE_BALANCED;
}

static int read_first_line(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    if (!fgets(buf, (int)bufsz, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    return 0;
}

static int write_string(const char *path, const char *val)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    int ret = (fputs(val, f) >= 0) ? 0 : -1;
    fclose(f);
    return ret;
}

static int has_epp(const char *policy_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/energy_performance_preference", policy_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    fclose(f);
    return 1;
}

int isde_cpufreq_available(void)
{
    glob_t g;
    int ret = glob(CPUFREQ_BASE "/policy*", 0, NULL, &g);
    if (ret != 0) {
        return 0;
    }
    int found = g.gl_pathc > 0;
    globfree(&g);
    return found;
}

IsdeCpuProfile isde_cpufreq_get_profile(void)
{
    glob_t g;
    if (glob(CPUFREQ_BASE "/policy*", 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        return ISDE_PROFILE_BALANCED;
    }

    char buf[128];
    char path[512];

    /* Try EPP first on policy0 */
    if (has_epp(g.gl_pathv[0])) {
        snprintf(path, sizeof(path), "%s/energy_performance_preference",
                 g.gl_pathv[0]);
        if (read_first_line(path, buf, sizeof(buf)) == 0) {
            globfree(&g);
            return profile_from_epp(buf);
        }
    }

    /* Fall back to scaling_governor */
    snprintf(path, sizeof(path), "%s/scaling_governor", g.gl_pathv[0]);
    if (read_first_line(path, buf, sizeof(buf)) == 0) {
        globfree(&g);
        return profile_from_governor(buf);
    }

    globfree(&g);
    return ISDE_PROFILE_BALANCED;
}

int isde_cpufreq_set_profile(IsdeCpuProfile profile)
{
    glob_t g;
    if (glob(CPUFREQ_BASE "/policy*", 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        globfree(&g);
        return -1;
    }

    int use_epp = has_epp(g.gl_pathv[0]);
    const char *val = use_epp ? epp_for_profile(profile)
                              : governor_for_profile(profile);
    const char *file = use_epp ? "energy_performance_preference"
                               : "scaling_governor";
    int errors = 0;

    for (size_t i = 0; i < g.gl_pathc; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", g.gl_pathv[i], file);
        if (write_string(path, val) != 0) {
            errors++;
        }
    }

    globfree(&g);
    return errors ? -1 : 0;
}

int isde_cpufreq_available_profiles(void)
{
    /* All three profiles are always conceptually available;
     * the kernel will reject unsupported values at write time. */
    return (1 << ISDE_PROFILE_POWER_SAVER) |
           (1 << ISDE_PROFILE_BALANCED) |
           (1 << ISDE_PROFILE_PERFORMANCE);
}
