/* BLE central for the voice assistant (N16R8) -- plan §2/§3.2.
 *
 * Role: scan for the RHC actuators (service UUID
 * c71566d8-7307-4f3e-a82f-bb0be617b22d, same strings as the
 * actuator and the plan doc), connect one link per actuator --
 * RHC-Livingroom (livingroom_light) and RHC-Bedroom
 * (bedroom_light) -- bond with LE Secure Connections (Just
 * Works, both sides NO_IO), stay connected and auto-reconnect,
 * subscribe to each state characteristic, and execute one
 * X-Command at a time, blocking for that actuator's state-notify
 * receipt (2 s). The main task calls ble_central_execute()
 * during a voice round; everything else runs on the NimBLE host
 * task via GAP callbacks.
 *
 * Name matching: the full broadcast name does not fit in the
 * 31-byte advertising packet next to the 128-bit service UUID,
 * so the actuator carries its name in the scan response and the
 * central scans actively; a scan response is trusted only when
 * the same address already advertised the RHC service UUID.
 *
 * Security note (v1): the link is bonded + encrypted, but the
 * actuator does not yet reject writes from unbonded centrals --
 * that enforcement is a hardening item, not a v1 blocker (bench,
 * single user). The server-side device registry (plan §9) is the
 * second gate for anything that reaches the backend.
 *
 * Pairing-stuck note: if either side's NVS bond is erased without
 * the other side's, re-pairing can loop on a stale key. Erasing
 * NVS on both boards clears it (idf.py erase-flash, then reflash).
 */
#pragma once

#include <stdbool.h>

/* Start the BLE central: scan, connect, bond, stay connected,
   auto-reconnect. Runs on the NimBLE host task. Call once from
   app_main (after NVS init, e.g. right after wifi_init). */
void ble_central_init(void);

/* Execute one control command from an X-Command header value, e.g.
   "livingroom_light:on" / "bedroom_light:off" /
   "livingroom_light:bri:80". Blocks the caller up to ~2 s for the
   addressed actuator's state-notify receipt. Returns true only if
   the actuator confirmed the resulting state. Unknown device, action
   or parameter => false WITHOUT transmitting anything (a physical
   action must never be guessed). */
bool ble_central_execute(const char *x_command);
