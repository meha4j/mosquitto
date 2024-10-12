#include "pkt.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>
#include <memory_mosq.h>
#include <mqtt_protocol.h>
#include <net_mosq.h>
#include <packet_mosq.h>

#define CMD_GET 0x0

struct lm {
  const char* topic;
  const char* value;

  size_t timestamp;
};

static uint8_t* set_all(uint8_t* dst, uint8_t* src, int s) {
  memcpy(dst, src, s);
  return dst + s;
}

static uint8_t* set_fmt(uint8_t* dst, const char* fmt, ...) {
  char buf[0xffff];
  va_list args;

  va_start(args, fmt);
  size_t s = vsprintf(buf, fmt, args);
  va_end(args);

  memcpy(dst, buf, s);
  return dst + s;
}

static uint8_t* set_ltm(uint8_t* dst, size_t t) {
  return dst;
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

  *src = (dst[0] << 8) | (dst[1]);
  return dst + 2;
}

static int pkt_con(struct mosquitto__packet* pack) {
  pack->command = 0x10;

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->remaining_length = 28;
  pack->payload = mosquitto__malloc(pack->remaining_length);

  uint8_t* pp = pack->payload;

  pp = set_u16(pp, 0x0004);                 // Protocol length
  pp = set_all(pp, (uint8_t*)"MQTT", 4);    // Protocol
  pp = set_u08(pp, 0x05);                   // Protocol level
  pp = set_u08(pp, 0x00);                   // Options
  pp = set_u16(pp, 0xffff);                 // Keep alive
  pp = set_u08(pp, 0x00);                   // Properties length
  pp = set_u16(pp, 0x000f);                 // Client ID length
  pp = set_fmt(pp, "vcas_%#010x", rand());  // Client ID

  pack->pos = 0;
  return 0;
}

static int pkt_pub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0x31;

  cJSON* obj = cJSON_CreateObject();

  if (!obj) {
    errno = ENOMEM;
    return -1;
  }

  if (cJSON_AddNumberToObject(obj, "timestamp", msg->timestamp)) {
    cJSON_Delete(obj);
    return -1;
  }

  if (msg->value && cJSON_AddStringToObject(obj, "value", msg->value)) {
    cJSON_Delete(obj);
    return -1;
  }

  char* pay = cJSON_PrintUnformatted(obj);

  uint16_t ts = strlen(msg->topic);
  size_t ps = strlen(pay);

  pack->remaining_length = 3 + ts + ps;

  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_all(pp, (uint8_t*)pay, ps);         // Payload

  free(pay);
  cJSON_Delete(obj);

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->payload = pl;
  pack->pos = 0;

  return 0;
}

static int pkt_sub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0x82;

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = 6 + ts;

  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, 0x3654);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Options

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->payload = pl;
  pack->pos = 3;

  return 0;
}

static int pkt_usub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0xa2;

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = 5 + ts;

  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, 0x3654);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->payload = pl;
  pack->pos = 2;

  return 0;
}

static int pkt_get(struct lm* msg, struct mosquitto__packet* pack) {
  if (pkt_sub(msg, pack))
    return -1;

  struct mosquitto__packet* p =
      mosquitto__malloc(sizeof(struct mosquitto__packet));

  if (!p) {
    errno = ENOMEM;
    return -1;
  }

  p->payload = 0;
  p->next = 0;

  packet__cleanup(p);

  if (pkt_usub(msg, p)) {
    mosquitto__free(p);
    return -1;
  }

  pack->next = p;
  return 0;
}

static int ltm_get(size_t* dst, char* src) {
  struct tm tp;

  if (!strptime(src, "%d.%m.%Y %H_%M_%S", &tp)) {
    errno = EPROTO;
    return -1;
  }

  if (!src) {
    errno = EPROTO;
    return -1;
  }

  int ms;

  if (sscanf(src + 1, "%d", &ms) != 1) {
    errno = EPROTO;
    return -1;
  }

  *dst = mktime(&tp) * 1000 + ms;
  return 0;
}

static int ltm_now(size_t* dst) {
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  *dst = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

  return 0;
}

int pkt_ltom(struct mosquitto__packet* pack) {
  if (pack->command == CMD_CONNECT)
    return pkt_con(pack);

  struct lm msg = {0, 0, 0};
  pack->command = 0;

  char* key = strtok((char*)pack->payload, ":");

  while (key) {
    char* val = strtok(0, "|");

    if (val)
      switch (key[0]) {
        case 'm':
          switch (val[0]) {
            case 'g':
              pack->command = CMD_GET;
              break;
            case 's':
              if (strlen(val) == 1 || val[1] == 'e')
                pack->command = CMD_PUBLISH;
              else
                pack->command = CMD_SUBSCRIBE;

              break;
            case 'r':
            case 'f':
              pack->command = CMD_UNSUBSCRIBE;
              break;
            default:
              return -1;
          };

          break;
        case 'n':
          msg.topic = val;
          break;
        case 't':
          if (strlen(key) > 1 && key[1] == 'i')
            if (ltm_get(&msg.timestamp, val))
              return -1;

          break;
        case 'v':
          msg.value = val;
          break;
      }

    key = strtok(0, ":");
  }

  if (!msg.timestamp)
    if (ltm_now(&msg.timestamp))
      return -1;

  if (!msg.topic) {
    errno = EPROTO;
    return -1;
  }

  switch (pack->command) {
    case CMD_GET:
      if (pkt_get(&msg, pack))
        return -1;

      break;
    case CMD_PUBLISH:
      if (pkt_pub(&msg, pack))
        return -1;

      break;
    case CMD_SUBSCRIBE:
      if (pkt_sub(&msg, pack))
        return -1;

      break;
    case CMD_UNSUBSCRIBE:
      if (pkt_usub(&msg, pack))
        return -1;

      break;
    default:
      errno = EPROTO;
      return -1;
  }

  return 0;
}

int pkt_mtol(struct mosquitto__packet* pack) {
  if ((pack->command & 0xf0) != CMD_PUBLISH)
    return -1;

  struct lm msg = {0, 0};
  uint8_t* pp = get_u08(pack->payload, NULL);

  pack->remaining_mult = 1;
  pack->remaining_length = 0;

  uint8_t cb;

  do {
    pp = get_u08(pp, &cb);

    pack->remaining_length += (cb & 0x7f) * pack->remaining_mult;
    pack->remaining_mult *= 0x80;
  } while ((cb & 0x80) != 0);

  uint16_t tl;

  pp = get_u16(pp, &tl);
  msg.topic = (char*)pp;
  pp = get_all(pp, NULL, tl);

  pack->remaining_length -= tl + 2;

  if (((pack->command & 0x06) >> 1) > 0) {
    pp = get_u16(pp, NULL);
    pack->remaining_length -= 2;
  }

  uint8_t pl;

  pp = get_u08(pp, &pl);
  pp = get_all(pp, NULL, pl);

  pack->remaining_length -= pl + 1;

  cJSON* obj = cJSON_ParseWithLength((char*)pp, pack->remaining_length);

  if (!obj)
    return -1;

  cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(obj, "timestamp");

  if (!timestamp) {
    errno = EPROTO;
    cJSON_Delete(obj);
    return -1;
  }

  if (!cJSON_IsNumber(timestamp)) {
    errno = EPROTO;
    cJSON_Delete(obj);
    cJSON_Delete(timestamp);
    return -1;
  }

  msg.timestamp = timestamp->valuedouble;
  msg.value = "null";

  cJSON_Delete(timestamp);
  cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, "value");

  int alloc = 0;

  if (value) {
    if (cJSON_IsString(value))
      msg.value = cJSON_GetStringValue(value);
    else if (cJSON_IsNumber(value)) {
      msg.value = cJSON_PrintUnformatted(value);
      alloc = 1;
    }
  }

  pack->remaining_length = 40 + tl + strlen(msg.value);

  uint8_t* pld = mosquitto__malloc(pack->remaining_length);
  pp = pld;

  pp = set_fmt(pp, "name:%s", msg.topic);
  pp = set_all(pp, (uint8_t*)"|type:rw", 8);
  pp = set_all(pp, (uint8_t*)"|units:-", 8);
  pp = set_ltm(pp, msg.timestamp);
  pp = set_fmt(pp, "|val:%s\n", msg.value);

  if (alloc)
    free((char*)msg.value);

  cJSON_Delete(obj);
  mosquitto__free(pack->payload);

  pack->payload = (uint8_t*)pld;
  pack->to_process = pack->remaining_length;

  return 0;
}
