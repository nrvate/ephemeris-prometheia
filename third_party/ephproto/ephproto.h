// Ephemeris Server protocol (version 3), shared by the server (eph_srv.cpp) and the
// client. This file is the byte-level authority: EPHEMERIS_SERVER_PLAN.md
// Part I section 4 is the design authority and every layout here is copied
// from it exactly. All integers little-endian, all structs packed, no
// padding, no pointers. Compiled into both ends, so everything is inline
// and allocation-free except the convenience builders, which return
// std::vector buffers the caller owns.
//
// Wire discipline, stated once: the packed structs below exist to pin the
// layout and to static_assert the sizes. They are never cast onto wire
// bytes directly; every field moves through the explicit little-endian
// helpers (put*/get*, the Reader and the Writer), which are correct on any
// host byte order. Fixed float arrays are stored as exact IEEE-754 bits,
// so f64 payloads are bit-identical to the numbers swe_calc_ut_r returned.

#ifndef EPHPROTO_H
#define EPHPROTO_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// offsetof is <cstddef>; pull it in unqualified for the static_asserts below.
using std::size_t;

namespace eph {

// ---- 4.1 Envelope constants --------------------------------------------

inline constexpr uint16_t kMagic        = 0x1EF0;
// Version 2 (2026-09-16, EPHEMERIS_REVIEW.md S4 and S9): WELCOME carries
// maxCells, and a DATA object whose rows fail only in part keeps the rows
// that computed -- a failed row's six values are NaN.
// Version 3 (2026-09-17, EPHEMERIS_SERVER_PRODUCTION_PLAN.md Phases 3-4):
// HELLO may carry a token; ERRORs 6-8; and versions are NEGOTIATED.
//
// The compatibility rule, from version 3 on. Released clients stay in use
// for years once a release points them at a public server, so:
//   - Each end speaks every version in [kProtoMin, kProtoVersion]. The
//     envelope's version byte is the version THAT message is written in.
//   - A client's HELLO names the highest version it speaks; the server
//     answers WELCOME, and everything after it, in the lower of that and its
//     own. Below kProtoMin the server answers ERROR 8 in the client's OWN
//     envelope version, so a client too old to parse newer envelopes can
//     still read why it was refused, and closes.
//   - New fields go only at the END of a structure, and are read only when
//     the session's version has them. An existing field is never
//     reinterpreted or moved. Optional behaviour goes behind a caps bit.
//   - kProtoMin rises only deliberately, when the clients below it are
//     known to be gone; the gate keeps a kProtoMin client bit-exact
//     against the current server (tools/ephsrv-golden.sh PROTO=2).
inline constexpr uint8_t  kProtoVersion = 3;
inline constexpr uint8_t  kProtoMin     = 2;
inline constexpr uint16_t kDefaultPort  = 47190;

// Message types 1-7 are the ephemeris service messages and are never
// reused. Types 8+ are reserved for future non-ephemeris services.
enum MsgType : uint16_t {
  kMsgHello   = 1,  // C->S  first message after connect
  kMsgWelcome = 2,  // S->C  server identity and limits
  kMsgRequest = 3,  // C->S  ephemeris batch request
  kMsgData    = 4,  // S->C  one chunk of results
  kMsgError   = 5,  // S->C  whole-request error
  kMsgPing    = 6,  // both  heartbeat
  kMsgPong    = 7,  // both  heartbeat reply
};

// Envelope flags byte: bit0 zstd (reserved, deferred), bit1 float32.
inline constexpr uint8_t kEnvFlagZstd    = 0x01;
inline constexpr uint8_t kEnvFlagFloat32 = 0x02;
inline constexpr uint8_t kEnvFlagMask    = 0x03;

// HELLO/WELCOME caps bits (same meanings both directions).
inline constexpr uint32_t kCapFloat32 = 1u << 0;
inline constexpr uint32_t kCapZstd    = 1u << 1;

// 4.6 ERROR codes.
enum ErrCode : int32_t {
  kErrBad        = 1,  // bad request / parse
  kErrLimits     = 2,  // exceeds WELCOME limits
  kErrUnknown    = 3,  // unknown type
  kErrInternal   = 4,  // internal
  kErrEphemeris  = 5,  // ephemeris data (carries SWE serr text)
  // Version 3.
  kErrRateLimited = 6, // over this address's or token's cell budget; the
                       // text says when to ask again. Not a refusal of the
                       // connection: later requests are served.
  kErrToken      = 7,  // the server requires a token and this HELLO's is
                       // missing or unknown; the server closes after it
  kErrVersion    = 8,  // this client's protocol is below the server's
                       // kProtoMin; the server closes after it
};
inline constexpr int32_t kErrMax = 8;

// REQUEST precision byte.
enum Precision : uint8_t { kPrecF64 = 0, kPrecF32 = 1 };

// REQUEST object record kind byte.
enum ObjKind : uint8_t {
  kObjBody   = 0,   // a body by SWE id
  kObjStar   = 1,   // a fixed star by name
  kObjNodAps = 2,   // a node or apsis of a body: swe_nod_aps()
};

// Node/apsis point (kObjNodAps): which of swe_nod_aps()'s four answers.
enum NodApsPoint : uint8_t {
  kPntNorthNode = 1, kPntSouthNode = 2, kPntPerihelion = 3, kPntAphelion = 4,
};
// Node/apsis method (kObjNodAps): SE_NODBIT_MEAN or SE_NODBIT_OSCU.
enum NodApsMethod : uint8_t { kNodMean = 0, kNodOscu = 1 };

// Protocol-level bits in REQUEST's u64 iflag. SWE's own flags are int32
// and live in the low 32 bits, which is all the server hands to SWE; the
// high half is the protocol's. Both are stripped before any SWE call.
//
// kIflagTimeTT: jdStart and every row are TT (ET, "ephemeris time"), not
// UT, and the server calls the ET entry points (swe_calc_r, swe_calc_pctr_r,
// swe_nod_aps_r, swe_fixstar_r) with the client's instant exactly as sent.
// This is how a client that computes its own delta-t -- Astrolog does, and
// lets the user override it -- gets answers bit-identical to calling SWE
// itself: the number SWE sees is the number the client made.
inline constexpr uint64_t kIflagTimeTT = 1ull << 32;
// kIflagCenter: the center field names a swe_calc_pctr() central body
// even when it is 0 (SE_SUN). Without the bit, 0 means "no central body"
// as 4.4 has always said, and a nonzero center means pctr either way.
inline constexpr uint64_t kIflagCenter = 1ull << 33;
inline constexpr uint64_t kIflagProtoMask = kIflagTimeTT | kIflagCenter;

// The largest |jdStart| a REQUEST may carry (parseRequest).
inline constexpr double kJdAbsMax = 1e8;

// The largest object id a REQUEST may name. Swiss's center-of-body mapping
// computes ipl*100 + 9099 (sweph.c's SEFLG_CENTER_BODY branch) and indexes
// ctx->nddat[ipl]: an id whose int32 form is negative, or past this bound,
// is undefined behaviour there -- UBSan flagged both sites when the review's
// fuzz ran ComputeCell with wire ids across the whole integer range
// (EPHEMERIS_REVIEW.md S-fork). No object SWE serves comes near it:
// asteroids, the widest space, are numbered in the millions today.
inline constexpr uint32_t kObjIdMax = (0x7FFFFFFF - 9099) / 100;

// WELCOME limits (server clamps/returns kErrLimits per these).
inline constexpr uint32_t kMaxObjs       = 64;
inline constexpr uint32_t kMaxRows       = 20000;
inline constexpr uint32_t kMaxChunkRows  = 500;
inline constexpr uint32_t kMaxPayload    = 4u * 1024u * 1024u;
// The default work bound: objects x rows in one REQUEST, which the server
// computes on its loop's only thread before it can answer anyone else on
// that loop. 64 bodies x 20000 rows measured 13.7 s there; 100000 cells is
// about a second, and 2 with a missing asteroid file's lookups. A client's
// largest routine window is 64 objects x 1000 rows. The server may be
// started with another bound (--max-cells), and WELCOME says which.
inline constexpr uint32_t kMaxCellsDefault = 100000;

// Field caps the layouts fix.
inline constexpr size_t kSerrMax     = 64;   // DATA metadata serr text
inline constexpr size_t kMetaNameMax = 56;   // DATA metadata body name
inline constexpr size_t kJplFileMax  = 64;   // REQUEST jplFile field
inline constexpr size_t kObjNameMax  = 96;   // host-side cap on a star name
inline constexpr size_t kColsPerObj  = 6;    // xx[0..5]

// ---- Little-endian primitives -------------------------------------------

inline void putU16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void putU32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
inline void putU64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
inline uint16_t getU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t getU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t getU64(const uint8_t *p) {
  uint64_t u = 0;
  for (int i = 7; i >= 0; i--) u = (u << 8) | p[i];
  return u;
}
inline void putI32(uint8_t *p, int32_t v) { putU32(p, (uint32_t)v); }
inline int32_t getI32(const uint8_t *p) { return (int32_t)getU32(p); }
// Floats travel as exact IEEE-754 bits, little-endian.
inline void putF64(uint8_t *p, double d) { uint64_t u; memcpy(&u, &d, 8); putU64(p, u); }
inline double getF64(const uint8_t *p) { uint64_t u = getU64(p); double d; memcpy(&d, &u, 8); return d; }
inline void putF32(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); putU32(p, u); }
inline float getF32(const uint8_t *p) { uint32_t u = getU32(p); float f; memcpy(&f, &u, 4); return f; }

// ---- Bounds-checked Writer / Reader --------------------------------------
//
// The Writer never writes past its buffer: an overrun latches a fail bit
// and everything after is discarded, so a size computed with one call of
// each put* can never corrupt memory on a mismatch. The Reader behaves the
// same way on truncation. Both are cheap enough for the hot path; the
// per-request and per-chunk code below uses them only on headers and puts
// bulk data with raw().

class Writer {
public:
  Writer(uint8_t *buf, size_t cap) : p_(buf), cap_(cap) {}
  bool ok() const { return !over_; }
  size_t size() const { return pos_; }
  size_t capacity() const { return cap_; }

  void u8(uint8_t v)  { if (pos_ + 1 <= cap_) p_[pos_] = v; else over_ = true; pos_ += 1; }
  void u16(uint16_t v){ if (pos_ + 2 <= cap_) putU16(p_ + pos_, v); else over_ = true; pos_ += 2; }
  void u32(uint32_t v){ if (pos_ + 4 <= cap_) putU32(p_ + pos_, v); else over_ = true; pos_ += 4; }
  void u64(uint64_t v){ if (pos_ + 8 <= cap_) putU64(p_ + pos_, v); else over_ = true; pos_ += 8; }
  void i32(int32_t v) { u32((uint32_t)v); }
  void f64(double v)  { uint64_t u; memcpy(&u, &v, 8); u64(u); }
  void f32(float v)   { uint32_t u; memcpy(&u, &v, 4); u32(u); }

  void raw(const void *src, size_t n) {
    if (pos_ + n <= cap_) { memcpy(p_ + pos_, src, n); }
    else over_ = true;
    pos_ += n;
  }
  // NUL-terminated string including the terminator, truncated to maxLen
  // bytes (always NUL-terminated when maxLen > 0).
  void strZ(const char *s, size_t maxLen) {
    size_t n = s ? strlen(s) : 0;
    if (n >= maxLen) n = maxLen - 1;
    if (pos_ + n + 1 <= cap_) {
      memcpy(p_ + pos_, s ? s : "", n);
      p_[pos_ + n] = 0;
    } else over_ = true;
    pos_ += n + 1;
  }

private:
  uint8_t *p_;
  size_t cap_;
  size_t pos_ = 0;
  bool over_ = false;
};

class Reader {
public:
  Reader(const uint8_t *buf, size_t len) : p_(buf), len_(len) {}
  bool ok() const { return !bad_; }
  size_t left() const { return len_ - pos_; }
  void skip(size_t n) { if (pos_ + n > len_) { bad_ = true; pos_ = len_; } else pos_ += n; }

  uint8_t u8()   { if (pos_ + 1 <= len_) return p_[pos_++]; bad_ = true; return 0; }
  uint16_t u16() { if (pos_ + 2 <= len_) { uint16_t v = getU16(p_ + pos_); pos_ += 2; return v; } bad_ = true; return 0; }
  uint32_t u32() { if (pos_ + 4 <= len_) { uint32_t v = getU32(p_ + pos_); pos_ += 4; return v; } bad_ = true; return 0; }
  uint64_t u64() { if (pos_ + 8 <= len_) { uint64_t v = getU64(p_ + pos_); pos_ += 8; return v; } bad_ = true; return 0; }
  int32_t i32()  { return (int32_t)u32(); }
  double f64()   { uint64_t u = u64(); double d; memcpy(&d, &u, 8); return d; }
  float f32()    { uint32_t u = u32(); float f; memcpy(&f, &u, 4); return f; }

  void raw(void *dst, size_t n) {
    if (pos_ + n <= len_) { memcpy(dst, p_ + pos_, n); pos_ += n; }
    else { bad_ = true; pos_ = len_; }
  }
  // Reads to the next NUL within the buffer. Returns a pointer into the
  // input or NULL when the string is missing its terminator.
  const char *strZ(size_t *outLen = nullptr) {
    const void *nul = memchr(p_ + pos_, 0, len_ - pos_);
    if (!nul) { bad_ = true; return nullptr; }
    size_t n = (size_t)((const uint8_t *)nul - (p_ + pos_));
    const char *s = (const char *)(p_ + pos_);
    pos_ += n + 1;
    if (outLen) *outLen = n;
    return s;
  }

private:
  const uint8_t *p_;
  size_t len_;
  size_t pos_ = 0;
  bool bad_ = false;
};

// ---- 4.1 Envelope (16 bytes) --------------------------------------------

#pragma pack(push, 1)
struct EnvelopeWire {
  uint16_t magic;        // kMagic
  uint8_t  protoVersion; // kProtoVersion
  uint8_t  flags;        // bit0 zstd, bit1 float32, bits2-7 reserved 0
  uint16_t type;         // MsgType
  uint16_t reserved;     // 0
  uint32_t requestId;
  uint32_t payloadLen;   // bytes
};
#pragma pack(pop)
static_assert(sizeof(EnvelopeWire) == 16, "envelope must be exactly 16 bytes");

inline constexpr size_t kEnvelopeSize = sizeof(EnvelopeWire);

struct Envelope {
  uint8_t version;     // the version this message is written in
  uint16_t type;
  uint8_t flags;
  uint32_t requestId;
  uint32_t payloadLen;
};

inline void writeEnvelope(uint8_t *dst, uint16_t type, uint32_t requestId,
                          uint8_t flags, uint32_t payloadLen,
                          uint8_t version = kProtoVersion) {
  putU16(dst + 0, kMagic);
  dst[2] = version;
  dst[3] = (uint8_t)(flags & kEnvFlagMask);
  putU16(dst + 4, type);
  putU16(dst + 6, 0);
  putU32(dst + 8, requestId);
  putU32(dst + 12, payloadLen);
}

// Parses and validates magic + protocol version, accepting every version in
// [kProtoMin, kProtoVersion]. Returns false on a bad magic or a version
// outside the range (caller answers kErrBad, or kErrVersion when
// envelopeVersionBelowMin says the version was the problem).
inline bool parseEnvelope(const uint8_t *src, Envelope *out) {
  if (getU16(src) != kMagic) return false;
  if (src[2] < kProtoMin || src[2] > kProtoVersion) return false;
  out->version = src[2];
  out->type = getU16(src + 4);
  out->flags = src[3];
  out->requestId = getU32(src + 8);
  out->payloadLen = getU32(src + 12);
  return true;
}

// A good magic with a version below kProtoMin: a client too old to talk to,
// which is owed an ERROR it can read (kErrVersion in its own version).
inline bool envelopeVersionBelowMin(const uint8_t *src, uint8_t *version) {
  if (getU16(src) != kMagic || src[2] >= kProtoMin) return false;
  *version = src[2];
  return true;
}

// ---- 4.2/4.3 HELLO and WELCOME -------------------------------------------

#pragma pack(push, 1)
struct HelloWire {
  uint32_t protoVersion;
  uint32_t caps;
  uint32_t build;
  // sz version string (NUL-terminated)
};
struct WelcomeWire {
  uint32_t protoVersion;
  uint32_t caps;
  uint32_t swissephVersion; // packed major*10000 + minor*100 + patch
  uint32_t maxObjs;
  uint32_t maxRows;
  uint32_t maxChunkRows;
  uint32_t maxPayload;
  uint32_t maxCells;        // objects x rows per REQUEST (version 2)
  // sz serverVersion string
};
#pragma pack(pop)
static_assert(sizeof(HelloWire) == 12, "HELLO fixed part must be 12 bytes");
static_assert(sizeof(WelcomeWire) == 32, "WELCOME fixed part must be 32 bytes");

// HELLO's payload: the fixed part, the client's version string, and from
// version 3 a token string (empty for none). dst must hold
// kHelloMaxSize bytes. proto is the highest version the client speaks.
inline constexpr size_t kTokenMax = 128;
inline constexpr size_t kHelloMaxSize = sizeof(HelloWire) + 256 + kTokenMax + 1;
inline void buildHello(uint8_t *dst, uint32_t caps, uint32_t build,
                       const char *version, uint32_t *payloadLen,
                       const char *token = nullptr,
                       uint8_t proto = kProtoVersion) {
  Writer w(dst, kHelloMaxSize);
  w.u32(proto);
  w.u32(caps);
  w.u32(build);
  w.strZ(version ? version : "", 256);
  if (proto >= 3)
    w.strZ(token ? token : "", kTokenMax + 1);
  *payloadLen = (uint32_t)w.size();
}

struct Hello {
  uint32_t protoVersion;
  uint32_t caps;
  uint32_t build;
  std::string version;
  std::string token;   // version 3; empty when absent
};

inline bool parseHello(const uint8_t *p, size_t len, Hello *out) {
  if (len < sizeof(HelloWire)) return false;
  Reader r(p, len);
  out->protoVersion = r.u32();
  out->caps = r.u32();
  out->build = r.u32();
  size_t n;
  const char *s = r.strZ(&n);
  if (!r.ok() || !s) return false;
  out->version.assign(s, n);
  out->token.clear();
  // Version 3's token is optional on the wire even there: a HELLO that ends
  // after the version string simply has none.
  if (out->protoVersion >= 3 && r.left() > 0) {
    const char *t = r.strZ(&n);
    if (!r.ok() || !t || n > kTokenMax) return false;
    out->token.assign(t, n);
  }
  return true;
}

inline void buildWelcome(uint8_t *dst, uint32_t caps, uint32_t swissephVersion,
                         uint32_t maxCells, const char *serverVersion,
                         uint32_t *payloadLen, uint8_t proto = kProtoVersion) {
  Writer w(dst, sizeof(WelcomeWire) + 256);
  w.u32(proto);
  w.u32(caps);
  w.u32(swissephVersion);
  w.u32(kMaxObjs);
  w.u32(kMaxRows);
  w.u32(kMaxChunkRows);
  w.u32(kMaxPayload);
  w.u32(maxCells);
  w.strZ(serverVersion ? serverVersion : "", 256);
  *payloadLen = (uint32_t)w.size();
}

struct Welcome {
  uint32_t protoVersion;
  uint32_t caps;
  uint32_t swissephVersion;
  uint32_t maxObjs;
  uint32_t maxRows;
  uint32_t maxChunkRows;
  uint32_t maxPayload;
  uint32_t maxCells;
  std::string serverVersion;
};

inline bool parseWelcome(const uint8_t *p, size_t len, Welcome *out) {
  if (len < sizeof(WelcomeWire)) return false;
  Reader r(p, len);
  out->protoVersion = r.u32();
  out->caps = r.u32();
  out->swissephVersion = r.u32();
  out->maxObjs = r.u32();
  out->maxRows = r.u32();
  out->maxChunkRows = r.u32();
  out->maxPayload = r.u32();
  out->maxCells = r.u32();
  size_t n;
  const char *s = r.strZ(&n);
  if (!r.ok() || !s) return false;
  out->serverVersion.assign(s, n);
  return true;
}

// ---- 4.4 REQUEST ----------------------------------------------------------
//
//   u32 nObj
//   nObj object records:
//     u8 kind (0 body by id, 1 fixed star by name, 2 node/apsis of a body)
//     kind 0: u32 id
//     kind 1: sz name
//     kind 2: u32 id, u8 point (NodApsPoint), u8 method (NodApsMethod)
//   then RequestFixed (141 bytes), fields exactly as the plan lists them.

#pragma pack(push, 1)
struct RequestFixed {
  int32_t center;      // 0 = use iflag center bits; else swe_calc_pctr body id
  uint64_t iflag;      // full SWE bitmask; server ORs in SEFLG_SWIEPH
  int32_t sidMode;     // swe_set_sid_mode_r triple, only with SEFLG_SIDEREAL
  double sidT0;
  double sidAyanOff;
  double topoLon;      // east-positive degrees; only with SEFLG_TOPOCTR
  double topoLat;
  double topoElv;      // meters, as swe_set_topo() takes it
  char jplFile[64];    // swe_set_jpl_file_r; only with SEFLG_JPLEPH
  double jdStart;      // UT
  uint32_t stepSeconds;
  uint32_t nTime;      // rows
  uint8_t precision;   // 0 = f64, 1 = f32
  uint32_t chunkRows;  // client hint; server clamps to maxChunkRows
};
#pragma pack(pop)
static_assert(sizeof(RequestFixed) == 141, "REQUEST fixed part must be 141 bytes");

// One object record on the wire:
//   kind 0: 1 byte kind + 4 bytes id                    = 5 bytes
//   kind 1: 1 byte kind + strlen + 1 byte NUL           = 2 + strlen bytes
//   kind 2: 1 byte kind + 4 bytes id + point + method   = 7 bytes
inline constexpr size_t kObjRecordBodySize = 5;
inline constexpr size_t kObjRecordNodApsSize = 7;

struct ObjSpec {
  uint8_t kind = kObjBody;
  uint32_t id = 0;              // kObjBody, kObjNodAps: raw SWE id
  char name[kObjNameMax] = {0}; // kObjStar: NUL-terminated star name
  uint8_t point = 0;            // kObjNodAps: NodApsPoint
  uint8_t method = 0;           // kObjNodAps: NodApsMethod
};

// Host-side, fully decoded REQUEST. Owned by the caller (the vector is the
// one allocation the caller owns; decode fills it).
struct Request {
  std::vector<ObjSpec> objs;
  int32_t center = 0;
  uint64_t iflag = 0;
  int32_t sidMode = 0;
  double sidT0 = 0.0, sidAyanOff = 0.0;
  double topoLon = 0.0, topoLat = 0.0, topoElv = 0.0;
  char jplFile[kJplFileMax] = {0};
  double jdStart = 0.0;
  uint32_t stepSeconds = 0, nTime = 0;
  uint8_t precision = kPrecF64;
  uint32_t chunkRows = 0;
};

enum ParseResult { kParseOk = 0, kParseBad = 1, kParseLimits = 2 };

// Decodes a REQUEST payload. Well-formedness failures return kParseBad
// (ERROR code 1); values beyond the WELCOME limits return kParseLimits
// (ERROR code 2). chunkRows above kMaxChunkRows is NOT a limit failure --
// the plan defines it as a hint the server clamps -- so the caller clamps.
inline ParseResult parseRequest(const uint8_t *p, size_t len, Request *out) {
  if (len < 4) return kParseBad;
  Reader r(p, len);
  uint32_t nObj = r.u32();
  if (!r.ok()) return kParseBad;
  if (nObj == 0) return kParseBad;
  if (nObj > kMaxObjs) return kParseLimits;

  out->objs.resize(nObj);
  for (uint32_t i = 0; i < nObj; i++) {
    ObjSpec &o = out->objs[i];
    uint8_t kind = r.u8();
    if (kind == kObjBody) {
      o.kind = kObjBody;
      o.id = r.u32();
      if (o.id > kObjIdMax) return kParseBad;
    } else if (kind == kObjStar) {
      size_t n;
      const char *s = r.strZ(&n);
      if (!r.ok() || !s || n + 1 > kObjNameMax) return kParseBad;
      o.kind = kObjStar;
      memcpy(o.name, s, n + 1);
    } else if (kind == kObjNodAps) {
      o.kind = kObjNodAps;
      o.id = r.u32();
      o.point = r.u8();
      o.method = r.u8();
      if (o.point < kPntNorthNode || o.point > kPntAphelion ||
          o.method > kNodOscu || o.id > kObjIdMax)
        return kParseBad;
    } else {
      return kParseBad;
    }
  }

  out->center = r.i32();
  out->iflag = r.u64();
  out->sidMode = r.i32();
  out->sidT0 = r.f64();
  out->sidAyanOff = r.f64();
  out->topoLon = r.f64();
  out->topoLat = r.f64();
  out->topoElv = r.f64();
  r.raw(out->jplFile, kJplFileMax);
  out->jplFile[kJplFileMax - 1] = 0;  // defensive; encoder NUL-pads anyway
  out->jdStart = r.f64();
  out->stepSeconds = r.u32();
  out->nTime = r.u32();
  out->precision = r.u8();
  out->chunkRows = r.u32();
  if (!r.ok()) return kParseBad;

  if (out->nTime == 0) return kParseBad;
  // Every real the server hands Swiss must be finite, and the instant must
  // be one Swiss has an answer for. A NaN jdStart reached
  // swemmoon.c's corr_mean_node(), which indexes a table with
  // (int)floor(NaN) -- one 180-byte REQUEST crashed every loop of the
  // server -- and the library's own range checks are "<" and ">", which
  // NaN passes. 1e8 days is well past both ends of every ephemeris.
  if (!std::isfinite(out->jdStart) || std::fabs(out->jdStart) > kJdAbsMax ||
      !std::isfinite(out->sidT0) || !std::isfinite(out->sidAyanOff) ||
      !std::isfinite(out->topoLon) || !std::isfinite(out->topoLat) ||
      !std::isfinite(out->topoElv))
    return kParseBad;
  // The high half of iflag is the protocol's; a bit it does not define is
  // a malformed request, not a cache key that can never hit.
  if ((out->iflag >> 32) & ~(kIflagProtoMask >> 32)) return kParseBad;
  if (out->nTime > kMaxRows) return kParseLimits;
  if (out->precision > kPrecF32) return kParseBad;
  if (!r.ok() || r.left() != 0) return kParseBad;  // trailing bytes
  return kParseOk;
}

// Encodes a REQUEST (client side / tests). Exact inverse of parseRequest.
inline void buildRequest(std::vector<uint8_t> *out, const Request &req) {
  size_t sz = 4;
  for (const ObjSpec &o : req.objs)
    sz += (o.kind == kObjBody) ? kObjRecordBodySize :
          (o.kind == kObjNodAps) ? kObjRecordNodApsSize :
          (1 + strlen(o.name) + 1);
  sz += sizeof(RequestFixed);
  out->resize(sz);
  Writer w(out->data(), sz);
  w.u32((uint32_t)req.objs.size());
  for (const ObjSpec &o : req.objs) {
    w.u8(o.kind);
    if (o.kind == kObjBody) w.u32(o.id);
    else if (o.kind == kObjNodAps) { w.u32(o.id); w.u8(o.point); w.u8(o.method); }
    else w.strZ(o.name, kObjNameMax);
  }
  w.i32(req.center);
  w.u64(req.iflag);
  w.i32(req.sidMode);
  w.f64(req.sidT0);
  w.f64(req.sidAyanOff);
  w.f64(req.topoLon);
  w.f64(req.topoLat);
  w.f64(req.topoElv);
  w.raw(req.jplFile, kJplFileMax);
  w.f64(req.jdStart);
  w.u32(req.stepSeconds);
  w.u32(req.nTime);
  w.u8(req.precision);
  w.u32(req.chunkRows);
}

// ---- 4.5 DATA (one chunk) -------------------------------------------------
//
//   u32 chunkIndex
//   u32 iTime     (first row index in this chunk)
//   u32 nTime     (rows in this chunk)
//   u8  precision
//   u32 nObj
//   nObj DataMeta records
//   data block: object-major, nObj * nTime * 6 values (f64 or f32)
//
// The metadata is the whole window's, the same in every chunk. An object's
// rows fail one at a time (an asteroid file's range, the ephemeris edge):
// a failed row's six values are NaN, and the rows that computed are real.
// retFlag < 0 means NO row of the object computed; serr carries the first
// failed row's text whenever any row failed, so a retFlag >= 0 with a
// non-empty serr is a partial answer. (Version 1 failed the whole object
// for one failed row and zeroed its values: EPHEMERIS_REVIEW.md S9.)

#pragma pack(push, 1)
struct DataWire {
  uint32_t chunkIndex;
  uint32_t iTime;
  uint32_t nTimeRows;
  uint8_t precision;
  uint32_t nObj;
};
struct DataMetaWire {
  int32_t retFlag;    // <0: no row computed; else flags SWE actually used
  int32_t flagsUsed;
  char serr[64];      // the first failed row's SWE text, else zero-filled
  char name[56];      // body name
};
#pragma pack(pop)
static_assert(sizeof(DataWire) == 17, "DATA fixed header must be 17 bytes");
static_assert(sizeof(DataMetaWire) == 128, "per-object metadata must be 128 bytes");
static_assert(offsetof(DataMetaWire, serr) == 8, "serr field must start at byte 8");
static_assert(offsetof(DataMetaWire, name) == 72, "name field must start at byte 72");
static_assert(kSerrMax == 64 && kMetaNameMax == 56, "serr/name field caps");

inline constexpr size_t kDataHeaderSize = sizeof(DataWire);
inline constexpr size_t kDataMetaSize = sizeof(DataMetaWire);

inline size_t dataPayloadSize(size_t nObj, size_t nTimeRows, uint8_t precision) {
  size_t esz = (precision == kPrecF32) ? 4 : 8;
  return kDataHeaderSize + nObj * kDataMetaSize + nObj * nTimeRows * kColsPerObj * esz;
}

// Header + metadata + the values for one chunk, written into a
// caller-owned buffer sized by dataPayloadSize(). The values live in cols
// as f64 in the plan's object-major layout -- object o, row r, column c at
// cols[(o * totalRows + r) * 6 + c] -- and totalRows is the request's full
// row count (chunks are contiguous row ranges, but the layout is
// object-major, so the chunk's rows are gathered per object here). When
// precision is f32 the conversion happens here, at send, exactly as the
// plan says.
inline void writeDataChunk(uint8_t *dst, size_t cap,
                           uint32_t chunkIndex, uint32_t iTime,
                           uint32_t nTimeRows, uint32_t totalRows,
                           uint32_t nObj, uint8_t precision,
                           const uint8_t *meta /* nObj * 128 */,
                           const double *cols /* object-major f64 */,
                           size_t *outLen) {
  Writer w(dst, cap);
  w.u32(chunkIndex);
  w.u32(iTime);
  w.u32(nTimeRows);
  w.u8(precision);
  w.u32(nObj);
  w.raw(meta, nObj * kDataMetaSize);
  for (uint32_t o = 0; o < nObj; o++) {
    const double *src = cols + ((size_t)o * totalRows + iTime) * kColsPerObj;
    size_t n = (size_t)nTimeRows * kColsPerObj;
    if (precision == kPrecF32) {
      for (size_t i = 0; i < n; i++) w.f32((float)src[i]);
    } else {
      for (size_t i = 0; i < n; i++) w.f64(src[i]);
    }
  }
  *outLen = w.size();
}

// Just the per-object metadata record (128 bytes): retFlag, flagsUsed,
// then serr[64] and name[56], each NUL-padded and truncated to fit.
inline void writeDataMeta(uint8_t *dst, int32_t retFlag, int32_t flagsUsed,
                          const char *serr, const char *name) {
  memset(dst, 0, kDataMetaSize);
  Writer w(dst, kDataMetaSize);
  w.i32(retFlag);
  w.i32(flagsUsed);
  {
    char buf[kSerrMax] = {0};
    if (serr) {
      size_t n = strlen(serr);
      if (n >= kSerrMax) n = kSerrMax - 1;
      memcpy(buf, serr, n);
    }
    w.raw(buf, kSerrMax);
  }
  {
    char buf[kMetaNameMax] = {0};
    if (name) {
      size_t n = strlen(name);
      if (n >= kMetaNameMax) n = kMetaNameMax - 1;
      memcpy(buf, name, n);
    }
    w.raw(buf, kMetaNameMax);
  }
}

// ---- 4.6 ERROR ------------------------------------------------------------

#pragma pack(push, 1)
struct ErrorWire {
  uint32_t requestId;
  int32_t code;
  // sz text
};
#pragma pack(pop)
static_assert(sizeof(ErrorWire) == 8, "ERROR fixed part must be 8 bytes");

struct ErrorMsg {
  uint32_t requestId;
  int32_t code;
  std::string text;
};

inline bool parseError(const uint8_t *p, size_t len, ErrorMsg *out) {
  if (len < sizeof(ErrorWire)) return false;
  Reader r(p, len);
  out->requestId = r.u32();
  out->code = r.i32();
  size_t n;
  const char *s = r.strZ(&n);
  if (!r.ok() || !s) return false;
  out->text.assign(s, n);
  return true;
}

// ---- Envelope + payload builders (convenience; caller owns the vector) ----

inline std::vector<uint8_t> makeMessage(uint16_t type, uint32_t requestId,
                                        const void *payload, size_t payloadLen,
                                        uint8_t flags = 0,
                                        uint8_t version = kProtoVersion) {
  std::vector<uint8_t> msg(kEnvelopeSize + payloadLen);
  writeEnvelope(msg.data(), type, requestId, flags, (uint32_t)payloadLen,
                version);
  if (payloadLen) memcpy(msg.data() + kEnvelopeSize, payload, payloadLen);
  return msg;
}

inline std::vector<uint8_t> buildPing(uint32_t requestId) {
  return makeMessage(kMsgPing, requestId, nullptr, 0);
}
inline std::vector<uint8_t> buildPong(uint32_t requestId) {
  return makeMessage(kMsgPong, requestId, nullptr, 0);
}

// ---- FNV-1a over canonical payload bytes (5, result cache key) ------------

inline uint64_t fnv1a64(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 14695981039346656037ull;          // FNV offset basis
  const uint64_t prime = 1099511628211ull;       // FNV prime
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)p[i];
    h *= prime;
  }
  return h;
}

}  // namespace eph

#endif  // EPHPROTO_H
