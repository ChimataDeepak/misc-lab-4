// Stable-AIDL-shaped HAL interface for the lab's modem manager.
// On a real device this is the boundary between the telephony framework (via the
// RIL / IRadio* HALs) and the vendor process that owns the modem. Here the vendor
// process is our C++ QmiClient talking QMI to modem_sim.
package com.mlab.modem;

import com.mlab.modem.IModemIndication;

@VintfStability  // vendor<->system boundary: frozen, versioned ABI
interface IModemControl {
    /** DMS GetRevision -> firmware revision string. */
    String getRevision();
    /** DMS GetIDs -> IMEI. */
    String getImei();
    /** NAS GetSignalStrength -> RSSI in dBm. */
    int getSignalDbm();
    /** Like IRadio.setResponseFunctions: framework hands the HAL its callback. */
    void setIndicationCallback(IModemIndication cb);
}
