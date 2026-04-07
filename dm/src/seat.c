#define _POSIX_C_SOURCE 200809L
/*
 * seat.c — libseat integration for isde-dm
 */
#include "dm.h"

#include <stdio.h>
#include <libseat.h>

static void on_enable_seat(struct libseat *seat, void *userdata)
{
    (void)seat;
    Dm *dm = (Dm *)userdata;
    dm->seat_active = 1;
    fprintf(stderr, "isde-dm: seat enabled\n");
}

static void on_disable_seat(struct libseat *seat, void *userdata)
{
    Dm *dm = (Dm *)userdata;
    dm->seat_active = 0;
    fprintf(stderr, "isde-dm: seat disabled\n");
    libseat_disable_seat(seat);
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat  = on_enable_seat,
    .disable_seat = on_disable_seat,
};

int dm_seat_init(Dm *dm)
{
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_ERROR);

    dm->seat = libseat_open_seat(&seat_listener, dm);
    if (!dm->seat) {
        fprintf(stderr, "isde-dm: libseat_open_seat failed\n");
        return -1;
    }

    dm->seat_fd = libseat_get_fd(dm->seat);
    dm->seat_active = 1;

    const char *name = libseat_seat_name(dm->seat);
    fprintf(stderr, "isde-dm: opened seat '%s'\n", name ? name : "(null)");

    return 0;
}

void dm_seat_cleanup(Dm *dm)
{
    if (dm->seat) {
        libseat_close_seat(dm->seat);
        dm->seat = NULL;
        dm->seat_fd = -1;
    }
}

void dm_seat_dispatch(Dm *dm)
{
    if (dm->seat) {
        libseat_dispatch(dm->seat, 0);
    }
}
