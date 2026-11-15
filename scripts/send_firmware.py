#!/usr/bin/env python3
"""Build and send AquaLight firmware using only Python's standard library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
MARKER = b"AQUALIGHT:ESP8266:D1_MINI:OTA_V1"


class UploadError(Exception):
    pass


def find_platformio():
    for name in ("pio", "platformio"):
        found = shutil.which(name)
        if found:
            return found
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    for relative in ("penv/bin/pio", "penv/Scripts/platformio.exe", "penv/Scripts/pio.exe"):
        candidate = core / relative
        if candidate.is_file():
            return str(candidate)
    raise UploadError("PlatformIO не найден. Установите расширение PlatformIO IDE.")


def load_settings(path):
    text = path.read_text(encoding="utf-8")
    values = dict(re.findall(r'^\s*#define\s+(BOT_TOKEN|TELEGRAM_OWNER_ID)\s+"([^"\r\n]+)"\s*(?://[^\n]*)?$', text, re.M))
    token, chat = values.get("BOT_TOKEN", ""), values.get("TELEGRAM_OWNER_ID", "")
    if not re.fullmatch(r"[0-9]+:[A-Za-z0-9_-]{20,}", token) or not re.fullmatch(r"[1-9][0-9]*", chat):
        raise UploadError("Заполните BOT_TOKEN и числовой TELEGRAM_OWNER_ID в include/secrets.h.")
    return token, chat


def read_firmware(path):
    blob = path.read_bytes()
    if not 4096 <= len(blob) <= 1044464 or blob[0] != 0xE9 or MARKER not in blob:
        raise UploadError("Нужен firmware.bin AquaLight для D1 mini с поддержкой OTA.")
    md5 = hashlib.md5(blob).hexdigest()
    checksum = path.with_suffix(".bin.md5")
    if checksum.read_text(encoding="ascii").strip() != "md5:" + md5:
        raise UploadError("Контрольная сумма не совпадает. Пересоберите прошивку.")
    return blob, md5


def make_upload(chat, blob, md5):
    boundary = "AquaLight" + uuid.uuid4().hex
    markup = {"inline_keyboard": [[{"text": "🔄 Установить прошивку", "callback_data": "ota:" + md5}]]}
    fields = {"chat_id": chat, "caption": "md5:" + md5,
              "reply_markup": json.dumps(markup, ensure_ascii=False)}
    body = bytearray()
    for key, value in fields.items():
        body.extend((f'--{boundary}\r\nContent-Disposition: form-data; name="{key}"\r\n\r\n{value}\r\n').encode())
    body.extend((f'--{boundary}\r\nContent-Disposition: form-data; name="document"; filename="firmware.bin"\r\nContent-Type: application/octet-stream\r\n\r\n').encode())
    body.extend(blob)
    body.extend(f'\r\n--{boundary}--\r\n'.encode())
    return bytes(body), "multipart/form-data; boundary=" + boundary


def send_firmware(token, chat, blob, md5):
    body, content_type = make_upload(chat, blob, md5)
    request = urllib.request.Request("https://api.telegram.org/bot" + token + "/sendDocument",
                                     data=body, headers={"Content-Type": content_type}, method="POST")
    # Do not poll getUpdates here: that would compete with the controller.
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            result = json.loads(response.read(65536))
    except urllib.error.HTTPError as exc:
        raise UploadError(f"Telegram отклонил отправку (HTTP {exc.code}). Проверьте токен и /start в чате бота.") from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise UploadError("Сетевая ошибка отправки. Проверьте чат перед повтором: документ мог быть доставлен.") from None
    except (ValueError, UnicodeError):
        raise UploadError("Некорректный ответ Telegram. Проверьте чат перед повтором.") from None
    if not result.get("ok"):
        raise UploadError("Telegram не подтвердил отправку. Проверьте чат перед повтором.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Сначала собрать d1_mini")
    parser.add_argument("--dry-run", action="store_true", help="Проверить файл без отправки в Telegram")
    parser.add_argument("--firmware", type=Path, default=ROOT / ".pio/build/d1_mini/firmware.bin")
    args = parser.parse_args(argv)
    try:
        if args.build:
            print("Сборка AquaLight D1 mini…", flush=True)
            subprocess.run([find_platformio(), "run", "-d", str(ROOT), "-e", "d1_mini"], check=True)
        blob, md5 = read_firmware(args.firmware)
        token, chat = load_settings(ROOT / "include/secrets.h")
        print(f"Прошивка проверена: {len(blob)} байт, MD5 {md5}", flush=True)
        if args.dry_run:
            print("Проверка завершена. Отправка в Telegram не выполнялась.")
            return 0
        send_firmware(token, chat, blob, md5)
        print("Файл отправлен в ваш чат с ботом. Нажмите «Установить прошивку» в Telegram.")
        return 0
    except subprocess.CalledProcessError:
        print("Ошибка сборки. Прошивка не отправлена.", file=sys.stderr)
    except UploadError as exc:
        print(str(exc), file=sys.stderr)
    except OSError:
        print("Не удалось прочитать файлы проекта или запустить PlatformIO.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
