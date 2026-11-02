// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <audio/state.h>

#include <audio/impl/cubeb_audio.h>
#include <audio/impl/sdl_audio.h>

#include <util/log.h>

#include <cassert>
#include <chrono>
#include <cstring>

static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Vita3K-BG: logs (at warning level) when a port that was being fed every frame
// suddenly stops receiving audio. Tells us whether the game's sound thread got
// stuck somewhere before it reached the audio output.
void AudioState::start_watchdog() {
    if (watchdog.joinable())
        return;
    watchdog = std::jthread([this](std::stop_token stop) {
        std::map<int, bool> reported;
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const std::lock_guard<std::mutex> lock(mutex);
            const uint64_t now = now_us();
            for (const auto &[id, port] : out_ports) {
                if (port->last_output == 0)
                    continue;
                const uint64_t idle = now > port->last_output ? now - port->last_output : 0;
                if (idle > 2'000'000) {
                    if (!reported[id]) {
                        reported[id] = true;
                        LOG_WARN("[Vita3K-BG] audio port {} (type {}) has received no audio for {} ms", id, port->type, idle / 1000);
                        if (on_port_stall && port->last_thread_id != 0)
                            on_port_stall(port->last_thread_id);
                    }
                } else if (reported[id]) {
                    reported[id] = false;
                    LOG_WARN("[Vita3K-BG] audio port {} is receiving audio again", id);
                }
            }
        }
    });
}

bool AudioState::init(const std::string &adapter_name) {
    start_watchdog();
    set_backend(adapter_name);
    if (!adapter)
        return false;

    return true;
}

void AudioState::stop_all_ports() {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (auto &[_, port] : out_ports) {
            port->stopping = true;
        }
    }
    if (adapter)
        adapter->wake_all_ports();
}

void AudioState::deinit() {
    stop_all_ports();

    const std::lock_guard<std::mutex> lock(mutex);

    out_ports.clear();

    if (in_port.running) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream *>(in_port.id));
        in_port.id = nullptr;
        in_port.running = false;
        in_port.len_bytes = 0;
    }

    next_port_id = 1;
}

void AudioState::set_backend(const std::string &adapter_name) {
    if (adapter_name == this->audio_backend)
        return;

    // first delete all ports then delete the backend
    out_ports.clear();
    adapter.reset();
    if (adapter_name == "SDL") {
        adapter = std::make_unique<SDLAudioAdapter>(*this);
    } else if (adapter_name == "Cubeb") {
        adapter = std::make_unique<CubebAudioAdapter>(*this);
    } else {
        LOG_ERROR("Unknown audio adapter {}", adapter_name);
        return;
    }
    this->audio_backend = adapter_name;

    // lock the mutex to make sure nothing happens until the initialisation is done
    const std::lock_guard<std::mutex> lock(mutex);
    if (!adapter->init()) {
        adapter.reset();
    }
}

AudioOutPortPtr AudioState::open_port(int nb_channels, int freq, int nb_sample) {
    AudioOutPortPtr port = adapter->open_port(nb_channels, freq, nb_sample);
    set_volume(*port, port->volume);
    return port;
}

void AudioState::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    adapter->audio_output(out_port, buffer);

    if (out_port.stopping)
        return;

    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t diff = now - out_port.last_output;
    uint64_t to_wait = out_port.len_microseconds - diff;
    if (diff < out_port.len_microseconds && to_wait > 1000) {
        // This is what we should be waiting to be perfectly accurate
        // However, doing so would cause the host audio buffer to often lack samples to output
        // This is because the PS Vita and the host audio parameters do not match exactly
        // So instead only wait 50% of the time
        // also don't sleep for less than 0.5 ms
        to_wait /= 2;
        std::this_thread::sleep_for(std::chrono::microseconds(to_wait));
        out_port.last_output = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        out_port.last_output = now;
    }
}

void AudioState::set_volume(AudioOutPort &out_port, float volume) {
    out_port.volume = volume;
    adapter->set_volume(out_port, volume * global_volume);
}

void AudioState::set_global_volume(float volume) {
    global_volume = volume;
    //  Update adapter volume for each port.
    const std::lock_guard lock(mutex);
    for (const auto &[_, port] : out_ports) {
        adapter->set_volume(*port, port->volume * volume);
    }
}

void AudioState::switch_state(const bool pause) {
    adapter->switch_state(pause);
}

int AudioState::get_rest_sample(AudioOutPort &out_port) {
    return adapter->get_rest_sample(out_port);
}

void AudioState::wake_all_ports() {
    const std::lock_guard<std::mutex> lock(mutex);
    for (auto &[_, port] : out_ports) {
        port->stopping = true;
    }
    if (adapter)
        adapter->wake_all_ports();
}