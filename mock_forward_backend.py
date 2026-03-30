#!/usr/bin/env python3
"""
Mock Forward Backend Server for SRS Dynamic Forward

This server simulates the backend API that SRS calls to get dynamic forward destinations.
When SRS publishes a stream, it will call this server to get the list of RTMP URLs to forward to.

Usage:
    python3 mock_forward_backend.py [port]

SRS Configuration:
    vhost __defaultVhost__ {
        forward {
            enabled on;
            backend http://127.0.0.1:8088/api/v1/forward;
        }
    }

Request from SRS (POST JSON):
    {
        "action": "on_forward",
        "server_id": "...",
        "service_id": "...",
        "client_id": "...",
        "ip": "192.168.1.100",
        "vhost": "__defaultVhost__",
        "app": "live",
        "tcUrl": "rtmp://192.168.1.10/live",
        "stream": "livestream",
        "param": ""
    }

Response (JSON):
    {
        "code": 0,
        "data": {
            "urls": [
                "rtmp://server1:1935/live/stream1",
                "rtmp://server2:1935/live/stream2"
            ]
        }
    }
"""

import json
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime

# Configure your forward destinations here
# Set via environment variable or edit directly
import os

# Get IVS endpoint from environment variable, or use default test destination
IVS_ENDPOINT = os.environ.get("IVS_ENDPOINT", "")

FORWARD_DESTINATIONS = {
    # Default destinations for any stream
    "default": [
        # Add your destinations here, examples:
        # "rtmp://backup-server:1935/live/backup_stream",
    ],
    
    # Stream-specific destinations (app/stream -> destinations)
    # "live/important_stream": [
    #     "rtmp://primary:1935/live/main",
    # ],
}

# If IVS_ENDPOINT is set, add it to default destinations
if IVS_ENDPOINT:
    FORWARD_DESTINATIONS["default"].append(IVS_ENDPOINT)
    print(f"[Config] Added IVS endpoint: {IVS_ENDPOINT[:50]}...")


class ForwardBackendHandler(BaseHTTPRequestHandler):
    """HTTP request handler for SRS forward backend API"""
    
    def log_message(self, format, *args):
        """Custom log format with timestamp"""
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{timestamp}] {self.address_string()} - {format % args}")
    
    def send_json_response(self, code, data):
        """Send JSON response to client"""
        response = json.dumps(data, indent=2)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(response))
        self.end_headers()
        self.wfile.write(response.encode("utf-8"))
    
    def do_POST(self):
        """Handle POST request from SRS"""
        # Read request body
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8")
        
        print(f"\n{'='*60}")
        print(f"Received request: {self.path}")
        print(f"Body: {body}")
        
        try:
            req = json.loads(body) if body else {}
        except json.JSONDecodeError:
            req = {}
        
        # Handle forward backend request
        if self.path == "/api/v1/forward":
            self.handle_forward(req)
        else:
            # Unknown endpoint
            self.send_json_response(404, {"code": 404, "msg": "Not Found"})
    
    def handle_forward(self, req):
        """Handle on_forward request from SRS"""
        action = req.get("action", "")
        app = req.get("app", "live")
        stream = req.get("stream", "")
        vhost = req.get("vhost", "__defaultVhost__")
        client_ip = req.get("ip", "unknown")
        
        print(f"\n[Forward Request]")
        print(f"  Action: {action}")
        print(f"  Client IP: {client_ip}")
        print(f"  Vhost: {vhost}")
        print(f"  App: {app}")
        print(f"  Stream: {stream}")
        
        # Determine forward destinations
        stream_key = f"{app}/{stream}"
        
        if stream_key in FORWARD_DESTINATIONS:
            urls = FORWARD_DESTINATIONS[stream_key]
            print(f"  Using stream-specific destinations for '{stream_key}'")
        else:
            urls = FORWARD_DESTINATIONS.get("default", [])
            print(f"  Using default destinations")
        
        # Send response
        response = {
            "code": 0,
            "data": {
                "urls": urls
            }
        }
        
        self.send_json_response(200, response)
        print(f"{'='*60}\n")
    
    def do_GET(self):
        """Handle GET request - for health check"""
        if self.path == "/health":
            self.send_json_response(200, {"status": "ok"})
        else:
            # Return simple status page
            html = """
<!DOCTYPE html>
<html>
<head><title>SRS Forward Backend Mock Server</title></head>
<body>
<h1>SRS Forward Backend Mock Server</h1>
<p>This server handles dynamic forward requests from SRS.</p>
<h2>API Endpoint</h2>
<pre>POST /api/v1/forward</pre>
<h2>Current Forward Destinations</h2>
<pre>%s</pre>
<h2>SRS Configuration Example</h2>
<pre>
vhost __defaultVhost__ {
    forward {
        enabled on;
        backend http://127.0.0.1:%d/api/v1/forward;
    }
}
</pre>
</body>
</html>
""" % (json.dumps(FORWARD_DESTINATIONS, indent=2), PORT)
            
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(html.encode("utf-8"))


def main():
    global PORT
    PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8088
    
    server = HTTPServer(("0.0.0.0", PORT), ForwardBackendHandler)
    
    print(f"""
╔══════════════════════════════════════════════════════════════╗
║         SRS Forward Backend Mock Server                      ║
╠══════════════════════════════════════════════════════════════╣
║  Listening on: http://0.0.0.0:{PORT:<5}                        ║
║  API Endpoint: POST /api/v1/forward                          ║
║  Health Check: GET /health                                   ║
╠══════════════════════════════════════════════════════════════╣
║  SRS Config:                                                 ║
║    forward {{                                                 ║
║        enabled on;                                           ║
║        backend http://127.0.0.1:{PORT}/api/v1/forward;        ║
║    }}                                                         ║
╚══════════════════════════════════════════════════════════════╝

Edit FORWARD_DESTINATIONS in this script to configure forward URLs.

Waiting for requests...
""")
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        server.shutdown()


if __name__ == "__main__":
    main()
