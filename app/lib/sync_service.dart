import 'dart:async';
import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:geocoding/geocoding.dart';
import 'package:geolocator/geolocator.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';

// Must match UUIDs in ble.c
const kServiceUuid     = "12345678-1234-1234-1234-123456789001";
const kWeatherCharUuid = "12345678-1234-1234-1234-123456789002";
const kTimeCharUuid    = "12345678-1234-1234-1234-123456789003";
const kDeviceName      = "DeskClock";
const _scanTimeout     = Duration(seconds: 30);

class WeatherData {
  final int    temperature;   // Celsius, int8
  final int    condition;     // weather_condition_t 0-7
  final int    rainProb;      // 0-100 %
  final int    alertLevel;    // 0-4
  final String location;      // city name, max 19 chars
  final String alertText;     // alert description, max 47 chars

  const WeatherData({
    required this.temperature,
    required this.condition,
    required this.rainProb,
    required this.alertLevel,
    required this.location,
    required this.alertText,
  });

  String toDisplayString() {
    if (alertLevel >= 3) return '! ${alertText.isNotEmpty ? alertText : "WEATHER ALERT"}';
    final cond = _conditionName(condition);
    final base = '$location ${temperature}C $cond';
    return rainProb >= 50 ? '$base $rainProb%' : base;
  }
}

String _conditionName(int c) {
  const names = ['Clear', 'Pt cloudy', 'Overcast', 'Fog', 'Drizzle', 'Rain', 'Snow', 'Storm'];
  return c >= 0 && c < names.length ? names[c] : '';
}

// ---------- location ----------

/// Returns (position, errorMessage). errorMessage is null on success.
Future<(Position?, String?)> getFreshPosition() async {
  if (!await Geolocator.isLocationServiceEnabled()) {
    return (null, 'Location services are off — enable in Android Settings → Location');
  }

  LocationPermission perm = await Geolocator.checkPermission();
  if (perm == LocationPermission.denied) {
    perm = await Geolocator.requestPermission();
  }
  if (perm == LocationPermission.deniedForever) {
    return (null, 'Location permission permanently denied — grant in App Info → Permissions');
  }
  if (perm == LocationPermission.denied) {
    return (null, 'Location permission denied');
  }

  try {
    // Last-known is instant (uses cached OS value); fallback to fresh fix.
    final pos = await Geolocator.getLastKnownPosition() ??
        await Geolocator.getCurrentPosition(
          locationSettings: const LocationSettings(
            accuracy: LocationAccuracy.lowest,
            timeLimit: Duration(seconds: 30),
          ),
        );
    return (pos, null);
  } catch (e) {
    return (null, 'Location error: $e');
  }
}

Future<void> cachePosition(double lat, double lon) async {
  final prefs = await SharedPreferences.getInstance();
  await prefs.setDouble('cached_lat', lat);
  await prefs.setDouble('cached_lon', lon);
}

Future<Map<String, double>?> loadCachedPosition() async {
  final prefs = await SharedPreferences.getInstance();
  final lat = prefs.getDouble('cached_lat');
  final lon = prefs.getDouble('cached_lon');
  if (lat == null || lon == null) return null;
  return {'lat': lat, 'lon': lon};
}

// ---------- weather fetch ----------

Future<WeatherData?> fetchWeather(double lat, double lon, String? owmApiKey) async {
  // Open-Meteo — free, no key
  final meteoUrl = Uri.parse(
    'https://api.open-meteo.com/v1/forecast'
    '?latitude=$lat&longitude=$lon'
    '&current=temperature_2m,weather_code'
    '&hourly=precipitation_probability'
    '&forecast_days=1&timezone=UTC',
  );

  final http.Response meteoResp;
  try {
    meteoResp = await http.get(meteoUrl).timeout(const Duration(seconds: 15));
  } catch (_) {
    return null;
  }
  if (meteoResp.statusCode != 200) return null;

  final m       = jsonDecode(meteoResp.body);
  final temp    = (m['current']['temperature_2m'] as num).round();
  final wmo     = (m['current']['weather_code']  as num).toInt();
  final utcHour = DateTime.now().toUtc().hour;
  final precip  = m['hourly']['precipitation_probability'] as List;
  final rain    = precip.length > utcHour + 1 ? (precip[utcHour + 1] as num).toInt() : 0;

  // Reverse geocoding via OS geocoder
  String city = '';
  try {
    final marks = await placemarkFromCoordinates(lat, lon);
    if (marks.isNotEmpty) city = marks.first.locality ?? marks.first.name ?? '';
  } catch (_) {}

  // OpenWeatherMap One Call 3.0 for alerts (optional)
  int    alertLevel = 0;
  String alertText  = '';
  if (owmApiKey != null && owmApiKey.isNotEmpty) {
    try {
      final owmUrl = Uri.parse(
        'https://api.openweathermap.org/data/3.0/onecall'
        '?lat=$lat&lon=$lon&appid=$owmApiKey'
        '&exclude=current,minutely,hourly,daily',
      );
      final owmResp = await http.get(owmUrl).timeout(const Duration(seconds: 15));
      if (owmResp.statusCode == 200) {
        final alerts = (jsonDecode(owmResp.body)['alerts'] as List?) ?? [];
        if (alerts.isNotEmpty) {
          final event = (alerts.first['event'] as String?) ?? 'Weather Alert';
          alertLevel  = _owmAlertLevel(event);
          alertText   = event;
        }
      }
    } catch (_) {}
  }

  return WeatherData(
    temperature: temp,
    condition:   _wmoToCondition(wmo),
    rainProb:    rain,
    alertLevel:  alertLevel,
    location:    city,
    alertText:   alertText,
  );
}

int _wmoToCondition(int wmo) {
  if (wmo == 0)                               { return 0; } // CLEAR
  if (wmo <= 2)                               { return 1; } // PARTLY_CLOUDY
  if (wmo == 3)                               { return 2; } // OVERCAST
  if (wmo <= 48)                              { return 3; } // FOG
  if (wmo <= 55)                              { return 4; } // DRIZZLE
  if (wmo <= 65 || (wmo >= 80 && wmo <= 82)) { return 5; } // RAIN
  if (wmo <= 77 || wmo == 85 || wmo == 86)   { return 6; } // SNOW
  return 7; // STORM
}

int _owmAlertLevel(String event) {
  final e = event.toLowerCase();
  if (e.contains('extreme') || e.contains('hurricane') ||
      e.contains('tornado') || e.contains('typhoon'))   { return 4; }
  if (e.contains('severe') || e.contains('warning') ||
      e.contains('danger'))                              { return 3; }
  if (e.contains('moderate') || e.contains('watch'))    { return 2; }
  return 1;
}

// ---------- BLE write ----------

/// Pack WeatherData into the 72-byte binary layout the ESP32 expects.
Uint8List packWeather(WeatherData wd) {
  final buf = Uint8List(72);
  final bd  = ByteData.sublistView(buf);

  bd.setInt8( 0, wd.temperature.clamp(-128, 127));
  bd.setUint8(1, wd.condition.clamp(0, 7));
  bd.setUint8(2, wd.rainProb.clamp(0, 100));
  bd.setUint8(3, wd.alertLevel.clamp(0, 4));

  _writeFixedStr(buf,  4, 20, wd.location);
  _writeFixedStr(buf, 24, 48, wd.alertText);

  return buf;
}

void _writeFixedStr(Uint8List buf, int offset, int maxLen, String s) {
  final bytes = utf8.encode(s);
  final n     = min(bytes.length, maxLen - 1);
  for (int i = 0; i < n; i++) { buf[offset + i] = bytes[i]; }
  buf[offset + n] = 0;
}

/// Scan, connect, push weather (optional) + UTC time to the DeskClock.
/// Returns a human-readable result string; 'ok' means success.
Future<String> syncToDevice(WeatherData? weather) async {
  BluetoothDevice? found;

  final sub = FlutterBluePlus.onScanResults.listen((results) {
    if (found != null) return;
    for (final r in results) {
      if (r.device.platformName == kDeviceName) {
        found = r.device;
        FlutterBluePlus.stopScan();
        break;
      }
    }
  });

  await FlutterBluePlus.startScan(withNames: [kDeviceName], timeout: _scanTimeout);
  await FlutterBluePlus.isScanning.where((v) => !v).first;
  sub.cancel();

  if (found == null) return 'DeskClock not found';

  final device = found!;
  try {
    await device.connect(timeout: const Duration(seconds: 10));
    await device.requestMtu(256);

    final services = await device.discoverServices();
    final svc = services.where(
      (s) => s.serviceUuid == Guid(kServiceUuid),
    ).firstOrNull;
    if (svc == null) return 'DeskClock service not found';

    final wxChr = svc.characteristics
        .where((c) => c.characteristicUuid == Guid(kWeatherCharUuid))
        .firstOrNull;
    final timeChr = svc.characteristics
        .where((c) => c.characteristicUuid == Guid(kTimeCharUuid))
        .firstOrNull;
    if (wxChr == null || timeChr == null) return 'Characteristics not found';

    if (weather != null) {
      await wxChr.write(packWeather(weather), withoutResponse: false);
    }

    final utcSecs  = DateTime.now().millisecondsSinceEpoch ~/ 1000;
    final timeBytes = Uint8List(4)
      ..buffer.asByteData().setUint32(0, utcSecs, Endian.little);
    await timeChr.write(timeBytes, withoutResponse: false);

    return 'ok';
  } catch (e) {
    return 'Error: $e';
  } finally {
    try { await device.disconnect(); } catch (_) {}
  }
}
