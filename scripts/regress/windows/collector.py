"""Append POST bodies from the guest to a report file.

The guest cannot be read from directly, so every phase script ends by POSTing
its log here. The runner tails the file for the phase's END marker.

    collector.py --bind 192.168.122.1 --port 9000 report.log
"""

import argparse
import datetime
import http.server
import sys


def make_handler(out_path):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode('utf-8', 'replace')
            stamp = datetime.datetime.now().isoformat(timespec='seconds')
            with open(out_path, 'a') as f:
                f.write('\n===== {} {}\n{}\n'.format(stamp, self.path, body))
                f.flush()
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'ok')

        def log_message(self, *_args):
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--bind', default='192.168.122.1')
    ap.add_argument('--port', type=int, default=9000)
    ap.add_argument('out')
    args = ap.parse_args()
    server = http.server.HTTPServer((args.bind, args.port), make_handler(args.out))
    server.serve_forever()
    return 0


if __name__ == '__main__':
    sys.exit(main())
