# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).

## What is signed

Only Windows binaries built from this repository's source code by the
[GitHub Actions workflow](.github/workflows/release.yml):

- `PhoneCam.exe` - the PC program
- `PhoneCamVirtual64.dll`, `PhoneCamVirtual32.dll` - the virtual webcam
- `phonecam-obs.dll` - the OBS Studio plugin

Third-party libraries are compiled from pinned upstream sources during the same build
(`third_party/build_webrtc.sh`); see [THIRD_PARTY.md](THIRD_PARTY.md).

## Team roles

| Role | Members |
|---|---|
| Committers and reviewers | [convicted3](https://github.com/convicted3) |
| Approvers | [convicted3](https://github.com/convicted3) |

Every release is built by the workflow from a version tag and approved manually in SignPath before it is signed.

## Privacy policy

See [PRIVACY.md](PRIVACY.md). This program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or operating it.
