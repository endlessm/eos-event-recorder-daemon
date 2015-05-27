#!/usr/bin/env python3

# Copyright 2015 Endless Mobile, Inc.

# This file is part of eos-event-recorder-daemon.
#
# eos-event-recorder-daemon is free software: you can redistribute it and/or
# modify it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or (at your
# option) any later version.
#
# eos-event-recorder-daemon is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
# Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with eos-event-recorder-daemon.  If not, see
# <http://www.gnu.org/licenses/>.

import http.client
import http.server
import sys

class PrintingHTTPRequestHandler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        print(self.path, flush=True)
        content_length = int(self.headers['Content-Length'])
        print(content_length, flush=True)
        request_body = self.rfile.read(content_length)
        sys.stdout.buffer.write(request_body)
        sys.stdout.buffer.flush()
        sys.stdin.readline()  # Block until test says to proceed.
        self.send_response(http.client.OK)
        self.end_headers()

# A metrics server that simply prints the requests it receives to stdout
class MockServer(http.server.HTTPServer):
    def __init__(self):
        SERVER_ADDRESS = ('localhost', 0)
        super().__init__(SERVER_ADDRESS, PrintingHTTPRequestHandler)

if __name__ == '__main__':
    mock_server = MockServer()
    print(mock_server.server_port, flush=True)
    mock_server.serve_forever()
