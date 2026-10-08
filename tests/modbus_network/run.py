"""Loopback fixture for the unchanged Modbus UI/worker; no LAN or tablet needed."""
import argparse, importlib.util, json, pathlib, socket, struct, subprocess, threading
root = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('fixture', root / 'tools/modbus_test_server.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)
exe = root / 'build/modbus-network/out/network_test.exe'
output = root / 'build/modbus-network'

def run_case(mode):
    server = socket.socket()
    server.bind(('127.0.0.1', 0))
    port = server.getsockname()[1]
    stop = threading.Event()
    requests = []
    errors = []
    disconnects = []
    connections = []
    if mode == 'closed':
        server.close()
    else:
        server.listen(4)
        server.settimeout(0.1)

    def serve():
        while not stop.is_set():
            try:
                connection, _ = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            connections.append(True)
            with connection:
                connection.settimeout(2)
                try:
                    request = fixture.read_exact(connection, 12)
                    requests.append(request)
                    assert len(request) == 12
                    assert struct.unpack_from('>HH', request, 2) == (0, 6)
                    assert request[6] == 1 and request[7] in (1, 2, 3, 4)
                    address, count = struct.unpack_from('>HH', request, 8)
                    expected_range = (10, 16)
                    if mode == 'views':
                        expected_range = [(10, 16), (65533, 3), (65535, 1), (10, 16)][len(requests) - 1]
                        assert request[7] == [3, 4, 4, 1][len(requests) - 1]
                    assert (address, count) == expected_range
                    reply = fixture.response(request, 2 if mode == 'exception' else 0)
                    if mode == 'views' and request[7] >= 3:
                        words = ([0x3f80, 0, 0x8000, 0, 0x7f80, 0, 0xff80, 0,
                                  0x7fc0, 0, 0, 1, 0x7f7f, 0xffff, 0xffff, 0xffff] if count == 16 else
                                 [0x47f1, 0x2000, 0xabcd] if count == 3 else [0xabcd])
                        reply = reply[:9] + struct.pack('>' + 'H' * count, *words)
                    if mode in ('stop', 'home') and len(requests) == 1:
                        assert connection.recv(1) == b''
                        disconnects.append(True)
                        continue
                    if mode in ('timeout', 'queued-read-remaining'):
                        stop.wait(6)
                        continue
                    if mode == 'mismatch':
                        reply = bytes((reply[0] ^ 1,)) + reply[1:]
                    if mode == 'oversized':
                        reply = reply[:4] + b'\xff\xff'
                    if mode == 'short':
                        reply = reply[:8]
                    chunk = 1 if mode in ('fragmented', 'slow') else len(reply)
                    for offset in range(0, len(reply), chunk):
                        if stop.is_set():
                            break
                        connection.sendall(reply[offset:offset + chunk])
                        if mode in ('fragmented', 'slow') and stop.wait(0.015 if mode == 'fragmented' else 0.65):
                            break
                except EOFError:
                    if mode not in ('probe', 'queued-probe-expired'):
                        errors.append('unexpected EOF')
                    else:
                        disconnects.append(True)
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    if mode not in ('slow', 'stop', 'home'):
                        errors.append('unexpected reset')
                except BaseException as error:
                    errors.append(repr(error))
    worker = None
    if mode != 'closed':
        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
    try:
        result = subprocess.run([str(exe), str(port), mode, str(output)], capture_output=True, text=True, timeout=35, creationflags=subprocess.CREATE_NO_WINDOW)
    finally:
        stop.set()
        if mode != 'closed':
            server.close()
            worker.join(3)
            assert not worker.is_alive()
    evidence = {'scenario': mode, 'exitCode': result.returncode,
                'requests': [request.hex() for request in requests], 'connections': len(connections),
                'disconnects': len(disconnects),
                'serverErrors': errors, 'stdout': result.stdout, 'stderr': result.stderr,
                'loopbackOnly': True, 'workerStartAndMonotonicClockControlled': mode.startswith('queued-'),
                'actualSdkSchedulerOrPhysicalWifiVerified': False}
    (output / f'{mode}-wire.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(result.stdout, end='', flush=True)
    if result.stderr:
        print(result.stderr, end='', flush=True)
    assert result.returncode == 0, (mode, result.returncode)
    assert not errors, (mode, errors)
    expected = (26 if mode == 'queued-retry' else 1 if mode in ('queued-read-ready', 'queued-read-remaining')
                else 0 if mode.startswith('queued-') else 4 if mode in ('views', 'functions', 'fragmented')
                else 100 if mode == 'repeated' else 2 if mode in ('stop', 'home')
                else 0 if mode in ('ui', 'inputs', 'probe', 'closed') else 1)
    assert len(requests) == expected, (mode, len(requests), expected)
    if mode in ('probe', 'stop', 'home'):
        assert disconnects
    if mode in ('functions', 'fragmented'):
        assert [r[7] for r in requests] == [1, 2, 3, 4]
    assert len({r[:2] for r in requests}) == len(requests), 'transaction IDs repeated'
    print(f'fixture {mode}: requests={len(requests)}, validated MBAP/read-only functions, server closed', flush=True)
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Real loopback Modbus worker checks; binds only 127.0.0.1.')
    parser.add_argument('--executable', type=pathlib.Path, default=exe)
    parser.add_argument('--output', type=pathlib.Path, default=output)
    parser.add_argument('modes', nargs='*')
    args = parser.parse_args()
    exe = args.executable
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    modes = args.modes or ['ui', 'views', 'inputs', 'functions', 'fragmented', 'gates', 'probe', 'exception', 'mismatch', 'oversized', 'short', 'closed', 'slow', 'timeout', 'stop', 'home', 'repeated',
                          'queued-read-expired', 'queued-probe-expired', 'queued-read-ready', 'queued-read-remaining', 'queued-read-stop', 'queued-read-home', 'queued-retry']
    for mode in modes:
        run_case(mode)
    print(f'All {len(modes)} real loopback scenarios passed.', flush=True)
