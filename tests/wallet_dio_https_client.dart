// Resolve with the wallet's package_config.json: exercises its installed Dio
// version and generated enrollment protobufs over verified native TLS.
import 'dart:io';
import 'package:dio/dio.dart';
import 'package:dio/io.dart';
import 'package:fortis_net_wallet/data/intelligence/generated/fortisnet/intelligence/v1/devices.pb.dart' as pb;

Future<void> main(List<String> args) async {
  if (args.length != 2) throw ArgumentError('BASE_URL CA_FILE required');
  final context = SecurityContext(withTrustedRoots: false)..setTrustedCertificates(args[1]);
  final dio = Dio(BaseOptions(baseUrl: args[0],
    connectTimeout: const Duration(seconds: 5), receiveTimeout: const Duration(seconds: 5)));
  dio.httpClientAdapter = IOHttpClientAdapter(createHttpClient: () => HttpClient(context: context));
  Options options([String? token]) => Options(responseType: ResponseType.bytes,
    contentType: 'application/protobuf', validateStatus: (status) => status != null && status < 500,
    headers: {'Accept': 'application/protobuf', if (token != null) 'Authorization': 'Bearer $token'});
  String? credential;
  try {
    final challengeResponse = await dio.post<List<int>>('/v1/devices/challenge', options: options());
    if (challengeResponse.statusCode != 200) throw StateError('Dio challenge framing failed');
    final challenge = pb.AttestationChallengeResponse.fromBuffer(challengeResponse.data!).challenge;
    final request = pb.EnrollDeviceRequest(platform: 'ios', appVersion: 'dio-native-tls-test',
      attestation: pb.DeviceAttestation(provider: 'development', challenge: challenge));
    final response = await dio.post<List<int>>('/v1/devices', data: request.writeToBuffer(), options: options());
    if (response.statusCode != 201) throw StateError('Dio enrollment failed: ${response.statusCode}');
    credential = pb.EnrollDeviceResponse.fromBuffer(response.data!).deviceToken;
    final networks = await dio.get<List<int>>('/v1/networks', options: options(credential));
    if (networks.statusCode != 200) throw StateError('Dio enrolled protobuf authentication failed');
    final refused = await dio.get<List<int>>('/v1/prices?ids=bitcoin', options: options());
    if (refused.statusCode != 401) throw StateError('Dio price authentication changed');
    final revoked = await dio.post<List<int>>('/v1/devices/revoke', options: options(credential));
    if (revoked.statusCode != 204) throw StateError('Dio test credential revocation failed');
    credential = null;
    stdout.writeln('Wallet Dio + generated protobuf TLS enrollment/auth/revocation passed');
  } finally {
    if (credential != null) {
      await dio.post<List<int>>('/v1/devices/revoke', options: options(credential));
    }
    dio.close(force: true);
  }
}
