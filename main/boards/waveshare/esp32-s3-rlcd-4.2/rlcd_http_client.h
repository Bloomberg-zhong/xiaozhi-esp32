#pragma once

#include <esp_http_client.h>

#include <map>
#include <optional>
#include <string>

#include "http.h"

// Synchronous board HTTP: its owner task performs all I/O and cleanup, so no
// TCP receive task can call into a destroyed Http object after passive EOF.
class RlcdHttpClient final : public Http {
public:
    RlcdHttpClient() = default;
    ~RlcdHttpClient() override;
    void SetTimeout(int timeout_ms) override;
    void SetHeader(const std::string& key, const std::string& value) override;
    void SetContent(std::string&& content) override;
    void SetKeepAlive(bool enable) override;
    NetworkResult<> Open(const std::string& method, const std::string& url) override;
    void Close() override;
    NetworkResult<int> Read(char* buffer, size_t buffer_size) override;
    NetworkResult<int> Write(const char* buffer, size_t buffer_size) override;
    NetworkResult<int> GetStatusCode() override;
    std::string GetResponseHeader(const std::string& key) const override;
    size_t GetBodyLength() override;
    std::string ReadAll() override;

private:
    static esp_err_t OnEvent(esp_http_client_event_t* event);
    esp_http_client_handle_t client_ = nullptr;
    std::map<std::string, std::string> request_headers_, response_headers_;
    std::optional<std::string> content_;
    int timeout_ms_ = 8000;
    int status_ = 0;
    int64_t content_length_ = -1;
    size_t received_ = 0, response_header_bytes_ = 0;
    bool keep_alive_ = false, head_ = false, complete_ = false;
    bool disconnected_ = false, header_overflow_ = false;
};
