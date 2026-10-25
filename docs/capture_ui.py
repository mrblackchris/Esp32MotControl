#!/usr/bin/env python3
"""Capture the actual controller page against read-only, simulated telemetry.

Requires Playwright and Chrome/Chromium. No ESP32 or external network is used.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
from threading import Thread

ROOT = Path(__file__).resolve().parent.parent
STATE = {
    'running': False, 'mode': 'remote', 'reverse': False,
    'freq': 50, 'mod': 10, 'carrier': 20000, 'actualCarrier': 20000,
    'dead': 1, 'ton': 20, 'toff': 80,
    'runSwitch': False, 'directionSwitch': False, 'localArmed': False,
    'potRaw': 2048, 'runVoltage': 3.1, 'directionVoltage': 3.1,
    'potVoltage': 1.65, 'localMaxHz': 50,
    'wifiConnected': True, 'ip': '192.0.2.32',
}


class PreviewHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/state':
            body, kind = json.dumps(STATE).encode(), 'application/json'
        elif self.path == '/motor-control.html':
            body, kind = (ROOT / 'motor-control.html').read_bytes(), 'text/html; charset=utf-8'
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        self.send_error(405, 'Documentation preview is read-only.')

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', default=shutil.which('google-chrome') or shutil.which('chromium'))
    args = parser.parse_args()
    from playwright.sync_api import sync_playwright

    server = ThreadingHTTPServer(('127.0.0.1', 0), PreviewHandler)
    worker = Thread(target=server.serve_forever, daemon=True)
    worker.start()
    origin = f'http://127.0.0.1:{server.server_port}'
    output = ROOT / 'docs/images'
    output.mkdir(parents=True, exist_ok=True)
    try:
        with sync_playwright() as playwright:
            options = {'headless': True}
            if args.browser:
                options['executable_path'] = args.browser
            browser = playwright.chromium.launch(**options)
            try:
                for name, width, height in [('controller-desktop', 1120, 900), ('controller-mobile', 390, 844)]:
                    page = browser.new_page(viewport={'width': width, 'height': height}, device_scale_factor=1)
                    errors = []
                    page.on('pageerror', lambda error: errors.append(str(error)))
                    page.route('**/*', lambda route: route.continue_() if route.request.url.startswith(origin + '/') else route.abort())
                    page.goto(origin + '/motor-control.html')
                    page.wait_for_function("document.querySelector('#live').textContent.includes('1.65 V')")
                    assert page.locator('input[name=freq]').input_value() == '50'
                    assert not page.locator('input[name=run]').is_checked()
                    assert 'run 3.1 V' in page.locator('#live').inner_text()
                    assert 'direction 3.1 V' in page.locator('#live').inner_text()
                    page.locator('#address').fill('http://192.0.2.32')
                    page.evaluate("document.querySelector('#feedback').textContent = 'Documentation preview · simulated device data · no ESP32 connected.'")
                    page.screenshot(path=str(output / f'{name}.png'), full_page=True)
                    assert not errors, errors
                    page.close()
                    print(f'Captured {output / (name + ".png")}')
            finally:
                browser.close()
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == '__main__':
    main()
