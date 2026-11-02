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

#include "SceAudiodecUser.h"

#include <array>
#include <audio/state.h>
#include <chrono>
#include <codec/state.h>
#include <kernel/state.h>
#include <mutex>
#include <util/lock_and_find.h>
#include <util/tracy.h>

TRACY_MODULE_NAME(SceAudiodecUser);

enum {
    SCE_AUDIODEC_ERROR_API_FAIL = 0x807F0000,
    SCE_AUDIODEC_ERROR_INVALID_TYPE = 0x807F0001,
    SCE_AUDIODEC_ERROR_NOT_INITIALIZED = 0x807F0005,
    SCE_AUDIODEC_ERROR_INVALID_PTR = 0x807F0008,
    SCE_AUDIODEC_ERROR_INVALID_HANDLE = 0x807F0009,
    SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE = 0x807F000A,
    SCE_AUDIODEC_ERROR_INVALID_SIZE = 0x807F000D,
    SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG = 0x807F2000,
    SCE_AUDIODEC_MP3_ERROR_INVALID_MPEG_VERSION = 0x807F2801,
};

enum {
    SCE_AUDIODEC_MP3_MPEG_VERSION_2_5,
    SCE_AUDIODEC_MP3_MPEG_VERSION_RESERVED,
    SCE_AUDIODEC_MP3_MPEG_VERSION_2,
    SCE_AUDIODEC_MP3_MPEG_VERSION_1,
};

typedef std::shared_ptr<DecoderState> DecoderPtr;
typedef std::map<SceUID, DecoderPtr> DecoderStates;
typedef std::set<SceUID> CodecDecoders;
typedef std::map<SceAudiodecCodec, CodecDecoders> CodecDecodersMap;

struct AudiodecState {
    std::mutex mutex;
    DecoderStates decoders;
    CodecDecodersMap codecs;
};

struct SceAudiodecInfoAt9 {
    uint32_t config_data;
    uint32_t channels;
    uint32_t bit_rate;
    uint32_t sample_rate;
    uint32_t super_frame_size;
    uint32_t frames_in_super_frame;
};

struct SceAudiodecInfoMp3 {
    uint32_t channels;
    uint32_t version;
};

struct SceAudiodecInfoAac {
    uint32_t is_adts;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t is_sbr;
};

struct SceAudiodecInfoCelp {
    uint32_t excitation_mode;
    uint32_t sample_rate;
    uint32_t bit_rate;
    uint32_t lost_count;
};

struct SceAudiodecInfo {
    uint32_t size;
    union {
        SceAudiodecInfoAt9 at9;
        SceAudiodecInfoMp3 mp3;
        SceAudiodecInfoAac aac;
        SceAudiodecInfoCelp celp;
    };
};

struct SceAudiodecCtrl {
    uint32_t size;
    SceUID handle;
    Ptr<uint8_t> es_data;
    uint32_t es_size_used;
    uint32_t es_size_max;
    Ptr<uint8_t> pcm_data;
    uint32_t pcm_size_given;
    uint32_t pcm_size_max;
    uint32_t word_length;
    Ptr<SceAudiodecInfo> info;
};

static_assert(sizeof(SceAudiodecCtrl) == 0x28);

constexpr uint32_t SCE_AUDIODEC_AT9_MAX_ES_SIZE = 1024;
constexpr uint32_t SCE_AUDIODEC_MP3_MAX_ES_SIZE = 1441;
// max size is 1792 for AAC ES if adts is enabled
constexpr uint32_t SCE_AUDIODEC_AAC_MAX_ES_SIZE = 1536;
constexpr uint32_t SCE_AUDIODEC_CELP_MAX_ES_SIZE = 27;

// this value is multiplied by 2 if sbr is enabled
constexpr uint32_t SCE_AUDIODEC_AAC_MAX_PCM_SIZE = KiB(2);
constexpr uint32_t SCE_AUDIODEC_MP3_V1_MAX_PCM_SIZE = 2304;
constexpr uint32_t SCE_AUDIODEC_MP3_V2_MAX_PCM_SIZE = 1152;

LIBRARY_INIT(SceAudiodec) {
    emuenv.kernel.obj_store.create<AudiodecState>();
}

EXPORT(int, sceAudiodecClearContext, SceAudiodecCtrl *ctrl) {
    TRACY_FUNC(sceAudiodecClearContext, ctrl)

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (ctrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    if (state->codecs.empty()) {
        return SCE_AUDIODEC_ERROR_NOT_INITIALIZED;
    }
    if (!ctrl->handle) {
        return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    }

    const DecoderPtr &decoder = lock_and_find(ctrl->handle, state->decoders, state->mutex);

    if (decoder) {
        decoder->flush();
    } else {
        return SCE_AUDIODEC_ERROR_NOT_HANDLE_IN_USE;
    }

    return 0;
}

static int create_decoder(EmuEnvState &emuenv, SceAudiodecCtrl *ctrl, SceAudiodecCodec codec) {
    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (ctrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    std::lock_guard<std::mutex> lock(state->mutex);

    SceUID handle = emuenv.kernel.get_next_uid();
    ctrl->handle = handle;
    state->codecs[codec].insert(handle);

    switch (codec) {
    case SCE_AUDIODEC_TYPE_AT9: {
        SceAudiodecInfoAt9 &info = ctrl->info.get(emuenv.mem)->at9;
        DecoderPtr decoder = std::make_shared<Atrac9DecoderState>(info.config_data);
        state->decoders[handle] = decoder;

        info.channels = decoder->get(DecoderQuery::CHANNELS);
        info.bit_rate = decoder->get(DecoderQuery::BIT_RATE);
        info.sample_rate = decoder->get(DecoderQuery::SAMPLE_RATE);
        info.super_frame_size = decoder->get(DecoderQuery::AT9_SUPERFRAME_SIZE);
        info.frames_in_super_frame = decoder->get(DecoderQuery::AT9_FRAMES_IN_SUPERFRAME);
        ctrl->es_size_max = std::min(info.super_frame_size, SCE_AUDIODEC_AT9_MAX_ES_SIZE);
        ctrl->pcm_size_max = decoder->get(DecoderQuery::AT9_SAMPLE_PER_FRAME)
            * decoder->get(DecoderQuery::CHANNELS) * sizeof(int16_t);
        return 0;
    }
    case SCE_AUDIODEC_TYPE_AAC: {
        SceAudiodecInfoAac &info = ctrl->info.get(emuenv.mem)->aac;
        DecoderPtr decoder = std::make_shared<AacDecoderState>(info.sample_rate, info.channels);
        state->decoders[handle] = decoder;

        ctrl->es_size_max = SCE_AUDIODEC_AAC_MAX_ES_SIZE;
        if (info.is_adts)
            ctrl->es_size_max += 0x100;
        ctrl->pcm_size_max = info.channels * SCE_AUDIODEC_AAC_MAX_PCM_SIZE;
        if (info.is_sbr)
            ctrl->pcm_size_max *= 2;

        LOG_WARN_IF(info.is_adts || info.is_sbr, "report it to dev, is_adts: {}, is_sbr: {}", info.is_adts, info.is_sbr);

        return 0;
    }
    case SCE_AUDIODEC_TYPE_MP3: {
        SceAudiodecInfoMp3 &info = ctrl->info.get(emuenv.mem)->mp3;
        DecoderPtr decoder = std::make_shared<Mp3DecoderState>(info.channels);
        state->decoders[handle] = decoder;

        ctrl->es_size_max = SCE_AUDIODEC_MP3_MAX_ES_SIZE;

        switch (info.version) {
        case SCE_AUDIODEC_MP3_MPEG_VERSION_1:
            ctrl->pcm_size_max = info.channels * SCE_AUDIODEC_MP3_V1_MAX_PCM_SIZE;
            return 0;
        case SCE_AUDIODEC_MP3_MPEG_VERSION_2:
        case SCE_AUDIODEC_MP3_MPEG_VERSION_2_5:
            ctrl->pcm_size_max = info.channels * SCE_AUDIODEC_MP3_V2_MAX_PCM_SIZE;
            return 0;
        default:
            LOG_ERROR("Invalid MPEG version {}.", info.version);
            return SCE_AUDIODEC_MP3_ERROR_INVALID_MPEG_VERSION;
        }
    }
    default: {
        LOG_ERROR("Unimplemented audio decoder {}.", codec);
        return -1;
    }
    }
}

EXPORT(int, sceAudiodecCreateDecoder, SceAudiodecCtrl *ctrl, SceAudiodecCodec codec) {
    TRACY_FUNC(sceAudiodecCreateDecoder, ctrl, codec);
    return create_decoder(emuenv, ctrl, codec);
}

EXPORT(int, sceAudiodecCreateDecoderExternal, SceAudiodecCtrl *ctrl, SceAudiodecCodec codec, void *context, uint32_t size) {
    TRACY_FUNC(sceAudiodecCreateDecoderExternal, ctrl, codec, context, size);
    // I think context is supposed to be just extra memory where I can allocate my context.
    // I'm just going to allocate like regular sceAudiodecCreateDecoder and see how it goes.
    // Almost sure zang has already tried this so :/ - desgroup
    return create_decoder(emuenv, ctrl, codec);
}

EXPORT(int, sceAudiodecCreateDecoderResident) {
    TRACY_FUNC(sceAudiodecCreateDecoderResident);
    return UNIMPLEMENTED();
}

// ---------------------------------------------------------------------------
// Vita3K-BG diagnostics: remember the last decode calls (where they wrote PCM,
// how much, and what sizes were reported back) so they can be dumped when a
// game's memory looks corrupted.
// ---------------------------------------------------------------------------
namespace {
struct BgDecodeRecord {
    uint64_t time_us = 0;
    const char *func = "";
    SceUID handle = 0;
    uint32_t es_addr = 0, es_size_max = 0, es_size_used = 0;
    uint32_t pcm_addr = 0, pcm_size_max = 0, pcm_size_given = 0;
    uint32_t offset = 0, count = 0;
    uint32_t write_start = 0, write_end = 0;
};
std::mutex bg_decode_mutex;
std::array<BgDecodeRecord, 32> bg_decode_ring{};
size_t bg_decode_next = 0;

uint64_t bg_decode_now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void bg_record_decode(const BgDecodeRecord &r) {
    const std::lock_guard<std::mutex> lock(bg_decode_mutex);
    bg_decode_ring[bg_decode_next] = r;
    bg_decode_next = (bg_decode_next + 1) % bg_decode_ring.size();
}
} // namespace

void bg_dump_audiodec_trace() {
    const std::lock_guard<std::mutex> lock(bg_decode_mutex);
    const uint64_t now = bg_decode_now();
    LOG_WARN("[Vita3K-BG] ---- last audio decode calls, oldest first ----");
    for (size_t i = 0; i < bg_decode_ring.size(); i++) {
        const BgDecodeRecord &r = bg_decode_ring[(bg_decode_next + i) % bg_decode_ring.size()];
        if (r.time_us == 0)
            continue;
        LOG_WARN("[Vita3K-BG]   -{:>6} ms {} h={} off={} n={} es@{} used/max={}/{} pcm@{} given/max={}/{} wrote {}..{}",
            (now - r.time_us) / 1000, r.func, r.handle, r.offset, r.count,
            log_hex(r.es_addr), r.es_size_used, r.es_size_max,
            log_hex(r.pcm_addr), r.pcm_size_given, r.pcm_size_max,
            log_hex(r.write_start), log_hex(r.write_end));
    }
    LOG_WARN("[Vita3K-BG] ---- end of decode trace ----");
}

static int decode_audio_frames(EmuEnvState &emuenv, const char *export_name, SceAudiodecCtrl *ctrl, SceUInt32 nb_frames) {
    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (ctrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    const DecoderPtr &decoder = lock_and_find(ctrl->handle, state->decoders, state->mutex);

    uint8_t *es_data = ctrl->es_data.get(emuenv.mem);
    uint8_t *pcm_data = ctrl->pcm_data.get(emuenv.mem);

    ctrl->es_size_used = 0;
    ctrl->pcm_size_given = 0;

    for (uint32_t frame = 0; frame < nb_frames; frame++) {
        DecoderSize size;
        if (!decoder->send(es_data, ctrl->es_size_max)
            || !decoder->receive(pcm_data, &size)) {
            return RET_ERROR(SCE_AUDIODEC_ERROR_API_FAIL);
        }

        uint32_t es_size_used = std::min(decoder->get_es_size(), ctrl->es_size_max);
        assert(es_size_used <= ctrl->es_size_max);
        ctrl->es_size_used += es_size_used;
        es_data += es_size_used;

        uint32_t pcm_size_given = size.samples * decoder->get(DecoderQuery::CHANNELS) * sizeof(int16_t);
        assert(pcm_size_given <= ctrl->pcm_size_max);
        ctrl->pcm_size_given += pcm_size_given;
        pcm_data += pcm_size_given;
    }

    {
        BgDecodeRecord r;
        r.time_us = bg_decode_now();
        r.func = export_name;
        r.handle = ctrl->handle;
        r.es_addr = ctrl->es_data.address();
        r.es_size_max = ctrl->es_size_max;
        r.es_size_used = ctrl->es_size_used;
        r.pcm_addr = ctrl->pcm_data.address();
        r.pcm_size_max = ctrl->pcm_size_max;
        r.pcm_size_given = ctrl->pcm_size_given;
        r.count = nb_frames;
        r.write_start = r.pcm_addr;
        r.write_end = r.pcm_addr + ctrl->pcm_size_given;
        bg_record_decode(r);
        if (ctrl->pcm_size_max && ctrl->pcm_size_given > ctrl->pcm_size_max)
            LOG_WARN("[Vita3K-BG] {} wrote {} bytes but the game's buffer is only {} bytes", export_name, ctrl->pcm_size_given, ctrl->pcm_size_max);
    }

    return 0;
}

EXPORT(int, sceAudiodecDecode, SceAudiodecCtrl *ctrl) {
    TRACY_FUNC(sceAudiodecDecode, ctrl);
    return decode_audio_frames(emuenv, export_name, ctrl, 1);
}

EXPORT(int, sceAudiodecDecodeNFrames, SceAudiodecCtrl *ctrl, SceUInt32 nFrames) {
    TRACY_FUNC(sceAudiodecDecodeNFrames, ctrl, nFrames);
    return decode_audio_frames(emuenv, export_name, ctrl, nFrames);
}

EXPORT(int, sceAudiodecDecodeNStreams, SceAudiodecCtrl *ctrl, SceUInt32 nStreams) {
    TRACY_FUNC(sceAudiodecDecodeNStreams, ctrl, nStreams);

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (ctrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    return UNIMPLEMENTED();
}

EXPORT(int, sceAudiodecDeleteDecoder, SceAudiodecCtrl *ctrl) {
    TRACY_FUNC(sceAudiodecDeleteDecoder, ctrl);

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (ctrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->decoders.erase(ctrl->handle);

    // there are at most 4 different codecs, we can afford to look
    // at all of them (the handle is in one of them)
    for (auto &codec : state->codecs) {
        codec.second.erase(ctrl->handle);
    }

    return 0;
}

EXPORT(int, sceAudiodecDeleteDecoderExternal, SceAudiodecCtrl *ctrl, void *context) {
    TRACY_FUNC(sceAudiodecDeleteDecoderExternal, ctrl, context);
    return CALL_EXPORT(sceAudiodecDeleteDecoder, ctrl);
}

EXPORT(int, sceAudiodecDeleteDecoderResident) {
    TRACY_FUNC(sceAudiodecDeleteDecoderResident);
    return UNIMPLEMENTED();
}

static std::uint32_t getAt9Factor(const std::uint8_t *config_data) {
    std::uint32_t value = (config_data[1] & 0xf) >> 1;
    if (value == 0)
        return 1;
    if ((value == 1) || (value == 2))
        return 2;
    return SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG;
}

EXPORT(int, sceAudiodecGetContextSize, SceAudiodecCtrl *pCtrl, SceAudiodecCodec codecType) {
    TRACY_FUNC(sceAudiodecGetContextSize, pCtrl, codecType);

    if (!pCtrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (pCtrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    switch (codecType) {
    case SCE_AUDIODEC_TYPE_AT9: {
        const std::uint32_t at9Factor = getAt9Factor(reinterpret_cast<std::uint8_t *>(&pCtrl->info.get(emuenv.mem)->at9.config_data));
        if (at9Factor == 1 || at9Factor == 2) {
            return 0x400 * at9Factor + 0x400;
        }
        return SCE_AUDIODEC_AT9_ERROR_INVALID_CONFIG;
    }
    case SCE_AUDIODEC_TYPE_AAC:
        return 0x18000;
    case SCE_AUDIODEC_TYPE_MP3:
    case SCE_AUDIODEC_TYPE_CELP:
        return 0;
    default:
        // Found these during reverse engineering, log them in case we need an implementation
        LOG_WARN_IF(codecType == 0x1007 || codecType == 0x1008, "Unsupported codec type {}", codecType);
        return SCE_AUDIODEC_ERROR_INVALID_TYPE;
    }
}

EXPORT(int, sceAudiodecGetInternalError) {
    TRACY_FUNC(sceAudiodecGetInternalError);
    return UNIMPLEMENTED();
}

EXPORT(SceInt32, sceAudiodecInitLibrary, SceAudiodecCodec codecType, SceAudiodecInitParam *pInitParam) {
    TRACY_FUNC(sceAudiodecInitLibrary, codecType, pInitParam);

    if (!pInitParam)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    std::lock_guard<std::mutex> lock(state->mutex);

    state->codecs[codecType] = CodecDecoders();
    return 0;
}

EXPORT(int, sceAudiodecPartlyDecode, SceAudiodecCtrl *ctrl, SceUInt32 samples_offset, SceUInt32 samples_to_decode) {
    TRACY_FUNC(sceAudiodecPartlyDecode, ctrl, samples_offset, samples_to_decode);

    if (!ctrl)
        return SCE_AUDIODEC_ERROR_INVALID_PTR;

    if (ctrl->size != sizeof(SceAudiodecCtrl))
        return SCE_AUDIODEC_ERROR_INVALID_SIZE;

    // this function is only called by libatrac
    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    if (!state->codecs[SCE_AUDIODEC_TYPE_AT9].contains(ctrl->handle)) {
        STUBBED("Call to sceAudiodecPartlyDecode with a codec other than Atrac9, report it to the devs");
    }

    const std::shared_ptr<DecoderState> &decoder = lock_and_find(ctrl->handle, state->decoders, state->mutex);

    uint8_t *es_data = ctrl->es_data.get(emuenv.mem);
    uint8_t *pcm_data = ctrl->pcm_data.get(emuenv.mem);

    // TODO: if the offset is too big, do not decode the first superframes (doesn't seem to happen with libatrac)
    const uint32_t bytes_per_sample = decoder->get(DecoderQuery::CHANNELS) * sizeof(int16_t);
    ctrl->es_size_used = 0;
    ctrl->pcm_size_given = 0;
    std::vector<uint8_t> temp_storage;
    temp_storage.reserve((samples_offset + samples_to_decode) * bytes_per_sample);

    while (ctrl->pcm_size_given < (samples_offset + samples_to_decode) * bytes_per_sample) {
        DecoderSize size;
        if (!decoder->send(es_data, ctrl->es_size_max)) {
            return RET_ERROR(SCE_AUDIODEC_ERROR_API_FAIL);
        }
        const uint32_t es_size_used = decoder->get_es_size();
        assert(es_size_used <= ctrl->es_size_max);
        ctrl->es_size_used += es_size_used;
        es_data += es_size_used;

        decoder->receive(nullptr, &size);
        const uint32_t pcm_size_given = size.samples * bytes_per_sample;
        ctrl->pcm_size_given += pcm_size_given;
        const uint32_t old_size = temp_storage.size();
        temp_storage.resize(old_size + pcm_size_given);
        decoder->receive(temp_storage.data() + old_size, &size);
    }

    memcpy(pcm_data + samples_offset * bytes_per_sample, temp_storage.data() + samples_offset * bytes_per_sample, samples_to_decode * bytes_per_sample);

    {
        BgDecodeRecord r;
        r.time_us = bg_decode_now();
        r.func = "sceAudiodecPartlyDecode";
        r.handle = ctrl->handle;
        r.es_addr = ctrl->es_data.address();
        r.es_size_max = ctrl->es_size_max;
        r.es_size_used = ctrl->es_size_used;
        r.pcm_addr = ctrl->pcm_data.address();
        r.pcm_size_max = ctrl->pcm_size_max;
        r.pcm_size_given = ctrl->pcm_size_given;
        r.offset = samples_offset;
        r.count = samples_to_decode;
        r.write_start = r.pcm_addr + samples_offset * bytes_per_sample;
        r.write_end = r.write_start + samples_to_decode * bytes_per_sample;
        bg_record_decode(r);
        if (ctrl->pcm_size_max && (samples_offset + samples_to_decode) * bytes_per_sample > ctrl->pcm_size_max)
            LOG_WARN("[Vita3K-BG] PartlyDecode wrote past the game's buffer (off {} n {} max {} bytes)", samples_offset, samples_to_decode, ctrl->pcm_size_max);
        if (ctrl->es_size_used > ctrl->es_size_max)
            LOG_WARN("[Vita3K-BG] PartlyDecode reported using {} input bytes but only {} were available", ctrl->es_size_used, ctrl->es_size_max);
    }

    return 0;
}

EXPORT(SceInt32, sceAudiodecTermLibrary, SceAudiodecCodec codecType) {
    TRACY_FUNC(sceAudiodecTermLibrary, codecType);
    const auto state = emuenv.kernel.obj_store.get<AudiodecState>();
    std::lock_guard<std::mutex> lock(state->mutex);

    // remove decoders associated with codecType
    for (auto &handle : state->codecs[codecType]) {
        state->decoders.erase(handle);
    }
    state->codecs.erase(codecType);
    return 0;
}
