//! Small wrapper around Windows DPAPI (`CryptProtectData`).
//!
//! The whole session JSON blob is encrypted with the current user's DPAPI key
//! and `CRYPTPROTECT_UI_FORBIDDEN`; the resulting blob is written by
//! `session::SessionStore` behind a small magic header. On non-Windows builds
//! `protect` is a stub (`Ok(None)`) and the session is stored as plain JSON,
//! matching the "token only" design; `unprotect` cannot handle DPAPI blobs
//! there and reports an error without echoing any secret.

use std::fmt;

#[cfg(windows)]
use windows_sys::Win32::Foundation::LocalFree;
#[cfg(windows)]
use windows_sys::Win32::Security::Cryptography::{
    CryptProtectData, CryptUnprotectData, CRYPT_INTEGER_BLOB, CRYPTPROTECT_UI_FORBIDDEN,
};

/// Protection failure. The message only contains the operation and the OS
/// error; it never contains the plaintext, the token or the protected blob.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SecretError {
    operation: &'static str,
    detail: String,
}

impl SecretError {
    fn new(operation: &'static str, detail: impl Into<String>) -> Self {
        Self {
            operation,
            detail: detail.into(),
        }
    }
}

impl fmt::Display for SecretError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.operation, self.detail)
    }
}

impl std::error::Error for SecretError {}

/// Encrypts a session JSON blob. `Ok(None)` means this target has no DPAPI and
/// the caller should store the plaintext (non-Windows stub).
#[cfg(windows)]
pub fn protect(plaintext: &[u8]) -> Result<Option<Vec<u8>>, SecretError> {
    if plaintext.is_empty() {
        return Err(SecretError::new("CryptProtectData", "empty session blob"));
    }

    let input = CRYPT_INTEGER_BLOB {
        cbData: plaintext.len() as u32,
        pbData: plaintext.as_ptr() as *mut u8,
    };
    let mut output = CRYPT_INTEGER_BLOB::default();
    let ok = unsafe {
        CryptProtectData(
            &input,
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
            CRYPTPROTECT_UI_FORBIDDEN,
            &mut output,
        )
    };
    if ok == 0 {
        return Err(last_os_error("CryptProtectData"));
    }
    if output.pbData.is_null() || output.cbData == 0 {
        if !output.pbData.is_null() {
            unsafe {
                LocalFree(output.pbData.cast::<std::ffi::c_void>());
            }
        }
        return Err(SecretError::new(
            "CryptProtectData",
            "empty protected blob",
        ));
    }

    let protected =
        unsafe { std::slice::from_raw_parts(output.pbData, output.cbData as usize) }.to_vec();
    unsafe {
        LocalFree(output.pbData.cast());
    }
    Ok(Some(protected))
}

/// Decrypts a DPAPI blob produced by [`protect`].
#[cfg(windows)]
pub fn unprotect(protected: &[u8]) -> Result<Vec<u8>, SecretError> {
    if protected.is_empty() {
        return Err(SecretError::new("CryptUnprotectData", "empty DPAPI blob"));
    }

    let input = CRYPT_INTEGER_BLOB {
        cbData: protected.len() as u32,
        pbData: protected.as_ptr() as *mut u8,
    };
    let mut output = CRYPT_INTEGER_BLOB::default();
    let ok = unsafe {
        CryptUnprotectData(
            &input,
            std::ptr::null_mut(),
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
            CRYPTPROTECT_UI_FORBIDDEN,
            &mut output,
        )
    };
    if ok == 0 {
        return Err(last_os_error("CryptUnprotectData"));
    }
    if output.pbData.is_null() || output.cbData == 0 {
        if !output.pbData.is_null() {
            unsafe {
                LocalFree(output.pbData.cast::<std::ffi::c_void>());
            }
        }
        return Err(SecretError::new(
            "CryptUnprotectData",
            "empty plaintext blob",
        ));
    }

    let plaintext =
        unsafe { std::slice::from_raw_parts(output.pbData, output.cbData as usize) }.to_vec();
    unsafe {
        LocalFree(output.pbData.cast());
    }
    Ok(plaintext)
}

#[cfg(windows)]
fn last_os_error(operation: &'static str) -> SecretError {
    SecretError::new(operation, std::io::Error::last_os_error().to_string())
}

/// Non-Windows stub: the token is stored as plain JSON by the caller.
#[cfg(not(windows))]
pub fn protect(_plaintext: &[u8]) -> Result<Option<Vec<u8>>, SecretError> {
    Ok(None)
}

/// Non-Windows stub: a DPAPI blob can only be decrypted on Windows.
#[cfg(not(windows))]
pub fn unprotect(_protected: &[u8]) -> Result<Vec<u8>, SecretError> {
    Err(SecretError::new(
        "CryptUnprotectData",
        "Windows DPAPI is unavailable on this platform",
    ))
}

#[cfg(test)]
mod tests {
    #[cfg(not(windows))]
    #[test]
    fn non_windows_is_a_plaintext_stub() {
        assert!(super::protect(b"test payload").unwrap().is_none());
        assert!(super::unprotect(b"test payload").is_err());
    }

    #[cfg(windows)]
    #[test]
    fn dpapi_round_trips_a_session_blob() {
        let token = vec!["test", "access", "token"].join("-");
        let payload = format!(
            r#"{{"serverUrl":"http://example","accessToken":"{}"}}"#,
            token
        );
        let protected = super::protect(payload.as_bytes())
            .expect("DPAPI protect failed")
            .expect("Windows must return a protected blob");
        assert!(!protected.is_empty());
        assert_eq!(super::unprotect(&protected).unwrap(), payload.as_bytes().to_vec());
    }

    #[cfg(windows)]
    #[test]
    fn dpapi_rejects_garbage() {
        assert!(super::unprotect(b"not a DPAPI blob").is_err());
    }
}
