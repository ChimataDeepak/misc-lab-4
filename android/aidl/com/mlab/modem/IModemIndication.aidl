package com.mlab.modem;

// Unsolicited events, HAL -> framework (like IRadioNetworkIndication).
// oneway: the HAL must never block on the framework.
@VintfStability
oneway interface IModemIndication {
    void signalChanged(int dbm);
}
