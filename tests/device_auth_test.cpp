#include "../src/auth/device_auth.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fortis::auth;

int main() {
  assert(fortis::auth::DeviceQuotaPerMinute(fortis::auth::DeviceQuotaTier::Development) == 240);
  assert(fortis::auth::DeviceQuotaPerMinute(fortis::auth::DeviceQuotaTier::Attested) == 600);
  assert(fortis::auth::DeviceQuotaPerMinute(fortis::auth::DeviceQuotaTier::Premium) == 1200);
  assert(fortis::auth::DeviceQuotaPerMinute(fortis::auth::DeviceQuotaTier::Standard) == 120);
  char hash[65];
  assert(fortis::auth::HashDeviceToken(
      "fintel_dev_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", hash));
  assert(strcmp(hash, "1b676187fcc5e01eea878e57dd32d1618b9fac56f7173e8f48e95849010c0be2") == 0);
  assert(!fortis::auth::IsDeviceToken("fintel_dev_short"));
  char directory[] = "/tmp/fortis-device-XXXXXX";
  assert(mkdtemp(directory));
  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  constexpr int64_t now = 1700000000000LL;
  fortis::auth::DeviceCredentials first{};
  fortis::auth::DeviceCredentials rotated{};
  fortis::auth::DeviceCredentials unicode{};
  fortis::auth::DeviceCredentials nul_version{};
  char unicode_app_version[151];
  for (size_t i = 0; i < 50; ++i) {
    unicode_app_version[i * 3] = static_cast<char>(0xe0);
    unicode_app_version[i * 3 + 1] = static_cast<char>(0xa0);
    unicode_app_version[i * 3 + 2] = static_cast<char>(0x80);
  }
  unicode_app_version[sizeof(unicode_app_version) - 1] = '\0';
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    fortis::auth::DeviceAuthStore auth(&kv);
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Android, "1.2.3", 5,
                                  now, 86400000, &first));
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Ios, unicode_app_version, 150,
                                  now, 86400000, &unicode));
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Ios, "a\0b", 3,
                                  now, 86400000, &nul_version));
    assert(strlen(first.id) == 26 && fortis::auth::IsDeviceToken(first.token));
    assert(first.expires_at_ms == now + 86400000);
    fortis::auth::DeviceRecord row{};
    assert(auth.Authenticate(first.token, now, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(strcmp(row.id, first.id) == 0 && strcmp(row.app_version, "1.2.3") == 0);
    assert(auth.Authenticate(unicode.token, now, false, &row) == fortis::auth::DeviceAuthResult::Valid);
    assert(row.has_app_version && row.app_version_length == 150);
    assert(memcmp(row.app_version, unicode_app_version, 150) == 0);
    assert(auth.Authenticate(nul_version.token, now, false, &row) == fortis::auth::DeviceAuthResult::Valid);
    assert(row.has_app_version && row.app_version_length == 3 &&
           memcmp(row.app_version, "a\0b", 3) == 0);
    assert(strcmp(row.token_hash, first.token) != 0);
    assert(auth.Authenticate(first.token, now, true, &row) ==
           fortis::auth::DeviceAuthResult::AttestationRequired);
    fortis::storage::KvActivityStats before{}, after{};
    kv.ActivityStats(before);
    assert(auth.AuthenticateSnapshot(first.token, now, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    kv.ActivityStats(after);
    assert(memcmp(before.reads, after.reads, sizeof(before.reads)) != 0);
    before = after;
    assert(auth.AuthenticateSnapshot(first.token, now + 1, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.AuthenticateSnapshot(first.token, now + 1, true, &row) ==
           fortis::auth::DeviceAuthResult::AttestationRequired);
    assert(auth.AuthenticateSnapshot(first.token, now + 1, true, &row, false) ==
           fortis::auth::DeviceAuthResult::Valid);
    kv.ActivityStats(after);
    assert(memcmp(before.reads, after.reads, sizeof(before.reads)) == 0);
    assert(auth.AuthenticateSnapshot(first.token, now + 29999, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    kv.ActivityStats(after);
    assert(memcmp(before.reads, after.reads, sizeof(before.reads)) == 0);
    assert(auth.AuthenticateSnapshot(first.token, now + 30000, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    kv.ActivityStats(after);
    assert(memcmp(before.reads, after.reads, sizeof(before.reads)) != 0);
    assert(auth.Authenticate("fintel_dev_short", now, false, &row) ==
           fortis::auth::DeviceAuthResult::Unauthorized);
    assert(auth.TouchPresence(first.id, now + 60000));
    assert(auth.Authenticate(first.token, now + 60000, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid && row.last_seen_at_ms == now);
    assert(auth.TouchPresence(first.id, now + 300001));
    assert(auth.Authenticate(first.token, now + 300001, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid && row.last_seen_at_ms == now + 300001);
    assert(auth.AuthenticateSnapshot(first.token, now + 300001, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.Rotate(row, now + 300002, 86400000, &rotated) == fortis::auth::DeviceRotateResult::Rotated);
    assert(strcmp(first.id, rotated.id) == 0 && strcmp(first.token, rotated.token) != 0);
    assert(auth.Authenticate(first.token, now + 300003, false, &row) ==
           fortis::auth::DeviceAuthResult::Unauthorized);
    assert(auth.AuthenticateSnapshot(first.token, now + 300003, false, &row) ==
           fortis::auth::DeviceAuthResult::Unauthorized);
    assert(auth.Authenticate(rotated.token, now + 300003, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
  }
  {
    const int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    char bytes[2048];
    const ssize_t count = read(fd, bytes, sizeof(bytes));
    assert(count > 0);
    assert(close(fd) == 0);
    // The bearer strings never appear in the durable transaction log.
    for (ssize_t i = 0; i + 54 <= count; ++i) {
      assert(memcmp(bytes + i, first.token, 54) != 0);
      assert(memcmp(bytes + i, rotated.token, 54) != 0);
      assert(memcmp(bytes + i, unicode.token, 54) != 0);
    }
  }
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    fortis::auth::DeviceAuthStore auth(&kv);
    fortis::auth::DeviceRecord row{};
    assert(auth.Authenticate(rotated.token, now + 300003, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.Authenticate(unicode.token, now + 300003, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(row.has_app_version && row.app_version_length == 150);
    assert(memcmp(row.app_version, unicode_app_version, 150) == 0);
    assert(auth.Authenticate(nul_version.token, now + 300003, false, &row) == fortis::auth::DeviceAuthResult::Valid);
    assert(row.has_app_version && row.app_version_length == 3 &&
           memcmp(row.app_version, "a\0b", 3) == 0);
    assert(auth.Authenticate(rotated.token, rotated.expires_at_ms, false, &row) ==
           fortis::auth::DeviceAuthResult::SessionExpired);
    assert(auth.Authenticate(rotated.token, now + 300003, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.AuthenticateSnapshot(unicode.token, now + 300004, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.AuthenticateSnapshot(rotated.token, rotated.expires_at_ms - 1, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.AuthenticateSnapshot(rotated.token, rotated.expires_at_ms, false, &row) ==
           fortis::auth::DeviceAuthResult::SessionExpired);
    assert(auth.AuthenticateSnapshot(rotated.token, now + 300004, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    assert(auth.Revoke(rotated.id, now + 300004));
    assert(auth.AuthenticateSnapshot(rotated.token, now + 300005, false, &row) ==
           fortis::auth::DeviceAuthResult::SessionExpired);
    assert(auth.Authenticate(rotated.token, now + 300005, false, &row) ==
           fortis::auth::DeviceAuthResult::SessionExpired);
    assert(auth.Authenticate(rotated.token, now + 300005, false, &row) ==
           fortis::auth::DeviceAuthResult::SessionExpired);
    assert(auth.Rotate(row, now + 300006, 86400000, &first) == fortis::auth::DeviceRotateResult::SessionExpired);
    kv.Close();
    assert(auth.AuthenticateSnapshot(unicode.token, now + 300005, false, &row) ==
           fortis::auth::DeviceAuthResult::StorageError);
    assert(kv.Open(directory));
    fortis::storage::KvActivityStats before{}, after{};
    kv.ActivityStats(before);
    assert(auth.AuthenticateSnapshot(unicode.token, now + 300005, false, &row) ==
           fortis::auth::DeviceAuthResult::Valid);
    kv.ActivityStats(after);
    assert(memcmp(before.reads, after.reads, sizeof(before.reads)) != 0);
    kv.Close();
    assert(auth.Authenticate(rotated.token, now + 300005, false, &row) ==
           fortis::auth::DeviceAuthResult::StorageError);
  }
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0 && rmdir(directory) == 0);
  // FORTIS_APPLE_ATTESTATION_REQUIRED=false: unverified iOS and macOS records
  // pass required mode, mirroring the Android exception; Android stays bound
  // by its own switch. A separate store keeps the scan above unchanged.
  {
    char apple_directory[] = "/tmp/fortis-device-apple-XXXXXX";
    assert(mkdtemp(apple_directory));
    fortis::storage::KvStore kv;
    assert(kv.Open(apple_directory));
    fortis::auth::DeviceAuthStore auth(&kv);
    fortis::auth::DeviceCredentials ios{}, mac{}, android{}, windows{}, linux{};
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Ios, "1.0.19", 6,
                                  now, 86400000, &ios));
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Macos, "1.0.19", 6,
                                  now, 86400000, &mac));
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Android, "1.0.19", 6,
                                  now, 86400000, &android));
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Windows, "1.0.19", 6,
                                  now, 86400000, &windows));
    assert(auth.EnrollDevelopment(fortis::auth::DevicePlatform::Linux, "1.0.19", 6,
                                  now, 86400000, &linux));
    fortis::auth::DeviceRecord row{};
    using fortis::auth::DeviceAuthResult;
    assert(fortis::auth::AppleAttestationRequired());
    assert(auth.Authenticate(ios.token, now, true, &row) == DeviceAuthResult::AttestationRequired);
    assert(auth.Authenticate(mac.token, now, true, &row) == DeviceAuthResult::AttestationRequired);

    fortis::auth::ConfigureAppleAttestation(false);
    assert(!fortis::auth::AppleAttestationRequired());
    assert(auth.Authenticate(ios.token, now, true, &row) == DeviceAuthResult::Valid);
    assert(auth.Authenticate(mac.token, now, true, &row) == DeviceAuthResult::Valid);
    assert(auth.AuthenticateSnapshot(mac.token, now, true, &row) == DeviceAuthResult::Valid);
    assert(auth.Authenticate(android.token, now, true, &row) ==
           DeviceAuthResult::AttestationRequired);

    fortis::auth::ConfigureAppleAttestation(true);
    assert(auth.Authenticate(ios.token, now, true, &row) == DeviceAuthResult::AttestationRequired);

    // Desktop (Windows, Linux) has its own switch.
    assert(fortis::auth::DesktopAttestationRequired());
    assert(auth.Authenticate(windows.token, now, true, &row) ==
           DeviceAuthResult::AttestationRequired);
    fortis::auth::ConfigureDesktopAttestation(false);
    assert(auth.Authenticate(windows.token, now, true, &row) == DeviceAuthResult::Valid);
    assert(auth.Authenticate(linux.token, now, true, &row) == DeviceAuthResult::Valid);
    assert(auth.Authenticate(mac.token, now, true, &row) ==
           DeviceAuthResult::AttestationRequired);
    fortis::auth::ConfigureDesktopAttestation(true);
    kv.Close();
    char file[256];
    snprintf(file, sizeof(file), "%s/fortis.kv", apple_directory);
    assert(unlink(file) == 0);
    snprintf(file, sizeof(file), "%s/fortis.kv.lock", apple_directory);
    assert(unlink(file) == 0 && rmdir(apple_directory) == 0);
  }
  puts("native device credential repository test passed");
}
