"""Append POST bodies from the guest to a report file.

The guest cannot be read from directly, so every phase script ends by POSTing
its log here. The runner tails the file for the phase's END marker. A body
POSTed to any path other than / is a whole log rather than a report: with
--save-dir it is written to <save-dir>/<last path segment> instead, and only a
line saying so goes into the report.

    collector.py --bind 192.168.122.1 --port 9000 --save-dir posts report.log
"""

import argparse
import datetime
import http.server
import os
import sys


def make_handler(out_path, save_dir):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get('Content-Length', 0))
            body = self.rfile.read(length).decode('utf-8', 'replace')
            stamp = datetime.datetime.now().isoformat(timespec='seconds')
            name = os.path.basename(self.path.rstrip('/'))
            if save_dir and name not in ('', '.', '..'):
                with open(os.path.join(save_dir, name), 'w', encoding='utf-8') as f:
                    f.write(body)
                body = 'saved {} ({} bytes)'.format(name, len(body))
            with open(out_path, 'a', encoding='utf-8') as f:
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
    ap.add_argument('--save-dir', help='where whole logs POSTed to /<name> go')
    ap.add_argument('out')
    args = ap.parse_args()
    server = http.server.HTTPServer((args.bind, args.port),
                                    make_handler(args.out, args.save_dir))
    server.serve_forever()
    return 0


if __name__ == '__main__':
    sys.exit(main())
