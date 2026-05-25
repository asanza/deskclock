import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

// Must match the UUIDs defined in ble.c
const _serviceUuid = "12345678-1234-1234-1234-123456789001";
const _timeCharUuid = "12345678-1234-1234-1234-123456789002";
const _deviceName = "DeskClock";
const _scanTimeout = Duration(seconds: 30);

void main() => runApp(const DeskClockApp());

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

enum _State { idle, scanning, connecting, syncing, done, error }

class SyncPage extends StatefulWidget {
  const SyncPage({super.key});

  @override
  State<SyncPage> createState() => _SyncPageState();
}

class _SyncPageState extends State<SyncPage> {
  _State _state = _State.idle;
  String _message = 'Press sync to update your clock.';
  BluetoothDevice? _device;

  @override
  void initState() {
    super.initState();
    _requestPermissions();
  }

  Future<void> _requestPermissions() async {
    await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.location,
    ].request();
  }

  @override
  void dispose() {
    _device?.disconnect();
    super.dispose();
  }

  Future<void> _sync() async {
    if (!await FlutterBluePlus.isSupported) {
      _setError('Bluetooth not supported on this device.');
      return;
    }

    _setState(_State.scanning, 'Scanning for DeskClock…');

    BluetoothDevice? found;

    // Listen for results, stop as soon as we find the device
    final sub = FlutterBluePlus.onScanResults.listen((results) {
      if (found != null) return;
      for (final r in results) {
        if (r.device.platformName == _deviceName) {
          found = r.device;
          FlutterBluePlus.stopScan();
          break;
        }
      }
    });

    await FlutterBluePlus.startScan(
      withNames: [_deviceName],
      timeout: _scanTimeout,
    );

    // Wait until scanning actually stops (device found → we called stopScan,
    // or the 30 s timeout fired)
    await FlutterBluePlus.isScanning.where((v) => !v).first;
    sub.cancel();

    if (found == null) {
      _setError('DeskClock not found.\nHold the button for 2 s to enable Bluetooth,\nthen try again.');
      return;
    }

    final device = found!;
    _device = device;
    _setState(_State.connecting, 'Connecting…');

    try {
      await device.connect(timeout: const Duration(seconds: 10));
    } catch (_) {
      _setError('Could not connect to DeskClock.');
      return;
    }

    _setState(_State.syncing, 'Syncing time…');

    try {
      final services = await device.discoverServices();
      final svc = services.where(
        (s) => s.serviceUuid == Guid(_serviceUuid),
      ).firstOrNull;

      if (svc == null) {
        _setError('DeskClock service not found.');
        return;
      }

      final chr = svc.characteristics.where(
        (c) => c.characteristicUuid == Guid(_timeCharUuid),
      ).firstOrNull;

      if (chr == null) {
        _setError('Time characteristic not found.');
        return;
      }

      // UTC seconds since epoch as 4-byte little-endian uint32
      final utcSecs = DateTime.now().millisecondsSinceEpoch ~/ 1000;
      final bytes = Uint8List(4)
        ..buffer.asByteData().setUint32(0, utcSecs, Endian.little);

      await chr.write(bytes, withoutResponse: false);
    } catch (e) {
      _setError('Sync failed: $e');
      return;
    } finally {
      await device.disconnect();
      _device = null;
    }

    _setState(_State.done, 'Clock updated!\nYou can close the app.');
  }

  void _setState(_State state, String message) {
    if (mounted) setState(() { _state = state; _message = message; });
  }

  void _setError(String message) {
    if (mounted) setState(() { _state = _State.error; _message = message; });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('DeskClock Sync')),
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              _buildIcon(),
              const SizedBox(height: 32),
              Text(
                _message,
                textAlign: TextAlign.center,
                style: Theme.of(context).textTheme.bodyLarge,
              ),
              const SizedBox(height: 40),
              if (_state == _State.idle || _state == _State.done || _state == _State.error)
                FilledButton.icon(
                  onPressed: _sync,
                  icon: const Icon(Icons.sync),
                  label: Text(_state == _State.done ? 'Sync again' : 'Sync now'),
                ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildIcon() {
    switch (_state) {
      case _State.done:
        return const Icon(Icons.check_circle, size: 120, color: Colors.green);
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
