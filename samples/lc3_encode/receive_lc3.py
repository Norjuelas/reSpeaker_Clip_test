#!/usr/bin/env python3
"""
LC3 Audio Receiver for reSpeaker Clip

Receives LC3 encoded audio data from UART and decodes it to WAV file.
Supports multiple start/stop sessions, auto-names files by datetime.

Usage:
    python3 receive_lc3.py /dev/ttyACM0 921600 . --mode mono     # Left channel only
    python3 receive_lc3.py /dev/ttyACM0 921600 . --mode stereo   # Stereo output
    python3 receive_lc3.py /dev/ttyACM0 921600 . --mode merge    # Mix L+R to mono

Requires: pyserial, lc3py (pip install lc3py)
          If lc3 module is not available, raw .lc3 frames are saved instead.
"""

import serial
import sys
import wave
import os
import time
import struct
import array
import argparse
from datetime import datetime

try:
    import lc3
    HAS_LC3 = True
except ImportError:
    HAS_LC3 = False
    print("Note: 'lc3' module not found. Install with: pip install lc3py")
    print("      Falling back to raw LC3 frame saving (.lc3 files)")


class LC3UARTReceiver:
    def __init__(self, port="/dev/ttyACM0", baudrate=921600, output_dir=".", mode="stereo"):
        self.ser = serial.Serial(port, baudrate, timeout=1)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        self.sample_rate = 16000
        self.device_channels = 2
        self.frame_size = 160        # samples per channel
        self.frame_duration_us = 10000
        self.bitrate = 64000         # total bitrate (per-ch * channels)
        self.output_dir = output_dir
        self.mode = mode
        self.decoder = None
        self.frame_bytes_per_ch = 0  # LC3 frame size in bytes per channel
        self.session_count = 0
        self.frames_pcm = []         # list of int16 PCM bytes per frame
        self.raw_frames = []         # list of raw LC3 bytes
        self.should_stop = False

        # Map mode to device command
        self.mode_cmd = {
            "mono": "1",
            "stereo": "2",
            "merge": "3"
        }.get(mode, "2")

        print(f"Mode: {mode.upper()} (will send command '{self.mode_cmd}' to device)")

    def send_cmd(self, cmd):
        """Send command to device"""
        self.ser.write((cmd + '\n').encode())
        self.ser.flush()
        time.sleep(0.05)

    def parse_header(self, line):
        """Parse header line"""
        if "=" in line:
            key, value = line.strip().split("=", 1)
            if key == "SAMPLE_RATE":
                self.sample_rate = int(value)
            elif key == "CHANNELS":
                self.device_channels = int(value)
            elif key == "FRAME_SIZE":
                self.frame_size = int(value)
            elif key == "FRAME_DURATION_US":
                self.frame_duration_us = int(value)
            elif key == "BITRATE":
                self.bitrate = int(value)

    def init_decoder(self):
        """Initialize LC3 decoder"""
        if self.decoder is not None:
            return

        if not HAS_LC3:
            print("LC3 decoder not available, saving raw frames")
            return

        print("Initializing LC3 decoder:")
        print(f"  Sample Rate: {self.sample_rate} Hz")
        print(f"  Channels: {self.device_channels}")
        print(f"  Frame Size: {self.frame_size} samples")
        print(f"  Frame Duration: {self.frame_duration_us} us")
        print(f"  Bitrate: {self.bitrate} bps")

        # Create decoder instances (one per channel)
        self.decoder = []
        for ch in range(self.device_channels):
            dec = lc3.Decoder(self.frame_duration_us, self.sample_rate)
            self.decoder.append(dec)

        # Calculate per-channel frame bytes for splitting stereo data
        # LC3 is CBR: frame_bytes = bitrate_per_ch * frame_duration / 8
        bitrate_per_ch = self.bitrate // self.device_channels
        self.frame_bytes_per_ch = bitrate_per_ch * (self.frame_duration_us // 1000) // 8
        # More precise: use decoder's get_frame_bytes
        self.frame_bytes_per_ch = self.decoder[0].get_frame_bytes(bitrate_per_ch)
        print(f"  Frame bytes per channel: {self.frame_bytes_per_ch}")

    def float_to_int16_bytes(self, float_samples):
        """Convert float PCM samples (-1.0 ~ 1.0) to signed 16-bit bytes"""
        int16_arr = array.array('h', [
            max(-32768, min(32767, int(s * 32767)))
            for s in float_samples
        ])
        return int16_arr.tobytes()

    def decode_frame(self, lc3_data):
        """Decode LC3 frame(s). For stereo, data contains two frames concatenated."""
        if not HAS_LC3 or not self.decoder:
            return None

        decoded_channels = []
        offset = 0

        for ch in range(self.device_channels):
            ch_data = lc3_data[offset:offset + self.frame_bytes_per_ch]
            if len(ch_data) < self.frame_bytes_per_ch:
                return None

            # Decode returns array.array('f') with float samples (-1.0 to 1.0)
            pcm_float = self.decoder[ch].decode(ch_data)
            decoded_channels.append(pcm_float)
            offset += self.frame_bytes_per_ch

        # Convert float to int16 and interleave for stereo
        if self.device_channels == 2:
            ch0 = decoded_channels[0]
            ch1 = decoded_channels[1]
            interleaved = bytearray()
            for i in range(len(ch0)):
                l = max(-32768, min(32767, int(ch0[i] * 32767)))
                r = max(-32768, min(32767, int(ch1[i] * 32767)))
                interleaved += struct.pack('<h', l)
                interleaved += struct.pack('<h', r)
            return bytes(interleaved)
        else:
            return self.float_to_int16_bytes(decoded_channels[0])

    def generate_filename(self, ext=None):
        """Generate filename with datetime and mode"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        if ext is None:
            ext = "wav" if HAS_LC3 else "lc3"
        return os.path.join(self.output_dir, f"recording_{timestamp}.{ext}")

    def wait_for_data_start(self):
        """Wait for DATA_START marker"""
        print("Waiting for data start...")
        start_time = time.time()
        while time.time() - start_time < 5 and not self.should_stop:
            line = self.ser.readline().decode('utf-8', errors='ignore').strip()
            if line:
                print(f"RX: {line}")
            if line.startswith(">>> DATA_START"):
                self.init_decoder()
                return True
            elif "=" in line:
                self.parse_header(line)
        return not self.should_stop

    def receive_one_session(self):
        """Receive one recording session"""
        self.frames_pcm = []
        self.raw_frames = []
        frame_count = 0

        # Wait for DATA_START
        if not self.wait_for_data_start():
            if self.should_stop:
                print("\nStopping...")
            else:
                print("Timeout waiting for data start")
            return False

        print("Receiving encoded frames... (press Ctrl+C to stop)")

        # Read frames until DATA_END or interrupted
        while not self.should_stop:
            line = self.ser.readline().decode('utf-8', errors='ignore').strip()

            if not line:
                continue

            if line.startswith(">>> DATA_END"):
                print(f"\nSession ended. Received {frame_count} frames")
                break

            # Parse frame data: <4hex length>\n<hex data>\n
            if len(line) == 4:
                try:
                    frame_len = int(line, 16)
                    if frame_len > 0 and frame_len < 4000:
                        # Read hex data
                        hex_line = self.ser.readline().decode('utf-8', errors='ignore').strip()
                        lc3_data = bytes.fromhex(hex_line)

                        if len(lc3_data) == frame_len:
                            # Store raw frame
                            self.raw_frames.append(lc3_data)

                            # Try to decode
                            if HAS_LC3 and self.decoder:
                                try:
                                    pcm_bytes = self.decode_frame(lc3_data)
                                    if pcm_bytes:
                                        self.frames_pcm.append(pcm_bytes)
                                except Exception as e:
                                    print(f"\nDecode error: {e}")

                            frame_count += 1

                            if frame_count % 100 == 0:
                                print(f"\rReceived {frame_count} frames", end='', flush=True)
                except ValueError:
                    continue

        # Save output
        print(f"\nFrames collected: {len(self.raw_frames)}")
        if HAS_LC3 and self.frames_pcm:
            try:
                output_file = self.generate_filename("wav")
                self.save_wav(self.frames_pcm, output_file)
                self.session_count += 1
                return True
            except Exception as e:
                print(f"Error saving WAV: {e}")
                import traceback
                traceback.print_exc()
                # Fall back to raw save
        elif self.raw_frames:
            output_file = self.generate_filename("lc3")
            self.save_raw(self.raw_frames, output_file)
            self.session_count += 1
            return True
        else:
            print("No data received in this session")
            return False

        return False

    def save_wav(self, frames_pcm, output_file):
        """Save decoded PCM data to WAV file"""
        if not frames_pcm:
            print("No audio data to save")
            return

        # Combine all frames
        pcm_data = b''.join(frames_pcm)

        # Create WAV file
        with wave.open(output_file, 'wb') as wav_file:
            wav_file.setnchannels(self.device_channels)
            wav_file.setsampwidth(2)  # 16-bit
            wav_file.setframerate(self.sample_rate)
            wav_file.writeframes(pcm_data)

        total_samples = len(pcm_data) // 2
        duration = total_samples / (self.sample_rate * self.device_channels)
        print(f"\n\n*** Saved to {output_file} ***")
        print(f"Channels: {self.device_channels}")
        print(f"Duration: {duration:.2f} seconds")
        print(f"Samples: {total_samples}")

    def save_raw(self, raw_frames, output_file):
        """Save raw LC3 frames to file with metadata header"""
        with open(output_file, 'wb') as f:
            # Write header: magic, sample_rate, channels, frame_duration_us, frame_count
            f.write(b'LC3R')  # Magic
            f.write(struct.pack('<I', self.sample_rate))
            f.write(struct.pack('<B', self.device_channels))
            f.write(struct.pack('<I', self.frame_duration_us))
            f.write(struct.pack('<I', len(raw_frames)))
            # Write each frame: [2-byte length][data]
            for frame in raw_frames:
                f.write(struct.pack('<H', len(frame)))
                f.write(frame)

        total_bytes = sum(len(fr) for fr in raw_frames)
        print(f"\n\n*** Saved to {output_file} ***")
        print(f"Format: Raw LC3 frames")
        print(f"Frames: {len(raw_frames)}")
        print(f"Total bytes: {total_bytes}")
        if self.frame_duration_us > 0:
            duration = len(raw_frames) * (self.frame_duration_us / 1000000)
            print(f"Duration: {duration:.2f} seconds")

    def signal_handler(self, signum, frame):
        """Handle interrupt signals"""
        print("\n\nInterrupt signal received...")
        self.should_stop = True

    def run(self):
        """Main loop: support multiple recording sessions"""
        print(f"LC3 UART Receiver")
        print(f"Port: {self.ser.port}")
        print(f"Baudrate: {self.ser.baudrate}")
        print(f"Output directory: {os.path.abspath(self.output_dir)}")
        print(f"Mode: {self.mode}")
        print(f"Decoder: {'LC3 -> WAV' if HAS_LC3 else 'Raw .lc3 frames'}")

        # Setup signal handlers
        import signal
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

        try:
            while not self.should_stop:
                # Send 'e' to ensure device is stopped
                print("\n" + "="*50)
                self.send_cmd('e')

                # Clear any pending data
                self.ser.reset_input_buffer()

                # Send mode command to device
                print(f"Sending mode command: {self.mode_cmd}")
                self.send_cmd(self.mode_cmd)

                # Wait for device to reinitialize encoder
                time.sleep(0.5)

                # Send 's' to start recording
                self.send_cmd('s')

                # Receive data
                success = self.receive_one_session()
                print(f"Session result: {success}, Total sessions: {self.session_count}")

                if not success and not self.should_stop:
                    print("Failed to receive data. Check device connection.")
                    break

                if self.should_stop:
                    break

                # Ask user if continue
                try:
                    input("\nPress Enter for next recording, or Ctrl+C to exit...")
                except KeyboardInterrupt:
                    self.should_stop = True
                    break

        finally:
            print("\n\nStopping...")
            self.send_cmd('e')
            print("Sent stop command")

            # Save any remaining data
            if self.raw_frames:
                print("Saving remaining data...")
                self.save_raw(self.raw_frames, self.generate_filename("lc3"))

    def close(self):
        if self.ser.is_open:
            self.ser.close()


def main():
    parser = argparse.ArgumentParser(
        description='LC3 Audio Receiver for reSpeaker Clip',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Audio modes:
  mono     - Use left channel only (single microphone)
  stereo   - Keep stereo output (left + right microphones)
  merge    - Mix both channels to mono (blended microphone sound)

Output format:
  If the 'lc3' module is installed (pip install lc3py), decoded WAV files.
  Otherwise, raw LC3 frames are saved in a custom .lc3 format.

Examples:
  %(prog)s /dev/ttyACM0 921600 . --mode mono
  %(prog)s /dev/ttyACM0 921600 recordings --mode merge
        '''
    )

    parser.add_argument('port', nargs='?', default='/dev/ttyACM0',
                        help='Serial port (default: /dev/ttyACM0)')
    parser.add_argument('baudrate', nargs='?', type=int, default=921600,
                        help='Baudrate (default: 921600)')
    parser.add_argument('output_dir', nargs='?', default='.',
                        help='Output directory (default: current directory)')
    parser.add_argument('--mode', choices=['mono', 'stereo', 'merge'],
                        default='stereo',
                        help='Audio mode (default: stereo)')

    args = parser.parse_args()

    receiver = LC3UARTReceiver(args.port, args.baudrate, args.output_dir, args.mode)

    try:
        receiver.run()
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
    finally:
        receiver.close()


if __name__ == "__main__":
    main()
