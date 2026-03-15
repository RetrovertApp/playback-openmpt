///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// OpenMPT Playback Plugin
//
// Implements RVPlaybackPlugin interface using libopenmpt to decode tracker music formats.
// Supports: MOD, XM, S3M, IT, MPTM, and many other tracker formats.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <libopenmpt/libopenmpt.h>
#include <libopenmpt/libopenmpt.hpp>

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
#include <retrovert/settings.h>

#include <assert.h>
#include <string.h>

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const int MAX_EXT_COUNT = 16 * 1024;
static char s_supported_extensions[MAX_EXT_COUNT];

// Global API pointers (set during static_init)
RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();
const RVSettings* g_settings_api = nullptr;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Setting IDs

#define ID_SAMPLE_RATE "SampleRate"
#define ID_CHANNELS "Channels"
#define ID_MASTER_GAIN "MasterGain"
#define ID_STEREO_SEPARATION "StereoSeparation"
#define ID_VOLUME_RAMPING "VolumeRamping"
#define ID_INTERPOLATION_FILTER "InterpolationFilter"
#define ID_TEMPO_FACTOR "TempoFactor"
#define ID_PITCH_FACTOR "PitchFactor"
#define ID_USE_AMIGA_RESAMPLER "AmigaModResampling"
#define ID_AMIGA_RESAMPLER_FILTER "AmigaModResamplerFilter"

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Settings range value tables

// clang-format off
static RVSIntegerRangeValue s_sample_rate_values[] = {
    { "8000 Hz",  8000 },
    { "11025 Hz", 11025 },
    { "16000 Hz", 16000 },
    { "22050 Hz", 22050 },
    { "32000 Hz", 32000 },
    { "44100 Hz", 44100 },
    { "48000 Hz", 48000 },
    { "96000 Hz", 96000 },
};

static RVSIntegerRangeValue s_channel_values[] = {
    { "Default", 0 },
    { "Mono",    1 },
    { "Stereo",  2 },
    { "Quad",    4 },
};

static RVSIntegerRangeValue s_volume_ramping_values[] = {
    { "Default",    -1 },
    { "Off",         0 },
    { "Strength 1",  1 },
    { "Strength 2",  2 },
    { "Strength 3",  3 },
    { "Strength 5",  5 },
    { "Strength 10", 10 },
};

static RVSIntegerRangeValue s_interpolation_filter_values[] = {
    { "Default",        0 },
    { "None",           1 },
    { "Linear",         2 },
    { "Cubic",          4 },
    { "Windowed Sinc",  8 },
};

static RVSStringRangeValue s_amiga_filter_values[] = {
    { "Auto",        "auto" },
    { "Unfiltered",  "unfiltered" },
    { "A500 Filter", "a500filter" },
    { "A1200 Filter","a1200filter" },
};

static RVSetting s_settings[] = {
    RVSIntValue_DescRange(ID_SAMPLE_RATE,             "Sample Rate",             "Output sample rate",                                  48000, s_sample_rate_values),
    RVSIntValue_DescRange(ID_CHANNELS,                "Channels",                "Output channel mode (0 = use device default)",        0,     s_channel_values),
    RVSFloatValue_Range(  ID_MASTER_GAIN,             "Master Gain",             "Master gain in percent (100 = 0 dB)",                 100.0f, 0.0f, 400.0f),
    RVSIntValue_Range(    ID_STEREO_SEPARATION,       "Stereo Separation",       "Stereo separation (0 = mono, 100 = normal, 200 = wide)", 100, 0, 200),
    RVSIntValue_DescRange(ID_VOLUME_RAMPING,          "Volume Ramping",          "Volume ramping strength (-1 = default)",               -1,   s_volume_ramping_values),
    RVSIntValue_DescRange(ID_INTERPOLATION_FILTER,    "Interpolation Filter",    "Resampling interpolation filter",                     0,    s_interpolation_filter_values),
    RVSFloatValue_Range(  ID_TEMPO_FACTOR,            "Tempo Factor",            "Playback tempo multiplier (1.0 = normal speed)",      1.0f, 0.01f, 4.0f),
    RVSFloatValue_Range(  ID_PITCH_FACTOR,            "Pitch Factor",            "Playback pitch multiplier (1.0 = normal pitch)",      1.0f, 0.01f, 4.0f),
    RVSBoolValue(         ID_USE_AMIGA_RESAMPLER,     "Amiga Resampling",        "Emulate Amiga hardware resampling",                   false),
    RVSStringValue_DescRange(ID_AMIGA_RESAMPLER_FILTER,"Amiga Resampler Filter", "Amiga resampler filter type",                         "auto", s_amiga_filter_values),
};
// clang-format on

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum class Channels {
    Default,
    Mono,
    Stereo,
    Quad,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct OpenMptData {
    openmpt::module* mod = nullptr;
    std::string ext;
    Channels channels = Channels::Default;
    int sample_rate = 0;
    float length = 0.0f;
    void* song_data = nullptr;
    bool scope_enabled = false;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* openmpt_supported_extensions() {
    // If we have already populated this we can just return
    if (s_supported_extensions[0] != 0) {
        return s_supported_extensions;
    }

    std::vector<std::string> ext_list = openmpt::get_supported_extensions();
    size_t count = ext_list.size();

    for (size_t i = 0; i < count; ++i) {
        strcat(s_supported_extensions, ext_list[i].c_str());
        if (i != count - 1) {
            strcat(s_supported_extensions, ",");
        }
    }

    return s_supported_extensions;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* openmpt_create(const RVService* service_api) {
    (void)service_api;

    OpenMptData* user_data = new OpenMptData;

    return (void*)user_data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int openmpt_destroy(void* user_data) {
    OpenMptData* data = (OpenMptData*)user_data;
    delete data->mod;
    delete data;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult openmpt_probe_can_play(uint8_t* data, uint64_t data_size, const char* filename,
                                            uint64_t total_size) {
    int res = openmpt::probe_file_header(openmpt::probe_file_header_flags_default2, data, data_size, total_size);

    switch (res) {
        case openmpt::probe_file_header_result_success:
            rv_info("OpenMPT: Supported: %s", filename);
            return RVProbeResult_Supported;
        case openmpt::probe_file_header_result_failure:
            rv_debug("OpenMPT: Unsupported: %s", filename);
            return RVProbeResult_Unsupported;
        case openmpt::probe_file_header_result_wantmoredata:
            rv_warn("OpenMPT: Unable to probe - not enough data");
            return RVProbeResult_Unsure;
    }

    rv_warn("OpenMPT: case %d not handled, assuming unsupported", res);
    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int openmpt_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("OpenMPT: Failed to load %s to memory", url);
        return -1;
    }

    OpenMptData* replayer_data = (OpenMptData*)user_data;

    try {
        replayer_data->mod = new openmpt::module(read_res.data, read_res.data_size);
    } catch (...) {
        rv_error("OpenMPT: Exception while loading %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    // Free the loaded data - openmpt makes its own copy
    rv_io_free_url_to_memory(read_res.data);

    replayer_data->length = (float)replayer_data->mod->get_duration_seconds();
    replayer_data->mod->select_subsong(subsong);

    rv_info("OpenMPT: Opened %s (duration: %.2fs)", url, replayer_data->length);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void openmpt_close(void* user_data) {
    OpenMptData* replayer_data = (OpenMptData*)user_data;

    delete replayer_data->mod;
    replayer_data->mod = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo openmpt_read_data(void* user_data, RVReadData dest) {
    OpenMptData* replayer_data = (OpenMptData*)user_data;
    uint32_t sample_rate = dest.info.format.sample_rate;

    // Calculate max frames we can generate
    const int samples_to_generate
        = (int)(dest.channels_output_max_bytes_size / (sizeof(float) * 2)); // stereo interleaved

    // Support overriding the default sample rate
    if (replayer_data->sample_rate != 0) {
        sample_rate = replayer_data->sample_rate;
    }

    uint8_t channel_count = 2;
    uint16_t gen_count = 0;

    switch (replayer_data->channels) {
        default:
        case Channels::Stereo:
        case Channels::Default: {
            gen_count = (uint16_t)replayer_data->mod->read_interleaved_stereo(sample_rate, samples_to_generate,
                                                                              (float*)dest.channels_output);
            break;
        }
        case Channels::Mono: {
            gen_count
                = (uint16_t)replayer_data->mod->read(sample_rate, samples_to_generate, (float*)dest.channels_output);
            channel_count = 1;
            break;
        }
        case Channels::Quad: {
            gen_count = (uint16_t)replayer_data->mod->read_interleaved_quad(sample_rate, samples_to_generate,
                                                                            (float*)dest.channels_output);
            channel_count = 4;
            break;
        }
    }

    RVAudioFormat format = { RVAudioStreamFormat_F32, channel_count, sample_rate };
    RVReadStatus status = (gen_count > 0) ? RVReadStatus_Ok : RVReadStatus_Finished;

    return RVReadInfo { format, gen_count, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t openmpt_seek(void* user_data, int64_t ms) {
    OpenMptData* replayer_data = (OpenMptData*)user_data;

    if (replayer_data->mod) {
        double seconds = (double)ms / 1000.0;
        replayer_data->mod->set_position_seconds(seconds);
        return ms;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* filename_from_path(const char* path) {
    for (size_t i = strlen(path) - 1; i > 0; i--) {
        if (path[i] == '/' || path[i] == '\\') {
            return &path[i + 1];
        }
    }
    return path;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int openmpt_metadata(const char* filename, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(filename)).data == nullptr) {
        rv_error("OpenMPT: Failed to load %s for metadata", filename);
        return -1;
    }

    openmpt::module* mod = nullptr;

    try {
        mod = new openmpt::module(read_res.data, read_res.data_size);
    } catch (...) {
        rv_error("OpenMPT: Failed to open %s for metadata", filename);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    auto index = rv_metadata_create_url(filename);
    char title[512] = { 0 };

    const auto& mod_title = mod->get_metadata("title");

    if (!mod_title.empty()) {
        strncpy(title, mod_title.c_str(), sizeof(title) - 1);
    } else {
        const char* file_title = filename_from_path(filename);
        strncpy(title, file_title, sizeof(title) - 1);
    }

    rv_info("OpenMPT: Updating metadata for %s", filename);

    rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, title);
    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, mod->get_metadata("type_long").c_str());
    rv_metadata_set_tag(index, RV_METADATA_AUTHORINGTOOL_TAG, mod->get_metadata("tracker").c_str());
    rv_metadata_set_tag(index, RV_METADATA_ARTIST_TAG, mod->get_metadata("artist").c_str());
    rv_metadata_set_tag(index, RV_METADATA_DATE_TAG, mod->get_metadata("date").c_str());
    rv_metadata_set_tag(index, RV_METADATA_MESSAGE_TAG, mod->get_metadata("message").c_str());
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, mod->get_duration_seconds());

    for (const auto& sample : mod->get_sample_names()) {
        rv_metadata_add_sample(index, sample.c_str());
    }

    for (const auto& instrument : mod->get_instrument_names()) {
        rv_metadata_add_instrument(index, instrument.c_str());
    }

    const int subsong_count = mod->get_num_subsongs();

    if (subsong_count > 1) {
        int i = 0;
        for (const auto& name : mod->get_subsong_names()) {
            mod->select_subsong(i);
            rv_metadata_add_subsong(index, i, name.c_str(), (float)mod->get_duration_seconds());
            ++i;
        }
    }

    // Cleanup
    delete mod;
    rv_io_free_url_to_memory(read_res.data);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void openmpt_event(void* user_data, uint8_t* data, uint64_t len) {
    OpenMptData* replayer_data = (OpenMptData*)user_data;

    if (replayer_data->mod == nullptr || data == nullptr || len < 8) {
        return;
    }

    int current_pattern = replayer_data->mod->get_current_pattern();
    int current_row = replayer_data->mod->get_current_row();
    int channel_vol_0 = (int)(replayer_data->mod->get_current_channel_vu_mono(0) * 255.0f);
    int channel_vol_1 = (int)(replayer_data->mod->get_current_channel_vu_mono(1) * 255.0f);
    int channel_vol_2 = (int)(replayer_data->mod->get_current_channel_vu_mono(2) * 255.0f);
    int channel_vol_3 = (int)(replayer_data->mod->get_current_channel_vu_mono(3) * 255.0f);

    data[7] = (uint8_t)(current_pattern & 0xFF);
    data[6] = (uint8_t)(current_row & 0xFF);
    data[5] = 0;
    data[4] = 0;
    data[3] = (uint8_t)channel_vol_0;
    data[2] = (uint8_t)channel_vol_1;
    data[1] = (uint8_t)channel_vol_2;
    data[0] = (uint8_t)channel_vol_3;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVSettingsUpdate openmpt_settings_updated(void* user_data, const RVService* service_api) {
    (void)service_api;

    OpenMptData* data = (OpenMptData*)user_data;
    const char* reg_id = "libopenmpt";
    const char* ext = "";

    // Sample rate
    RVSIntResult sr = RVSettings_get_int(g_settings_api, reg_id, ext, ID_SAMPLE_RATE);
    if (sr.result == RVSettingsResult_Ok) {
        data->sample_rate = sr.value;
    }

    // Channels
    RVSIntResult ch = RVSettings_get_int(g_settings_api, reg_id, ext, ID_CHANNELS);
    if (ch.result == RVSettingsResult_Ok) {
        switch (ch.value) {
            case 0:
                data->channels = Channels::Default;
                break;
            case 1:
                data->channels = Channels::Mono;
                break;
            case 2:
                data->channels = Channels::Stereo;
                break;
            case 4:
                data->channels = Channels::Quad;
                break;
            default:
                data->channels = Channels::Default;
                break;
        }
    }

    // Module-specific settings require an active module
    if (data->mod == nullptr) {
        return RVSettingsUpdate_Default;
    }

    // Master gain: convert percentage to millibels (100% = 0 dB = 0 mb)
    RVSFloatResult gain = RVSettings_get_float(g_settings_api, reg_id, ext, ID_MASTER_GAIN);
    if (gain.result == RVSettingsResult_Ok && gain.value > 0.0f) {
        int millibels = (int)(2000.0 * std::log10((double)gain.value / 100.0));
        data->mod->ctl_set_integer("render.mastergain_millibel", millibels);
    }

    // Stereo separation
    RVSIntResult sep = RVSettings_get_int(g_settings_api, reg_id, ext, ID_STEREO_SEPARATION);
    if (sep.result == RVSettingsResult_Ok) {
        data->mod->ctl_set_integer("render.stereo_separation", sep.value);
    }

    // Volume ramping
    RVSIntResult ramp = RVSettings_get_int(g_settings_api, reg_id, ext, ID_VOLUME_RAMPING);
    if (ramp.result == RVSettingsResult_Ok) {
        data->mod->ctl_set_integer("render.volumeramping", ramp.value);
    }

    // Interpolation filter
    RVSIntResult interp = RVSettings_get_int(g_settings_api, reg_id, ext, ID_INTERPOLATION_FILTER);
    if (interp.result == RVSettingsResult_Ok) {
        data->mod->ctl_set_integer("render.interpolationfilter", interp.value);
    }

    // Tempo factor
    RVSFloatResult tempo = RVSettings_get_float(g_settings_api, reg_id, ext, ID_TEMPO_FACTOR);
    if (tempo.result == RVSettingsResult_Ok) {
        data->mod->ctl_set_floatingpoint("play.tempo_factor", (double)tempo.value);
    }

    // Pitch factor
    RVSFloatResult pitch = RVSettings_get_float(g_settings_api, reg_id, ext, ID_PITCH_FACTOR);
    if (pitch.result == RVSettingsResult_Ok) {
        data->mod->ctl_set_floatingpoint("play.pitch_factor", (double)pitch.value);
    }

    // Amiga resampler emulation
    RVSBoolResult amiga = RVSettings_get_bool(g_settings_api, reg_id, ext, ID_USE_AMIGA_RESAMPLER);
    if (amiga.result == RVSettingsResult_Ok) {
        data->mod->ctl_set_boolean("render.resampler.emulate_amiga", amiga.value);
    }

    // Amiga resampler filter type
    RVSStringResult filter = RVSettings_get_string(g_settings_api, reg_id, ext, ID_AMIGA_RESAMPLER_FILTER);
    if (filter.result == RVSettingsResult_Ok && filter.value != nullptr) {
        data->mod->ctl_set_text("render.resampler.emulate_amiga_type", filter.value);
    }

    return RVSettingsUpdate_Default;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int openmpt_get_tracker_info(void* user_data, RVTrackerInfo* info) {
    OpenMptData* data = (OpenMptData*)user_data;
    if (data == nullptr || data->mod == nullptr || info == nullptr) {
        return -1;
    }

    memset(info, 0, sizeof(*info));

    openmpt::module* mod = data->mod;

    info->num_patterns = (uint16_t)mod->get_num_patterns();
    info->num_channels = (uint16_t)mod->get_num_channels();
    info->num_orders = (uint16_t)mod->get_num_orders();
    info->num_samples = (uint16_t)mod->get_num_samples();
    info->current_pattern = (uint16_t)mod->get_current_pattern();
    info->current_row = (uint16_t)mod->get_current_row();
    info->current_order = (uint16_t)mod->get_current_order();
    info->rows_per_pattern = (uint16_t)mod->get_pattern_num_rows(info->current_pattern);

    // Get song name
    std::string name = mod->get_metadata("title");
    strncpy(info->song_name, name.c_str(), sizeof(info->song_name) - 1);
    info->song_name[sizeof(info->song_name) - 1] = '\0';

    // Get module type (e.g., "mod", "xm", "s3m", "it")
    std::string type = mod->get_metadata("type");
    strncpy(info->module_type, type.c_str(), sizeof(info->module_type) - 1);
    info->module_type[sizeof(info->module_type) - 1] = '\0';

    // Get sample names
    std::vector<std::string> sample_names = mod->get_sample_names();
    for (size_t i = 0; i < sample_names.size() && i < 32; i++) {
        strncpy(info->sample_names[i], sample_names[i].c_str(), sizeof(info->sample_names[i]) - 1);
        info->sample_names[i][sizeof(info->sample_names[i]) - 1] = '\0';
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int openmpt_get_pattern_cell(void* user_data, int pattern, int row, int channel, RVPatternCell* cell) {
    OpenMptData* data = (OpenMptData*)user_data;
    if (data == nullptr || data->mod == nullptr || cell == nullptr) {
        return -1;
    }

    openmpt::module* mod = data->mod;

    // Validate indices
    if (pattern < 0 || pattern >= mod->get_num_patterns()) {
        return -1;
    }
    if (row < 0 || row >= mod->get_pattern_num_rows(pattern)) {
        return -1;
    }
    if (channel < 0 || channel >= mod->get_num_channels()) {
        return -1;
    }

    // Get cell data using libopenmpt API
    cell->note
        = mod->get_pattern_row_channel_command(pattern, row, channel, openmpt::module::command_index::command_note);
    cell->instrument = mod->get_pattern_row_channel_command(pattern, row, channel,
                                                            openmpt::module::command_index::command_instrument);
    cell->volume
        = mod->get_pattern_row_channel_command(pattern, row, channel, openmpt::module::command_index::command_volume);
    // Use format_pattern_row_channel_command for effect to get the correct format-specific letter
    std::string effect_str = mod->format_pattern_row_channel_command(pattern, row, channel,
                                                                     openmpt::module::command_index::command_effect);
    cell->effect = (!effect_str.empty() && effect_str[0] != '.') ? (uint8_t)effect_str[0] : 0;
    cell->effect_param = mod->get_pattern_row_channel_command(pattern, row, channel,
                                                              openmpt::module::command_index::command_parameter);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int openmpt_get_pattern_num_rows(void* user_data, int pattern) {
    OpenMptData* data = (OpenMptData*)user_data;
    if (data == nullptr || data->mod == nullptr) {
        return 0;
    }

    openmpt::module* mod = data->mod;

    if (pattern < 0 || pattern >= mod->get_num_patterns()) {
        return 0;
    }

    return mod->get_pattern_num_rows(pattern);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Scope/Waveform visualization API

static uint32_t openmpt_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    OpenMptData* data = (OpenMptData*)user_data;
    if (data == nullptr || data->mod == nullptr || buffer == nullptr) {
        return 0;
    }

    // Auto-enable scope capture on first call
    if (!data->scope_enabled) {
        data->mod->enable_scope_capture(true);
        data->scope_enabled = true;
    }

    return static_cast<uint32_t>(data->mod->get_channel_scope_data(channel, buffer, num_samples));
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void openmpt_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
    g_settings_api = RVService_get_settings(service_api, RV_SETTINGS_API_VERSION);

    RVSettings_register_array(g_settings_api, "libopenmpt", s_settings);

    rv_info("OpenMPT plugin initialized (libopenmpt %s)", openmpt::string::get("library_version").c_str());
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t openmpt_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    OpenMptData* data = static_cast<OpenMptData*>(user_data);
    if (data == nullptr || data->mod == nullptr)
        return 0;

    static char s_name_bufs[64][16];
    uint32_t count = (uint32_t)data->mod->get_num_channels();
    if (count > 64)
        count = 64;
    if (count > max_channels)
        count = max_channels;
    for (uint32_t i = 0; i < count; i++) {
        snprintf(s_name_bufs[i], sizeof(s_name_bufs[i]), "Ch %u", i + 1);
        names[i] = s_name_bufs[i];
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin s_openmpt_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "libopenmpt",
    "0.1.0",
    "libopenmpt 0.8.x",
    openmpt_probe_can_play,
    openmpt_supported_extensions,
    openmpt_create,
    openmpt_destroy,
    openmpt_event,
    openmpt_open,
    openmpt_close,
    openmpt_read_data,
    openmpt_seek,
    openmpt_metadata,
    openmpt_static_init,
    openmpt_settings_updated,
    // Tracker visualization API
    openmpt_get_tracker_info,
    openmpt_get_pattern_cell,
    openmpt_get_pattern_num_rows,
    // Scope/Waveform visualization API
    openmpt_get_scope_data,
    nullptr, // static_destroy
    openmpt_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Plugin entry point

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin() {
    return &s_openmpt_plugin;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
