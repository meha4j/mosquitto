#include "legacy.h"

#include "memory_mosq.h"
#include "mqtt_protocol.h"
#include "net_mosq.h"

#include <stdio.h>
#include <stdlib.h>

#define PAY_CON 28
#define PAY_PUB 42
#define PAY_SUB 6
#define PAY_USUB 28

#define PUB_FMT "{\"value\":\"%s\",\"timestamp\":%13lu,}"

struct lm {
  char* topic;
  char* value;

  size_t timestamp;
};

static uint8_t* set_all(uint8_t* dst, uint8_t* src, int s) {
  memcpy(dst, src, s);
  return dst + s;
}

static uint8_t* set_u08(uint8_t* dst, uint8_t src) {
  dst[0] = src;
  return dst + 1;
}

static uint8_t* set_u16(uint8_t* dst, uint16_t src) {
  dst[0] = MOSQ_MSB(src);
  dst[1] = MOSQ_LSB(src);
  return dst + 2;
}

static uint8_t* set_u32(uint8_t* dst, uint32_t src) {
  memcpy(dst, &src, 4);
  return dst + 4;
}

static uint8_t* get_all(uint8_t* dst, uint8_t* src, int s) {
  if (!src)
    return dst + s;

  memcpy(src, dst, s);
  return dst + s;
}

static uint8_t* get_u08(uint8_t* dst, uint8_t* src) {
  if (!src)
    return dst + 1;

  *src = dst[0];
  return dst + 1;
}

static uint8_t* get_u16(uint8_t* dst, uint16_t* src) {
  if (!src)
    return dst + 2;

  *src = ((uint16_t)dst[0] << 8) & ((uint16_t)dst[1]);
  return dst + 2;
}

static int ltom_con(struct mosquitto__packet* pack) {
  pack->command = 0x10;  // Connect

  pack->remaining_length = PAY_CON;
  pack->payload = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pack->payload;

  pp = set_u16(pp, 0x0004);               // Protocol length
  pp = set_all(pp, (uint8_t*)"MQTT", 4);  // Protocol
  pp = set_u08(pp, 0x05);                 // Protocol level
  pp = set_u08(pp, 0x00);                 // Options
  pp = set_u16(pp, 0xffff);               // Keep alive
  pp = set_u08(pp, 0x00);                 // Properties length
  pp = set_u16(pp, 0x000f);               // Client ID length

  sprintf((char*)pp, "vcas_%#010x", rand());  // Client ID

  pack->pos = 0;

  return 0;
}

static int ltom_pub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0x35;  // Publish, DUP = 0, QoS = 2, Retain = 1

  uint16_t ts = strlen(msg->topic);
  uint16_t vs = strlen(msg->value);

  pack->remaining_length = PAY_PUB + ts + vs;
  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Properties length

  sprintf((char*)pp, PUB_FMT, msg->value, msg->timestamp);  // Payload

  mosquitto__free(pack->payload);
  pack->payload = pl;
  pack->pos = 0;

  return 0;
}

static int ltom_sub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0x82;  // Subscribe

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = PAY_SUB + ts;
  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, 0x05be);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x02);                      // Options

  mosquitto__free(pack->payload);
  pack->payload = pl;
  pack->pos = 3;

  return 0;
}

static int ltom_usub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0xa2;

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = PAY_SUB + ts;
  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, 0x05be);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic

  mosquitto__free(pack->payload);
  pack->payload = pl;
  pack->pos = 2;

  return 0;
}

static int tm_parse(size_t* dst, char* src) {
  struct tm tm;
  struct timespec ts;

  if (!strptime(src, "%d.%m.%Y %H_%M_%S", &tm))
    return -1;

  ts.tv_sec = mktime(&tm);

  if (!src)
    return -1;

  if (sscanf(src, "%lu", &ts.tv_nsec) != 1)
    return -1;

  *dst = ts.tv_sec * 1000 + ts.tv_nsec;

  return 0;
}

static int tm_now(size_t* dst) {
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  *dst = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

  return 0;
}

int lcy_ltom(struct mosquitto__packet* pack) {
  if (pack->command == CMD_CONNECT)
    return ltom_con(pack);

  pack->command = 0;

  struct lm msg = {0, 0, 0};
  char* key = strtok((char*)pack->payload, ":");

  while (key) {
    char* val = strtok(0, "|");

    if (!val)
      return -1;

    switch (key[0]) {
      case 'm':  // Method
        switch (val[0]) {
          case 'g':  // Get
            pack->command = CMD_SUBSCRIBE;
            break;
          case 's':
            if (strlen(val) == 1 || val[1] == 'e')  // Publish
              pack->command = CMD_PUBLISH;
            else  // Subscribe
              pack->command = CMD_SUBSCRIBE;

            break;
          case 'r':  // Unsubscribe
          case 'f':
            pack->command = CMD_UNSUBSCRIBE;
            break;
          default:
            return -1;
        };

        break;
      case 'n':  // Topic
        msg.topic = val;
        break;
      case 't':  // Time
        if (tm_parse(&msg.timestamp, val))
          return -1;

        break;
      case 'v':  // Value
        msg.value = val;
        break;
    }

    key = strtok(0, ":");
  }

  if (!msg.timestamp)
    if (tm_now(&msg.timestamp))
      return -1;

  switch (pack->command) {
    case CMD_PUBLISH:
      if (ltom_pub(&msg, pack))
        return -1;

      break;
    case CMD_SUBSCRIBE:
      if (ltom_sub(&msg, pack))
        return -1;

      break;
    case CMD_UNSUBSCRIBE:
      if (ltom_usub(&msg, pack))
        return -1;

      break;
    default:
      return -1;
  }

  return 0;
}

int lcy_mtol(struct mosquitto__packet* pack) {
  if ((pack->command & 0xf0) != CMD_PUBLISH)
    return -1;

  uint8_t* pp = pack->payload;
  uint16_t tl;
  uint8_t pl;

  struct lm msg = {0, 0, 0};

  pp = get_u16(pp, &tl);

  msg.topic = (char*)pp;

  pp = get_all(pp, NULL, tl);
  pp = get_u08(pp, &pl);
  pp = get_all(pp, NULL, pl);

  char* key = strtok((char*)pp, ":");

  while (key) {
    char* val = strtok(0, ",");

    if (!val)
      return -1;

    if (strstr(key, "val")) {
      char* c = strchr(val, '"');

      if (c) {
        msg.value = c + 1;
        strchr(c + 1, '"')[0] = '\n';
      } else
        msg.value = val;
    } else if (strstr(key, "time"))
      if (sscanf(val, "%lu", &msg.timestamp) != 1)
        return -1;

    key = strtok(0, ":");
  }

  if (!msg.timestamp)
    return -1;

  char* pld = mosquitto__malloc(39 + strlen(msg.topic) + strlen(msg.value));
  char* p = pld;
  time_t t = msg.timestamp / 1000;

  p += sprintf(p, "name:%s|time:", msg.topic);
  p += strftime(p, 19, "%d.%m.%Y %H_%M_%S", localtime(&t));
  p += sprintf(p, ".%3lu|val:%s", msg.timestamp - (t * 1000), msg.value);

  mosquitto__free(pack->payload);

  pack->payload = (uint8_t*)pld;

  return 0;
}
