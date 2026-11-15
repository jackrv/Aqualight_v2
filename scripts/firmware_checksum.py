"""PlatformIO post-build: create a Telegram caption next to firmware.bin."""
from pathlib import Path
import hashlib

Import("env")


def firmware_checksum(source, target, env):
    firmware = Path(str(target[0]))
    caption = "md5:" + hashlib.md5(firmware.read_bytes()).hexdigest()
    firmware.with_suffix(".bin.md5").write_text(caption + "\n", encoding="ascii")
    print("Telegram OTA caption: " + caption)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", firmware_checksum)
