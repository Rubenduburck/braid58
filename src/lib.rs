//! Fixed-width Bitcoin Base58 for 32- and 64-byte values.
//!
//! Braid58 is allocation-free.
//! Cargo target features select the scalar, AVX2, or AVX-512 backend at compile time.
//! The `avx2` and `avx512` features explicitly require those SIMD backends.
//! Enabling either feature makes the resulting binary require the corresponding ISA.
//!
//! ```
//! let bytes = [42_u8; 32];
//! let encoded = braid58::encode_32(&bytes);
//! let decoded = braid58::decode_32(encoded.as_str())?;
//! assert_eq!(decoded, bytes);
//!
//! let wide = [7_u8; 64];
//! let encoded = braid58::encode_64(&wide);
//! assert_eq!(braid58::decode_64(&encoded)?, wide);
//! # Ok::<(), braid58::DecodeError>(())
//! ```

#![no_std]

use core::ffi::{c_char, c_int};
use core::{fmt, ops, str};

/// The decoded width accepted and produced by the 32-byte API.
pub const BINARY_32_SIZE: usize = 32;

/// The decoded width accepted and produced by the 64-byte API.
pub const BINARY_64_SIZE: usize = 64;

/// The decoded width accepted and produced by the original 32-byte API.
pub const BINARY_SIZE: usize = BINARY_32_SIZE;

/// The maximum encoded length of a 32-byte value.
pub const MAX_ENCODED_32_LEN: usize = 44;

/// The maximum encoded length of a 64-byte value.
pub const MAX_ENCODED_64_LEN: usize = 88;

/// The maximum encoded length of the original 32-byte API.
pub const MAX_ENCODED_LEN: usize = MAX_ENCODED_32_LEN;

const ENCODED_32_CAPACITY: usize = MAX_ENCODED_32_LEN + 1;
const ENCODED_64_CAPACITY: usize = MAX_ENCODED_64_LEN + 1;

unsafe extern "C" {
    fn braid58_encode_32(input: *const u8, output: *mut c_char) -> usize;
    fn braid58_encode_32x2(input: *const u8, output: *mut c_char, output_len: *mut usize);
    fn braid58_encode_32x3(input: *const u8, output: *mut c_char, output_len: *mut usize);
    fn braid58_decode_32(input: *const c_char, length: usize, output: *mut u8) -> c_int;
    fn braid58_encode_64(input: *const u8, output: *mut c_char) -> usize;
    fn braid58_encode_64x2(input: *const u8, output: *mut c_char, output_len: *mut usize);
    fn braid58_encode_64x3(input: *const u8, output: *mut c_char, output_len: *mut usize);
    fn braid58_decode_64(input: *const c_char, length: usize, output: *mut u8) -> c_int;
}

/// An encoded 32-byte value.
///
/// The value owns its inline output buffer, borrows as `str` or `[u8]`, and implements
/// [`fmt::Display`].
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct Encoded32 {
    bytes: [u8; ENCODED_32_CAPACITY],
    len: u8,
}

impl Encoded32 {
    fn from_parts(bytes: [u8; ENCODED_32_CAPACITY], len: usize) -> Self {
        debug_assert!((BINARY_32_SIZE..=MAX_ENCODED_32_LEN).contains(&len));
        #[allow(clippy::cast_possible_truncation)]
        let len = len as u8;
        Self { bytes, len }
    }

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
///
/// The returned [`Encoded32`] owns its output and does not allocate.
///
/// # Examples
///
/// ```
/// let encoded = braid58::encode_32(&[0_u8; 32]);
/// assert_eq!(encoded.as_str(), "11111111111111111111111111111111");
/// assert_eq!(encoded.as_bytes(), encoded.as_str().as_bytes());
/// assert_eq!(braid58::decode_32(&encoded)?, [0_u8; 32]);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
#[must_use]
pub fn encode_32(input: &[u8; BINARY_32_SIZE]) -> Encoded32 {
    let mut bytes = [0_u8; ENCODED_32_CAPACITY];
    // SAFETY: Both pointers reference live buffers with the required sizes.
    let len = unsafe { braid58_encode_32(input.as_ptr(), bytes.as_mut_ptr().cast()) };
    Encoded32::from_parts(bytes, len)
}

/// Encodes two independent 32-byte values through the fixed x2 entry point.
///
/// # Examples
///
/// ```
/// let input = [[0_u8; 32], [1_u8; 32]];
/// let encoded = braid58::encode_32x2(&input);
/// assert_eq!(braid58::decode_32(&encoded[0])?, input[0]);
/// assert_eq!(braid58::decode_32(&encoded[1])?, input[1]);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
#[must_use]
pub fn encode_32x2(input: &[[u8; BINARY_32_SIZE]; 2]) -> [Encoded32; 2] {
    let mut bytes = [[0_u8; ENCODED_32_CAPACITY]; 2];
    let mut lengths = [0_usize; 2];
    // SAFETY: The pointers reference fixed-size live input and output arrays.
    unsafe {
        braid58_encode_32x2(
            input.as_ptr().cast(),
            bytes.as_mut_ptr().cast(),
            lengths.as_mut_ptr(),
        );
    }
    [
        Encoded32::from_parts(bytes[0], lengths[0]),
        Encoded32::from_parts(bytes[1], lengths[1]),
    ]
}

/// Encodes three independent 32-byte values through the fixed x3 entry point.
///
/// # Examples
///
/// ```
/// let input = [[1_u8; 32], [2_u8; 32], [3_u8; 32]];
/// let encoded = braid58::encode_32x3(&input);
/// for (text, bytes) in encoded.iter().zip(input) {
///     assert_eq!(braid58::decode_32(text)?, bytes);
/// }
/// # Ok::<(), braid58::DecodeError>(())
/// ```
#[must_use]
pub fn encode_32x3(input: &[[u8; BINARY_32_SIZE]; 3]) -> [Encoded32; 3] {
    let mut bytes = [[0_u8; ENCODED_32_CAPACITY]; 3];
    let mut lengths = [0_usize; 3];
    // SAFETY: The pointers reference fixed-size live input and output arrays.
    unsafe {
        braid58_encode_32x3(
            input.as_ptr().cast(),
            bytes.as_mut_ptr().cast(),
            lengths.as_mut_ptr(),
        );
    }
    [
        Encoded32::from_parts(bytes[0], lengths[0]),
        Encoded32::from_parts(bytes[1], lengths[1]),
        Encoded32::from_parts(bytes[2], lengths[2]),
    ]
}

/// An encoded 64-byte value.
///
/// The value owns its inline output buffer, borrows as `str` or `[u8]`, and implements
/// [`fmt::Display`].
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub struct Encoded64 {
    bytes: [u8; ENCODED_64_CAPACITY],
    len: u8,
}

impl Encoded64 {
    fn from_parts(bytes: [u8; ENCODED_64_CAPACITY], len: usize) -> Self {
        debug_assert!((BINARY_64_SIZE..=MAX_ENCODED_64_LEN).contains(&len));
        #[allow(clippy::cast_possible_truncation)]
        let len = len as u8;
        Self { bytes, len }
    }

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

impl AsRef<[u8]> for Encoded64 {
    fn as_ref(&self) -> &[u8] {
        self.as_bytes()
    }
}

impl AsRef<str> for Encoded64 {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

impl ops::Deref for Encoded64 {
    type Target = str;

    fn deref(&self) -> &Self::Target {
        self.as_str()
    }
}

impl fmt::Display for Encoded64 {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

impl fmt::Debug for Encoded64 {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("Encoded64")
            .field(&self.as_str())
            .finish()
    }
}

/// Encodes exactly 64 bytes with the Bitcoin Base58 alphabet.
///
/// The returned [`Encoded64`] owns its output and does not allocate.
///
/// # Examples
///
/// ```
/// let encoded = braid58::encode_64(&[0_u8; 64]);
/// assert_eq!(encoded.len(), 64);
/// assert!(encoded.as_bytes().iter().all(|byte| *byte == b'1'));
/// assert_eq!(braid58::decode_64(&encoded)?, [0_u8; 64]);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
#[must_use]
pub fn encode_64(input: &[u8; BINARY_64_SIZE]) -> Encoded64 {
    let mut bytes = [0_u8; ENCODED_64_CAPACITY];
    // SAFETY: Both pointers reference live buffers with the required sizes.
    let len = unsafe { braid58_encode_64(input.as_ptr(), bytes.as_mut_ptr().cast()) };
    Encoded64::from_parts(bytes, len)
}

/// Encodes two independent 64-byte values through the fixed x2 entry point.
///
/// # Examples
///
/// ```
/// let input = [[0_u8; 64], [1_u8; 64]];
/// let encoded = braid58::encode_64x2(&input);
/// assert_eq!(braid58::decode_64(&encoded[0])?, input[0]);
/// assert_eq!(braid58::decode_64(&encoded[1])?, input[1]);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
#[must_use]
pub fn encode_64x2(input: &[[u8; BINARY_64_SIZE]; 2]) -> [Encoded64; 2] {
    let mut bytes = [[0_u8; ENCODED_64_CAPACITY]; 2];
    let mut lengths = [0_usize; 2];
    // SAFETY: The pointers reference fixed-size live input and output arrays.
    unsafe {
        braid58_encode_64x2(
            input.as_ptr().cast(),
            bytes.as_mut_ptr().cast(),
            lengths.as_mut_ptr(),
        );
    }
    [
        Encoded64::from_parts(bytes[0], lengths[0]),
        Encoded64::from_parts(bytes[1], lengths[1]),
    ]
}

/// Encodes three independent 64-byte values through the fixed x3 entry point.
///
/// # Examples
///
/// ```
/// let input = [[1_u8; 64], [2_u8; 64], [3_u8; 64]];
/// let encoded = braid58::encode_64x3(&input);
/// for (text, bytes) in encoded.iter().zip(input) {
///     assert_eq!(braid58::decode_64(text)?, bytes);
/// }
/// # Ok::<(), braid58::DecodeError>(())
/// ```
#[must_use]
pub fn encode_64x3(input: &[[u8; BINARY_64_SIZE]; 3]) -> [Encoded64; 3] {
    let mut bytes = [[0_u8; ENCODED_64_CAPACITY]; 3];
    let mut lengths = [0_usize; 3];
    // SAFETY: The pointers reference fixed-size live input and output arrays.
    unsafe {
        braid58_encode_64x3(
            input.as_ptr().cast(),
            bytes.as_mut_ptr().cast(),
            lengths.as_mut_ptr(),
        );
    }
    [
        Encoded64::from_parts(bytes[0], lengths[0]),
        Encoded64::from_parts(bytes[1], lengths[1]),
        Encoded64::from_parts(bytes[2], lengths[2]),
    ]
}

/// An invalid, overflowing, or noncanonical fixed-width Base58 value.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct DecodeError;

impl fmt::Display for DecodeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("invalid fixed-width Bitcoin Base58 value")
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
///
/// # Examples
///
/// ```
/// let bytes = [42_u8; 32];
/// let encoded = braid58::encode_32(&bytes);
/// assert_eq!(braid58::decode_32(encoded)?, bytes);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
pub fn decode_32(input: impl AsRef<[u8]>) -> Result<[u8; BINARY_32_SIZE], DecodeError> {
    let mut output = [0_u8; BINARY_32_SIZE];
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
///
/// # Examples
///
/// ```
/// let bytes = [42_u8; 32];
/// let encoded = braid58::encode_32(&bytes);
/// let mut output = [0_u8; 32];
/// braid58::decode_32_into(encoded, &mut output)?;
/// assert_eq!(output, bytes);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
pub fn decode_32_into(
    input: impl AsRef<[u8]>,
    output: &mut [u8; BINARY_32_SIZE],
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

/// Decodes a canonical Bitcoin Base58 value into exactly 64 bytes.
///
/// Both `&str` and byte slices are accepted.
///
/// # Errors
///
/// Returns [`DecodeError`] for invalid, overflowing, or noncanonical input.
///
/// # Examples
///
/// ```
/// let bytes = [42_u8; 64];
/// let encoded = braid58::encode_64(&bytes);
/// assert_eq!(braid58::decode_64(encoded)?, bytes);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
pub fn decode_64(input: impl AsRef<[u8]>) -> Result<[u8; BINARY_64_SIZE], DecodeError> {
    let mut output = [0_u8; BINARY_64_SIZE];
    decode_64_into(input, &mut output)?;
    Ok(output)
}

/// Decodes into an existing 64-byte buffer.
///
/// `output` is unchanged when decoding fails.
///
/// # Errors
///
/// Returns [`DecodeError`] for invalid, overflowing, or noncanonical input.
///
/// # Examples
///
/// ```
/// let bytes = [42_u8; 64];
/// let encoded = braid58::encode_64(&bytes);
/// let mut output = [0_u8; 64];
/// braid58::decode_64_into(encoded, &mut output)?;
/// assert_eq!(output, bytes);
/// # Ok::<(), braid58::DecodeError>(())
/// ```
pub fn decode_64_into(
    input: impl AsRef<[u8]>,
    output: &mut [u8; BINARY_64_SIZE],
) -> Result<(), DecodeError> {
    let input = input.as_ref();
    // SAFETY: The pointers reference live buffers for the supplied lengths.
    let success =
        unsafe { braid58_decode_64(input.as_ptr().cast(), input.len(), output.as_mut_ptr()) };
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
    fn failure_preserves_output() {
        let mut output = [0xa5; 32];
        assert_eq!(decode_32_into("not base58", &mut output), Err(DecodeError));
        assert_eq!(output, [0xa5; 32]);

        let mut wide = [0xa5; 64];
        assert_eq!(decode_64_into("not base58", &mut wide), Err(DecodeError));
        assert_eq!(wide, [0xa5; 64]);
    }
}
