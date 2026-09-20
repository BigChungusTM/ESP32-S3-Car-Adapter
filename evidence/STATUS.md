# Initial setup validation

- All five ESP32-S3 controller-only images built successfully with the installed ESP-IDF 5.1.6 package.
- Compiler commands confirm PROBE_CASE 0 through 4 respectively.
- All five application binaries have distinct SHA-256 hashes; manifest also covers bootloaders and partition tables.
- Generated configs confirm controller-only mode and ESP32-S3 target.
- Build completed with exit code 0. See build.log.
- Shell scripts pass bash syntax validation; radio inventory completed successfully.
- Dependency compatibility pins: ruamel.yaml 0.18.6, ruamel.yaml.clib 0.2.8, setuptools 70.3.0.
- Hardware follow-up: all five cases ran and matched expectations; see hardware-run-01/RESULTS.md. No Classic packet, discovery or car USB behavior has been demonstrated.
