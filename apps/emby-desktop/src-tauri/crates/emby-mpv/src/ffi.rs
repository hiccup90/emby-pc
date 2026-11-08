//! Raw libmpv C API mirror.
//!
//! Every declaration in this file was transcribed from `mpv/client.h`
//! (`MPV_CLIENT_API_VERSION == MPV_MAKE_VERSION(2, 6)`, i.e. `(2 << 16) | 6`).
//! Do not add/changing declarations "from memory": re-check the header first.
//!
//! The libmpv ABI is documented as stable for a major API version, so we bind
//! dynamically and check `mpv_client_api_version() >> 16 == 2` at load time.

use core::ffi::{c_char, c_int, c_ulong, c_void};

/// Opaque `mpv_handle *`.
pub type MpvHandle = c_void;

/// `mpv_format` (C enum passed as `int`).
pub type MpvFormat = c_int;
/// `mpv_event_id` (C enum passed as `int`).
pub type MpvEventId = c_int;

// ---- enum mpv_format (client.h:630) -------------------------------------------------
pub const MPV_FORMAT_NONE: MpvFormat = 0;
pub const MPV_FORMAT_STRING: MpvFormat = 1;
pub const MPV_FORMAT_OSD_STRING: MpvFormat = 2;
pub const MPV_FORMAT_FLAG: MpvFormat = 3;
pub const MPV_FORMAT_INT64: MpvFormat = 4;
pub const MPV_FORMAT_DOUBLE: MpvFormat = 5;
pub const MPV_FORMAT_NODE: MpvFormat = 6;
pub const MPV_FORMAT_NODE_ARRAY: MpvFormat = 7;
pub const MPV_FORMAT_NODE_MAP: MpvFormat = 8;
pub const MPV_FORMAT_BYTE_ARRAY: MpvFormat = 9;

// ---- enum mpv_event_id (client.h:1246) ---------------------------------------------
pub const MPV_EVENT_NONE: MpvEventId = 0;
pub const MPV_EVENT_SHUTDOWN: MpvEventId = 1;
pub const MPV_EVENT_LOG_MESSAGE: MpvEventId = 2;
pub const MPV_EVENT_GET_PROPERTY_REPLY: MpvEventId = 3;
pub const MPV_EVENT_SET_PROPERTY_REPLY: MpvEventId = 4;
pub const MPV_EVENT_COMMAND_REPLY: MpvEventId = 5;
pub const MPV_EVENT_START_FILE: MpvEventId = 6;
pub const MPV_EVENT_END_FILE: MpvEventId = 7;
pub const MPV_EVENT_FILE_LOADED: MpvEventId = 8;
pub const MPV_EVENT_CLIENT_MESSAGE: MpvEventId = 16;
pub const MPV_EVENT_VIDEO_RECONFIG: MpvEventId = 17;
pub const MPV_EVENT_AUDIO_RECONFIG: MpvEventId = 18;
pub const MPV_EVENT_SEEK: MpvEventId = 20;
pub const MPV_EVENT_PLAYBACK_RESTART: MpvEventId = 21;
pub const MPV_EVENT_PROPERTY_CHANGE: MpvEventId = 22;
pub const MPV_EVENT_QUEUE_OVERFLOW: MpvEventId = 24;
pub const MPV_EVENT_HOOK: MpvEventId = 25;

// ---- enum mpv_end_file_reason (client.h:1465) --------------------------------------
pub const MPV_END_FILE_REASON_EOF: c_int = 0;
pub const MPV_END_FILE_REASON_STOP: c_int = 2;
pub const MPV_END_FILE_REASON_QUIT: c_int = 3;
pub const MPV_END_FILE_REASON_ERROR: c_int = 4;
pub const MPV_END_FILE_REASON_REDIRECT: c_int = 5;

/// Major API version this crate is written against (`(2 << 16) | 6`).
pub const EXPECTED_API_MAJOR: u32 = 2;

/// `MPV_CLIENT_API_VERSION` the header we transcribed from declares.
pub const EXPECTED_CLIENT_API_VERSION: u64 = (2 << 16) | 6;

// ---- repr(C) event structs (client.h:1382, 1508, 1585) -----------------------------

/// `mpv_event_property`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MpvEventProperty {
    pub name: *const c_char,
    pub format: MpvFormat,
    pub data: *mut c_void,
}

/// `mpv_event_end_file`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MpvEventEndFile {
    pub reason: c_int,
    pub error: c_int,
    pub playlist_entry_id: i64,
    pub playlist_insert_id: i64,
    pub playlist_insert_num_entries: c_int,
}

/// `mpv_event_log_message`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MpvEventLogMessage {
    pub prefix: *const c_char,
    pub level: *const c_char,
    pub text: *const c_char,
    pub log_level: c_int,
}

/// `mpv_event`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MpvEvent {
    pub event_id: MpvEventId,
    pub error: c_int,
    pub reply_userdata: u64,
    pub data: *mut c_void,
}

// ---- function pointer types (client.h signatures) ----------------------------------

pub type MpvCreateFn = unsafe extern "C" fn() -> *mut MpvHandle;
pub type MpvInitializeFn = unsafe extern "C" fn(ctx: *mut MpvHandle) -> c_int;
pub type MpvSetOptionStringFn =
    unsafe extern "C" fn(ctx: *mut MpvHandle, name: *const c_char, data: *const c_char) -> c_int;
pub type MpvSetPropertyStringFn =
    unsafe extern "C" fn(ctx: *mut MpvHandle, name: *const c_char, data: *const c_char) -> c_int;
pub type MpvGetPropertyStringFn =
    unsafe extern "C" fn(ctx: *mut MpvHandle, name: *const c_char) -> *mut c_char;
pub type MpvFreeFn = unsafe extern "C" fn(data: *mut c_void);
pub type MpvCommandFn = unsafe extern "C" fn(ctx: *mut MpvHandle, args: *const *const c_char) -> c_int;
pub type MpvObservePropertyFn =
    unsafe extern "C" fn(ctx: *mut MpvHandle, reply_userdata: u64, name: *const c_char, format: MpvFormat) -> c_int;
pub type MpvWaitEventFn = unsafe extern "C" fn(ctx: *mut MpvHandle, timeout: f64) -> *mut MpvEvent;
pub type MpvTerminateDestroyFn = unsafe extern "C" fn(ctx: *mut MpvHandle);
pub type MpvErrorStringFn = unsafe extern "C" fn(error: c_int) -> *const c_char;
pub type MpvClientApiVersionFn = unsafe extern "C" fn() -> c_ulong;
pub type MpvRequestLogMessagesFn = unsafe extern "C" fn(ctx: *mut MpvHandle, min_level: *const c_char) -> c_int;
pub type MpvWakeupFn = unsafe extern "C" fn(ctx: *mut MpvHandle);

/// The subset of libmpv symbols this player needs.
///
/// `mpv_render_context_*` is deliberately absent: we embed with `--wid` and let
/// libmpv own its D3D11 swapchain.
pub struct MpvApi {
    pub create: MpvCreateFn,
    pub initialize: MpvInitializeFn,
    pub set_option_string: MpvSetOptionStringFn,
    pub set_property_string: MpvSetPropertyStringFn,
    pub get_property_string: MpvGetPropertyStringFn,
    pub free: MpvFreeFn,
    pub command: MpvCommandFn,
    pub observe_property: MpvObservePropertyFn,
    pub wait_event: MpvWaitEventFn,
    pub terminate_destroy: MpvTerminateDestroyFn,
    pub error_string: MpvErrorStringFn,
    pub client_api_version: MpvClientApiVersionFn,
    /// Optional: absent in very old builds only.
    pub request_log_messages: Option<MpvRequestLogMessagesFn>,
    /// Optional: only used to interrupt `mpv_wait_event` early.
    pub wakeup: Option<MpvWakeupFn>,
}

impl MpvApi {
    /// Symbol names in load order, used by the loader for required symbols.
    pub const REQUIRED_SYMBOLS: &'static [&'static str] = &[
        "mpv_create",
        "mpv_initialize",
        "mpv_set_option_string",
        "mpv_set_property_string",
        "mpv_get_property_string",
        "mpv_free",
        "mpv_command",
        "mpv_observe_property",
        "mpv_wait_event",
        "mpv_terminate_destroy",
        "mpv_error_string",
        "mpv_client_api_version",
    ];

    pub const OPTIONAL_SYMBOLS: &'static [&'static str] =
        &["mpv_request_log_messages", "mpv_wakeup"];
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::mem::{align_of, offset_of, size_of};

    #[test]
    fn enum_values_match_header() {
        // client.h enum mpv_format
        assert_eq!(
            [
                MPV_FORMAT_NONE,
                MPV_FORMAT_STRING,
                MPV_FORMAT_OSD_STRING,
                MPV_FORMAT_FLAG,
                MPV_FORMAT_INT64,
                MPV_FORMAT_DOUBLE,
                MPV_FORMAT_NODE,
                MPV_FORMAT_NODE_ARRAY,
                MPV_FORMAT_NODE_MAP,
                MPV_FORMAT_BYTE_ARRAY,
            ],
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
        );
        // client.h enum mpv_event_id
        assert_eq!(
            [
                MPV_EVENT_NONE,
                MPV_EVENT_SHUTDOWN,
                MPV_EVENT_LOG_MESSAGE,
                MPV_EVENT_END_FILE,
                MPV_EVENT_FILE_LOADED,
                MPV_EVENT_SEEK,
                MPV_EVENT_PLAYBACK_RESTART,
                MPV_EVENT_PROPERTY_CHANGE,
                MPV_EVENT_QUEUE_OVERFLOW,
            ],
            [0, 1, 2, 7, 8, 20, 21, 22, 24]
        );
        // client.h enum mpv_end_file_reason
        assert_eq!(
            [
                MPV_END_FILE_REASON_EOF,
                MPV_END_FILE_REASON_STOP,
                MPV_END_FILE_REASON_QUIT,
                MPV_END_FILE_REASON_ERROR,
                MPV_END_FILE_REASON_REDIRECT,
            ],
            [0, 2, 3, 4, 5]
        );
        // MPV_MAKE_VERSION(2, 6)
        assert_eq!(EXPECTED_CLIENT_API_VERSION, (2 << 16) | 6);
        assert_eq!(EXPECTED_CLIENT_API_VERSION >> 16, 2);
        assert_eq!(EXPECTED_API_MAJOR, 2);
    }

    // Layout assertions are for 64-bit targets (the only Windows targets this
    // app ships). Sizes/offsets follow the C struct definitions above; if a
    // struct is edited, these tests fail instead of corrupting mpv's heap.
    #[test]
    #[cfg(target_pointer_width = "64")]
    fn event_struct_layout_is_stable() {
        // struct mpv_event { int event_id; int error; uint64_t reply_userdata; void *data; }
        assert_eq!(size_of::<MpvEvent>(), 24);
        assert_eq!(align_of::<MpvEvent>(), 8);
        assert_eq!(offset_of!(MpvEvent, event_id), 0);
        assert_eq!(offset_of!(MpvEvent, error), 4);
        assert_eq!(offset_of!(MpvEvent, reply_userdata), 8);
        assert_eq!(offset_of!(MpvEvent, data), 16);

        // struct mpv_event_property { const char *name; mpv_format format; void *data; }
        assert_eq!(size_of::<MpvEventProperty>(), 24);
        assert_eq!(offset_of!(MpvEventProperty, name), 0);
        assert_eq!(offset_of!(MpvEventProperty, format), 8);
        assert_eq!(offset_of!(MpvEventProperty, data), 16);

        // struct mpv_event_end_file { int reason; int error; int64_t ...; int64_t ...; int ...; }
        assert_eq!(size_of::<MpvEventEndFile>(), 32);
        assert_eq!(offset_of!(MpvEventEndFile, reason), 0);
        assert_eq!(offset_of!(MpvEventEndFile, error), 4);
        assert_eq!(offset_of!(MpvEventEndFile, playlist_entry_id), 8);
        assert_eq!(offset_of!(MpvEventEndFile, playlist_insert_id), 16);
        assert_eq!(offset_of!(MpvEventEndFile, playlist_insert_num_entries), 24);

        // struct mpv_event_log_message { const char *prefix; const char *level; const char *text; mpv_log_level log_level; }
        assert_eq!(size_of::<MpvEventLogMessage>(), 32);
        assert_eq!(offset_of!(MpvEventLogMessage, prefix), 0);
        assert_eq!(offset_of!(MpvEventLogMessage, level), 8);
        assert_eq!(offset_of!(MpvEventLogMessage, text), 16);
        assert_eq!(offset_of!(MpvEventLogMessage, log_level), 24);
    }

    #[test]
    fn required_symbol_list_covers_every_used_entry_point() {
        // Guard against adding a call to `MpvApi` without loading its symbol.
        for name in [
            "mpv_create",
            "mpv_initialize",
            "mpv_set_option_string",
            "mpv_set_property_string",
            "mpv_get_property_string",
            "mpv_free",
            "mpv_command",
            "mpv_observe_property",
            "mpv_wait_event",
            "mpv_terminate_destroy",
            "mpv_error_string",
            "mpv_client_api_version",
        ] {
            assert!(MpvApi::REQUIRED_SYMBOLS.contains(&name), "missing {name}");
        }
        assert!(!MpvApi::REQUIRED_SYMBOLS.contains(&"mpv_render_context_create"));
    }
}
