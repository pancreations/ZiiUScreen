"""Reusable Wii U Workbench CLI and stdio MCP server (Python standard library only)."""
from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import ftplib
import hashlib
import io
import json
import re
from pathlib import Path, PurePosixPath
import secrets
import socket
import struct
import sys
import time
import zlib
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
LOCAL = ROOT / '.local' / 'wiiu' / 'workbench'
CONFIG = LOCAL / 'connection.json'
SD = '/fs/vol/external01'
PLUGIN = SD + '/wiiu/environments/aroma/plugins/ziiu-workbench.wps'
TOKEN = SD + '/wiiu/ziiu-workbench.token'
BUTTONS = dict(A=0x8000, B=0x4000, X=0x2000, Y=0x1000, LEFT=0x800,
               RIGHT=0x400, UP=0x200, DOWN=0x100, ZL=0x80, ZR=0x40,
               L=0x20, R=0x10, PLUS=8, MINUS=4, HOME=2, TV=0x10000,
               RSTICK=0x20000, LSTICK=0x40000)


def remote_path(path):
    if not isinstance(path, str) or not path.startswith('/') or any(ord(c) < 32 for c in path):
        raise ValueError('Use an absolute FTP path without control characters')
    if '..' in PurePosixPath(path).parts:
        raise ValueError('Parent traversal is not allowed')
    return str(PurePosixPath(path))


def destination(name):
    """All console data stays in the gitignored workspace, including memory dumps."""
    if not isinstance(name, str) or not name or '/' in name or '\\' in name or ':' in name or name in ('.', '..'):
        raise ValueError('Output must be a simple filename')
    LOCAL.mkdir(parents=True, exist_ok=True)
    path = LOCAL / name
    if path.resolve().parent != LOCAL.resolve():
        raise ValueError('Output must stay in the local workbench directory')
    return path


def ppm_to_png(ppm):
    magic, dimensions, maximum, rgb = ppm.split(b'\n', 3)
    w, h = map(int, dimensions.split())
    if magic != b'P6' or maximum != b'255' or not (0 < w <= 640 and 0 < h <= 480) or len(rgb) != w * h * 3:
        raise ValueError('Invalid screenshot data')
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    scanlines = b''.join(b'\0' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b''))


class Workbench:
    def __init__(self, config):
        self.config = config
        self.host = config.get('host')
        if not self.host:
            raise ValueError('No console configured. Run install --host <Wii-U-IP> first.')

    @contextmanager
    def ftp(self):
        ftp = ftplib.FTP(timeout=5)
        try:
            ftp.connect(self.host, self.config.get('ftp_port', 21))
            ftp.login(self.config.get('ftp_user', 'anonymous'), self.config.get('ftp_password', ''))
            yield ftp
        finally:
            ftp.close()

    def request(self, command, *args):
        if any(not str(p) or any(ord(c) <= 32 or ord(c) > 126 for c in str(p)) for p in (command, *args)):
            raise ValueError('Command arguments must be single printable ASCII words')
        if command == 'TITLES':
            raise ValueError('Console title enumeration was removed; use the FTP titles() method')
        if command != 'STATUS' and not getattr(self, '_verified_build', False):
            info = self._request_wire('STATUS')
            if info.get('build') != '0.1.2':
                raise ValueError('Refusing console commands: Workbench 0.1.2 is required; older builds are quarantined')
            self._verified_build = True
        return self._request_wire(command, *args)

    def _request_wire(self, command, *args):
        token = self.config.get('token', '')
        if len(token) != 64 or any(c not in '0123456789abcdef' for c in token):
            raise ValueError('Missing authentication token; run install first')
        parts = [command, *(str(arg) for arg in args)]
        if any(not p or any(ord(c) <= 32 or ord(c) > 126 for c in p) for p in parts):
            raise ValueError('Command arguments must be single printable ASCII words')
        payload = (' '.join([token, *parts]) + '\n').encode('ascii')
        if len(payload) > 511:
            raise ValueError('Command too long')
        # One attempt, a whole-request deadline, and bounded replies. Never retry a mutation.
        deadline = time.monotonic() + 8
        try:
            with socket.create_connection((self.host, self.config.get('port', 49426)), timeout=3) as conn:
                conn.settimeout(3)
                conn.sendall(payload)
                pending = bytearray()
                def receive():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('Workbench request deadline exceeded')
                    conn.settimeout(min(3, remaining))
                    block = conn.recv(8192)
                    if not block:
                        raise ConnectionError('Console closed the connection before completing the reply')
                    pending.extend(block)
                def line():
                    while b'\n' not in pending:
                        if len(pending) > 16384:
                            raise ValueError('Console reply line too large')
                        receive()
                    first, _, rest = pending.partition(b'\n')
                    pending[:] = rest
                    if len(first) > 16384:
                        raise ValueError('Console reply line too large')
                    value = json.loads(first)
                    if not isinstance(value, dict):
                        raise ValueError('Expected JSON object')
                    return value
                result = line()
                if result.get('ok') is not True:
                    raise ValueError(result.get('error', f'Console operation failed: {result}'))
                if 'bytes' in result:
                    size = result['bytes']
                    if type(size) is not int or not 0 <= size <= 1024 * 1024:
                        raise ValueError('Invalid binary response length')
                    while len(pending) < size:
                        receive()
                    result['data'] = bytes(pending[:size])
                elif 'records' in result:
                    count = result['records']
                    if type(count) is not int or not 0 <= count <= 4096:
                        raise ValueError('Invalid record count')
                    result['items'] = [line() for _ in range(count)]
                return result
        except OSError as exc:
            raise ConnectionError('Workbench unavailable or connection interrupted. Check that the Wii U is on, '
                                  'Aroma has loaded ZiiU Workbench, and its Enable setting is on. '
                                  'No automatic retry was made; an interrupted action may have been accepted. '
                                  f'({exc})') from exc

    def list_files(self, path='/'):
        path = remote_path(path)
        with self.ftp() as ftp:
            try:
                entries = [{'name': name, **facts} for name, facts in ftp.mlsd(path)]
                return {'path': path, 'entries': entries}
            except ftplib.error_perm as exc:
                if not str(exc).startswith(('500', '502', '504')):
                    raise
                return {'path': path, 'entries': [{'name': p} for p in ftp.nlst(path)]}

    def titles(self):
        """Browse title directories through FTP, never the hooked MCP_TitleList API."""
        roots = ['/storage_mlc/usr/title', '/storage_usb/usr/title', '/storage_mlc/sys/title']
        items, unavailable = [], []
        with self.ftp() as ftp:
            for root in roots:
                try:
                    types = ftp.nlst(root)
                except (ftplib.error_perm, ftplib.error_temp) as exc:
                    unavailable.append({'root': root, 'error': str(exc)})
                    continue
                for entry in types:
                    high = PurePosixPath(entry).name.lower()
                    if not re.fullmatch('[0-9a-f]{8}', high):
                        continue
                    for title in ftp.nlst(root + '/' + high):
                        low = PurePosixPath(title).name.lower()
                        if re.fullmatch('[0-9a-f]{8}', low):
                            items.append({'title_id': high + low, 'path': root + '/' + high + '/' + low,
                                          'launchable_type': high in ('00050000', '00050010')})
        return {'ok': True, 'source': 'FTP title directories', 'records': len(items),
                'items': items, 'unavailable_roots': unavailable}

    def launch(self, title_id):
        title_id = title_id.lower()
        if not re.fullmatch('[0-9a-f]{16}', title_id) or title_id[:8] not in ('00050000', '00050010'):
            raise ValueError('Expected a Wii U game or system application title ID')
        candidates = [t for t in self.titles()['items'] if t['title_id'] == title_id]
        if not candidates:
            raise ValueError('Title was not found on console storage')
        metadata = self.read_file(candidates[0]['path'] + '/meta/meta.xml')
        recorded = ET.fromstring(metadata).findtext('title_id', '').strip().lower()
        if recorded != title_id:
            raise ValueError('Installed title metadata does not match the requested ID')
        return self.request('LAUNCH', title_id)

    def read_file(self, path, limit=65536):
        if type(limit) is not int or not 1 <= limit <= 1024 * 1024:
            raise ValueError('Read limit must be 1..1048576 bytes')
        data = bytearray()
        def collect(block):
            data.extend(block)
            if len(data) > limit:
                raise ValueError(f'File exceeds {limit} bytes; use download instead')
        with self.ftp() as ftp:
            ftp.retrbinary('RETR ' + remote_path(path), collect)
        return bytes(data)

    def download(self, remote, name):
        out = destination(name)
        part = out.with_name(out.name + '.part')
        try:
            with self.ftp() as ftp, part.open('wb') as f:
                ftp.retrbinary('RETR ' + remote_path(remote), f.write)
            part.replace(out)
        finally:
            part.unlink(missing_ok=True)
        return {'path': str(out), 'bytes': out.stat().st_size}

    def upload(self, local, remote):
        remote = remote_path(remote)
        if not remote.startswith(SD + '/'):
            raise ValueError('Uploads are limited to the SD card; system NAND writes are not exposed')
        path = Path(local).resolve()
        if not path.is_relative_to(ROOT.resolve()) or not path.is_file():
            raise ValueError('Upload source must be a file inside this workspace')
        data = path.read_bytes()
        if len(data) > 64 * 1024 * 1024:
            raise ValueError('Upload is limited to 64 MiB')
        with self.ftp() as ftp:
            backup = self._backup(ftp, remote)
            self._verified_put(ftp, remote, data)
        return {'remote': remote, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'backup': backup}

    @staticmethod
    def _backup(ftp, remote):
        name = PurePosixPath(remote).name
        backup = destination(f'backup-{time.time_ns()}-{name}')
        try:
            with backup.open('wb') as f:
                ftp.retrbinary('RETR ' + remote, f.write)
        except (ftplib.error_perm, ftplib.error_temp) as exc:
            backup.unlink(missing_ok=True)
            # FTPiiU uses 450 for a missing file. Verify absence: either code can
            # also mean a genuine permission or transient failure.
            if str(exc).startswith(('450', '550')) and name not in {PurePosixPath(p).name for p in ftp.nlst(str(PurePosixPath(remote).parent))}:
                return None
            raise
        return str(backup)

    @staticmethod
    def _verified_put(ftp, remote, data):
        staging = remote + '.upload'
        ftp.storbinary('STOR ' + staging, io.BytesIO(data))
        digest = hashlib.sha256()
        ftp.retrbinary('RETR ' + staging, digest.update)
        if digest.digest() != hashlib.sha256(data).digest():
            raise ValueError('Uploaded data failed SHA-256 verification; target was not replaced')
        # FTPiiU refuses rename-over-existing. Preserve the old remote file,
        # then publish the verified stage; restore the old name if publication fails.
        parent = str(PurePosixPath(remote).parent)
        names = {PurePosixPath(p).name for p in ftp.nlst(parent)}
        previous = None
        if PurePosixPath(remote).name in names:
            previous = remote + '.previous-' + str(time.time_ns())
            ftp.rename(remote, previous)
        try:
            ftp.rename(staging, remote)
        except ftplib.all_errors:
            if previous is not None:
                ftp.rename(previous, remote)
            raise
        digest = hashlib.sha256()
        ftp.retrbinary('RETR ' + remote, digest.update)
        if digest.digest() != hashlib.sha256(data).digest():
            raise ValueError('Final file failed SHA-256 verification')

    def install(self):
        raise ValueError('Automatic installation is held pending console validation of 0.1.2. '
                         'Versions 0.1.0 and 0.1.1 are quarantined.')
        # Retained for repair review; unreachable until the boot failure is resolved.
        binary = Path(__file__).with_name('ziiu-workbench.wps')
        if not binary.exists():
            raise ValueError('Build the plugin with build.ps1 first')
        LOCAL.mkdir(parents=True, exist_ok=True)
        with self.ftp() as ftp:
            # Check Aroma directories before changing anything.
            ftp.nlst(SD + '/wiiu/environments/aroma/modules')
            names = {PurePosixPath(p).name for p in ftp.nlst(SD + '/wiiu')}
            if PurePosixPath(TOKEN).name in names:
                existing = bytearray()
                ftp.retrbinary('RETR ' + TOKEN, existing.extend)
                token = existing.decode('ascii').strip()
                if len(token) != 64 or any(c not in '0123456789abcdef' for c in token):
                    raise ValueError('Existing token is malformed; refusing to replace it silently')
            else:
                token = secrets.token_hex(32)
                self._verified_put(ftp, TOKEN, (token + '\n').encode('ascii'))
            self.config['token'] = token
            CONFIG.write_text(json.dumps(self.config, indent=2) + '\n', encoding='utf-8')
            backup = self._backup(ftp, PLUGIN)
            data = binary.read_bytes()
            self._verified_put(ftp, PLUGIN, data)
        return {'installed': PLUGIN, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                'backup': backup, 'next': 'Fully power off and restart the Wii U, then run status. No probe app needs launching.'}

    def input(self, buttons='', ms=150, x=-1, y=-1, sx=0, sy=0):
        if type(ms) is not int or not 20 <= ms <= 2000:
            raise ValueError('Duration must be 20..2000 ms')
        mask = 0
        for button in buttons.upper().replace('+', ',').split(','):
            if button:
                if button not in BUTTONS:
                    raise ValueError(f'Unknown button: {button}')
                mask |= BUTTONS[button]
        if any(type(n) is not int for n in (x, y, sx, sy)) or not (-1 <= x <= 4095 and -1 <= y <= 4095) or ((x == -1) != (y == -1)) or not (-1000 <= sx <= 1000 and -1000 <= sy <= 1000):
            raise ValueError('Touch uses raw 0..4095 coordinates or -1,-1; stick uses -1000..1000')
        result = self.request('INPUT', hex(mask), ms, x, y, sx, sy)
        # Leave time for the release edge before the next discrete press.
        time.sleep((ms + 60) / 1000)
        return result

    def screenshot(self, screen='tv', name='screen.png'):
        if screen not in ('tv', 'gamepad'):
            raise ValueError('Screen must be tv or gamepad')
        result = self.request('SCREENSHOT', screen)
        png = ppm_to_png(result['data'])
        path = destination(name)
        path.write_bytes(png)
        return {'path': str(path), 'bytes': len(png)}, png


def schema(properties=None, required=()):
    return {'type': 'object', 'properties': properties or {}, 'required': list(required), 'additionalProperties': False}


STRING = {'type': 'string'}
TOOLS = [
    ('status', 'Connection, current title and supported commands. One attempt; no polling.', schema(), True),
    ('radio', 'Read DRH/GamePad/WPS states without exporting pairing secrets.', schema(), True),
    ('list_files', 'List any FTP-exposed directory, including SD, USB and system storage.', schema({'path': STRING}, ['path']), True),
    ('read_file', 'Read a small UTF-8 text file through FTP.', schema({'path': STRING}, ['path']), True),
    ('download', 'Download a file to the gitignored local workbench directory.', schema({'remote': STRING, 'name': STRING}, ['remote', 'name']), True),
    ('upload', 'Upload a workspace file to SD with backup and readback verification.', schema({'local': STRING, 'remote': STRING}, ['local', 'remote']), False),
    ('screenshot', 'Capture the next TV or GamePad frame and return a PNG. Two-second console deadline.', schema({'screen': {'type': 'string', 'enum': ['tv', 'gamepad']}}, ['screen']), True),
    ('input', 'Press buttons, raw touch or left stick for 20..2000ms. Requires physical GamePad connected.', schema({'buttons': STRING, 'ms': {'type': 'integer', 'minimum': 20, 'maximum': 2000}, 'x': {'type': 'integer'}, 'y': {'type': 'integer'}, 'sx': {'type': 'integer'}, 'sy': {'type': 'integer'}}), False),
    ('release', 'Release all remotely injected input immediately.', schema(), False),
    ('titles', 'Browse title directories on internal/USB storage through FTP. Does not call console MCP APIs.', schema(), True),
    ('launch', 'Launch an installed title; connection may close as the title changes. Do not blindly retry.', schema({'title_id': STRING}, ['title_id']), False),
    ('menu', 'Return to the Wii U Menu; connection may close during transition.', schema(), False),
    ('symbol', 'Find an exported function/data address in an already-loaded module; does not invoke it.', schema({'module': STRING, 'symbol': STRING, 'kind': {'type': 'string', 'enum': ['function', 'data']}}, ['module', 'symbol']), True),
    ('memory_read', 'Read 1..4096 bytes from mapped user code/data RAM. Save locally; no kernel/MMIO reads or writes.', schema({'address': STRING, 'length': {'type': 'integer', 'minimum': 1, 'maximum': 4096}}, ['address', 'length']), True),
]


def call_tool(client, name, args):
    matches = [t for t in TOOLS if 'wiiu_' + t[0] == name]
    if not matches:
        raise ValueError('Unknown tool')
    short, _, spec, _ = matches[0]
    if not isinstance(args, dict) or set(args) - set(spec['properties']) or set(spec['required']) - set(args):
        raise ValueError('Invalid tool arguments')
    for key, value in args.items():
        rule = spec['properties'][key]
        if (rule['type'] == 'string' and not isinstance(value, str)) or (rule['type'] == 'integer' and type(value) is not int):
            raise ValueError(f'Invalid type for {key}')
        if 'enum' in rule and value not in rule['enum']:
            raise ValueError(f'Invalid value for {key}')
        if 'minimum' in rule and not rule['minimum'] <= value <= rule['maximum']:
            raise ValueError(f'Invalid range for {key}')
    image = None
    if short in ('status', 'radio', 'release', 'menu'):
        result = client.request(short.upper())
    elif short == 'launch':
        result = client.launch(args['title_id'])
    elif short == 'symbol':
        result = client.request('SYMBOL', args['module'], args['symbol'], args.get('kind', 'function'))
    elif short == 'memory_read':
        address, length = args['address'], args['length']
        result = client.request('READ', address, length)
        data = result.pop('data')
        path = destination('memory.bin'); path.write_bytes(data)
        result.update(path=str(path), hex=data.hex())
    elif short == 'read_file':
        result = {'path': args['path'], 'text': client.read_file(args['path']).decode('utf-8', errors='replace')}
    elif short == 'screenshot':
        result, image = client.screenshot(**args)
    else:
        result = getattr(client, short)(**args)
    content = [{'type': 'text', 'text': json.dumps(result)}]
    if image:
        content.append({'type': 'image', 'mimeType': 'image/png', 'data': base64.b64encode(image).decode('ascii')})
    return {'content': content}


def serve_mcp(client, stdin=sys.stdin, stdout=sys.stdout):
    for line in stdin:
        ident = None
        try:
            try:
                request = json.loads(line)
            except json.JSONDecodeError:
                raise RuntimeError(-32700, 'Invalid JSON')
            if not isinstance(request, dict) or request.get('jsonrpc') != '2.0' or not isinstance(request.get('method'), str):
                raise RuntimeError(-32600, 'Invalid JSON-RPC request')
            ident = request.get('id')
            if 'id' not in request:
                continue
            method = request['method']
            params = request.get('params', {})
            if not isinstance(params, dict):
                raise RuntimeError(-32602, 'Invalid params')
            if method == 'initialize':
                version = params.get('protocolVersion', '2025-06-18')
                if version not in ('2024-11-05', '2025-03-26', '2025-06-18'):
                    version = '2025-06-18'
                result = {'protocolVersion': version, 'capabilities': {'tools': {}},
                          'serverInfo': {'name': 'ziiu-workbench', 'version': '0.1.2'}}
            elif method == 'ping':
                result = {}
            elif method == 'tools/list':
                result = {'tools': [{'name': 'wiiu_' + n, 'description': d, 'inputSchema': s,
                                     'annotations': {'readOnlyHint': read, 'destructiveHint': not read}}
                                    for n, d, s, read in TOOLS]}
            elif method == 'tools/call':
                try:
                    result = call_tool(client, params.get('name'), params.get('arguments', {}))
                except (OSError, ValueError, EOFError, ftplib.Error) as exc:
                    result = {'isError': True, 'content': [{'type': 'text', 'text': str(exc)}]}
            else:
                raise RuntimeError(-32601, 'Method not found')
            response = {'jsonrpc': '2.0', 'id': ident, 'result': result}
        except RuntimeError as exc:
            code, message = exc.args
            response = {'jsonrpc': '2.0', 'id': ident, 'error': {'code': code, 'message': message}}
        stdout.write(json.dumps(response, separators=(',', ':')) + '\n')
        stdout.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', help='Override the saved Wii U address')
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('install', 'status', 'radio', 'titles', 'menu', 'release', 'mcp'):
        sub.add_parser(name)
    p = sub.add_parser('ls'); p.add_argument('path', nargs='?', default='/')
    p = sub.add_parser('cat'); p.add_argument('path')
    p = sub.add_parser('get'); p.add_argument('remote'); p.add_argument('name')
    p = sub.add_parser('put'); p.add_argument('local'); p.add_argument('remote')
    p = sub.add_parser('screenshot'); p.add_argument('screen', choices=['tv', 'gamepad'], nargs='?', default='tv'); p.add_argument('--name', default='screen.png')
    p = sub.add_parser('input'); p.add_argument('buttons', nargs='?', default=''); p.add_argument('--ms', type=int, default=150)
    for name, default in [('x', -1), ('y', -1), ('sx', 0), ('sy', 0)]:
        p.add_argument('--' + name, type=int, default=default)
    p = sub.add_parser('launch'); p.add_argument('title_id')
    p = sub.add_parser('symbol'); p.add_argument('module'); p.add_argument('symbol'); p.add_argument('--kind', default='function', choices=['function', 'data'])
    p = sub.add_parser('read'); p.add_argument('address'); p.add_argument('length', type=int); p.add_argument('--name', default='memory.bin')
    args = parser.parse_args()
    config = json.loads(CONFIG.read_text(encoding='utf-8')) if CONFIG.exists() else {}
    if args.host:
        config['host'] = args.host
    client = Workbench(config)
    command = args.command
    if command == 'mcp':
        serve_mcp(client); return
    if command in ('status', 'radio', 'menu', 'release'):
        result = client.request(command.upper())
    elif command == 'titles': result = client.titles()
    elif command == 'install': result = client.install()
    elif command == 'ls': result = client.list_files(args.path)
    elif command == 'cat':
        print(client.read_file(args.path).decode('utf-8', errors='replace')); return
    elif command == 'get': result = client.download(args.remote, args.name)
    elif command == 'put': result = client.upload(args.local, args.remote)
    elif command == 'screenshot': result, _ = client.screenshot(args.screen, args.name)
    elif command == 'input': result = client.input(args.buttons, args.ms, args.x, args.y, args.sx, args.sy)
    elif command == 'launch': result = client.launch(args.title_id)
    elif command == 'symbol': result = client.request('SYMBOL', args.module, args.symbol, args.kind)
    elif command == 'read':
        result = client.request('READ', args.address, args.length)
        data = result.pop('data'); path = destination(args.name); path.write_bytes(data)
        result.update(path=str(path), hex=data.hex())
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, EOFError, ftplib.Error) as exc:
        print(f'Workbench: {exc}', file=sys.stderr)
        sys.exit(1)
