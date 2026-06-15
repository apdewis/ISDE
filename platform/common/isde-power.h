/*
 * isde-power.h — battery and AC power supply monitoring via sysfs
 */
#ifndef ISDE_POWER_H
#define ISDE_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IsdeBatteryStatus {
    ISDE_BAT_CHARGING,
    ISDE_BAT_DISCHARGING,
    ISDE_BAT_FULL,
    ISDE_BAT_NOT_PRESENT
} IsdeBatteryStatus;

typedef struct IsdeBattery {
    int                capacity;      /* 0–100 percent */
    IsdeBatteryStatus  status;
    long               energy_now;    /* microWh (or converted from microAh) */
    long               energy_full;   /* microWh */
    long               power_now;     /* microW  */
} IsdeBattery;

typedef struct IsdePower IsdePower;

IsdePower *isde_power_init(void);
void       isde_power_free(IsdePower *pw);

void       isde_power_poll(IsdePower *pw);

int        isde_power_battery_count(IsdePower *pw);
const IsdeBattery *isde_power_get_battery(IsdePower *pw, int index);

int        isde_power_on_ac(IsdePower *pw);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_POWER_H */
