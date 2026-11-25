// main/mrrc_prov.h — first-time setup over the device's own access point.
#pragma once

/* Starts a SoftAP plus a one-page form and does not return: saving reboots
 * into station mode.  Entered either when no credentials exist at all, or when
 * a 3 s UP hold arms the setup flag and the next boot honours it. */
void mrrc_prov_run(void);
