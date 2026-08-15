//! Fixed-width Bitcoin Base58 for 32-byte values.
//!
//! Braid58 is allocation-free and selects dedicated AVX-512 or AVX2
//! implementations at runtime. Other CPUs use the portable scalar backend.
//!
//! ```
//! let bytes = [42_u8; 32];
//! let encoded = braid58::encode_32(&bytes);
//! let decoded = braid58::decode_32(encoded.as_str())?;
//! assert_eq!(decoded, bytes);
//! # Ok::<(), braid58::DecodeError>(())
//! ```

#![no_std]

use core::ffi::{c_char, c_int};
use core::{fmt, ops, str};

/// The decoded width accepted and produced by this crate.
pub const BINARY_SIZE: usize = 32;

/// The maximum encoded length of a 32-byte value.
pub const MAX_ENCODED_LEN: usize = 44;

const ENCODED_CAPACITY: usize = MAX_ENCODED_LEN + 1;

unsafe extern "C" {
    fn braid58_encode_32(input: *const u8, output: *mut c_char) -> usize;
    fn braid58_decode_32(input: *const c_char, length: usize, output: *mut u8) -> c_int;
    fn braid58_get_backend() -> c_int;
}

/// The implementation selected for the current CPU.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum Backend {
    /// Portable scalar conversion.
    Scalar,
    /// AVX2 conversion.
    Avx2,
    /// AVX2 plus AVX-512 F/DQ/BW/VL/IFMA/VBMI/VBMI2.
    Avx512,
}

/// Returns the implementation selected for the current CPU.
#[must_use]
pub fn backend() -> Backend {
    // SAFETY: The function takes no pointers and has no preconditions.
    match unsafe { braid58_get_backend() } {
        1 => Backend::Avx2,
        2 => Backend::Avx512,
        _ => Backend::Scalar,
    }
}

/// An encoded 32-byte value.
///
/// It borrows as `str` or `[u8]` and implements [`fmt::Display`].
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct Encoded32 {
    bytes: [u8; ENCODED_CAPACITY],
    len: u8,
}

impl Encoded32 {
    /// Returns the encoded bytes without the internal NUL terminator.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len()]
    }

    /// Returns the encoded value as ASCII text.
    #[must_use]
    pub fn as_str(&self) -> &str {
        // SAFETY: The C encoder only emits the ASCII Bitcoin alphabet.
        unsafe { str::from_utf8_unchecked(self.as_bytes()) }
    }

    /// Returns the encoded length.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.len as usize
    }

    /// Encoded fixed-width values are never empty.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        false
    }
}

impl AsRef<[u8]> for Encoded32 {
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl AsRef<str> for Encoded32 {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

impl ops::Deref for Encoded32 {
    type Target = str;

    fn deref(&self) -> &Self::Target {
        self.as_str()
    }
}

impl fmt::Display for Encoded32 {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

impl fmt::Debug for Encoded32 {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("Encoded32")
            .field(&self.as_str())
            .finish()
    }
}

/// Encodes exactly 32 bytes with the Bitcoin Base58 alphabet.
#[must_use]
pub fn encode_32(input: &[u8; BINARY_SIZE]) -> Encoded32 {
    let mut bytes = [0_u8; ENCODED_CAPACITY];
    // SAFETY: Both pointers reference live buffers with the required sizes.
    let len = unsafe { braid58_encode_32(input.as_ptr(), bytes.as_mut_ptr().cast()) };
    debug_assert!((BINARY_SIZE..=MAX_ENCODED_LEN).contains(&len));
    #[allow(clippy::cast_possible_truncation)]
    let len = len as u8;
    Encoded32 { bytes, len }
}

/// An invalid, overflowing, or noncanonical fixed-width Base58 value.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct DecodeError;

impl fmt::Display for DecodeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("invalid 32-byte Bitcoin Base58 value")
    }
}

impl core::error::Error for DecodeError {}

/// Decodes a canonical Bitcoin Base58 value into exactly 32 bytes.
///
/// Both `&str` and byte slices are accepted.
///
/// # Errors
///
/// Returns [`DecodeError`] for invalid, overflowing, or noncanonical input.
pub fn decode_32(input: impl AsRef<[u8]>) -> Result<[u8; BINARY_SIZE], DecodeError> {
    let mut output = [0_u8; BINARY_SIZE];
    decode_32_into(input, &mut output)?;
    Ok(output)
}

/// Decodes into an existing 32-byte buffer.
///
/// `output` is unchanged when decoding fails.
///
/// # Errors
///
/// Returns [`DecodeError`] for invalid, overflowing, or noncanonical input.
pub fn decode_32_into(
    input: impl AsRef<[u8]>,
    output: &mut [u8; BINARY_SIZE],
) -> Result<(), DecodeError> {
    let input = input.as_ref();
    // SAFETY: The pointers reference live buffers for the supplied lengths.
    let success =
        unsafe { braid58_decode_32(input.as_ptr().cast(), input.len(), output.as_mut_ptr()) };
    if success == 1 {
        Ok(())
    } else {
        Err(DecodeError)
    }
}

#[cfg(test)]
mod tests {
    extern crate std;

    use self::std::string::ToString;
    use super::*;

    const BYTES: [u8; 32] = [
        24, 243, 6, 223, 230, 153, 210, 8, 92, 137, 123, 67, 164, 197, 79, 196, 125, 43, 183, 85,
        103, 91, 232, 167, 73, 131, 104, 131, 0, 101, 214, 231,
    ];
    const TEXT: &str = "2gPihUTjt3FJqf1VpidgrY5cZ6PuyMccGVwQHRfjMPZG";

    #[test]
    fn known_value_round_trip() {
        let encoded = encode_32(&BYTES);
        assert_eq!(encoded.as_str(), TEXT);
        assert_eq!(encoded.as_bytes(), TEXT.as_bytes());
        assert_eq!(encoded.to_string(), TEXT);
        assert_eq!(decode_32(encoded), Ok(BYTES));
    }

    #[test]
    fn leading_zeroes_are_canonical() {
        let zero = encode_32(&[0; 32]);
        assert_eq!(zero.as_str(), "11111111111111111111111111111111");
        assert_eq!(decode_32(zero), Ok([0; 32]));
    }

    #[test]
    fn failure_preserves_output() {
        let mut output = [0xa5; 32];
        assert_eq!(decode_32_into("not base58", &mut output), Err(DecodeError));
        assert_eq!(output, [0xa5; 32]);
    }

    #[test]
    fn backend_is_known() {
        assert!(matches!(
            backend(),
            Backend::Scalar | Backend::Avx2 | Backend::Avx512
        ));
    }
}
