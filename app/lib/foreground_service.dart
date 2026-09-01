import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:geolocator/geolocator.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'sync_service.dart';

// ---------- public API ----------

void initForegroundTask() {
  FlutterForegroundTask.init(
    androidNotificationOptions: AndroidNotificationOptions(
      channelId: 'deskclock',
      channelName: 'DeskClock Sync',
      channelDescription: 'Keeps your DeskClock synchronised.',
      channelImportance: NotificationChannelImportance.LOW,
      priority: NotificationPriority.LOW,
      playSound: false,
      enableVibration: false,
    ),
    iosNotificationOptions: const IOSNotificationOptions(
      showNotification: false,
    ),
    foregroundTaskOptions: ForegroundTaskOptions(
      eventAction: ForegroundTaskEventAction.nothing(),
      autoRunOnBoot: true,
      allowWakeLock: true,
    ),
  );
}

Future<void> startForegroundService() async {
  if (await FlutterForegroundTask.isRunningService) return;
  await FlutterForegroundTask.startService(
    serviceId: 1,
    notificationTitle: 'DeskClock Sync',
    notificationText: 'Searching for DeskClock…',
    callback: _startCallback,
  );
}

// ---------- task entry point ----------

@pragma('vm:entry-point')
void _startCallback() {
  FlutterForegroundTask.setTaskHandler(_SyncTaskHandler());
}

// ---------- task handler ----------

class _SyncTaskHandler extends TaskHandler {
  static const _secure = FlutterSecureStorage(
    aOptions: AndroidOptions(encryptedSharedPreferences: true),
  );

  BluetoothDevice?         _device;
  BluetoothCharacteristic? _timeChr;
  BluetoothCharacteristic? _weatherChr;
  BluetoothCharacteristic? _uptimeChr;
  BluetoothCharacteristic? _battChr;
  StreamSubscription<BluetoothConnectionState>? _connSub;

  bool         _connected  = false;
  bool         _connecting = false;
  int?         _battery;
  int?         _uptimeSecs;
  WeatherData? _cachedWeather;

  Timer? _minuteTimer;
  Timer? _weatherTimer;
  Timer? _reconnectTimer;

  // ---- lifecycle ----

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    _unawaited(_connect());
    _startTimers();
  }

  @override
  Future<void> onRepeatEvent(DateTime timestamp) async {}

  @override
  Future<void> onDestroy(DateTime timestamp) async {
    _minuteTimer?.cancel();
    _weatherTimer?.cancel();
    _reconnectTimer?.cancel();
    _connSub?.cancel();
    try { await _device?.disconnect(); } catch (_) {}
  }

  @override
  void onReceiveData(Object data) {
    if (data == 'sync_now')       _unawaited(_forcePush());
    if (data == 'request_status') _unawaited(_sendStatus());
  }

  // ---- timers ----

  void _startTimers() {
    _minuteTimer = Timer.periodic(
      const Duration(minutes: 1),
      (_) => _unawaited(_pushTime()),
    );
    _weatherTimer = Timer.periodic(
      const Duration(hours: 1),
      (_) => _unawaited(_fetchAndPushWeather()),
    );
  }

  void _scheduleReconnect() {
    _reconnectTimer?.cancel();
    _reconnectTimer = Timer(
      const Duration(seconds: 15),
      () => _unawaited(_connect()),
    );
    _unawaited(_sendStatus());
  }

  // ---- BLE connection ----

  Future<void> _connect() async {
    if (_connecting) return;
    _connecting = true;
    _device = _timeChr = _weatherChr = _battChr = _uptimeChr = null;

    try {
      if (await FlutterBluePlus.adapterState.first != BluetoothAdapterState.on) {
        _scheduleReconnect();
        return;
      }

      BluetoothDevice? found;
      final sub = FlutterBluePlus.onScanResults.listen((results) {
        for (final r in results) {
          if (r.device.platformName == kDeviceName && found == null) {
            found = r.device;
            FlutterBluePlus.stopScan();
          }
        }
      });

      await FlutterBluePlus.startScan(
        withNames: [kDeviceName],
        timeout: const Duration(seconds: 30),
      );
      await FlutterBluePlus.isScanning.where((v) => !v).first;
      sub.cancel();

      if (found == null) { _scheduleReconnect(); return; }

      await found!.connect(timeout: const Duration(seconds: 10));
      await found!.requestMtu(256);

      final services = await found!.discoverServices();

      final svc = services.where(
        (s) => s.serviceUuid == Guid(kServiceUuid),
      ).firstOrNull;
      final battSvc = services.where(
        (s) => s.serviceUuid == Guid(kBatteryServiceUuid),
      ).firstOrNull;

      if (svc == null) {
        await found!.disconnect();
        _scheduleReconnect();
        return;
      }

      _timeChr    = svc.characteristics.where((c) => c.characteristicUuid == Guid(kTimeCharUuid)).firstOrNull;
      _weatherChr = svc.characteristics.where((c) => c.characteristicUuid == Guid(kWeatherCharUuid)).firstOrNull;
      _uptimeChr  = svc.characteristics.where((c) => c.characteristicUuid == Guid(kUptimeCharUuid)).firstOrNull;
      _battChr    = battSvc?.characteristics.where((c) => c.characteristicUuid == Guid(kBatteryLevelCharUuid)).firstOrNull;

      _device    = found;
      _connected = true;

      _connSub?.cancel();
      _connSub = found!.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected && _connected) {
          _connected  = false;
          _battery    = null;
          _uptimeSecs = null;
          _scheduleReconnect();
        }
      });

      // Initial push on connect
      await _pushTime();
      await _fetchAndPushWeather();
      await _readBatteryAndUptime();
      _unawaited(_sendStatus());

    } catch (_) {
      _connected = false;
      _scheduleReconnect();
    } finally {
      _connecting = false;
    }
  }

  // ---- BLE writes/reads ----

  Future<void> _pushTime() async {
    if (!_connected || _timeChr == null) return;
    try {
      final utcSecs = DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000;
      final bytes = Uint8List(4)
        ..buffer.asByteData().setUint32(0, utcSecs, Endian.little);
      await _timeChr!.write(bytes, withoutResponse: false);
    } catch (_) {}
  }

  Future<void> _pushWeather(WeatherData wd) async {
    if (!_connected || _weatherChr == null) return;
    try {
      await _weatherChr!.write(packWeather(wd), withoutResponse: false);
    } catch (_) {}
  }

  Future<void> _fetchAndPushWeather() async {
    final prefs = await SharedPreferences.getInstance();

    double? lat, lon;
    try {
      final perm = await Geolocator.checkPermission();
      if (perm == LocationPermission.always ||
          perm == LocationPermission.whileInUse) {
        final pos = await Geolocator.getLastKnownPosition() ??
            await Geolocator.getCurrentPosition(
              locationSettings: const LocationSettings(
                accuracy: LocationAccuracy.lowest,
                timeLimit: Duration(seconds: 15),
              ),
            );
        lat = pos.latitude;
        lon = pos.longitude;
        await prefs.setDouble('cached_lat', lat);
        await prefs.setDouble('cached_lon', lon);
      }
    } catch (_) {}

    lat ??= prefs.getDouble('cached_lat');
    lon ??= prefs.getDouble('cached_lon');
    if (lat == null || lon == null) return;

    final owmKey = await _secure.read(key: 'owm_api_key') ?? '';
    final weather = await fetchWeather(lat, lon, owmKey.isNotEmpty ? owmKey : null);
    if (weather == null) return;

    _cachedWeather = weather;
    await _pushWeather(weather);

    await prefs.setInt('last_sync_ts', DateTime.now().millisecondsSinceEpoch);
    await prefs.setString('last_wx_str', weather.toDisplayString());
    _unawaited(_sendStatus());
  }

  Future<void> _readBatteryAndUptime() async {
    try {
      if (_battChr != null) {
        final data = await _battChr!.read();
        if (data.isNotEmpty) _battery = data[0];
      }
    } catch (_) {}

    try {
      if (_uptimeChr != null) {
        final data = await _uptimeChr!.read();
        if (data.length >= 4) {
          _uptimeSecs = ByteData.sublistView(Uint8List.fromList(data))
              .getUint32(0, Endian.little);
        }
      }
    } catch (_) {}
  }

  Future<void> _forcePush() async {
    await _pushTime();
    if (_cachedWeather != null) await _pushWeather(_cachedWeather!);
    await _readBatteryAndUptime();
    _unawaited(_sendStatus());
  }

  // ---- status → main isolate ----

  Future<void> _sendStatus() async {
    final prefs     = await SharedPreferences.getInstance();
    final lastSyncTs = prefs.getInt('last_sync_ts');
    final lastWx    = prefs.getString('last_wx_str') ?? '';

    FlutterForegroundTask.sendDataToMain({
      'connected':  _connected,
      'battery':    _battery,
      'uptimeSecs': _uptimeSecs,
      'lastSyncTs': lastSyncTs,
      'lastWx':     lastWx,
    });

    await FlutterForegroundTask.updateService(
      notificationTitle: 'DeskClock Sync',
      notificationText: _connected
          ? (_battery != null ? 'Connected · $_battery%' : 'Connected')
          : 'Searching for DeskClock…',
    );
  }
}

void _unawaited(Future<void> f) => f.catchError((_) {});
