/*
 * AirItlwm gated debug logging.
 *
 * Informational driver traces (state transitions, parameter echoes, progress
 * breadcrumbs) print only when the "-airitldbg" boot-arg is set. Logs that
 * report a failure stay unconditional.
 *
 * AIRITL_DBG requires XYLog (itl80211/linux/types.h) to be declared first.
 * Usable from C and C++.
 */

#ifndef AIRITL_DEBUG_H
#define AIRITL_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

extern int airitl_debug_log;

#ifdef __cplusplus
}
#endif

#define AIRITL_DBG(fmt, ...) \
    do { if (airitl_debug_log) XYLog(fmt, ##__VA_ARGS__); } while (0)

#endif /* AIRITL_DEBUG_H */
