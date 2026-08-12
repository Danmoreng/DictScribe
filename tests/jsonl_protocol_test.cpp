#include "dictscribe/protocol/jsonl_protocol.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

void expect_parse_failure(std::string_view input) {
    try {
        (void)dictscribe::protocol::parse(input);
        assert(false && "parse should have failed");
    } catch (const std::exception&) {
    }
}

} // namespace

int main() {
    const auto ping = dictscribe::protocol::parse(R"({"v":1,"type":"ping","id":"one"})");
    assert(ping.at("type") == "ping");
    assert(dictscribe::protocol::encode(ping).back() == '\n');

    const auto failure = dictscribe::protocol::error(
        "MODEL_LOAD_FAILED", "could not load model", false, "request-1", "session-1");
    assert(failure.at("v") == 1);
    assert(failure.at("id") == "request-1");
    assert(failure.at("sessionId") == "session-1");

    expect_parse_failure("");
    expect_parse_failure("[]");
    expect_parse_failure(R"({"v":2,"type":"ping"})");
    expect_parse_failure(R"({"v":1,"type":""})");

    std::cout << "JSONL protocol tests passed\n";
    return 0;
}
