/*
 * sim_core.h -- the "brain" of the simulated modem: request in, response out.
 * Pure functions, no sockets, so they can be unit tested directly.
 */
#ifndef MLAB_SIM_CORE_H
#define MLAB_SIM_CORE_H

#include "qmi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_REVISION "MLAB-SIM-MODEM 1.0.0 (simulated, not real firmware)"
#define SIM_ESN      "80ABCDEF"
#define SIM_IMEI     "490154203237518"
#define SIM_MEID     "A1000012345678"

typedef struct {
    uint8_t next_cid[4];        /* per service index (CTL..NAS) */
    uint8_t allocated[4][32];   /* bitmap of handed-out client ids */
    int8_t signal_dbm;          /* current "RSSI" */
    uint16_t ind_txn;           /* indications are not replies; we still number them */
} sim_state;

void sim_init(sim_state *s);
int sim_cid_allocated(const sim_state *s, uint8_t service, uint8_t cid);

/* Handles one parsed request. Writes the response into out.
 * Returns response length (>0), 0 if no response is due, <0 on encode error. */
ssize_t sim_handle(sim_state *s, const qmi_msg *req, uint8_t *out, size_t cap);

/* Builds a NAS Event Report indication (TLV 0x10 = {int8 dBm, u8 radio if}). */
ssize_t sim_build_signal_indication(sim_state *s, uint8_t nas_cid, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
