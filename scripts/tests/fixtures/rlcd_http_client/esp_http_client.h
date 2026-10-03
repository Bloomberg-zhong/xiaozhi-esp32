#pragma once
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_HTTP_EAGAIN = 0x7007;
enum esp_http_client_method_t {
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT
};
enum esp_http_client_event_id_t { HTTP_EVENT_ON_HEADER, HTTP_EVENT_DISCONNECTED };
struct esp_http_client_event_t {
    esp_http_client_event_id_t event_id;
    void* user_data;
    const char* header_key;
    const char* header_value;
};
struct esp_http_client_config_t {
    const char* url = nullptr;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    int timeout_ms = 0, max_authorization_retries = 0, buffer_size = 0, buffer_size_tx = 0;
    bool disable_auto_redirect = false, is_async = false;
    esp_err_t (*event_handler)(esp_http_client_event_t*) = nullptr;
    void* user_data = nullptr;
    esp_err_t (*crt_bundle_attach)(void*) = nullptr;
};
struct FakeReadStep {
    int result;
    int socket_errno = 0;
    bool complete = false;
    bool disconnected = false;
};
struct FakeHttpResponse {
    int status = 200;
    int64_t length = 5;
    bool chunked = false, eof_complete = true, disconnect_on_eof = true;
    int open_result = ESP_OK, fetch_result = ESP_OK;
    std::string body = "hello";
    std::map<std::string, std::string> headers;
    std::deque<FakeReadStep> steps;
};
struct FakeIdfClient {
    esp_http_client_config_t config;
    FakeHttpResponse response;
    size_t position = 0;
    bool complete = false;
    int timeout = 0;
    std::thread::id owner;
};
using esp_http_client_handle_t = FakeIdfClient*;
inline FakeHttpResponse next_response;
inline esp_http_client_config_t last_config;
inline FakeIdfClient* last_client = nullptr;
inline std::map<std::string, std::string> sent_headers;
inline std::string sent_content;
inline int live_clients = 0, cleanups = 0, close_calls = 0, read_calls = 0;
inline int callbacks_during_close = 0, last_timeout = 0;
inline void Emit(FakeIdfClient* client, esp_http_client_event_id_t id, const char* key = nullptr,
                 const char* value = nullptr) {
    assert(client->owner == std::this_thread::get_id());
    if (client->config.event_handler) {
        esp_http_client_event_t event{id, client->config.user_data, key, value};
        client->config.event_handler(&event);
    }
}
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    auto* client = new FakeIdfClient{*config, next_response,      0,
                                     false,   config->timeout_ms, std::this_thread::get_id()};
    assert(!config->is_async);
    assert(config->disable_auto_redirect);
    last_config = *config;
    last_client = client;
    sent_headers.clear();
    sent_content.clear();
    ++live_clients;
    return client;
}
inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char* key,
                                            const char* value) {
    sent_headers[key] = value;
    return ESP_OK;
}
inline esp_err_t esp_http_client_open(esp_http_client_handle_t client, int) {
    assert(client->owner == std::this_thread::get_id());
    return client->response.open_result;
}
inline int esp_http_client_write(esp_http_client_handle_t client, const char* body, int length) {
    assert(client->owner == std::this_thread::get_id());
    // Force partial writes so SetContent's production send loop is exercised.
    const int count = std::min(length, 2);
    sent_content.append(body, count);
    return count;
}
inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) {
    for (const auto& header : client->response.headers)
        Emit(client, HTTP_EVENT_ON_HEADER, header.first.c_str(), header.second.c_str());
    if (client->response.fetch_result != ESP_OK)
        return client->response.fetch_result;
    return std::max<int64_t>(client->response.length, 0);
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    return client->response.status;
}
inline int64_t esp_http_client_get_content_length(esp_http_client_handle_t client) {
    return client->response.length;
}
inline bool esp_http_client_is_chunked_response(esp_http_client_handle_t client) {
    return client->response.chunked;
}
inline esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout) {
    client->timeout = last_timeout = timeout;
    return ESP_OK;
}
inline int esp_http_client_read(esp_http_client_handle_t client, char* bytes, int length) {
    assert(client->owner == std::this_thread::get_id());
    ++read_calls;
    if (!client->response.steps.empty()) {
        auto step = client->response.steps.front();
        client->response.steps.pop_front();
        errno = step.socket_errno;
        client->complete = step.complete;
        if (step.disconnected)
            Emit(client, HTTP_EVENT_DISCONNECTED);
        if (step.result <= 0)
            return step.result;
        length = std::min(length, step.result);
    }
    const size_t count = std::min(size_t(length), client->response.body.size() - client->position);
    std::memcpy(bytes, client->response.body.data() + client->position, count);
    client->position += count;
    if (!count) {
        client->complete =
            client->response.eof_complete &&
            (client->response.length < 0 || client->position >= uint64_t(client->response.length));
        if (client->response.disconnect_on_eof)
            Emit(client, HTTP_EVENT_DISCONNECTED);
    }
    return static_cast<int>(count);
}
inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
    return client->complete;
}
inline esp_err_t esp_http_client_close(esp_http_client_handle_t client) {
    assert(client->owner == std::this_thread::get_id());
    ++close_calls;
    ++callbacks_during_close;
    Emit(client, HTTP_EVENT_DISCONNECTED);
    return ESP_OK;
}
inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
    assert(client->owner == std::this_thread::get_id());
    // Include a cleanup-time callback: the owner's fields must still be alive.
    Emit(client, HTTP_EVENT_DISCONNECTED);
    --live_clients;
    ++cleanups;
    delete client;
    last_client = nullptr;
    return ESP_OK;
}
