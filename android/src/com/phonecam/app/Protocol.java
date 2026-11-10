package com.phonecam.app;

/**
 * PhoneCam wire protocol (v1).
 *
 * TCP, phone is the server (port 5757).
 *   PC  -> phone : "PHC1 <width> <height> <fps> <bitrate_kbps> <camera>\n"
 *                  camera = back | front | <camera id>
 *   phone -> PC  : "PHC1 OK <width> <height> <rotation>\n"  or  "PHC1 ERR <message>\n"
 *   then binary packets from phone:
 *        u32 BE  payload length
 *        u8      type (1 = codec config SPS/PPS, 2 = key frame, 3 = frame, 4 = rotation, 5 = stats text)
 *        i64 BE  pts in microseconds
 *        payload (H.264 Annex-B for types 1..3; u16 BE degrees for type 4)
 *   PC may send control lines at any time:
 *        "TORCH 0|1\n", "ZOOM <float>\n", "KEYFRAME\n", "AF\n"
 *
 * UDP discovery on port 5758:
 *   PC broadcasts "PHC_DISCOVER", phone answers "PHC_HERE <tcp port> <device name>".
 */
final class Protocol {
    static final int TCP_PORT = 5757;
    static final int UDP_PORT = 5758;

    static final int PKT_CONFIG = 1;
    static final int PKT_KEY = 2;
    static final int PKT_FRAME = 3;
    static final int PKT_ROTATION = 4;
    static final int PKT_STATS = 5; // UTF-8 text with phone-side statistics
    static final int PKT_INFO = 7;  // "evhw 0|1": 0 = camera ignores exposure compensation, PC should do it
    static final int PKT_PONG = 6;  // "<pc time> <phone time us>" answer to "PING <pc time>" (clock sync)

    private Protocol() {}
}
