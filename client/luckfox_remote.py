#!/usr/bin/env python3
import argparse
import json
import sys
import urllib.request
import urllib.error

DEFAULT_HOST = "https://luckfoxpico1.aiserver.onmobilespace.com"

def api_get(base, path):
    url = f"{base}{path}"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return json.loads(e.read())
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def api_post(base, path, data=None, timeout=10):
    url = f"{base}{path}"
    body = json.dumps(data).encode() if data else b''
    try:
        req = urllib.request.Request(url, data=body, method='POST')
        req.add_header('Content-Type', 'application/json')
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return json.loads(e.read())
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(description="LuckFox Agent API Client")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"Base URL (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=None,
                        help="Port override (used when --host is an IP)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("status", help="Get device status")
    sub.add_parser("state", help="Get current agent state")

    p_set = sub.add_parser("set", help="Set agent state")
    p_set.add_argument("state", choices=["idle", "listening", "thinking", "speaking", "error"],
                       help="Target state")
    p_set.add_argument("--text", default=None, help="Optional text (for speaking/error states)")

    args = parser.parse_args()

    host = args.host
    if host.startswith("http://") or host.startswith("https://"):
        base = host.rstrip("/")
    else:
        port = args.port if args.port else 8080
        base = f"http://{host}:{port}"

    if args.command == "status":
        result = api_get(base, "/api/status")
        print(json.dumps(result, indent=2))

    elif args.command == "state":
        result = api_get(base, "/api/agent/state")
        print(json.dumps(result, indent=2))

    elif args.command == "set":
        data = {"state": args.state}
        if args.text:
            data["text"] = args.text
        result = api_post(base, "/api/agent/state", data=data)
        print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
