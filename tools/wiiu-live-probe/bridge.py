"""Read-only PC client and stdio MCP bridge for the Wii U Live Probe.

Run `py -3 bridge.py --status --host <Wii-U-IP>` for a direct status query.
Run `py -3 bridge.py --mcp --host <Wii-U-IP>` from an MCP client.
"""
import argparse
import json
import socket
import sys

PORT = 49425


def status(host, port=PORT):
    with socket.create_connection((host, port), timeout=3) as connection:
        connection.settimeout(3)
        connection.sendall(b"STATUS\n")
        data = bytearray()
        while b"\n" not in data and len(data) < 1024:
            chunk = connection.recv(1024)
            if not chunk:
                break
            data.extend(chunk)
    if b"\n" not in data:
        raise ValueError("Live probe returned no complete status")
    reply = json.loads(data.split(b"\n", 1)[0])
    if reply.get("version") != 1:
        raise ValueError("Unsupported live probe version")
    return reply


def serve_mcp(host, port):
    for line in sys.stdin:
        request = None
        try:
            request = json.loads(line)
            method = request.get("method")
            ident = request.get("id")
            if ident is None:  # notifications have no reply
                continue
            if method == "initialize":
                result = {
                    "protocolVersion": request.get("params", {}).get("protocolVersion", "2025-06-18"),
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "ziiu-live-probe", "version": "0.1.0"},
                }
            elif method == "ping":
                result = {}
            elif method == "tools/list":
                result = {"tools": [{
                    "name": "wiiu_radio_status",
                    "description": "Read the Wii U DRH and GamePad connection states; no keys or EEPROM data",
                    "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
                }]}
            elif method == "tools/call" and request.get("params", {}).get("name") == "wiiu_radio_status":
                try:
                    result = {"content": [{"type": "text", "text": json.dumps(status(host, port))}]}
                except (OSError, ValueError) as exc:
                    result = {"isError": True, "content": [{"type": "text", "text": str(exc)}]}
            else:
                raise ValueError("Unknown method or tool")
            reply = {"jsonrpc": "2.0", "id": ident, "result": result}
        except (ValueError, TypeError) as exc:
            reply = {"jsonrpc": "2.0", "id": request.get("id") if isinstance(request, dict) else None,
                     "error": {"code": -32601, "message": str(exc)}}
        print(json.dumps(reply, separators=(",", ":")), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--status", action="store_true")
    mode.add_argument("--mcp", action="store_true")
    parser.add_argument("--host", required=True, help="Wii U address displayed by its FTP app")
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()
    if args.status:
        print(json.dumps(status(args.host, args.port), indent=2))
    else:
        serve_mcp(args.host, args.port)


if __name__ == "__main__":
    main()
