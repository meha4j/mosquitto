#ifndef LCY_PKT_H
#define LCY_PKT_H

#include "mosquitto_internal.h"

int pkt_ltom(struct mosquitto__packet* pack);
int pkt_mtol(struct mosquitto__packet* pack);

#endif  // LCY_PKT_H
