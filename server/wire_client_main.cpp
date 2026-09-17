// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-wire-client: the reference client for prometheiad (or any
// server speaking Astrolog's ephemeris protocol, version 3). Sends HELLO and
// one REQUEST, collects the DATA chunks and prints one line per object per
// row. docs/SERVER.md.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ephproto.h"
#include "ws_client.hpp"

using namespace prometheia;
using namespace prometheia::server;

namespace {

constexpr const char* kUsage =
    "usage: prometheia-wire-client [options]\n"
    "  --host H            default 127.0.0.1\n"
    "  --port N            default 47190\n"
    "  --obj ID            a body by wire id (repeatable)\n"
    "  --star NAME         a fixed star by name (repeatable)\n"
    "  --jd JD             first row, UT unless --tt (default 2451545.0)\n"
    "  --tt                rows are TT\n"
    "  --step SECONDS      between rows (default 86400)\n"
    "  --count N           rows (default 1)\n"
    "  --iflag HEX         the low 32 bits of iflag (wire-map numbering)\n"
    "  --sid MODE,T0,AYAN  sidereal mode and user anchor\n"
    "  --topo LON,LAT,ELV  observer site (degrees east, degrees, metres)\n"
    "  --f32               ask for float32 values\n"
    "  --chunk N           chunk-size hint (default 500)\n"
    "  --proto N           HELLO's protocol version (default 3)\n"
    "Prints: <object> <row> <retFlag> <six values, %.17g> and, per object, its\n"
    "name and error text on a '#' line. Exit 2 on a server ERROR.\n";

bool three(const char* s, double& a, double& b, double& c) {
    return std::sscanf(s, "%lf,%lf,%lf", &a, &b, &c) == 3;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = eph::kDefaultPort;
    unsigned proto = eph::kProtoVersion;
    eph::Request req;
    req.jdStart = 2451545.0;
    req.stepSeconds = 86400;
    req.nTime = 1;
    req.chunkRows = eph::kMaxChunkRows;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--host") {
            host = value();
        } else if (arg == "--port") {
            port = std::atoi(value());
        } else if (arg == "--obj") {
            eph::ObjSpec o;
            o.id = uint32_t(std::strtoul(value(), nullptr, 10));
            req.objs.push_back(o);
        } else if (arg == "--star") {
            eph::ObjSpec o;
            o.kind = eph::kObjStar;
            std::snprintf(o.name, sizeof o.name, "%s", value());
            req.objs.push_back(o);
        } else if (arg == "--jd") {
            req.jdStart = std::strtod(value(), nullptr);
        } else if (arg == "--tt") {
            req.iflag |= eph::kIflagTimeTT;
        } else if (arg == "--step") {
            req.stepSeconds = uint32_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--count") {
            req.nTime = uint32_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--iflag") {
            req.iflag |= std::strtoull(value(), nullptr, 16) & 0xFFFFFFFFull;
        } else if (arg == "--sid") {
            double mode = 0.0;
            if (!three(value(), mode, req.sidT0, req.sidAyanOff)) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            req.sidMode = int32_t(mode);
        } else if (arg == "--topo") {
            if (!three(value(), req.topoLon, req.topoLat, req.topoElv)) {
                std::fputs(kUsage, stderr);
                return 2;
            }
        } else if (arg == "--f32") {
            req.precision = eph::kPrecF32;
        } else if (arg == "--chunk") {
            req.chunkRows = uint32_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--proto") {
            proto = unsigned(std::strtoul(value(), nullptr, 10));
        } else {
            std::fprintf(stderr, "bad option %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
    if (req.objs.empty()) {
        std::fprintf(stderr, "no objects\n%s", kUsage);
        return 2;
    }

    WsClient ws;
    if (auto r = ws.connect(host, port); !r) {
        std::fprintf(stderr, "%s\n", r.error().message.c_str());
        return 1;
    }
    const auto fail = [](const Error& e) {
        std::fprintf(stderr, "%s\n", e.message.c_str());
        return 1;
    };

    uint8_t hello[eph::kHelloMaxSize];
    uint32_t len = 0;
    eph::buildHello(hello, eph::kCapFloat32, 1, "prometheia-wire-client/0.1.0", &len, nullptr,
                    uint8_t(proto));
    if (auto r = ws.send(eph::makeMessage(eph::kMsgHello, 1, hello, len, 0, uint8_t(proto))); !r) {
        return fail(r.error());
    }
    std::vector<uint8_t> payload;
    eph::buildRequest(&payload, req);
    const uint32_t request_id = 2;
    if (auto r = ws.send(eph::makeMessage(eph::kMsgRequest, request_id, payload.data(),
                                          payload.size(), 0, uint8_t(proto)));
        !r) {
        return fail(r.error());
    }

    const auto n_obj = uint32_t(req.objs.size());
    std::vector<double> cols(size_t(n_obj) * req.nTime * 6, NAN);
    std::vector<int32_t> ret(n_obj, -1);
    uint32_t rows_seen = 0, chunk_expected = 0;
    while (rows_seen < req.nTime) {
        auto msg = ws.receive();
        if (!msg) {
            return fail(msg.error());
        }
        const std::vector<uint8_t>& m = msg.value();
        eph::Envelope env{};
        if (m.size() < eph::kEnvelopeSize || !eph::parseEnvelope(m.data(), &env) ||
            env.payloadLen != m.size() - eph::kEnvelopeSize) {
            std::fprintf(stderr, "malformed message from the server\n");
            return 1;
        }
        const uint8_t* p = m.data() + eph::kEnvelopeSize;
        if (env.type == eph::kMsgWelcome) {
            eph::Welcome w;
            if (eph::parseWelcome(p, env.payloadLen, &w)) {
                std::printf("# WELCOME %s protocol %u maxCells %u\n", w.serverVersion.c_str(),
                            w.protoVersion, w.maxCells);
            }
            continue;
        }
        if (env.type == eph::kMsgError) {
            eph::ErrorMsg e;
            if (eph::parseError(p, env.payloadLen, &e)) {
                std::fprintf(stderr, "ERROR %d: %s\n", e.code, e.text.c_str());
            }
            return 2;
        }
        if (env.type != eph::kMsgData || env.requestId != request_id) {
            continue;
        }
        eph::Reader rd(p, env.payloadLen);
        const uint32_t chunk = rd.u32(), i_time = rd.u32(), rows = rd.u32();
        const uint8_t precision = rd.u8();
        if (chunk != chunk_expected++ || i_time != rows_seen || rd.u32() != n_obj ||
            i_time + rows > req.nTime) {
            std::fprintf(stderr, "out-of-order or mismatched DATA chunk\n");
            return 1;
        }
        for (uint32_t o = 0; o < n_obj; ++o) {
            char serr[eph::kSerrMax + 1] = {0}, name[eph::kMetaNameMax + 1] = {0};
            ret[o] = rd.i32();
            rd.i32();
            rd.raw(serr, eph::kSerrMax);
            rd.raw(name, eph::kMetaNameMax);
            if (chunk == 0) {
                std::printf("# object %u name \"%s\" retFlag %d serr \"%s\"\n", o, name, ret[o],
                            serr);
            }
        }
        for (uint32_t o = 0; o < n_obj; ++o) {
            for (uint32_t k = 0; k < rows * 6; ++k) {
                cols[(size_t(o) * req.nTime + i_time) * 6 + k] =
                    precision == eph::kPrecF32 ? double(rd.f32()) : rd.f64();
            }
        }
        if (!rd.ok() || rd.left() != 0) {
            std::fprintf(stderr, "DATA chunk size mismatch\n");
            return 1;
        }
        rows_seen += rows;
    }
    for (uint32_t o = 0; o < n_obj; ++o) {
        for (uint32_t r = 0; r < req.nTime; ++r) {
            const double* v = &cols[(size_t(o) * req.nTime + r) * 6];
            std::printf("%u %u %d %.17g %.17g %.17g %.17g %.17g %.17g\n", o, r, ret[o], v[0], v[1],
                        v[2], v[3], v[4], v[5]);
        }
    }
    return 0;
}
