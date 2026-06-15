#define _POSIX_C_SOURCE 200809L
/*
 * isde-power.c — battery and AC power supply monitoring via sysfs
 */
#include "isde-power.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define MAX_BATTERIES 8
#define SYSFS_PS "/sys/class/power_supply"

struct IsdePower {
    IsdeBattery batteries[MAX_BATTERIES];
    int         nbatteries;
    int         on_ac;
};

static long read_sysfs_long(const char *dir, const char *file)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    long val = -1;
    if (fscanf(f, "%ld", &val) != 1) {
        val = -1;
    }
    fclose(f);
    return val;
}

static int read_sysfs_string(const char *dir, const char *file,
                             char *buf, size_t bufsz)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
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

static IsdeBatteryStatus parse_status(const char *s)
{
    if (strcmp(s, "Charging") == 0) {
        return ISDE_BAT_CHARGING;
    }
    if (strcmp(s, "Discharging") == 0) {
        return ISDE_BAT_DISCHARGING;
    }
    if (strcmp(s, "Full") == 0) {
        return ISDE_BAT_FULL;
    }
    if (strcmp(s, "Not charging") == 0) {
        return ISDE_BAT_FULL;
    }
    return ISDE_BAT_NOT_PRESENT;
}

IsdePower *isde_power_init(void)
{
    IsdePower *pw = calloc(1, sizeof(IsdePower));
    if (pw) {
        isde_power_poll(pw);
    }
    return pw;
}

void isde_power_free(IsdePower *pw)
{
    free(pw);
}

void isde_power_poll(IsdePower *pw)
{
    pw->nbatteries = 0;
    pw->on_ac = 0;

    DIR *d = opendir(SYSFS_PS);
    if (!d) {
        return;
    }

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') {
            continue;
        }

        char dir[512];
        snprintf(dir, sizeof(dir), "%s/%s", SYSFS_PS, de->d_name);

        char type[64];
        if (read_sysfs_string(dir, "type", type, sizeof(type)) != 0) {
            continue;
        }

        if (strcmp(type, "Mains") == 0) {
            long online = read_sysfs_long(dir, "online");
            if (online > 0) {
                pw->on_ac = 1;
            }
            continue;
        }

        if (strcmp(type, "Battery") != 0) {
            continue;
        }
        if (pw->nbatteries >= MAX_BATTERIES) {
            continue;
        }

        IsdeBattery *bat = &pw->batteries[pw->nbatteries];
        memset(bat, 0, sizeof(*bat));

        long cap = read_sysfs_long(dir, "capacity");
        bat->capacity = (cap >= 0 && cap <= 100) ? (int)cap : 0;

        char status[64];
        if (read_sysfs_string(dir, "status", status, sizeof(status)) == 0) {
            bat->status = parse_status(status);
        } else {
            bat->status = ISDE_BAT_NOT_PRESENT;
        }

        bat->energy_now = read_sysfs_long(dir, "energy_now");
        bat->energy_full = read_sysfs_long(dir, "energy_full");
        bat->power_now = read_sysfs_long(dir, "power_now");

        if (bat->energy_now < 0) {
            long charge_now = read_sysfs_long(dir, "charge_now");
            long voltage = read_sysfs_long(dir, "voltage_now");
            if (charge_now >= 0 && voltage > 0) {
                bat->energy_now = charge_now * voltage / 1000000;
            }
        }
        if (bat->energy_full < 0) {
            long charge_full = read_sysfs_long(dir, "charge_full");
            long voltage = read_sysfs_long(dir, "voltage_now");
            if (charge_full >= 0 && voltage > 0) {
                bat->energy_full = charge_full * voltage / 1000000;
            }
        }
        if (bat->power_now < 0) {
            long current_now = read_sysfs_long(dir, "current_now");
            long voltage = read_sysfs_long(dir, "voltage_now");
            if (current_now >= 0 && voltage > 0) {
                bat->power_now = current_now * voltage / 1000000;
            }
        }

        pw->nbatteries++;
    }
    closedir(d);
}

int isde_power_battery_count(IsdePower *pw)
{
    return pw ? pw->nbatteries : 0;
}

const IsdeBattery *isde_power_get_battery(IsdePower *pw, int index)
{
    if (!pw || index < 0 || index >= pw->nbatteries) {
        return NULL;
    }
    return &pw->batteries[index];
}

int isde_power_on_ac(IsdePower *pw)
{
    return pw ? pw->on_ac : 0;
}
