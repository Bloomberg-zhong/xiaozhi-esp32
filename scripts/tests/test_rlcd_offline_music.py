"""Run production offline controls with real local files and state transitions."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/waveshare/esp32-s3-rlcd-4.2'


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class OfflineMusicTests(unittest.TestCase):
    def compile_run(self, driver, sources=(), music=1):
        if 'main/music/local_music.cc' in sources:
            sources = (*sources, 'main/music/music_cache.cc', 'main/music/music_util.cc')
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            (folder / 'esp_log.h').write_text('#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n')
            path = folder / 'test.cc'
            path.write_text(driver)
            result = subprocess.run([
                os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra',
                '-Werror', '-Wno-unused-variable', '-I' + str(folder),
                f'-DCONFIG_USE_MUSIC_PLAYER={music}',
                '-I' + str(ROOT / 'main'), '-I' + str(BOARD), str(path),
                *[str(ROOT / source) for source in sources], '-o', str(folder / 'test'),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(folder / 'test')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_local_boot_plays_without_protocol_and_reconnect_defers_activation(self):
        source = (ROOT / 'main/application.cc').read_text()
        driver = (ROOT / 'scripts/tests/cpp/offline_music_application_test.cc').read_text()
        methods = '\n'.join(method(source, 'void Application::' + name + '(') for name in (
            'PlayMusic', 'TryStartMusic', 'HandleNetworkConnectedEvent',
            'StopMusicPlayback', 'HandleNetworkDisconnectedEvent',
        ))
        self.compile_run(driver.replace('// PRODUCTION_METHODS', methods), (
            'main/device_state_machine.cc', 'main/music/local_music.cc',
        ))

    def test_key_starts_card_from_idle_and_controls_existing_playback(self):
        source = (BOARD / 'waveshare-s3-rlcd-4.2.cc').read_text()
        methods = method(source, 'void InitializeUserButton()')
        driver = (ROOT / 'scripts/tests/cpp/rlcd_offline_buttons_test.cc').read_text()
        self.compile_run(driver.replace('// PRODUCTION_METHODS', methods))

    def test_boards_without_music_keep_existing_boot_transitions(self):
        self.compile_run(r'''
#include <cassert>
#include "device_state_machine.h"
int main() {
    DeviceStateMachine state;
    assert(state.TransitionTo(kDeviceStateStarting));
    assert(!state.TransitionTo(kDeviceStateIdle));
    assert(state.TransitionTo(kDeviceStateWifiConfiguring));
    assert(!state.TransitionTo(kDeviceStateIdle));
    assert(state.TransitionTo(kDeviceStateActivating));
    assert(state.TransitionTo(kDeviceStateIdle));
}
''', ('main/device_state_machine.cc',), music=0)

    def test_card_scan_is_async_bounded_cancellable_and_never_uses_server(self):
        source = (BOARD / 'waveshare-s3-rlcd-4.2.cc').read_text()
        methods = method(source, 'void StartLocalMusic()')
        driver = (ROOT / 'scripts/tests/cpp/rlcd_local_scan_test.cc').read_text()
        self.compile_run(driver.replace('// PRODUCTION_METHODS', methods), ('main/music/local_music.cc',))

    def test_offline_assets_are_not_reloaded_when_voice_activation_runs(self):
        source = (ROOT / 'main/application.cc').read_text()
        self.assertIn('void Application::ApplyInstalledAssets()', source)
        methods = method(source, 'void Application::ApplyInstalledAssets()')
        driver = r'''
#include <cassert>
struct Assets {
    int applies = 0;
    bool valid = true;
    static Assets& GetInstance() { static Assets a; return a; }
    bool Apply() { ++applies; return valid; }
};
struct Application { bool assets_applied_ = false; void ApplyInstalledAssets(); };
// PRODUCTION_METHODS
int main() {
    Application app;
    app.ApplyInstalledAssets(); // offline start loads fonts and wake models
    app.ApplyInstalledAssets(); // online activation must retain live model pointers
    assert(Assets::GetInstance().applies == 1);
    Assets::GetInstance().valid = false;
    Application failed;
    failed.ApplyInstalledAssets();
    Assets::GetInstance().valid = true;
    failed.ApplyInstalledAssets();
    assert(failed.assets_applied_ && Assets::GetInstance().applies == 3);
}
'''
        self.compile_run(driver.replace('// PRODUCTION_METHODS', methods))
