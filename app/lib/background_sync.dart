import 'package:flutter/widgets.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'sync_service.dart';

const _secure = FlutterSecureStorage(
  aOptions: AndroidOptions(encryptedSharedPreferences: true),
);

// Called by android_alarm_manager_plus — must be a top-level function.
@pragma('vm:entry-point')
Future<void> backgroundSyncCallback() async {
  WidgetsFlutterBinding.ensureInitialized();

  final prefs  = await SharedPreferences.getInstance();
  final lat    = prefs.getDouble('cached_lat');
  final lon    = prefs.getDouble('cached_lon');
  final owmKey = await _secure.read(key: 'owm_api_key') ?? '';

  if (lat == null || lon == null) return;

  final weather = await fetchWeather(lat, lon, owmKey.isNotEmpty ? owmKey : null);
  if (weather == null) return;

  await syncToDevice(weather);
}
