// Installed-wallet transport qualification, using Dart's actual HttpClient.
// Generated local CA only; no certificate bypass or real device credential.
import 'dart:convert';
import 'dart:io';

Future<void> main(List<String> args) async {
  if (args.length != 2) throw ArgumentError('BASE_URL CA_FILE required');
  final context = SecurityContext(withTrustedRoots: false)
    ..setTrustedCertificates(args[1]);
  final client = HttpClient(context: context)
    ..connectionTimeout = const Duration(seconds: 5)
    ..idleTimeout = const Duration(seconds: 15);
  final ports = <int>{};
  Future<void> health() async {
    final request = await client.getUrl(Uri.parse('${args[0]}/healthz'));
    request.persistentConnection = true;
    final response = await request.close();
    if (response.statusCode != 200 || response.certificate == null) {
      throw StateError('verified health request failed');
    }
    final info = response.connectionInfo;
    if (info != null) ports.add(info.localPort);
    final body = await response.transform(utf8.decoder).join();
    if ((jsonDecode(body) as Map<String, dynamic>)['ok'] != true) {
      throw StateError('invalid health body');
    }
  }
  try {
    for (var index = 0; index < 20; index++) {
      await health();
    }
    if (ports.isEmpty || ports.length > 2) throw StateError('sequential connections were not reused: ${ports.length}');
    final sequentialConnections = ports.length;
    await Future.wait(List.generate(20, (_) => health()));
    final request = await client.getUrl(Uri.parse('${args[0]}/v1/prices?ids=bitcoin'));
    request.headers.set(HttpHeaders.acceptHeader, 'application/protobuf');
    final response = await request.close();
    if (response.statusCode != 401) throw StateError('device authentication changed');
    await response.drain<void>();
    stdout.writeln(jsonEncode({'dart_http1_tls': 'passed', 'health_requests': 40,
      'sequential_connection_reuse': true, 'sequential_connections': sequentialConnections, 'unauthenticated_price_status': 401}));
  } finally {
    client.close(force: true);
  }
}
