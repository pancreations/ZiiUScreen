import io
import ftplib
import json
import socket
import struct
import threading
import unittest
import zlib
from unittest.mock import patch
from contextlib import contextmanager

import workbench as w


class WireTests(unittest.TestCase):
    def exchange(self, chunks, command='STATUS'):
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0)); listener.listen(1)
        port = listener.getsockname()[1]
        requests = []
        def server():
            try:
                with listener.accept()[0] as conn:
                    data = bytearray()
                    while b'\n' not in data:
                        data.extend(conn.recv(1024))
                    requests.append(bytes(data))
                    for chunk in chunks:
                        conn.sendall(chunk)
            finally:
                listener.close()
        thread = threading.Thread(target=server, daemon=True); thread.start()
        try:
            result = w.Workbench({'host': '127.0.0.1', 'port': port, 'token': 'a' * 64}).request(command)
            self.assertEqual(requests, [('a' * 64 + ' ' + command + '\n').encode()])
            return result
        finally:
            thread.join(2)
            self.assertFalse(thread.is_alive())

    def test_fragmented_header_and_binary(self):
        result = self.exchange([b'{"ok":tr', b'ue,"bytes":4}\n\0', b'\x01\n\xff'])
        self.assertEqual(result['data'], b'\0\x01\n\xff')

    def test_coalesced_records(self):
        result = self.exchange([b'{"ok":true,"records":2}\n{"title_id":"a"}\n{"title_id":"b"}\n'])
        self.assertEqual(len(result['items']), 2)

    def test_early_eof_is_error_not_retry(self):
        with self.assertRaises(ConnectionError):
            self.exchange([b'{"ok":true,"bytes":4}\nxx'])

    def test_bad_length_and_auth_error(self):
        for response in [b'{"ok":true,"bytes":2000000}\n', b'{"ok":false,"error":"Authentication failed"}\n']:
            with self.assertRaises(ValueError):
                self.exchange([response])

    def test_command_injection_rejected_before_connect(self):
        client = w.Workbench({'host': '127.0.0.1', 'token': 'a' * 64})
        with patch('socket.create_connection') as connection:
            with self.assertRaises(ValueError):
                client.request('SYMBOL', 'coreinit\nMENU')
            connection.assert_not_called()


class InterfaceTests(unittest.TestCase):
    def test_mcp_notification_and_recover_after_malformed_request(self):
        source = io.StringIO('bad json\n' + '\n'.join(map(json.dumps, [
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': 'unknown'}},
            {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list'},
            {'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call', 'params': {'name': 'wiiu_input', 'arguments': {'ms': 'bad'}}},
            {'jsonrpc': '2.0', 'id': 4, 'method': 'ping'},
        ])) + '\n')
        output = io.StringIO()
        w.serve_mcp(None, source, output)
        responses = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(responses), 5)
        self.assertEqual(responses[0]['error']['code'], -32700)
        self.assertEqual(responses[1]['result']['protocolVersion'], '2025-06-18')
        self.assertEqual(len(responses[2]['result']['tools']), 14)
        self.assertTrue(responses[3]['result']['isError'])
        self.assertEqual(responses[4]['result'], {})

    def test_paths(self):
        for path in ['relative', '/a/../b', '/a\r\nDELE b']:
            with self.assertRaises(ValueError): w.remote_path(path)
        for name in ['../secret', 'C:\\escape', 'a/b', '..']:
            with self.assertRaises(ValueError): w.destination(name)

    def test_png_conversion(self):
        png = w.ppm_to_png(b'P6\n2 1\n255\n\xff\0\0\0\xff\0')
        self.assertEqual(png[:8], b'\x89PNG\r\n\x1a\n')
        offset = 8; compressed = b''
        while offset < len(png):
            size = struct.unpack_from('>I', png, offset)[0]
            kind = png[offset+4:offset+8]
            data = png[offset+8:offset+8+size]
            crc = struct.unpack_from('>I', png, offset+8+size)[0]
            self.assertEqual(crc, zlib.crc32(kind + data))
            if kind == b'IDAT': compressed += data
            offset += size + 12
        self.assertEqual(zlib.decompress(compressed), b'\0\xff\0\0\0\xff\0')
        with self.assertRaises(ValueError): w.ppm_to_png(b'P6\n2 1\n255\nshort')

    def test_failed_upload_does_not_replace_target(self):
        class FTP:
            renamed = False
            def storbinary(self, command, f): self.data = f.read()
            def retrbinary(self, command, callback): callback(b'corrupted')
            def rename(self, old, new): self.renamed = True
        ftp = FTP()
        with self.assertRaises(ValueError): w.Workbench._verified_put(ftp, '/target', b'good')
        self.assertFalse(ftp.renamed)

    def test_upload_denies_nand_before_connect(self):
        client = w.Workbench({'host': 'localhost'})
        with self.assertRaises(ValueError): client.upload(__file__, '/storage_slc/system.bin')

    def test_old_console_build_cannot_receive_actions(self):
        client = w.Workbench({'host': 'localhost'})
        with patch.object(client, '_request_wire', return_value={'ok': True, 'version': 1}) as wire:
            with self.assertRaisesRegex(ValueError, 'older builds are quarantined'):
                client.request('INPUT', '0x400', 120, -1, -1, 0, 0)
            wire.assert_called_once_with('STATUS')

    def test_titles_never_use_console_api(self):
        class FTP:
            def nlst(self, path):
                if path == '/storage_usb/usr/title':
                    raise ftplib.error_temp('450 No USB device')
                if path.endswith('/title'): return [path + '/00050000', path, 'invalid']
                return [path + '/12345678', path + '/invalid']
        @contextmanager
        def ftp(): yield FTP()
        client = w.Workbench({'host': 'localhost'})
        with patch.object(client, 'ftp', ftp), patch.object(client, '_request_wire') as wire:
            result = client.titles()
            self.assertEqual(result['records'], 2)
            self.assertEqual(result['items'][0]['title_id'], '0005000012345678')
            self.assertEqual(len(result['unavailable_roots']), 1)
            wire.assert_not_called()
            with self.assertRaisesRegex(ValueError, 'enumeration was removed'): client.request('TITLES')
            wire.assert_not_called()

    def test_ftpiiu_450_requires_confirmed_absence(self):
        class FTP:
            names = []
            def retrbinary(self, command, callback):
                raise ftplib.error_temp('450 No such file or directory')
            def nlst(self, directory): return self.names
        ftp = FTP()
        with patch.object(w, 'destination') as destination:
            destination.return_value.open.side_effect = lambda mode: io.BytesIO()
            self.assertIsNone(w.Workbench._backup(ftp, '/plugins/new.wps'))
            ftp.names = ['/plugins/new.wps']
            with self.assertRaises(ftplib.error_temp):
                w.Workbench._backup(ftp, '/plugins/new.wps')

    def test_ftpiiu_replacement_preserves_old_file_and_rolls_back_failure(self):
        class FTP:
            def __init__(self, fail=False): self.files = {'/p': b'old'}; self.fail = fail
            def storbinary(self, command, f): self.files[command[5:]] = f.read()
            def retrbinary(self, command, callback): callback(self.files[command[5:]])
            def nlst(self, parent): return list(self.files)
            def rename(self, old, new):
                if new in self.files: raise ftplib.error_perm('550 File exists')
                if self.fail and old.endswith('.upload'): raise ftplib.error_temp('450 Rename failed')
                self.files[new] = self.files.pop(old)
        ftp = FTP()
        w.Workbench._verified_put(ftp, '/p', b'new')
        self.assertEqual(ftp.files['/p'], b'new')
        self.assertIn(b'old', ftp.files.values())
        ftp = FTP(fail=True)
        with self.assertRaises(ftplib.error_temp): w.Workbench._verified_put(ftp, '/p', b'new')
        self.assertEqual(ftp.files['/p'], b'old')


if __name__ == '__main__':
    unittest.main()
