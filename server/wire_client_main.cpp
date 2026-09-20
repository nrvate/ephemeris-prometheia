// SPDX-License-Identifier: GPL-2.0-or-later
//
// prometheia-wire-client: the reference client for prometheiad (or any
// server speaking Astrolog's ephemeris protocol, version 4). Sends HELLO and
// one REQUEST, collects the DATA chunks and prints one line per object per
// row. docs/SERVER.md.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "ephproto.h"
#include "prometheia/hypotheticals.hpp"
#include "ws_client.hpp"

using namespace prometheia;
using namespace prometheia::server;

namespace {

constexpr const char* kUsage =
    "usage: prometheia-wire-client [options]\n"
    "  --host H            default 127.0.0.1\n"
    "  --port N            default 47190\n"
    "  --obj NAIF          a body by NAIF/SPK-ID (repeatable)\n"
    "  --name NAME         a catalog body by designation or name (repeatable)\n"
    "  --star NAME         a fixed star by name (repeatable)\n"
    "  --hyp TOKEN         a named hypothetical body, kind 3 (repeatable)\n"
    "  --elements FILE     every body in a JSON Lines element file, sent as kind 4\n"
    "                      with exactly its elements (docs/HYPOTHETICALS.md)\n"
    "  --node NAIF.P[.M]   an orbit point: P = a|d|p|A (asc, desc, peri, apo),\n"
    "                      M = m (mean, default), o (osculating) or 0..4 (A.14)\n"
    "  --jd JD             first row, TT (default 2451545.0)\n"
    "  --ut                rows are UT1 (server's delta T)\n"
    "  --deltat SEC        send TT-UT1 explicitly (one value), so the server's own\n"
    "                      delta T model enters nothing\n"
    "  --step SECONDS      between rows (default 86400)\n"
    "  --count N           rows (default 1)\n"
    "  --helio | --bary    the observer (default geocentric); --helio asks for light\n"
    "                      time and aberration unless --corrections says otherwise,\n"
    "                      since the Sun's centre cannot be deflected (ERROR 11)\n"
    "  --center NAIF       observer at a body's centre\n"
    "  --eq                equatorial plane (default ecliptic)\n"
    "  --j2000 | --icrs    the frame (default true of date)\n"
    "  --no-corrections    geometric positions (same as --corrections 0)\n"
    "  --corrections MASK  the correction bits to ask for, 0..7:\n"
    "                      1 light time, 2 deflection, 4 aberration\n"
    "  --sid TOKEN         a zodiac (fagan-bradley, lahiri, user)\n"
    "  --sidu T0,AYAN      a user zodiac's anchor (mean ayanamsa at TT epoch)\n"
    "  --sid-plane P       the sidereal plane (A.8): date (default), anchor (the\n"
    "                      ecliptic of the zodiac's anchor epoch) or invariable\n"
    "  --topo LON,LAT,ELV  observer site (degrees east, degrees, metres)\n"
    "  --f32               ask for float32 values\n"
    "  --chunk N           chunk-size hint (default: the server's maximum)\n"
    "  --segments          representation 1: ask for SEGDATA (rectangular form,\n"
    "                      answered with Chebyshev segments over the span)\n"
    "  --target ARCSEC     the segments' target error (with --segments)\n"
    "  --max-degree N      largest Chebyshev degree to buffer (with --segments)\n"
    "  --priority 0|1      0 interactive (default), 1 prefetch\n"
    "  --request-id N      the REQUEST's id (default: a fresh one per run, printed\n"
    "                      as '# request N', which prometheiad logs as req=N)\n"
    "  --timeout SECONDS   wait this long for each message from the server\n"
    "                      (default 10; 0 waits forever): a large request on a\n"
    "                      busy server computes before its first byte\n"
    "  --cancel-after-ms N send CANCEL after N ms (shows ERROR 10 unless the\n"
    "                      answer already went out)\n"
    "  --token T           HELLO's access token\n"
    "  --tls               wss:// (verifies the certificate and name)\n"
    "  --ca FILE           trust anchors for --tls (default: the system store)\n"
    "  --sni NAME          name to verify (default: --host)\n"
    "  --insecure          --tls without verification\n"
    "Prints: <object> <row> <six values, %.17g> and, per object, its name,\n"
    "corrections applied and error text on a '#' line. Exit 2 on a server ERROR.\n";

bool three(const char* s, double& a, double& b, double& c) {
    return std::sscanf(s, "%lf,%lf,%lf", &a, &b, &c) == 3;
}

std::vector<uint8_t> message(uint16_t type, uint32_t request_id, const uint8_t* payload,
                             size_t len) {
    std::vector<uint8_t> out;
    eph::WriteEnvelope(&out, type, request_id, len);
    out.insert(out.end(), payload, payload + len);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = eph::kDefaultPort;
    std::string token;
    WsTlsOptions tls;
    eph::Request req;          // defaults: TT grid, one profile, f64
    req.start.jd1 = 2451545.0; // the documented --jd default; the codec's own is JD 0
    eph::Profile& pf = req.profiles.emplace_back();
    double step_seconds = 86400.0;
    double target_arcsec = 0.1;
    int cancel_after_ms = -1;
    int timeout_ms = 10000;
    // A fresh id per run, so the server's log line for this request
    // (prometheiad: "req=<id>") finds this run and no other. Nonzero, 31 bits.
    uint32_t request_id =
        uint32_t((std::chrono::system_clock::now().time_since_epoch().count() ^ getpid()) &
                 0x7fffffff) |
        1u;
    bool corrections_given = false;
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
            eph::Object o;
            o.kind = eph::kObjBody;
            o.naif = std::atoi(value());
            req.objs.push_back(o);
        } else if (arg == "--name") {
            eph::Object o;
            o.kind = eph::kObjDesignation;
            o.name = value();
            req.objs.push_back(o);
        } else if (arg == "--star") {
            eph::Object o;
            o.kind = eph::kObjStar;
            o.name = value();
            req.objs.push_back(o);
        } else if (arg == "--hyp") {
            eph::Object o;
            o.kind = eph::kObjHypothetical;
            o.name = value();
            req.objs.push_back(o);
        } else if (arg == "--elements") {
            const std::string path = value();
            std::ifstream f(path, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            if (!f) {
                std::fprintf(stderr, "cannot open %s\n", path.c_str());
                return 2;
            }
            auto bodies = prometheia::hypotheticals::parse(text, path);
            if (!bodies) {
                std::fprintf(stderr, "%s\n", bodies.error().message.c_str());
                return 2;
            }
            // Element-major coefficients, M's first (3.5a); the epoch and an
            // explicit equinox are TT; the wire calls the origin "centre".
            for (const prometheia::hypotheticals::Body& b : bodies.value()) {
                const prometheia::PolynomialElements& el = b.elements;
                eph::Object o;
                o.kind = eph::kObjElements;
                o.epoch = eph::Time{el.epoch_jd_tt, 0.0};
                o.equinox = uint8_t(el.equinox);
                o.equinoxJd = el.equinox_jd_tt;
                o.centre = el.origin == prometheia::ElementOrigin::Earth ? 1 : 0;
                o.nTerms = uint8_t(el.n_terms);
                for (const double* c : {el.mean_anomaly, el.semi_major_axis, el.eccentricity,
                                        el.arg_perihelion, el.ascending_node, el.inclination}) {
                    o.coef.insert(o.coef.end(), c, c + el.n_terms);
                }
                o.name = b.name.empty() ? b.token : b.name;
                req.objs.push_back(std::move(o));
            }
        } else if (arg == "--node") {
            // NAIF.P[.M]: the point (A.13) a|d|p|A, then the method (A.14)
            // m|o or its registry number; mean when absent.
            const std::string spec = value();
            const size_t dot = spec.find('.');
            const std::string point = dot == std::string::npos ? "" : spec.substr(dot + 1, 1);
            const size_t dot2 = dot == std::string::npos ? dot : spec.find('.', dot + 1);
            const std::string method = dot2 == std::string::npos ? "m" : spec.substr(dot2 + 1);
            const std::string points = "adpA";
            eph::Object o;
            o.kind = eph::kObjOrbitPoint;
            o.naif = std::atoi(spec.substr(0, dot).c_str());
            if (point.size() != 1 || points.find(point[0]) == std::string::npos) {
                std::fprintf(stderr, "--node %s: the point must be a, d, p or A\n%s", spec.c_str(),
                             kUsage);
                return 2;
            }
            o.point = uint8_t(points.find(point[0]));
            if (method == "m") {
                o.method = 0;
            } else if (method == "o") {
                o.method = 1;
            } else if (method.size() == 1 && method[0] >= '0' && method[0] <= '4') {
                o.method = uint8_t(method[0] - '0');
            } else {
                std::fprintf(stderr, "--node %s: the method must be m, o or 0..4\n%s", spec.c_str(),
                             kUsage);
                return 2;
            }
            req.objs.push_back(o);
        } else if (arg == "--jd") {
            req.start.jd1 = std::strtod(value(), nullptr);
        } else if (arg == "--deltat") {
            req.deltaTSec = std::strtod(value(), nullptr);
        } else if (arg == "--ut") {
            req.timeScale = eph::kTimeUT1;
        } else if (arg == "--step") {
            step_seconds = std::strtod(value(), nullptr);
        } else if (arg == "--count") {
            req.nTime = uint32_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--helio") {
            pf.observer = eph::kObsHelio;
        } else if (arg == "--bary") {
            pf.observer = eph::kObsBary;
        } else if (arg == "--center") {
            pf.observer = eph::kObsBody;
            pf.observerBody = std::atoi(value());
        } else if (arg == "--eq") {
            pf.plane = eph::kPlaneEquator;
        } else if (arg == "--j2000") {
            pf.frame = eph::kFrameJ2000;
        } else if (arg == "--icrs") {
            pf.frame = eph::kFrameIcrf;
        } else if (arg == "--no-corrections") {
            pf.corrections = 0;
            corrections_given = true;
        } else if (arg == "--corrections") {
            const long m = std::strtol(value(), nullptr, 0);
            if (m < 0 || m > eph::kCorrMask) {
                std::fprintf(stderr, "--corrections must be 0..%u\n%s", eph::kCorrMask, kUsage);
                return 1;
            }
            pf.corrections = uint8_t(m);
            corrections_given = true;
        } else if (arg == "--sid") {
            pf.zodiac = value();
        } else if (arg == "--sid-plane") {
            const std::string plane = value();
            if (plane == "date") {
                pf.siderealPlane = eph::kSidPlaneDate;
            } else if (plane == "anchor") {
                pf.siderealPlane = eph::kSidPlaneAnchor;
            } else if (plane == "invariable") {
                pf.siderealPlane = eph::kSidPlaneInvariable;
            } else {
                std::fprintf(stderr, "--sid-plane: date, anchor or invariable\n%s", kUsage);
                return 2;
            }
        } else if (arg == "--sidu") {
            if (std::sscanf(value(), "%lf,%lf", &pf.anchorEpoch.jd1, &pf.anchorAyanamsaDeg) != 2) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            pf.zodiac = "user";
        } else if (arg == "--topo") {
            if (!three(value(), pf.siteLonEastDeg, pf.siteLatDeg, pf.siteHeightM)) {
                std::fputs(kUsage, stderr);
                return 2;
            }
            pf.observer = eph::kObsTopo;
        } else if (arg == "--f32") {
            req.precision = eph::kPrecF32;
        } else if (arg == "--chunk") {
            req.chunkRows = uint32_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--segments") {
            req.representation = 1;
            pf.form = eph::kFormRectangular; // segments are rectangular (3.4)
            pf.columns = 0;
        } else if (arg == "--target") {
            target_arcsec = std::strtod(value(), nullptr);
        } else if (arg == "--max-degree") {
            req.maxDegreeHint = uint8_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--priority") {
            req.priority = uint8_t(std::strtoul(value(), nullptr, 10));
        } else if (arg == "--request-id") {
            request_id = uint32_t(std::strtoul(value(), nullptr, 10));
            if (request_id == 0) {
                std::fprintf(stderr, "--request-id must be nonzero (3.3)\n");
                return 2;
            }
        } else if (arg == "--timeout") {
            const double t = std::strtod(value(), nullptr);
            timeout_ms = t > 0.0 ? int(std::min(t, 2e6) * 1000.0) : -1;
        } else if (arg == "--cancel-after-ms") {
            cancel_after_ms = std::atoi(value());
        } else if (arg == "--token") {
            token = value();
        } else if (arg == "--tls") {
            tls.enabled = true;
        } else if (arg == "--ca") {
            tls.ca_file = value();
        } else if (arg == "--sni") {
            tls.sni = value();
        } else if (arg == "--insecure") {
            tls.enabled = true;
            tls.verify = false;
        } else {
            std::fprintf(stderr, "bad option %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
    // The apparent place at the Sun's centre is light time and aberration:
    // deflection is not honoured there, and asking for it is ERROR 11 (3.5a).
    if (pf.observer == eph::kObsHelio && !corrections_given) {
        pf.corrections = eph::kCorrLightTime | eph::kCorrAberration;
    }
    if (req.objs.empty()) {
        std::fprintf(stderr, "no objects\n%s", kUsage);
        return 2;
    }
    if (req.nTime > 1 && step_seconds == 0.0) {
        std::fprintf(stderr, "--step 0 with several rows\n%s", kUsage);
        return 2;
    }
    // 3.5: stepNs is 0 with one row, nonzero otherwise.
    req.stepNs = req.nTime > 1 ? int64_t(step_seconds * 1e9) : 0;
    if (req.representation == 1) {
        if (!(target_arcsec > 0.0)) {
            std::fprintf(stderr, "--segments needs --target ARCSEC > 0\n%s", kUsage);
            return 2;
        }
        req.segTargetErrArcsec = float(target_arcsec);
    }

    WsClient ws;
    if (auto r = ws.connect(host, port, "/", tls); !r) {
        std::fprintf(stderr, "%s\n", r.error().message.c_str());
        return 1;
    }
    const auto fail = [](const Error& e) {
        std::fprintf(stderr, "%s\n", e.message.c_str());
        return 1;
    };

    eph::Hello hello;
    hello.protoMax = eph::kProtoVersion;
    hello.protoMin = eph::kProtoMin;
    hello.clientName = "prometheia-wire-client/0.7.0";
    hello.token = token;
    std::vector<uint8_t> hello_payload;
    eph::EncodeHello(&hello_payload, hello);
    if (auto r = ws.send(message(eph::kMsgHello, 0, hello_payload.data(), hello_payload.size()));
        !r) {
        return fail(r.error());
    }
    std::vector<uint8_t> payload;
    eph::EncodeRequest(&payload, req);
    std::printf("# request %u\n", request_id);
    if (auto r = ws.send(message(eph::kMsgRequest, request_id, payload.data(), payload.size()));
        !r) {
        return fail(r.error());
    }
    if (cancel_after_ms >= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cancel_after_ms));
        // A request already answered completely earns silence (3.4); one
        // still computing earns ERROR 10.
        if (auto r = ws.send(message(eph::kMsgCancel, request_id, nullptr, 0)); !r) {
            return fail(r.error());
        }
    }

    const auto n_obj = uint32_t(req.objs.size());
    std::vector<double> cols;
    std::vector<eph::Meta> meta;
    uint32_t rows_seen = 0, chunk_expected = 0;
    int n_cols = 6;
    for (;;) {
        auto msg = ws.receive(timeout_ms);
        if (!msg) {
            return fail(msg.error());
        }
        const std::vector<uint8_t>& m = msg.value();
        eph::Envelope env{};
        std::string why;
        if (m.size() < eph::kEnvelopeSize ||
            eph::ParseEnvelope(m.data(), m.size(), &env, &why) != eph::kOk) {
            std::fprintf(stderr, "malformed message from the server (%s)\n", why.c_str());
            return 1;
        }
        const uint8_t* p = m.data() + eph::kEnvelopeSize;
        if (env.type == eph::kMsgWelcome) {
            eph::Welcome w;
            if (eph::ParseWelcome(p, env.payloadLen, &w, &why) == eph::kOk) {
                std::printf("# WELCOME %s protocol %u engine \"%s\" dataset %s maxCells %u\n",
                            w.serverName.c_str(), w.protoSession, w.engine.c_str(),
                            w.datasetId.c_str(), w.maxCells);
                // A.3 0x0004, one line per entry: what the server says it can
                // honour, for which observers, then 0x0014's additions per
                // object kind. A checker compares the corrApplied it reports
                // against these.
                eph::Capabilities caps;
                if (eph::ParseCapabilities(w.caps_, &caps, &why) == eph::kOk) {
                    for (const auto& e : caps.corrMasks) {
                        std::printf("# corrmask observers %u corrections %u\n", e.first, e.second);
                    }
                    // A.3 0x0014: masks added per (observer, kind), on top of
                    // the 0x0004 lines above (which hold for every kind).
                    for (const auto& e : caps.corrByKind) {
                        std::printf("# corrkind observers %u kinds %u corrections %u\n",
                                    e.observers, e.kinds, e.mask);
                    }
                    // One line of the rest, for tools comparing two servers'
                    // surfaces side by side.
                    const auto list = [](const std::vector<std::string>& v) {
                        std::string out;
                        for (const std::string& x : v) {
                            out += (out.empty() ? "" : ",") + x;
                        }
                        return out.empty() ? std::string("-") : out;
                    };
                    std::printf("# caps kinds %u observers %u planes %u forms %u frames %u "
                                "orbitpoints %u orbitmethods %u columns %u timescales %u "
                                "segments %d segkinds %u lookup %u zodiacs %s hypotheticals %s\n",
                                caps.kinds, caps.observers, caps.planes, caps.forms, caps.frames,
                                caps.orbitPoints, caps.orbitMethods, caps.columns, caps.timeScales,
                                caps.fSegments ? 1 : 0, caps.segKinds, unsigned(caps.lookupMax),
                                list(caps.zodiacs).c_str(), list(caps.hypotheticals).c_str());
                    // A.3 0x0013, the server's own bound on how far its rate
                    // columns may sit from a central difference of its
                    // positions. Absent means the registry's default, which
                    // is why the two cases are printed differently: a checker
                    // must not read silence as a promise of zero.
                    if (caps.fRatesBound)
                        std::printf("# ratesbound %.9g %.9g\n", double(caps.ratesDegPerDay),
                                    double(caps.ratesAuPerDay));
                    else
                        std::printf("# ratesbound - -\n");
                }
            }
            continue;
        }
        if (env.type == eph::kMsgError) {
            eph::Error e;
            if (eph::ParseError(p, env.payloadLen, &e, &why) == eph::kOk) {
                if (e.code == eph::kErrCancelled && cancel_after_ms >= 0) {
                    std::printf("# cancelled before the answer was whole\n");
                    return 0;
                }
                std::fprintf(stderr, "ERROR %u: %s\n", e.code, e.text.c_str());
            }
            return 2;
        }
        if (env.requestId != request_id) {
            continue;
        }
        if (req.representation == 1) {
            if (env.type != eph::kMsgSegData) {
                continue;
            }
            eph::SegDataChunk s;
            if (eph::ParseSegData(p, env.payloadLen, &s, &why) != eph::kOk) {
                std::fprintf(stderr, "bad SEGDATA chunk: %s\n", why.c_str());
                return 1;
            }
            for (const eph::Meta& mm : s.meta) {
                std::printf("# object %ld name \"%s\" segments %d corr %u err %u \"%s\"\n",
                            long(&mm - s.meta.data()), mm.name.c_str(), mm.rowsOk, mm.corrApplied,
                            mm.errCode, mm.errText.c_str());
            }
            for (const eph::AyanSeries& series : s.ayan) {
                float worst = 0.0f;
                double from = 0.0, to = 0.0;
                for (const eph::AyanSeg& g : series.segs) {
                    worst = std::max(worst, g.errArcsec);
                    from = std::min(from == 0.0 ? g.mid.jd1 - g.halfSpanDays : from,
                                    g.mid.jd1 - g.halfSpanDays);
                    to = std::max(to, g.mid.jd1 + g.halfSpanDays);
                }
                std::printf("# ayanamsa profile %u: %zu segments, JD %.1f..%.1f, worst %.4g\"\n",
                            series.profile, series.segs.size(), from, to, worst);
            }
            for (uint32_t i = 0; i < s.nObjChunk; ++i) {
                const std::vector<eph::Segment>& segs = s.segs[i];
                float worst = 0.0f, worst_rate = 0.0f;
                double from = 0.0, to = 0.0;
                int max_deg = 0;
                for (const eph::Segment& g : segs) {
                    worst = std::max(worst, g.errArcsec);
                    worst_rate = std::max(worst_rate, g.errRateArcsecPerDay);
                    max_deg = std::max(max_deg, int(g.degree));
                    from = std::min(from == 0.0 ? g.mid.jd1 - g.halfSpanDays : from,
                                    g.mid.jd1 - g.halfSpanDays);
                    to = std::max(to, g.mid.jd1 + g.halfSpanDays);
                }
                std::printf("seg %u %zu segments JD %.1f..%.1f degree<=%d err<=%.4g\" "
                            "rate<=%.4g\"/day\n",
                            s.iObj + i, segs.size(), from, to, max_deg, worst, worst_rate);
            }
            if (s.flags & eph::kChunkLast) {
                return 0;
            }
            continue;
        }
        if (env.type != eph::kMsgData) {
            continue;
        }
        eph::DataChunk d;
        if (eph::ParseData(p, env.payloadLen, &d, &why) != eph::kOk) {
            std::fprintf(stderr, "bad DATA chunk: %s\n", why.c_str());
            return 1;
        }
        if (d.chunkIndex != chunk_expected || d.iTime != rows_seen ||
            d.iTime + d.nRows > req.nTime) {
            std::fprintf(stderr, "out-of-order or mismatched DATA chunk\n");
            return 1;
        }
        n_cols = d.Cols();
        if (cols.empty()) {
            cols.assign(size_t(n_obj) * req.nTime * n_cols, NAN);
        }
        for (const eph::Meta& mm : d.meta) {
            std::printf("# object %ld name \"%s\" rowsOk %d corr %u err %u \"%s\"\n",
                        long(&mm - d.meta.data()), mm.name.c_str(), mm.rowsOk, mm.corrApplied,
                        mm.errCode, mm.errText.c_str());
        }
        for (uint32_t o = 0; o < n_obj; ++o) {
            for (uint32_t r = 0; r < d.nRows; ++r) {
                for (int k = 0; k < n_cols; ++k) {
                    const size_t at = (size_t(o) * d.totalRows + d.iTime + r) * n_cols + k;
                    cols[at] = d.values[(size_t(o) * d.nRows + r) * n_cols + k];
                }
            }
        }
        rows_seen += d.nRows;
        ++chunk_expected;
        if (d.flags & eph::kChunkLast) {
            break;
        }
    }
    for (uint32_t o = 0; o < n_obj; ++o) {
        for (uint32_t r = 0; r < req.nTime; ++r) {
            const double* v = &cols[(size_t(o) * req.nTime + r) * n_cols];
            std::printf("%u %u %.17g %.17g %.17g %.17g %.17g %.17g", o, r, v[0], v[1], v[2], v[3],
                        v[4], v[5]);
            for (int k = 6; k < n_cols; ++k) {
                std::printf(" %.17g", v[k]);
            }
            std::printf("\n");
        }
    }
    return 0;
}
