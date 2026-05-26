// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Carl Kittelberger <icedream@icedream.pw>

#include "stdafx.h"
#include "qobuz_api.h"

// ---- qobuz:// filesystem provider -----------------------------------------------
//
// Handles URIs of the form:  qobuz://track/TRACK_ID
//
// Registers as a filesystem service so the Converter validates qobuz:// as a
// valid source path.  When foobar2000 opens the file it gets back a proxy that
// wraps the real CDN HTTPS stream but explicitly hides its MIME type.
//
// Why hide the MIME type?
//   If the returned file advertises "audio/flac" (as the raw HTTP stream does),
//   foobar2000 immediately hands it to the built-in FLAC decoder and bypasses
//   our input service entirely — metadata is lost and our open() is never called.
//   By returning false from get_content_type() we force foobar2000 to fall back
//   to path-based input selection, which correctly finds our qobuz_input_impl
//   via g_is_our_path("qobuz://track/…").
//
// The input service (qobuz_input_impl) then handles:
//   – metadata (get_info via Qobuz API)
//   – decoding (opens a fresh CDN stream itself in decode_initialize)

namespace {

// ---- content-type-hiding file proxy -----------------------------------------

class qobuz_file_proxy : public file {
public:
    explicit qobuz_file_proxy(file::ptr inner) : m_inner(inner) {}

    // ---- stream_reader ----
    t_size read(void* p_buffer, t_size p_bytes, abort_callback& p_abort) override {
        return m_inner->read(p_buffer, p_bytes, p_abort);
    }

    // ---- stream_writer ----
    void write(const void* p_buffer, t_size p_bytes, abort_callback& p_abort) override {
        m_inner->write(p_buffer, p_bytes, p_abort);
    }

    // ---- file ----
    t_filesize get_size(abort_callback& p_abort) override {
        return m_inner->get_size(p_abort);
    }
    t_filesize get_position(abort_callback& p_abort) override {
        return m_inner->get_position(p_abort);
    }
    void resize(t_filesize p_size, abort_callback& p_abort) override {
        m_inner->resize(p_size, p_abort);
    }
    void seek(t_filesize p_position, abort_callback& p_abort) override {
        m_inner->seek(p_position, p_abort);
    }
    void seek_ex(t_sfilesize p_position, t_seek_mode p_mode, abort_callback& p_abort) override {
        m_inner->seek_ex(p_position, p_mode, p_abort);
    }
    bool can_seek() override { return m_inner->can_seek(); }

    // KEY: hide the content type so foobar2000 does path-based input lookup
    // instead of content-type lookup.  Without this it uses the built-in FLAC
    // decoder directly and our qobuz_input_impl::open() is never called.
    bool get_content_type(pfc::string_base& /*p_out*/) override { return false; }

    bool is_in_memory() override { return m_inner->is_in_memory(); }
    void on_idle(abort_callback& p_abort) override { m_inner->on_idle(p_abort); }
    t_filetimestamp get_timestamp(abort_callback& p_abort) override {
        return m_inner->get_timestamp(p_abort);
    }
    void reopen(abort_callback& p_abort) override { m_inner->reopen(p_abort); }
    bool is_remote() override { return m_inner->is_remote(); }

private:
    file::ptr m_inner;
};

// ---- path helpers -----------------------------------------------------------

static pfc::string8 parse_track_id(const char* path) {
    const char* prefix      = "qobuz://track/";
    const size_t prefix_len = std::strlen(prefix);
    if (std::strncmp(path, prefix, prefix_len) != 0)
        throw exception_io_data("Invalid qobuz:// URI");
    const char* id_start = path + prefix_len;
    // Stop at extension (.flac/.mp3 appended by get_canonical_path), query-string or fragment
    const char* id_end = id_start;
    while (*id_end && *id_end != '.' && *id_end != '?' && *id_end != '#' && *id_end != '/')
        ++id_end;
    if (id_end == id_start)
        throw exception_io_data("Empty track ID in Qobuz URL");
    return pfc::string8(id_start, id_end - id_start);
}

// ---- filesystem service -----------------------------------------------------

class qobuz_filesystem_impl : public filesystem {
public:
    bool get_canonical_path(const char* path, pfc::string_base& out) override {
        // Append the file extension to the qobuz:// URI so foobar2000 can derive
        // %ext% from the path.  This is needed for the Converter's Copy mode to
        // produce correctly-named output files.  If the path already has a
        // recognised audio extension, leave it unchanged.
        const char* existing_ext = pfc::string_extension(path);
        if (pfc::string_has_prefix(existing_ext, "flac") ||
            pfc::string_has_prefix(existing_ext, "mp3")) {
            out = path;
            return true;
        }
        // Determine extension from the configured quality format_id.
        int format_id = (int)cfg_quality().get();
        const char* ext = (format_id == 5) ? "mp3" : "flac";
        out = pfc::string8(path) + "." + ext;
        return true;
    }

    bool is_our_path(const char* path) override {
        return pfc::string_has_prefix(path, "qobuz://track/");
    }

    bool get_display_path(const char* path, pfc::string_base& out) override {
        out = path;
        return true;
    }

    bool is_remote(const char*) override { return true; }

    bool supports_content_types() override { return false; }

    void open(file::ptr& p_out, const char* path, t_open_mode mode,
              abort_callback& abort) override {
        if (mode != open_mode_read)
            throw exception_io_denied();

        auto track_id  = parse_track_id(path);
        int  format_id = (int)cfg_quality().get();

        // Resolve to HTTPS CDN stream URL via Qobuz API
        pfc::string8 stream_url = g_qobuz_api.get_track_url(track_id.c_str(), format_id, abort);

        // Open the raw CDN HTTP stream
        file::ptr inner;
        filesystem::g_open(inner, stream_url, open_mode_read, abort);

        // Wrap in proxy that hides the MIME type.
        // This is crucial: if we return the raw HTTP file its get_content_type()
        // returns "audio/flac" and foobar2000 routes it straight to the FLAC
        // decoder, skipping our qobuz_input_impl entirely (no metadata, our
        // open() never called).  The proxy returns false from get_content_type()
        // so foobar2000 falls back to path-based input selection.
        p_out = fb2k::service_new<qobuz_file_proxy>(inner);
    }

    void get_stats(const char*, t_filestats& stats, bool& writable,
                   abort_callback&) override {
        stats.m_size      = filesize_invalid;
        stats.m_timestamp = filetimestamp_invalid;
        writable          = false;
    }

    void remove(const char*, abort_callback&) override { throw exception_io_denied(); }
    void move(const char*, const char*, abort_callback&) override { throw exception_io_denied(); }
    void create_directory(const char*, abort_callback&) override { throw exception_io_denied(); }
    void list_directory(const char*, directory_callback&, abort_callback&) override {
        throw exception_io_denied();
    }
};

static service_factory_single_t<qobuz_filesystem_impl> g_qobuz_filesystem_factory;

}  // anonymous namespace
