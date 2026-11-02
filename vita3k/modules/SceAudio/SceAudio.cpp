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

#include <module/module.h>

#include <audio/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <modules/module_parent.h>
#include <util/lock_and_find.h>
#include <util/tracy.h>

#include <fmt/format.h>

#include <atomic>

TRACY_MODULE_NAME(SceAudio);

enum SceAudioOutMode {
    SCE_AUDIO_OUT_MODE_MONO = 0,
    SCE_AUDIO_OUT_MODE_STEREO = 1
};

enum SceAudioOutAlcMode {
    SCE_AUDIO_ALC_OFF,
    SCE_AUDIO_ALC_MODE1,
    SCE_AUDIO_ALC_MODE_MAX
};

enum SceAudioOutPortType {
    //! Used for main audio output, freq must be set to 48000 Hz
    SCE_AUDIO_OUT_PORT_TYPE_MAIN = 0,
    //! Used for Background Music port
    SCE_AUDIO_OUT_PORT_TYPE_BGM = 1,
    //! Used for voice chat port
    SCE_AUDIO_OUT_PORT_TYPE_VOICE = 2
};

enum SceAudioOutErrorCode : uint32_t {
    SCE_AUDIO_OUT_ERROR_NOT_OPENED = 0x80260001,
    SCE_AUDIO_OUT_ERROR_BUSY = 0x80260002,
    SCE_AUDIO_OUT_ERROR_INVALID_PORT = 0x80260003,
    SCE_AUDIO_OUT_ERROR_INVALID_POINTER = 0x80260004,
    SCE_AUDIO_OUT_ERROR_PORT_FULL = 0x80260005,
    SCE_AUDIO_OUT_ERROR_INVALID_SIZE = 0x80260006,
    SCE_AUDIO_OUT_ERROR_INVALID_FORMAT = 0x80260007,
    SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ = 0x80260008,
    SCE_AUDIO_OUT_ERROR_INVALID_VOLUME = 0x80260009,
    SCE_AUDIO_OUT_ERROR_INVALID_PORT_TYPE = 0x8026000A,
    SCE_AUDIO_OUT_ERROR_INVALID_FX_TYPE = 0x8026000B,
    SCE_AUDIO_OUT_ERROR_INVALID_CONF_TYPE = 0x8026000C,
    SCE_AUDIO_OUT_ERROR_OUT_OF_MEMORY = 0x8026000D
};

enum SceAudioOutChannelFlag {
    SCE_AUDIO_VOLUME_FLAG_L_CH = 1, //!< Left Channel
    SCE_AUDIO_VOLUME_FLAG_R_CH = 2 //!< Right Channel
};

enum SceAudioOutConfigType {
    SCE_AUDIO_OUT_CONFIG_TYPE_LEN,
    SCE_AUDIO_OUT_CONFIG_TYPE_FREQ,
    SCE_AUDIO_OUT_CONFIG_TYPE_MODE
};

template <>
std::string to_debug_str<SceAudioOutPortType>(const MemState &mem, SceAudioOutPortType type) {
    switch (type) {
    case SCE_AUDIO_OUT_PORT_TYPE_MAIN:
        return "SCE_AUDIO_OUT_PORT_TYPE_MAIN";
    case SCE_AUDIO_OUT_PORT_TYPE_BGM:
        return "SCE_AUDIO_OUT_PORT_TYPE_BGM";
    case SCE_AUDIO_OUT_PORT_TYPE_VOICE:
        return "SCE_AUDIO_OUT_PORT_TYPE_VOICE";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutConfigType>(const MemState &mem, SceAudioOutConfigType type) {
    switch (type) {
    case SCE_AUDIO_OUT_CONFIG_TYPE_LEN:
        return "SCE_AUDIO_OUT_CONFIG_TYPE_LEN";
    case SCE_AUDIO_OUT_CONFIG_TYPE_FREQ:
        return "SCE_AUDIO_OUT_CONFIG_TYPE_FREQ";
    case SCE_AUDIO_OUT_CONFIG_TYPE_MODE:
        return "SCE_AUDIO_OUT_CONFIG_TYPE_MODE";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutMode>(const MemState &mem, SceAudioOutMode type) {
    switch (type) {
    case SCE_AUDIO_OUT_MODE_MONO:
        return "SCE_AUDIO_OUT_MODE_MONO";
    case SCE_AUDIO_OUT_MODE_STEREO:
        return "SCE_AUDIO_OUT_MODE_STEREO";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutAlcMode>(const MemState &mem, SceAudioOutAlcMode type) {
    switch (type) {
    case SCE_AUDIO_ALC_OFF:
        return "SCE_AUDIO_ALC_OFF";
    case SCE_AUDIO_ALC_MODE1:
        return "SCE_AUDIO_ALC_MODE1";
    case SCE_AUDIO_ALC_MODE_MAX:
        return "SCE_AUDIO_ALC_MODE_MAX";
    }
    return std::to_string(type);
}

template <>
std::string to_debug_str<SceAudioOutChannelFlag>(const MemState &mem, SceAudioOutChannelFlag type) {
    switch (static_cast<uint32_t>(type)) {
    case SCE_AUDIO_VOLUME_FLAG_L_CH:
        return "SCE_AUDIO_VOLUME_FLAG_L_CH";
    case SCE_AUDIO_VOLUME_FLAG_R_CH:
        return "SCE_AUDIO_VOLUME_FLAG_R_CH";
        // Third case for special flag SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH
        // passed by games to set the volume of both channels at the same time
    case (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH):
        return "SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH";
    }
    return std::to_string(type);
}

// Vita3K-BG diagnostics: describe the calling thread and the open ports so the
// log shows exactly which port number a game used and what existed at the time.
static std::string bg_thread_name(EmuEnvState &emuenv, SceUID thread_id) {
    const ThreadStatePtr t = emuenv.kernel.get_thread(thread_id);
    return t ? t->name : std::string("?");
}

static std::string bg_open_ports(EmuEnvState &emuenv) {
    std::string out;
    for (const auto &[id, p] : emuenv.audio.out_ports)
        out += fmt::format("{}(type {}) ", id, p->type);
    return out.empty() ? std::string("none") : out;
}

static void bg_log_invalid(EmuEnvState &emuenv, SceUID thread_id, const char *func, int port) {
    static std::atomic<int> count{ 0 };
    if (count++ < 40) {
        const std::lock_guard<std::mutex> lock(emuenv.audio.mutex);
        LOG_WARN("[Vita3K-BG] {} called with unknown port {} (0x{:X}) from thread '{}'; open ports: {}",
            func, port, static_cast<uint32_t>(port), bg_thread_name(emuenv, thread_id), bg_open_ports(emuenv));
    }
}

EXPORT(int, sceAudioOutGetAdopt, SceAudioOutPortType type) {
    TRACY_FUNC(sceAudioOutGetAdopt, type);
    // this is used in case the vita is using voice chat, not useful for us
    // all types of audio ports are always enabled
    return 1;
}

EXPORT(int, sceAudioOutGetConfig, int port, SceAudioOutConfigType type) {
    TRACY_FUNC(sceAudioOutGetConfig, type);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutGetPortVolume_forUser) {
    TRACY_FUNC(sceAudioOutGetPortVolume_forUser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutOpenPort, SceAudioOutPortType type, int len, int freq, SceAudioOutMode mode) {
    TRACY_FUNC(sceAudioOutOpenPort, type, len, freq, mode);
    if (type < SCE_AUDIO_OUT_PORT_TYPE_MAIN || type > SCE_AUDIO_OUT_PORT_TYPE_VOICE) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
    }
    if (type == SCE_AUDIO_OUT_PORT_TYPE_MAIN && freq != 48000) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ);
    }
    if ((mode != SCE_AUDIO_OUT_MODE_MONO) && (mode != SCE_AUDIO_OUT_MODE_STEREO)) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_FORMAT);
    }
    if (len <= 0) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SIZE);
    }

    const int channels = (mode == SCE_AUDIO_OUT_MODE_MONO) ? 1 : 2;

    AudioOutPortPtr port = emuenv.audio.open_port(channels, freq, len);
    if (!port)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_NOT_OPENED);

    // Save the port configuration
    port->type = type;
    port->len = len;
    port->freq = freq;
    port->mode = mode;

    const std::lock_guard<std::mutex> lock(emuenv.audio.mutex);
    if (!emuenv.audio.on_port_stall)
        emuenv.audio.on_port_stall = [](int tid) { bg_dump_import_trace(tid); };
    // Reuse the lowest free port number instead of counting up forever.
    // Real hardware only has a handful of ports, so games that open and
    // release ports repeatedly never see large port numbers.
    int port_id = 1;
    while (emuenv.audio.out_ports.contains(port_id))
        port_id++;
    emuenv.audio.out_ports.emplace(port_id, port);
    LOG_WARN("[Vita3K-BG] OpenPort type {} len {} freq {} mode {} -> port {} (thread '{}'); open ports: {}",
        static_cast<int>(type), len, freq, static_cast<int>(mode), port_id, bg_thread_name(emuenv, thread_id), bg_open_ports(emuenv));

    return port_id;
}

EXPORT(int, sceAudioOutOutput, int port, const void *buf) {
    TRACY_FUNC(sceAudioOutOutput, port, buf);
    const AudioOutPortPtr prt = lock_and_find(port, emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt) {
        {
            bg_log_invalid(emuenv, thread_id, "sceAudioOutOutput", port);
            return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
        }
    }

    // Empty "buf" variable is valid. It mean wait until sound output is completed.
    // Because this function always returns when all sound is out, then on empty buf it returns immediately.
    // Return value is the number of samples (value of 0 or greater) registered to the audio driver for normal termination.
    if (!buf)
        return 0;

    const ThreadStatePtr thread = emuenv.kernel.get_thread(thread_id);
    if (!thread) {
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }
    // is it really useful to update the thread status?
    prt->last_thread_id = thread_id;
    thread->begin_host_wait();
    emuenv.audio.audio_output(*prt, buf);
    thread->end_host_wait();

    return prt->len;
}

EXPORT(int, sceAudioOutGetRestSample, int port) {
    TRACY_FUNC(sceAudioOutGetRestSample, port);
    const AudioOutPortPtr prt = lock_and_find(port, emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt) {
        {
            bg_log_invalid(emuenv, thread_id, "sceAudioOutGetRestSample", port);
            return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
        }
    }
    int samples_available = emuenv.audio.get_rest_sample(*prt);
    if (prt->type == SCE_AUDIO_OUT_PORT_TYPE_MAIN) {
        samples_available = std::clamp(samples_available, 0, prt->len);
    } else {
        // for other port types only the granularity value set for the len argument or 0 can be returned.
        if (samples_available < prt->len / 2)
            samples_available = 0;
        else
            samples_available = prt->len;
    }
    return samples_available;
}

EXPORT(int, sceAudioOutOpenExtPort) {
    TRACY_FUNC(sceAudioOutOpenExtPort);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutReleasePort, int port) {
    TRACY_FUNC(sceAudioOutReleasePort, port);
    const std::lock_guard<std::mutex> guard(emuenv.audio.mutex);
    if (!emuenv.audio.out_ports.erase(port)) {
        LOG_WARN("[Vita3K-BG] ReleasePort on unknown port {} (thread '{}'); open ports: {}", port, bg_thread_name(emuenv, thread_id), bg_open_ports(emuenv));
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }
    LOG_WARN("[Vita3K-BG] ReleasePort {} (thread '{}'); open ports: {}", port, bg_thread_name(emuenv, thread_id), bg_open_ports(emuenv));

    return 0;
}

EXPORT(int, sceAudioOutSetAdoptMode) {
    TRACY_FUNC(sceAudioOutSetAdoptMode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetAdopt_forUser) {
    TRACY_FUNC(sceAudioOutSetAdopt_forUser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetAlcMode, SceAudioOutAlcMode mode) {
    TRACY_FUNC(sceAudioOutSetAlcMode, mode);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetCompress) {
    TRACY_FUNC(sceAudioOutSetCompress);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetConfig, int port, int len, int freq, int mode) {
    TRACY_FUNC(sceAudioOutSetConfig, port, len, freq, mode);
    if (len == 0)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SIZE);

    if ((mode >= 0) && (mode != SCE_AUDIO_OUT_MODE_MONO) && (mode != SCE_AUDIO_OUT_MODE_STEREO))
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_FORMAT);

    AudioOutPortPtr prt = lock_and_find(port, emuenv.audio.out_ports, emuenv.audio.mutex);
    if (!prt) {
        bg_log_invalid(emuenv, thread_id, "sceAudioOutSetConfig", port);
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }

    if ((freq >= 0) && (prt->type == SCE_AUDIO_OUT_PORT_TYPE_MAIN) && (freq != 48000))
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ);

    const auto set_len = len > 0 ? len : prt->len;
    const auto set_freq = freq >= 0 ? freq : prt->freq;
    const auto set_mode = mode >= 0 ? mode : prt->mode;

    if ((set_len == prt->len) && (set_freq == prt->freq) && (set_mode == prt->mode))
        return 0;

    // Build the replacement port first, then swap it in under the lock.
    // Previously the old port was released before the new one existed, so a
    // sound thread calling sceAudioOutOutput in between got INVALID_PORT, and
    // the port type and volume were lost on every reconfiguration.
    const auto channels = (set_mode == SCE_AUDIO_OUT_MODE_MONO) ? 1 : 2;

    AudioOutPortPtr new_prt = emuenv.audio.open_port(channels, set_freq, set_len);
    if (!new_prt)
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_NOT_OPENED);

    new_prt->type = prt->type;
    new_prt->freq = set_freq;
    new_prt->len = set_len;
    new_prt->mode = set_mode;
    new_prt->left_channel_volume = prt->left_channel_volume;
    new_prt->right_channel_volume = prt->right_channel_volume;
    new_prt->volume = prt->volume;
    emuenv.audio.set_volume(*new_prt, new_prt->volume);

    // Wake anything still blocked on the old port so it doesn't hang
    // (must happen while the old port is still registered).
    prt->stopping = true;
    emuenv.audio.adapter->wake_all_ports();

    const std::lock_guard<std::mutex> lock(emuenv.audio.mutex);
    auto it = emuenv.audio.out_ports.find(port);
    if (it == emuenv.audio.out_ports.end())
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    it->second = new_prt;
    LOG_WARN("[Vita3K-BG] SetConfig port {} -> len {} freq {} mode {} (thread '{}')", port, set_len, set_freq, set_mode, bg_thread_name(emuenv, thread_id));

    return 0;
}

EXPORT(int, sceAudioOutSetEffectType) {
    TRACY_FUNC(sceAudioOutSetEffectType);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetPortVolume_forUser) {
    TRACY_FUNC(sceAudioOutSetPortVolume_forUser);
    return UNIMPLEMENTED();
}

EXPORT(int, sceAudioOutSetVolume, int port, SceAudioOutChannelFlag ch, int *vol) {
    TRACY_FUNC(sceAudioOutSetVolume, port, ch, vol[0], vol[1]);
    if (ch == 0) // no channel selected, no changes
        return 0;

    const AudioOutPortPtr prt = lock_and_find(port, emuenv.audio.out_ports, emuenv.audio.mutex);

    if (!prt) {
        bg_log_invalid(emuenv, thread_id, "sceAudioOutSetVolume", port);
        return RET_ERROR(SCE_AUDIO_OUT_ERROR_INVALID_PORT);
    }

    const int left = (ch & SCE_AUDIO_VOLUME_FLAG_L_CH) ? vol[0] : prt->left_channel_volume;
    const int right = (ch & SCE_AUDIO_VOLUME_FLAG_R_CH) ? vol[1] : prt->right_channel_volume;
    const float volume_level = (static_cast<float>(left + right) / static_cast<float>(SCE_AUDIO_VOLUME_0DB * 2));

    // then update channel volumes in case there was a change
    prt->left_channel_volume = left;
    prt->right_channel_volume = right;
    emuenv.audio.set_volume(*prt, volume_level);

    return 0;
}
