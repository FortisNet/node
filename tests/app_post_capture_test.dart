// From app/: flutter test ../tests/app_post_capture_test.dart
// --dart-define=FORTIS_POST_CAPTURE=/absolute/output.json
// Real app API methods and Dio stream, synthetic inputs, no network/credentials.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:dio/dio.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:fortis_net_wallet/data/intelligence/intelligence_api.dart';
import 'package:fortis_net_wallet/data/intelligence/intelligence_conversation_api.dart';
import 'package:fortis_net_wallet/data/intelligence/intelligence_sync_client.dart';
import 'package:fortis_net_wallet/data/intelligence/generated/fortisnet/intelligence/v1/devices.pb.dart' as devices;

class _Capture implements HttpClientAdapter {
  final rows = <Map<String, Object?>>[];
  @override
  Future<ResponseBody> fetch(RequestOptions options, Stream<Uint8List>? stream,
      Future<void>? cancelFuture) async {
    final bytes = <int>[];
    if (stream != null) { await for (final chunk in stream) { bytes.addAll(chunk); } }
    rows.add({'method': options.method, 'path': options.path, 'contentType': options.contentType,
      'hex': bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join()});
    if (options.path == '/v1/devices/challenge') {
      return ResponseBody.fromBytes(devices.AttestationChallengeResponse(
        challenge: 'development-challenge', expiresIn: 300).writeToBuffer(), 200,
        headers: {Headers.contentTypeHeader: ['application/protobuf']});
    }
    return ResponseBody.fromBytes(utf8.encode('{"error":"CAPTURE_ONLY"}'), 503,
      headers: {Headers.contentTypeHeader: ['application/json']});
  }
  @override
  void close({bool force = false}) {}
}

void main() {
  test('capture every app POST through API methods and Dio', () async {
    const output = String.fromEnvironment('FORTIS_POST_CAPTURE');
    expect(output, isNotEmpty, reason: 'Set --dart-define=FORTIS_POST_CAPTURE=/absolute/output.json');
    final capture = _Capture();
    final dio = Dio(BaseOptions(baseUrl: 'https://fixture.invalid'))..httpClientAdapter = capture;
    final api = IntelligenceApi(dio, deviceToken: 'fixture-device-token');
    Future<void> send(Future<dynamic> Function() call) async {
      final count = capture.rows.length;
      await expectLater(call(), throwsA(isA<DioException>()));
      expect(capture.rows.length, greaterThan(count), reason: 'API must reach the wire');
    }
    const address = '0x1111111111111111111111111111111111111111';
    final calls = <Map<String, String>>[{'to': '0x2222222222222222222222222222222222222222',
      'data': '0x', 'value': '0x0', 'gas': '0x5208'}];
    await send(() => api.registerDevice(platform: 'ios', appVersion: '1.0'));
    await send(api.rotateDeviceToken);
    await send(api.revokeDeviceToken);
    await send(() => IntelligenceSyncClient(dio, deviceToken: () => 'fixture-device-token')
      .fetch(cursor: 's1_previous', limit: 50));
    await send(() => api.priceHistoryBatch(['bitcoin'], from: DateTime.utc(2026, 1, 1), to: DateTime.utc(2026, 1, 2)));
    await send(() => api.tokenMarkets(['bitcoin']));
    await send(() => api.tokenComparison([(network: 'eth-mainnet', tokenId: 'native')]));
    await send(() => api.bittensorAnalytics('/api/extrinsic/v1', {'limit': 20, 'network': 'finney', 'page': 1}));
    await send(() => api.submitCrashReport({'platform': 'ios', 'appVersion': '1.0', 'buildMode': 'release',
      'source': 'flutter', 'fatal': false, 'errorType': 'StateError', 'message': 'fixture failure',
      'stack': 'fixture stack', 'occurredAt': '2026-01-01T00:00:00Z'}));
    await send(() => api.diagnoseRobinhoodTransaction(from: address, calls: calls, sponsored: true));
    await send(() => api.prepareRobinhoodSponsoredTrade(from: address, calls: calls));
    await send(() => api.submitRobinhoodSponsoredTrade(ticket: 'fixture-ticket', signatures: ['0x1234']));
    await send(() => api.crossChainQuotes({'from': {'chainId': '1', 'token': 'native', 'decimals': 18, 'address': address},
      'to': {'chainId': '56', 'token': 'native', 'decimals': 18, 'address': address},
      'amountIn': '1000000000000000', 'slippageBps': 50, 'prepare': false, 'providers': ['lifi']}));
    await send(() => api.crossChainStatus({'providerId': 'lifi', 'txHash': '0x' + 'ab' * 32,
      'fromChainId': '1', 'toChainId': '56', 'toToken': 'native', 'toSymbol': 'BNB', 'toDecimals': 18}));
    final conversation = IntelligenceConversationApi(dio);
    const id = '01JZ0000000000000000000000';
    await send(() => conversation.open(deviceToken: 'fixture-device-token', request: {
      'address': address, 'nonce': 'a' * 32, 'timestamp': '2026-01-01T00:00:00Z',
      'snapshotHash': 'b' * 64, 'accessToken': 'c' * 43, 'accessTokenHash': 'd' * 64,
      'snapshotJson': '{"version":2}', 'signature': '0x' + '00' * 65,
      'firstMessage': 'Fixture question', 'clientMessageId': 'e' * 32}));
    await send(() => conversation.send(deviceToken: 'fixture-device-token', accessToken: 'fixture-session',
      conversationId: id, clientMessageId: 'f' * 32, body: 'Fixture follow-up',
      attachment: PortfolioMessageAttachment(snapshotJson: '{"version":2}', sha256: 'b' * 64)));
    await send(() => conversation.retry(deviceToken: 'fixture-device-token', accessToken: 'fixture-session',
      conversationId: id, messageId: '01JZ0000000000000000000001'));
    expect(capture.rows, hasLength(18));
    expect(capture.rows.map((r) => r['path']).toSet(), hasLength(18));
    File(output).writeAsStringSync('${const JsonEncoder.withIndent('  ').convert({
      'description': 'App API/Dio wire captures with synthetic fixture inputs; no real credentials/signatures',
      'requests': capture.rows})}\n');
  });
}
