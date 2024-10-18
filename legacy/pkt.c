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
#include <mosquitto_broker.h>

#define CMD_GET 0x0
#define FMT "%d.%m.%Y %H_%M_%S"

struct lm {
  int cmd;

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
  char buf[0xff];

  time_t s = t / 1000;
  time_t ms = t - s * 1000;

  strftime(buf, 20, FMT, localtime(&s));
  sprintf(buf + 19, ".%03ld", ms);

  memcpy(dst, buf, 23);
  return dst + 23;
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

static uint8_t* get_all(uint8_t* src, uint8_t* dst, int s) {
  if (!dst)
    return src + s;

  memcpy(dst, src, s);
  return src + s;
}

static uint8_t* get_ltm(uint8_t* src, size_t* dst) {
  struct tm tm;

  if (!strptime((char*)src, FMT, &tm)) {
    errno = MOSQ_ERR_MALFORMED_PACKET;
    return 0;
  }

  size_t ms;

  if (sscanf((char*)src + 20, "%03ld", &ms) != 1) {
    errno = MOSQ_ERR_MALFORMED_PACKET;
    return 0;
  }

  *dst = mktime(&tm) * 1000 + ms;
  return src + 23;
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

static int ltm_now(size_t* dst) {
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  *dst = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

  return 0;
}

static int pkt_con(struct mosquitto__packet* pack) {
  pack->command = 0x10;

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->remaining_length = 28;
  pack->payload = mosquitto__malloc(pack->remaining_length);

  if (!pack->payload) {
    errno = MOSQ_ERR_NOMEM;
    return -1;
  }

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
    errno = MOSQ_ERR_NOMEM;
    return -1;
  }

  cJSON_AddNumberToObject(obj, "timestamp", msg->timestamp);

  if (msg->value)
    cJSON_AddStringToObject(obj, "value", msg->value);

  char* pay = cJSON_PrintUnformatted(obj);

  uint16_t ts = strlen(msg->topic);
  size_t ps = strlen(pay);

  pack->remaining_length = 3 + ts + ps;

  uint8_t* pld = mosquitto__malloc(pack->remaining_length);

  if (!pld) {
    free(pay);
    cJSON_Delete(obj);

    errno = MOSQ_ERR_NOMEM;
    return -1;
  }

  uint8_t* pp = pld;

  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_all(pp, (uint8_t*)pay, ps);         // Payload

  free(pay);
  cJSON_Delete(obj);

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->payload = pld;
  pack->pos = 0;

  return 0;
}

static int pkt_sub(struct lm* msg, struct mosquitto__packet* pack, int n) {
  pack->command = 0x82;

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = 6 + ts;

  uint8_t* pld = mosquitto__malloc(pack->remaining_length);

  if (!pld) {
    errno = MOSQ_ERR_NOMEM;
    return -1;
  }

  uint8_t* pp = pld;

  pp = set_u16(pp, 0x3654);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Options

  if (n) {
    pack->next = malloc(sizeof(struct mosquitto__packet));

    if (!pack->next) {
      mosquitto__free(pld);
      errno = MOSQ_ERR_NOMEM;
      return -1;
    }

    pack->next->payload = pack->payload;
  } else
    mosquitto__free(pack->payload);

  pack->payload = pld;
  pack->pos = 3;

  return 0;
}

static int pkt_usub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0xa2;

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = 5 + ts;

  uint8_t* pld = mosquitto__malloc(pack->remaining_length);

  if (!pld) {
    errno = MOSQ_ERR_NOMEM;
    return -1;
  }

  uint8_t* pp = pld;

  pp = set_u16(pp, 0x3654);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic

  if (pack->payload)
    mosquitto__free(pack->payload);

  pack->payload = pld;
  pack->pos = 2;

  return 0;
}

static int pkt_get(struct lm* msg, struct mosquitto__packet* pack) {
  if (pkt_sub(msg, pack, 1))
    return -1;

  pack->next->next = 0;

  if (pkt_usub(msg, pack->next)) {
    mosquitto__free(pack->next);
    return -1;
  }

  return 0;
}

int pkt_parse(char* payload, struct lm* msg) {
  char* tok = strtok(payload, "|");

  while (tok) {
    char* key = tok;
    char* val = strchr(tok, ':');

    if (val) {
      *val = 0;
      val += 1;
    } else {
      tok = strtok(0, "|");
      continue;
    }

    switch (key[0]) {
      case 'm':
        switch (val[0]) {
          case 'g':
            msg->cmd = CMD_GET;
            break;
          case 's':
            if (strlen(val) == 1 || val[1] == 'e')
              msg->cmd = CMD_PUBLISH;
            else
              msg->cmd = CMD_SUBSCRIBE;

            break;
          case 'r':
          case 'f':
            msg->cmd = CMD_UNSUBSCRIBE;
            break;
          default:
            errno = MOSQ_ERR_MALFORMED_PACKET;
            return -1;
        };

        break;
      case 'n':
        msg->topic = val;
        break;
      case 't':
        if (strlen(key) > 1 && key[1] == 'i')
          if (!get_ltm((uint8_t*)val, &msg->timestamp)) {
            errno = MOSQ_ERR_MALFORMED_PACKET;
            return -1;
          }

        break;
      case 'v':
        msg->value = val;
        break;
    }

    tok = strtok(0, "|");
  }

  return 0;
}

int pkt_ltom(struct mosquitto__packet* pack) {
  if (pack->command == CMD_CONNECT)
    return pkt_con(pack);

  printf("Parsing VCAS packet: %s\n", pack->payload);

  struct lm msg = {-1, 0, 0, 0};

  if (pkt_parse((char*)pack->payload, &msg))
    return -1;

  if (!msg.timestamp)
    if (ltm_now(&msg.timestamp))
      return -1;

  if (!msg.topic) {
    errno = MOSQ_ERR_MALFORMED_PACKET;
    return -1;
  }

  switch (msg.cmd) {
    case CMD_GET:
      if (pkt_get(&msg, pack))
        return -1;

      break;
    case CMD_PUBLISH:
      if (pkt_pub(&msg, pack))
        return -1;

      break;
    case CMD_SUBSCRIBE:
      if (pkt_sub(&msg, pack, 0))
        return -1;

      break;
    case CMD_UNSUBSCRIBE:
      if (pkt_usub(&msg, pack))
        return -1;

      break;
    default:
      errno = MOSQ_ERR_MALFORMED_PACKET;
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

  if (!obj) {
    errno = MOSQ_ERR_MALFORMED_PACKET;
    return -1;
  }

  cJSON* field = cJSON_GetObjectItemCaseSensitive(obj, "timestamp");

  if (!field) {
    cJSON_Delete(obj);
    errno = MOSQ_ERR_MALFORMED_PACKET;
    return -1;
  }

  if (!cJSON_IsNumber(field)) {
    cJSON_Delete(field);
    cJSON_Delete(obj);

    errno = MOSQ_ERR_MALFORMED_PACKET;
    return -1;
  }

  msg.timestamp = cJSON_GetNumberValue(field);
  msg.value = "null";

  field = cJSON_GetObjectItemCaseSensitive(obj, "value");

  int alloc = 0;

  if (field) {
    if (cJSON_IsString(field))
      msg.value = cJSON_GetStringValue(field);
    else if (cJSON_IsNumber(field)) {
      msg.value = cJSON_PrintUnformatted(field);
      alloc = 1;
    }
  }

  pack->remaining_length = 56 + tl + strlen(msg.value);

  uint8_t* pld = mosquitto__malloc(pack->remaining_length);

  if (!pld) {
    if (alloc)
      free((char*)msg.value);

    cJSON_Delete(obj);

    errno = MOSQ_ERR_NOMEM;
    return -1;
  }

  pp = pld;

  pp = set_fmt(pp, "name:%s", msg.topic);
  pp = set_all(pp, (uint8_t*)"|type:rw", 8);
  pp = set_all(pp, (uint8_t*)"|units:-|time:", 14);
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
