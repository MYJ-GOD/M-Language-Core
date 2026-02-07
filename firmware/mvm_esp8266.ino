/*
 * Minimal M-Token VM for NodeMCU (ESP8266)
 * Hardware:
 *   - Water sensor: AO -> A0
 *   - DHT11: DATA -> D4 (GPIO2)
 *   - Relay1: IN1 -> D1 (GPIO5)
 *   - Relay2: IN2 -> D2 (GPIO4)
 *
 * Serial protocol:
 *   [4-byte LE length][varint bytecode...]
 * Response:
 *   OK:    [result varint][steps varint][0x01]
 *   FAULT: [fault varint][pc varint][0x00]
 */

#include <Arduino.h>
#include <DHT.h>

// Pins
static const int PIN_WATER_A0 = A0;
static const int PIN_DHT = D4;   // GPIO2
static const int PIN_RELAY1 = D1; // GPIO5
static const int PIN_RELAY2 = D2; // GPIO4

// DHT
static const int DHTTYPE = DHT11;
static DHT dht(PIN_DHT, DHTTYPE);

// M-Token opcodes (subset)
static const uint32_t OP_B      = 10;
static const uint32_t OP_E      = 11;
static const uint32_t OP_IF     = 12;
static const uint32_t OP_WH     = 13;
static const uint32_t OP_FN     = 15;
static const uint32_t OP_RT     = 16;
static const uint32_t OP_CL     = 17;
static const uint32_t OP_PH     = 18;

static const uint32_t OP_LIT    = 30;
static const uint32_t OP_V      = 31;
static const uint32_t OP_LET    = 32;
static const uint32_t OP_SET    = 33;

static const uint32_t OP_LT     = 40;
static const uint32_t OP_GT     = 41;
static const uint32_t OP_LE     = 42;
static const uint32_t OP_GE     = 43;
static const uint32_t OP_EQ     = 44;

static const uint32_t OP_ADD    = 50;
static const uint32_t OP_SUB    = 51;
static const uint32_t OP_MUL    = 52;
static const uint32_t OP_DIV    = 53;
static const uint32_t OP_AND    = 54;
static const uint32_t OP_OR     = 55;
static const uint32_t OP_XOR    = 56;
static const uint32_t OP_SHL    = 57;
static const uint32_t OP_SHR    = 58;

static const uint32_t OP_LEN    = 60;
static const uint32_t OP_GET    = 61;
static const uint32_t OP_PUT    = 62;
static const uint32_t OP_SWP    = 63;

static const uint32_t OP_DUP    = 64;
static const uint32_t OP_DRP    = 65;
static const uint32_t OP_ROT    = 66;

static const uint32_t OP_IOW    = 70;
static const uint32_t OP_IOR    = 71;

static const uint32_t OP_GTWAY  = 80;
static const uint32_t OP_WAIT   = 81;
static const uint32_t OP_HALT   = 82;
static const uint32_t OP_TRACE  = 83;

static const uint32_t OP_JMP    = 100;
static const uint32_t OP_JZ     = 101;
static const uint32_t OP_JNZ    = 102;

static const uint32_t OP_MOD    = 110;
static const uint32_t OP_NEG    = 111;
static const uint32_t OP_NOT    = 112;
static const uint32_t OP_NEQ    = 113;

// Fault codes (align with host enum where possible)
enum FaultCode {
  FAULT_NONE = 0,
  FAULT_STACK_OVERFLOW = 1,
  FAULT_STACK_UNDERFLOW = 2,
  FAULT_RET_STACK_OVERFLOW = 3,
  FAULT_RET_STACK_UNDERFLOW = 4,
  FAULT_LOCALS_OOB = 5,
  FAULT_GLOBALS_OOB = 6,
  FAULT_PC_OOB = 7,
  FAULT_DIV_BY_ZERO = 8,
  FAULT_MOD_BY_ZERO = 9,
  FAULT_UNKNOWN_OP = 10,
  FAULT_STEP_LIMIT = 11,
  FAULT_GAS_EXHAUSTED = 12,
  FAULT_BAD_ENCODING = 13,
  FAULT_UNAUTHORIZED = 14,
  FAULT_TYPE_MISMATCH = 15,
  FAULT_INDEX_OOB = 16,
  FAULT_BAD_ARG = 17,
  FAULT_OOM = 18,
  FAULT_ASSERT_FAILED = 19,
  FAULT_BREAKPOINT = 20,
  FAULT_DEBUG_STEP = 21,
  FAULT_CALL_DEPTH_LIMIT = 22
};

// VM limits
static const int STACK_SIZE = 64;
static const int CODE_MAX = 1024;
static const uint32_t STEP_LIMIT = 20000;

// VM state
static uint8_t code_buf[CODE_MAX];
static int code_len = 0;
static int pc = 0;
static int64_t stack[STACK_SIZE];
static int sp = -1;
static uint8_t caps[32]; // 0..255
static uint32_t steps = 0;
static FaultCode fault = FAULT_NONE;

// Helpers
static void caps_clear() {
  memset(caps, 0, sizeof(caps));
}

static void caps_add(uint32_t id) {
  if (id > 255) return;
  caps[id >> 3] |= (1u << (id & 7));
}

static bool caps_has(uint32_t id) {
  if (id > 255) return false;
  return (caps[id >> 3] & (1u << (id & 7))) != 0;
}

static bool read_exact(uint8_t* dst, int n) {
  int got = 0;
  unsigned long start = millis();
  while (got < n) {
    if (Serial.available()) {
      dst[got++] = (uint8_t)Serial.read();
      start = millis();
    } else {
      if (millis() - start > 2000) return false;
      delay(1);
    }
  }
  return true;
}

static bool decode_uvarint(const uint8_t* buf, int len, int* p, uint64_t* out) {
  uint64_t res = 0;
  int shift = 0;
  int i = *p;
  while (i < len) {
    uint8_t b = buf[i++];
    res |= (uint64_t)(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *p = i;
      *out = res;
      return true;
    }
    shift += 7;
    if (shift >= 64) return false;
  }
  return false;
}

static int64_t decode_zigzag64(uint64_t n) {
  return (int64_t)((n >> 1) ^ (uint64_t)-(int64_t)(n & 1));
}

static int encode_uvarint(uint64_t v, uint8_t* out) {
  int i = 0;
  while (v > 0x7FULL) {
    out[i++] = (uint8_t)(v & 0x7F) | 0x80;
    v >>= 7;
  }
  out[i++] = (uint8_t)v;
  return i;
}

static void push(int64_t v) {
  if (sp + 1 >= STACK_SIZE) {
    fault = FAULT_STACK_OVERFLOW;
    return;
  }
  stack[++sp] = v;
}

static int64_t pop() {
  if (sp < 0) {
    fault = FAULT_STACK_UNDERFLOW;
    return 0;
  }
  return stack[sp--];
}

static int64_t read_sensor(uint32_t dev) {
  if (dev == 1) {
    return (int64_t)analogRead(PIN_WATER_A0);
  }
  if (dev == 2) {
    float t = dht.readTemperature();
    if (isnan(t)) return 0;
    return (int64_t)t;
  }
  if (dev == 3) {
    float h = dht.readHumidity();
    if (isnan(h)) return 0;
    return (int64_t)h;
  }
  return 0;
}

static void write_actuator(uint32_t dev, int64_t val) {
  int on = (val != 0) ? HIGH : LOW;
  if (dev == 5) {
    digitalWrite(PIN_RELAY1, on);
  } else if (dev == 6) {
    digitalWrite(PIN_RELAY2, on);
  }
}

static void run_vm() {
  pc = 0;
  sp = -1;
  steps = 0;
  fault = FAULT_NONE;
  caps_clear();

  while (pc < code_len && fault == FAULT_NONE) {
    if (++steps > STEP_LIMIT) {
      fault = FAULT_STEP_LIMIT;
      break;
    }

    uint64_t op = 0;
    if (!decode_uvarint(code_buf, code_len, &pc, &op)) {
      fault = FAULT_BAD_ENCODING;
      break;
    }

    switch ((uint32_t)op) {
      case OP_LIT: {
        uint64_t enc = 0;
        if (!decode_uvarint(code_buf, code_len, &pc, &enc)) {
          fault = FAULT_BAD_ENCODING;
          break;
        }
        push(decode_zigzag64(enc));
        break;
      }
      case OP_ADD: {
        int64_t b = pop();
        int64_t a = pop();
        push(a + b);
        break;
      }
      case OP_SUB: {
        int64_t b = pop();
        int64_t a = pop();
        push(a - b);
        break;
      }
      case OP_MUL: {
        int64_t b = pop();
        int64_t a = pop();
        push(a * b);
        break;
      }
      case OP_DIV: {
        int64_t b = pop();
        if (b == 0) { fault = FAULT_DIV_BY_ZERO; break; }
        int64_t a = pop();
        push(a / b);
        break;
      }
      case OP_GT: {
        int64_t b = pop();
        int64_t a = pop();
        push(a > b ? 1 : 0);
        break;
      }
      case OP_LT: {
        int64_t b = pop();
        int64_t a = pop();
        push(a < b ? 1 : 0);
        break;
      }
      case OP_EQ: {
        int64_t b = pop();
        int64_t a = pop();
        push(a == b ? 1 : 0);
        break;
      }
      case OP_NEQ: {
        int64_t b = pop();
        int64_t a = pop();
        push(a != b ? 1 : 0);
        break;
      }
      case OP_DUP: {
        if (sp < 0) { fault = FAULT_STACK_UNDERFLOW; break; }
        push(stack[sp]);
        break;
      }
      case OP_DRP: {
        (void)pop();
        break;
      }
      case OP_GTWAY: {
        uint64_t cap = 0;
        if (!decode_uvarint(code_buf, code_len, &pc, &cap)) {
          fault = FAULT_BAD_ENCODING;
          break;
        }
        caps_add((uint32_t)cap);
        break;
      }
      case OP_IOW: {
        uint64_t dev = 0;
        if (!decode_uvarint(code_buf, code_len, &pc, &dev)) {
          fault = FAULT_BAD_ENCODING;
          break;
        }
        if (!caps_has((uint32_t)dev)) {
          fault = FAULT_UNAUTHORIZED;
          break;
        }
        int64_t val = pop();
        write_actuator((uint32_t)dev, val);
        break;
      }
      case OP_IOR: {
        uint64_t dev = 0;
        if (!decode_uvarint(code_buf, code_len, &pc, &dev)) {
          fault = FAULT_BAD_ENCODING;
          break;
        }
        if (!caps_has((uint32_t)dev)) {
          fault = FAULT_UNAUTHORIZED;
          break;
        }
        push(read_sensor((uint32_t)dev));
        break;
      }
      case OP_WAIT: {
        uint64_t ms = 0;
        if (!decode_uvarint(code_buf, code_len, &pc, &ms)) {
          fault = FAULT_BAD_ENCODING;
          break;
        }
        delay((unsigned long)ms);
        break;
      }
      case OP_HALT:
        return;
      default:
        fault = FAULT_UNKNOWN_OP;
        break;
    }
  }
}

static void send_response() {
  uint8_t out[32];
  int pos = 0;
  if (fault == FAULT_NONE) {
    int64_t result = (sp >= 0) ? stack[sp] : 0;
    pos += encode_uvarint((uint64_t)result, out + pos);
    pos += encode_uvarint((uint64_t)steps, out + pos);
    out[pos++] = 0x01;
  } else {
    pos += encode_uvarint((uint64_t)fault, out + pos);
    pos += encode_uvarint((uint64_t)pc, out + pos);
    out[pos++] = 0x00;
  }
  uint32_t len = (uint32_t)pos;
  Serial.write((uint8_t*)&len, 4);
  Serial.write(out, pos);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_RELAY1, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  digitalWrite(PIN_RELAY1, LOW);
  digitalWrite(PIN_RELAY2, LOW);
  dht.begin();
}

void loop() {
  if (Serial.available() < 4) {
    delay(1);
    return;
  }

  uint8_t len_buf[4];
  if (!read_exact(len_buf, 4)) return;
  uint32_t len = (uint32_t)len_buf[0] |
                 ((uint32_t)len_buf[1] << 8) |
                 ((uint32_t)len_buf[2] << 16) |
                 ((uint32_t)len_buf[3] << 24);
  if (len == 0 || len > CODE_MAX) {
    fault = FAULT_BAD_ARG;
    pc = 0;
    steps = 0;
    send_response();
    return;
  }
  if (!read_exact(code_buf, (int)len)) {
    fault = FAULT_BAD_ENCODING;
    pc = 0;
    steps = 0;
    send_response();
    return;
  }
  code_len = (int)len;
  run_vm();
  send_response();
}

