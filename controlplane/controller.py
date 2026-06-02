import sys
import socket
import struct
import select
import json
import time
import readline  # Built-in history support
from dataclasses import dataclass
from typing import Optional, Tuple, List, Dict
from pathlib import Path

# TODO: make this a module and commonise...
# Protocol constants
MAX_PACKET_SIZE = 4096
ACK_TIMEOUT_S = 1.0
RECV_TIMEOUT_S = 0.1

# Status codes
STATUS_OK = 0
STATUS_INVALID_TEST = 1
STATUS_EXEC_FAILED = 2
STATUS_BUSY = 3
STATUS_UNKNOWN_ERROR = 255

# Opcode constants
OP_HELLO = 1
OP_START_TEST = 2
OP_STOP = 3
OP_TEST_EXIT = 4


@dataclass
class ControlOp:
    opcode: int
    name: str
    num_args: int


class Controller:
    def __init__(self, target_host: str, target_port: int, manifest: dict):
        self.target = (target_host, target_port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(RECV_TIMEOUT_S)

        self.opcodes_by_name: Dict[str, ControlOp] = {}
        for opcode_str, spec in manifest["opcodes"].items():
            opcode = int(opcode_str)
            op = ControlOp(opcode, spec[0], int(spec[1]))
            self.opcodes_by_name[op.name] = op

        self.seq_counter = 0

    def _build_packet(self, opcode: int, args: List[str], seq: int) -> bytes:
        data = struct.pack("!BI", opcode, seq)
        for arg in args:
            arg_bytes = arg.encode('utf-8')
            data += struct.pack("!H", len(arg_bytes)) + arg_bytes
        return data

    def _parse_packet(self, data: bytes) -> Optional[Tuple[bool, int, int, int, List[str]]]:
        if len(data) < 5:
            return None

        first_byte = data[0]
        seq = struct.unpack("!I", data[1:5])[0]

        if first_byte == 0xFF:
            status = data[5] if len(data) >= 6 else STATUS_UNKNOWN_ERROR
            return (True, 0, seq, status, [])

        opcode = first_byte
        args = []
        idx = 5

        while idx < len(data):
            if idx + 2 > len(data):
                break
            arg_len = struct.unpack("!H", data[idx:idx+2])[0]
            idx += 2
            if idx + arg_len > len(data):
                break
            arg = data[idx:idx+arg_len].decode('utf-8', errors='replace')
            args.append(arg)
            idx += arg_len

        return (False, opcode, seq, 0, args)

    def _status_to_str(self, status: int) -> str:
        if status == STATUS_OK:
            return "OK"
        elif status == STATUS_INVALID_TEST:
            return "INVALID_TEST"
        elif status == STATUS_EXEC_FAILED:
            return "EXEC_FAILED"
        elif status == STATUS_BUSY:
            return "BUSY"
        else:
            return f"ERROR({status})"

    def send_op(self, op_name: str, args: List[str]) -> Tuple[bool, int]:
        if op_name not in self.opcodes_by_name:
            print(f"Unknown operation: {op_name}")
            return (False, -1)

        op = self.opcodes_by_name[op_name]
        if len(args) < op.num_args:
            print(f"Too few arguments for {op_name} (need {op.num_args})")
            return (False, -1)

        args = args[:op.num_args] if op.num_args > 0 else []

        self.seq_counter += 1
        seq = self.seq_counter
        packet = self._build_packet(op.opcode, args, seq)

        for attempt in range(10):
            self.sock.sendto(packet, self.target)

            deadline = time.monotonic() + ACK_TIMEOUT_S
            while time.monotonic() < deadline:
                ready, _, _ = select.select([self.sock], [], [], 0.05)
                if ready:
                    try:
                        data, addr = self.sock.recvfrom(MAX_PACKET_SIZE)
                        parsed = self._parse_packet(data)
                        if parsed and parsed[0] and parsed[2] == seq:
                            return (True, parsed[3])
                        elif parsed and not parsed[0]:
                            self._handle_incoming(parsed, addr)
                    except (socket.timeout, OSError):
                        pass

            print(f"  Retry {attempt + 1}...", file=sys.stderr)

        return (False, -1)

    def _handle_incoming(self, parsed: Tuple[bool, int, int, int, List[str]],
                         addr: Tuple[str, int]) -> None:
        is_ack, opcode, seq, status, args = parsed
        if is_ack:
            return

        ack_packet = struct.pack("!BIB", 0xFF, seq, STATUS_OK)
        self.sock.sendto(ack_packet, addr)

        if opcode == OP_TEST_EXIT:
            if len(args) >= 1:
                print(f"\n[CONTROL PLANE] Test exited with status: {args[0]}")
            else:
                print(f"\n[CONTROL PLANE] Test exited")

    def check_for_messages(self) -> None:
        ready, _, _ = select.select([self.sock], [], [], 0)
        if ready:
            try:
                data, addr = self.sock.recvfrom(MAX_PACKET_SIZE)
                parsed = self._parse_packet(data)
                if parsed:
                    self._handle_incoming(parsed, addr)
            except (socket.timeout, OSError):
                pass


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <host> <port>")
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2])

    manifest_path = Path(__file__).parent / "experiment_manifest.json"
    with open(manifest_path) as f:
        manifest = json.load(f)

    ctrl = Controller(host, port, manifest)

    # Setup readline history
    histfile = Path.home() / ".controller_history"
    try:
        readline.read_history_file(histfile)
    except FileNotFoundError:
        pass
    import atexit
    atexit.register(readline.write_history_file, histfile)

    print("Available commands:")
    for name, op in sorted(ctrl.opcodes_by_name.items()):
        print(f"  {name} [{op.num_args} arg(s)]")
    print("Type 'quit' to exit")
    print()

    while True:
        try:
            ctrl.check_for_messages()

            line = input("> ").strip()
            if not line:
                continue

            parts = line.split()
            cmd = parts[0]
            args = parts[1:]

            if cmd == "quit":
                break

            success, status = ctrl.send_op(cmd, args)
            if status == -1:
                continue

            if success:
                print(f"ACK {ctrl._status_to_str(status)}")
            else:
                print("ACK TIMEOUT")

        except KeyboardInterrupt:
            break
        except EOFError:
            break

    print("Exiting.")


if __name__ == "__main__":
    main()

