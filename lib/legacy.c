#include "legacy.h"

#include "memory_mosq.h"
#include "mqtt_protocol.h"
#include "net_mosq.h"
#include "packet_mosq.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>

#define CMD_GET 0x0
#define PUB_FMT "{\"value\":\"%s\",\"timestamp\":%13lu,}"

struct lm {
  const char* topic;
  const char* value;

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

  *src = (dst[0] << 8) | (dst[1]);
  return dst + 2;
}

static int ltom_con(struct mosquitto__packet* pack) {
  pack->command = 0x10;  // Connect

  pack->remaining_length = 28;
  pack->payload = mosquitto__malloc(pack->remaining_length);

  uint8_t* pp = pack->payload;

  pp = set_u16(pp, 0x0004);               // Protocol length
  pp = set_all(pp, (uint8_t*)"MQTT", 4);  // Protocol
  pp = set_u08(pp, 0x05);                 // Protocol level
  pp = set_u08(pp, 0x00);                 // Options
  pp = set_u16(pp, 0xffff);               // Keep alive
  pp = set_u08(pp, 0x00);                 // Properties length
  pp = set_u16(pp, 0x000f);               // Client ID length

  sprintf((char*)pp, "vcas_%#010x", rand());  // Client ID (15)

  pack->pos = 0;

  return 0;
}

static int ltom_pub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0x31;  // Publish, DUP = 0, QoS = 0, Retain = 1

  struct json_object* obj = json_object_new_object();

  if (!obj)
    return -1;

  json_object_object_add(obj, "timestamp",
                         json_object_new_int64(msg->timestamp));

  if (msg->value) {
    double dv = strtod(msg->value, 0);

    if (dv != 0)
      json_object_object_add(obj, "value", json_object_new_double(dv));
    else
      json_object_object_add(obj, "value", json_object_new_string(msg->value));
  }

  uint64_t ps;
  uint16_t ts = strlen(msg->topic);

  const char* p =
      json_object_to_json_string_length(obj, JSON_C_TO_STRING_PLAIN, &ps);

  pack->remaining_length = 3 + ts + ps;

  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_all(pp, (uint8_t*)p, ps);

  json_object_put(obj);
  mosquitto__free(pack->payload);

  pack->payload = pl;
  pack->pos = 0;

  return 0;
}

static int ltom_sub(struct lm* msg, struct mosquitto__packet* pack) {
  pack->command = 0x82;  // Subscribe

  uint16_t ts = strlen(msg->topic);

  pack->remaining_length = 6 + ts;

  uint8_t* pl = mosquitto__malloc(pack->remaining_length);
  uint8_t* pp = pl;

  pp = set_u16(pp, 0x3654);                    // Packet identifier
  pp = set_u08(pp, 0x00);                      // Properties length
  pp = set_u16(pp, ts);                        // Topic length
  pp = set_all(pp, (uint8_t*)msg->topic, ts);  // Topic
  pp = set_u08(pp, 0x00);                      // Options

  mosquitto__free(pack->payload);
  pack->payload = pl;
  pack->pos = 3;

  return 0;
}

static int ltom_usub(struct lm* msg, struct mosquitto__packet* pack) {
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

static int ltom_get(struct lm* msg, struct mosquitto__packet* pack) {
  if (ltom_sub(msg, pack))
    return -1;

  struct mosquitto__packet* p =
      mosquitto__malloc(sizeof(struct mosquitto__packet));

  if (!p)
    return -1;

  p->payload = 0;
  p->next = 0;

  packet__cleanup(p);

  if (ltom_usub(msg, p)) {
    mosquitto__free(p);
    return -1;
  }

  pack->next = p;
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

  if (sscanf(src + 1, "%lu", &ts.tv_nsec) != 1)
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

    if (val)
      switch (key[0]) {
        case 'm':  // Method
          switch (val[0]) {
            case 'g':  // Get
              pack->command = CMD_GET;
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

  if (!msg.topic)
    return -1;

  switch (pack->command) {
    case CMD_GET:
      if (ltom_get(&msg, pack))
        return -1;

      break;
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

  struct lm msg = {0, 0, 0};
  errno = 0;

  uint16_t tl;
  uint8_t pl;
  uint8_t cb;

  uint8_t* pp = pack->payload;

  pp = get_u08(pp, NULL);

  pack->remaining_mult = 1;
  pack->remaining_length = 0;

  do {
    pp = get_u08(pp, &cb);

    pack->remaining_length += (cb & 0x7f) * pack->remaining_mult;
    pack->remaining_mult *= 0x80;
  } while ((cb & 0x80) != 0);

  pp = get_u16(pp, &tl);
  msg.topic = (char*)pp;
  pp = get_all(pp, NULL, tl);

  pack->remaining_length -= tl + 2;

  if (((pack->command & 0x06) >> 1) > 0) {
    pp = get_u16(pp, NULL);
    pack->remaining_length -= 2;
  }

  pp = get_u08(pp, &pl);
  pp = get_all(pp, NULL, pl);

  pack->remaining_length -= pl + 1;

  struct json_tokener* tok = json_tokener_new();

  if (!tok)
    return -1;

  struct json_object* obj =
      json_tokener_parse_ex(tok, (char*)pp, pack->remaining_length);

  json_tokener_free(tok);

  if (!obj)
    return -1;

  if (json_object_get_type(obj) != json_type_object) {
    json_object_put(obj);
    return -1;
  }

  struct json_object* field;

  if (!json_object_object_get_ex(obj, "timestamp", &field)) {
    json_object_put(obj);
    return -1;
  }

  if (json_object_get_type(field) != json_type_int) {
    json_object_put(obj);
    return -1;
  }

  msg.timestamp = json_object_get_uint64(field);
  msg.value = "null";

  if (json_object_object_get_ex(obj, "value", &field)) {
    switch (json_object_get_type(field)) {
      case json_type_int:
      case json_type_double:
      case json_type_boolean:
        msg.value = json_object_to_json_string(field);
        break;
      case json_type_string:
        msg.value = json_object_get_string(field);
        break;
      default:
        break;
    }
  }

  pack->remaining_length = 40 + tl + strlen(msg.value);

  char* pld = mosquitto__malloc(pack->remaining_length);
  char* p = pld;
  time_t t = msg.timestamp / 1000;

  p += sprintf(p, "name:%s|time:", msg.topic);
  strftime(p, 21, "%d.%m.%Y %H_%M_%S", localtime(&t));
  p += 19;
  p += sprintf(p, ".%03lu|val:%s\n", msg.timestamp - (t * 1000), msg.value);

  json_object_put(obj);
  mosquitto__free(pack->payload);

  pack->payload = (uint8_t*)pld;
  pack->to_process = pack->remaining_length;

  return 0;
}
