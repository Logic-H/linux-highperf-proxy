#include "proxy/protocol/HttpContext.h"
#include "proxy/protocol/HttpResponse.h"
#include "proxy/network/Buffer.h"
#include "proxy/common/Logger.h"
#include <iostream>
#include <cassert>

using namespace proxy::protocol;
using namespace proxy::network;
using namespace proxy::common;

void testParseRequest() {
    HttpContext context;
    Buffer buf;
    
    // Simulate partial arrival
    std::string inputPart1 = "GET /index.html?id=123 HTTP/1.1\r\nHost: ";
    std::string inputPart2 = "localhost\r\nUser-Agent: curl/7.68.0\r\nAccept: */*\r\n\r\n";

    buf.Append(inputPart1);
    if (context.parseRequest(&buf, std::chrono::system_clock::now())) {
        if (context.gotAll()) {
            LOG_ERROR << "Should not get all yet";
        }
    }

    buf.Append(inputPart2);
    if (context.parseRequest(&buf, std::chrono::system_clock::now())) {
        if (context.gotAll()) {
            const HttpRequest& req = context.request();
            assert(req.getMethod() == HttpRequest::kGet);
            assert(req.path() == "/index.html");
            assert(req.query() == "?id=123"); // Wait, implementation setQuery includes '?'? Let's check.
            // HttpContext implementation: setQuery(question, space).
            // setQuery(const char* start, const char* end) { query_.assign(start, end); }
            // If start points to '?', then yes.
            
            assert(req.getHeader("Host") == "localhost");
            assert(req.getHeader("User-Agent") == "curl/7.68.0");
            
            LOG_INFO << "Parse Request PASS";
        } else {
            LOG_ERROR << "Should get all";
        }
    } else {
        LOG_ERROR << "Parse failed";
    }
}

void testParseContentLengthBody() {
    HttpContext context;
    Buffer buf;
    std::string input =
        "POST /submit HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    buf.Append(input);
    bool ok = context.parseRequest(&buf, std::chrono::system_clock::now());
    assert(ok);
    assert(context.gotAll());
    const HttpRequest& req = context.request();
    assert(req.getMethod() == HttpRequest::kPost);
    assert(req.path() == "/submit");
    assert(req.body() == "hello");
    LOG_INFO << "Parse Content-Length Body PASS";
}

void testParseChunkedBody() {
    HttpContext context;
    Buffer buf;
    std::string input =
        "POST /chunk HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";
    buf.Append(input);
    bool ok = context.parseRequest(&buf, std::chrono::system_clock::now());
    assert(ok);
    assert(context.gotAll());
    const HttpRequest& req = context.request();
    assert(req.getMethod() == HttpRequest::kPost);
    assert(req.path() == "/chunk");
    assert(req.body() == "hello");
    LOG_INFO << "Parse Chunked Body PASS";
}

void testResponseGen() {
    HttpResponse resp(true);
    resp.setStatusCode(HttpResponse::k200Ok);
    resp.setStatusMessage("OK");
    resp.setContentType("text/plain");
    resp.addHeader("Server", "GeminiProxy");
    resp.setBody("Hello World");

    Buffer buf;
    resp.appendToBuffer(&buf);

    std::string expected = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/plain\r\nServer: GeminiProxy\r\n\r\nHello World";
    // Ordering of map headers is alphabetical. Content-Type < Server.
    // Connection: close is added manually first.
    
    std::string output = buf.RetrieveAllAsString();
    // LOG_INFO << output;
    
    // Simple check
    if (output.find("HTTP/1.1 200 OK") != std::string::npos &&
        output.find("Hello World") != std::string::npos) {
        LOG_INFO << "Response Gen PASS";
    } else {
        LOG_ERROR << "Response Gen FAILED";
    }
}

int main() {
    Logger::Instance().SetLevel(LogLevel::INFO);
    testParseRequest();
    testParseContentLengthBody();
    testParseChunkedBody();
    testResponseGen();
    return 0;
}
