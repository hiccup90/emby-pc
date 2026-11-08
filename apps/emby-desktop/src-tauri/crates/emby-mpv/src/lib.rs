//! libmpv embedding kernel for Emby Desktop.
//!
//! Responsibilities kept deliberately Tauri-free so they can be unit tested and
//! cross-compiled in CI:
//! * dynamic libmpv loading + client API ABI check (`loader`, `ffi`)
//! * the pre-initialize option set (`options`)
//! * CSS → physical pixel placement math (`geometry`) and the `VideoSync` seq
//!   latch (`seq`)
//! * the mpv event thread, property observation and snapshot state machine
//!   (`session`, `state`, `hub`)
//!
//! The Win32 host window and Tauri commands live in the application crate; they
//! only push a rectangle / commands into this crate.

pub mod error;
pub mod ffi;
pub mod geometry;
pub mod hub;
pub mod loader;
pub mod options;
pub mod seq;
pub mod session;
pub mod state;
pub mod types;

pub use error::{DllAttempt, MpvError};
pub use geometry::{resolve_placement, PixelRect};
pub use hub::{SnapshotHub, SnapshotSink};
pub use loader::MpvLibrary;
pub use options::{MpvOption, build_initial_options};
pub use seq::SeqLatch;
pub use session::{PlayerSession, SessionConfig, EVENT_TIMEOUT_SECS, SHUTDOWN_TIMEOUT, SNAPSHOT_INTERVAL};
pub use state::SnapshotAssembler;
pub use types::{
    OpenRequest, PlayerCommand, PlayerSnapshot, PlayerStatus, PlayerTrack, TrackKind, VideoSync,
    STATUS_ENDED, STATUS_ERROR, STATUS_IDLE, STATUS_LOADING, STATUS_PAUSED, STATUS_PLAYING,
};
