#!/usr/bin/env python3

# Copyright 2015, 2016 Endless Mobile, Inc.

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

import gzip
import http.server
import sys


class PrintingHTTPRequestHandler(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        print(self.path, flush=True)

        content_encoding = self.headers["X-Endless-Content-Encoding"]
        print(content_encoding, flush=True)

        content_length = int(self.headers["Content-Length"])
        compressed_request_body = self.rfile.read(content_length)
        decompressed_request_body = gzip.decompress(compressed_request_body)
        print(len(decompressed_request_body), flush=True)
        sys.stdout.buffer.write(decompressed_request_body)
        sys.stdout.buffer.flush()

        status_code_str = sys.stdin.readline()
        status_code = int(status_code_str)
        if status_code < 0:
            # Send garbage back
            self.send_header("I am a teapot", "\nhello\n\n\n\r\n\r\n")
            self.send_header("X-Y", "Z")
            self.send_response(200)
        else:
            self.send_response(status_code)
        self.end_headers()


# A metrics server that simply prints the requests it receives to stdout
class MockServer(http.server.HTTPServer):
    def __init__(self):
        SERVER_ADDRESS = ("localhost", 0)
        super().__init__(SERVER_ADDRESS, PrintingHTTPRequestHandler)


if __name__ == "__main__":
    mock_server = MockServer()
    print(mock_server.server_port, flush=True)
    mock_server.serve_forever()
