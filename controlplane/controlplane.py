import os
import sys
import json
import socket
import struct
import signal
import select
import subprocess
import time
from collections import deque
from dataclasses import dataclass
from typing import Optional, Tuple, Dict, List
from pathlib import Path

# Protocol constants
MAX_RETRIES = 10
ACK_TIMEOUT_S = 1.0
MAX_PACKET_SIZE = 4096

# Status codes for ACKs
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


@dataclass
class PendingAck:
    packet: bytes
    dest: Tuple[str, int]
    retries: int
    timestamp: float
    seq: int

# TODO: make this MUCH better, this is hacky garbage


class ReliableUDPSocket:
    """
    UDP socket with reliable acknowledgment protocol.
    Format: [opcode:1][seq:4][payload...]
    Ack format: [0xFF:1][seq:4][status:1]
    """

    def __init__(self, port: int):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", port))
        self.sock.setblocking(False)
        self.pending_acks: deque[PendingAck] = deque()

    def send_with_ack(self, opcode: int, seq: int, payload: bytes,
                      dest: Tuple[str, int]) -> None:
        """Send a packet that requires acknowledgment."""
        packet = struct.pack("!BI", opcode, seq) + payload

        pending = PendingAck(
            packet=packet,
            dest=dest,
            retries=0,
            timestamp=time.monotonic(),
            seq=seq
        )
        self.pending_acks.append(pending)
        self._try_send(pending)

    def send_ack(self, seq: int, status: int, dest: Tuple[str, int]) -> None:
        """Send an acknowledgment with status code."""
        ack_packet = struct.pack("!BIB", 0xFF, seq, status)
        try:
            self.sock.sendto(ack_packet, dest)
        except OSError:
            pass

    def _try_send(self, pending: PendingAck) -> bool:
        try:
            self.sock.sendto(pending.packet, pending.dest)
            return True
        except OSError:
            return False

    def process_acks(self) -> None:
        now = time.monotonic()
        still_pending: deque[PendingAck] = deque()

        for pending in self.pending_acks:
            elapsed = now - pending.timestamp

            if elapsed > ACK_TIMEOUT_S:
                pending.retries += 1
                pending.timestamp = now

                if pending.retries > MAX_RETRIES:
                    raise RuntimeError(
                        f"Failed to receive acknowledgment after {MAX_RETRIES} retries"
                    )

                self._try_send(pending)
                still_pending.append(pending)
            else:
                still_pending.append(pending)

        self.pending_acks = still_pending

    def handle_ack(self, seq: int) -> bool:
        for i, pending in enumerate(self.pending_acks):
            if pending.seq == seq:
                self.pending_acks = deque(
                    p for j, p in enumerate(self.pending_acks) if j != i
                )
                return True
        return False

    def receive(self) -> Optional[Tuple[bytes, Tuple[str, int]]]:
        try:
            data, addr = self.sock.recvfrom(MAX_PACKET_SIZE)
            return data, addr
        except BlockingIOError:
            return None
        except OSError:
            return None


class TestProcessManager:
    def __init__(self):
        self.current_proc: Optional[subprocess.Popen] = None
        self.test_exit_status: Optional[int] = None
        self._exit_pipe_r: Optional[int] = None
        self._exit_pipe_w: Optional[int] = None
        self._setup_sigchld_handler()

    def _setup_sigchld_handler(self) -> None:
        self._exit_pipe_r, self._exit_pipe_w = os.pipe()

        import fcntl
        flags = fcntl.fcntl(self._exit_pipe_r, fcntl.F_GETFL)
        fcntl.fcntl(self._exit_pipe_r, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        def sigchld_handler(signum, frame):
            try:
                os.write(self._exit_pipe_w, b'\x00')
            except OSError:
                pass

        signal.signal(signal.SIGCHLD, sigchld_handler)

    def get_exit_pipe_fd(self) -> int:
        return self._exit_pipe_r

    def handle_sigchld(self) -> bool:
        try:
            while os.read(self._exit_pipe_r, 256):
                pass
        except BlockingIOError:
            pass

        if self.current_proc is None:
            return False

        ret = self.current_proc.poll()
        if ret is not None:
            print("Child died!")
            self.test_exit_status = ret
            self.current_proc = None
            return True

        while True:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
                if pid == 0:
                    break
            except ChildProcessError:
                break

        return False

    def start_test(self, test_dir: str, args: List[str]) -> bool:
        print("Stopping test...")
        self.stop_test()

        run_script = Path(test_dir) / "run.sh"
        print(f"running {run_script}")
        if not run_script.exists():
            print("\tscript doesn't exist!")
            return False

        cmd = [str(run_script)] + args

        try:
            self.current_proc = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True
            )
            return True
        except OSError as e:
            print(e)
            return False

    def stop_test(self) -> None:
        if self.current_proc is None:
            return

        try:
            os.killpg(os.getpgid(self.current_proc.pid), signal.SIGTERM)

            try:
                self.current_proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self.current_proc.pid), signal.SIGKILL)
                self.current_proc.wait()
        except (ProcessLookupError, OSError):
            pass

        self.current_proc = None
        self.test_exit_status = None

    def is_running(self) -> bool:
        if self.current_proc is None:
            return False
        return self.current_proc.poll() is None


class ControlSocket:
    def __init__(self, port: int, manifest: dict):
        self.port = port
        self.manifest = manifest
        self.udp = ReliableUDPSocket(port)
        self.proc_manager = TestProcessManager()

        self.opcodes: Dict[int, ControlOp] = {}
        for opcode_str, spec in manifest["opcodes"].items():
            opcode = int(opcode_str)
            self.opcodes[opcode] = ControlOp(
                opcode=opcode,
                name=spec[0],
                num_args=int(spec[1])
            )

        self.tests: Dict[str, str] = {}
        print("### TESTS AVAILABLE ###")
        for test_entry in manifest["tests"]:
            name, path = test_entry
            print(f"\t{name} @ {path}")
            self.tests[name] = path

        self.controller_addr: Optional[Tuple[str, int]] = None
        self.ops_queue: deque[Tuple[ControlOp, List[str], Tuple[str, int], int]] = deque()
        self.next_seq = 0

    def _parse_packet(self, data: bytes) -> Optional[Tuple[int, int, List[str]]]:
        if len(data) < 5:
            return None

        opcode = data[0]
        seq = struct.unpack("!I", data[1:5])[0]

        if opcode == 0xFF:
            # Handle ACK with status: [0xFF:1][seq:4][status:1]
            if len(data) >= 6:
                status = data[5]
                # For now we just care about ack matching, status handled elsewhere
                pass
            self.udp.handle_ack(seq)
            return None

        if opcode not in self.opcodes:
            return None

        op = self.opcodes[opcode]
        args: List[str] = []

        idx = 5
        for _ in range(op.num_args):
            if idx + 2 > len(data):
                return None
            arg_len = struct.unpack("!H", data[idx:idx+2])[0]
            idx += 2

            if idx + arg_len > len(data):
                return None
            arg = data[idx:idx+arg_len].decode('utf-8', errors='replace')
            args.append(arg)
            idx += arg_len

        return opcode, seq, args

    def listen(self) -> None:
        fds = [self.udp.sock.fileno(), self.proc_manager.get_exit_pipe_fd()]

        readable, _, _ = select.select(fds, [], [], None)

        if self.proc_manager.get_exit_pipe_fd() in readable:
            if self.proc_manager.handle_sigchld():
                if self.controller_addr:
                    self._queue_test_exit_notification()

        if self.udp.sock.fileno() in readable:
            result = self.udp.receive()
            if result:
                data, addr = result
                parsed = self._parse_packet(data)
                if parsed:
                    opcode, seq, args = parsed
                    self._handle_operation(opcode, seq, args, addr)

        self.udp.process_acks()

    def _handle_operation(self, opcode: int, seq: int, args: List[str],
                          addr: Tuple[str, int]) -> None:
        op = self.opcodes[opcode]
        self.controller_addr = addr

        # Execute immediately to determine status, then ack
        status = STATUS_OK

        if op.opcode == OP_START_TEST:
            if len(args) >= 1:
                test_name = args[0]
                test_args = args[1:] if len(args) > 1 else []

                if self.proc_manager.is_running():
                    print(f"Already a test running!", file=sys.stderr)
                    status = STATUS_BUSY
                else:
                    if test_name not in self.tests:
                        print(f"Invalid test name: {test_name}", file=sys.stderr)
                        status = STATUS_INVALID_TEST
                    elif not self.proc_manager.start_test(self.tests[test_name], test_args):
                        print(f"Failed to start test: {test_name}", file=sys.stderr)
                        status = STATUS_EXEC_FAILED
                    else:
                        print(f"Started test: {test_name}", file=sys.stderr)
            else:
                print("start_test missing test name", file=sys.stderr)
                status = STATUS_INVALID_TEST

        elif op.opcode == OP_STOP:
            self.proc_manager.stop_test()
            print("Stopped current test", file=sys.stderr)

        elif op.opcode == OP_HELLO:
            print("Hello received", file=sys.stderr)

        else:
            status = STATUS_UNKNOWN_ERROR

        # Send acknowledgment with status
        self.udp.send_ack(seq, status, addr)

        # Queue notification ops for main loop handling
        if op.opcode == OP_TEST_EXIT:
            self.ops_queue.append((op, args, addr, seq))

    def _queue_test_exit_notification(self) -> None:
        status = self.proc_manager.test_exit_status or 0
        exit_op = ControlOp(OP_TEST_EXIT, "test_exit", 1)
        self.ops_queue.append((exit_op, [str(status)], self.controller_addr, 0))

    def get_next_op(self) -> Optional[Tuple[ControlOp, List[str], Tuple[str, int]]]:
        if self.ops_queue:
            op, args, addr, _ = self.ops_queue.popleft()
            return op, args, addr
        return None

    def has_pending_ops(self) -> bool:
        return len(self.ops_queue) > 0

    def inform_test_done(self, exit_status: int = 0) -> None:
        if self.controller_addr is None:
            return

        self.next_seq += 1
        seq = self.next_seq

        status_bytes = str(exit_status).encode('utf-8')
        payload = struct.pack("!H", len(status_bytes)) + status_bytes

        self.udp.send_with_ack(OP_TEST_EXIT, seq, payload, self.controller_addr)


def main(port: int):
    manifest_path = Path(__file__).parent / "experiment_manifest.json"
    with open(manifest_path, mode='r') as f:
        manifest = json.load(f)

    cs = ControlSocket(port, manifest)

    print(f"Control plane listening on UDP port {port}", file=sys.stderr)

    while True:
        cs.listen()

        while cs.has_pending_ops():
            op, args, addr = cs.get_next_op()

            if op.opcode == OP_TEST_EXIT:
                status = int(args[0]) if args else 0
                print(f"Notifying controller of test exit, status={status}", file=sys.stderr)
                cs.inform_test_done(status)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise RuntimeError("Must specify a control port")
    port = int(sys.argv[1])
    main(port)

