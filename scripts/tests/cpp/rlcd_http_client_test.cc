#include <cassert>
#include <iostream>

#include "rlcd_http_client.h"

void Reset() {
    assert(live_clients == 0);
    next_response = {};
    close_calls = cleanups = callbacks_during_close = read_calls = last_timeout = 0;
}
void FixedLengthAndHeaders() {
    Reset();
    next_response.status = 206;
    next_response.headers = {{"Content-Type", "audio/mpeg"}, {"Content-Range", "bytes 20-24/25"}};
    {
        RlcdHttpClient client;
        client.SetTimeout(8000);
        client.SetHeader("Authorization", "Bearer secret");
        client.SetHeader("Range", "bytes=20-");
        client.SetKeepAlive(false);
        assert(client.Open("GET", "http://fixture.example/stream"));
        assert(last_config.timeout_ms == 8000);
        assert(sent_headers.at("authorization") == "Bearer secret");
        assert(sent_headers.at("range") == "bytes=20-");
        assert(sent_headers.at("Connection") == "close");
        assert(*client.GetStatusCode() == 206 && client.GetBodyLength() == 5);
        assert(client.GetResponseHeader("content-range") == "bytes 20-24/25");
        assert(client.GetResponseHeader("CONTENT-TYPE") == "audio/mpeg");
        char buffer[32];
        auto count = client.Read(buffer, sizeof(buffer));
        assert(count && *count == 5 && std::string(buffer, *count) == "hello");
        assert(last_timeout == 8000);
        assert(*client.Read(buffer, sizeof(buffer)) == 0);
        assert(read_calls == 1);
        client.Close();
        client.Close();
        assert(!client.Read(buffer, sizeof(buffer)));
    }
    assert(live_clients == 0 && cleanups == 1 && close_calls == 1 && callbacks_during_close == 1);
}
void TimeoutPreservesOffset() {
    Reset();
    next_response.steps = {{2}, {-ESP_ERR_HTTP_EAGAIN}, {-1, EAGAIN}, {0}, {3}};
    {
        RlcdHttpClient client;
        client.SetTimeout(3000);
        assert(client.Open("GET", "http://fixture.example/stream"));
        client.SetTimeout(200);
        char buffer[32];
        auto count = client.Read(buffer, sizeof(buffer));
        assert(count && *count == 2 && std::string(buffer, *count) == "he");
        for (int failure = 0; failure < 3; ++failure) {
            auto timeout = client.Read(buffer, sizeof(buffer));
            assert(!timeout && timeout.error().code == NetworkErrc::Timeout);
            assert(last_timeout == 200 && last_client->position == 2);
        }
        count = client.Read(buffer, sizeof(buffer));
        assert(count && *count == 3 && std::string(buffer, *count) == "llo");
        assert(*client.Read(buffer, sizeof(buffer)) == 0);
    }
    assert(cleanups == 1 && live_clients == 0);
}
void PrematureCloseIsError() {
    Reset();
    next_response.body = "abc";
    {
        RlcdHttpClient client;
        assert(client.Open("GET", "http://fixture.example/stream"));
        char buffer[32];
        assert(*client.Read(buffer, sizeof(buffer)) == 3);
        auto closed = client.Read(buffer, sizeof(buffer));
        assert(!closed && closed.error().code == NetworkErrc::ServerDisconnected);
    }
    assert(live_clients == 0 && cleanups == 1);
}
void ChunkedAndConnectionClose() {
    for (bool chunked : {false, true}) {
        Reset();
        next_response.length = -1;
        next_response.chunked = chunked;
        next_response.steps = {{2}, {3}};
        {
            RlcdHttpClient client;
            assert(client.Open("GET", "http://fixture.example/stream"));
            assert(client.GetBodyLength() == 0);
            assert(client.ReadAll() == "hello");
            char byte;
            assert(*client.Read(&byte, 1) == 0);
        }
        assert(live_clients == 0 && cleanups == 1);
    }
    Reset();
    next_response.length = -1;
    next_response.chunked = true;
    next_response.eof_complete = false;
    {
        RlcdHttpClient client;
        assert(client.Open("GET", "http://fixture.example/stream"));
        assert(client.ReadAll().empty());
    }
}
void HeadNoBodyAndTls() {
    for (const auto& response : std::map<std::string, int>{{"HEAD", 200}, {"GET", 204}}) {
        Reset();
        next_response.status = response.second;
        next_response.length = 999;
        {
            RlcdHttpClient client;
            assert(client.Open(response.first, "https://fixture.example/metadata"));
            assert(last_config.crt_bundle_attach != nullptr);
            char byte;
            assert(*client.Read(&byte, 1) == 0 && read_calls == 0);
        }
        assert(live_clients == 0 && cleanups == 1);
    }
}
void PostAndReopenConsumeHeaders() {
    Reset();
    {
        RlcdHttpClient client;
        client.SetHeader("Content-Type", "application/json");
        client.SetHeader("Authorization", "Bearer secret");
        client.SetContent(std::string("{\"test\":1}"));
        assert(client.Open("POST", "http://fixture.example/test"));
        assert(sent_content == "{\"test\":1}" && last_config.method == HTTP_METHOD_POST);
        assert(client.ReadAll() == "hello");
        assert(client.Open("GET", "http://other.example/test"));
        assert(!sent_headers.count("authorization") && !sent_headers.count("content-type"));
        assert(sent_content.empty());
        assert(cleanups == 1);
        auto write = client.Write("x", 1);
        assert(!write && write.error().code == NetworkErrc::InvalidArgument);
    }
    assert(live_clients == 0 && cleanups == 2);
}
void FailureCleanupAndHeaderLimit() {
    for (bool open_failure : {true, false}) {
        Reset();
        if (open_failure)
            next_response.open_result = ESP_FAIL;
        else
            next_response.fetch_result = -ESP_ERR_HTTP_EAGAIN;
        {
            RlcdHttpClient client;
            auto result = client.Open("GET", "http://fixture.example/test");
            assert(!result);
            if (!open_failure)
                assert(result.error().code == NetworkErrc::Timeout);
        }
        assert(live_clients == 0 && cleanups == 1);
    }
    Reset();
    next_response.headers["x-large"] = std::string(17 * 1024, 'x');
    {
        RlcdHttpClient client;
        auto result = client.Open("GET", "http://fixture.example/test");
        assert(!result && result.error().code == NetworkErrc::ProtocolError);
    }
    assert(live_clients == 0 && cleanups == 1);
}
int main() {
    FixedLengthAndHeaders();
    TimeoutPreservesOffset();
    PrematureCloseIsError();
    ChunkedAndConnectionClose();
    HeadNoBodyAndTls();
    PostAndReopenConsumeHeaders();
    FailureCleanupAndHeaderLimit();
    assert(live_clients == 0);
    std::cout << "Synchronous HTTP contracts passed\n";
}
