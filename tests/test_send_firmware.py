import hashlib
import json
from email.parser import BytesParser
from email.policy import default
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.send_firmware import (MARKER, UploadError, load_settings, main,
                                   make_upload, read_firmware, send_firmware)


class SenderTests(unittest.TestCase):
    def test_image_and_checksum(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'firmware.bin'
            blob = b'\xe9' + MARKER + bytes(4096)
            path.write_bytes(blob)
            path.with_suffix('.bin.md5').write_text('md5:' + hashlib.md5(blob).hexdigest())
            self.assertEqual(read_firmware(path)[0], blob)
            path.write_bytes(blob + b'changed')
            with self.assertRaises(UploadError):
                read_firmware(path)
            path.write_bytes(b'\xe9' + bytes(5000))
            with self.assertRaises(UploadError):
                read_firmware(path)

    def test_secrets_comments(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'secrets.h'
            token = '12345678:' + 'a' * 35
            path.write_text('// #define BOT_TOKEN "ignored"\n#define BOT_TOKEN "' + token + '"\n#define TELEGRAM_OWNER_ID "123456789"\n')
            self.assertEqual(load_settings(path), (token, '123456789'))
            path.write_text('#define BOT_TOKEN "placeholder"\n#define TELEGRAM_OWNER_ID "123456789"\n')
            with self.assertRaises(UploadError):
                load_settings(path)

    def test_document_and_install_button(self):
        blob = b'\xe9\x00\xff\r\n' + MARKER
        md5 = hashlib.md5(blob).hexdigest()
        body, content_type = make_upload('123456789', blob, md5)
        message = BytesParser(policy=default).parsebytes(('Content-Type: ' + content_type + '\r\nMIME-Version: 1.0\r\n\r\n').encode() + body)
        parts = {part.get_param('name', header='content-disposition'): part for part in message.iter_parts()}
        self.assertEqual(parts['document'].get_payload(decode=True), blob)
        self.assertEqual(parts['document'].get_filename(), 'firmware.bin')
        self.assertEqual(parts['caption'].get_payload(decode=True).decode(), 'md5:' + md5)
        markup = json.loads(parts['reply_markup'].get_payload(decode=True))
        self.assertEqual(markup['inline_keyboard'][0][0]['callback_data'], 'ota:' + md5)

    @patch('scripts.send_firmware.subprocess.run', side_effect=__import__('subprocess').CalledProcessError(1, 'pio'))
    @patch('scripts.send_firmware.find_platformio', return_value='pio')
    @patch('scripts.send_firmware.send_firmware')
    def test_build_failure_never_sends(self, send, find, run):
        self.assertEqual(main(['--build']), 1)
        send.assert_not_called()

    @patch('scripts.send_firmware.load_settings', return_value=('token', 'chat'))
    @patch('scripts.send_firmware.read_firmware', return_value=(b'firmware', 'a' * 32))
    @patch('scripts.send_firmware.send_firmware')
    def test_dry_run_never_sends(self, send, image, settings):
        self.assertEqual(main(['--dry-run']), 0)
        send.assert_not_called()

    @patch('scripts.send_firmware.urllib.request.urlopen', side_effect=OSError('private request URL'))
    def test_network_error_hides_url(self, network):
        with self.assertRaises(UploadError) as error:
            send_firmware('private-token', '123', b'bin', 'a' * 32)
        self.assertNotIn('private', str(error.exception))


if __name__ == '__main__':
    unittest.main()
