/*
 * isde-cpufreq.h — CPU performance profile get/set
 */
#ifndef ISDE_CPUFREQ_H
#define ISDE_CPUFREQ_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IsdeCpuProfile {
    ISDE_PROFILE_POWER_SAVER,
    ISDE_PROFILE_BALANCED,
    ISDE_PROFILE_PERFORMANCE
} IsdeCpuProfile;

/* Returns 1 if any cpufreq policy dir exists. */
int             isde_cpufreq_available(void);

/* Get the current profile.  Returns ISDE_PROFILE_BALANCED if unreadable. */
IsdeCpuProfile  isde_cpufreq_get_profile(void);

/* Set profile across all policy dirs.  Returns 0 on success. */
int             isde_cpufreq_set_profile(IsdeCpuProfile profile);

/* Bit mask of available profiles (1 << ISDE_PROFILE_*). */
int             isde_cpufreq_available_profiles(void);

#ifdef __cplusplus
}
#endif

#endif /* ISDE_CPUFREQ_H */
