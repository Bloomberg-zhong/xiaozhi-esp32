"""Exercise the production board router and GetNetwork with host network doubles."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/waveshare/esp32-s3-rlcd-4.2"


def definition(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class RlcdHttpRouteTest(unittest.TestCase):
    def test_board_routes_music_api_weather_cover_and_cache_to_owned_http(self):
        source = (BOARD / "waveshare-s3-rlcd-4.2.cc").read_text()
        router = definition(source, "class RlcdNetwork : public EspNetwork") + ";"
        get_network = definition(source, "NetworkInterface* GetNetwork() override")
        driver = r'''
#include <cassert>
#include <memory>
#include <vector>
struct Http { virtual ~Http() = default; };
struct NetworkInterface {
    virtual ~NetworkInterface() = default;
    virtual std::unique_ptr<Http> CreateHttp(int connect_id = -1) = 0;
};
struct VendorHttp : Http { int id; explicit VendorHttp(int connect_id) : id(connect_id) {} };
struct RlcdHttpClient : Http {};
struct EspNetwork : NetworkInterface {
    std::vector<int> base_requests;
    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override {
        base_requests.push_back(connect_id);
        return std::make_unique<VendorHttp>(connect_id);
    }
};
struct WifiBoard {
    virtual ~WifiBoard() = default;
    virtual NetworkInterface* GetNetwork() = 0;
};
'''
        driver += router + "\nstruct CustomBoard : WifiBoard {\n" + get_network + "\n};\n"
        driver += r'''
int main() {
    CustomBoard board;
    auto* network = board.GetNetwork();
    assert(network == board.GetNetwork()); // repeated calls retain the same owned router
    auto* router = dynamic_cast<RlcdNetwork*>(network);
    assert(router);
    for (const int id : {4, 5, 6, 7, 8}) {
        auto http = network->CreateHttp(id);
        assert(dynamic_cast<RlcdHttpClient*>(http.get()));
        assert(router->base_requests.empty());
    }
    for (const int id : {-1, 0, 3, 9}) {
        auto http = network->CreateHttp(id);
        auto* vendor = dynamic_cast<VendorHttp*>(http.get());
        assert(vendor && vendor->id == id);
    }
    assert((router->base_requests == std::vector<int>{-1, 0, 3, 9}));
    auto default_http = network->CreateHttp();
    auto* vendor = dynamic_cast<VendorHttp*>(default_http.get());
    assert(vendor && vendor->id == -1);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            cpp = pathlib.Path(temp) / "route.cc"
            exe = pathlib.Path(temp) / "route"
            cpp.write_text(driver)
            result = subprocess.run([
                os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                str(cpp), "-o", str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
