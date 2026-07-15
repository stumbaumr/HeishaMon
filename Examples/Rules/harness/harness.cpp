/*
 * Host-side harness for the HeishaMon rules engine.
 *
 * Parses a rules.txt with the real engine (src/rules/rules.cpp), then drives
 * simulated scenarios: fires events, advances a virtual clock to elapse
 * timers, and records every @Set... command emitted to stdout so two
 * rulesets can be diffed for behavioral equivalence.
 *
 * Usage: harness <rules.txt> <scenario-name|all>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

#include <string>
#include <map>
#include <vector>
#include <algorithm>

#include "Arduino.h"   /* host shim */
#include "src/rules/rules.h"
#include "src/common/mem.h"
#include "src/common/strnicmp.h"

#include "valid_names.h"

/* ---------- host externs required by rules.h (host branch) ---------- */

static void serial_printf(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static void serial_println(const char *val) { fprintf(stderr, "%s\n", val); }
static void serial_flush(void) { }
struct serial_t Serial = { serial_printf, serial_println, serial_flush };
void *MMU_SEC_HEAP = NULL;

/* ---------- log.h implementation ---------- */

static int log_verbose = 0;
void _logprintln(const char *file, unsigned int line, char *msg) {
  if(log_verbose) fprintf(stderr, "[log] %s\n", msg);
}
void _logprintf(const char *file, unsigned int line, char *fmt, ...) {
  if(!log_verbose) return;
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[log] "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
  va_end(ap);
}
void _logprintln_P(const char *file, unsigned int line, const __FlashStringHelper *msg) {
  fprintf(stderr, "[log] %s\n", (const char *)msg);
}
void _logprintf_P(const char *file, unsigned int line, const __FlashStringHelper *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[log] "); vfprintf(stderr, (const char *)fmt, ap); fprintf(stderr, "\n");
  va_end(ap);
}

static int parsing = 0;
/* ---------- gpio shim ---------- */
static int gpio_state[64] = {0};
void digitalWrite(int pin, int state) {
  if(parsing == 0) printf("CMD gpio(%d) = %d\n", pin, state);
  if(pin >= 0 && pin < 64) gpio_state[pin] = state;
}
int digitalRead(int pin) { return (pin >= 0 && pin < 64) ? gpio_state[pin] : 0; }

/* ---------- virtual timer queue (replaces common/timerqueue.cpp) ---------- */

static long long simNow = 0;                 /* virtual seconds */
struct vtimer { long long fireAt; int nr; };
static std::vector<vtimer> vtimers;

void timerqueue_insert(int sec, int usec, int nr) {
  (void)usec;
  for(size_t i = 0; i < vtimers.size(); i++) {
    if(vtimers[i].nr == nr) {
      if(sec <= 0) vtimers.erase(vtimers.begin()+i);
      else vtimers[i].fireAt = simNow + sec;
      return;
    }
  }
  if(sec == 0) return;
  vtimers.push_back({ simNow + sec, nr });
}

/* ---------- simulated heat pump state ---------- */

static int simHour = 12, simMinute = 30;
static std::map<std::string, double> topicvals;   /* readable @topics */
static int unknown_reads = 0, unknown_names = 0;

static bool valid_at(const char *name, size_t len) {
  for(int i = 0; valid_at_names[i] != 0; i++) {
    if(strlen(valid_at_names[i]) == len && strnicmp((char *)name, (char *)valid_at_names[i], len) == 0) return true;
  }
  return false;
}

/* ---------- varstacks (copied shape from HeishaMon/rules.cpp) ---------- */

typedef struct array_t {
  const char *key;
  union { int i; float f; void *n; const char *s; } val;
  uint8_t type;
} array_t;

typedef struct varstack_t {
  struct array_t *array;
  uint16_t nr;
} varstack_t;

static struct varstack_t global_varstack = { NULL, 0 };

static struct rules_t **rules = NULL;
uint8_t nrrules = 0;

struct rule_options_t rule_options;

/* ---------- rule_options callbacks ---------- */

static int8_t is_variable(char *text, uint16_t size) {
  uint16_t i = 1;
  if(text[0] == '$' || text[0] == '#' || text[0] == '@') {
    while(i < size && (isalnum((unsigned char)text[i]) || text[i] == '_')) i++;
    if(i == 1) return -1;
    if(text[0] == '@') {
      if(!valid_at(&text[1], i-1)) {
        fprintf(stderr, "!!! unknown @name: %.*s\n", (int)i, text);
        unknown_names++;
        return -1;
      }
    }
    return i;
  }
  if(text[0] == '%') {
    if(size == 5 && strnicmp(&text[1], (char *)"hour", 4) == 0) return 5;
    if(size == 7 && strnicmp(&text[1], (char *)"minute", 6) == 0) return 7;
    if(size == 6 && strnicmp(&text[1], (char *)"month", 5) == 0) return 6;
    if(size == 4 && strnicmp(&text[1], (char *)"day", 3) == 0) return 4;
  }
  return -1;
}

static int8_t is_event(char *text, uint16_t size) {
  if(text[0] == '@') {
    if(valid_at(&text[1], size-1)) return size;
    fprintf(stderr, "!!! unknown @event: %.*s\n", (int)size, text);
    unknown_names++;
    return -1;
  }
  if(isalpha((unsigned char)text[0]) || text[0] == '_') {
    uint16_t i = 0;
    while(i < size && (isalnum((unsigned char)text[i]) || text[i] == '_' || text[i] == '#')) i++;
    if(i == size) return size;
  }
  return -1;
}

static int8_t event_cb(struct rules_t *obj, char *name) {
  int8_t nr = rule_by_name(rules, nrrules, name);
  if(nr == -1) {
    fprintf(stderr, "!!! rule block '%s' not found\n", name);
    return -1;
  }
  obj->ctx.go = rules[nr];
  rules[nr]->ctx.ret = obj;
  return 1;
}

static void rule_done_cb(struct rules_t *obj) { }

static int8_t vm_value_get(struct rules_t *obj) {
  int16_t x = 0;
  if(rules_gettop() < 1) return -1;
  if(rules_type(-1) != VCHAR) return -1;
  const char *key = rules_tostring(-1);

  if(key[0] == '%') {
    if(strnicmp((char *)&key[1], (char *)"hour", 4) == 0) { rules_pushinteger(simHour); return 0; }
    if(strnicmp((char *)&key[1], (char *)"minute", 6) == 0) { rules_pushinteger(simMinute); return 0; }
    if(strnicmp((char *)&key[1], (char *)"month", 5) == 0) { rules_pushinteger(6); return 0; }
    if(strnicmp((char *)&key[1], (char *)"day", 3) == 0) { rules_pushinteger(3); return 0; }
    rules_pushnil();
    return 0;
  }
  if(key[0] == '@') {
    std::map<std::string, double>::iterator it = topicvals.find(&key[1]);
    if(it == topicvals.end()) {
      if(parsing == 0) {
        fprintf(stderr, "!!! read of unset topic %s -> NULL\n", key);
        unknown_reads++;
      }
      rules_pushnil();
      return 0;
    }
    double v = it->second;
    if(v == (long long)v) rules_pushinteger((int)v);
    else rules_pushfloat((float)v);
    return 0;
  }

  struct varstack_t *table = NULL;
  struct array_t *array = NULL;
  if(key[0] == '$') table = (struct varstack_t *)obj->userdata;
  else if(key[0] == '#') table = &global_varstack;
  if(table == NULL) { rules_pushnil(); return 0; }
  for(x = 0; x < table->nr; x++) {
    if(strcmp(table->array[x].key, key) == 0) { array = &table->array[x]; break; }
  }
  if(array == NULL) { rules_pushnil(); return 0; }
  switch(array->type) {
    case VINTEGER: rules_pushinteger(array->val.i); return 0;
    case VFLOAT: rules_pushfloat(array->val.f); return 0;
    case VCHAR: rules_pushstring((char *)array->val.s); return 0;
    case VNULL: rules_pushnil(); return 0;
  }
  return 0;
}

static int8_t vm_value_set(struct rules_t *obj) {
  struct varstack_t *table = NULL;
  uint16_t x = 0;
  uint8_t type = 0;

  if(rules_gettop() < 2) return -1;
  type = rules_type(-1);
  if(rules_type(-2) != VCHAR ||
     (type != VINTEGER && type != VFLOAT && type != VNULL && type != VCHAR)) return -1;

  const char *key = rules_tostring(-2);

  if(key[0] == '@') {
    char payload[256] = {0};
    switch(type) {
      case VCHAR: snprintf(payload, sizeof(payload), "%s", rules_tostring(-1)); break;
      case VINTEGER: snprintf(payload, sizeof(payload), "%d", rules_tointeger(-1)); break;
      case VFLOAT: snprintf(payload, sizeof(payload), "%g", rules_tofloat(-1)); break;
      case VNULL: snprintf(payload, sizeof(payload), "NULL"); break;
    }
    if(parsing == 0) printf("CMD [t=%lld %02d:%02d] %s = %s\n", simNow, simHour, simMinute, key, payload);
    return 0;
  }

  if(key[0] == '$') {
    table = (struct varstack_t *)obj->userdata;
    if(table == NULL) {
      table = (struct varstack_t *)MALLOC(sizeof(struct varstack_t));
      memset(table, 0, sizeof(struct varstack_t));
      obj->userdata = table;
    }
  } else if(key[0] == '#') {
    table = &global_varstack;
  } else {
    return 0;
  }

  struct array_t *array = NULL;
  for(x = 0; x < table->nr; x++) {
    if(strcmp(table->array[x].key, key) == 0) { array = &table->array[x]; break; }
  }
  if(array == NULL) {
    table->array = (struct array_t *)REALLOC(table->array, sizeof(struct array_t)*(table->nr+1));
    array = &table->array[table->nr];
    memset(array, 0, sizeof(struct array_t));
    table->nr++;
    rules_ref(key);
  }
  array->key = key;
  switch(type) {
    case VINTEGER:
      if(array->type == VCHAR && array->val.s != NULL) rules_unref(array->val.s);
      array->val.i = rules_tointeger(-1); array->type = VINTEGER; break;
    case VFLOAT:
      if(array->type == VCHAR && array->val.s != NULL) rules_unref(array->val.s);
      array->val.f = rules_tofloat(-1); array->type = VFLOAT; break;
    case VCHAR: {
      uint8_t doref = 1;
      if(array->type == VCHAR && array->val.s != NULL) {
        if(strcmp(rules_tostring(-1), array->val.s) != 0) rules_unref(array->val.s);
        else doref = 0;
      }
      array->val.s = rules_tostring(-1);
      array->type = VCHAR;
      if(doref == 1) rules_ref(array->val.s);
    } break;
    case VNULL:
      if(array->type == VCHAR && array->val.s != NULL) rules_unref(array->val.s);
      array->val.n = NULL; array->type = VNULL; break;
  }
  return 0;
}

/* ---------- parse + drive ---------- */

static unsigned char *mempool = NULL;

static void rules_free_local_stacks(void) {
  for(int x = 0; x < nrrules; x++) {
    struct varstack_t *node = (struct varstack_t *)rules[x]->userdata;
    if(node != NULL) { FREE(node->array); FREE(node); }
    rules[x]->userdata = NULL;
  }
}

static int parse_rules(const char *path) {
  FILE *f = fopen(path, "rb");
  if(f == NULL) { fprintf(stderr, "cannot open %s\n", path); return -1; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);

  mempool = (unsigned char *)malloc(MEMPOOL_SIZE);
  memset(mempool, 0, MEMPOOL_SIZE);

  unsigned int txtoffset = alignedbuffer(MEMPOOL_SIZE - len - 5);
  if(fread(&mempool[txtoffset], 1, len, f) != (size_t)len) { fclose(f); return -1; }
  fclose(f);

  struct pbuf mem; memset(&mem, 0, sizeof(mem));
  struct pbuf input; memset(&input, 0, sizeof(input));
  mem.payload = mempool;
  mem.len = 0;
  mem.tot_len = MEMPOOL_SIZE;
  input.payload = &mempool[txtoffset];
  input.len = txtoffset;
  input.tot_len = len;

  int ret = 0;
  parsing = 1;
  while((ret = rule_initialize(&input, &rules, &nrrules, &mem, NULL)) == 0) {
    input.payload = &mempool[input.len];
  }
  parsing = 0;
  if(ret == -1) {
    fprintf(stderr, "PARSE ERROR\n");
    return -1;
  }
  fprintf(stderr, "parsed OK: %d rule blocks, rules memory used: %d / %d\n", nrrules, mem.len, mem.tot_len);
  return 0;
}

static int show_locals = 0;

/* mimic the device console dump: triggered rule's locals only (current firmware)
   vs all blocks' locals (proposed fix) */
static void print_local_stacks(int trigger_nr) {
  printf("LOCALS(device-current, block '%s'):\n", rules[trigger_nr]->name);
  struct varstack_t *t = (struct varstack_t *)rules[trigger_nr]->userdata;
  if(t != NULL) {
    for(uint16_t x = 0; x < t->nr; x++) {
      struct array_t *a = &t->array[x];
      if(a->type == VINTEGER) printf("  %s = %d\n", a->key, a->val.i);
      else if(a->type == VFLOAT) printf("  %s = %g\n", a->key, a->val.f);
      else if(a->type == VCHAR) printf("  %s = %s\n", a->key, a->val.s);
      else printf("  %s = NULL\n", a->key);
    }
  }
  printf("LOCALS(all blocks):\n");
  for(int r = 0; r < nrrules; r++) {
    struct varstack_t *n = (struct varstack_t *)rules[r]->userdata;
    if(n == NULL || n->nr == 0) continue;
    printf("  block '%s':\n", rules[r]->name ? rules[r]->name : "?");
    for(uint16_t x = 0; x < n->nr; x++) {
      struct array_t *a = &n->array[x];
      if(a->type == VINTEGER) printf("    %s = %d\n", a->key, a->val.i);
      else if(a->type == VFLOAT) printf("    %s = %g\n", a->key, a->val.f);
      else if(a->type == VCHAR) printf("    %s = %s\n", a->key, a->val.s);
      else printf("    %s = NULL\n", a->key);
    }
  }
}

static void fire(const char *name) {
  int8_t nr = rule_by_name(rules, nrrules, (char *)name);
  if(nr > -1) {
    int ret = rule_run(rules[nr], 0);
    if(ret != 0) fprintf(stderr, "!!! rule_run(%s) returned %d\n", name, ret);
    if(show_locals) print_local_stacks(nr);
    rules_free_local_stacks();
  }
}

static void set_topic(const char *name, double v, bool trigger) {
  topicvals[name] = v;
  if(trigger) {
    std::string ev = std::string("@") + name;
    fire(ev.c_str());
  }
}

/* advance virtual time, firing due timers in (time, id) order */
static void advance(int seconds) {
  long long target = simNow + seconds;
  for(;;) {
    int best = -1;
    for(size_t i = 0; i < vtimers.size(); i++) {
      if(vtimers[i].fireAt <= target) {
        if(best == -1 || vtimers[i].fireAt < vtimers[best].fireAt ||
           (vtimers[i].fireAt == vtimers[best].fireAt && vtimers[i].nr < vtimers[best].nr)) best = (int)i;
      }
    }
    if(best == -1) break;
    vtimer t = vtimers[best];
    vtimers.erase(vtimers.begin()+best);
    simNow = t.fireAt;
    char name[32];
    snprintf(name, sizeof(name), "timer=%d", t.nr);
    fire(name);
  }
  simNow = target;
}

/* drop a pending timer (used to silence the self-rearming master tick) */
static void drop_timer(int nr) {
  for(size_t i = 0; i < vtimers.size(); i++) {
    if(vtimers[i].nr == nr) { vtimers.erase(vtimers.begin()+i); return; }
  }
}

static void dump_globals(void) {
  /* stable order for diffing */
  std::vector<std::string> keys;
  for(uint16_t x = 0; x < global_varstack.nr; x++) keys.push_back(global_varstack.array[x].key);
  std::sort(keys.begin(), keys.end());
  for(size_t k = 0; k < keys.size(); k++) {
    for(uint16_t x = 0; x < global_varstack.nr; x++) {
      struct array_t *a = &global_varstack.array[x];
      if(keys[k] != a->key) continue;
      /* skip scratch vars that legitimately differ between variants */
      if(keys[k] == "#roomDelta" || keys[k] == "#outletDeltaHeat" || keys[k] == "#newHeatRequest") continue;
      switch(a->type) {
        case VINTEGER: printf("VAR %s = %d\n", a->key, a->val.i); break;
        case VFLOAT: printf("VAR %s = %g\n", a->key, a->val.f); break;
        case VCHAR: printf("VAR %s = %s\n", a->key, a->val.s); break;
        case VNULL: printf("VAR %s = NULL\n", a->key); break;
      }
    }
  }
}

/* ---------- scenarios ---------- */

static void seed_default_state(void) {
  set_topic("Heatpump_State", 1, false);
  set_topic("Operating_Mode_State", 0, false);
  set_topic("Outside_Temp", 10, false);
  set_topic("DHW_Temp", 44, false);
  set_topic("DHW_Target_Temp", 48, false);
  set_topic("DHW_Heat_Delta", -8, false);
  set_topic("Max_Pump_Duty", 112, false);
  set_topic("Room_Thermostat_Temp", 21, false);
  set_topic("Quiet_Mode_Level", 1, false);
  set_topic("Main_Inlet_Temp", 30, false);
  set_topic("Main_Outlet_Temp", 33, false);
  set_topic("Main_Target_Temp", 33, false);
  set_topic("Z1_Heat_Request_Temp", 0, false);
  set_topic("Z1_Cool_Request_Temp", 0, false);
  set_topic("Z1_Heat_Curve_Target_High_Temp", 40, false);
  set_topic("Z1_Heat_Curve_Target_Low_Temp", 31, false);
  set_topic("ThreeWay_Valve_State", 0, false);
  set_topic("Defrosting_State", 0, false);
  set_topic("DHW_Power_Consumption", 0, false);
  set_topic("Heat_Power_Consumption", 500, false);
}

static void master_tick_at(int hour, int minute) {
  simHour = hour; simMinute = minute;
  printf("--- tick %02d:%02d\n", hour, minute);
  fire("timer=10");
}

/* one simulated day of master ticks with interesting hours, heating profile */
static void scenario_day(double outsideTemp, double roomTemp, int opMode) {
  printf("=== scenario_day outside=%g room=%g opmode=%d\n", outsideTemp, roomTemp, opMode);
  seed_default_state();
  set_topic("Outside_Temp", outsideTemp, false);
  set_topic("Room_Thermostat_Temp", roomTemp, false);
  set_topic("Operating_Mode_State", opMode, false);
  simNow = 0; vtimers.clear();
  simHour = 7; simMinute = 0;
  fire("System#Boot");
  advance(45); /* first master tick (40s) fires inside */
  drop_timer(10);

  /* master ticks around each interesting hour; both :00 ticks like the real 30s cadence */
  int hours[] = {7, 8, 9, 13, 18, 19, 20};
  for(size_t h = 0; h < sizeof(hours)/sizeof(hours[0]); h++) {
    master_tick_at(hours[h], 0);
    drop_timer(10);
    master_tick_at(hours[h], 0);   /* second 30s tick within minute :00 */
    drop_timer(10);
    /* run any timers armed by the tick (e.g. dhw_start chain) */
    advance(700);
    master_tick_at(hours[h], 30);
    drop_timer(10);
    advance(700);
  }
  dump_globals();
}

/* the full DHW cycle incl. defrost-during-DHW and recovery chain */
static void scenario_dhw(double outsideTemp) {
  printf("=== scenario_dhw outside=%g\n", outsideTemp);
  seed_default_state();
  set_topic("Outside_Temp", outsideTemp, false);
  set_topic("DHW_Temp", 38, false);
  simNow = 0; vtimers.clear();
  simHour = 13; simMinute = 0;
  fire("System#Boot");
  advance(45);
  drop_timer(10);

  /* 13:00 tick starts DHW */
  master_tick_at(13, 0);
  drop_timer(10);
  advance(600);            /* timer=20 (35s warm / 540s cold) -> SetOperationMode */
  simMinute = 10;

  /* pump obeys: now in DHW mode, valve flips */
  set_topic("Operating_Mode_State", 4, false);
  set_topic("ThreeWay_Valve_State", 1, true);
  advance(70);             /* timer=20 (30s), timer=21 (60s) */

  /* throttle firings while making DHW */
  set_topic("Main_Target_Temp", 46, false);
  set_topic("Main_Outlet_Temp", 44.5, true);
  set_topic("Main_Outlet_Temp", 46.2, true);
  set_topic("Main_Outlet_Temp", 47.8, true);

  /* defrost kicks in mid-DHW */
  set_topic("Defrosting_State", 1, true);
  advance(70);             /* timer=22 (30s), timer=23 (60s) */
  set_topic("Operating_Mode_State", 0, false);
  /* valve flips to heating during the defrost: must NOT arm recovery */
  set_topic("ThreeWay_Valve_State", 0, true);
  advance(5);
  set_topic("Defrosting_State", 0, true);
  advance(2);              /* timer=20 (1s) */
  set_topic("Operating_Mode_State", 4, false);
  /* valve re-fires as pump returns to DHW */
  set_topic("ThreeWay_Valve_State", 1, true);
  advance(70);

  /* DHW target reached: consumption drops to 0 */
  set_topic("DHW_Temp", 49, false);
  set_topic("DHW_Power_Consumption", 0, true);
  advance(2);              /* timer=24 (1s) */

  /* pump acts on restored mode: valve back to heating */
  set_topic("Operating_Mode_State", 0, false);
  set_topic("ThreeWay_Valve_State", 0, true);
  advance(310);            /* timers 22/23/24 (240/270/300) */

  /* master tick meanwhile (13:3x, snapshot branch) */
  master_tick_at(13, 35);
  drop_timer(10);

  /* pump off: valve/defrost events must be ignored */
  printf("-- events while pump off\n");
  set_topic("Heatpump_State", 0, false);
  set_topic("ThreeWay_Valve_State", 1, true);
  set_topic("Defrosting_State", 1, true);
  set_topic("Defrosting_State", 0, true);
  set_topic("ThreeWay_Valve_State", 0, true);
  advance(400);
  set_topic("Heatpump_State", 1, false);
  dump_globals();
}

/* low-tank fallback triggers dhw_start outside schedule */
static void scenario_lowtank(void) {
  printf("=== scenario_lowtank\n");
  seed_default_state();
  set_topic("DHW_Temp", 15, false);
  set_topic("Outside_Temp", 2, false);  /* cold path: force defrost first */
  simNow = 0; vtimers.clear();
  simHour = 10; simMinute = 17;
  fire("System#Boot");
  advance(45);
  drop_timer(10);
  master_tick_at(10, 17);
  drop_timer(10);
  advance(600);            /* timer=20 after 540s */
  dump_globals();
}

/* throttle sweep across outlet deltas: heating, DHW, cooling */
static void scenario_throttle(void) {
  printf("=== scenario_throttle\n");
  seed_default_state();
  simNow = 0; vtimers.clear();
  simHour = 15; simMinute = 20;
  fire("System#Boot");
  advance(45);
  drop_timer(10);

  double deltas[] = {-2.0, -1.1, -0.4, 0.0, 0.2, 0.31, 0.9, 1.0, 1.5, 1.6, 2.4, 3.7, 5.2, 7.9};
  /* heating mode sweeps, with and without warm-room bias */
  int modes[] = {0, 2};
  for(size_t m = 0; m < sizeof(modes)/sizeof(modes[0]); m++) {
    set_topic("Operating_Mode_State", modes[m], false);
    for(int bias = 0; bias < 2; bias++) {
      set_topic("Outside_Temp", bias ? 4 : 10, false);
      set_topic("Room_Thermostat_Temp", bias ? 23 : 20, false);
      for(int req = 0; req <= 5; req += 2) {
        set_topic("Z1_Heat_Request_Temp", req, false);
        for(size_t d = 0; d < sizeof(deltas)/sizeof(deltas[0]); d++) {
          set_topic("Main_Target_Temp", 35, false);
          printf("-- throttle heat mode=%d bias=%d req=%d delta=%g\n", modes[m], bias, req, deltas[d]);
          set_topic("Main_Outlet_Temp", 35 + deltas[d], true);
        }
      }
    }
  }
  /* DHW-active branch (valve = 1, odd op mode to prove valve overrides) */
  set_topic("ThreeWay_Valve_State", 1, false);
  set_topic("Operating_Mode_State", 5, false);
  for(size_t d = 0; d < sizeof(deltas)/sizeof(deltas[0]); d++) {
    set_topic("Main_Target_Temp", 46, false);
    printf("-- throttle dhw delta=%g\n", deltas[d]);
    set_topic("Main_Outlet_Temp", 46 + deltas[d], true);
  }
  /* cooling branch */
  set_topic("ThreeWay_Valve_State", 0, false);
  set_topic("Operating_Mode_State", 1, false);
  for(int req = -2; req <= 3; req += 2) {
    set_topic("Z1_Cool_Request_Temp", req, false);
    for(size_t d = 0; d < sizeof(deltas)/sizeof(deltas[0]); d++) {
      set_topic("Main_Target_Temp", 18, false);
      printf("-- throttle cool req=%d delta=%g\n", req, deltas[d]);
      set_topic("Main_Outlet_Temp", 18 + deltas[d], true);
    }
  }
  /* quiet floor at night */
  set_topic("Operating_Mode_State", 0, false);
  master_tick_at(19, 0);
  drop_timer(10);
  set_topic("Main_Target_Temp", 35, false);
  printf("-- throttle night quiet floor\n");
  set_topic("Main_Outlet_Temp", 35.1, true);
  /* heatpump off: throttle must do nothing */
  set_topic("Heatpump_State", 0, false);
  printf("-- throttle pump off\n");
  set_topic("Main_Outlet_Temp", 36.5, true);
  set_topic("Heatpump_State", 1, false);
  dump_globals();
}

/* smart pause via Heat_Power_Consumption and morning restart */
static void scenario_pause(void) {
  printf("=== scenario_pause\n");
  seed_default_state();
  set_topic("Outside_Temp", 12, false);
  set_topic("Z1_Heat_Request_Temp", 4, false);
  simNow = 0; vtimers.clear();
  simHour = 14; simMinute = 45;
  fire("System#Boot");
  advance(45);
  drop_timer(10);

  printf("-- power drops to 0 in mild weather\n");
  set_topic("Heat_Power_Consumption", 0, true);
  /* pump obeys */
  set_topic("Heatpump_State", 0, false);

  printf("-- evening/next morning ticks\n");
  master_tick_at(18, 0); drop_timer(10);
  master_tick_at(19, 0); drop_timer(10);
  set_topic("Outside_Temp", 8, false);
  master_tick_at(8, 0); drop_timer(10);
  set_topic("Heatpump_State", 1, false);
  master_tick_at(8, 30); drop_timer(10);
  dump_globals();
}

int main(int argc, char **argv) {
  if(argc < 3) { fprintf(stderr, "usage: %s <rules.txt> <scenario|all>\n", argv[0]); return 1; }

  memset(&rule_options, 0, sizeof(rule_options));
  rule_options.is_variable_cb = is_variable;
  rule_options.is_event_cb = is_event;
  rule_options.done_cb = rule_done_cb;
  rule_options.vm_value_set = vm_value_set;
  rule_options.vm_value_get = vm_value_get;
  rule_options.event_cb = event_cb;

  if(parse_rules(argv[1]) != 0) return 2;
  if(unknown_names > 0) { fprintf(stderr, "unknown @names during parse\n"); return 3; }

  std::string s = argv[2];
  if(s == "parse") return 0;
  if(s == "locals") {
    /* one throttle firing via a topic trigger, then inspect local stacks */
    seed_default_state();
    simNow = 0; vtimers.clear(); simHour = 15; simMinute = 20;
    fire("System#Boot");
    advance(45); drop_timer(10);
    show_locals = 1;
    set_topic("Main_Target_Temp", 35, false);
    printf("== firing @Main_Outlet_Temp (calls throttle())\n");
    set_topic("Main_Outlet_Temp", 36.2, true);
    printf("== firing timer=10 directly\n");
    fire("timer=10");
    drop_timer(10);
    return 0;
  }
  if(s == "all" || s == "day") {
    scenario_day(5, 20, 0);    /* cold heating day */
    scenario_day(10, 22, 0);   /* mild heating day: night pause path */
    scenario_day(28, 25, 1);   /* hot cooling day */
    scenario_day(14, 21, 0);   /* medium day, no 8:00 duty raise */
  }
  if(s == "all" || s == "dhw") {
    scenario_dhw(10);          /* warm-weather DHW */
    scenario_dhw(2);           /* cold-weather DHW incl. forced defrost */
  }
  if(s == "all" || s == "lowtank") scenario_lowtank();
  if(s == "all" || s == "throttle") scenario_throttle();
  if(s == "all" || s == "pause") scenario_pause();

  if(unknown_reads > 0) fprintf(stderr, "WARNING: %d reads of unset topics\n", unknown_reads);
  return 0;
}
