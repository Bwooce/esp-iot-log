#!/usr/bin/env python3
"""
ESP IoT Log Receiver
Copyright (c) 2026 Bruce Fitzsimons

Cross-platform Python script to receive multicast logs from ESP devices.
Advertises mDNS service to let ESP devices know a listener is active.
"""

import sys
import time
import socket
import struct
import threading
import argparse
import json
import signal
import subprocess
import os
import platform
from datetime import datetime, timezone
from typing import Optional, Dict, Any, List

# Platform-specific mDNS imports
try:
    from zeroconf import ServiceInfo, Zeroconf, IPVersion
    HAS_ZEROCONF = True
except ImportError:
    print("Warning: zeroconf not available. Install with: pip install zeroconf")
    HAS_ZEROCONF = False

# Default configuration
DEFAULT_MULTICAST_IP = "239.255.1.100"
DEFAULT_MULTICAST_PORT = 4210
DEFAULT_SERVICE_NAME = "_esp-iot-log._udp.local."

# Protocol constants
LOG_MAGIC = 0xE510
PROTOCOL_VERSION = 1

# Log types
LOG_TYPE_TEXT = 0x01
LOG_TYPE_TELEMETRY = 0x02
LOG_TYPE_EXCEPTION = 0x03
LOG_TYPE_METRIC = 0x04

# Log levels
LOG_LEVELS = ["NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE"]

# ANSI color codes
class Colors:
    RESET = '\033[0m'
    ERROR = '\033[91m'    # Red
    WARN = '\033[93m'     # Yellow
    INFO = '\033[94m'     # Blue
    DEBUG = '\033[92m'    # Green
    VERBOSE = '\033[95m'  # Magenta
    TIMESTAMP = '\033[90m' # Gray
    DEVICE = '\033[96m'   # Cyan


class BacktraceDecoder:
    """Decodes ESP crash backtraces using addr2line."""

    # Default paths for PlatformIO toolchains
    DEFAULT_ADDR2LINE_PATHS = [
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa/bin/xtensa-lx106-elf-addr2line"),  # ESP8266
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line"),  # ESP32
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s2/bin/xtensa-esp32s2-elf-addr2line"),  # ESP32-S2
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line"),  # ESP32-S3
    ]

    def __init__(self, elf_file: Optional[str] = None, addr2line_path: Optional[str] = None):
        self.elf_file = elf_file
        self.addr2line_path = addr2line_path or self._find_addr2line()
        self._cache: Dict[int, str] = {}

    def _find_addr2line(self) -> Optional[str]:
        """Find addr2line in default locations."""
        for path in self.DEFAULT_ADDR2LINE_PATHS:
            if os.path.exists(path):
                return path
        return None

    def decode_address(self, addr: int) -> str:
        """Decode a single address to function:line."""
        if not self.elf_file or not self.addr2line_path:
            return f"0x{addr:08X}"

        if addr in self._cache:
            return self._cache[addr]

        if addr == 0:
            return "(null)"

        try:
            result = subprocess.run(
                [self.addr2line_path, "-e", self.elf_file, "-f", "-C", f"0x{addr:08X}"],
                capture_output=True, text=True, timeout=5
            )
            if result.returncode == 0:
                lines = result.stdout.strip().split('\n')
                if len(lines) >= 2:
                    func = lines[0]
                    loc = lines[1]
                    # Shorten path if it's too long
                    if '/' in loc:
                        loc = loc.split('/')[-1]
                    decoded = f"{func} at {loc}"
                    self._cache[addr] = decoded
                    return decoded
        except (subprocess.TimeoutExpired, FileNotFoundError, Exception):
            pass

        return f"0x{addr:08X}"

    def decode_backtrace(self, addresses: List[int]) -> List[str]:
        """Decode a list of backtrace addresses."""
        return [self.decode_address(addr) for addr in addresses if addr != 0]


# Global backtrace decoder (set from main)
_backtrace_decoder: Optional[BacktraceDecoder] = None


class LogMessage:
    """Represents a parsed log message from ESP device."""

    def __init__(self, header: Dict, payload: bytes, source_ip: str = None):
        self.magic = header['magic']
        self.version = header['version']
        self.device_id = header['device_id']
        self.timestamp = header['timestamp']
        self.log_type = header['log_type']
        self.length = header['length']
        self.payload = payload
        self.source_ip = source_ip
        self.parsed_payload = None
        self.received_time = datetime.now(timezone.utc)

        self._parse_payload()

    def _parse_payload(self):
        """Parse payload based on log type."""
        try:
            if self.log_type == LOG_TYPE_TEXT:
                if len(self.payload) >= 1:
                    level = self.payload[0]
                    message = self.payload[1:].decode('utf-8', errors='replace')
                    self.parsed_payload = {'level': level, 'message': message}

            elif self.log_type == LOG_TYPE_TELEMETRY:
                if len(self.payload) >= 22:  # Size of SystemTelemetry struct (22 bytes)
                    # Basic telemetry: heap_free(4) + heap_largest(4) + frag(1) + rssi(1) +
                    # status(1) + reconnects(2) + temp(2) + reset(1) + uptime(4) + stack(2)
                    tel = struct.unpack('<IIBbBHhBIH', self.payload[:22])
                    self.parsed_payload = {
                        'heap_free': tel[0],
                        'heap_largest_block': tel[1],
                        'heap_fragmentation': tel[2],
                        'wifi_rssi': tel[3] if tel[3] != 255 else None,
                        'wifi_status': tel[4],
                        'wifi_reconnects': tel[5],
                        'temperature': tel[6] / 10.0 if tel[6] != 0x7FFF else None,
                        'reset_reason': tel[7],
                        'uptime': tel[8],
                        'free_stack': tel[9]
                    }

                    # ESP-IDF Advanced telemetry (if present, ESP32 only)
                    if len(self.payload) >= 98:  # Extended telemetry size (22 + 76)
                        try:
                            # Advanced heap info
                            advanced = struct.unpack('<IIIIIIBH16s', self.payload[22:65])
                            self.parsed_payload['advanced'] = {
                                'internal_free': advanced[0],
                                'internal_largest': advanced[1],
                                'external_free': advanced[2],
                                'external_largest': advanced[3],
                                'dma_free': advanced[4],
                                'task_count': advanced[5],
                                'min_stack_remaining': advanced[6],
                                'critical_task': advanced[7].decode('utf-8', errors='replace').rstrip('\x00')
                            }

                            # WiFi advanced (16 bytes)
                            wifi_adv = struct.unpack('<BbBHB4s', self.payload[65:81])
                            self.parsed_payload['wifi_advanced'] = {
                                'channel': wifi_adv[0],
                                'tx_power': wifi_adv[1],
                                'bandwidth': wifi_adv[2],
                                'beacon_timeout': wifi_adv[3],
                                'phy_mode': wifi_adv[4],
                                'country_code': wifi_adv[5].decode('utf-8', errors='replace').rstrip('\x00')
                            }

                            # Chip info (16 bytes)
                            chip = struct.unpack('<BBBIBIBB', self.payload[81:97])
                            self.parsed_payload['chip_info'] = {
                                'model': chip[0],
                                'cores': chip[1],
                                'revision': chip[2],
                                'flash_size': chip[3],
                                'flash_mode': chip[4],
                                'cpu_freq_mhz': chip[5],
                                'secure_boot': bool(chip[6]),
                                'flash_encryption': bool(chip[7])
                            }
                        except Exception as e:
                            # If advanced parsing fails, continue with basic telemetry
                            pass

            elif self.log_type == LOG_TYPE_EXCEPTION:
                if len(self.payload) >= 72:  # Size of CrashData struct
                    crash = struct.unpack('<I I B B H I I 8I 32s B', self.payload[:72])
                    self.parsed_payload = {
                        'magic': crash[0],
                        'timestamp': crash[1],
                        'crash_type': crash[2],
                        'reset_reason': crash[3],
                        'heap_free': crash[4],
                        'stack_ptr': crash[5],
                        'pc': crash[6],
                        'backtrace': list(crash[7:15]),
                        'last_function': crash[15].decode('utf-8', errors='replace').rstrip('\x00'),
                        'checksum': crash[16]
                    }

            elif self.log_type == LOG_TYPE_METRIC:
                if len(self.payload) >= 1:
                    name_len = self.payload[0]
                    if len(self.payload) >= 1 + name_len + 4:
                        name = self.payload[1:1+name_len].decode('utf-8', errors='replace')
                        if len(self.payload) == 1 + name_len + 4:
                            # Integer metric
                            value = struct.unpack('<i', self.payload[1+name_len:1+name_len+4])[0]
                        else:
                            # String metric
                            value_len = self.payload[1+name_len]
                            value = self.payload[2+name_len:2+name_len+value_len].decode('utf-8', errors='replace')

                        self.parsed_payload = {'name': name, 'value': value}

        except Exception as e:
            print(f"Error parsing payload: {e}")
            self.parsed_payload = None

    def format_device_id(self) -> str:
        """Format device ID as MAC address."""
        return ':'.join(f'{(self.device_id >> (8*i)) & 0xFF:02X}' for i in range(6))

    def format_timestamp(self) -> str:
        """Format device timestamp (millis since boot)."""
        seconds = self.timestamp / 1000
        hours = int(seconds // 3600)
        minutes = int((seconds % 3600) // 60)
        secs = int(seconds % 60)
        millis = int(self.timestamp % 1000)
        return f"{hours:02d}:{minutes:02d}:{secs:02d}.{millis:03d}"

    def __str__(self) -> str:
        """Format log message for display."""
        device_short = self.format_device_id()[-8:]  # Last 4 bytes of MAC
        timestamp = self.format_timestamp()
        received = self.received_time.strftime("%H:%M:%S.%f")[:-3]
        ip_str = f" ({self.source_ip})" if self.source_ip else ""

        if self.log_type == LOG_TYPE_TEXT and self.parsed_payload:
            level = self.parsed_payload['level']
            level_name = LOG_LEVELS[level] if level < len(LOG_LEVELS) else f"L{level}"
            message = self.parsed_payload['message']

            # Colorize based on log level
            color = Colors.RESET
            if level == 1:  # ERROR
                color = Colors.ERROR
            elif level == 2:  # WARN
                color = Colors.WARN
            elif level == 3:  # INFO
                color = Colors.INFO
            elif level == 4:  # DEBUG
                color = Colors.DEBUG
            elif level == 5:  # VERBOSE
                color = Colors.VERBOSE

            return (f"{Colors.TIMESTAMP}{received}{Colors.RESET} "
                   f"{Colors.DEVICE}{device_short}{ip_str}{Colors.RESET} "
                   f"[{timestamp}] "
                   f"{color}[{level_name:>7s}]{Colors.RESET} "
                   f"{message}")

        elif self.log_type == LOG_TYPE_TELEMETRY and self.parsed_payload:
            tel = self.parsed_payload
            temp_str = f"{tel['temperature']:.1f}°C" if tel['temperature'] is not None else "N/A"
            wifi_str = f"{tel['wifi_rssi']}dBm" if tel['wifi_rssi'] is not None else "N/A"

            # Reset reason names for ESP8266
            reset_reasons = {
                0: "Normal",      # REASON_DEFAULT_RST
                1: "WDT",         # REASON_WDT_RST
                2: "Exception",   # REASON_EXCEPTION_RST
                3: "SoftWDT",     # REASON_SOFT_WDT_RST
                4: "SoftReset",   # REASON_SOFT_RESTART
                5: "DeepSleep",   # REASON_DEEP_SLEEP_AWAKE
                6: "ExtReset",    # REASON_EXT_SYS_RST
            }
            reset_str = reset_reasons.get(tel.get('reset_reason', 0), f"?{tel.get('reset_reason', 0)}")

            # Basic telemetry
            result = (f"{Colors.TIMESTAMP}{received}{Colors.RESET} "
                     f"{Colors.DEVICE}{device_short}{ip_str}{Colors.RESET} "
                     f"[{timestamp}] "
                     f"{Colors.INFO}[TELEMETRY]{Colors.RESET} "
                     f"Heap: {tel['heap_free']}B (frag: {tel['heap_fragmentation']}%), "
                     f"WiFi: {wifi_str}, Temp: {temp_str}, "
                     f"Stack: {tel['free_stack']}B, Uptime: {tel['uptime']/1000:.1f}s, "
                     f"Reset: {reset_str}")

            # ESP-IDF Advanced telemetry (if present)
            if 'advanced' in tel:
                adv = tel['advanced']
                result += f"\n                    Advanced: Internal: {adv['internal_free']}B, "
                if adv['external_free'] > 0:
                    result += f"PSRAM: {adv['external_free']}B, "
                result += f"Tasks: {adv['task_count']}, Critical: {adv['critical_task']}({adv['min_stack_remaining']}B)"

            if 'wifi_advanced' in tel:
                wifi_adv = tel['wifi_advanced']
                if wifi_adv['channel'] > 0:
                    result += f"\n                    WiFi: Ch{wifi_adv['channel']}, {wifi_adv['bandwidth']}MHz, {wifi_adv['tx_power']}dBm, {wifi_adv['country_code']}"

            if 'chip_info' in tel:
                chip = tel['chip_info']
                security = ""
                if chip['secure_boot']:
                    security += "+SecureBoot"
                if chip['flash_encryption']:
                    security += "+FlashEnc"
                result += f"\n                    Chip: Model{chip['model']}, {chip['cores']} cores, Rev{chip['revision']}, Flash:{chip['flash_size']//1024//1024}MB{security}"

            return result

        elif self.log_type == LOG_TYPE_EXCEPTION and self.parsed_payload:
            crash = self.parsed_payload

            # Decode PC and backtrace if decoder is available
            pc_str = f"0x{crash['pc']:08X}"
            backtrace_str = ""
            if _backtrace_decoder:
                pc_str = _backtrace_decoder.decode_address(crash['pc'])
                bt = crash.get('backtrace', [])
                if bt and any(addr != 0 for addr in bt):
                    decoded_bt = _backtrace_decoder.decode_backtrace(bt)
                    if decoded_bt:
                        backtrace_str = "\n                    Backtrace:\n"
                        for i, frame in enumerate(decoded_bt):
                            backtrace_str += f"                      #{i}: {frame}\n"

            result = (f"{Colors.TIMESTAMP}{received}{Colors.RESET} "
                   f"{Colors.DEVICE}{device_short}{ip_str}{Colors.RESET} "
                   f"[{timestamp}] "
                   f"{Colors.ERROR}[CRASH]{Colors.RESET} "
                   f"Type: {crash['crash_type']}, Reason: {crash['reset_reason']}, "
                   f"Heap: {crash['heap_free']}B, PC: {pc_str}, "
                   f"Func: {crash['last_function'] or 'unknown'}")
            return result + backtrace_str.rstrip()

        elif self.log_type == LOG_TYPE_METRIC and self.parsed_payload:
            metric = self.parsed_payload
            return (f"{Colors.TIMESTAMP}{received}{Colors.RESET} "
                   f"{Colors.DEVICE}{device_short}{ip_str}{Colors.RESET} "
                   f"[{timestamp}] "
                   f"{Colors.VERBOSE}[METRIC]{Colors.RESET} "
                   f"{metric['name']} = {metric['value']}")

        else:
            return (f"{Colors.TIMESTAMP}{received}{Colors.RESET} "
                   f"{Colors.DEVICE}{device_short}{ip_str}{Colors.RESET} "
                   f"[{timestamp}] "
                   f"[TYPE_{self.log_type}] "
                   f"Unknown payload ({len(self.payload)} bytes)")

class ESPIoTLogReceiver:
    """Main receiver class that handles mDNS advertising and UDP multicast listening."""

    def __init__(self, multicast_ip: str, port: int, service_name: str,
                 output_file: Optional[str] = None, json_output: bool = False):
        self.multicast_ip = multicast_ip
        self.port = port
        self.service_name = service_name
        self.output_file = output_file
        self.json_output = json_output
        self.running = False

        # Network objects
        self.sock = None
        self.zeroconf = None
        self.service_info = None

        # Statistics
        self.message_count = 0
        self.start_time = time.time()
        self.devices_seen = set()

        # File output
        self.file_handle = None
        if output_file:
            self.file_handle = open(output_file, 'a', encoding='utf-8')

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.stop()

    def start(self):
        """Start the receiver - advertise service and begin listening."""
        print(f"Starting ESP IoT Log Receiver...")
        print(f"Multicast: {self.multicast_ip}:{self.port}")
        print(f"Service: {self.service_name}")

        self.running = True

        # Start mDNS service advertising
        if HAS_ZEROCONF:
            try:
                self._start_mdns_service()
                print("✓ mDNS service advertised")
            except Exception as e:
                print(f"⚠ mDNS service failed: {e}")
        else:
            print("⚠ mDNS not available - devices won't auto-discover this listener")

        # Start UDP multicast listener
        try:
            self._start_udp_listener()
            print("✓ UDP multicast listener started")
        except Exception as e:
            print(f"✗ UDP listener failed: {e}")
            self.stop()
            return False

        # Log startup to output file
        if self.file_handle:
            timestamp = datetime.now(timezone.utc).strftime("%H:%M:%S.%f")[:-3]
            hostname = platform.node()

            # Build parameter info
            params = []
            if _backtrace_decoder and _backtrace_decoder.elf_file:
                params.append(f"ELF: {_backtrace_decoder.elf_file}")
            if self.output_file:
                params.append(f"Output: {self.output_file}")
            if self.json_output:
                params.append("Format: JSON")

            param_str = f" ({', '.join(params)})" if params else ""
            self.file_handle.write(f"\n{timestamp} === ESP IoT Log Receiver started on {hostname}{param_str} ===\n")
            self.file_handle.flush()

        print(f"\nListening for ESP IoT logs... (Press Ctrl+C to stop)\n")
        return True

    def stop(self):
        """Stop the receiver and clean up resources."""
        self.running = False

        if self.sock:
            self.sock.close()

        if self.zeroconf:
            self.zeroconf.unregister_service(self.service_info)
            self.zeroconf.close()

        if self.file_handle:
            self.file_handle.close()

        # Print statistics
        runtime = time.time() - self.start_time
        print(f"\n--- Session Statistics ---")
        print(f"Runtime: {runtime:.1f}s")
        print(f"Messages received: {self.message_count}")
        print(f"Devices seen: {len(self.devices_seen)}")
        if runtime > 0:
            print(f"Average rate: {self.message_count / runtime:.1f} msgs/sec")

    def _start_mdns_service(self):
        """Start mDNS service to advertise that we're listening."""
        if not HAS_ZEROCONF:
            return

        # Get local IP address
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]

        # Create service info
        self.service_info = ServiceInfo(
            "_esp-iot-log._udp.local.",
            f"ESP-IoT-Log-Receiver._esp-iot-log._udp.local.",
            addresses=[socket.inet_aton(local_ip)],
            port=self.port,
            properties={'multicast': self.multicast_ip, 'version': '1.0'},
        )

        # Register service
        self.zeroconf = Zeroconf(ip_version=IPVersion.V4Only)
        self.zeroconf.register_service(self.service_info)

    def _start_udp_listener(self):
        """Start UDP multicast socket listener."""
        # Create UDP socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        # Bind to multicast address
        self.sock.bind((self.multicast_ip, self.port))

        # Join multicast group
        mreq = struct.pack("4s4s", socket.inet_aton(self.multicast_ip), socket.inet_aton("0.0.0.0"))
        self.sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

        # Set timeout for graceful shutdown
        self.sock.settimeout(1.0)

    def listen(self):
        """Main listening loop."""
        while self.running:
            try:
                data, addr = self.sock.recvfrom(1024)
                self._handle_message(data, addr)
            except socket.timeout:
                continue  # Check running flag
            except Exception as e:
                if self.running:
                    print(f"Error receiving data: {e}")

    def _handle_message(self, data: bytes, addr):
        """Parse and display received log message."""
        try:
            # Parse header (18 bytes: magic(2) + version(1) + device_id(8) + timestamp(4) + log_type(1) + length(2))
            if len(data) < 18:  # Minimum header size
                return

            header_data = data[:18]
            magic, version, device_id, timestamp, log_type, length = struct.unpack('<HBQIBH', header_data)

            if magic != LOG_MAGIC:
                return  # Not our protocol

            header = {
                'magic': magic,
                'version': version,
                'device_id': device_id,
                'timestamp': timestamp,
                'log_type': log_type,
                'length': length
            }

            # Extract payload and verify checksum
            payload_end = 18 + length
            if len(data) < payload_end + 2:  # +2 for checksum
                return

            payload = data[18:payload_end]
            checksum = struct.unpack('<H', data[payload_end:payload_end+2])[0]

            # TODO: Verify checksum if needed

            # Create log message and display (extract IP from addr tuple)
            source_ip = addr[0] if addr else None
            log_msg = LogMessage(header, payload, source_ip)

            # Track statistics
            self.message_count += 1
            self.devices_seen.add(log_msg.device_id)

            # Output message
            if self.json_output:
                self._output_json(log_msg)
            else:
                print(log_msg)

            # Write to file if specified
            if self.file_handle:
                self.file_handle.write(str(log_msg) + '\n')
                self.file_handle.flush()

        except Exception as e:
            print(f"Error parsing message: {e}")

    def _output_json(self, log_msg: LogMessage):
        """Output log message in JSON format."""
        json_data = {
            'timestamp': log_msg.received_time.isoformat(),
            'device_id': log_msg.format_device_id(),
            'source_ip': log_msg.source_ip,
            'device_timestamp': log_msg.timestamp,
            'log_type': log_msg.log_type,
            'payload': log_msg.parsed_payload
        }
        print(json.dumps(json_data))

def signal_handler(signum, frame):
    """Handle Ctrl+C gracefully."""
    print("\nShutting down...")
    sys.exit(0)

def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(description='ESP IoT Log Receiver')
    parser.add_argument('--ip', default=DEFAULT_MULTICAST_IP,
                       help=f'Multicast IP address (default: {DEFAULT_MULTICAST_IP})')
    parser.add_argument('--port', type=int, default=DEFAULT_MULTICAST_PORT,
                       help=f'UDP port (default: {DEFAULT_MULTICAST_PORT})')
    parser.add_argument('--service', default=DEFAULT_SERVICE_NAME,
                       help=f'mDNS service name (default: {DEFAULT_SERVICE_NAME})')
    parser.add_argument('--output', '-o', help='Output file for logs')
    parser.add_argument('--json', action='store_true',
                       help='Output in JSON format')
    parser.add_argument('--no-color', action='store_true',
                       help='Disable colored output')
    parser.add_argument('--elf', '-e', help='ELF file for backtrace decoding')
    parser.add_argument('--addr2line', help='Path to addr2line tool (auto-detected if not specified)')

    args = parser.parse_args()

    # Disable colors if requested or output is not a TTY
    if args.no_color or not sys.stdout.isatty():
        for attr in dir(Colors):
            if not attr.startswith('_'):
                setattr(Colors, attr, '')

    # Set up backtrace decoder
    global _backtrace_decoder
    _backtrace_decoder = BacktraceDecoder(args.elf, args.addr2line)
    if args.elf:
        if _backtrace_decoder.addr2line_path:
            print(f"Backtrace decoding enabled: {args.elf}")
        else:
            print("Warning: ELF file specified but addr2line not found")

    # Set up signal handler
    signal.signal(signal.SIGINT, signal_handler)

    # Start receiver
    with ESPIoTLogReceiver(args.ip, args.port, args.service, args.output, args.json) as receiver:
        if receiver.start():
            try:
                receiver.listen()
            except KeyboardInterrupt:
                pass

if __name__ == '__main__':
    main()