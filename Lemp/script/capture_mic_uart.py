import argparse
import struct
import sys
import time
import wave

import serial

SERIAL_PORT = "COM8"
BAUD_RATE = 1000000
SAMPLE_RATE = 16000
CHANNELS = 1
DEFAULT_BYTES = 320000
OUTPUT_FILE = "mic_test.wav"
TIMEOUT = 90.0


def read_until_marker(ser, marker: bytes, timeout_seconds: float) -> bytes:
    deadline = time.time() + timeout_seconds
    buffer = bytearray()

    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buffer.extend(chunk)
            index = buffer.find(marker)
            if index != -1:
                return bytes(buffer[index:])
    raise TimeoutError(f"Timed out waiting for marker: {marker!r}")


def print_pcm16_stats(pcm: bytes) -> None:
    sample_count = len(pcm) // 2
    if sample_count == 0:
        print("PCM stats: empty")
        return

    samples = [value[0] for value in struct.iter_unpack("<h", pcm[:sample_count * 2])]
    min_sample = min(samples)
    max_sample = max(samples)
    peak = max(abs(min_sample), abs(max_sample))
    avg_abs = sum(abs(value) for value in samples) / sample_count
    rms = (sum(value * value for value in samples) / sample_count) ** 0.5
    nonzero = sum(1 for value in samples if value != 0)

    print(f"PCM stats: samples={sample_count}, min={min_sample}, max={max_sample}, peak={peak}, avg_abs={avg_abs:.2f}, rms={rms:.2f}, nonzero={nonzero}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture UART microphone PCM and save as WAV")
    parser.add_argument("--baud", type=int, default=BAUD_RATE, help="Serial baud rate")
    parser.add_argument("--rate", type=int, default=SAMPLE_RATE, help="WAV sample rate")
    parser.add_argument("--channels", type=int, default=CHANNELS, help="WAV channels")
    parser.add_argument("--bytes", type=int, default=DEFAULT_BYTES, help="Expected PCM byte count")
    parser.add_argument("--output", default=OUTPUT_FILE, help="Output WAV path")
    parser.add_argument("--timeout", type=float, default=TIMEOUT, help="Timeout in seconds")
    args = parser.parse_args()

    print(f"Opening {SERIAL_PORT} @ {args.baud}...")
    with serial.Serial(SERIAL_PORT, args.baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        print("Waiting for AUDIO_BEGIN...")
        header = read_until_marker(ser, b"AUDIO_BEGIN", args.timeout)
        header_parts = header.split(b"\n", 1)
        header_line = header_parts[0].decode(errors="ignore").strip()
        initial_pcm = header_parts[1] if len(header_parts) > 1 else b""
        print(f"Header: {header_line}")

        expected_bytes = args.bytes
        parts = header_line.split()
        if len(parts) >= 2 and parts[1].isdigit():
            expected_bytes = int(parts[1])

        print(f"Receiving {expected_bytes} bytes of PCM...")
        pcm = bytearray(initial_pcm)
        deadline = time.time() + args.timeout
        while len(pcm) < expected_bytes and time.time() < deadline:
            chunk = ser.read(min(4096, expected_bytes - len(pcm)))
            if chunk:
                pcm.extend(chunk)

        if len(pcm) != expected_bytes:
            print(f"Receive incomplete: got {len(pcm)} / {expected_bytes} bytes", file=sys.stderr)
            return 1

        print_pcm16_stats(pcm)

        tail = ser.read(64)
        if b"AUDIO_END" not in tail:
            print("Warning: AUDIO_END not seen immediately after payload")

    print(f"Writing WAV to {args.output}...")
    with wave.open(args.output, "wb") as wav_file:
        wav_file.setnchannels(args.channels)
        wav_file.setsampwidth(2)
        wav_file.setframerate(args.rate)
        wav_file.writeframes(pcm)

    print("Done.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
