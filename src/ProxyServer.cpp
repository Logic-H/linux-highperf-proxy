#include "proxy/ProxyServer.h"
#include "proxy/common/Config.h"
#include "proxy/common/Logger.h"
#include "proxy/ProxySessionContext.h"
#include "proxy/protocol/HttpRequest.h"
#include "proxy/protocol/HttpResponse.h"
#include "proxy/protocol/Cookie.h"
#include "proxy/protocol/Http2Common.h"
#include "proxy/protocol/GrpcFramer.h"
#include "proxy/protocol/ProtobufLite.h"
#include "proxy/protocol/Compression.h"
#include "proxy/monitor/Stats.h"
#include "proxy/monitor/TokenBucket.h"
#include "proxy/monitor/CongestionControl.h"
#include "proxy/monitor/AccessControl.h"
#include "proxy/monitor/AuditLogger.h"
#include "proxy/monitor/PerKeyRateLimiter.h"

#include <cstring>
#include <cstdlib>
#include <sys/timerfd.h>
#include <unistd.h>
#include <sstream>
#include <optional>
#include <functional>
#include <fstream>
#include <deque>
#include <array>
#include <unordered_map>
#include <algorithm>

namespace proxy {

namespace {

static std::string GrpcEchoUnaryReply(const std::string& in) {
    return in;
}

static std::vector<std::string> GrpcEchoStreamReply(const std::string& in) {
    return {in + "#1", in + "#2", in + "#3"};
}

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

static bool headerContainsTokenCI(const std::string& headerValue, const std::string& token) {
    // token match on comma-separated list, case-insensitive, ignoring spaces.
    size_t i = 0;
    while (i < headerValue.size()) {
        while (i < headerValue.size() && (headerValue[i] == ' ' || headerValue[i] == '\t' || headerValue[i] == ',')) ++i;
        size_t start = i;
        while (i < headerValue.size() && headerValue[i] != ',') ++i;
        size_t end = i;
        while (end > start && (headerValue[end - 1] == ' ' || headerValue[end - 1] == '\t')) --end;
        if (end > start) {
            const std::string part = headerValue.substr(start, end - start);
            if (iequals(part, token)) return true;
        }
        if (i < headerValue.size() && headerValue[i] == ',') ++i;
    }
    return false;
}

static bool icontains(const std::string& s, const std::string& needle) {
    if (needle.empty()) return true;
    if (s.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= s.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            char a = s[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

static std::optional<std::string> ExtractJsonString(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n'))
        ++pos;
    if (pos >= body.size() || body[pos] != '"') return std::nullopt;
    ++pos;
    size_t end = body.find('"', pos);
    if (end == std::string::npos) return std::nullopt;
    return body.substr(pos, end - pos);
}

static std::optional<double> ExtractJsonNumber(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n'))
        ++pos;
    size_t end = pos;
    while (end < body.size()) {
        char c = body[end];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
            ++end;
            continue;
        }
        break;
    }
    if (end == pos) return std::nullopt;
    try {
        return std::stod(body.substr(pos, end - pos));
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<std::string> ExtractBackendId(const std::string& body) {
    auto backend = ExtractJsonString(body, "backend");
    if (backend && !backend->empty()) return backend;

    auto ip = ExtractJsonString(body, "ip");
    auto port = ExtractJsonNumber(body, "port");
    if (ip && port) {
        int p = static_cast<int>(*port);
        if (p > 0 && p <= 65535) {
            return *ip + ":" + std::to_string(p);
        }
    }
    return std::nullopt;
}

static std::string ExtractQueryParam(const std::string& query, const std::string& key) {
    // query may start with '?'. No percent-decoding (best-effort).
    if (key.empty()) return {};
    std::string q = query;
    if (!q.empty() && q[0] == '?') q.erase(0, 1);
    size_t pos = 0;
    while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.size();
        size_t eq = q.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            const std::string k = q.substr(pos, eq - pos);
            if (k == key) {
                return q.substr(eq + 1, amp - (eq + 1));
            }
        } else {
            const std::string k = q.substr(pos, amp - pos);
            if (k == key) return "";
        }
        pos = (amp < q.size()) ? amp + 1 : q.size();
    }
    return {};
}

static std::string PlainResponse(int code,
                                 const std::string& reason,
                                 const std::string& body,
                                 bool close = true) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << reason << "\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: " << (close ? "close" : "keep-alive") << "\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

static bool isHttp2PrefacePrefix(const char* data, size_t len) {
    if (!data || len == 0) return false;
    if (len > protocol::kHttp2ConnectionPrefaceLen) len = protocol::kHttp2ConnectionPrefaceLen;
    return std::memcmp(data, protocol::kHttp2ConnectionPreface, len) == 0;
}

static std::string getH2HeaderCI(const std::vector<protocol::Hpack::Header>& headers, const std::string& name) {
    for (const auto& h : headers) {
        if (iequals(h.name, name)) return h.value;
    }
    return {};
}

static bool headerContainsTokenCI2(const std::string& headerValue, const std::string& token) {
    return headerContainsTokenCI(headerValue, token);
}

static protocol::Compression::Encoding ChooseEncodingFromAccept(const std::string& acceptEncoding) {
    if (headerContainsTokenCI2(acceptEncoding, "gzip")) return protocol::Compression::Encoding::kGzip;
    if (headerContainsTokenCI2(acceptEncoding, "deflate")) return protocol::Compression::Encoding::kDeflate;
    return protocol::Compression::Encoding::kIdentity;
}

static std::string EncodingToHeaderValue(protocol::Compression::Encoding enc) {
    switch (enc) {
        case protocol::Compression::Encoding::kGzip:
            return "gzip";
        case protocol::Compression::Encoding::kDeflate:
            return "deflate";
        default:
            return "";
    }
}

static void ParseHttp1HeadMeta(const std::string& head, std::string* outStatusLine, std::string* outContentEncoding) {
    if (outStatusLine) outStatusLine->clear();
    if (outContentEncoding) outContentEncoding->clear();
    const size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) return;
    if (outStatusLine) *outStatusLine = head.substr(0, lineEnd);
    std::istringstream iss(head);
    std::string line;
    std::getline(iss, line); // status
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        if (outContentEncoding && iequals(k, "Content-Encoding")) {
            *outContentEncoding = v;
        }
    }
}

static bool decodeChunkedBody(const std::string& raw, std::string* outBody) {
    if (!outBody) return false;
    outBody->clear();
    size_t p = 0;
    while (p < raw.size()) {
        size_t lineEnd = raw.find("\r\n", p);
        if (lineEnd == std::string::npos) return false;
        std::string line = raw.substr(p, lineEnd - p);
        const size_t semi = line.find(';');
        if (semi != std::string::npos) line = line.substr(0, semi);
        // trim
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        line = line.substr(i);
        if (line.empty()) return false;
        char* endp = nullptr;
        unsigned long long n = std::strtoull(line.c_str(), &endp, 16);
        if (endp == line.c_str()) return false;
        p = lineEnd + 2;
        const size_t chunkLen = static_cast<size_t>(n);
        if (chunkLen == 0) return true;
        if (p + chunkLen + 2 > raw.size()) return false;
        outBody->append(raw.data() + p, raw.data() + p + chunkLen);
        p += chunkLen;
        if (raw.compare(p, 2, "\r\n") != 0) return false;
        p += 2;
    }
    return false;
}

static bool parseHttp1Response(const std::string& raw,
                               int* outStatus,
                               std::vector<protocol::Hpack::Header>* outHeaders,
                               std::string* outContentEncoding,
                               std::string* outStatusLine,
                               std::string* outBody) {
    if (!outStatus || !outHeaders || !outContentEncoding || !outStatusLine || !outBody) return false;
    *outStatus = 502;
    outHeaders->clear();
    outContentEncoding->clear();
    outStatusLine->clear();
    outBody->clear();

    const size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false;
    const std::string head = raw.substr(0, hdrEnd + 4);
    const std::string bodyRaw = raw.substr(hdrEnd + 4);

    const size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    const std::string statusLine = head.substr(0, lineEnd);
    *outStatusLine = statusLine;
    // HTTP/1.1 200 OK
    const size_t sp1 = statusLine.find(' ');
    if (sp1 == std::string::npos) return false;
    const size_t sp2 = statusLine.find(' ', sp1 + 1);
    const std::string codeStr = (sp2 == std::string::npos) ? statusLine.substr(sp1 + 1) : statusLine.substr(sp1 + 1, sp2 - sp1 - 1);
    int code = 0;
    for (char c : codeStr) {
        if (c < '0' || c > '9') return false;
        code = code * 10 + (c - '0');
    }
    *outStatus = code;

	    bool chunked = false;
	    {
	        std::istringstream iss(head);
	        std::string line;
	        std::getline(iss, line); // status
	        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
	            std::string k = line.substr(0, colon);
	            std::string v = line.substr(colon + 1);
	            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
	            std::string lk;
	            lk.reserve(k.size());
	            for (unsigned char c : k) lk.push_back(static_cast<char>(std::tolower(c)));
	            outHeaders->push_back({lk, v});
	            if (iequals(k, "Transfer-Encoding")) {
	                std::string lv;
	                lv.reserve(v.size());
	                for (unsigned char c : v) lv.push_back(static_cast<char>(std::tolower(c)));
	                if (lv.find("chunked") != std::string::npos) chunked = true;
	            }
	            if (iequals(k, "Content-Encoding")) *outContentEncoding = v;
	        }
	    }
	    if (!chunked) {
	        *outBody = bodyRaw;
	        return true;
	    }
    std::string decoded;
    if (!decodeChunkedBody(bodyRaw, &decoded)) return false;
    *outBody = std::move(decoded);
    return true;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

static std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

static std::string DashboardHtml() {
    // Minimal single-file dashboard (no external deps): polls /stats and renders key metrics.
    return R"(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Proxy Dashboard</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; margin: 16px; color:#111; }
    .row { display:flex; flex-wrap:wrap; gap:12px; }
    .card { border:1px solid #e5e7eb; border-radius:10px; padding:12px; min-width:220px; background:#fff; }
    .k { font-size:12px; color:#6b7280; }
    .v { font-size:22px; font-weight:600; }
    pre { background:#0b1220; color:#d1e7ff; padding:12px; border-radius:10px; overflow:auto; }
    .muted { color:#6b7280; font-size:12px; }
    .badge { display:inline-block; padding:2px 8px; border-radius:999px; background:#eef2ff; color:#3730a3; font-size:12px; }
  </style>
</head>
<body>
  <h2>Proxy Dashboard <span class="badge">realtime</span></h2>
  <div class="muted">Polling <code>/stats</code> every 1s</div>
  <div class="muted" style="margin-top:6px;">Links: <a href="/dashboard">/dashboard</a> · <a href="/config">/config</a> · <a href="/diagnostics">/diagnostics</a> · <a href="/history_ui">/history_ui</a> · <a href="/stats" target="_blank">/stats</a></div>
  <div class="row" style="margin-top:12px;">
    <div class="card"><div class="k">Active Connections</div><div class="v" id="active">-</div></div>
    <div class="card"><div class="k">Total Requests</div><div class="v" id="total">-</div></div>
    <div class="card"><div class="k">Avg QPS</div><div class="v" id="qps">-</div></div>
    <div class="card"><div class="k">I/O Model (runtime/config)</div><div class="v" id="io">-</div></div>
    <div class="card"><div class="k">Backend Error Rate</div><div class="v" id="berr">-</div></div>
    <div class="card"><div class="k">P50/P90/P99 (ms)</div><div class="v" id="lat">-</div></div>
    <div class="card"><div class="k">Bytes In/Out</div><div class="v" id="bio">-</div></div>
    <div class="card"><div class="k">Process RSS / FD</div><div class="v" id="proc">-</div></div>
  </div>
  <h3 style="margin-top:18px;">Raw JSON</h3>
  <pre id="raw">{}</pre>
  <script>
    function fmtNum(x, d) {
      if (x === null || x === undefined) return "-";
      if (typeof x !== "number") x = Number(x);
      if (!isFinite(x)) return "-";
      return (d === undefined) ? String(x) : x.toFixed(d);
    }
    function fmtBytes(n) {
      if (n === null || n === undefined) return "-";
      n = Number(n);
      if (!isFinite(n)) return "-";
      const units = ["B","KB","MB","GB","TB"];
      let i=0; while (n>=1024 && i<units.length-1) { n/=1024; i++; }
      return n.toFixed(i===0?0:2)+" "+units[i];
    }
    async function tick() {
      try {
        const r = await fetch("/stats", { cache: "no-store" });
        const j = await r.json();
        document.getElementById("active").textContent = fmtNum(j.active_connections);
        document.getElementById("total").textContent = fmtNum(j.total_requests);
        document.getElementById("qps").textContent = fmtNum(j.avg_qps, 2);
        if (j.io) {
          document.getElementById("io").textContent = String(j.io.runtime_model || "-") + " / " + String(j.io.configured_model || "-");
        } else {
          document.getElementById("io").textContent = "-";
        }
        document.getElementById("berr").textContent = fmtNum(j.backend_error_rate, 6);
        if (j.latency_ms) {
          document.getElementById("lat").textContent =
            fmtNum(j.latency_ms.p50_ms,3)+"/"+fmtNum(j.latency_ms.p90_ms,3)+"/"+fmtNum(j.latency_ms.p99_ms,3);
        } else {
          document.getElementById("lat").textContent = "-";
        }
        document.getElementById("bio").textContent = fmtBytes(j.bytes_in) + " / " + fmtBytes(j.bytes_out);
        if (j.process) {
          document.getElementById("proc").textContent = fmtBytes(j.process.rss_bytes) + " / " + fmtNum(j.process.fd_count);
        } else {
          document.getElementById("proc").textContent = "-";
        }
        document.getElementById("raw").textContent = JSON.stringify(j, null, 2);
      } catch (e) {
        document.getElementById("raw").textContent = "ERROR: " + String(e);
      }
    }
    tick();
    setInterval(tick, 1000);
  </script>
</body>
</html>)";
}

static std::string ConfigHtml() {
    return R"CFG(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Proxy Config</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; margin: 16px; color:#111; }
    .row { display:flex; gap:12px; flex-wrap:wrap; }
    .card { border:1px solid #e5e7eb; border-radius:10px; padding:12px; background:#fff; }
    input, button, select { font-size:14px; padding:8px; }
    table { border-collapse: collapse; width: 100%; }
    th, td { border-bottom: 1px solid #e5e7eb; text-align:left; padding:8px; font-size:13px; }
    th { color:#6b7280; font-weight:600; }
    pre { background:#0b1220; color:#d1e7ff; padding:12px; border-radius:10px; overflow:auto; }
    .muted { color:#6b7280; font-size:12px; }
  </style>
</head>
<body>
  <h2>Proxy Config</h2>
  <div class="muted">Edits update in-memory config and can optionally persist to the loaded config file. Most settings require restart to take effect.</div>
  <div class="muted" style="margin-top:6px;">Links: <a href="/dashboard">/dashboard</a> · <a href="/config">/config</a> · <a href="/diagnostics">/diagnostics</a> · <a href="/history_ui">/history_ui</a></div>

  <div class="row" style="margin-top:12px;">
    <div class="card">
      <div style="display:flex; gap:8px; align-items:center; flex-wrap:wrap;">
        <button onclick="loadCfg()">Reload</button>
        <label><input type="checkbox" id="save" checked> Save to file</label>
        <span class="muted" id="file">file: -</span>
      </div>
    </div>
    <div class="card">
      <div class="muted">Quick edit</div>
      <div style="display:flex; gap:8px; flex-wrap:wrap; margin-top:6px;">
        <input id="sec" placeholder="section" value="global">
        <input id="key" placeholder="key" value="log_level">
        <input id="val" placeholder="value" value="ERROR">
        <button onclick="applyOne()">Apply</button>
      </div>
    </div>
  </div>

  <h3 style="margin-top:18px;">Settings</h3>
  <div class="card">
    <table>
      <thead><tr><th>Section</th><th>Key</th><th>Value</th><th></th></tr></thead>
      <tbody id="rows"></tbody>
    </table>
  </div>

  <h3 style="margin-top:18px;">Raw JSON</h3>
  <pre id="raw">{}</pre>

  <script>
    function esc(s){ return String(s).replace(/[&<>\"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c])); }
    async function loadCfg(){
      const r = await fetch('/admin/config', {cache:'no-store'});
      const j = await r.json();
      document.getElementById('raw').textContent = JSON.stringify(j, null, 2);
      document.getElementById('file').textContent = 'file: ' + (j.file || '-');
      const tbody = document.getElementById('rows');
      tbody.innerHTML = '';
      const s = j.settings || {};
      const secs = Object.keys(s).sort();
      for (const sec of secs) {
        const keys = Object.keys(s[sec]||{}).sort();
        for (const k of keys) {
          const v = s[sec][k];
          const tr = document.createElement('tr');
          tr.innerHTML = '<td>'+esc(sec)+'</td><td>'+esc(k)+'</td>'+
            '<td><input style=\"width:100%\" value=\"'+esc(v)+'\" /></td>'+
            '<td><button>Save</button></td>';
          tr.querySelector('button').onclick = async () => {
            const nv = tr.querySelector('input').value;
            await updateCfg(sec, k, nv);
            await loadCfg();
          };
          tbody.appendChild(tr);
        }
      }
    }
    async function updateCfg(section, key, value){
      const save = document.getElementById('save').checked ? 1 : 0;
      const body = { updates: [{section, key, value}], save };
      const r = await fetch('/admin/config', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
      const j = await r.json();
      if (!j.ok) alert('update failed: '+JSON.stringify(j));
    }
    async function applyOne(){
      const section = document.getElementById('sec').value;
      const key = document.getElementById('key').value;
      const value = document.getElementById('val').value;
      await updateCfg(section, key, value);
      await loadCfg();
    }
    loadCfg();
  </script>
</body>
</html>)CFG";
}

static std::string HistoryUiHtml() {
    return R"HIS(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>History</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; margin: 16px; color:#111; }
    .row { display:flex; gap:12px; flex-wrap:wrap; align-items:center; }
    .card { border:1px solid #e5e7eb; border-radius:10px; padding:12px; background:#fff; }
    button, input, select { font-size:14px; padding:8px; }
    canvas { width:100%; height:220px; border:1px solid #e5e7eb; border-radius:10px; background:#fff; }
    .muted { color:#6b7280; font-size:12px; }
    .grid { display:grid; grid-template-columns: 1fr; gap:12px; }
    @media (min-width: 900px) { .grid { grid-template-columns: 1fr 1fr; } }
    .err { color:#b91c1c; }
    a { color:#2563eb; text-decoration:none; }
    a:hover { text-decoration:underline; }
  </style>
</head>
<body>
  <h2>History <span class="muted">(requires [history].enable=1)</span></h2>
  <div class="muted">Charts are rendered from <code>/history</code> points (no external dependencies).</div>
  <div class="muted" style="margin-top:6px;">Links: <a href="/dashboard">/dashboard</a> · <a href="/config">/config</a> · <a href="/diagnostics">/diagnostics</a> · <a href="/history/summary" target="_blank">/history/summary</a></div>

  <div class="row" style="margin-top:12px;">
    <div class="card">
      <label class="muted">window</label>
      <select id="win">
        <option value="60">60s</option>
        <option value="300" selected>300s</option>
        <option value="900">900s</option>
        <option value="3600">3600s</option>
      </select>
      <button onclick="reload()">Reload</button>
      <label style="margin-left:8px;" class="muted"><input type="checkbox" id="auto" checked> auto</label>
      <span class="muted" style="margin-left:8px;" id="meta">-</span>
    </div>
    <div class="card">
      <div class="muted err" id="err"></div>
      <div class="muted">Tip: history 数据在代理进程内存里；可用 <code>/history/summary</code> 快速看 min/max/avg。</div>
    </div>
  </div>

  <div class="grid" style="margin-top:12px;">
    <div class="card"><div class="muted">QPS</div><canvas id="c_qps"></canvas></div>
    <div class="card"><div class="muted">Active Connections</div><canvas id="c_conns"></canvas></div>
    <div class="card"><div class="muted">P99 Latency (ms)</div><canvas id="c_p99"></canvas></div>
    <div class="card"><div class="muted">Backend Error Rate (interval)</div><canvas id="c_berr"></canvas></div>
    <div class="card"><div class="muted">CPU % (single core, interval)</div><canvas id="c_cpu"></canvas></div>
    <div class="card"><div class="muted">RSS (MB)</div><canvas id="c_rss"></canvas></div>
  </div>

  <script>
    function setErr(e){ document.getElementById('err').textContent = String(e || ''); }
    function clearErr(){ setErr(''); }
    function qs(id){ return document.getElementById(id); }

    function fitCanvas(c) {
      const dpr = window.devicePixelRatio || 1;
      const rect = c.getBoundingClientRect();
      const w = Math.max(10, Math.floor(rect.width * dpr));
      const h = Math.max(10, Math.floor(rect.height * dpr));
      if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
      return {w, h, dpr};
    }

    function drawSeries(canvas, xs, ys, opts) {
      const ctx = canvas.getContext('2d');
      const {w, h} = fitCanvas(canvas);
      ctx.clearRect(0,0,w,h);
      ctx.fillStyle = '#ffffff';
      ctx.fillRect(0,0,w,h);
      const pad = 28;
      const x0 = pad, y0 = pad, x1 = w - pad, y1 = h - pad;

      // Frame
      ctx.strokeStyle = '#e5e7eb';
      ctx.lineWidth = 1;
      ctx.strokeRect(x0, y0, x1-x0, y1-y0);

      if (!xs.length || !ys.length) {
        ctx.fillStyle = '#6b7280';
        ctx.font = '12px sans-serif';
        ctx.fillText('no data', x0 + 8, y0 + 18);
        return;
      }

      let ymin = ys[0], ymax = ys[0];
      for (const v of ys) { if (v < ymin) ymin = v; if (v > ymax) ymax = v; }
      if (opts && typeof opts.min === 'number') ymin = Math.min(ymin, opts.min);
      if (opts && typeof opts.max === 'number') ymax = Math.max(ymax, opts.max);
      if (ymax - ymin < 1e-9) { ymax = ymin + 1; }

      const xMin = xs[0], xMax = xs[xs.length - 1];
      const sx = (xMax === xMin) ? 1 : (x1 - x0) / (xMax - xMin);
      const sy = (y1 - y0) / (ymax - ymin);
      const X = (t) => x0 + (t - xMin) * sx;
      const Y = (v) => y1 - (v - ymin) * sy;

      // Y labels
      ctx.fillStyle = '#6b7280';
      ctx.font = '11px sans-serif';
      const fmt = (v) => (Math.abs(v) >= 100 ? String(Math.round(v)) : (Math.round(v*100)/100).toFixed(2));
      ctx.fillText(fmt(ymax), 6, y0 + 10);
      ctx.fillText(fmt(ymin), 6, y1);

      // Line
      ctx.strokeStyle = (opts && opts.color) ? opts.color : '#2563eb';
      ctx.lineWidth = 2;
      ctx.beginPath();
      for (let i = 0; i < xs.length; i++) {
        const px = X(xs[i]);
        const py = Y(ys[i]);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.stroke();

      // Last value
      const last = ys[ys.length - 1];
      ctx.fillStyle = '#111827';
      ctx.font = '12px sans-serif';
      ctx.fillText('last: ' + fmt(last), x0 + 8, y0 + 18);
    }

    async function fetchHistory(seconds) {
      const r = await fetch('/history?seconds=' + encodeURIComponent(seconds), {cache:'no-store'});
      if (!r.ok) throw new Error('GET /history: ' + r.status);
      return await r.json();
    }

    function getPoints(j) {
      if (!j) return [];
      if (j.points && Array.isArray(j.points)) return j.points;
      return [];
    }

    function extract(points, key, scale) {
      const xs = [];
      const ys = [];
      for (const p of points) {
        if (!p || typeof p.ts_ms !== 'number') continue;
        const v = p[key];
        if (typeof v !== 'number') continue;
        xs.push(p.ts_ms);
        ys.push(scale ? v * scale : v);
      }
      return {xs, ys};
    }

    async function reload() {
      clearErr();
      const seconds = Number(qs('win').value || 300);
      try {
        const j = await fetchHistory(seconds);
        const pts = getPoints(j);
        qs('meta').textContent = 'points: ' + pts.length;
        const qps = extract(pts, 'qps');
        const conns = extract(pts, 'active_connections');
        const p99 = extract(pts, 'p99_ms');
        const berr = extract(pts, 'backend_error_rate_interval');
        const cpu = extract(pts, 'cpu_pct_single_core');
        const rss = extract(pts, 'rss_bytes', 1/1024/1024);
        drawSeries(qs('c_qps'), qps.xs, qps.ys, {color:'#2563eb', min:0});
        drawSeries(qs('c_conns'), conns.xs, conns.ys, {color:'#16a34a', min:0});
        drawSeries(qs('c_p99'), p99.xs, p99.ys, {color:'#9333ea', min:0});
        drawSeries(qs('c_berr'), berr.xs, berr.ys, {color:'#dc2626', min:0});
        drawSeries(qs('c_cpu'), cpu.xs, cpu.ys, {color:'#0f766e', min:0});
        drawSeries(qs('c_rss'), rss.xs, rss.ys, {color:'#f59e0b', min:0});
      } catch (e) {
        setErr(e);
      }
    }

    let timer = null;
    function setAuto(on) {
      if (timer) { clearInterval(timer); timer = null; }
      if (on) timer = setInterval(reload, 2000);
    }

    window.addEventListener('resize', () => reload());
    qs('win').onchange = reload;
    qs('auto').onchange = () => setAuto(qs('auto').checked);
    setAuto(true);
    reload();
  </script>
</body>
</html>)HIS";
}

static std::string DiagnosticsHtml() {
    // Minimal diagnostics console: tail audit log + dump combined diagnose JSON.
    return R"DIAG(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Proxy Diagnostics</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; margin: 16px; color:#111; }
    .row { display:flex; gap:12px; flex-wrap:wrap; }
    .card { border:1px solid #e5e7eb; border-radius:10px; padding:12px; background:#fff; }
    button, input { font-size:14px; padding:8px; }
    pre { background:#0b1220; color:#d1e7ff; padding:12px; border-radius:10px; overflow:auto; white-space:pre-wrap; }
    a { color:#2563eb; text-decoration:none; }
    a:hover { text-decoration:underline; }
    .muted { color:#6b7280; font-size:12px; }
  </style>
</head>
<body>
  <h2>Proxy Diagnostics</h2>
  <div class="muted">Links: <a href="/stats" target="_blank">/stats</a> · <a href="/history/summary" target="_blank">/history/summary</a> · <a href="/dashboard" target="_blank">/dashboard</a> · <a href="/config" target="_blank">/config</a> · <a href="/history_ui" target="_blank">/history_ui</a></div>

  <div class="row" style="margin-top:12px;">
    <div class="card" style="flex:1; min-width:320px;">
      <div style="display:flex; gap:8px; align-items:center;">
        <div style="font-weight:600;">Audit Log (tail)</div>
        <div style="margin-left:auto;"></div>
        <label class="muted">lines</label>
        <input id="lines" value="200" style="width:90px;">
        <button onclick="loadLogs()">Refresh</button>
      </div>
      <pre id="log">(click Refresh)</pre>
    </div>
    <div class="card" style="flex:1; min-width:320px;">
      <div style="display:flex; gap:8px; align-items:center;">
        <div style="font-weight:600;">Diagnose JSON</div>
        <div style="margin-left:auto;"></div>
        <button onclick="loadDiag()">Refresh</button>
      </div>
      <pre id="diag">{}</pre>
    </div>
  </div>

  <script>
    async function loadLogs(){
      const lines = Number(document.getElementById('lines').value || 200);
      try {
        const r = await fetch('/admin/logs?type=audit&lines=' + encodeURIComponent(lines), {cache:'no-store'});
        const t = await r.text();
        document.getElementById('log').textContent = t;
      } catch (e) {
        document.getElementById('log').textContent = 'ERROR: ' + String(e);
      }
    }
    async function loadDiag(){
      try {
        const r = await fetch('/admin/diagnose', {cache:'no-store'});
        const j = await r.json();
        document.getElementById('diag').textContent = JSON.stringify(j, null, 2);
      } catch (e) {
        document.getElementById('diag').textContent = 'ERROR: ' + String(e);
      }
    }
  </script>
</body>
</html>)DIAG";
}

static std::string TailTextFileLines(const std::string& path, size_t maxLines, size_t maxBytes = 512 * 1024) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::deque<std::string> q;
    size_t bytes = 0;
    std::string line;
    while (std::getline(f, line)) {
        q.push_back(line);
        bytes += line.size() + 1;
        while (q.size() > maxLines) {
            bytes -= q.front().size() + 1;
            q.pop_front();
        }
        while (bytes > maxBytes && !q.empty()) {
            bytes -= q.front().size() + 1;
            q.pop_front();
        }
    }
    std::string out;
    out.reserve(bytes + 16);
    for (size_t i = 0; i < q.size(); ++i) {
        out += q[i];
        if (i + 1 < q.size()) out.push_back('\n');
    }
    return out;
}

static bool IsTruthy(const std::string& v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'A' && c <= 'Z') s.push_back(static_cast<char>(c - 'A' + 'a'));
        else s.push_back(c);
    }
    return (s == "1" || s == "true" || s == "yes" || s == "on");
}

static int ParsePriorityValue(const std::string& v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        s.push_back(c);
    }
    if (s.empty()) return 0;
    try {
        int p = std::stoi(s);
        if (p < 0) p = 0;
        if (p > 9) p = 9;
        return p;
    } catch (...) {
        return 0;
    }
}

static int ParseNonNegativeInt(const std::string& v, int defVal) {
    try {
        int x = std::stoi(v);
        if (x < 0) x = 0;
        return x;
    } catch (...) {
        return defVal;
    }
}

static std::string ComputeFlowKey(const protocol::HttpRequest& req, const network::TcpConnectionPtr& conn, const ProxyServer::PriorityConfig& cfg) {
    std::string key = req.getHeader(cfg.flowHeader);
    if (key.empty()) key = ExtractQueryParam(req.query(), cfg.flowQuery);
    if (key.empty() && conn) key = conn->peerAddress().toIp();
    if (key.empty()) key = "unknown";
    return key;
}

static std::chrono::steady_clock::time_point ComputeDeadline(const protocol::HttpRequest& req, const ProxyServer::PriorityConfig& cfg) {
    std::string v = req.getHeader(cfg.deadlineHeader);
    if (v.empty()) v = ExtractQueryParam(req.query(), cfg.deadlineQuery);
    int ms = cfg.defaultDeadlineMs;
    if (!v.empty()) ms = ParseNonNegativeInt(v, cfg.defaultDeadlineMs);
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

static std::vector<std::pair<std::string, std::string>> ParseAggregateRequests(const std::string& body) {
    // Very small JSON parser for: {"requests":[{"path":"/a","method":"GET"}, ...]}
    // For this assignment, we only need strings and a flat array of objects.
    std::vector<std::pair<std::string, std::string>> out; // (method, path)
    const size_t arrKey = body.find("\"requests\"");
    if (arrKey == std::string::npos) return out;
    size_t lb = body.find('[', arrKey);
    size_t rb = body.find(']', lb == std::string::npos ? arrKey : lb);
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb) return out;
    std::string arr = body.substr(lb + 1, rb - (lb + 1));
    size_t pos = 0;
    while (true) {
        size_t ob = arr.find('{', pos);
        if (ob == std::string::npos) break;
        size_t cb = arr.find('}', ob + 1);
        if (cb == std::string::npos) break;
        const std::string obj = arr.substr(ob + 1, cb - (ob + 1));
        const auto path = ExtractJsonString(obj, "path");
        if (path && !path->empty()) {
            std::string method = "GET";
            const auto m = ExtractJsonString(obj, "method");
            if (m && !m->empty()) method = *m;
            out.push_back({method, *path});
        }
        pos = cb + 1;
    }
    return out;
}

struct ApiAggregateOp : std::enable_shared_from_this<ApiAggregateOp> {
    struct Result {
        std::string method;
        std::string path;
        std::string backend;
        int status{502};
        std::string body;
    };

    balancer::BackendManager* backendManager{nullptr};
    balancer::BackendConnectionPool* backendPool{nullptr};
    std::function<void()> resumeCb;
    std::weak_ptr<proxy::network::TcpConnection> client;
    std::shared_ptr<ProxySessionContext> ctx;
    std::string clientIp;
    std::string affinityKey;
    bool clientClose{true};
    std::chrono::steady_clock::time_point start{};

    std::vector<std::pair<std::string, std::string>> reqs; // (method,path)
    size_t idx{0};
    std::vector<Result> results;

    // current request
    proxy::network::InetAddress backendAddr{0};
    std::shared_ptr<balancer::BackendConnectionPool::Lease> lease;
    protocol::HttpResponseContext resp;
    std::string raw;

    size_t maxRespBytes{1024 * 1024};
    size_t maxBodyBytes{4096};

    void Start() {
        results.reserve(reqs.size());
        Next();
    }

    void FinishAndReply() {
        auto c = client.lock();
        if (!c || !c->connected()) {
            Cleanup();
            return;
        }
        std::string json = "{\"results\":[";
        for (size_t i = 0; i < results.size(); ++i) {
            if (i) json += ",";
            const auto& r = results[i];
            std::string body = r.body;
            if (body.size() > maxBodyBytes) body.resize(maxBodyBytes);
            json += "{";
            json += "\"method\":\"" + JsonEscape(r.method) + "\",";
            json += "\"path\":\"" + JsonEscape(r.path) + "\",";
            json += "\"backend\":\"" + JsonEscape(r.backend) + "\",";
            json += "\"status\":" + std::to_string(r.status) + ",";
            json += "\"body\":\"" + JsonEscape(body) + "\"";
            json += "}";
        }
        json += "]}";

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << json.size() << "\r\n"
            << "Connection: " << (clientClose ? "close" : "keep-alive") << "\r\n"
            << "\r\n"
            << json;
        c->Send(oss.str());
        if (clientClose) c->Shutdown();

        ctx->waitingBackendResponse = false;
        if (resumeCb) resumeCb();
        Cleanup();
    }

    void Cleanup() {
        if (lease) {
            lease->Release(false);
            lease.reset();
        }
    }

    void Next() {
        if (idx >= reqs.size()) {
            FinishAndReply();
            return;
        }
        auto c = client.lock();
        if (!c || !c->connected()) {
            Cleanup();
            return;
        }

        const auto [method, path] = reqs[idx];
        std::string p = path.empty() ? "/" : path;
        const std::string selectionKey = p + "#" + affinityKey;
        if (!backendManager || !backendPool) {
            FinishAndReply();
            return;
        }
        backendAddr = backendManager->SelectBackend(selectionKey);
        if (backendAddr.toIpPort() == "0.0.0.0:0") {
            Result r;
            r.method = method;
            r.path = p;
            r.backend = "-";
            r.status = 503;
            r.body = "Service Unavailable";
            results.push_back(std::move(r));
            ++idx;
            Next();
            return;
        }

        // Acquire backend connection and send request.
        auto self = shared_from_this();
        backendPool->Acquire(c->getLoop(), backendAddr,
                                      [self, method, p](std::shared_ptr<balancer::BackendConnectionPool::Lease> l) {
                                          auto c2 = self->client.lock();
                                          if (!c2 || !c2->connected()) {
                                              if (l) l->Release(false);
                                              self->Cleanup();
                                              return;
                                          }
                                          if (!l || !l->connection() || !l->connection()->connected()) {
                                              Result r;
                                              r.method = method;
                                              r.path = p;
                                              r.backend = self->backendAddr.toIpPort();
                                              r.status = 502;
                                              r.body = "Bad Gateway";
                                              self->results.push_back(std::move(r));
                                              ++self->idx;
                                              if (l) l->Release(false);
                                              self->Next();
                                              return;
                                          }
                                          self->lease = l;
                                          self->resp.reset();
                                          self->raw.clear();

                                          self->backendManager->OnBackendConnectionStart(self->backendAddr);
                                          auto backendConn = l->connection();

                                          auto done = std::make_shared<bool>(false);
                                          auto finalize = [self, backendConn, done](bool closed) mutable {
                                              if (*done) return;
                                              *done = true;

                                              Result r;
                                              r.method = self->reqs[self->idx].first;
                                              r.path = self->reqs[self->idx].second;
                                              r.backend = self->backendAddr.toIpPort();
                                              r.status = 502;
                                              r.body = "Bad Gateway";

                                              int status = 502;
                                              std::vector<protocol::Hpack::Header> hs;
                                              std::string body;
                                              std::string contentEnc;
                                              std::string statusLine;
                                              if (parseHttp1Response(self->raw, &status, &hs, &contentEnc, &statusLine, &body)) {
                                                  r.status = status;
                                                  r.body = body;
                                              }

                                              self->results.push_back(std::move(r));
                                              self->backendManager->OnBackendConnectionEnd(self->backendAddr);
                                              const bool keep = (!closed) && self->resp.keepAlive() && self->resp.gotAll();
                                              if (self->lease) {
                                                  self->lease->Release(keep);
                                                  self->lease.reset();
                                              }
                                              backendConn->SetMessageCallback({});
                                              backendConn->SetCloseCallback({});
                                              ++self->idx;
                                              self->Next();
                                          };

                                          backendConn->SetCloseCallback([finalize](const network::TcpConnectionPtr&) mutable { finalize(true); });
                                          backendConn->SetMessageCallback([self, finalize](const network::TcpConnectionPtr&,
                                                                                           network::Buffer* b,
                                                                                           std::chrono::system_clock::time_point) mutable {
                                              if (!b) return;
                                              const char* data = b->Peek();
                                              const size_t n = b->ReadableBytes();
                                              if (n > 0) {
                                                  self->raw.append(data, data + n);
                                                  self->resp.feed(data, n);
                                                  b->RetrieveAll();
                                                  if (self->raw.size() > self->maxRespBytes) {
                                                      finalize(false);
                                                      return;
                                                  }
                                              }
                                              if (self->resp.hasError()) {
                                                  finalize(false);
                                                  return;
                                              }
                                              if (self->resp.gotAll()) {
                                                  finalize(false);
                                                  return;
                                              }
                                          });

                                          std::string req;
                                          req.reserve(256);
                                          req += method + " " + p + " HTTP/1.1\r\n";
                                          req += "Host: " + self->backendAddr.toIpPort() + "\r\n";
                                          req += "Connection: Keep-Alive\r\n";
                                          req += "Accept-Encoding: identity\r\n";
                                          req += "\r\n";
                                          backendConn->Send(req);
                                      });
    }
};

struct BatchSplitOp : std::enable_shared_from_this<BatchSplitOp> {
    struct Result {
        int status{502};
        std::string body;
    };

    balancer::BackendManager* backendManager{nullptr};
    balancer::BackendConnectionPool* backendPool{nullptr};
    std::function<void()> resumeCb;
    std::weak_ptr<proxy::network::TcpConnection> client;
    std::shared_ptr<ProxySessionContext> ctx;

    proxy::network::InetAddress backendAddr{0};
    std::string route; // path + query
    std::string model;
    std::string modelVersion;
    bool clientClose{true};
    std::chrono::steady_clock::time_point start{};

    std::vector<std::string> items; // JSON values
    size_t idx{0};
    std::vector<Result> results;

    // current request
    std::shared_ptr<balancer::BackendConnectionPool::Lease> lease;
    protocol::HttpResponseContext resp;
    std::string raw;

    size_t maxRespBytes{1024 * 1024};
    size_t maxBodyBytes{16 * 1024};

    void Start() {
        results.reserve(items.size());
        Next();
    }

    void FinishAndReply() {
        auto c = client.lock();
        if (!c || !c->connected()) {
            Cleanup();
            return;
        }

        std::string json = "{\"results\":[";
        for (size_t i = 0; i < results.size(); ++i) {
            if (i) json += ",";
            const auto& r = results[i];
            std::string body = r.body;
            if (body.size() > maxBodyBytes) body.resize(maxBodyBytes);
            json += "{";
            json += "\"status\":" + std::to_string(r.status) + ",";
            json += "\"body\":\"" + JsonEscape(body) + "\"";
            json += "}";
        }
        json += "]}";

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << json.size() << "\r\n"
            << "Connection: " << (clientClose ? "close" : "keep-alive") << "\r\n"
            << "\r\n"
            << json;
        c->Send(oss.str());
        if (clientClose) c->Shutdown();

        ctx->waitingBackendResponse = false;
        if (resumeCb) resumeCb();
        Cleanup();
    }

    void Cleanup() {
        if (lease) {
            lease->Release(false);
            lease.reset();
        }
    }

    void Next() {
        if (idx >= items.size()) {
            FinishAndReply();
            return;
        }
        auto c = client.lock();
        if (!c || !c->connected()) {
            Cleanup();
            return;
        }
        if (!backendPool || !backendManager) {
            FinishAndReply();
            return;
        }
        if (backendAddr.toIpPort() == "0.0.0.0:0") {
            results.push_back(Result{503, "Service Unavailable"});
            ++idx;
            Next();
            return;
        }

        const std::string body = items[idx];
        auto self = shared_from_this();
        backendPool->Acquire(c->getLoop(), backendAddr,
                             [self, body](std::shared_ptr<balancer::BackendConnectionPool::Lease> l) {
                                 auto c2 = self->client.lock();
                                 if (!c2 || !c2->connected()) {
                                     if (l) l->Release(false);
                                     self->Cleanup();
                                     return;
                                 }
                                 if (!l || !l->connection() || !l->connection()->connected()) {
                                     self->results.push_back(Result{502, "Bad Gateway"});
                                     ++self->idx;
                                     if (l) l->Release(false);
                                     self->Next();
                                     return;
                                 }

                                 self->lease = l;
                                 self->resp.reset();
                                 self->raw.clear();
                                 self->backendManager->OnBackendConnectionStart(self->backendAddr);

                                 auto backendConn = l->connection();
                                 auto done = std::make_shared<bool>(false);
                                 auto finalize = [self, backendConn, done](bool closed) mutable {
                                     if (*done) return;
                                     *done = true;

                                     Result r;
                                     r.status = 502;
                                     r.body = "Bad Gateway";

                                     int status = 502;
                                     std::vector<protocol::Hpack::Header> hs;
                                     std::string body2;
                                     std::string contentEnc;
                                     std::string statusLine;
                                     if (parseHttp1Response(self->raw, &status, &hs, &contentEnc, &statusLine, &body2)) {
                                         r.status = status;
                                         r.body = body2;
                                     }
                                     self->results.push_back(std::move(r));

                                     self->backendManager->OnBackendConnectionEnd(self->backendAddr);
                                     const bool keep = (!closed) && self->resp.keepAlive() && self->resp.gotAll();
                                     if (self->lease) {
                                         self->lease->Release(keep);
                                         self->lease.reset();
                                     }

                                     ++self->idx;
                                     self->Next();
                                 };

                                 backendConn->SetCloseCallback([finalize](const network::TcpConnectionPtr&) mutable { finalize(true); });
                                 backendConn->SetMessageCallback([self, finalize](const network::TcpConnectionPtr&,
                                                                                  network::Buffer* b,
                                                                                  std::chrono::system_clock::time_point) mutable {
                                     if (!b) return;
                                     const char* data = b->Peek();
                                     const size_t n = b->ReadableBytes();
                                     if (n > 0) {
                                         self->raw.append(data, data + n);
                                         self->resp.feed(data, n);
                                         b->RetrieveAll();
                                         if (self->raw.size() > self->maxRespBytes) {
                                             finalize(false);
                                             return;
                                         }
                                     }
                                     if (self->resp.hasError()) {
                                         finalize(false);
                                         return;
                                     }
                                     if (self->resp.gotAll()) {
                                         finalize(false);
                                         return;
                                     }
                                 });

                                 std::string req;
                                 req.reserve(1024 + body.size());
                                 req += "POST " + self->route + " HTTP/1.1\r\n";
                                 req += "Host: " + self->backendAddr.toIpPort() + "\r\n";
                                 req += "Connection: Keep-Alive\r\n";
                                 req += "Accept-Encoding: identity\r\n";
                                 req += "Content-Type: application/json\r\n";
                                 if (!self->model.empty()) req += "X-Model: " + self->model + "\r\n";
                                 if (!self->modelVersion.empty()) req += "X-Model-Version: " + self->modelVersion + "\r\n";
                                 req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                                 req += "\r\n";
                                 req += body;
                                 backendConn->Send(req);
                             });
    }
};

} // namespace

class PriorityDispatcher : public std::enable_shared_from_this<PriorityDispatcher> {
public:
    explicit PriorityDispatcher(int maxInflight) : maxInflight_(maxInflight) {}

    void SetMaxInflight(int v) {
        std::lock_guard<std::mutex> lock(mu_);
        maxInflight_ = v;
    }

    void Enqueue(int prio, std::function<void()> startFn) {
        if (prio < 0) prio = 0;
        if (prio > 9) prio = 9;
        std::vector<std::function<void()>> toRun;
        {
            std::lock_guard<std::mutex> lock(mu_);
            queues_[static_cast<size_t>(prio)].push_back(std::move(startFn));
            DrainLocked(&toRun);
        }
        for (auto& f : toRun) f();
    }

    void OnTaskDone() {
        std::vector<std::function<void()>> toRun;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (inflight_ > 0) inflight_--;
            DrainLocked(&toRun);
        }
        for (auto& f : toRun) f();
    }

private:
    void DrainLocked(std::vector<std::function<void()>>* toRun) {
        if (!toRun) return;
        while (true) {
            if (maxInflight_ > 0 && inflight_ >= maxInflight_) return;
            std::function<void()> fn;
            bool found = false;
            for (int p = 9; p >= 0; --p) {
                auto& q = queues_[static_cast<size_t>(p)];
                if (q.empty()) continue;
                fn = std::move(q.front());
                q.pop_front();
                found = true;
                break;
            }
            if (!found) return;
            inflight_++;
            toRun->push_back(std::move(fn));
        }
    }

    std::mutex mu_;
    int maxInflight_{0};
    int inflight_{0};
    std::array<std::deque<std::function<void()>>, 10> queues_{};
};

class FairQueueDispatcher : public std::enable_shared_from_this<FairQueueDispatcher> {
public:
    explicit FairQueueDispatcher(int maxInflight) : maxInflight_(maxInflight) {}

    void SetMaxInflight(int v) {
        std::lock_guard<std::mutex> lock(mu_);
        maxInflight_ = v;
    }

    void Enqueue(const std::string& flow, std::function<void()> startFn) {
        std::vector<std::function<void()>> toRun;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto& st = flows_[flow];
            if (st.q.empty() && !st.active) {
                active_.push_back(flow);
                st.active = true;
            }
            st.q.push_back(std::move(startFn));
            DrainLocked(&toRun);
        }
        for (auto& f : toRun) f();
    }

    void OnTaskDone() {
        std::vector<std::function<void()>> toRun;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (inflight_ > 0) inflight_--;
            DrainLocked(&toRun);
        }
        for (auto& f : toRun) f();
    }

private:
    struct FlowState {
        std::deque<std::function<void()>> q;
        bool active{false};
    };

    void DrainLocked(std::vector<std::function<void()>>* toRun) {
        if (!toRun) return;
        while (true) {
            if (maxInflight_ > 0 && inflight_ >= maxInflight_) return;
            if (active_.empty()) return;

            // Round-robin: take front flow, dispatch one task, then rotate.
            std::string flow = std::move(active_.front());
            active_.pop_front();
            auto it = flows_.find(flow);
            if (it == flows_.end()) continue;
            auto& st = it->second;
            if (st.q.empty()) {
                st.active = false;
                flows_.erase(it);
                continue;
            }
            auto fn = std::move(st.q.front());
            st.q.pop_front();
            inflight_++;
            toRun->push_back(std::move(fn));

            if (!st.q.empty()) {
                active_.push_back(flow);
            } else {
                st.active = false;
                flows_.erase(flow);
            }
        }
    }

    std::mutex mu_;
    int maxInflight_{0};
    int inflight_{0};
    std::unordered_map<std::string, FlowState> flows_;
    std::deque<std::string> active_;
};

class EdfDispatcher : public std::enable_shared_from_this<EdfDispatcher> {
public:
    explicit EdfDispatcher(int maxInflight) : maxInflight_(maxInflight) {}

    void SetMaxInflight(int v) {
        std::lock_guard<std::mutex> lock(mu_);
        maxInflight_ = v;
    }

    void Enqueue(std::chrono::steady_clock::time_point deadline, std::function<void()> startFn) {
        std::vector<std::function<void()>> toRun;
        {
            std::lock_guard<std::mutex> lock(mu_);
            Item it;
            it.deadline = deadline;
            it.seq = seq_++;
            it.fn = std::move(startFn);
            heap_.push_back(std::move(it));
            std::push_heap(heap_.begin(), heap_.end(), Cmp{});
            DrainLocked(&toRun);
        }
        for (auto& f : toRun) f();
    }

    void OnTaskDone() {
        std::vector<std::function<void()>> toRun;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (inflight_ > 0) inflight_--;
            DrainLocked(&toRun);
        }
        for (auto& f : toRun) f();
    }

private:
    struct Item {
        std::chrono::steady_clock::time_point deadline{};
        uint64_t seq{0};
        std::function<void()> fn;
    };
    struct Cmp {
        bool operator()(const Item& a, const Item& b) const {
            if (a.deadline != b.deadline) return a.deadline > b.deadline; // min-heap via > with push_heap
            return a.seq > b.seq;
        }
    };

    void DrainLocked(std::vector<std::function<void()>>* toRun) {
        if (!toRun) return;
        while (true) {
            if (maxInflight_ > 0 && inflight_ >= maxInflight_) return;
            if (heap_.empty()) return;
            std::pop_heap(heap_.begin(), heap_.end(), Cmp{});
            Item it = std::move(heap_.back());
            heap_.pop_back();
            inflight_++;
            toRun->push_back(std::move(it.fn));
        }
    }

    std::mutex mu_;
    int maxInflight_{0};
    int inflight_{0};
    uint64_t seq_{0};
    std::vector<Item> heap_;
};

ProxyServer::ProxyServer(network::EventLoop* loop, uint16_t port, const std::string& strategy, const std::string& name, bool reusePort)
    : loop_(loop),
      server_(loop,
              network::InetAddress(port),
              name,
              reusePort ? network::TcpServer::kReusePort : network::TcpServer::kNoReusePort),
      reusePort_(reusePort),
      backendManager_(loop, strategy),
      backendPool_(std::make_unique<balancer::BackendConnectionPool>()) {
    
    server_.SetConnectionCallback(
        std::bind(&ProxyServer::OnConnection, this, std::placeholders::_1));
    
    server_.SetMessageCallback(
        std::bind(&ProxyServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

}

void ProxyServer::ConfigureL4Tunnel(uint16_t listenPort) {
    l4ListenPort_ = listenPort;
    if (l4ListenPort_ == 0) {
        l4Server_.reset();
        return;
    }
    if (!l4Server_) {
        l4Server_ = std::make_unique<network::TcpServer>(
            loop_,
            network::InetAddress(l4ListenPort_),
            "L4Tunnel",
            reusePort_ ? network::TcpServer::kReusePort : network::TcpServer::kNoReusePort);
        l4Server_->SetConnectionCallback(std::bind(&ProxyServer::OnConnectionL4, this, std::placeholders::_1));
        l4Server_->SetMessageCallback(
            std::bind(&ProxyServer::OnMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }
}

void ProxyServer::ConfigureHttpBatching(const protocol::HttpBatcher::Config& cfg) {
    std::lock_guard<std::mutex> lock(batchMu_);
    batchCfg_ = cfg;
    batchers_.clear(); // apply new config
}

void ProxyServer::ConfigureRewriteRules(const std::vector<protocol::RewriteRule>& rules) {
    rewrite_.SetRules(rules);
}

void ProxyServer::ConfigureTrafficMirror(const protocol::TrafficMirror::Config& cfg) {
    mirror_.Configure(cfg);
}

void ProxyServer::ConfigureCache(const protocol::Cache::Config& cfg) {
    cache_.Configure(cfg);
}

bool ProxyServer::EnableTls(const std::string& certPemPath, const std::string& keyPemPath) {
    return server_.EnableTls(certPemPath, keyPemPath);
}

void ProxyServer::SetAcmeChallengeDir(const std::string& dir) {
    acmeChallengeDir_ = dir;
}

void ProxyServer::ConfigureHistory(const monitor::HistoryStore::Config& cfg) {
    if (!cfg.enabled) {
        if (history_) {
            history_->Stop();
            history_.reset();
        }
        return;
    }
    if (!history_) {
        history_ = std::make_unique<monitor::HistoryStore>(loop_, cfg);
    }
    history_->Start();
}

void ProxyServer::ConfigurePlugins(const common::PluginManager::Config& cfg) {
    if (!plugins_) plugins_ = std::make_unique<common::PluginManager>();
    const bool ok = plugins_->LoadAll(cfg);
    if (!ok) {
        LOG_WARN << "One or more plugins failed to load";
    }
}

void ProxyServer::ConfigurePriorityScheduling(const PriorityConfig& cfg) {
    std::lock_guard<std::mutex> lock(prioMu_);
    prioCfg_ = cfg;
}

std::shared_ptr<protocol::HttpBatcher> ProxyServer::GetOrCreateBatcher(network::EventLoop* loop) {
    if (!loop || !backendPool_ || !batchCfg_.enabled) return nullptr;
    std::lock_guard<std::mutex> lock(batchMu_);
    auto it = batchers_.find(loop);
    if (it != batchers_.end()) return it->second;

    auto cfg = batchCfg_;
    auto b = std::make_shared<protocol::HttpBatcher>(
        loop,
        backendPool_.get(),
        &backendManager_,
        cfg,
        [this](const network::TcpConnectionPtr& c) {
            if (!c || !c->connected()) return;
            try {
                auto* ctxPtr = std::any_cast<std::shared_ptr<ProxySessionContext>>(c->GetMutableContext());
                if (!ctxPtr || !*ctxPtr) return;
                auto& ctx = *ctxPtr;
                if (ctx->waitingBackendResponse) return;
                if (ctx->buffer.empty()) return;
                auto pending = std::make_shared<std::string>(std::move(ctx->buffer));
                ctx->buffer.clear();
                c->getLoop()->QueueInLoop([this, c, pending]() {
                    if (!c->connected()) return;
                    network::Buffer bb;
                    bb.Append(*pending);
                    this->OnMessage(c, &bb, std::chrono::system_clock::now());
                });
            } catch (const std::bad_any_cast&) {
            }
        });
    batchers_[loop] = b;
    return b;
}

void ProxyServer::ConfigureHealthCheck(const std::string& mode,
                                       double timeoutSec,
                                       const std::string& httpHost,
                                       const std::string& httpPath,
                                       const std::string& scriptCmd) {
    backendManager_.ConfigureHealthCheck(mode, timeoutSec, httpHost, httpPath, scriptCmd);
}

void ProxyServer::StartHealthCheck(double intervalSec) {
    backendManager_.StartHealthCheck(intervalSec);
}

void ProxyServer::ConfigureAiServiceCheck(double timeoutSec, const std::string& httpHost, const std::string& httpPath) {
    backendManager_.ConfigureAiServiceCheck(timeoutSec, httpHost, httpPath);
}

void ProxyServer::StartAiServiceCheck(double intervalSec) {
    backendManager_.StartAiServiceCheck(intervalSec);
}

void ProxyServer::ConfigureWarmup(bool enable,
                                  const std::string& model,
                                  double timeoutSec,
                                  const std::string& httpHost,
                                  const std::string& httpPath) {
    backendManager_.ConfigureWarmup(enable, model, timeoutSec, httpHost, httpPath);
}

void ProxyServer::EnableRateLimit(double qps, double burstTokens) {
    if (qps <= 0.0) {
        requestRateLimiter_.reset();
        return;
    }
    if (burstTokens <= 0.0) {
        burstTokens = qps; // reasonable default burst
    }
    requestRateLimiter_ = std::make_unique<monitor::TokenBucket>(qps, burstTokens);
}

void ProxyServer::ConfigureCongestionControl(const monitor::CongestionControl::Config& cfg) {
    if (!cfg.enabled) {
        congestion_.reset();
        return;
    }
    congestion_ = std::make_unique<monitor::CongestionControl>(cfg);
}

void ProxyServer::SetConnectionLimits(int maxConnections, int maxConnectionsPerIp) {
    server_.SetMaxConnections(maxConnections);
    server_.SetMaxConnectionsPerIp(maxConnectionsPerIp);
}

void ProxyServer::SetIdleTimeout(double idleTimeoutSec, double cleanupIntervalSec) {
    server_.SetIdleTimeout(idleTimeoutSec, cleanupIntervalSec);
}

void ProxyServer::SetAcceptRateLimit(double qps, double burst) {
    server_.SetAcceptRateLimit(qps, burst);
}

void ProxyServer::SetPerIpAcceptRateLimit(double qps, double burst, double idleSec, size_t maxEntries) {
    server_.SetPerIpAcceptRateLimit(qps, burst, idleSec, maxEntries);
}

void ProxyServer::SetSessionAffinity(const std::string& mode, const std::string& headerName, const std::string& cookieName) {
    affinityMode_ = mode;
    affinityHeader_ = headerName;
    affinityCookie_ = cookieName;
}

void ProxyServer::SetAccessControl(const monitor::AccessControl::Config& cfg) {
    accessControl_ = std::make_unique<monitor::AccessControl>(cfg);
}

void ProxyServer::EnableAuditLog(const std::string& path) {
    if (path.empty()) {
        auditLogger_.reset();
        return;
    }
    auditLogger_ = std::make_unique<monitor::AuditLogger>(path);
}

void ProxyServer::EnablePerIpRateLimit(double qps, double burst, double idleSec, size_t maxEntries) {
    if (qps <= 0.0) {
        perIpRateLimiter_.reset();
        return;
    }
    monitor::PerKeyRateLimiter::Config cfg;
    cfg.qps = qps;
    cfg.burst = burst;
    cfg.idleSec = idleSec;
    cfg.maxEntries = maxEntries;
    perIpRateLimiter_ = std::make_unique<monitor::PerKeyRateLimiter>(cfg);
}

void ProxyServer::EnablePerPathRateLimit(double qps, double burst, double idleSec, size_t maxEntries) {
    if (qps <= 0.0) {
        perPathRateLimiter_.reset();
        return;
    }
    monitor::PerKeyRateLimiter::Config cfg;
    cfg.qps = qps;
    cfg.burst = burst;
    cfg.idleSec = idleSec;
    cfg.maxEntries = maxEntries;
    perPathRateLimiter_ = std::make_unique<monitor::PerKeyRateLimiter>(cfg);
}

void ProxyServer::SetMaxConnectionsPerUser(int maxConnections, const std::string& headerName, size_t maxEntries) {
    if (maxConnections <= 0) {
        perUserConnLimiter_.reset();
        return;
    }
    monitor::PerKeyConnectionLimiter::Config cfg;
    cfg.maxConnections = maxConnections;
    cfg.maxEntries = maxEntries;
    userConnHeader_ = headerName.empty() ? "X-Api-Token" : headerName;
    perUserConnLimiter_ = std::make_unique<monitor::PerKeyConnectionLimiter>(cfg);
}

void ProxyServer::SetMaxConnectionsPerService(int maxConnections, size_t maxEntries) {
    if (maxConnections <= 0) {
        perServiceConnLimiter_.reset();
        return;
    }
    monitor::PerKeyConnectionLimiter::Config cfg;
    cfg.maxConnections = maxConnections;
    cfg.maxEntries = maxEntries;
    perServiceConnLimiter_ = std::make_unique<monitor::PerKeyConnectionLimiter>(cfg);
}

void ProxyServer::Start() {
    LOG_INFO << "ProxyServer starts listening on " << server_.hostport();
    server_.Start();
    if (l4Server_ && l4ListenPort_ != 0) {
        LOG_INFO << "L4Tunnel starts listening on " << l4Server_->hostport();
        l4Server_->Start();
    }
}

void ProxyServer::SetThreadNum(int numThreads) {
    server_.SetThreadNum(numThreads);
    if (l4Server_) l4Server_->SetThreadNum(numThreads);
}

void ProxyServer::AddBackend(const std::string& ip, uint16_t port, int weight) {
    backendManager_.AddBackend(ip, port, weight);
    LOG_INFO << "Added backend: " << ip << ":" << port << " weight=" << weight;
}

bool ProxyServer::UpdateBackendMetrics(const std::string& id,
                                       int queueLen,
                                       double gpuUtil01,
                                       int vramUsedMb,
                                       int vramTotalMb) {
    return backendManager_.UpdateBackendMetrics(id, queueLen, gpuUtil01, vramUsedMb, vramTotalMb);
}

void ProxyServer::EnableAutoWeightAdjust(bool on) {
    backendManager_.EnableAutoWeightAdjust(on);
}

void ProxyServer::OnConnection(const network::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO << "New connection from " << conn->peerAddress().toIpPort();
        monitor::Stats::Instance().IncActiveConnections();
        
        // Initialize Context for L7 Parsing
        auto ctx = std::make_shared<ProxySessionContext>();
        ctx->type = ProxySessionContext::kHttp; 
        conn->SetContext(ctx);
        
    } else {
        LOG_INFO << "Connection closed: " << conn->name();
        monitor::Stats::Instance().DecActiveConnections();

        // Release per-user/per-service connection limits, if acquired.
	        if (conn->GetMutableContext()->has_value()) {
	            try {
	                auto* ctxPtr = std::any_cast<std::shared_ptr<ProxySessionContext>>(conn->GetMutableContext());
	                if (ctxPtr && *ctxPtr) {
	                    auto& ctx = *ctxPtr;
                        if (!ctx->http2Pending.empty()) {
                            for (auto& kv : ctx->http2Pending) {
                                if (kv.second && kv.second->lease) {
                                    kv.second->lease->Release(false);
                                }
                            }
                            ctx->http2Pending.clear();
                        }
	                    if (ctx->connLimitApplied) {
	                        if (perUserConnLimiter_ && !ctx->userKey.empty()) {
	                            perUserConnLimiter_->Release(ctx->userKey);
	                        }
                        if (perServiceConnLimiter_ && !ctx->serviceKey.empty()) {
                            perServiceConnLimiter_->Release(ctx->serviceKey);
                        }
                        ctx->connLimitApplied = false;
                    }
                }
            } catch (const std::bad_any_cast&) {
                // ignore
            }
        }
    }
}

void ProxyServer::OnConnectionL4(const network::TcpConnectionPtr& conn) {
    if (!conn) return;
    if (conn->connected()) {
        LOG_INFO << "New L4 tunnel connection from " << conn->peerAddress().toIpPort();
        monitor::Stats::Instance().IncActiveConnections();

        auto ctx = std::make_shared<ProxySessionContext>();
        ctx->type = ProxySessionContext::kTunnel;
        conn->SetContext(ctx);

        const std::string selectionKey = std::string("l4#") + conn->peerAddress().toIpPort();
        network::InetAddress backendAddr = backendManager_.SelectBackend(selectionKey);
        if (backendAddr.toIpPort() == "0.0.0.0:0") {
            conn->Shutdown();
            return;
        }
        balancer::BackendSession::TunnelConfig tcfg;
        tcfg.enableBackpressure = true;
        tcfg.pauseClientReadUntilBackendConnected = true;
        tcfg.highWaterMarkBytes = 8 * 1024 * 1024;
        ctx->backendSession = std::make_shared<balancer::BackendSession>(
            conn->getLoop(),
            backendAddr.toIp(),
            backendAddr.toPort(),
            conn,
            &backendManager_,
            tcfg);
        ctx->backendSession->Start();
        return;
    }
    LOG_INFO << "L4 tunnel connection closed: " << conn->name();
    monitor::Stats::Instance().DecActiveConnections();
}

void ProxyServer::OnMessage(const network::TcpConnectionPtr& conn,
                            network::Buffer* buf,
                            std::chrono::system_clock::time_point receiveTime) {
    if (!conn->GetMutableContext()->has_value()) {
        buf->RetrieveAll();
        return;
    }

    try {
        // any_cast on pointer returns pointer to contained value
        auto* ctxPtr = std::any_cast<std::shared_ptr<ProxySessionContext>>(conn->GetMutableContext());
        if (!ctxPtr || !*ctxPtr) {
            LOG_ERROR << "Empty context ptr";
            buf->RetrieveAll();
            return;
        }
        auto& ctx = *ctxPtr; // Reference to shared_ptr
        
	        if (ctx->type == ProxySessionContext::kTunnel) {
	            // L4 Forwarding Mode
	            if (ctx->backendSession) {
	                if (buf->ReadableBytes() > 0) {
	                    ctx->backendSession->Send(buf->Peek(), buf->ReadableBytes());
	                    buf->RetrieveAll();
	                }
	            }
	        } 
            else if (ctx->type == ProxySessionContext::kHttp2) {
                if (buf->ReadableBytes() > 0) {
                    ctx->http2.OnData(reinterpret_cast<const uint8_t*>(buf->Peek()), buf->ReadableBytes());
                    buf->RetrieveAll();
                }
            }
	        else if (ctx->type == ProxySessionContext::kHttp) {
	            // L7 Parsing Mode
                // HTTP/2 prior-knowledge (h2c): connection preface starts the stream.
                if (!ctx->waitingBackendResponse && buf->ReadableBytes() > 0 && isHttp2PrefacePrefix(buf->Peek(), buf->ReadableBytes())) {
                    ctx->type = ProxySessionContext::kHttp2;
                    ctx->http2.Reset();
                    auto weakConn = std::weak_ptr<network::TcpConnection>(conn);
                    ctx->http2.SetSendCallback([weakConn](const void* data, size_t len) {
                        auto c = weakConn.lock();
                        if (!c || !c->connected() || !data || len == 0) return;
                        c->Send(data, len);
                    });
	                    ctx->http2.SetRequestCallback([this, weakConn, ctx](const protocol::Http2Connection::Request& r) {
	                        auto c = weakConn.lock();
	                        if (!c || !c->connected()) return;
	
	                        const auto reqStart = std::chrono::steady_clock::now();

                            // gRPC over HTTP/2 (h2c): handle a tiny built-in Echo service for this assignment.
                            // This provides protobuf serialization + server-streaming semantics without external deps.
                            const std::string ct = getH2HeaderCI(r.headers, "content-type");
	                            if (icontains(ct, "application/grpc")) {
	                                std::vector<std::string> msgs;
	                                if (!protocol::GrpcFramer::DecodeMessages(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size(), &msgs) ||
	                                    msgs.empty()) {
	                                    ctx->http2.SendHeaders(r.streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                    ctx->http2.SendTrailers(r.streamId, {{"grpc-status", "13"}});
	                                    return;
	                                }

                                std::string inText;
	                                if (!protocol::ProtobufLite::DecodeStringField1(reinterpret_cast<const uint8_t*>(msgs[0].data()), msgs[0].size(), &inText)) {
	                                    ctx->http2.SendHeaders(r.streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                    ctx->http2.SendTrailers(r.streamId, {{"grpc-status", "13"}});
	                                    return;
	                                }

	                                if (r.path == "/proxy.Gateway/HttpUnary") {
	                                    // gRPC -> HTTP/1.1 conversion: request string carries backend path.
	                                    const std::string backendPath = inText.empty() ? "/" : inText;

	                                    std::string affinityKey;
	                                    if (affinityMode_ == "ip") {
	                                        affinityKey = c->peerAddress().toIp();
	                                    } else if (affinityMode_ == "header") {
	                                        if (!affinityHeader_.empty()) affinityKey = getH2HeaderCI(r.headers, affinityHeader_);
	                                    } else if (affinityMode_ == "cookie") {
	                                        if (!affinityCookie_.empty()) {
	                                            const std::string cookie = getH2HeaderCI(r.headers, "cookie");
	                                            affinityKey = protocol::GetCookieValue(cookie, affinityCookie_);
	                                        }
	                                    }
	                                    if (affinityKey.empty()) affinityKey = c->peerAddress().toIp();

	                                    std::string selectionKey = backendPath + "#" + affinityKey;
	                                    network::InetAddress backendAddr = backendManager_.SelectBackend(selectionKey);
	                                    if (backendAddr.toIpPort() == "0.0.0.0:0") {
	                                        ctx->http2.SendHeaders(r.streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                        ctx->http2.SendTrailers(r.streamId, {{"grpc-status", "14"}});
	                                        monitor::Stats::Instance().IncBackendFailures();
	                                        return;
	                                    }

	                                    std::string out;
	                                    out.reserve(256);
	                                    out += "GET " + backendPath + " HTTP/1.1\r\n";
	                                    out += "Host: " + backendAddr.toIpPort() + "\r\n";
	                                    out += "Connection: Keep-Alive\r\n";
	                                    out += "\r\n";

	                                    auto pending = std::make_shared<ProxySessionContext::Http2Pending>();
	                                    pending->backendAddr = backendAddr;
	                                    pending->resp.reset();
	                                    pending->raw.clear();
	                                    pending->firstByteRecorded = false;
	                                    pending->start = reqStart;
	                                    ctx->http2Pending[r.streamId] = pending;

	                                    backendPool_->Acquire(c->getLoop(), backendAddr,
	                                                          [this, weakConn, ctx, pending, streamId = r.streamId, backendAddr, out](std::shared_ptr<balancer::BackendConnectionPool::Lease> lease) {
	                                                              auto c2 = weakConn.lock();
	                                                              if (!c2 || !c2->connected()) {
	                                                                  if (lease) lease->Release(false);
	                                                                  return;
	                                                              }
	                                                              if (!lease || !lease->connection() || !lease->connection()->connected()) {
	                                                                  ctx->http2.SendHeaders(streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                                                  ctx->http2.SendTrailers(streamId, {{"grpc-status", "14"}});
	                                                                  monitor::Stats::Instance().IncBackendFailures();
	                                                                  if (lease) lease->Release(false);
	                                                                  ctx->http2Pending.erase(streamId);
	                                                                  return;
	                                                              }

	                                                              pending->lease = lease;
	                                                              backendManager_.OnBackendConnectionStart(backendAddr);
	                                                              auto backendConn = lease->connection();

	                                                              auto done = std::make_shared<bool>(false);
	                                                              auto finalize = [this, ctx, pending, lease, backendConn, backendAddr, streamId, done](bool closed) mutable {
	                                                                  if (*done) return;
	                                                                  *done = true;

	                                                                  int status = 502;
	                                                                  std::vector<protocol::Hpack::Header> hs;
	                                                                  std::string body;
	                                                                  std::string contentEnc;
	                                                                  std::string statusLine;
	                                                                  if (!parseHttp1Response(pending->raw, &status, &hs, &contentEnc, &statusLine, &body)) {
	                                                                      body.clear();
	                                                                  }

	                                                                  std::string pb;
	                                                                  protocol::ProtobufLite::EncodeStringField1(body, &pb);
	                                                                  std::string frame;
	                                                                  protocol::GrpcFramer::EncodeMessage(pb, &frame);
	                                                                  ctx->http2.SendHeaders(streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                                                  ctx->http2.SendData(streamId, frame, false);
	                                                                  ctx->http2.SendTrailers(streamId, {{"grpc-status", (status >= 200 && status < 400) ? "0" : "13"}});

	                                                                  backendManager_.OnBackendConnectionEnd(backendAddr);
	                                                                  const bool keep = (!closed) && pending->resp.keepAlive() && pending->resp.gotAll();
	                                                                  if (lease) lease->Release(keep);
	                                                                  ctx->http2Pending.erase(streamId);
	                                                                  backendConn->SetMessageCallback({});
	                                                                  backendConn->SetCloseCallback({});
	                                                              };

	                                                              backendConn->SetCloseCallback([finalize](const network::TcpConnectionPtr&) mutable { finalize(true); });
	                                                              backendConn->SetMessageCallback([pending, finalize](const network::TcpConnectionPtr&,
	                                                                                                                 network::Buffer* b,
	                                                                                                                 std::chrono::system_clock::time_point) mutable {
	                                                                  if (!b) return;
	                                                                  const char* data = b->Peek();
	                                                                  const size_t n = b->ReadableBytes();
	                                                                  if (n > 0) {
	                                                                      pending->raw.append(data, data + n);
	                                                                      pending->resp.feed(data, n);
	                                                                      b->RetrieveAll();
	                                                                  }
	                                                                  if (pending->resp.hasError()) {
	                                                                      finalize(false);
	                                                                      return;
	                                                                  }
	                                                                  if (pending->resp.gotAll()) {
	                                                                      finalize(false);
	                                                                      return;
	                                                                  }
	                                                              });

	                                                              backendConn->Send(out);
	                                                          });
	                                    return;
	                                }

	                                if (r.path == "/proxy.Echo/Unary") {
	                                    std::string pb;
	                                    protocol::ProtobufLite::EncodeStringField1(GrpcEchoUnaryReply(inText), &pb);
	                                    std::string frame;
	                                    protocol::GrpcFramer::EncodeMessage(pb, &frame);
	                                    ctx->http2.SendHeaders(r.streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                    ctx->http2.SendData(r.streamId, frame, false);
	                                    ctx->http2.SendTrailers(r.streamId, {{"grpc-status", "0"}});
	                                    return;
	                                }

	                                if (r.path == "/proxy.Echo/Stream") {
	                                    ctx->http2.SendHeaders(r.streamId, 200, {{"content-type", "application/grpc"}}, false);
	                                    const auto parts = GrpcEchoStreamReply(inText);
	                                    for (const auto& part : parts) {
	                                        std::string pb;
	                                        protocol::ProtobufLite::EncodeStringField1(part, &pb);
	                                        std::string frame;
	                                        protocol::GrpcFramer::EncodeMessage(pb, &frame);
	                                        ctx->http2.SendData(r.streamId, frame, false);
	                                    }
	                                    ctx->http2.SendTrailers(r.streamId, {{"grpc-status", "0"}});
	                                    return;
	                                }

                                // Unimplemented method.
                                ctx->http2.SendHeaders(r.streamId, 200, {{"content-type", "application/grpc"}}, false);
                                ctx->http2.SendTrailers(r.streamId, {{"grpc-status", "12"}});
                                return;
                            }
	
	                        std::string fullPath = r.path.empty() ? "/" : r.path;
	                        std::string pathOnly = fullPath;
	                        std::string query;
                        const auto qpos = fullPath.find('?');
                        if (qpos != std::string::npos) {
                            pathOnly = fullPath.substr(0, qpos);
                            query = fullPath.substr(qpos);
                        }

                        std::string model = getH2HeaderCI(r.headers, "x-model");
                        if (model.empty()) model = ExtractQueryParam(query, "model");
                        std::string modelVersion = getH2HeaderCI(r.headers, "x-model-version");
                        if (modelVersion.empty()) modelVersion = ExtractQueryParam(query, "version");
                        if (modelVersion.empty()) modelVersion = ExtractQueryParam(query, "model_version");

                        std::string affinityKey;
                        if (affinityMode_ == "ip") {
                            affinityKey = c->peerAddress().toIp();
                        } else if (affinityMode_ == "header") {
                            if (!affinityHeader_.empty()) affinityKey = getH2HeaderCI(r.headers, affinityHeader_);
                        } else if (affinityMode_ == "cookie") {
                            if (!affinityCookie_.empty()) {
                                const std::string cookie = getH2HeaderCI(r.headers, "cookie");
                                affinityKey = protocol::GetCookieValue(cookie, affinityCookie_);
                            }
                        }
                        if (affinityKey.empty()) affinityKey = c->peerAddress().toIp();

                        std::string selectionKey = pathOnly + "#" + affinityKey;
                        if (!model.empty()) selectionKey += "#model:" + model;
                        if (!modelVersion.empty()) selectionKey += "#ver:" + modelVersion;
                        network::InetAddress backendAddr;
                        if (!modelVersion.empty()) {
                            backendAddr = backendManager_.SelectBackendForModelVersion(selectionKey, model, modelVersion);
                        } else {
                            backendAddr = model.empty() ? backendManager_.SelectBackend(selectionKey)
                                                      : backendManager_.SelectBackendForModel(selectionKey, model);
                        }
                        if (backendAddr.toIpPort() == "0.0.0.0:0") {
                            ctx->http2.SendResponse(r.streamId, 503, {{"content-type", "text/plain"}}, "Service Unavailable\n");
                            monitor::Stats::Instance().IncBackendFailures();
                            return;
                        }

	                        // Build HTTP/1.1 request for backend (normalize compression).
	                        std::string bodyToSend = r.body;
	                        std::vector<protocol::Hpack::Header> reqHs = r.headers;
	                        {
	                            const std::string ce = getH2HeaderCI(reqHs, "content-encoding");
	                            const auto enc = protocol::Compression::ParseContentEncoding(ce);
	                            if (enc == protocol::Compression::Encoding::kGzip || enc == protocol::Compression::Encoding::kDeflate) {
	                                std::string dec;
	                                if (!protocol::Compression::Decompress(enc, bodyToSend, &dec)) {
	                                    ctx->http2.SendResponse(r.streamId, 400, {{"content-type", "text/plain"}}, "Bad Request\n");
	                                    return;
	                                }
	                                bodyToSend = std::move(dec);
	                            }
	                        }

	                        const int rr = rewrite_.MatchHttp2(r.method, pathOnly);
	                        if (rr >= 0) {
	                            rewrite_.ApplyRequestHttp2(rr, &reqHs, &bodyToSend);
	                        }

	                        mirror_.MirrorRequestHttp2(c->peerAddress().toIp(),
	                                                   backendAddr.toIpPort(),
	                                                   r.streamId,
	                                                   r.method,
	                                                   pathOnly + query,
	                                                   reqHs,
	                                                   bodyToSend);

	                        std::string out;
	                        out.reserve(1024 + bodyToSend.size());
	                        out += r.method + " " + pathOnly + query + " HTTP/1.1\r\n";
	                        bool hasHost = false;
	                        for (const auto& h : reqHs) {
	                            if (!h.name.empty() && h.name[0] == ':') continue;
	                            if (iequals(h.name, "Host")) hasHost = true;
	                            if (iequals(h.name, "Connection") || iequals(h.name, "Proxy-Connection") || iequals(h.name, "Keep-Alive")) continue;
	                            if (iequals(h.name, "Transfer-Encoding")) continue;
                            if (iequals(h.name, "Content-Length")) continue;
                            if (iequals(h.name, "Content-Encoding")) continue;
                            if (iequals(h.name, "Accept-Encoding")) continue;
                            out += h.name + ": " + h.value + "\r\n";
                        }
                        if (!hasHost) {
                            if (!r.authority.empty()) out += "Host: " + r.authority + "\r\n";
                            else out += "Host: " + backendAddr.toIpPort() + "\r\n";
                        }
                        out += "X-Forwarded-For: " + c->peerAddress().toIp() + "\r\n";
                        out += "Connection: Keep-Alive\r\n";
                        out += "Accept-Encoding: identity\r\n";
                        if (!bodyToSend.empty()) {
                            out += "Content-Length: " + std::to_string(bodyToSend.size()) + "\r\n";
                        }
                        out += "\r\n";
	                        if (!bodyToSend.empty()) out += bodyToSend;

	                        auto pending = std::make_shared<ProxySessionContext::Http2Pending>();
	                        pending->backendAddr = backendAddr;
	                        pending->resp.reset();
	                        pending->raw.clear();
	                        pending->clientAcceptEncoding = getH2HeaderCI(reqHs, "accept-encoding");
	                        pending->rewriteRuleIdx = rr;
	                        pending->clientIp = c->peerAddress().toIp();
	                        pending->mirrorMethod = r.method;
	                        pending->mirrorPath = pathOnly + query;
	                        pending->firstByteRecorded = false;
	                        pending->start = reqStart;
	                        ctx->http2Pending[r.streamId] = pending;

                        backendPool_->Acquire(c->getLoop(), backendAddr,
                                              [this, weakConn, ctx, pending, streamId = r.streamId, backendAddr, out](std::shared_ptr<balancer::BackendConnectionPool::Lease> lease) {
                                                  auto c2 = weakConn.lock();
                                                  if (!c2 || !c2->connected()) {
                                                      if (lease) lease->Release(false);
                                                      return;
                                                  }
                                                  if (!lease || !lease->connection() || !lease->connection()->connected()) {
                                                      ctx->http2.SendResponse(streamId, 502, {{"content-type", "text/plain"}}, "Bad Gateway\n");
                                                      monitor::Stats::Instance().IncBackendFailures();
                                                      if (lease) lease->Release(false);
                                                      ctx->http2Pending.erase(streamId);
                                                      return;
                                                  }

                                                  pending->lease = lease;
                                                  backendManager_.OnBackendConnectionStart(backendAddr);
                                                  auto backendConn = lease->connection();

                                                  auto done = std::make_shared<bool>(false);
                                                  auto finalize = [this, ctx, pending, lease, backendConn, backendAddr, streamId, done](bool closed) mutable {
                                                      if (*done) return;
                                                      *done = true;

	                                                      int status = 502;
	                                                      std::vector<protocol::Hpack::Header> hs;
	                                                      std::string body;
	                                                      std::string contentEnc;
	                                                      std::string statusLine;
	                                                      if (parseHttp1Response(pending->raw, &status, &hs, &contentEnc, &statusLine, &body)) {
	                                                          const auto backendEnc = protocol::Compression::ParseContentEncoding(contentEnc);
	                                                          const auto desired = ChooseEncodingFromAccept(pending->clientAcceptEncoding);
	                                                          const int ruleIdx = pending->rewriteRuleIdx;
	                                                          const bool needRewrite =
	                                                              (ruleIdx >= 0) &&
	                                                              (static_cast<size_t>(ruleIdx) < rewrite_.rules().size()) &&
	                                                              (rewrite_.rules()[static_cast<size_t>(ruleIdx)].HasResponseMutations());

	                                                          std::vector<protocol::Hpack::Header> outHs = hs;
	                                                          std::string outBody = body;
	                                                          protocol::Compression::Encoding outEnc = backendEnc;

	                                                          if (needRewrite) {
	                                                              bool canRewriteBody = true;
	                                                              if (backendEnc == protocol::Compression::Encoding::kGzip || backendEnc == protocol::Compression::Encoding::kDeflate) {
	                                                                  std::string dec;
	                                                                  if (!protocol::Compression::Decompress(backendEnc, outBody, &dec)) {
	                                                                      canRewriteBody = false;
	                                                                  } else {
	                                                                      outBody = std::move(dec);
	                                                                      outEnc = protocol::Compression::Encoding::kIdentity;
	                                                                  }
	                                                              } else if (backendEnc == protocol::Compression::Encoding::kUnknown) {
	                                                                  outEnc = protocol::Compression::Encoding::kIdentity;
	                                                              }

	                                                              if (canRewriteBody && outEnc == protocol::Compression::Encoding::kIdentity) {
	                                                                  rewrite_.ApplyResponse(ruleIdx, &outHs, &outBody);
	                                                                  if (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate) {
	                                                                      std::string comp;
	                                                                      if (protocol::Compression::Compress(desired, outBody, &comp)) {
	                                                                          outBody = std::move(comp);
	                                                                          outEnc = desired;
	                                                                      }
	                                                                  } else {
	                                                                      outEnc = protocol::Compression::Encoding::kIdentity;
	                                                                  }
	                                                              } else {
	                                                                  rewrite_.ApplyResponse(ruleIdx, &outHs, nullptr);
	                                                                  outBody = body;
	                                                                  outEnc = backendEnc;
	                                                                  if ((backendEnc == protocol::Compression::Encoding::kGzip || backendEnc == protocol::Compression::Encoding::kDeflate) &&
	                                                                      desired != backendEnc) {
	                                                                      std::string dec;
	                                                                      if (protocol::Compression::Decompress(backendEnc, outBody, &dec)) {
	                                                                          outBody = std::move(dec);
	                                                                          outEnc = protocol::Compression::Encoding::kIdentity;
	                                                                      }
	                                                                  } else if ((backendEnc == protocol::Compression::Encoding::kIdentity || backendEnc == protocol::Compression::Encoding::kUnknown) &&
	                                                                             (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate)) {
	                                                                      std::string comp;
	                                                                      if (protocol::Compression::Compress(desired, outBody, &comp)) {
	                                                                          outBody = std::move(comp);
	                                                                          outEnc = desired;
	                                                                      }
	                                                                  }
	                                                              }
	                                                          } else {
	                                                              if (backendEnc != protocol::Compression::Encoding::kIdentity &&
	                                                                  backendEnc != protocol::Compression::Encoding::kUnknown &&
	                                                                  desired != backendEnc) {
	                                                                  std::string dec;
	                                                                  if (protocol::Compression::Decompress(backendEnc, outBody, &dec)) {
	                                                                      outBody = std::move(dec);
	                                                                      outEnc = protocol::Compression::Encoding::kIdentity;
	                                                                  }
	                                                              } else if ((backendEnc == protocol::Compression::Encoding::kIdentity || backendEnc == protocol::Compression::Encoding::kUnknown) &&
	                                                                         (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate)) {
	                                                                  std::string comp;
	                                                                  if (protocol::Compression::Compress(desired, outBody, &comp)) {
	                                                                      outBody = std::move(comp);
	                                                                      outEnc = desired;
	                                                                  }
	                                                              }
	                                                          }

	                                                          std::vector<protocol::Hpack::Header> filtered;
	                                                          filtered.reserve(outHs.size() + 1);
	                                                          for (const auto& h : outHs) {
	                                                              if (h.name.empty() || h.name[0] == ':') continue;
	                                                              if (iequals(h.name, "connection") || iequals(h.name, "proxy-connection") || iequals(h.name, "keep-alive")) continue;
	                                                              if (iequals(h.name, "transfer-encoding")) continue;
	                                                              if (iequals(h.name, "content-length")) continue;
	                                                              if (iequals(h.name, "content-encoding")) continue;
	                                                              filtered.push_back(h);
	                                                          }
	                                                          const std::string outEncHdr = EncodingToHeaderValue(outEnc);
	                                                          if (!outEncHdr.empty()) filtered.push_back({"content-encoding", outEncHdr});

	                                                          const auto rtMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
	                                                                                std::chrono::steady_clock::now() - pending->start)
	                                                                                .count();
	                                                          mirror_.MirrorResponseHttp2(pending->clientIp,
	                                                                                      backendAddr.toIpPort(),
	                                                                                      streamId,
	                                                                                      pending->mirrorMethod,
	                                                                                      pending->mirrorPath,
	                                                                                      status,
	                                                                                      rtMs,
	                                                                                      nullptr);
	                                                          ctx->http2.SendResponse(streamId, status, filtered, outBody);
	                                                      } else {
	                                                          ctx->http2.SendResponse(streamId, 502, {{"content-type", "text/plain"}}, "Bad Gateway\n");
	                                                      }

                                                      backendManager_.OnBackendConnectionEnd(backendAddr);
                                                      const bool keep = (!closed) && pending->resp.keepAlive() && pending->resp.gotAll();
                                                      if (lease) lease->Release(keep);
                                                      ctx->http2Pending.erase(streamId);
                                                      backendConn->SetMessageCallback({});
                                                      backendConn->SetCloseCallback({});
                                                  };

                                                  backendConn->SetCloseCallback([finalize](const network::TcpConnectionPtr&) mutable { finalize(true); });
                                                  backendConn->SetMessageCallback([pending, finalize](const network::TcpConnectionPtr&,
                                                                                                     network::Buffer* b,
                                                                                                     std::chrono::system_clock::time_point) mutable {
                                                      if (!b) return;
                                                      const char* data = b->Peek();
                                                      const size_t n = b->ReadableBytes();
                                                      if (n > 0) {
                                                          if (!pending->firstByteRecorded) {
                                                              pending->firstByteRecorded = true;
                                                          }
                                                          pending->raw.append(data, data + n);
                                                          pending->resp.feed(data, n);
                                                          b->RetrieveAll();
                                                      }
                                                      if (pending->resp.hasError()) {
                                                          finalize(false);
                                                          return;
                                                      }
                                                      if (pending->resp.gotAll()) {
                                                          finalize(false);
                                                          return;
                                                      }
                                                  });

                                                  backendConn->Send(out);
                                              });
                    });
                    ctx->http2.OnData(reinterpret_cast<const uint8_t*>(buf->Peek()), buf->ReadableBytes());
                    buf->RetrieveAll();
                    return;
                }
	            if (ctx->waitingBackendResponse) {
	                if (buf->ReadableBytes() > 0) {
	                    ctx->buffer += buf->RetrieveAllAsString();
	                }
	                return;
            }
            if (!ctx->httpContext.parseRequest(buf, receiveTime)) {
                conn->Send("HTTP/1.1 400 Bad Request\r\n\r\n");
                conn->Shutdown();
                return;
            }

            if (ctx->httpContext.gotAll()) {
                const auto reqStart = std::chrono::steady_clock::now();
                if (requestRateLimiter_ && !requestRateLimiter_->Allow(1.0)) {
                    const char* msg = "Too Many Requests";
                    std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                                           "Content-Type: text/plain\r\n"
                                           "Content-Length: " + std::to_string(strlen(msg)) + "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" + std::string(msg);
                    conn->Send(response);
                    conn->Shutdown();
                    if (auditLogger_) {
                        std::ostringstream oss;
                        oss << "deny rate_limit"
                            << " ip=" << conn->peerAddress().toIp()
                            << " path=" << ctx->httpContext.request().path();
                        auditLogger_->AppendLine(oss.str());
                    }
                    return;
                }

                const auto& req = ctx->httpContext.request();
                const bool isWebSocketUpgrade =
                    (req.getMethod() == protocol::HttpRequest::kGet) &&
                    headerContainsTokenCI(req.getHeader("Connection"), "Upgrade") &&
                    iequals(req.getHeader("Upgrade"), "websocket") &&
                    !req.getHeader("Sec-WebSocket-Key").empty();

                // Apply connection limits (per-user/per-service) once per connection, after first request is parsed.
                if (!ctx->connLimitApplied && (perUserConnLimiter_ || perServiceConnLimiter_)) {
                    ctx->userKey.clear();
                    ctx->serviceKey.clear();

                    if (perUserConnLimiter_) {
                        std::string token = req.getHeader(userConnHeader_);
                        if (token.empty()) token = "anonymous";
                        ctx->userKey = token;
                        if (!perUserConnLimiter_->TryAcquire(ctx->userKey)) {
                            const char* msg = "Too Many Connections";
                            std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                                                   "Content-Type: text/plain\r\n"
                                                   "Content-Length: " + std::to_string(strlen(msg)) + "\r\n"
                                                   "Connection: close\r\n"
                                                   "\r\n" + std::string(msg);
                            conn->Send(response);
                            conn->Shutdown();
                            if (auditLogger_) {
                                std::ostringstream oss;
                                oss << "deny conn_limit_user"
                                    << " ip=" << conn->peerAddress().toIp()
                                    << " user=" << ctx->userKey
                                    << " method=" << req.methodString()
                                    << " path=" << req.path();
                                auditLogger_->AppendLine(oss.str());
                            }
                            return;
                        }
                    }

                    if (perServiceConnLimiter_) {
                        std::string svc = req.path();
                        if (svc.empty() || svc[0] != '/') svc = "/";
                        // first segment after '/'
                        size_t start = 1;
                        size_t end = svc.find('/', start);
                        std::string seg = (end == std::string::npos) ? svc.substr(start) : svc.substr(start, end - start);
                        if (seg.empty()) seg = "/";
                        ctx->serviceKey = seg;
                        if (!perServiceConnLimiter_->TryAcquire(ctx->serviceKey)) {
                            if (perUserConnLimiter_ && !ctx->userKey.empty()) {
                                perUserConnLimiter_->Release(ctx->userKey);
                            }
                            ctx->connLimitApplied = false;
                            const char* msg = "Too Many Connections";
                            std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                                                   "Content-Type: text/plain\r\n"
                                                   "Content-Length: " + std::to_string(strlen(msg)) + "\r\n"
                                                   "Connection: close\r\n"
                                                   "\r\n" + std::string(msg);
                            conn->Send(response);
                            conn->Shutdown();
                            if (auditLogger_) {
                                std::ostringstream oss;
                                oss << "deny conn_limit_service"
                                    << " ip=" << conn->peerAddress().toIp()
                                    << " service=" << ctx->serviceKey
                                    << " method=" << req.methodString()
                                    << " path=" << req.path();
                                auditLogger_->AppendLine(oss.str());
                            }
                            return;
                        }
                    }

                    ctx->connLimitApplied = true;
                }

                if (perIpRateLimiter_) {
                    const std::string ip = conn->peerAddress().toIp();
                    if (!perIpRateLimiter_->Allow(ip)) {
                        const char* msg = "Too Many Requests";
                        std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                                               "Content-Type: text/plain\r\n"
                                               "Content-Length: " + std::to_string(strlen(msg)) + "\r\n"
                                               "Connection: close\r\n"
                                               "\r\n" + std::string(msg);
                        conn->Send(response);
                        conn->Shutdown();
                        if (auditLogger_) {
                            std::ostringstream oss;
                            oss << "deny per_ip_rate_limit"
                                << " ip=" << ip
                                << " method=" << req.methodString()
                                << " path=" << req.path();
                            auditLogger_->AppendLine(oss.str());
                        }
                        return;
                    }
                }

                if (perPathRateLimiter_) {
                    const std::string path = req.path();
                    if (!perPathRateLimiter_->Allow(path)) {
                        const char* msg = "Too Many Requests";
                        std::string response = "HTTP/1.1 429 Too Many Requests\r\n"
                                               "Content-Type: text/plain\r\n"
                                               "Content-Length: " + std::to_string(strlen(msg)) + "\r\n"
                                               "Connection: close\r\n"
                                               "\r\n" + std::string(msg);
                        conn->Send(response);
                        conn->Shutdown();
                        if (auditLogger_) {
                            std::ostringstream oss;
                            oss << "deny per_path_rate_limit"
                                << " ip=" << conn->peerAddress().toIp()
                                << " method=" << req.methodString()
                                << " path=" << path;
                            auditLogger_->AppendLine(oss.str());
                        }
                        return;
                    }
                }

                if (accessControl_) {
                    std::string token;
                    std::string apiKey;
                    if (!accessControl_->config().tokenHeader.empty()) {
                        token = req.getHeader(accessControl_->config().tokenHeader);
                    }
                    if (!accessControl_->config().apiKeyHeader.empty()) {
                        apiKey = req.getHeader(accessControl_->config().apiKeyHeader);
                    }
                    if (!accessControl_->Allow(conn->peerAddress().toIp(), token, apiKey)) {
                        const char* msg = "Forbidden";
                        std::string response = "HTTP/1.1 403 Forbidden\r\n"
                                               "Content-Type: text/plain\r\n"
                                               "Content-Length: " + std::to_string(strlen(msg)) + "\r\n"
                                               "Connection: close\r\n"
                                               "\r\n" + std::string(msg);
                        conn->Send(response);
                        conn->Shutdown();
                        if (auditLogger_) {
                            std::ostringstream oss;
                            oss << "deny access_control"
                                << " ip=" << conn->peerAddress().toIp()
                                << " method=" << req.methodString()
                                << " path=" << req.path();
                            auditLogger_->AppendLine(oss.str());
                        }
                        return;
                    }
                }

                // Plugin hook: allow dlopen-based extensions to handle certain HTTP paths.
                // Executed after access control / rate limiting, and before core routing.
                if (plugins_ && plugins_->LoadedCount() > 0) {
                    std::string pluginResp;
                    if (plugins_->DispatchHttp(req.methodString(),
                                               req.path(),
                                               req.query(),
                                               conn->peerAddress().toIp(),
                                               req.body(),
                                               &pluginResp)) {
                        conn->Send(pluginResp);
                        conn->Shutdown();
                        if (auditLogger_) {
                            std::ostringstream oss;
                            oss << "ok plugin"
                                << " ip=" << conn->peerAddress().toIp()
                                << " method=" << req.methodString()
                                << " path=" << req.path()
                                << " code=200"
                                << " backend=-";
                            auditLogger_->AppendLine(oss.str());
                        }
                        return;
                    }
                }
                
                // Admin endpoint: update backend metrics (queue length / GPU util).
                // Example body:
                // {"backend":"127.0.0.1:9001","queue_len":12,"gpu_util":0.35,"vram_used_mb":2048,"vram_total_mb":16384}
                if (req.path() == "/admin/backend_metrics") {
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use POST"));
                        conn->Shutdown();
                        return;
                    }
                    const auto backend = ExtractJsonString(req.body(), "backend");
                    if (!backend || backend->empty()) {
                        conn->Send(PlainResponse(400, "Bad Request", "missing backend"));
                        conn->Shutdown();
                        return;
                    }
                    const auto queueLen = ExtractJsonNumber(req.body(), "queue_len");
                    const auto gpuUtil = ExtractJsonNumber(req.body(), "gpu_util");
                    const auto vramUsed = ExtractJsonNumber(req.body(), "vram_used_mb");
                    const auto vramTotal = ExtractJsonNumber(req.body(), "vram_total_mb");

                    int qv = queueLen ? static_cast<int>(*queueLen) : -1;
                    double gu = gpuUtil ? *gpuUtil : -1.0;
                    int vu = vramUsed ? static_cast<int>(*vramUsed) : 0;
                    int vt = vramTotal ? static_cast<int>(*vramTotal) : 0;

                    bool ok = backendManager_.UpdateBackendMetrics(*backend, qv, gu, vu, vt);
                    if (!ok) {
                        conn->Send(PlainResponse(404, "Not Found", "unknown backend"));
                        conn->Shutdown();
                        return;
                    }
                    const std::string body = "OK\n";
                    conn->Send(PlainResponse(200, "OK", body));
                    conn->Shutdown();
                    if (auditLogger_) {
                        std::ostringstream oss;
                        oss << "admin backend_metrics"
                            << " ip=" << conn->peerAddress().toIp()
                            << " backend=" << *backend
                            << " queue_len=" << qv
                            << " gpu_util=" << gu;
                        auditLogger_->AppendLine(oss.str());
                    }
                    return;
                }

                // Admin endpoint: set backend loaded model (for model affinity).
                // Body: {"backend":"ip:port","model":"llama2","version":"v1","loaded":1}
                if (req.path() == "/admin/backend_model") {
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use POST"));
                        conn->Shutdown();
                        return;
                    }
                    const auto id = ExtractBackendId(req.body());
                    const auto model = ExtractJsonString(req.body(), "model");
                    const auto version = ExtractJsonString(req.body(), "version");
                    const auto loaded = ExtractJsonNumber(req.body(), "loaded");
                    if (!id || !loaded) {
                        conn->Send(PlainResponse(400, "Bad Request", "missing backend/loaded"));
                        conn->Shutdown();
                        return;
                    }
                    bool ok = backendManager_.SetBackendLoadedModel(*id,
                                                                   model ? *model : std::string(),
                                                                   version ? *version : std::string(),
                                                                   (*loaded != 0.0));
                    conn->Send(PlainResponse(ok ? 200 : 404, ok ? "OK" : "Not Found", ok ? "OK\n" : "unknown backend"));
                    conn->Shutdown();
                    if (auditLogger_) {
                        std::ostringstream oss;
                        oss << "admin backend_model"
                            << " ip=" << conn->peerAddress().toIp()
                            << " backend=" << *id
                            << " model=" << (model ? *model : std::string())
                            << " version=" << (version ? *version : std::string())
                            << " loaded=" << ((*loaded != 0.0) ? "1" : "0");
                        auditLogger_->AppendLine(oss.str());
                    }
                    return;
                }

                // Admin endpoint: dynamic registration of backends (service discovery via HTTP API).
                // Body:
                // {"ip":"127.0.0.1","port":9001,"weight":1}
                if (req.path() == "/admin/backend_register") {
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use POST"));
                        conn->Shutdown();
                        return;
                    }
                    const auto ip = ExtractJsonString(req.body(), "ip");
                    const auto port = ExtractJsonNumber(req.body(), "port");
                    const auto weight = ExtractJsonNumber(req.body(), "weight");
                    if (!ip || !port) {
                        conn->Send(PlainResponse(400, "Bad Request", "missing ip/port"));
                        conn->Shutdown();
                        return;
                    }
                    int p = static_cast<int>(*port);
                    if (p <= 0 || p > 65535) {
                        conn->Send(PlainResponse(400, "Bad Request", "invalid port"));
                        conn->Shutdown();
                        return;
                    }
                    int w = weight ? static_cast<int>(*weight) : 1;
                    if (w <= 0) w = 1;
                    backendManager_.AddBackend(*ip, static_cast<uint16_t>(p), w);
                    conn->Send(PlainResponse(200, "OK", "OK\n"));
                    conn->Shutdown();
                    if (auditLogger_) {
                        std::ostringstream oss;
                        oss << "admin backend_register"
                            << " ip=" << conn->peerAddress().toIp()
                            << " backend=" << *ip << ":" << p
                            << " weight=" << w;
                        auditLogger_->AppendLine(oss.str());
                    }
                    return;
                }

                // Admin endpoint: remove backend.
                // Body: {"backend":"ip:port"} OR {"ip":"x","port":y}
                if (req.path() == "/admin/backend_remove") {
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use POST"));
                        conn->Shutdown();
                        return;
                    }
                    const auto id = ExtractBackendId(req.body());
                    if (!id || id->empty()) {
                        conn->Send(PlainResponse(400, "Bad Request", "missing backend"));
                        conn->Shutdown();
                        return;
                    }
                    bool ok = backendManager_.RemoveBackendById(*id);
                    conn->Send(PlainResponse(ok ? 200 : 404, ok ? "OK" : "Not Found", ok ? "OK\n" : "unknown backend"));
                    conn->Shutdown();
                    return;
                }

                // Admin endpoint: set backend online/offline.
                // Body: {"backend":"ip:port","online":1}
                if (req.path() == "/admin/backend_online") {
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use POST"));
                        conn->Shutdown();
                        return;
                    }
                    const auto id = ExtractBackendId(req.body());
                    const auto online = ExtractJsonNumber(req.body(), "online");
                    if (!id || !online) {
                        conn->Send(PlainResponse(400, "Bad Request", "missing backend/online"));
                        conn->Shutdown();
                        return;
                    }
                    bool ok = backendManager_.SetBackendOnline(*id, (*online != 0.0));
                    conn->Send(PlainResponse(ok ? 200 : 404, ok ? "OK" : "Not Found", ok ? "OK\n" : "unknown backend"));
                    conn->Shutdown();
                    return;
                }

                // Admin endpoint: update backend base weight (enables dynamic weight adjustment based on load metrics).
                // Body: {"backend":"ip:port","base_weight":3}
                if (req.path() == "/admin/backend_weight") {
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use POST"));
                        conn->Shutdown();
                        return;
                    }
                    const auto id = ExtractBackendId(req.body());
                    const auto bw = ExtractJsonNumber(req.body(), "base_weight");
                    if (!id || !bw) {
                        conn->Send(PlainResponse(400, "Bad Request", "missing backend/base_weight"));
                        conn->Shutdown();
                        return;
                    }
                    int w = static_cast<int>(*bw);
                    if (w <= 0) w = 1;
                    bool ok = backendManager_.SetBackendBaseWeight(*id, w);
                    conn->Send(PlainResponse(ok ? 200 : 404, ok ? "OK" : "Not Found", ok ? "OK\n" : "unknown backend"));
                    conn->Shutdown();
                    return;
                }

                // Monitor Endpoint Interception
                if (req.path() == "/stats") {
                    // Refresh backend snapshot for service-layer metrics.
                    {
                        auto bs = backendManager_.GetBackendSnapshot();
                        std::vector<monitor::Stats::BackendSnapshot> out;
                        out.reserve(bs.size());
                        for (const auto& b : bs) {
                            monitor::Stats::BackendSnapshot s;
                            s.id = b.id;
                            s.healthy = b.healthy;
                            s.online = b.online;
                            s.weight = b.weight;
                            s.baseWeight = b.baseWeight;
                            s.activeConnections = b.activeConnections;
                            s.ewmaResponseMs = b.ewmaResponseMs;
                            s.failures = b.failures;
                            s.successes = b.successes;
                            s.errorRate = b.errorRate;
                            s.hasQueueLen = b.hasQueueLen;
                            s.queueLen = b.queueLen;
                            s.hasGpu = b.hasGpu;
                            s.gpuUtil01 = b.gpuUtil01;
                            s.vramUsedMb = b.vramUsedMb;
                            s.vramTotalMb = b.vramTotalMb;
                            s.hasModelLoaded = b.hasModelLoaded;
                            s.modelLoaded = b.modelLoaded;
                            s.hasModelName = b.hasModelName;
                            s.modelName = b.modelName;
                            s.hasModelVersion = b.hasModelVersion;
                            s.modelVersion = b.modelVersion;
                            out.push_back(std::move(s));
                        }
                        monitor::Stats::Instance().SetBackendSnapshot(std::move(out));
                    }
                    std::string json = monitor::Stats::Instance().ToJsonCached(200.0);
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: " + std::to_string(json.size()) + "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" + json;
                    conn->Send(response);
                    conn->Shutdown();
                    {
                        const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                                            std::chrono::steady_clock::now() - reqStart)
                                            .count();
                        monitor::Stats::Instance().RecordRequestLatencyMs(ms);
                    }
                    if (auditLogger_) {
                        const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                                            std::chrono::steady_clock::now() - reqStart)
                                            .count();
                        std::ostringstream oss;
                        oss << "ok"
                            << " ip=" << conn->peerAddress().toIp()
                            << " method=" << req.methodString()
                            << " path=" << req.path()
                            << " code=200"
                            << " backend=-"
                            << " rt_ms=" << ms;
                        auditLogger_->AppendLine(oss.str());
                    }
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/dashboard") {
                    const std::string html = DashboardHtml();
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/html; charset=utf-8\r\n"
                                           "Content-Length: " +
                                           std::to_string(html.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           html;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/config") {
                    const std::string html = ConfigHtml();
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/html; charset=utf-8\r\n"
                                           "Content-Length: " +
                                           std::to_string(html.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           html;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/history_ui") {
                    const std::string html = HistoryUiHtml();
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/html; charset=utf-8\r\n"
                                           "Content-Length: " +
                                           std::to_string(html.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           html;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/diagnostics") {
                    const std::string html = DiagnosticsHtml();
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/html; charset=utf-8\r\n"
                                           "Content-Length: " +
                                           std::to_string(html.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           html;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                // Admin endpoint: get/set config (INI in-memory + optional persist).
                if (req.path() == "/admin/config") {
                    const std::string fmt = ExtractQueryParam(req.query(), "format");
                    if (fmt == "ini") {
                        if (req.getMethod() == protocol::HttpRequest::kGet) {
                            const std::string body = proxy::common::Config::Instance().DumpIni();
                            std::string response = "HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: text/plain; charset=utf-8\r\n"
                                                   "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                                   "Connection: close\r\n"
                                                   "\r\n" + body;
                            conn->Send(response);
                            conn->Shutdown();
                            return;
                        }
                        if (req.getMethod() != protocol::HttpRequest::kPost) {
                            conn->Send(PlainResponse(405, "Method Not Allowed", "Use GET/POST"));
                            conn->Shutdown();
                            return;
                        }
                        bool ok = proxy::common::Config::Instance().LoadFromString(req.body());
                        bool savedOk = true;
                        bool save = false;
                        const std::string saveQ = ExtractQueryParam(req.query(), "save");
                        if (!saveQ.empty()) {
                            try {
                                save = (std::stoi(saveQ) != 0);
                            } catch (...) {
                                save = false;
                            }
                        }
                        if (ok && save) {
                            savedOk = proxy::common::Config::Instance().Save();
                        }
                        std::string out = std::string("{\"ok\":") + ((ok && savedOk) ? "true" : "false") +
                                         ",\"saved\":" + (save ? "true" : "false") + "}";
                        conn->Send(out);
                        conn->Shutdown();
                        return;
                    }

                    if (req.getMethod() == protocol::HttpRequest::kGet) {
                        auto all = proxy::common::Config::Instance().GetAll();
                        auto file = proxy::common::Config::Instance().LoadedFilename();
                        std::string json = "{\"ok\":true,";
                        json += "\"file\":\"" + JsonEscape(file ? *file : std::string()) + "\",";
                        json += "\"settings\":{";
                        bool firstSec = true;
                        for (const auto& sec : all) {
                            if (!firstSec) json += ",";
                            firstSec = false;
                            json += "\"" + JsonEscape(sec.first) + "\":{";
                            bool firstKey = true;
                            for (const auto& kv : sec.second) {
                                if (!firstKey) json += ",";
                                firstKey = false;
                                json += "\"" + JsonEscape(kv.first) + "\":\"" + JsonEscape(kv.second) + "\"";
                            }
                            json += "}";
                        }
                        json += "}}";
                        std::string response = "HTTP/1.1 200 OK\r\n"
                                               "Content-Type: application/json\r\n"
                                               "Content-Length: " +
                                               std::to_string(json.size()) +
                                               "\r\n"
                                               "Connection: close\r\n"
                                               "\r\n" +
                                               json;
                        conn->Send(response);
                        conn->Shutdown();
                        return;
                    }
                    if (req.getMethod() != protocol::HttpRequest::kPost) {
                        conn->Send(PlainResponse(405, "Method Not Allowed", "Use GET/POST"));
                        conn->Shutdown();
                        return;
                    }

                    // Parse: {"updates":[{"section":"s","key":"k","value":"v"},...], "save":1}
                    std::vector<std::tuple<std::string, std::string, std::string>> updates;
                    std::vector<std::pair<std::string, std::string>> deletes;
                    std::vector<std::string> deleteSections;
                    const size_t upKey = req.body().find("\"updates\"");
                    if (upKey != std::string::npos) {
                        size_t lb = req.body().find('[', upKey);
                        size_t rb = req.body().find(']', lb == std::string::npos ? upKey : lb);
                        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                            const std::string arr = req.body().substr(lb + 1, rb - (lb + 1));
                            size_t pos = 0;
                            while (true) {
                                size_t ob = arr.find('{', pos);
                                if (ob == std::string::npos) break;
                                size_t cb = arr.find('}', ob + 1);
                                if (cb == std::string::npos) break;
                                const std::string obj = arr.substr(ob + 1, cb - (ob + 1));
                                auto sec = ExtractJsonString(obj, "section");
                                auto key = ExtractJsonString(obj, "key");
                                auto val = ExtractJsonString(obj, "value");
                                if (sec && key && val) {
                                    updates.push_back({*sec, *key, *val});
                                }
                                pos = cb + 1;
                            }
                        }
                    } else {
                        auto sec = ExtractJsonString(req.body(), "section");
                        auto key = ExtractJsonString(req.body(), "key");
                        auto val = ExtractJsonString(req.body(), "value");
                        if (sec && key && val) updates.push_back({*sec, *key, *val});
                    }

                    const size_t delKey = req.body().find("\"deletes\"");
                    if (delKey != std::string::npos) {
                        size_t lb = req.body().find('[', delKey);
                        size_t rb = req.body().find(']', lb == std::string::npos ? delKey : lb);
                        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                            const std::string arr = req.body().substr(lb + 1, rb - (lb + 1));
                            size_t pos = 0;
                            while (true) {
                                size_t ob = arr.find('{', pos);
                                if (ob == std::string::npos) break;
                                size_t cb = arr.find('}', ob + 1);
                                if (cb == std::string::npos) break;
                                const std::string obj = arr.substr(ob + 1, cb - (ob + 1));
                                auto sec = ExtractJsonString(obj, "section");
                                auto key = ExtractJsonString(obj, "key");
                                if (sec && key) deletes.push_back({*sec, *key});
                                pos = cb + 1;
                            }
                        }
                    }

                    const size_t delSecKey = req.body().find("\"delete_sections\"");
                    if (delSecKey != std::string::npos) {
                        size_t lb = req.body().find('[', delSecKey);
                        size_t rb = req.body().find(']', lb == std::string::npos ? delSecKey : lb);
                        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
                            const std::string arr = req.body().substr(lb + 1, rb - (lb + 1));
                            size_t pos = 0;
                            while (true) {
                                size_t q1 = arr.find('"', pos);
                                if (q1 == std::string::npos) break;
                                size_t q2 = arr.find('"', q1 + 1);
                                if (q2 == std::string::npos) break;
                                deleteSections.push_back(arr.substr(q1 + 1, q2 - (q1 + 1)));
                                pos = q2 + 1;
                            }
                        }
                    }
                    bool save = false;
                    if (auto sv = ExtractJsonNumber(req.body(), "save")) {
                        save = (*sv >= 1.0);
                    }

                    if (updates.empty() && deletes.empty() && deleteSections.empty()) {
                        conn->Send("{\"ok\":false,\"error\":\"no_updates\"}");
                        conn->Shutdown();
                        return;
                    }
                    for (const auto& u : updates) {
                        proxy::common::Config::Instance().SetString(std::get<0>(u), std::get<1>(u), std::get<2>(u));
                    }
                    for (const auto& d : deletes) {
                        proxy::common::Config::Instance().DeleteKey(d.first, d.second);
                    }
                    for (const auto& s : deleteSections) {
                        proxy::common::Config::Instance().DeleteSection(s);
                    }
                    bool savedOk = true;
                    if (save) {
                        savedOk = proxy::common::Config::Instance().Save();
                    }
                    std::string out = std::string("{\"ok\":") + (savedOk ? "true" : "false") + ",\"saved\":" +
                                     (save ? "true" : "false") + "}";
                    conn->Send(out);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/admin/logs") {
                    std::string type = ExtractQueryParam(req.query(), "type");
                    if (type.empty()) type = "audit";
                    int lines = 200;
                    try {
                        const std::string v = ExtractQueryParam(req.query(), "lines");
                        if (!v.empty()) lines = std::stoi(v);
                    } catch (...) {
                    }
                    if (lines < 1) lines = 1;
                    if (lines > 2000) lines = 2000;

                    if (type != "audit") {
                        const std::string body = "unsupported_log_type";
                        std::string response = "HTTP/1.1 400 Bad Request\r\n"
                                               "Content-Type: text/plain; charset=utf-8\r\n"
                                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                               "Connection: close\r\n"
                                               "\r\n" + body;
                        conn->Send(response);
                        conn->Shutdown();
                        return;
                    }
                    if (!auditLogger_) {
                        const std::string body = "audit_log_not_enabled";
                        std::string response = "HTTP/1.1 404 Not Found\r\n"
                                               "Content-Type: text/plain; charset=utf-8\r\n"
                                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                               "Connection: close\r\n"
                                               "\r\n" + body;
                        conn->Send(response);
                        conn->Shutdown();
                        return;
                    }
                    const std::string body = TailTextFileLines(auditLogger_->path(), static_cast<size_t>(lines));
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: text/plain; charset=utf-8\r\n"
                                           "Content-Length: " +
                                           std::to_string(body.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           body;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/admin/diagnose") {
                    const std::string statsJson = monitor::Stats::Instance().ToJsonCached(200.0);
                    const std::string histSummary =
                        history_ ? history_->SummaryLastSecondsJson(300) : std::string("{\"error\":\"history_disabled\"}");
                    const auto cfgOpt = proxy::common::Config::Instance().LoadedFilename();
                    const std::string cfgFile = cfgOpt ? *cfgOpt : std::string();
                    const std::string auditPath = auditLogger_ ? auditLogger_->path() : std::string("");
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();

                    std::ostringstream oss;
                    oss << "{"
                        << "\"ok\":1,"
                        << "\"pid\":" << static_cast<long long>(::getpid()) << ","
                        << "\"now_ms\":" << static_cast<long long>(nowMs) << ","
                        << "\"config_file\":\"" << JsonEscape(cfgFile) << "\","
                        << "\"audit_log_path\":\"" << JsonEscape(auditPath) << "\","
                        << "\"stats\":" << statsJson << ","
                        << "\"history_summary\":" << histSummary
                        << "}";
                    const std::string body = oss.str();
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: " +
                                           std::to_string(body.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           body;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/history") {
                    int seconds = 60;
                    try {
                        const std::string v = ExtractQueryParam(req.query(), "seconds");
                        if (!v.empty()) seconds = std::stoi(v);
                    } catch (...) {
                    }
                    if (seconds < 1) seconds = 1;
                    if (seconds > 3600) seconds = 3600;
                    const std::string json = history_ ? history_->PointsLastSecondsJson(seconds) : std::string("{\"error\":\"history_disabled\"}");
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: " +
                                           std::to_string(json.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           json;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }

                if (req.getMethod() == protocol::HttpRequest::kGet && req.path() == "/history/summary") {
                    int seconds = 300;
                    try {
                        const std::string v = ExtractQueryParam(req.query(), "seconds");
                        if (!v.empty()) seconds = std::stoi(v);
                    } catch (...) {
                    }
                    if (seconds < 1) seconds = 1;
                    if (seconds > 24 * 3600) seconds = 24 * 3600;
                    const std::string json =
                        history_ ? history_->SummaryLastSecondsJson(seconds) : std::string("{\"error\":\"history_disabled\"}");
                    std::string response = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: " +
                                           std::to_string(json.size()) +
                                           "\r\n"
                                           "Connection: close\r\n"
                                           "\r\n" +
                                           json;
                    conn->Send(response);
                    conn->Shutdown();
                    return;
                }
                
	                monitor::Stats::Instance().IncTotalRequests();
	                LOG_INFO << "HTTP Request: " << req.methodString() << " " << req.path();
                if (isWebSocketUpgrade) {
                    LOG_INFO << "WebSocket upgrade request from " << conn->peerAddress().toIp();
                }
                monitor::Stats::Instance().RecordRequestMethod(req.methodString());
	                monitor::Stats::Instance().RecordRequestPath(req.path());
                std::string model;
                std::string modelVersion;
	                {
	                    model = req.getHeader("X-Model");
	                    if (model.empty()) model = ExtractQueryParam(req.query(), "model");
                        modelVersion = req.getHeader("X-Model-Version");
                        if (modelVersion.empty()) modelVersion = ExtractQueryParam(req.query(), "version");
                        if (modelVersion.empty()) modelVersion = ExtractQueryParam(req.query(), "model_version");
	                    if (!model.empty()) {
	                        monitor::Stats::Instance().RecordModelName(model);
	                    }
	                }
                const bool streamWanted =
                    IsTruthy(req.getHeader("X-Stream")) ||
                    IsTruthy(ExtractQueryParam(req.query(), "stream")) ||
                    headerContainsTokenCI(req.getHeader("Accept"), "text/event-stream");

                // ACME HTTP-01 challenge: serve token file from local directory (bypass rate limits / ACL).
                // GET /.well-known/acme-challenge/<token>
                if (!acmeChallengeDir_.empty() && req.getMethod() == protocol::HttpRequest::kGet) {
                    const std::string prefix = "/.well-known/acme-challenge/";
                    if (req.path().rfind(prefix, 0) == 0 && req.path().size() > prefix.size()) {
                        std::string token = req.path().substr(prefix.size());
                        // basic sanitization: disallow slashes and parent traversal
                        if (token.find('/') == std::string::npos && token.find("..") == std::string::npos) {
                            const std::string path = acmeChallengeDir_ + "/" + token;
                            std::ifstream f(path);
                            if (f.is_open()) {
                                std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                                std::string response = "HTTP/1.1 200 OK\r\n"
                                                       "Content-Type: text/plain\r\n"
                                                       "Content-Length: " +
                                                       std::to_string(body.size()) +
                                                       "\r\n"
                                                       "Connection: close\r\n"
                                                       "\r\n" +
                                                       body;
                                conn->Send(response);
                                conn->Shutdown();
                            } else {
                                conn->Send(PlainResponse(404, "Not Found", "no such token"));
                                conn->Shutdown();
                            }
                            return;
                        }
                    }
                }

                // HTTP -> gRPC conversion (REST -> internal gRPC Echo), plus JSON <-> ProtoBuf conversion.
	                if (req.getMethod() == protocol::HttpRequest::kPost && req.path() == "/api/echo/unary") {
	                    std::string bodyNorm = req.body();
	                    const auto enc = protocol::Compression::ParseContentEncoding(req.getHeader("Content-Encoding"));
	                    if (enc == protocol::Compression::Encoding::kGzip || enc == protocol::Compression::Encoding::kDeflate) {
	                        std::string dec;
	                        if (!protocol::Compression::Decompress(enc, bodyNorm, &dec)) {
	                            conn->Send(PlainResponse(400, "Bad Request", "bad content-encoding"));
	                            conn->Shutdown();
	                            return;
	                        }
	                        bodyNorm = std::move(dec);
	                    }
	                    const auto msg = ExtractJsonString(bodyNorm, "msg");
	                    if (!msg) {
	                        conn->Send(PlainResponse(400, "Bad Request", "missing msg"));
	                        conn->Shutdown();
	                        return;
	                    }
	                    const std::string outMsg = GrpcEchoUnaryReply(*msg);
	                    const std::string json = std::string("{\"msg\":\"") + outMsg + "\"}";
	                    std::ostringstream oss;
	                    oss << "HTTP/1.1 200 OK\r\n"
	                        << "Content-Type: application/json\r\n"
	                        << "Content-Length: " << json.size() << "\r\n"
	                        << "Connection: close\r\n"
	                        << "\r\n"
	                        << json;
	                    conn->Send(oss.str());
	                    conn->Shutdown();
	                    return;
	                }
	                if (req.getMethod() == protocol::HttpRequest::kPost && req.path() == "/api/echo/stream") {
	                    std::string bodyNorm = req.body();
	                    const auto enc = protocol::Compression::ParseContentEncoding(req.getHeader("Content-Encoding"));
	                    if (enc == protocol::Compression::Encoding::kGzip || enc == protocol::Compression::Encoding::kDeflate) {
	                        std::string dec;
	                        if (!protocol::Compression::Decompress(enc, bodyNorm, &dec)) {
	                            conn->Send(PlainResponse(400, "Bad Request", "bad content-encoding"));
	                            conn->Shutdown();
	                            return;
	                        }
	                        bodyNorm = std::move(dec);
	                    }
	                    const auto msg = ExtractJsonString(bodyNorm, "msg");
	                    if (!msg) {
	                        conn->Send(PlainResponse(400, "Bad Request", "missing msg"));
	                        conn->Shutdown();
	                        return;
	                    }
	                    const auto parts = GrpcEchoStreamReply(*msg);
	                    std::string json = "[";
	                    for (size_t i = 0; i < parts.size(); ++i) {
	                        if (i) json += ",";
	                        json += "\"";
	                        json += parts[i];
	                        json += "\"";
	                    }
	                    json += "]";
	                    std::ostringstream oss;
	                    oss << "HTTP/1.1 200 OK\r\n"
	                        << "Content-Type: application/json\r\n"
	                        << "Content-Length: " << json.size() << "\r\n"
	                        << "Connection: close\r\n"
	                        << "\r\n"
	                        << json;
		                    conn->Send(oss.str());
		                    conn->Shutdown();
		                    return;
		                }

		                // API aggregation: call multiple backend APIs and return a single JSON response.
		                // Request body example:
		                // {"requests":[{"path":"/a","method":"GET"},{"path":"/b"}]}
		                if (req.getMethod() == protocol::HttpRequest::kPost && req.path() == "/aggregate") {
		                    std::string bodyNorm = req.body();
		                    const auto enc = protocol::Compression::ParseContentEncoding(req.getHeader("Content-Encoding"));
		                    if (enc == protocol::Compression::Encoding::kGzip || enc == protocol::Compression::Encoding::kDeflate) {
		                        std::string dec;
		                        if (!protocol::Compression::Decompress(enc, bodyNorm, &dec)) {
		                            conn->Send(PlainResponse(400, "Bad Request", "bad content-encoding"));
		                            conn->Shutdown();
		                            return;
		                        }
		                        bodyNorm = std::move(dec);
		                    }
		                    auto reqs = ParseAggregateRequests(bodyNorm);
		                    if (reqs.empty() || reqs.size() > 16) {
		                        conn->Send(PlainResponse(400, "Bad Request", "invalid requests"));
		                        conn->Shutdown();
		                        return;
		                    }

		                    // Determine whether to close client after this response.
		                    bool clientClose = false;
		                    if (req.getVersion() == protocol::HttpRequest::kHttp10) {
		                        clientClose = !headerContainsTokenCI(req.getHeader("Connection"), "Keep-Alive");
		                    } else {
		                        clientClose = headerContainsTokenCI(req.getHeader("Connection"), "close");
		                    }

		                    std::string affinityKey;
		                    if (affinityMode_ == "ip") {
		                        affinityKey = conn->peerAddress().toIp();
		                    } else if (affinityMode_ == "header") {
		                        if (!affinityHeader_.empty()) affinityKey = req.getHeader(affinityHeader_);
		                    } else if (affinityMode_ == "cookie") {
		                        if (!affinityCookie_.empty()) {
		                            affinityKey = protocol::GetCookieValue(req.getHeader("Cookie"), affinityCookie_);
		                        }
		                    }
		                    if (affinityKey.empty()) affinityKey = conn->peerAddress().toIp();

		                    // Pause further parsing on this client connection until aggregation finishes.
		                    ctx->httpContext.reset();
		                    ctx->waitingBackendResponse = true;
		                    ctx->buffer += buf->RetrieveAllAsString();

		                    auto op = std::make_shared<ApiAggregateOp>();
		                    op->backendManager = &backendManager_;
		                    op->backendPool = backendPool_.get();
		                    op->client = std::weak_ptr<network::TcpConnection>(conn);
		                    op->ctx = ctx;
		                    op->clientIp = conn->peerAddress().toIp();
		                    op->affinityKey = std::move(affinityKey);
		                    op->clientClose = clientClose;
		                    op->start = reqStart;
		                    op->reqs = std::move(reqs);
		                    op->resumeCb = [this, conn, ctx]() {
		                        if (!conn || !conn->connected()) return;
		                        if (ctx->waitingBackendResponse) return;
		                        if (ctx->buffer.empty()) return;
		                        auto pending = std::make_shared<std::string>(std::move(ctx->buffer));
		                        ctx->buffer.clear();
		                        conn->getLoop()->QueueInLoop([this, conn, pending]() {
		                            if (!conn->connected()) return;
		                            network::Buffer bb;
		                            bb.Append(*pending);
		                            this->OnMessage(conn, &bb, std::chrono::system_clock::now());
		                        });
		                    };
		                    op->Start();
		                    return;
		                }

                        // Batch request splitting: split a JSON array request into single requests, merge results.
                        // Trigger: X-Batch-Split: 1 (truthy) and JSON array body.
                        if (req.getMethod() == protocol::HttpRequest::kPost && IsTruthy(req.getHeader("X-Batch-Split"))) {
                            std::string bodyNorm = req.body();
                            const auto enc = protocol::Compression::ParseContentEncoding(req.getHeader("Content-Encoding"));
                            if (enc == protocol::Compression::Encoding::kGzip || enc == protocol::Compression::Encoding::kDeflate) {
                                std::string dec;
                                if (!protocol::Compression::Decompress(enc, bodyNorm, &dec)) {
                                    conn->Send(PlainResponse(400, "Bad Request", "bad content-encoding"));
                                    conn->Shutdown();
                                    return;
                                }
                                bodyNorm = std::move(dec);
                            }

                            std::vector<std::string> parts;
                            if (protocol::HttpBatcher::splitJsonArrayItems(bodyNorm, &parts) && !parts.empty() && parts.size() <= 64) {
                                // Determine whether to close client after this response.
                                bool clientClose = false;
                                if (req.getVersion() == protocol::HttpRequest::kHttp10) {
                                    clientClose = !headerContainsTokenCI(req.getHeader("Connection"), "Keep-Alive");
                                } else {
                                    clientClose = headerContainsTokenCI(req.getHeader("Connection"), "close");
                                }

                                std::string affinityKey;
                                if (affinityMode_ == "ip") {
                                    affinityKey = conn->peerAddress().toIp();
                                } else if (affinityMode_ == "header") {
                                    if (!affinityHeader_.empty()) affinityKey = req.getHeader(affinityHeader_);
                                } else if (affinityMode_ == "cookie") {
                                    if (!affinityCookie_.empty()) {
                                        affinityKey = protocol::GetCookieValue(req.getHeader("Cookie"), affinityCookie_);
                                    }
                                }
                                if (affinityKey.empty()) affinityKey = conn->peerAddress().toIp();

                                std::string selectionKey = req.path() + "#" + affinityKey;
                                if (!model.empty()) selectionKey += "#model:" + model;
                                if (!modelVersion.empty()) selectionKey += "#ver:" + modelVersion;
                                network::InetAddress backendAddr;
                                if (!modelVersion.empty()) {
                                    backendAddr = backendManager_.SelectBackendForModelVersion(selectionKey, model, modelVersion);
                                } else {
                                    backendAddr = model.empty() ? backendManager_.SelectBackend(selectionKey)
                                                                : backendManager_.SelectBackendForModel(selectionKey, model);
                                }
                                if (backendAddr.toIpPort() == "0.0.0.0:0") {
                                    conn->Send("HTTP/1.1 503 Service Unavailable\r\n\r\n");
                                    conn->Shutdown();
                                    monitor::Stats::Instance().IncBackendFailures();
                                    return;
                                }

                                // Pause further parsing on this client connection until batch split finishes.
                                ctx->httpContext.reset();
                                ctx->waitingBackendResponse = true;
                                ctx->buffer += buf->RetrieveAllAsString();

                                auto op = std::make_shared<BatchSplitOp>();
                                op->backendManager = &backendManager_;
                                op->backendPool = backendPool_.get();
                                op->client = std::weak_ptr<network::TcpConnection>(conn);
                                op->ctx = ctx;
                                op->backendAddr = backendAddr;
                                op->route = req.path() + req.query();
                                op->model = model;
                                op->modelVersion = modelVersion;
                                op->clientClose = clientClose;
                                op->start = reqStart;
                                op->items = std::move(parts);
                                op->resumeCb = [this, conn, ctx]() {
                                    if (!conn || !conn->connected()) return;
                                    if (ctx->waitingBackendResponse) return;
                                    if (ctx->buffer.empty()) return;
                                    auto pending = std::make_shared<std::string>(std::move(ctx->buffer));
                                    ctx->buffer.clear();
                                    conn->getLoop()->QueueInLoop([this, conn, pending]() {
                                        if (!conn->connected()) return;
                                        network::Buffer bb;
                                        bb.Append(*pending);
                                        this->OnMessage(conn, &bb, std::chrono::system_clock::now());
                                    });
                                };
                                op->Start();
                                return;
                            }
                        }

		                // L7 Routing Logic
		                std::string affinityKey;
		                if (affinityMode_ == "ip") {
		                    affinityKey = conn->peerAddress().toIp();
                } else if (affinityMode_ == "header") {
                    if (!affinityHeader_.empty()) affinityKey = req.getHeader(affinityHeader_);
	                } else if (affinityMode_ == "cookie") {
	                    if (!affinityCookie_.empty()) {
	                        affinityKey = protocol::GetCookieValue(req.getHeader("Cookie"), affinityCookie_);
	                    }
	                }
	                if (affinityKey.empty()) affinityKey = conn->peerAddress().toIp();

	                // Cache lookup (best-effort). Only cache simple GET responses for identity encoding,
	                // and skip when rewrite rules may affect the response.
	                ctx->cacheKey.clear();
	                if (cache_.Enabled() &&
	                    !isWebSocketUpgrade &&
	                    req.getMethod() == protocol::HttpRequest::kGet &&
	                    req.body().empty() &&
	                    ChooseEncodingFromAccept(req.getHeader("Accept-Encoding")) == protocol::Compression::Encoding::kIdentity &&
	                    rewrite_.MatchHttp1(req) < 0) {
	                    std::string key = std::string(req.methodString()) + " " + req.path() + req.query();
	                    if (!model.empty()) key += "#model:" + model;
                        if (!modelVersion.empty()) key += "#ver:" + modelVersion;
	                    std::string val;
	                    if (cache_.Get(key, &val)) {
	                        size_t p1 = val.find('\n');
	                        size_t p2 = (p1 == std::string::npos) ? std::string::npos : val.find('\n', p1 + 1);
	                        int status = 200;
	                        std::string ct;
	                        std::string body;
	                        if (p1 != std::string::npos && p2 != std::string::npos) {
	                            try {
	                                status = std::stoi(val.substr(0, p1));
	                            } catch (...) {
	                                status = 200;
	                            }
	                            ct = val.substr(p1 + 1, p2 - (p1 + 1));
	                            body = val.substr(p2 + 1);
	                        } else {
	                            body = val;
	                        }

	                        bool clientClose = false;
	                        if (req.getVersion() == protocol::HttpRequest::kHttp10) {
	                            clientClose = !headerContainsTokenCI(req.getHeader("Connection"), "Keep-Alive");
	                        } else {
	                            clientClose = headerContainsTokenCI(req.getHeader("Connection"), "close");
	                        }

	                        std::ostringstream oss;
	                        oss << "HTTP/1.1 " << status << " OK\r\n";
	                        if (!ct.empty()) oss << "Content-Type: " << ct << "\r\n";
	                        oss << "Content-Length: " << body.size() << "\r\n";
	                        oss << "Connection: " << (clientClose ? "close" : "keep-alive") << "\r\n";
	                        oss << "\r\n";
	                        std::string out = oss.str();
	                        out += body;
	                        conn->Send(out);
	                        if (clientClose) conn->Shutdown();
	                        return;
	                    }
	                    ctx->cacheKey = key;
	                }

	                std::string selectionKey = req.path() + "#" + affinityKey;
	                if (!model.empty()) selectionKey += "#model:" + model;
                    if (!modelVersion.empty()) selectionKey += "#ver:" + modelVersion;
                    network::InetAddress backendAddr;
                    if (!modelVersion.empty()) {
                        backendAddr = backendManager_.SelectBackendForModelVersion(selectionKey, model, modelVersion);
                    } else {
                        backendAddr = model.empty() ? backendManager_.SelectBackend(selectionKey)
                                                    : backendManager_.SelectBackendForModel(selectionKey, model);
                    }
                
                if (backendAddr.toIpPort() == "0.0.0.0:0") {
                    conn->Send("HTTP/1.1 503 Service Unavailable\r\n\r\n");
                    conn->Shutdown();
                    monitor::Stats::Instance().IncBackendFailures();
                    if (auditLogger_) {
                        const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                                            std::chrono::steady_clock::now() - reqStart)
                                            .count();
                        std::ostringstream oss;
                        oss << "fail"
                            << " ip=" << conn->peerAddress().toIp()
                            << " method=" << req.methodString()
                            << " path=" << req.path()
                            << " code=503"
                            << " backend=-"
                            << " rt_ms=" << ms;
                        auditLogger_->AppendLine(oss.str());
                    }
                    return;
                }

                if (auditLogger_) {
                    const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                                        std::chrono::steady_clock::now() - reqStart)
                                        .count();
                    std::ostringstream oss;
                    oss << "proxy"
                        << " ip=" << conn->peerAddress().toIp()
                        << " method=" << req.methodString()
                        << " path=" << req.path()
                        << " code=200"
                        << " backend=" << backendAddr.toIpPort()
                        << " rt_ms=" << ms;
                    auditLogger_->AppendLine(oss.str());
                }

                if (isWebSocketUpgrade) {
                    // WebSocket needs full-duplex tunneling.
                    ctx->type = ProxySessionContext::kTunnel;
                    balancer::BackendSession::TunnelConfig tcfg;
                    tcfg.enableBackpressure = true;
                    tcfg.pauseClientReadUntilBackendConnected = true;
                    tcfg.highWaterMarkBytes = 8 * 1024 * 1024;
                    ctx->backendSession = std::make_shared<balancer::BackendSession>(
                        conn->getLoop(),
                        backendAddr.toIp(),
                        backendAddr.toPort(),
                        conn,
                        &backendManager_,
                        tcfg);

                    std::string forwardedReq = std::string(req.methodString()) + " " + req.path() + req.query() + " HTTP/1.1\r\n";
                    for (const auto& header : req.headers()) {
                        forwardedReq += header.first + ": " + header.second + "\r\n";
                    }
                    forwardedReq += "X-Forwarded-For: " + conn->peerAddress().toIp() + "\r\n";
                    forwardedReq += "\r\n";

                    ctx->backendSession->Start();
                    ctx->backendSession->Send(forwardedReq);
                    if (buf->ReadableBytes() > 0) {
                        ctx->backendSession->Send(buf->Peek(), buf->ReadableBytes());
                        buf->RetrieveAll();
                    }
                    return;
                }

                // HTTP reverse proxy mode with backend connection pooling.
                if (ctx->waitingBackendResponse) {
                    // Defensive: should not happen because parse is paused while waiting.
                    ctx->buffer += buf->RetrieveAllAsString();
                    return;
                }

                // Determine whether to close client after this response.
                bool clientClose = false;
                if (req.getVersion() == protocol::HttpRequest::kHttp10) {
                    clientClose = !headerContainsTokenCI(req.getHeader("Connection"), "Keep-Alive");
                } else {
                    clientClose = headerContainsTokenCI(req.getHeader("Connection"), "close");
                }

	                protocol::HttpRequest fwdReq = req;
	                ctx->rewriteRuleIdx = -1;

	                // Normalize request body encoding (Content-Encoding) for proxying/conversion.
	                ctx->clientAcceptEncoding = fwdReq.getHeader("Accept-Encoding");
	                std::string reqBodyNorm = fwdReq.body();
	                const auto reqEnc = protocol::Compression::ParseContentEncoding(fwdReq.getHeader("Content-Encoding"));
	                if (reqEnc == protocol::Compression::Encoding::kGzip || reqEnc == protocol::Compression::Encoding::kDeflate) {
	                    std::string dec;
	                    if (!protocol::Compression::Decompress(reqEnc, reqBodyNorm, &dec)) {
	                        conn->Send("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
	                        conn->Shutdown();
	                        return;
	                    }
	                    reqBodyNorm = std::move(dec);
	                }

	                fwdReq.setBody(reqBodyNorm);
	                ctx->rewriteRuleIdx = rewrite_.MatchHttp1(fwdReq);
		                if (ctx->rewriteRuleIdx >= 0) {
		                    rewrite_.ApplyRequestHttp1(ctx->rewriteRuleIdx, &fwdReq);
		                    reqBodyNorm = fwdReq.body();
		                    ctx->clientAcceptEncoding = fwdReq.getHeader("Accept-Encoding");
		                }

		                ctx->mirrorMethod = fwdReq.methodString();
		                ctx->mirrorPath = fwdReq.path() + fwdReq.query();
		                mirror_.MirrorRequestHttp1(conn->peerAddress().toIp(),
		                                           backendAddr.toIpPort(),
		                                           fwdReq,
		                                           reqBodyNorm);

	                // Batch processing optimization (opt-in). Skip when request is compressed or rewritten.
	                if (batchCfg_.enabled &&
	                    ctx->rewriteRuleIdx < 0 &&
	                    !(reqEnc == protocol::Compression::Encoding::kGzip || reqEnc == protocol::Compression::Encoding::kDeflate)) {
	                    auto batcher = GetOrCreateBatcher(conn->getLoop());
	                    if (batcher && batcher->IsEnabled()) {
	                        if (batcher->TryEnqueue(conn, ctx, fwdReq, backendAddr, model, clientClose, reqStart)) {
	                            // Reset parser and pause further parsing on this client until the batched response is delivered.
	                            ctx->httpContext.reset();
	                            ctx->waitingBackendResponse = true;
	                            ctx->backendFirstByteRecorded = false;
	                            ctx->backendStart = reqStart;
                            ctx->backendResp.reset();
                            ctx->closeAfterResponse = clientClose;
                            ctx->backendBytesForwarded = 0;
                            if (buf->ReadableBytes() > 0) {
                                ctx->buffer += buf->RetrieveAllAsString();
                            }
                            return;
                        }
                    }
	                }

	                std::string forwardedReq;
	                forwardedReq.reserve(1024 + reqBodyNorm.size());
	                forwardedReq += std::string(fwdReq.methodString()) + " " + fwdReq.path() + fwdReq.query() + " HTTP/1.1\r\n";
		
	                bool hasHost = false;
	                for (const auto& header : fwdReq.headers()) {
	                    const std::string& k = header.first;
	                    if (iequals(k, "Host")) hasHost = true;
	                    if (iequals(k, "Connection") || iequals(k, "Proxy-Connection") || iequals(k, "Keep-Alive")) continue;
	                    if (iequals(k, "Transfer-Encoding")) continue; // normalize to Content-Length
	                    if (iequals(k, "Content-Length")) continue;    // normalize to Content-Length
	                    if (iequals(k, "Content-Encoding")) continue;  // normalize to identity
	                    if (iequals(k, "Accept-Encoding")) continue;   // normalize to identity
	                    forwardedReq += k + ": " + header.second + "\r\n";
	                }
                if (!hasHost) {
                    forwardedReq += "Host: " + backendAddr.toIpPort() + "\r\n";
                }
	                forwardedReq += "X-Forwarded-For: " + conn->peerAddress().toIp() + "\r\n";
	                forwardedReq += "Connection: Keep-Alive\r\n";
	                forwardedReq += "Accept-Encoding: identity\r\n";
	                if (!reqBodyNorm.empty()) {
	                    forwardedReq += "Content-Length: " + std::to_string(reqBodyNorm.size()) + "\r\n";
	                }
	                forwardedReq += "\r\n";
	                if (!reqBodyNorm.empty()) {
	                    forwardedReq += reqBodyNorm;
	                }

                PriorityConfig prioCfg;
                std::string schedMode;
                std::shared_ptr<PriorityDispatcher> prioDispatcher;
                std::shared_ptr<FairQueueDispatcher> fairDispatcher;
                std::shared_ptr<EdfDispatcher> edfDispatcher;
                enum class SchedKind { None, Priority, Fair, Edf };
                SchedKind schedKind = SchedKind::None;
                {
                    std::lock_guard<std::mutex> lock(prioMu_);
                    prioCfg = prioCfg_;
                }
                schedMode = ToLowerAscii(prioCfg.mode);
                if (schedMode.empty()) schedMode = "priority";
                if (prioCfg.enabled && prioCfg.maxInflight > 0) {
                    thread_local std::string tlsMode;
                    thread_local int tlsMaxInflight = -1;
                    thread_local std::shared_ptr<PriorityDispatcher> tlsPrio;
                    thread_local std::shared_ptr<FairQueueDispatcher> tlsFair;
                    thread_local std::shared_ptr<EdfDispatcher> tlsEdf;

                    if (tlsMode != schedMode || tlsMaxInflight != prioCfg.maxInflight) {
                        tlsMode = schedMode;
                        tlsMaxInflight = prioCfg.maxInflight;
                        tlsPrio.reset();
                        tlsFair.reset();
                        tlsEdf.reset();
                        if (schedMode == "fair") {
                            tlsFair = std::make_shared<FairQueueDispatcher>(prioCfg.maxInflight);
                        } else if (schedMode == "edf") {
                            tlsEdf = std::make_shared<EdfDispatcher>(prioCfg.maxInflight);
                        } else {
                            tlsPrio = std::make_shared<PriorityDispatcher>(prioCfg.maxInflight);
                        }
                    }

                    if (schedMode == "fair") {
                        fairDispatcher = tlsFair;
                        schedKind = fairDispatcher ? SchedKind::Fair : SchedKind::None;
                    } else if (schedMode == "edf") {
                        edfDispatcher = tlsEdf;
                        schedKind = edfDispatcher ? SchedKind::Edf : SchedKind::None;
                    } else {
                        prioDispatcher = tlsPrio;
                        schedKind = prioDispatcher ? SchedKind::Priority : SchedKind::None;
                    }
                }

                int prioVal = 0;
                std::string flowKey;
                std::chrono::steady_clock::time_point deadlineTp{};
                if (schedKind == SchedKind::Priority) {
                    prioVal = ParsePriorityValue(req.getHeader(prioCfg.priorityHeader));
                    if (prioVal == 0) prioVal = ParsePriorityValue(ExtractQueryParam(req.query(), prioCfg.priorityQuery));
                    if (prioVal < prioCfg.highThreshold) prioVal = 0;
                } else if (schedKind == SchedKind::Fair) {
                    flowKey = ComputeFlowKey(req, conn, prioCfg);
                } else if (schedKind == SchedKind::Edf) {
                    deadlineTp = ComputeDeadline(req, prioCfg);
                }

	                // Reset request parser to allow keep-alive client connections.
                ctx->httpContext.reset();

                ctx->waitingBackendResponse = true;
                ctx->backendFirstByteRecorded = false;
                ctx->backendStart = reqStart;
	                ctx->backendResp.reset();
	                ctx->closeAfterResponse = clientClose;
	                ctx->backendBytesForwarded = 0;
	                ctx->backendResponseBuffer.clear();
	                ctx->backendResponseModeDecided = false;
	                ctx->backendResponseConvert = false;
                    ctx->forceStreamResponse = streamWanted;
	                ctx->backendResponseStatusLine.clear();
                if (buf->ReadableBytes() > 0) {
	                    ctx->buffer += buf->RetrieveAllAsString();
	                }

                    const bool ccEnabled = (congestion_ != nullptr && congestion_->enabled());
                    bool ccAcquired = false;
                    if (ccEnabled) {
                        ccAcquired = congestion_->TryAcquire();
                        if (!ccAcquired) {
                            // Congestion window full: treat as "congested" and shed load quickly.
                            congestion_->OnDrop();
                            conn->Send("HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nRetry-After: 1\r\n\r\n");
                            conn->Shutdown();
                            return;
                        }
                    }
                    auto ccResult = std::make_shared<bool>(false); // default: failure

	                auto weakConn = std::weak_ptr<network::TcpConnection>(conn);
	                auto doneFlag = std::make_shared<bool>(false);
	                auto onTaskDone = [schedKind, prioDispatcher, fairDispatcher, edfDispatcher]() {
	                    if (schedKind == SchedKind::Priority) {
	                        if (prioDispatcher) prioDispatcher->OnTaskDone();
	                    } else if (schedKind == SchedKind::Fair) {
	                        if (fairDispatcher) fairDispatcher->OnTaskDone();
	                    } else if (schedKind == SchedKind::Edf) {
	                        if (edfDispatcher) edfDispatcher->OnTaskDone();
	                    }
	                };
	                auto doneOnce = [this, onTaskDone, doneFlag, ccEnabled, ccAcquired, ccResult]() {
	                    if (*doneFlag) return;
	                    *doneFlag = true;
                        if (ccEnabled && ccAcquired && congestion_) {
                            congestion_->OnComplete(*ccResult);
                        }
	                    onTaskDone();
	                };

                auto* loopPtr = conn->getLoop();
	                auto startAcquire = [this, weakConn, ctx, backendAddr, forwardedReq, loopPtr, doneOnce, ccResult](bool) {
	                    backendPool_->Acquire(loopPtr, backendAddr,
	                                      [this, weakConn, ctx, backendAddr, forwardedReq, doneOnce, ccResult](std::shared_ptr<balancer::BackendConnectionPool::Lease> lease) {
	                                          auto c = weakConn.lock();
	                                          if (!c) {
	                                              if (lease) lease->Release(false);
	                                              doneOnce();
	                                              return;
                                          }
                                          if (!lease || !lease->connection() || !lease->connection()->connected()) {
                                              c->Send("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n");
                                              c->Shutdown();
                                              monitor::Stats::Instance().IncBackendFailures();
                                              if (lease) lease->Release(false);
                                              ctx->waitingBackendResponse = false;
                                              doneOnce();
                                              return;
                                          }

	                                          ctx->backendLease = lease;
	                                          backendManager_.OnBackendConnectionStart(backendAddr);

	                                          auto backendConn = lease->connection();

	                                          // Close callback: detect backend disconnect for "read until close" responses.
	                                          backendConn->SetCloseCallback([this, weakConn, ctx, backendAddr, doneOnce, ccResult](const network::TcpConnectionPtr&) {
	                                              if (!ctx->waitingBackendResponse) return;

	                                              // Backend disconnected mid-flight.
	                                              auto c = weakConn.lock();
	                                              if (ctx->backendResp.needsCloseToFinish()) {
                                                  if (ccResult) *ccResult = true;
	                                                  if (c && ctx->closeAfterResponse) c->Shutdown();
	                                              } else {
	                                                  monitor::Stats::Instance().IncBackendFailures();
	                                                  backendManager_.ReportBackendFailure(backendAddr);
                                                  if (c) {
                                                      if (ctx->backendBytesForwarded == 0) {
                                                          c->Send("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n");
                                                      }
                                                      c->Shutdown();
                                                  }
	                                              }

	                                              ctx->waitingBackendResponse = false;
	                                              backendManager_.OnBackendConnectionEnd(backendAddr);
	                                              if (ctx->backendLease) {
                                                  ctx->backendLease->Release(false);
                                                  ctx->backendLease.reset();
	                                              }
	                                              doneOnce();
	                                          });

		                                          backendConn->SetMessageCallback([this, weakConn, ctx, backendAddr, doneOnce, ccResult](const network::TcpConnectionPtr&,
		                                                                                                           network::Buffer* b,
		                                                                                                           std::chrono::system_clock::time_point) {
	                                              if (!ctx->waitingBackendResponse) {
	                                                  b->RetrieveAll();
	                                                  return;
                                              }
                                              if (!ctx->backendFirstByteRecorded) {
                                                  ctx->backendFirstByteRecorded = true;
                                                  const auto now = std::chrono::steady_clock::now();
                                                  const auto ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - ctx->backendStart).count();
                                                  backendManager_.RecordBackendResponseTimeMs(backendAddr, ms);
                                              }

	                                              const char* data = b->Peek();
	                                              const size_t n = b->ReadableBytes();
	                                              const bool done = ctx->backendResp.feed(data, n);
                                              if (ctx->backendResp.hasError()) {
                                                  monitor::Stats::Instance().IncBackendFailures();
                                                  backendManager_.ReportBackendFailure(backendAddr);
                                                  auto c = weakConn.lock();
                                                  if (c) {
                                                      if (ctx->backendBytesForwarded == 0) {
                                                          c->Send("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n");
                                                      }
                                                      c->Shutdown();
                                                  }
                                                  b->RetrieveAll();
                                                  ctx->waitingBackendResponse = false;
                                                  backendManager_.OnBackendConnectionEnd(backendAddr);
                                                  if (ctx->backendLease) {
                                                      ctx->backendLease->Release(false);
                                                      ctx->backendLease.reset();
                                                  }
                                                  doneOnce();
                                                  return;
                                              }
	                                              auto c = weakConn.lock();
	                                              if (n > 0) {
		                                                  if (!ctx->backendResponseModeDecided) {
		                                                      ctx->backendResponseBuffer.append(data, data + n);
		                                                      if (ctx->backendResponseBuffer.size() > ctx->backendResponseBufLimit) {
		                                                          ctx->backendResponseModeDecided = true;
		                                                          ctx->backendResponseConvert = false;
		                                                          ctx->cacheKey.clear();
		                                                          if (c) {
		                                                              c->Send(ctx->backendResponseBuffer);
		                                                              ctx->backendBytesForwarded += ctx->backendResponseBuffer.size();
		                                                          }
	                                                          ctx->backendResponseBuffer.clear();
	                                                      } else {
	                                                          const size_t hdrEnd = ctx->backendResponseBuffer.find("\r\n\r\n");
	                                                          if (hdrEnd != std::string::npos) {
	                                                              const std::string head = ctx->backendResponseBuffer.substr(0, hdrEnd + 4);
	                                                              std::string ce;
		                                                              ParseHttp1HeadMeta(head, &ctx->backendResponseStatusLine, &ce);
		                                                              const auto backendEnc = protocol::Compression::ParseContentEncoding(ce);
		                                                              const auto desired = ChooseEncodingFromAccept(ctx->clientAcceptEncoding);
		                                                              const bool needRewrite =
		                                                                  (ctx->rewriteRuleIdx >= 0) &&
		                                                                  (static_cast<size_t>(ctx->rewriteRuleIdx) < rewrite_.rules().size()) &&
		                                                                  (rewrite_.rules()[static_cast<size_t>(ctx->rewriteRuleIdx)].HasResponseMutations());
		                                                              const bool needCacheStore = !ctx->cacheKey.empty();
		                                                              const bool needDecompress =
		                                                                  (backendEnc == protocol::Compression::Encoding::kGzip || backendEnc == protocol::Compression::Encoding::kDeflate) &&
		                                                                  (desired != backendEnc);
		                                                              const bool needCompress =
		                                                                  (backendEnc == protocol::Compression::Encoding::kIdentity || backendEnc == protocol::Compression::Encoding::kUnknown) &&
		                                                                  (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate);
		                                                              ctx->backendResponseModeDecided = true;
		                                                              ctx->backendResponseConvert =
		                                                                  !ctx->forceStreamResponse &&
		                                                                  (needDecompress || needCompress || needRewrite || needCacheStore) &&
		                                                                  !ctx->backendResp.needsCloseToFinish();
		                                                              if (!ctx->backendResponseConvert) ctx->cacheKey.clear();
		                                                              if (!ctx->backendResponseConvert) {
		                                                                  if (c) {
		                                                                      c->Send(ctx->backendResponseBuffer);
		                                                                      ctx->backendBytesForwarded += ctx->backendResponseBuffer.size();
	                                                                  }
	                                                                  ctx->backendResponseBuffer.clear();
	                                                              }
	                                                          }
	                                                      }
	                                                  } else if (!ctx->backendResponseConvert) {
	                                                      if (c) {
	                                                          c->Send(data, n);
	                                                          ctx->backendBytesForwarded += n;
	                                                      }
	                                                  } else {
	                                                      if (ctx->backendResp.needsCloseToFinish()) {
		                                                          // Cannot safely convert without a definite end-of-body; fallback to pass-through.
		                                                          ctx->backendResponseConvert = false;
		                                                          ctx->cacheKey.clear();
		                                                          if (c) {
		                                                              if (!ctx->backendResponseBuffer.empty()) {
		                                                                  c->Send(ctx->backendResponseBuffer);
		                                                                  ctx->backendBytesForwarded += ctx->backendResponseBuffer.size();
	                                                              }
	                                                              c->Send(data, n);
	                                                              ctx->backendBytesForwarded += n;
	                                                          }
	                                                          ctx->backendResponseBuffer.clear();
	                                                      } else {
	                                                          ctx->backendResponseBuffer.append(data, data + n);
		                                                          if (ctx->backendResponseBuffer.size() > ctx->backendResponseBufLimit) {
		                                                              // Fallback: stop converting and stream what we have.
		                                                              ctx->backendResponseConvert = false;
		                                                              ctx->cacheKey.clear();
		                                                              if (c) {
		                                                                  c->Send(ctx->backendResponseBuffer);
		                                                                  ctx->backendBytesForwarded += ctx->backendResponseBuffer.size();
		                                                              }
	                                                              ctx->backendResponseBuffer.clear();
	                                                          }
	                                                      }
	                                                  }
	                                              }
	                                              b->RetrieveAll();
	                                              if (!done) return;

	                                              // If conversion is enabled, finalize now with the full buffered response.
		                                              if (ctx->backendResponseModeDecided && ctx->backendResponseConvert) {
	                                                  int status = 502;
	                                                  std::vector<protocol::Hpack::Header> hs;
	                                                  std::string body;
	                                                  std::string contentEnc;
	                                                  std::string statusLine;
	                                                  if (!parseHttp1Response(ctx->backendResponseBuffer, &status, &hs, &contentEnc, &statusLine, &body)) {
	                                                      if (c && ctx->backendBytesForwarded == 0) {
	                                                          c->Send("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n");
	                                                      }
	                                                      if (c) c->Shutdown();
	                                                      ctx->backendResponseBuffer.clear();
		                                                  } else {
		                                                      const auto backendEnc = protocol::Compression::ParseContentEncoding(contentEnc);
		                                                      const auto desired = ChooseEncodingFromAccept(ctx->clientAcceptEncoding);
		                                                      const int ruleIdx = ctx->rewriteRuleIdx;
		                                                      const bool needRewrite =
		                                                          (ruleIdx >= 0) &&
		                                                          (static_cast<size_t>(ruleIdx) < rewrite_.rules().size()) &&
		                                                          (rewrite_.rules()[static_cast<size_t>(ruleIdx)].HasResponseMutations());

		                                                      std::vector<protocol::Hpack::Header> outHs = hs;
		                                                      std::string outBody = body;
		                                                      protocol::Compression::Encoding outEnc = backendEnc;

		                                                      if (needRewrite) {
		                                                          bool canRewriteBody = true;
		                                                          if (backendEnc == protocol::Compression::Encoding::kGzip || backendEnc == protocol::Compression::Encoding::kDeflate) {
		                                                              std::string dec;
		                                                              if (!protocol::Compression::Decompress(backendEnc, outBody, &dec)) {
		                                                                  canRewriteBody = false;
		                                                              } else {
		                                                                  outBody = std::move(dec);
		                                                                  outEnc = protocol::Compression::Encoding::kIdentity;
		                                                              }
		                                                          } else if (backendEnc == protocol::Compression::Encoding::kUnknown) {
		                                                              outEnc = protocol::Compression::Encoding::kIdentity;
		                                                          }

		                                                          if (canRewriteBody && outEnc == protocol::Compression::Encoding::kIdentity) {
		                                                              rewrite_.ApplyResponse(ruleIdx, &outHs, &outBody);
		                                                              // Choose output encoding for client after rewrite (body is identity).
		                                                              if (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate) {
		                                                                  std::string comp;
		                                                                  if (protocol::Compression::Compress(desired, outBody, &comp)) {
		                                                                      outBody = std::move(comp);
		                                                                      outEnc = desired;
		                                                                  }
		                                                              } else {
		                                                                  outEnc = protocol::Compression::Encoding::kIdentity;
		                                                              }
		                                                          } else {
		                                                              // Header-only rewrite + original conversion behavior for the body.
		                                                              rewrite_.ApplyResponse(ruleIdx, &outHs, nullptr);
		                                                              outBody = body;
		                                                              outEnc = backendEnc;
		                                                              if ((backendEnc == protocol::Compression::Encoding::kGzip || backendEnc == protocol::Compression::Encoding::kDeflate) &&
		                                                                  desired != backendEnc) {
		                                                                  std::string dec;
		                                                                  if (protocol::Compression::Decompress(backendEnc, outBody, &dec)) {
		                                                                      outBody = std::move(dec);
		                                                                      outEnc = protocol::Compression::Encoding::kIdentity;
		                                                                  }
		                                                              } else if ((backendEnc == protocol::Compression::Encoding::kIdentity || backendEnc == protocol::Compression::Encoding::kUnknown) &&
		                                                                         (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate)) {
		                                                                  std::string comp;
		                                                                  if (protocol::Compression::Compress(desired, outBody, &comp)) {
		                                                                      outBody = std::move(comp);
		                                                                      outEnc = desired;
		                                                                  }
		                                                              }
		                                                          }
		                                                      } else {
		                                                          if ((backendEnc == protocol::Compression::Encoding::kGzip || backendEnc == protocol::Compression::Encoding::kDeflate) &&
		                                                              desired != backendEnc) {
		                                                              std::string dec;
		                                                              if (protocol::Compression::Decompress(backendEnc, outBody, &dec)) {
		                                                                  outBody = std::move(dec);
		                                                                  outEnc = protocol::Compression::Encoding::kIdentity;
		                                                              }
		                                                          } else if ((backendEnc == protocol::Compression::Encoding::kIdentity || backendEnc == protocol::Compression::Encoding::kUnknown) &&
		                                                                     (desired == protocol::Compression::Encoding::kGzip || desired == protocol::Compression::Encoding::kDeflate)) {
		                                                              std::string comp;
		                                                              if (protocol::Compression::Compress(desired, outBody, &comp)) {
		                                                                  outBody = std::move(comp);
		                                                                  outEnc = desired;
		                                                              }
		                                                          }
		                                                      }
		                                                      std::string sl = !statusLine.empty() ? statusLine : ctx->backendResponseStatusLine;
		                                                      if (sl.empty()) sl = "HTTP/1.1 " + std::to_string(status) + " OK";
		                                                      std::ostringstream oss;
		                                                      oss << sl << "\r\n";
		                                                      for (const auto& h : outHs) {
		                                                          if (h.name.empty()) continue;
		                                                          if (iequals(h.name, "connection") || iequals(h.name, "proxy-connection") || iequals(h.name, "keep-alive")) continue;
		                                                          if (iequals(h.name, "transfer-encoding")) continue;
		                                                          if (iequals(h.name, "content-length")) continue;
		                                                          if (iequals(h.name, "content-encoding")) continue;
		                                                          oss << h.name << ": " << h.value << "\r\n";
		                                                      }
		                                                      const std::string outEncHdr = EncodingToHeaderValue(outEnc);
		                                                      if (!outEncHdr.empty()) oss << "Content-Encoding: " << outEncHdr << "\r\n";
		                                                      oss << "Content-Length: " << outBody.size() << "\r\n";
		                                                      oss << "Connection: " << (ctx->closeAfterResponse ? "close" : "keep-alive") << "\r\n";
		                                                      oss << "\r\n";
		                                                      std::string out = oss.str();
		                                                      out += outBody;
		                                                      if (!ctx->cacheKey.empty() &&
		                                                          status == 200 &&
		                                                          (outEnc == protocol::Compression::Encoding::kIdentity || outEnc == protocol::Compression::Encoding::kUnknown)) {
		                                                          std::string ct;
		                                                          for (const auto& h : outHs) {
		                                                              if (iequals(h.name, "content-type")) {
		                                                                  ct = h.value;
		                                                                  break;
		                                                              }
		                                                          }
		                                                          std::string val = std::to_string(status) + "\n" + ct + "\n" + outBody;
		                                                          cache_.Set(ctx->cacheKey, val);
		                                                          ctx->cacheKey.clear();
		                                                      } else {
		                                                          ctx->cacheKey.clear();
		                                                      }
		                                                      if (c) {
		                                                          c->Send(out);
		                                                          ctx->backendBytesForwarded += out.size();
		                                                      }
	                                                      ctx->backendResponseBuffer.clear();
	                                                  }
		                                              }

		                                              if (c) {
		                                                  int st = ctx->backendResp.statusCode();
		                                                  if (st == 0) st = 502;
		                                                  const auto rtMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
		                                                                        std::chrono::steady_clock::now() - ctx->backendStart)
		                                                                        .count();
		                                                  mirror_.MirrorResponseHttp1(c->peerAddress().toIp(),
		                                                                              backendAddr.toIpPort(),
		                                                                              ctx->mirrorMethod,
		                                                                              ctx->mirrorPath,
		                                                                              st,
		                                                                              rtMs,
		                                                                              nullptr);
		                                              }

			                                              if (c && ctx->closeAfterResponse) c->Shutdown();

                                                  if (ccResult) *ccResult = true;
			                                              ctx->waitingBackendResponse = false;
			                                              backendManager_.OnBackendConnectionEnd(backendAddr);
		                                              const bool keep = ctx->backendResp.keepAlive();
		                                              if (ctx->backendLease) {
	                                                  ctx->backendLease->Release(keep);
                                                  ctx->backendLease.reset();
                                              }
	                                              ctx->backendResp.reset();
	                                              ctx->backendResponseBuffer.clear();
	                                              ctx->backendResponseModeDecided = false;
	                                              ctx->backendResponseConvert = false;
                                                  ctx->forceStreamResponse = false;
	                                              ctx->backendResponseStatusLine.clear();

                                              // Process buffered client bytes (pipelined/next requests).
                                              if (c && !ctx->buffer.empty()) {
                                                  auto pending = std::make_shared<std::string>(std::move(ctx->buffer));
                                                  ctx->buffer.clear();
                                                  c->getLoop()->QueueInLoop([this, c, pending]() {
                                                      network::Buffer bb;
                                                      bb.Append(*pending);
                                                      this->OnMessage(c, &bb, std::chrono::system_clock::now());
                                                  });
                                              }
                                              doneOnce();
                                          });

                                          // Send request now.
                                          backendConn->Send(forwardedReq);
                                      });
                };

                if (schedKind != SchedKind::None) {
                    auto startFn = [weakConn, startAcquire, doneOnce]() mutable {
                        auto c = weakConn.lock();
                        if (!c || !c->connected()) {
                            doneOnce(); // inflight was already reserved by dispatcher
                            return;
                        }
                        startAcquire(true);
                    };

                    if (schedKind == SchedKind::Priority && prioDispatcher) {
                        if (prioVal == 0 && prioCfg.lowDelayMs > 0) {
                            int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
                            if (tfd < 0) {
                                prioDispatcher->Enqueue(prioVal, std::move(startFn));
                            } else {
                                struct itimerspec howlong;
                                bzero(&howlong, sizeof howlong);
                                const int ms = prioCfg.lowDelayMs;
                                howlong.it_value.tv_sec = static_cast<time_t>(ms / 1000);
                                howlong.it_value.tv_nsec = static_cast<long>((ms % 1000) * 1000000L);
                                ::timerfd_settime(tfd, 0, &howlong, NULL);

                                auto ch = std::make_shared<proxy::network::Channel>(loopPtr, tfd);
                                auto pd = prioDispatcher;
                                ch->SetReadCallback([weakConn, pd, startFn, ch, tfd](std::chrono::system_clock::time_point) mutable {
                                    uint64_t one;
                                    ::read(tfd, &one, sizeof one);
                                    ch->DisableAll();
                                    ch->Remove();
                                    ::close(tfd);

                                    auto c = weakConn.lock();
                                    if (!c || !c->connected()) {
                                        return; // not dispatched, no inflight reserved
                                    }
                                    if (pd) pd->Enqueue(0, std::move(startFn));
                                });
                                ch->EnableReading();
                            }
                        } else {
                            prioDispatcher->Enqueue(prioVal, std::move(startFn));
                        }
                    } else if (schedKind == SchedKind::Fair && fairDispatcher) {
                        fairDispatcher->Enqueue(flowKey, std::move(startFn));
                    } else if (schedKind == SchedKind::Edf && edfDispatcher) {
                        edfDispatcher->Enqueue(deadlineTp, std::move(startFn));
                    } else {
                        startAcquire(false);
                    }
                } else {
                    startAcquire(false);
                }
            }
        }

    } catch (const std::bad_any_cast&) {
        LOG_ERROR << "Bad context cast in OnMessage";
        buf->RetrieveAll();
    }
}


} // namespace proxy
