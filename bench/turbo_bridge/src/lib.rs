use base58_turbo::BITCOIN;
use core::slice;

const ERROR: usize = usize::MAX;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn turbo_base58_encode_32(input: *const u8, output: *mut u8) -> usize {
    // SAFETY: The C benchmark supplies fixed-size live buffers.
    let input = unsafe { slice::from_raw_parts(input, 32) };
    let output = unsafe { slice::from_raw_parts_mut(output, 44) };
    BITCOIN.encode_into(input, output).unwrap_or(ERROR)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn turbo_base58_encode_64(input: *const u8, output: *mut u8) -> usize {
    // SAFETY: The C benchmark supplies fixed-size live buffers.
    let input = unsafe { slice::from_raw_parts(input, 64) };
    let output = unsafe { slice::from_raw_parts_mut(output, 88) };
    BITCOIN.encode_into(input, output).unwrap_or(ERROR)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn turbo_base58_decode_32(
    input: *const u8,
    input_length: usize,
    output: *mut u8,
) -> usize {
    // Turbo's public decoder requires output capacity >= encoded input length.
    let input = unsafe { slice::from_raw_parts(input, input_length) };
    let output = unsafe { slice::from_raw_parts_mut(output, input_length) };
    BITCOIN.decode_into(input, output).unwrap_or(ERROR)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn turbo_base58_decode_64(
    input: *const u8,
    input_length: usize,
    output: *mut u8,
) -> usize {
    // Turbo's public decoder requires output capacity >= encoded input length.
    let input = unsafe { slice::from_raw_parts(input, input_length) };
    let output = unsafe { slice::from_raw_parts_mut(output, input_length) };
    BITCOIN.decode_into(input, output).unwrap_or(ERROR)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn five8_base58_encode_32(input: *const u8, output: *mut u8) -> usize {
    // SAFETY: The C benchmark supplies fixed-size live buffers.
    let input = unsafe { &*input.cast::<[u8; 32]>() };
    let output = unsafe { &mut *output.cast::<[u8; 44]>() };
    usize::from(five8::encode_32(input, output))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn five8_base58_encode_64(input: *const u8, output: *mut u8) -> usize {
    // SAFETY: The C benchmark supplies fixed-size live buffers.
    let input = unsafe { &*input.cast::<[u8; 64]>() };
    let output = unsafe { &mut *output.cast::<[u8; 88]>() };
    usize::from(five8::encode_64(input, output))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn five8_base58_decode_32(
    input: *const u8,
    input_length: usize,
    output: *mut u8,
) -> usize {
    // SAFETY: The C benchmark supplies fixed-size live buffers.
    let input = unsafe { slice::from_raw_parts(input, input_length) };
    let output = unsafe { &mut *output.cast::<[u8; 32]>() };
    five8::decode_32(input, output).map_or(ERROR, |()| 32)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn five8_base58_decode_64(
    input: *const u8,
    input_length: usize,
    output: *mut u8,
) -> usize {
    // SAFETY: The C benchmark supplies fixed-size live buffers.
    let input = unsafe { slice::from_raw_parts(input, input_length) };
    let output = unsafe { &mut *output.cast::<[u8; 64]>() };
    five8::decode_64(input, output).map_or(ERROR, |()| 64)
}
