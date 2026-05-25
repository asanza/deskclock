import 'dart:async';

import 'package:android_alarm_manager_plus/android_alarm_manager_plus.dart';
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'background_sync.dart';
import 'sync_service.dart';

const _alarmId = 42;

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await AndroidAlarmManager.initialize();
  runApp(const DeskClockApp());
}

class DeskClockApp extends StatelessWidget {
  const DeskClockApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DeskClock',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      home: const SyncPage(),
    );
  }
}

enum _State { idle, fetching, scanning, done, error }

class SyncPage extends StatefulWidget {
  const SyncPage({super.key});

  @override
  State<SyncPage> createState() => _SyncPageState();
}

class _SyncPageState extends State<SyncPage> {
  _State _state  = _State.idle;
  String _msg    = 'Press sync to update your clock.';
  String _lastWx = '';
  String _lastAt = '';
  bool   _autoSync = false;

  final _apiKeyCtrl = TextEditingController();

  @override
  void initState() {
    super.initState();
    _requestPermissions();
    _loadPrefs();
  }

  @override
  void dispose() {
    _apiKeyCtrl.dispose();
    super.dispose();
  }

  Future<void> _requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();
  }

  Future<void> _loadPrefs() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      _apiKeyCtrl.text = prefs.getString('owm_api_key') ?? '';
      _autoSync        = prefs.getBool('auto_sync') ?? false;
      _lastWx          = prefs.getString('last_wx_str') ?? '';
      _lastAt          = prefs.getString('last_sync_at') ?? '';
    });
  }

  Future<void> _saveApiKey(String key) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString('owm_api_key', key);
  }

  Future<void> _setAutoSync(bool enable) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool('auto_sync', enable);
    setState(() => _autoSync = enable);

    if (enable) {
      // Schedule first fire at the next UTC HH:00:10
      final now      = DateTime.now().toUtc();
      final nextHour = DateTime.utc(now.year, now.month, now.day, now.hour + 1, 0, 10);
      await AndroidAlarmManager.periodic(
        const Duration(hours: 1),
        _alarmId,
        backgroundSyncCallback,
        startAt:            nextHour,
        exact:              true,
        wakeup:             true,
        rescheduleOnReboot: true,
      );
    } else {
      await AndroidAlarmManager.cancel(_alarmId);
    }
  }

  Future<void> _sync() async {
    _setStatus(_State.fetching, 'Fetching weather…');

    // Get location — try fresh, fall back to cached
    double? lat, lon;
    final pos = await getFreshPosition();
    if (pos != null) {
      lat = pos.latitude;
      lon = pos.longitude;
      await cachePosition(lat, lon);
    } else {
      final cached = await loadCachedPosition();
      if (cached != null) {
        lat = cached['lat'];
        lon = cached['lon'];
      }
    }

    WeatherData? weather;
    if (lat != null && lon != null) {
      final owmKey = _apiKeyCtrl.text.trim();
      weather = await fetchWeather(lat, lon, owmKey.isNotEmpty ? owmKey : null);
    }

    _setStatus(_State.scanning, 'Scanning for DeskClock…');

    final result = weather != null
        ? await syncToDevice(weather)
        : 'No location available';

    if (result == 'ok') {
      final wxStr = weather!.toDisplayString();
      final now   = DateTime.now();
      final at    = '${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}';
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString('last_wx_str', wxStr);
      await prefs.setString('last_sync_at', at);
      setState(() { _lastWx = wxStr; _lastAt = at; });
      _setStatus(_State.done, 'Clock updated!');
    } else {
      _setStatus(_State.error, result);
    }
  }

  void _setStatus(_State state, String msg) {
    if (mounted) setState(() { _state = state; _msg = msg; });
  }

  @override
  Widget build(BuildContext context) {
    final busy = _state != _State.idle &&
                 _state != _State.done &&
                 _state != _State.error;
    return Scaffold(
      appBar: AppBar(title: const Text('DeskClock Sync')),
      body: ListView(
        padding: const EdgeInsets.all(24),
        children: [
          const SizedBox(height: 16),
          Center(child: _buildIcon()),
          const SizedBox(height: 24),
          Center(
            child: Text(
              _msg,
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.bodyLarge,
            ),
          ),
          if (_lastWx.isNotEmpty) ...[
            const SizedBox(height: 8),
            Center(
              child: Text(
                '$_lastWx  ($_lastAt)',
                textAlign: TextAlign.center,
                style: Theme.of(context).textTheme.bodySmall?.copyWith(color: Colors.grey),
              ),
            ),
          ],
          const SizedBox(height: 32),
          if (!busy)
            Center(
              child: FilledButton.icon(
                onPressed: _sync,
                icon: const Icon(Icons.sync),
                label: Text(_state == _State.done ? 'Sync again' : 'Sync now'),
              ),
            ),
          const SizedBox(height: 40),
          const Divider(),
          const SizedBox(height: 16),
          Text('Settings', style: Theme.of(context).textTheme.titleSmall),
          const SizedBox(height: 12),
          TextField(
            controller: _apiKeyCtrl,
            decoration: const InputDecoration(
              labelText: 'OpenWeatherMap API key (optional)',
              helperText: 'Needed for severe weather alerts only.',
              border: OutlineInputBorder(),
            ),
            onChanged: _saveApiKey,
          ),
          const SizedBox(height: 16),
          SwitchListTile(
            title: const Text('Auto-sync every hour'),
            subtitle: const Text('Clock must be awake at HH:00 UTC'),
            value: _autoSync,
            onChanged: busy ? null : _setAutoSync,
            contentPadding: EdgeInsets.zero,
          ),
        ],
      ),
    );
  }

  Widget _buildIcon() {
    switch (_state) {
      case _State.done:
        return const Icon(Icons.check_circle, size: 100, color: Colors.green);
      case _State.error:
        return const Icon(Icons.error_outline, size: 80, color: Colors.red);
      case _State.idle:
        return const Icon(Icons.watch, size: 80, color: Colors.indigo);
      default:
        return const SizedBox(
          width: 80,
          height: 80,
          child: CircularProgressIndicator(strokeWidth: 6),
        );
    }
  }
}
