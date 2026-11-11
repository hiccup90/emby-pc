//! Native child window that libmpv embeds into with `--wid`.
//!
//! Why a host window instead of embedding into the Tauri window directly:
//! libmpv creates and owns its own `HWND` under the given `wid` parent from its
//! playback thread. Calling Win32 window APIs on that foreign window from the
//! UI thread (the old project used `SetWindowSubclass`) is not thread safe, so
//! we only ever:
//! * move/show/hide the **host** window (owned by the main thread),
//! * walk `EnumChildWindows` and sync the geometry / ex-style of **direct
//!   children** with `SWP_ASYNCWINDOWPOS`,
//! * assert the host is the top sibling (`GetWindow(host, GW_HWNDPREV) == null`).
//!
//! Child syncing is **event driven**, not polled. Every path that can make the
//! host change size or gain a child ends in one synchronous
//! `sync_direct_children` call:
//! * `WM_SIZE` — the host was placed or resized (`apply_pending` runs on every
//!   `video_sync`, so this covers the page moving or resizing the video rect,
//!   window resizes and fullscreen),
//! * `WM_PARENTNOTIFY` — a child of the host was created or destroyed (mpv
//!   creates its VO window as a child of the host on its playback thread),
//! * the host's own `WM_CREATE`, so a restarted host starts aligned.
//!
//! A slow `FALLBACK_INTERVAL_MS` timer remains **only** as a safety net: an
//! out-of-band child creation (mpv setting `WS_EX_NOPARENTNOTIFY` suppresses
//! `WM_PARENTNOTIFY`, and `SetWinEventHook` would need a different
//! `windows-sys` feature set) must not leave the video permanently mis-sized
//! on a machine we cannot debug. It does no Win32 work unless the child really
//! is out of sync, so the steady-state cost is one timer tick per second.
//!
//! Coordinates: `VideoSync` is CSS pixels relative to the webview client area;
//! `emby_mpv::resolve_placement` converts to physical pixels and clamps to the
//! parent client area.

#[cfg(windows)]
mod imp {
    use std::ffi::c_void;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

    use emby_mpv::{resolve_placement, SeqLatch, VideoSync};
    use windows_sys::Win32::Foundation::{
        COLORREF, GetLastError, HWND, LPARAM, LRESULT, RECT, WPARAM,
    };
    use windows_sys::Win32::Graphics::Gdi::{
        BeginPaint, CreateSolidBrush, DeleteObject, EndPaint, FillRect, HBRUSH, HDC, HGDIOBJ,
        PAINTSTRUCT,
    };
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, CREATESTRUCTW, DefWindowProcW, DestroyWindow, EnumChildWindows,
        GetClientRect,
        GetParent, GetWindow, GetWindowLongPtrW, KillTimer, LoadCursorW, PostMessageW,
        RegisterClassExW, SetTimer, SetWindowLongPtrW, SetWindowPos, ShowWindow, GWLP_USERDATA,
        GWL_EXSTYLE, GW_HWNDPREV, HTTRANSPARENT, HWND_TOP, IDC_ARROW, MA_NOACTIVATE,
        SWP_ASYNCWINDOWPOS, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE, SWP_SHOWWINDOW, SW_HIDE,
        SW_SHOWNA, WM_APP, WM_CREATE, WM_ERASEBKGND, WM_MOUSEACTIVATE, WM_NCDESTROY, WM_NCHITTEST,
        WM_PAINT, WM_PARENTNOTIFY, WM_SIZE, WM_TIMER, WNDCLASSEXW, WS_CHILD, WS_CLIPCHILDREN,
        WS_CLIPSIBLINGS, WS_EX_NOACTIVATE, WS_EX_NOPARENTNOTIFY, WS_EX_TRANSPARENT,
    };

    /// `WM_APP + 1`: "apply the latest `VideoSync`" (posted from IPC threads so
    /// the Win32 calls run on the window owner / main thread).
    pub(crate) const WM_EMBY_VIDEO_SYNC: u32 = WM_APP + 1;

    /// Safety-net timer id. Geometry itself is event driven (see the module
    /// docs); this only re-checks for a child window the events missed.
    const TIMER_ID: usize = 0xE1B1;
    /// One second, not the old 120 ms: every in-band change already syncs
    /// synchronously, so this only has to *eventually* repair a miss.
    const FALLBACK_INTERVAL_MS: u32 = 1000;

    /// UTF-16 for "EmbyVideoHost" + NUL. Kept as a const so the pointer handed
    /// to `RegisterClassExW` stays valid for the process lifetime.
    const CLASS_NAME: &[u16] = &[
        0x0045, 0x006D, 0x0062, 0x0079, 0x0056, 0x0069, 0x0064, 0x0065, 0x006F, 0x0048, 0x006F,
        0x0073, 0x0074, 0x0000,
    ];
    /// UTF-16 for "EmbyVideoHost" window title (invisible, useful in Spy++).
    const WINDOW_TITLE: &[u16] = &[
        0x0045, 0x006D, 0x0062, 0x0079, 0x0056, 0x0069, 0x0064, 0x0065, 0x006F, 0x0048, 0x006F,
        0x0073, 0x0074, 0x0000,
    ];

    const HOST_EX_STYLE: u32 = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY;

    /// Physical-pixel bleed added on every side of the video rect. Win11 keeps a
    /// 1 px non-client inset on frameless windows (tao compensates the frame but
    /// leaves `top_inset = round(dpi/96)`), which shows up as a desktop-coloured
    /// seam when the window is flush with the top of the screen. The host is a
    /// child window, so it is clipped to the parent client area anyway: drawing
    /// one pixel wider is invisible, while one pixel narrower would expose the
    /// seam.
    const HOST_BLEED: i32 = 1;

    static CLASS_REGISTRATION: OnceLock<Result<(), u32>> = OnceLock::new();

    struct PendingState {
        pending: Option<VideoSync>,
        latch: SeqLatch,
    }

    struct HostState {
        pending: Mutex<PendingState>,
        /// `HBRUSH` as `usize` so the state stays `Send + Sync` for IPC threads.
        black_brush: usize,
        destroyed: AtomicBool,
    }

    impl HostState {
        fn new(black_brush: usize) -> Self {
            Self {
                pending: Mutex::new(PendingState {
                    pending: None,
                    latch: SeqLatch::new(),
                }),
                black_brush,
                destroyed: AtomicBool::new(false),
            }
        }

        fn lock(&self) -> MutexGuard<'_, PendingState> {
            self.pending
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
        }

        /// Keep only the newest `seq`; returns `false` for stale updates.
        ///
        /// `seq == 0` is treated as "the UI counter restarted" (route remount).
        /// Without this, leaving and re-entering the player page would starve
        /// every later `video_sync` because the latch still remembers the old,
        /// higher sequence numbers.
        fn store(&self, sync: VideoSync) -> bool {
            let mut state = self.lock();
            if sync.seq == 0 {
                state.latch.reset();
            }
            if !state.latch.accept(sync.seq) {
                return false;
            }
            state.pending = Some(sync);
            true
        }

        fn take(&self) -> Option<VideoSync> {
            self.lock().pending.take()
        }

        fn store_hidden(&self) {
            let mut state = self.lock();
            let seq = state.latch.next();
            state.pending = Some(VideoSync::hidden(seq));
        }

        fn brush(&self) -> HBRUSH {
            self.black_brush as HBRUSH
        }
    }

    /// Main-thread handle to the host window. `Send + Sync` on purpose (raw
    /// `HWND` kept as `usize`); all Win32 calls still happen on the owner
    /// thread or through `PostMessageW`.
    pub struct VideoHost {
        hwnd: usize,
        state: Arc<HostState>,
    }

    impl VideoHost {
        /// Must be called on the main thread (Tauri `setup`).
        pub fn create(parent_hwnd: isize) -> Result<Self, String> {
            if parent_hwnd == 0 {
                return Err("video host: parent HWND is null".to_string());
            }
            let hinstance = unsafe { GetModuleHandleW(std::ptr::null()) };
            ensure_class_registered(hinstance)?;

            let black_brush = unsafe { CreateSolidBrush(0 as COLORREF) };
            if black_brush.is_null() {
                return Err(format!(
                    "video host: CreateSolidBrush failed (Win32 error {})",
                    unsafe { GetLastError() }
                ));
            }
            let state = Arc::new(HostState::new(black_brush as usize));
            // Ownership of this `Box<Arc<HostState>>` moves to GWLP_USERDATA and
            // is released in WM_NCDESTROY. If creation fails after WM_CREATE,
            // Windows may already have delivered WM_NCDESTROY (freeing the
            // Box), so the failure path below must not free it again. A Box
            // that was never installed leaks once on this one-shot fatal
            // path, which is safer than a double free.
            let userdata = Box::into_raw(Box::new(Arc::clone(&state)));

            let hwnd = unsafe {
                CreateWindowExW(
                    HOST_EX_STYLE,
                    CLASS_NAME.as_ptr(),
                    WINDOW_TITLE.as_ptr(),
                    WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                    0,
                    0,
                    0,
                    0,
                    parent_hwnd as HWND,
                    std::ptr::null_mut(),
                    hinstance,
                    userdata as *const c_void,
                )
            };

            if hwnd.is_null() {
                // No `Box::from_raw` here, see the ownership note above.
                unsafe { DeleteObject(black_brush as HGDIOBJ) };
                return Err(format!(
                    "video host: CreateWindowExW failed (Win32 error {})",
                    unsafe { GetLastError() }
                ));
            }

            // One explicit sync at creation: put the host on top of the Tauri
            // window's child stack and pick up any child that already exists.
            // Everything after this is event driven (see the module docs); the
            // timer below is only the safety net.
            unsafe {
                maintain_z_order(hwnd);
                sync_direct_children(hwnd);
                SetTimer(hwnd, TIMER_ID, FALLBACK_INTERVAL_MS, None);
            }

            Ok(Self {
                hwnd: hwnd as usize,
                state,
            })
        }

        /// Raw HWND for the mpv `wid` option (decimal string).
        pub fn hwnd(&self) -> u64 {
            self.hwnd as u64
        }

        /// Store the newest rect and wake the main thread. Safe from any thread.
        pub fn push_sync(&self, sync: VideoSync) -> bool {
            if self.state.destroyed.load(Ordering::SeqCst) || self.hwnd == 0 {
                return false;
            }
            if !self.state.store(sync) {
                return false;
            }
            unsafe { PostMessageW(self.hwnd as HWND, WM_EMBY_VIDEO_SYNC, 0, 0) != 0 }
        }

        /// Hide the video (used before toggling fullscreen, so the native child
        /// never survives a window/size transition visibly).
        pub fn hide(&self) -> bool {
            if self.state.destroyed.load(Ordering::SeqCst) || self.hwnd == 0 {
                return false;
            }
            self.state.store_hidden();
            unsafe { PostMessageW(self.hwnd as HWND, WM_EMBY_VIDEO_SYNC, 0, 0) != 0 }
        }

        /// Must be called on the main thread, after the mpv thread terminated.
        pub fn destroy(&self) {
            if self.state.destroyed.swap(true, Ordering::SeqCst) {
                return;
            }
            if self.hwnd != 0 {
                unsafe {
                    KillTimer(self.hwnd as HWND, TIMER_ID);
                    DestroyWindow(self.hwnd as HWND);
                }
            }
            let brush = self.state.brush();
            if !brush.is_null() {
                unsafe { DeleteObject(brush as HGDIOBJ) };
            }
        }
    }

    fn ensure_class_registered(hinstance: *mut c_void) -> Result<(), String> {
        let result = CLASS_REGISTRATION.get_or_init(|| unsafe {
            let window_class = WNDCLASSEXW {
                cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
                style: 0,
                lpfnWndProc: Some(host_proc),
                cbClsExtra: 0,
                cbWndExtra: 0,
                hInstance: hinstance,
                hIcon: std::ptr::null_mut(),
                hCursor: LoadCursorW(std::ptr::null_mut(), IDC_ARROW),
                // Background is painted black by WM_ERASEBKGND/WM_PAINT.
                hbrBackground: std::ptr::null_mut(),
                lpszMenuName: std::ptr::null(),
                lpszClassName: CLASS_NAME.as_ptr(),
                hIconSm: std::ptr::null_mut(),
            };
            let atom = RegisterClassExW(&window_class);
            if atom == 0 {
                Err(GetLastError())
            } else {
                Ok(())
            }
        });
        match result {
            Ok(()) => Ok(()),
            Err(code) => Err(format!(
                "video host: RegisterClassExW(\"EmbyVideoHost\") failed (Win32 error {code})"
            )),
        }
    }

    /// The returned reference is valid for the duration of the current window
    /// message only: `Box<Arc<HostState>>` is released in `WM_NCDESTROY`, which
    /// `DestroyWindow` delivers last. Never store the result; the `'static`
    /// lifetime is a convenience for the wndproc, not a real guarantee.
    unsafe fn host_state(hwnd: HWND) -> Option<&'static HostState> {
        let pointer = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *const Arc<HostState>;
        if pointer.is_null() {
            None
        } else {
            Some(&**pointer)
        }
    }

    unsafe extern "system" fn host_proc(
        hwnd: HWND,
        msg: u32,
        wparam: WPARAM,
        lparam: LPARAM,
    ) -> LRESULT {
        match msg {
            // The video area must never swallow clicks: the webview below owns
            // the overlay controls and the click-to-pause surface.
            WM_NCHITTEST => HTTRANSPARENT as LRESULT,
            WM_MOUSEACTIVATE => MA_NOACTIVATE as LRESULT,
            WM_CREATE => {
                // WM_CREATE's lParam is a `*const CREATESTRUCTW`, NOT the
                // lpCreateParams value itself. Storing lParam directly would
                // make `host_state()` dereference the CREATESTRUCT (stack
                // memory) as an `Arc<HostState>`.
                let create = &*(lparam as *const CREATESTRUCTW);
                SetWindowLongPtrW(
                    hwnd,
                    GWLP_USERDATA,
                    create.lpCreateParams as isize,
                );
                0
            }
            WM_ERASEBKGND => {
                if let Some(state) = host_state(hwnd) {
                    let mut rect = RECT::default();
                    if GetClientRect(hwnd, &mut rect) != 0 {
                        FillRect(wparam as HDC, &rect, state.brush());
                    }
                }
                1
            }
            WM_PAINT => {
                let mut paint = PAINTSTRUCT::default();
                let hdc = BeginPaint(hwnd, &mut paint);
                if let Some(state) = host_state(hwnd) {
                    let mut rect = RECT::default();
                    if GetClientRect(hwnd, &mut rect) != 0 {
                        FillRect(hdc, &rect, state.brush());
                    }
                }
                EndPaint(hwnd, &paint);
                0
            }
            // Safety net only (see the module docs): repair a child that was
            // created without a WM_PARENTNOTIFY we could observe.
            WM_TIMER => {
                if wparam == TIMER_ID && host_needs_repair(hwnd) {
                    maintain_z_order(hwnd);
                    sync_direct_children(hwnd);
                }
                0
            }
            // The host was placed or resized (apply_pending uses SetWindowPos).
            //
            // mpv's VO child does not follow its parent on its own, so it is
            // re-aligned here instead of from a timer. `WM_SIZE` alone is enough:
            // `DefWindowProcW` turns a position change into `WM_SIZE` for this
            // window anyway, and a pure *move* needs no child sync at all — the
            // child is laid out at `(0, 0)` inside the host client area, so only
            // its size can drift.
            WM_SIZE => {
                DefWindowProcW(hwnd, msg, wparam, lparam);
                sync_direct_children(hwnd);
                0
            }
            // A child of the host is being created or destroyed. mpv creates its
            // VO window on the playback thread, so this is the notification that
            // replaces "poll until it shows up": syncing right away means the new
            // child is never visible at the wrong size.
            WM_PARENTNOTIFY => {
                sync_direct_children(hwnd);
                0
            }
            WM_EMBY_VIDEO_SYNC => {
                if let Some(state) = host_state(hwnd) {
                    apply_pending(hwnd, state);
                }
                0
            }
            WM_NCDESTROY => {
                KillTimer(hwnd, TIMER_ID);
                let pointer = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut Arc<HostState>;
                if !pointer.is_null() {
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    drop(Box::from_raw(pointer));
                }
                0
            }
            _ => DefWindowProcW(hwnd, msg, wparam, lparam),
        }
    }

    /// Host must be the topmost sibling of the Tauri window, above the WebView2
    /// child, otherwise the video hides behind the UI.
    ///
    /// Runs on the host's owner thread (from `WM_TIMER`), so `SetWindowPos` is a
    /// synchronous same-input-queue call. `SWP_ASYNCWINDOWPOS` would degrade to
    /// a normal call here (it only posts when the target belongs to another
    /// input queue), so it is omitted to keep the synchronous path explicit.
    /// `WM_WINDOWPOSCHANGED` does not run this: `apply_pending` already places
    /// the host with `HWND_TOP`, and re-entering `SetWindowPos` from that
    /// message would recurse.
    unsafe fn maintain_z_order(hwnd: HWND) {
        let previous = GetWindow(hwnd, GW_HWNDPREV);
        if !previous.is_null() {
            SetWindowPos(
                hwnd,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
            );
        }
    }

    unsafe fn sync_direct_children(hwnd: HWND) {
        let mut context = ChildSyncContext {
            host: hwnd,
            stale: false,
        };
        EnumChildWindows(
            hwnd,
            Some(sync_child_proc),
            &mut context as *mut ChildSyncContext as LPARAM,
        );
    }

    /// True when the host is not the top sibling or a direct child is missing
    /// the click-through ex-style bits / does not match the host client area.
    ///
    /// The safety-net timer only does its Win32 work when this returns true, so
    /// a healthy session costs one `GetWindow` + `EnumChildWindows` +
    /// `GetClientRect` per second and nothing else.
    unsafe fn host_needs_repair(hwnd: HWND) -> bool {
        if !GetWindow(hwnd, GW_HWNDPREV).is_null() {
            return true;
        }
        let mut context = ChildSyncContext {
            host: hwnd,
            stale: false,
        };
        EnumChildWindows(
            hwnd,
            Some(stale_child_proc),
            &mut context as *mut ChildSyncContext as LPARAM,
        );
        context.stale
    }

    unsafe extern "system" fn stale_child_proc(child: HWND, lparam: LPARAM) -> i32 {
        let context = &mut *(lparam as *mut ChildSyncContext);
        if GetParent(child) != context.host {
            return 1;
        }
        let mut host_rect = RECT::default();
        let mut child_rect = RECT::default();
        let size_ok = GetClientRect(context.host, &mut host_rect) != 0
            && GetClientRect(child, &mut child_rect) != 0
            && child_rect.right == host_rect.right
            && child_rect.bottom == host_rect.bottom;
        let style = GetWindowLongPtrW(child, GWL_EXSTYLE) as u32;
        let style_ok = style | HOST_EX_STYLE == style;
        if !size_ok || !style_ok {
            context.stale = true;
            return 0; // stop enumerating: one stale child is enough to act.
        }
        1
    }

    struct ChildSyncContext {
        host: HWND,
        /// Set by `stale_child_proc`; unused by the syncing walk.
        stale: bool,
    }

    /// Only direct children belong to mpv's VO. Nested windows are left alone.
    unsafe extern "system" fn sync_child_proc(child: HWND, lparam: LPARAM) -> i32 {
        let context = &*(lparam as *const ChildSyncContext);
        if GetParent(child) != context.host {
            return 1;
        }

        let mut host_rect = RECT::default();
        if GetClientRect(context.host, &mut host_rect) == 0 {
            return 1;
        }

        // mpv owns this child from its playback thread. `SetWindowLongPtrW`
        // on a foreign-thread window is marshalled synchronously and sends
        // WM_STYLECHANGING/WM_STYLECHANGED to that thread, so it can block
        // this timer tick while mpv is busy. Only touch the style when the
        // bits are actually missing (in practice: once, right after mpv
        // creates its VO window); there is no PostMessage equivalent for
        // changing ex-style, and dropping the bits would break click-through.
        let style = GetWindowLongPtrW(child, GWL_EXSTYLE) as u32;
        let wanted = style | HOST_EX_STYLE;
        if wanted != style {
            SetWindowLongPtrW(child, GWL_EXSTYLE, wanted as isize);
        }

        let mut child_rect = RECT::default();
        if GetClientRect(child, &mut child_rect) == 0 {
            return 1;
        }
        if child_rect.right != host_rect.right || child_rect.bottom != host_rect.bottom {
            // Cross-thread: mpv's window belongs to another thread, so the
            // position update must be asynchronous.
            SetWindowPos(
                child,
                HWND_TOP,
                0,
                0,
                host_rect.right.max(0),
                host_rect.bottom.max(0),
                SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS,
            );
        }
        1
    }

    unsafe fn apply_pending(hwnd: HWND, state: &HostState) {
        let Some(sync) = state.take() else {
            return;
        };
        let parent = GetParent(hwnd);
        if parent.is_null() {
            return;
        }
        let mut client = RECT::default();
        if GetClientRect(parent, &mut client) == 0 {
            return;
        }

        match resolve_placement(&sync, client.right, client.bottom) {
            Some(rect) => {
                let style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
                let wanted = style | HOST_EX_STYLE;
                if wanted != style {
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wanted as isize);
                }
                // Outward 1 px, still inside the parent client area (`top` is
                // additionally clamped at 0 so a rect that starts on the very
                // top edge never yields a negative origin, which Win32 would
                // interpret relative to the screen).
                let left = (rect.left - HOST_BLEED).max(0);
                let top = (rect.top - HOST_BLEED).max(0);
                let right = (rect.right() + HOST_BLEED).min(client.right.max(left));
                let bottom = (rect.bottom() + HOST_BLEED).min(client.bottom.max(top));
                let width = (right - left).max(1);
                let height = (bottom - top).max(1);
                // Same-thread call (WM_APP+1 is posted so it runs on the
                // owner thread); no SWP_ASYNCWINDOWPOS needed.
                SetWindowPos(
                    hwnd,
                    HWND_TOP,
                    left,
                    top,
                    width,
                    height,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW,
                );
                ShowWindow(hwnd, SW_SHOWNA);
            }
            None => {
                ShowWindow(hwnd, SW_HIDE);
            }
        }
    }
}

#[cfg(windows)]
pub use imp::VideoHost;

#[cfg(not(windows))]
mod stub {
    use emby_mpv::VideoSync;

    /// Non-Windows stub. The Tauri app still starts (UI only), but video
    /// embedding reports a readable error.
    pub struct VideoHost {
        _private: (),
    }

    impl VideoHost {
        pub fn create(_parent_hwnd: isize) -> Result<Self, String> {
            Err("video host: native video embedding is only implemented on Windows".to_string())
        }

        pub fn hwnd(&self) -> u64 {
            0
        }

        pub fn push_sync(&self, _sync: VideoSync) -> bool {
            false
        }

        pub fn hide(&self) -> bool {
            false
        }

        pub fn destroy(&self) {}
    }
}

#[cfg(not(windows))]
pub use stub::VideoHost;
