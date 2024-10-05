#ifndef LEGACY_H
#define LEGACY_H

#include "mosquitto_internal.h"

int lcy_ltom(struct mosquitto__packet* pack);
int lcy_mtol(struct mosquitto__packet* pack);

#endif  // LEGACY_H
