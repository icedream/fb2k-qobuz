// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Carl Kittelberger <icedream@icedream.pw>

#include "stdafx.h"
#include "qobuz_api.h"

// ---- Qobuz input_singletrack_impl ------------------------------------------
//
// Handles qobuz://track/TRACK_ID URIs.
//
// open()              — fetches track metadata from the API (track/get)
// get_info()          — exposes that metadata to foobar2000 (playlist display,
//                       Converter tagging, Properties dialog, …)
// decode_initialize() — resolves the HTTPS stream URL and opens an inner
//                       decoder (built-in FLAC/AAC input) on it
// decode_run() etc.   — delegate to the inner decoder
//
// Because the inner decoder is opened on an HTTPS CDN URL there is no recursion.

// ---- URL helpers -----------------------------------------------------------

static constexpr const char* k_qobuz_uri_prefix  = "qobuz://track/";
static constexpr const char* k_qobuz_web_prefix   = "https://open.qobuz.com/track/";

static bool is_qobuz_track_url(const char* path) {
    return pfc::string_has_prefix(path, k_qobuz_uri_prefix) ||
           pfc::string_has_prefix(path, k_qobuz_web_prefix);
}

// Extract the numeric track ID from either URL form.
// Strips any trailing query string (?) or fragment (#).
static pfc::string8 extract_track_id(const char* path) {
    const char* prefix = nullptr;
    if (pfc::string_has_prefix(path, k_qobuz_uri_prefix))
        prefix = k_qobuz_uri_prefix;
    else if (pfc::string_has_prefix(path, k_qobuz_web_prefix))
        prefix = k_qobuz_web_prefix;
    else
        throw exception_io_data("Unrecognised Qobuz track URL");

    const char* id_start = path + std::strlen(prefix);
    // Find end of ID: stop at '/', '?', '#', '.', or '\0'
    const char* id_end = id_start;
    while (*id_end && *id_end != '/' && *id_end != '?' && *id_end != '#' && *id_end != '.')
        ++id_end;
    if (id_end == id_start)
        throw exception_io_data("Empty track ID in Qobuz URL");
    return pfc::string8(id_start, id_end - id_start);
}

// ---- format_id to extension mapping ----------------------------------------

static const char* format_id_to_ext(int format_id) {
    switch (format_id) {
        case 27: // Studio Master FLAC
        case 7:  // Hi-Res FLAC
        case 6:  // CD Quality FLAC
            return "flac";
        case 5:  // MP3 320 kbps
            return "mp3";
        default:
            return nullptr;  // unknown
    }
}

// ---- helper to extract fmt parameter from query string ----------------------

static int extract_format_id_from_url(const char* url) {
    // Find fmt= in the query string
    const char* fmt_pos = std::strstr(url, "fmt=");
    if (!fmt_pos) return 0;  // not found
    
    // Parse the integer after fmt=
    int val = 0;
    int n = std::sscanf(fmt_pos, "fmt=%d", &val);
    return (n == 1) ? val : 0;
}

class qobuz_input_impl : public input_stubs {
public:
    // ---- static entry-point methods -----------------------------------------

    static bool g_is_our_path(const char* p_path, const char* /*p_ext*/) {
        // Claim qobuz:// URIs so input service is always called for playback/metadata
        return is_qobuz_track_url(p_path);
    }

    static bool g_is_our_content_type(const char* /*p_type*/) { return false; }

    static bool g_is_low_merit() { return false; }

    static GUID g_get_guid() {
        // {B3C14A7E-2D5F-4E8B-A631-9C4D7B3E5F12}
        static constexpr GUID guid =
            { 0xb3c14a7e, 0x2d5f, 0x4e8b, { 0xa6, 0x31, 0x9c, 0x4d, 0x7b, 0x3e, 0x5f, 0x12 } };
        return guid;
    }

    static const char* g_get_name() { return "Qobuz streaming input"; }

    // ---- open ---------------------------------------------------------------

    void open(service_ptr_t<file> p_filehint, const char* p_path,
              t_input_open_reason p_reason, abort_callback& p_abort)
    {
        m_path     = p_path;
        m_track_id = extract_track_id(p_path);

        // If the filesystem service already opened the HTTP stream, save it for
        // decode_initialize() to reuse. For info_read we don't need it.
        m_file = p_filehint;

        // Fetch metadata and stream URL in one shot, so both are available
        // for get_info() (which may be called for Converter file naming).
        try {
            m_track = g_qobuz_api.get_track_info(m_track_id.c_str(), p_abort);
        } catch (...) {}

        // Resolve stream URL and extract extension early so get_info() can use it.
        try {
            int format_id = (int)cfg_quality().get();
            m_stream_url = g_qobuz_api.get_track_url(m_track_id.c_str(), format_id, p_abort);

            // Extract fmt parameter from the query string and map to file extension.
            int fmt_from_url = extract_format_id_from_url(m_stream_url.c_str());
            const char* ext_cstr = format_id_to_ext(fmt_from_url);
            if (ext_cstr) {
                m_stream_ext = ext_cstr;
            } else {
                m_stream_ext = format_id >= 27 ? "flac" : (format_id == 5 ? "mp3" : "flac");
            }
        } catch (...) {}
    }

    // ---- file stats ---------------------------------------------------------

    t_filestats2 get_stats2(uint32_t /*flags*/, abort_callback& /*p_abort*/) {
        return filestats2_invalid;
    }

    // ---- metadata -----------------------------------------------------------

    void get_info(file_info& p_info, abort_callback& p_abort) {
        QobuzTrack t = m_track;
        FB2K_console_formatter() << "[qobuz] get_info() track_id='" << m_track_id.c_str()
            << "' title='" << t.title.c_str() << "'";

        // If metadata wasn't fetched in open() (shouldn't happen, but be safe),
        // try on-demand using the saved path.
        if (t.id.empty() && !m_track_id.empty()) {
            try {
                t = g_qobuz_api.get_track_info(m_track_id.c_str(), p_abort);
                m_track = t;
            } catch (...) {}
        }

        if (!t.title.empty())        p_info.meta_add("TITLE",        t.title.c_str());
        if (!t.artist.empty())       p_info.meta_add("ARTIST",       t.artist.c_str());
        if (!t.album_artist.empty()) p_info.meta_add("ALBUM ARTIST", t.album_artist.c_str());
        if (!t.album.empty())        p_info.meta_add("ALBUM",        t.album.c_str());
        if (t.track_number > 0)
            p_info.meta_add("TRACKNUMBER", std::to_string(t.track_number).c_str());
        if (t.total_tracks > 0)
            p_info.meta_add("TOTALTRACKS", std::to_string(t.total_tracks).c_str());
        if (t.disc_number > 0)
            p_info.meta_add("DISCNUMBER",  std::to_string(t.disc_number).c_str());
        if (t.total_discs > 1)
            p_info.meta_add("TOTALDISCS",  std::to_string(t.total_discs).c_str());
        if (!t.date.empty())       p_info.meta_add("DATE",        t.date.c_str());
        if (!t.genre.empty())      p_info.meta_add("GENRE",       t.genre.c_str());
        if (!t.composer.empty())   p_info.meta_add("COMPOSER",    t.composer.c_str());
        if (!t.label.empty())      p_info.meta_add("LABEL",       t.label.c_str());
        if (!t.isrc.empty())       p_info.meta_add("ISRC",        t.isrc.c_str());
        if (!t.copyright.empty())  p_info.meta_add("COPYRIGHT",   t.copyright.c_str());
        if (!t.upc.empty())        p_info.meta_add("UPC",         t.upc.c_str());
        if (!t.performers.empty()) p_info.meta_add("PERFORMERS",  t.performers.c_str());

        if (t.has_rg) {
            pfc::string8 gain_str, peak_str;
            gain_str << t.rg_track_gain << " dB";
            peak_str << t.rg_track_peak;
            p_info.meta_add("REPLAYGAIN_TRACK_GAIN", gain_str);
            p_info.meta_add("REPLAYGAIN_TRACK_PEAK", peak_str);
        }

        // Technical info
        p_info.set_length(t.duration);
        if (t.sampling_rate > 0)
            p_info.info_set_int("samplerate", (t_int64)(t.sampling_rate * 1000.0 + 0.5));
        if (t.bit_depth > 0)
            p_info.info_set_int("bitspersample", t.bit_depth);
        if (t.channels > 0)
            p_info.info_set_int("channels", t.channels);
        p_info.info_set("codec",    (int)cfg_quality().get() >= 27 ? "FLAC" : "AAC");
        p_info.info_set("encoding", "lossless");
        // Set file extension so Converter uses the right output filename
        if (!m_stream_ext.is_empty())
            p_info.info_set("extension", m_stream_ext.c_str());
        else
            p_info.info_set("extension", (int)cfg_quality().get() >= 27 ? "flac" : "m4a");
    }

    // ---- decoding -----------------------------------------------------------

    void decode_initialize(unsigned p_flags, abort_callback& p_abort) {
        // Use the cached stream URL (already resolved in open()).
        // If for some reason it's empty, we can still try to re-fetch it.
        pfc::string8 stream_url = m_stream_url;
        if (stream_url.is_empty()) {
            int format_id = (int)cfg_quality().get();
            stream_url = g_qobuz_api.get_track_url(m_track_id.c_str(), format_id, p_abort);
            FB2K_console_formatter() << "[qobuz] decode_initialize() had to fetch stream_url (cache was empty)";
        } else {
            FB2K_console_formatter() << "[qobuz] decode_initialize() using cached stream_url";
        }

        FB2K_console_formatter() << "[qobuz] decode_initialize() stream_url=" << stream_url.c_str()
            << " ext=" << (m_stream_ext.is_empty() ? "(empty)" : m_stream_ext.c_str());

        // Always open a fresh HTTP connection for the inner decoder.
        // The m_file hint (from filesystem service proxy) may be non-seekable
        // or stale; a fresh open is simpler and more reliable.
        input_entry::g_open_for_decoding(m_inner, nullptr, stream_url, p_abort);
        m_inner->initialize(0, p_flags, p_abort);
    }

    bool decode_run(audio_chunk& p_chunk, abort_callback& p_abort) {
        return m_inner->run(p_chunk, p_abort);
    }

    void decode_seek(double p_seconds, abort_callback& p_abort) {
        m_inner->seek(p_seconds, p_abort);
    }

    bool decode_can_seek() {
        return m_inner.is_valid() && m_inner->can_seek();
    }

    bool decode_get_dynamic_info(file_info& p_out, double& p_timestamp_delta) {
        return m_inner.is_valid() &&
               m_inner->get_dynamic_info(p_out, p_timestamp_delta);
    }

    bool decode_get_dynamic_info_track(file_info& p_out, double& p_timestamp_delta) {
        return m_inner.is_valid() &&
               m_inner->get_dynamic_info_track(p_out, p_timestamp_delta);
    }

    void decode_on_idle(abort_callback& p_abort) {
        if (m_inner.is_valid()) m_inner->on_idle(p_abort);
    }

    // ---- tag writing (not supported for streaming tracks) -------------------

    void retag(const file_info& /*p_info*/, abort_callback& /*p_abort*/) {
        throw exception_tagging_unsupported();
    }

    void remove_tags(abort_callback& /*p_abort*/) {
        throw exception_tagging_unsupported();
    }

private:
    pfc::string8                  m_path;
    pfc::string8                  m_track_id;
    pfc::string8                  m_stream_url;  // cached CDN URL (resolved in open)
    pfc::string8                  m_stream_ext;  // extension parsed from CDN URL (flac, mp4, …)
    QobuzTrack                    m_track;
    service_ptr_t<file>           m_file;   // pre-opened HTTP stream from filesystem service
    service_ptr_t<input_decoder>  m_inner;
};

static input_singletrack_factory_t<qobuz_input_impl> g_qobuz_input_factory;

// ---- Album art extractor for qobuz://track/ URIs ----------------------------

class qobuz_album_art_extractor : public album_art_extractor {
public:
    bool is_our_path(const char* p_path, const char* /*p_extension*/) override {
        return is_qobuz_track_url(p_path);
    }

    album_art_extractor_instance_ptr open(file_ptr /*p_filehint*/,
                                          const char* p_path,
                                          abort_callback& p_abort) override
    {
        pfc::string8 track_id = extract_track_id(p_path);
        QobuzTrack track = g_qobuz_api.get_track_info(track_id.c_str(), p_abort);

        if (track.cover_url.empty())
            throw exception_album_art_not_found();

        std::string jpeg = g_qobuz_api.download_url(track.cover_url.c_str());
        if (jpeg.empty())
            throw exception_album_art_not_found();

        album_art_data_ptr art = album_art_data_impl::g_create(jpeg.data(), jpeg.size());
        auto inst = fb2k::service_new<album_art_extractor_instance_simple>();
        inst->set(album_art_ids::cover_front, art);
        return inst;
    }
};

static service_factory_single_t<qobuz_album_art_extractor> g_qobuz_art_extractor;

