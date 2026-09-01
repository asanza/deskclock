import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'foreground_service.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  initForegroundTask();
  runApp(const DeskClockApp());
}

class DeskClockApp extends StatelessWidget {
  const DeskClockApp({super.key});

  @override
  Widget build(BuildContext context) {
    return WithForegroundTask(
      child: MaterialApp(
        title: 'DeskClock',
        theme: ThemeData(
          colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
          useMaterial3: true,
        ),
        home: const SyncPage(),
      ),
    );
  }
}

class SyncPage extends StatefulWidget {
  const SyncPage({super.key});

  @override
  State<SyncPage> createState() => _SyncPageState();
}

class _SyncPageState extends State<SyncPage> with WidgetsBindingObserver {
  bool   _serviceRunning = false;
  bool   _connected      = false;
  int?   _battery;
  int?   _uptimeSecs;
  String _lastWx         = '';
  String _lastSync       = '';

  final _apiKeyCtrl = TextEditingController();
  static const _secure = FlutterSecureStorage(
    aOptions: AndroidOptions(encryptedSharedPreferences: true),
  );

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    FlutterForegroundTask.addTaskDataCallback(_onTaskData);
    _init();
  }

  @override
  void dispose() {
    FlutterForegroundTask.removeTaskDataCallback(_onTaskData);
    WidgetsBinding.instance.removeObserver(this);
    _apiKeyCtrl.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      // Re-request current status from the task handler when foregrounded
      FlutterForegroundTask.sendDataToTask('request_status');
    }
  }

  Future<void> _init() async {
    await _requestPermissions();
    await _loadPrefs();
    await startForegroundService();
    setState(() => _serviceRunning = true);
  }

  Future<void> _requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();
    await FlutterForegroundTask.requestNotificationPermission();
  }

  Future<void> _loadPrefs() async {
    final prefs  = await SharedPreferences.getInstance();
    final apiKey = await _secure.read(key: 'owm_api_key') ?? '';
    final tsMs   = prefs.getInt('last_sync_ts');
    setState(() {
      _apiKeyCtrl.text = apiKey;
      _lastWx          = prefs.getString('last_wx_str') ?? '';
      _lastSync        = tsMs != null ? _formatSyncTime(tsMs) : '';
    });
  }

  void _onTaskData(Object data) {
    if (data is! Map) return;
    final tsMs = data['lastSyncTs'] as int?;
    setState(() {
      _connected  = data['connected']  as bool?  ?? false;
      _battery    = data['battery']    as int?;
      _uptimeSecs = data['uptimeSecs'] as int?;
      _lastWx     = data['lastWx']     as String? ?? _lastWx;
      _lastSync   = tsMs != null ? _formatSyncTime(tsMs) : _lastSync;
    });
  }

  Future<void> _saveApiKey(String key) async {
    await _secure.write(key: 'owm_api_key', value: key);
  }

  String _formatSyncTime(int tsMs) {
    final t    = DateTime.fromMillisecondsSinceEpoch(tsMs);
    final now  = DateTime.now();
    final hhmm = '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
    final isToday     = t.year == now.year && t.month == now.month && t.day == now.day;
    final isYesterday = DateTime(t.year, t.month, t.day)
        .isAtSameMomentAs(DateTime(now.year, now.month, now.day - 1));
    if (isToday)     return 'Today $hhmm';
    if (isYesterday) return 'Yesterday $hhmm';
    const days = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];
    return '${days[t.weekday - 1]} $hhmm';
  }

  String _formatUptime(int? secs) {
    if (secs == null) return '';
    final d = secs ~/ 86400;
    final h = (secs % 86400) ~/ 3600;
    final m = (secs % 3600)  ~/ 60;
    if (d > 0) return '${d}d ${h}h';
    if (h > 0) return '${h}h ${m}m';
    return '${m}m';
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);

    return Scaffold(
      appBar: AppBar(title: const Text('DeskClock Sync')),
      body: ListView(
        padding: const EdgeInsets.all(24),
        children: [
          const SizedBox(height: 16),
          Center(child: _buildStatusIcon()),
          const SizedBox(height: 16),
          Center(
            child: Text(
              _connected ? 'Connected' : (_serviceRunning ? 'Searching…' : 'Starting…'),
              style: theme.textTheme.titleMedium,
            ),
          ),

          // Battery + uptime row
          if (_connected) ...[
            const SizedBox(height: 8),
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                if (_battery != null) ...[
                  const Icon(Icons.battery_full, size: 16, color: Colors.grey),
                  const SizedBox(width: 4),
                  Text('$_battery%',
                      style: theme.textTheme.bodySmall?.copyWith(color: Colors.grey)),
                ],
                if (_battery != null && _uptimeSecs != null)
                  const Text('  ·  ',
                      style: TextStyle(color: Colors.grey)),
                if (_uptimeSecs != null)
                  Text('Up ${_formatUptime(_uptimeSecs)}',
                      style: theme.textTheme.bodySmall?.copyWith(color: Colors.grey)),
              ],
            ),
          ],

          // Last sync + weather
          if (_lastSync.isNotEmpty) ...[
            const SizedBox(height: 8),
            Center(
              child: Text(
                _lastWx.isNotEmpty ? '$_lastWx  ·  $_lastSync' : 'Last sync: $_lastSync',
                textAlign: TextAlign.center,
                style: theme.textTheme.bodySmall?.copyWith(color: Colors.grey),
              ),
            ),
          ],

          const SizedBox(height: 24),
          if (_connected)
            Center(
              child: OutlinedButton.icon(
                onPressed: () =>
                    FlutterForegroundTask.sendDataToTask('sync_now'),
                icon: const Icon(Icons.sync, size: 18),
                label: const Text('Force sync'),
              ),
            ),

          const SizedBox(height: 40),
          const Divider(),
          const SizedBox(height: 16),
          Text('Settings', style: theme.textTheme.titleSmall),
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
        ],
      ),
    );
  }

  Widget _buildStatusIcon() {
    if (_connected) {
      return const Icon(Icons.watch, size: 80, color: Colors.green);
    }
    if (_serviceRunning) {
      return const SizedBox(
        width: 80, height: 80,
        child: CircularProgressIndicator(strokeWidth: 6),
      );
    }
    return const Icon(Icons.watch, size: 80, color: Colors.grey);
  }
}
