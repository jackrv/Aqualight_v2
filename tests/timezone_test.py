import datetime
import os
import time
from zoneinfo import ZoneInfo

os.environ['TZ'] = 'EET-2EEST,M3.5.0/3,M10.5.0/4'
time.tzset()
for stamp in ['2026-01-15T12:00:00', '2026-07-15T12:00:00',
              '2026-03-29T00:59:59', '2026-03-29T01:00:00',
              '2026-10-25T00:59:59', '2026-10-25T01:00:00']:
    utc = datetime.datetime.fromisoformat(stamp).replace(tzinfo=datetime.timezone.utc)
    expected = utc.astimezone(ZoneInfo('Europe/Kyiv'))
    actual = time.localtime(utc.timestamp())
    assert tuple(actual[:6]) == (expected.year, expected.month, expected.day,
                                 expected.hour, expected.minute, expected.second), stamp
print('Kyiv winter, summer and DST transitions: PASS')
