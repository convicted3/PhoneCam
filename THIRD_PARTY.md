# Сторонний код / Third-party code

PhoneCam itself is MIT-licensed (see `LICENSE`). It includes or links the following:

| Component | Where | License |
|---|---|---|
| [softcam](https://github.com/tshino/softcam) (DirectShow virtual camera, patched) | `third_party/softcam` | MIT |
| DirectShow base classes (from the Windows SDK samples, via softcam) | `third_party/softcam/src/baseclasses` | MIT |
| [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (WebRTC) | `third_party/webrtc` (prebuilt static libs + headers) | MPL-2.0 |
| [libjuice](https://github.com/paullouisageneau/libjuice) (ICE) | `third_party/webrtc` | MPL-2.0 |
| [libsrtp](https://github.com/cisco/libsrtp) | `third_party/webrtc` | BSD-3-Clause |
| [usrsctp](https://github.com/sctplab/usrsctp) | `third_party/webrtc` | BSD-3-Clause |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 3.6.2 (`MBEDTLS_SSL_DTLS_SRTP` enabled) | `third_party/webrtc` | Apache-2.0 |
| [libobs](https://github.com/obsproject/obs-studio) headers + import library | `pc/third_party` | GPL-2.0-or-later |

License texts: `third_party/softcam/LICENSE`, `third_party/webrtc/licenses/`.

**OBS plugin note:** `phonecam-obs.dll` links against libobs, so the plugin binary as distributed is covered by the
GPL-2.0 (the PhoneCam source code itself stays MIT, which is GPL-compatible).

Android build tools (`android.jar` from SDK platform 29, `dx` from build-tools 30.0.2) are **not** in this repository;
`android/tools/fetch_sdk.sh` downloads them from Google.
